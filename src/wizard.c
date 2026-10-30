/* Orchestration: preflight gates, the NVCleanstall page state machine,
 * package build, install, and the post-install script. */
#include "common.h"
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>

/* Defined locally rather than taken from libuuid, so the link is identical
 * under MinGW and MSVC. */
static const GUID k_FOLDERID_ProgramData =
    {0x62ab5d82,0xfdc1,0x4dc3,{0xa9,0xdd,0x07,0x0d,0x1d,0x49,0x5d,0x97}};
static const GUID k_FOLDERID_Documents =
    {0xfdd39ad0,0x238f,0x46af,{0xad,0xb4,0x6c,0x85,0x48,0x03,0x69,0xc7}};
static const GUID k_FOLDERID_Desktop =
    {0xb4bfcc3a,0xdb2c,0x424c,{0xb0,0x29,0x7f,0xe9,0x9a,0x87,0xc6,0x41}};

#define T_WINDOW_MS    60000      /* NVCleanstall main window appears */
#define T_PAGE_MS      45000      /* normal page transition */
#define T_DOWNLOAD_MS  2700000    /* driver download + repack: 45 min */
#define T_BUILD_MS     1800000    /* package build: 30 min */
#define T_INSTALL_MS   3600000    /* driver install: 60 min */
#define T_CAPTURE_MS   600000     /* settings capture: 10 min, which allows for
                                     a first-use Profile Inspector download */

static BOOL cancelled(volatile LONG *flag)
{
    return flag && InterlockedCompareExchange(flag, 0, 0) != 0;
}

/* ====================================================================
 *  Process helpers
 * ==================================================================== */

typedef struct { DWORD pid; HWND hwnd; const wchar_t *cls; } WinSearch;

static BOOL CALLBACK enum_main_cb(HWND h, LPARAM lp)
{
    WinSearch *s = (WinSearch *)lp;
    DWORD pid = 0;
    wchar_t cls[64] = L"";

    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != NULL && !s->cls) return TRUE;

    if (s->cls) {
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, s->cls) != 0) return TRUE;
    } else if (GetWindowTextLengthW(h) == 0) {
        return TRUE;
    }
    s->hwnd = h;
    return FALSE;
}

/* Run a console program with no window, feeding it stdin_utf8 (may be NULL)
 * and relaying every line it prints into the log - which is what puts it in
 * the application's own output pane.
 *
 * NvPostInstall.ps1 ends with Read-Host "Reboot now? (y/N)". With no console
 * that would hang forever, so "n" is written to stdin: the script declines,
 * exits cleanly, and this program's own reboot prompt is the only one. */
static void relay_line(const char *utf8)
{
    wchar_t w[2048];
    if (!utf8[0]) return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 2048) == 0 &&
        MultiByteToWideChar(CP_ACP, 0, utf8, -1, w, 2048) == 0)
        return;
    log_write(LG_INFO, L"  | %s", w);
}

BOOL proc_run_logged(const wchar_t *exe, const wchar_t *args,
                     const char *stdin_utf8, DWORD timeout_ms,
                     DWORD *out_exit)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE outR = NULL, outW = NULL, inR = NULL, inW = NULL;
    wchar_t cmdline[NVDK_MAX_PATH];
    char buf[4096], line[8192];
    size_t llen = 0;
    DWORD got = 0, wrote = 0;
    BOOL ok = FALSE;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&outR, &outW, &sa, 0)) return FALSE;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&inR, &inW, &sa, 0)) {
        CloseHandle(outR); CloseHandle(outW);
        return FALSE;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError  = outW;
    si.hStdInput  = inR;
    ZeroMemory(&pi, sizeof(pi));

    _snwprintf(cmdline, NVDK_MAX_PATH, L"\"%s\" %s", exe, args ? args : L"");
    cmdline[NVDK_MAX_PATH - 1] = L'\0';

    if (!CreateProcessW(exe, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &si, &pi)) {
        log_write(LG_FAIL, L"CreateProcess failed (%lu): %s", GetLastError(), exe);
        CloseHandle(outR); CloseHandle(outW);
        CloseHandle(inR);  CloseHandle(inW);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(outW);              /* parent must drop its write end */
    CloseHandle(inR);

    if (stdin_utf8 && *stdin_utf8)
        WriteFile(inW, stdin_utf8, (DWORD)strlen(stdin_utf8), &wrote, NULL);
    CloseHandle(inW);               /* EOF, so nothing can block on input */

    log_write(LG_INFO, L"Started pid %lu (silent): %s", pi.dwProcessId, exe);

    while (ReadFile(outR, buf, sizeof(buf), &got, NULL) && got > 0) {
        DWORD i;
        for (i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n' || llen >= sizeof(line) - 1) {
                line[llen] = '\0';
                relay_line(line);
                llen = 0;
            } else {
                line[llen++] = c;
            }
        }
    }
    if (llen) { line[llen] = '\0'; relay_line(line); }
    CloseHandle(outR);

    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        log_write(LG_FAIL, L"Timed out after %lu ms waiting for %s", timeout_ms, exe);
    } else {
        if (out_exit) GetExitCodeProcess(pi.hProcess, out_exit);
        ok = TRUE;
    }
    CloseHandle(pi.hProcess);
    return ok;
}

BOOL proc_main_window(DWORD pid, HWND *out)
{
    WinSearch s = { pid, NULL, NULL };
    EnumWindows(enum_main_cb, (LPARAM)&s);
    *out = s.hwnd;
    return s.hwnd != NULL;
}

static BOOL proc_dialog_window(DWORD pid, HWND *out)
{
    WinSearch s = { pid, NULL, L"#32770" };
    EnumWindows(enum_main_cb, (LPARAM)&s);
    *out = s.hwnd;
    return s.hwnd != NULL;
}

BOOL proc_run(const wchar_t *exe, const wchar_t *args, BOOL show_console,
              DWORD timeout_ms, DWORD *out_exit, HANDLE *out_proc)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmdline[NVDK_MAX_PATH];
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* argv[0] still has to be present in the command line, but the image is
     * resolved only from lpApplicationName - never through PATH, never
     * through a shell, so an unquoted-path hijack has nothing to grab. */
    if (args && *args)
        _snwprintf(cmdline, NVDK_MAX_PATH, L"\"%s\" %s", exe, args);
    else
        _snwprintf(cmdline, NVDK_MAX_PATH, L"\"%s\"", exe);
    cmdline[NVDK_MAX_PATH - 1] = L'\0';

    flags |= show_console ? CREATE_NEW_CONSOLE : 0;

    if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) {
        log_write(LG_FAIL, L"CreateProcess failed (%lu): %s", GetLastError(), exe);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    log_write(LG_INFO, L"Started pid %lu: %s", pi.dwProcessId, exe);

    if (timeout_ms == 0) {
        if (out_proc) *out_proc = pi.hProcess;
        else CloseHandle(pi.hProcess);
        return TRUE;
    }

    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
        log_write(LG_FAIL, L"Timed out after %lu ms waiting for %s", timeout_ms, exe);
        CloseHandle(pi.hProcess);
        return FALSE;
    }
    if (out_exit) GetExitCodeProcess(pi.hProcess, out_exit);
    if (out_proc) *out_proc = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return TRUE;
}

/* ====================================================================
 *  Configuration
 * ==================================================================== */

static void cfg_path(const RunConfig *cfg, wchar_t *out)
{
    _snwprintf(out, NVDK_MAX_PATH, L"%s\\config.ini", cfg->data_dir);
    out[NVDK_MAX_PATH - 1] = L'\0';
}

/* Builds "<directory of this exe>\<leaf>". NvPostInstall.ps1 ships beside the
 * exe, so that copy is the default rather than a path that only existed on the
 * machine this was written on. Leaves out empty if the module path cannot be
 * read, and the caller falls back to a bare filename. */
static void exe_sibling(const wchar_t *leaf, wchar_t *out)
{
    wchar_t *slash;
    DWORD    n;

    n = GetModuleFileNameW(NULL, out, NVDK_MAX_PATH);
    if (n == 0 || n >= NVDK_MAX_PATH) { out[0] = L'\0'; return; }
    out[NVDK_MAX_PATH - 1] = L'\0';

    slash = wcsrchr(out, L'\\');
    if (!slash) { out[0] = L'\0'; return; }
    slash[1] = L'\0';
    wcsncat(out, leaf, NVDK_MAX_PATH - wcslen(out) - 1);
}

BOOL cfg_load(RunConfig *cfg)
{
    wchar_t *pd = NULL;
    wchar_t ini[NVDK_MAX_PATH];
    wchar_t post_default[NVDK_MAX_PATH];

    ZeroMemory(cfg, sizeof(*cfg));

    if (FAILED(SHGetKnownFolderPath(&k_FOLDERID_ProgramData, 0, NULL, &pd)) || !pd)
        return FALSE;
    _snwprintf(cfg->data_dir, NVDK_MAX_PATH, L"%s\\NvDriverKit", pd);
    CoTaskMemFree(pd);
    cfg->data_dir[NVDK_MAX_PATH - 1] = L'\0';

    CreateDirectoryW(cfg->data_dir, NULL);
    sec_harden_directory(cfg->data_dir);

    /* The finished package is saved to the Desktop. It is NOT executed from
     * there - the Desktop is writable by a standard user - so a hash-verified
     * copy is run out of exec_dir instead. */
    {
        wchar_t *dsk = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&k_FOLDERID_Desktop, 0, NULL, &dsk)) && dsk) {
            wcsncpy(cfg->package_dir, dsk, NVDK_MAX_PATH - 1);
            cfg->package_dir[NVDK_MAX_PATH - 1] = L'\0';
            CoTaskMemFree(dsk);
        } else {
            _snwprintf(cfg->package_dir, NVDK_MAX_PATH, L"%s\\packages", cfg->data_dir);
            CreateDirectoryW(cfg->package_dir, NULL);
        }
    }

    _snwprintf(cfg->exec_dir, NVDK_MAX_PATH, L"%s\\run", cfg->data_dir);
    cfg->exec_dir[NVDK_MAX_PATH - 1] = L'\0';
    CreateDirectoryW(cfg->exec_dir, NULL);
    sec_harden_directory(cfg->exec_dir);

    cfg_path(cfg, ini);

    GetPrivateProfileStringW(L"paths", L"NVCleanstall",
        L"F:\\Windows\\Downloads\\NVCleanstall_1.19.0.exe",
        cfg->nvcleanstall, NVDK_MAX_PATH, ini);
    exe_sibling(L"NvPostInstall.ps1", post_default);
    if (!post_default[0])
        wcsncpy(post_default, L"NvPostInstall.ps1", NVDK_MAX_PATH - 1);
    GetPrivateProfileStringW(L"paths", L"PostInstallScript",
        post_default, cfg->post_script, NVDK_MAX_PATH, ini);

    /* Write the defaults back so the file is self-documenting on first run. */
    WritePrivateProfileStringW(L"paths", L"NVCleanstall", cfg->nvcleanstall, ini);
    WritePrivateProfileStringW(L"paths", L"PostInstallScript", cfg->post_script, ini);
    return TRUE;
}

/* ====================================================================
 *  Preflight
 * ==================================================================== */

/* Verify a source file, pin its hash on first sight, copy it into the
 * hardened data directory, and hand back the path of the staged copy.
 *
 * Staging matters: both inputs normally live in Downloads, which standard
 * users can write. An elevated process must not execute from there. */
static BOOL stage_input(const RunConfig *cfg, const wchar_t *src,
                        const wchar_t *leaf, BOOL require_signature,
                        const wchar_t *pin_name, wchar_t *out_path)
{
    wchar_t bin[NVDK_MAX_PATH], ini[NVDK_MAX_PATH];
    wchar_t hash_src[80], hash_dst[80], pinned[80], tp[64], cn[128];

    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
        log_write(LG_FAIL, L"Not found: %s", src);
        return FALSE;
    }

    if (require_signature) {
        if (!sec_verify_authenticode(src))
            return FALSE;
        if (!sec_signer_thumbprint(src, tp, 64)) {
            log_write(LG_FAIL, L"Could not read signer thumbprint: %s", src);
            return FALSE;
        }
        if (_wcsicmp(tp, NVCS_SIGNER_THUMBPRINT) != 0) {
            log_write(LG_FAIL, L"Signer thumbprint %s does not match the pin %s",
                      tp, NVCS_SIGNER_THUMBPRINT);
            return FALSE;
        }
        if (sec_signer_cn(src, cn, 128))
            log_write(LG_OK, L"Signed by %s (thumbprint pinned)", cn);
    }

    if (!sec_sha256_file(src, hash_src, 80)) {
        log_write(LG_FAIL, L"Could not hash %s", src);
        return FALSE;
    }

    cfg_path(cfg, ini);
    GetPrivateProfileStringW(L"pins", pin_name, L"", pinned, 80, ini);

    if (pinned[0] == L'\0') {
        WritePrivateProfileStringW(L"pins", pin_name, hash_src, ini);
        log_write(LG_WARN, L"Pinned %s on first use: sha256 %s", pin_name, hash_src);
    } else if (_wcsicmp(pinned, hash_src) != 0) {
        log_write(LG_FAIL, L"%s changed since it was pinned.", pin_name);
        log_write(LG_FAIL, L"  pinned  %s", pinned);
        log_write(LG_FAIL, L"  on disk %s", hash_src);
        log_write(LG_INFO, L"If the change was intentional, clear the [pins] entry "
                           L"in config.ini and run again.");
        return FALSE;
    } else {
        log_write(LG_OK, L"%s matches its pinned hash", pin_name);
    }

    _snwprintf(bin, NVDK_MAX_PATH, L"%s\\bin", cfg->data_dir);
    bin[NVDK_MAX_PATH - 1] = L'\0';
    CreateDirectoryW(bin, NULL);
    sec_harden_directory(bin);

    _snwprintf(out_path, NVDK_MAX_PATH, L"%s\\%s", bin, leaf);
    out_path[NVDK_MAX_PATH - 1] = L'\0';

    if (!CopyFileW(src, out_path, FALSE)) {
        log_write(LG_FAIL, L"Could not stage %s (%lu)", leaf, GetLastError());
        return FALSE;
    }
    if (!sec_sha256_file(out_path, hash_dst, 80) || _wcsicmp(hash_src, hash_dst) != 0) {
        log_write(LG_FAIL, L"Staged copy of %s does not match the source hash", leaf);
        return FALSE;
    }

    {
        BOOL writable = TRUE;
        if (!sec_path_is_nonadmin_writable(out_path, &writable) || writable) {
            log_write(LG_FAIL, L"Staged %s is writable by a non-administrator - refusing to run it", leaf);
            return FALSE;
        }
    }
    log_write(LG_OK, L"Staged %s (admin-only, sha256 %.16s...)", leaf, hash_dst);
    return TRUE;
}

/* NVCleanstall reuses %TEMP%\NVCleanstall. A leftover tree from an earlier
 * session can be picked up mid-build, so clear it first. */
static void clear_stale_workspace(void)
{
    wchar_t tmp[NVDK_MAX_PATH], target[NVDK_MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    size_t len;

    if (!GetTempPathW(NVDK_MAX_PATH, tmp)) return;
    _snwprintf(target, NVDK_MAX_PATH, L"%sNVCleanstall", tmp);
    target[NVDK_MAX_PATH - 1] = L'\0';

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES)
        return;

    /* Guard: only ever a directory named NVCleanstall directly under %TEMP%. */
    len = wcslen(target);
    if (len < 13 || _wcsicmp(target + len - 13, L"\\NVCleanstall") != 0) {
        log_write(LG_WARN, L"Refusing to clear unexpected path: %s", target);
        return;
    }

    target[len + 1] = L'\0';      /* SHFileOperation wants a double NUL */

    ZeroMemory(&op, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = target;
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    if (SHFileOperationW(&op) == 0)
        log_write(LG_OK, L"Cleared stale workspace %s", target);
    else
        log_write(LG_WARN, L"Could not fully clear %s", target);
}

/* A clean install wipes driver settings. NvPostInstall.ps1 restores them from
 * MySettings.nip - which only exists if -Capture was run beforehand. Finding
 * that out after the wipe is too late, so check now. */
static BOOL check_settings_capture(void)
{
    wchar_t *docs = NULL;
    wchar_t nip[NVDK_MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    SYSTEMTIME st;

    if (FAILED(SHGetKnownFolderPath(&k_FOLDERID_Documents, 0, NULL, &docs)) || !docs)
        return FALSE;
    _snwprintf(nip, NVDK_MAX_PATH, L"%s\\NvPostInstall\\MySettings.nip", docs);
    CoTaskMemFree(docs);
    nip[NVDK_MAX_PATH - 1] = L'\0';

    if (!GetFileAttributesExW(nip, GetFileExInfoStandard, &fad)) {
        log_write(LG_FAIL, L"No saved driver settings at %s", nip);
        log_write(LG_INFO, L"A clean install erases your settings. Capture them first:");
        log_write(LG_INFO, L"  powershell -ExecutionPolicy Bypass -File NvPostInstall.ps1 -Capture");
        return FALSE;
    }
    if (fad.nFileSizeLow < 64 && fad.nFileSizeHigh == 0) {
        log_write(LG_FAIL, L"MySettings.nip is empty (%lu bytes) - re-run -Capture", fad.nFileSizeLow);
        return FALSE;
    }
    FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
    log_write(LG_OK, L"Saved settings present (%lu bytes, %04u-%02u-%02u)",
              fad.nFileSizeLow, st.wYear, st.wMonth, st.wDay);
    return TRUE;
}

/* ====================================================================
 *  Wizard page helpers
 * ==================================================================== */

static BOOL wait_page(UiaCtx *ctx, UiaEl *win, const wchar_t *header,
                      DWORD timeout_ms, volatile LONG *cancel)
{
    DWORD start = GetTickCount();
    log_write(LG_INFO, L"Waiting for page \"%s\"...", header);
    for (;;) {
        UiaEl *h;
        if (cancelled(cancel)) return FALSE;
        h = uia_find_by_name(ctx, win, header, CT_ANY);
        if (h) {
            uia_release(h);
            log_write(LG_OK, L"Page: %s", header);
            return TRUE;
        }
        if (GetTickCount() - start >= timeout_ms) {
            log_write(LG_FAIL, L"Page \"%s\" did not appear within %lu s",
                      header, timeout_ms / 1000);
            return FALSE;
        }
        Sleep(400);
    }
}

static BOOL click_named(UiaCtx *ctx, UiaEl *win, const wchar_t *name, int type)
{
    UiaEl *el = uia_wait_for_name(ctx, win, name, type, 10000);
    BOOL ok;
    if (!el) {
        log_write(LG_FAIL, L"Control not found: \"%s\"", name);
        return FALSE;
    }
    uia_scroll_into_view(el);
    ok = uia_invoke(el);
    uia_release(el);
    if (!ok) log_write(LG_FAIL, L"Could not activate \"%s\"", name);
    else     log_write(LG_INFO, L"Clicked \"%s\"", name);
    Sleep(350);
    return ok;
}

/* Dump the live control tree. Called whenever a page step fails, so a broken
 * run leaves behind exactly what is needed to re-map it. */
static void dump_on_failure(UiaCtx *ctx, UiaEl *win)
{
    log_write(LG_INFO, L"---- control tree at the point of failure ----");
    uia_dump_tree(ctx, win, 12);
    log_write(LG_INFO, L"---- end of tree ----");
}

static BOOL click_by_id(UiaCtx *ctx, UiaEl *win, const wchar_t *id)
{
    UiaEl *el = uia_wait_for_id(ctx, win, id, 10000);
    BOOL ok;

    if (!el) {
        log_write(LG_FAIL, L"Control id \"%s\" not found", id);
        return FALSE;
    }
    uia_scroll_into_view(el);
    ok = uia_invoke(el);
    uia_release(el);
    if (!ok) log_write(LG_FAIL, L"Could not activate id \"%s\"", id);
    else     log_write(LG_INFO, L"Clicked \"%s\"", id);
    Sleep(350);
    return ok;
}

/* Checkbox by AutomationId. Preferred over the name variant: the visible
 * labels carry long parenthetical suffixes that are easy to get wrong. */
static BOOL ensure_checkbox_id(UiaCtx *ctx, UiaEl *win, const wchar_t *id,
                               const wchar_t *label, int want)
{
    UiaEl *el = uia_find_by_id(ctx, win, id);
    int state = -1;

    if (!el) {
        log_write(LG_FAIL, L"Checkbox id \"%s\" not found", id);
        return FALSE;
    }
    uia_scroll_into_view(el);

    if (!uia_get_toggle(el, &state)) {
        log_write(LG_FAIL, L"Cannot read checkbox \"%s\"", label);
        uia_release(el);
        return FALSE;
    }
    if (state != want) {
        log_write(LG_WARN, L"%-44s was %s, correcting", label, state ? L"on" : L"off");
        if (!uia_set_toggle(el, want) || !uia_get_toggle(el, &state) || state != want) {
            log_write(LG_FAIL, L"Could not set \"%s\" to %s", label, want ? L"on" : L"off");
            uia_release(el);
            return FALSE;
        }
    }
    log_write(LG_OK, L"%-44s %s", label, want ? L"on" : L"off");
    uia_release(el);
    return TRUE;
}

/* ====================================================================
 *  Page 1 - driver version
 * ==================================================================== */

static BOOL page_driver(UiaCtx *ctx, UiaEl *win, const NvDriverInfo *cur,
                        wchar_t *avail_out, BOOL *up_to_date, BOOL force)
{
    UiaEl *pkg;
    wchar_t pkgname[256] = L"";

    *up_to_date = FALSE;

    /* Cross-check the wizard's own reading against the registry. */
    pkg = uia_find_by_id(ctx, win, ID_CURRENT_VERSION);
    if (pkg) {
        wchar_t shown[64] = L"";
        if (uia_get_name(pkg, shown, 64) && shown[0] &&
            wcscmp(shown, cur->nv_version) != 0)
            log_write(LG_WARN, L"Wizard shows current driver %s, registry says %s",
                      shown, cur->nv_version);
        uia_release(pkg);
    }

    /* BestDriver holds the package filename, but it is EMPTY until the online
     * version lookup finishes - the combo still reads "Loading...". Polling
     * for a non-empty name is what makes this reliable. */
    pkg = uia_wait_for_id(ctx, win, ID_BEST_DRIVER, 15000);
    if (!pkg) {
        log_write(LG_FAIL, L"Control id \"%s\" not found on the driver page",
                  ID_BEST_DRIVER);
        return FALSE;
    }
    log_write(LG_INFO, L"Waiting for the online driver lookup...");
    if (!uia_wait_nonempty_name(pkg, pkgname, 256, 90000)) {
        log_write(LG_FAIL, L"The wizard never reported an available driver "
                           L"(still loading after 90 s) - check connectivity.");
        uia_release(pkg);
        return FALSE;
    }
    uia_release(pkg);

    if (!nv_version_from_package_name(pkgname, avail_out, 16)) {
        log_write(LG_FAIL, L"Unrecognised package name: %s", pkgname);
        return FALSE;
    }
    log_write(LG_INFO, L"Installed: %s   Available: %s", cur->nv_version, avail_out);
    log_write(LG_INFO, L"Package:   %s", pkgname);

    if (nv_compare_versions(cur->nv_version, avail_out) >= 0) {
        *up_to_date = TRUE;
        if (!force)
            return TRUE;
        log_write(LG_WARN, L"Already on %s - continuing anyway (--force). "
                           L"This reinstalls the same version cleanly.", avail_out);
    }

    {
        UiaEl *radio = uia_find_by_id(ctx, win, ID_RADIO_AUTODETECT);
        BOOL sel = FALSE;
        if (!radio) {
            log_write(LG_FAIL, L"Radio id \"%s\" not found", ID_RADIO_AUTODETECT);
            return FALSE;
        }
        if (!uia_is_selected(radio, &sel)) sel = FALSE;
        if (!sel) uia_select(radio);
        if (!uia_is_selected(radio, &sel) || !sel) {
            log_write(LG_FAIL, L"Could not select \"Install best driver for my hardware\"");
            uia_release(radio);
            return FALSE;
        }
        uia_release(radio);
        log_write(LG_OK, L"Source: install best driver for my hardware");
    }

    /* By id, not by label: the real names carry long suffixes
     * ("... bypass Cloudflare content delivery network in problem areas"). */
    if (!ensure_checkbox_id(ctx, win, ID_FALLBACK_SERVERS, L"Use fallback servers", 0))
        return FALSE;
    ensure_checkbox_id(ctx, win, ID_CHECK_BACKGROUND,
                       L"Periodic background update check", 1);

    return click_by_id(ctx, win, ID_BUTTON_NEXT);
}

/* ====================================================================
 *  Page 2 - components
 * ==================================================================== */

static BOOL page_components(UiaCtx *ctx, UiaEl *win)
{
    wchar_t checked[1024] = L"";
    int n;

    /* "Minimum" leaves only the required display driver ticked. */
    if (!click_by_id(ctx, win, ID_BUTTON_MINIMUM))
        return FALSE;
    Sleep(600);

    /* Rows are ListItems wrapping an unnamed CheckBox, so the label has to be
     * read from the row rather than the box. */
    n = uia_list_checked_items(ctx, win, checked, 1024);
    if (n < 0) {
        log_write(LG_FAIL, L"Could not enumerate the component list");
        return FALSE;
    }
    log_write(LG_INFO, L"Components ticked (%d): %s", n, checked[0] ? checked : L"(none)");

    if (n != 1 || !wcsstr(checked, COMPONENT_REQUIRED)) {
        log_write(LG_FAIL, L"Expected only \"%s\" to be selected", COMPONENT_REQUIRED);
        return FALSE;
    }
    log_write(LG_OK, L"Components: display driver only");
    return click_by_id(ctx, win, ID_BUTTON_NEXT);
}

/* ====================================================================
 *  Page 3 - tweaks
 * ==================================================================== */

static BOOL page_tweaks(UiaCtx *ctx, UiaEl *win)
{
    size_t n, i;
    const Tweak *m = tw_manifest(&n);
    BOOL all_ok = TRUE;

    /* Every checkbox on this page has an AutomationId identical to its key in
     * PreviousTweaks, so each one is driven directly. That is far steadier
     * than clicking "Use Previous Settings" and hoping: one of these labels is
     * empty (RebuildSignatureEAC) and would never match by name. */
    log_write(LG_STEP, L"Setting every tweak by AutomationId");

    for (i = 0; i < n; i++) {
        if (m[i].type != TW_BOOL) continue;
        if (!ensure_checkbox_id(ctx, win, m[i].key,
                                m[i].ui_name ? m[i].ui_name : m[i].key, m[i].num))
            all_ok = FALSE;
        /* The expert tweaks are hidden until this one is on. */
        if (wcscmp(m[i].key, L"ShowExpertOptions") == 0)
            Sleep(600);
    }

    if (!all_ok) {
        log_write(LG_FAIL, L"Tweak page does not match the manifest - stopping "
                           L"before anything is built.");
        return FALSE;
    }

    /* The Interrupt Policy/Priority and NVENC-target combos are ComboBox
     * controls that expose NO UI Automation patterns and no children. Dumping
     * one reads:
     *
     *     MSIPriority: type=50003 enabled=yes patterns=none 0 descendant(s)
     *
     * so there is no route to read or drive them - not ExpandCollapse, not
     * Selection, not Value, not LegacyIAccessible, and type-ahead cannot be
     * confirmed. Nothing is attempted here. The interrupt priority is instead
     * written straight to the device key after the install, where it can be
     * verified; see nv_set_interrupt_priority. */
    {
        const wchar_t *ids[] = { L"MSIPolicy", L"MSIPriority", L"NvEncPatchVersions" };
        size_t c;
        for (c = 0; c < 3; c++) {
            UiaEl *el = uia_find_by_id(ctx, win, ids[c]);
            if (!el) {
                log_write(LG_WARN, L"Combo \"%s\" not found", ids[c]);
                continue;
            }
            log_write(LG_INFO, L"%-44s left at default (exposes no UIA patterns)", ids[c]);
            uia_release(el);
        }
    }

    log_write(LG_OK, L"All tweak checkboxes set and read back");
    return click_by_id(ctx, win, ID_BUTTON_NEXT);
}

/* ====================================================================
 *  Finished page - build the package
 * ==================================================================== */

static BOOL wait_file_stable(const wchar_t *path, DWORD timeout_ms,
                             volatile LONG *cancel)
{
    DWORD start = GetTickCount();
    LARGE_INTEGER last = { 0 };
    int stable = 0;

    for (;;) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (cancelled(cancel)) return FALSE;

        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            LARGE_INTEGER sz;
            sz.LowPart  = fad.nFileSizeLow;
            sz.HighPart = (LONG)fad.nFileSizeHigh;

            if (sz.QuadPart > 0 && sz.QuadPart == last.QuadPart) {
                if (++stable >= 5) {           /* unchanged for ~5 s */
                    log_write(LG_OK, L"Package built: %.1f MB",
                              (double)sz.QuadPart / (1024.0 * 1024.0));
                    return TRUE;
                }
            } else {
                stable = 0;
                last = sz;
            }
        }
        if (GetTickCount() - start >= timeout_ms) {
            log_write(LG_FAIL, L"Package build did not finish within %lu minutes",
                      timeout_ms / 60000);
            return FALSE;
        }
        Sleep(1000);
    }
}

static BOOL page_build(UiaCtx *ctx, UiaEl *win, DWORD nvcs_pid,
                       const wchar_t *pkg_path, volatile LONG *cancel)
{
    HWND dlg = NULL;
    UiaEl *droot = NULL, *edit = NULL;
    DWORD start;
    BOOL ok = FALSE;

    if (!click_by_id(ctx, win, ID_BUILD_PACKAGE))
        return FALSE;

    /* Save dialog */
    start = GetTickCount();
    while (!proc_dialog_window(nvcs_pid, &dlg)) {
        if (cancelled(cancel)) return FALSE;
        if (GetTickCount() - start > 30000) {
            log_write(LG_FAIL, L"Save dialog did not appear");
            return FALSE;
        }
        Sleep(300);
    }

    droot = uia_from_hwnd(ctx, dlg);
    if (!droot) {
        log_write(LG_FAIL, L"Could not attach to the save dialog");
        return FALSE;
    }

    edit = uia_wait_for_name(ctx, droot, L"File name:", CT_EDIT, 10000);
    if (!edit) edit = uia_find_by_name(ctx, droot, L"File name", CT_EDIT);
    if (!edit) {
        log_write(LG_FAIL, L"File name field not found in the save dialog");
        uia_release(droot);
        return FALSE;
    }

    if (uia_set_value(edit, pkg_path)) {
        wchar_t back[NVDK_MAX_PATH] = L"";
        uia_get_value(edit, back, NVDK_MAX_PATH);
        log_write(LG_INFO, L"Save path: %s", back);
        ok = click_named(ctx, droot, L"Save", CT_BUTTON);
    } else {
        log_write(LG_FAIL, L"Could not set the save path");
    }
    uia_release(edit);
    uia_release(droot);
    if (!ok) return FALSE;

    log_write(LG_STEP, L"Building package (this downloads the driver, ~1 GB)");
    return wait_file_stable(pkg_path, T_BUILD_MS, cancel);
}

/* ====================================================================
 *  Main sequence
 * ==================================================================== */

static void close_nvcleanstall(HANDLE proc, DWORD pid)
{
    HWND h = NULL;
    if (proc_main_window(pid, &h) && h)
        PostMessageW(h, WM_CLOSE, 0, 0);
    if (proc && WaitForSingleObject(proc, 8000) == WAIT_TIMEOUT) {
        log_write(LG_WARN, L"NVCleanstall did not exit; terminating");
        TerminateProcess(proc, 0);
    }
}

RunResult wizard_run(const RunConfig *cfg, volatile LONG *cancel)
{
    NvDriverInfo before, after;
    wchar_t nvcs[NVDK_MAX_PATH], script[NVDK_MAX_PATH], pkg[NVDK_MAX_PATH];
    wchar_t runpkg[NVDK_MAX_PATH];
    wchar_t avail[16] = L"", pshell[NVDK_MAX_PATH], args[NVDK_MAX_PATH], hash[80];
    HANDLE hproc = NULL;
    DWORD pid = 0, exitcode = 0;
    HWND hwnd = NULL;
    UiaCtx *ctx = NULL;
    UiaEl *win = NULL;
    SYSTEMTIME st;
    RunResult result = RUN_ERROR;
    BOOL up_to_date = FALSE;

    log_write(LG_STEP, L"Preflight");

    if (!sec_is_elevated()) {
        log_write(LG_FAIL, L"Not running elevated.");
        return RUN_ABORTED;
    }
    log_write(LG_OK, L"Running elevated");

    if (!nv_get_installed(&before)) {
        log_write(LG_FAIL, L"No NVIDIA display adapter found");
        return RUN_ABORTED;
    }
    log_write(LG_OK, L"%s - driver %s (%s)", before.desc, before.nv_version,
              before.raw_version);

    if (!check_settings_capture())
        return RUN_ABORTED;

    if (!stage_input(cfg, cfg->nvcleanstall, L"NVCleanstall.exe", TRUE,
                     L"nvcleanstall_sha256", nvcs))
        return RUN_ABORTED;
    if (!stage_input(cfg, cfg->post_script, L"NvPostInstall.ps1", FALSE,
                     L"postinstall_sha256", script))
        return RUN_ABORTED;

    clear_stale_workspace();

    log_write(LG_STEP, L"Writing tweak manifest");
    if (!tw_write_manifest())
        return RUN_ABORTED;
    if (!tw_verify_registry(NULL, 0)) {
        log_write(LG_FAIL, L"Tweak manifest did not read back correctly");
        return RUN_ABORTED;
    }
    log_write(LG_OK, L"Manifest verified in the registry");
    log_write(LG_INFO, L"UnattendedReboot is forced off so the post-install "
                       L"script always gets to run.");

    /* ---- drive the wizard ---- */
    log_write(LG_STEP, L"Launching NVCleanstall");
    if (!proc_run(nvcs, NULL, FALSE, 0, NULL, &hproc))
        return RUN_ERROR;
    pid = GetProcessId(hproc);

    {
        DWORD start = GetTickCount();
        while (!proc_main_window(pid, &hwnd)) {
            if (cancelled(cancel)) goto cleanup;
            if (GetTickCount() - start > T_WINDOW_MS) {
                log_write(LG_FAIL, L"NVCleanstall window never appeared");
                goto cleanup;
            }
            Sleep(300);
        }
    }
    Sleep(1200);

    if (!uia_init(&ctx)) goto cleanup;
    win = uia_from_hwnd(ctx, hwnd);
    if (!win) {
        log_write(LG_FAIL, L"Could not attach UI Automation to NVCleanstall");
        goto cleanup;
    }
    {
        wchar_t title[256] = L"";
        GetWindowTextW(hwnd, title, 256);
        log_write(LG_INFO, L"Window: \"%s\"", title);
        if (wcscmp(title, NVCS_WINDOW_TITLE) != 0)
            log_write(LG_WARN, L"Expected \"%s\". The automation is validated "
                               L"against that build only.", NVCS_WINDOW_TITLE);
    }

    log_write(LG_STEP, L"Checking for a newer driver");
    if (!wait_page(ctx, win, PAGE_DRIVER, T_PAGE_MS, cancel)) { dump_on_failure(ctx, win); goto cleanup; }
    if (!page_driver(ctx, win, &before, avail, &up_to_date, cfg->force)) { dump_on_failure(ctx, win); goto cleanup; }

    if (up_to_date && !cfg->force) {
        log_write(LG_OK, L"Up to date - driver %s is current. Nothing to do.",
                  before.nv_version);
        result = RUN_UP_TO_DATE;
        goto cleanup;
    }
    log_write(LG_OK, L"Update available: %s -> %s", before.nv_version, avail);

    log_write(LG_STEP, L"Selecting components");
    if (!wait_page(ctx, win, PAGE_COMPONENTS, T_PAGE_MS, cancel)) { dump_on_failure(ctx, win); goto cleanup; }
    if (!page_components(ctx, win)) { dump_on_failure(ctx, win); goto cleanup; }

    log_write(LG_STEP, L"Applying installation tweaks");
    if (!wait_page(ctx, win, PAGE_TWEAKS, T_PAGE_MS, cancel)) { dump_on_failure(ctx, win); goto cleanup; }
    if (!page_tweaks(ctx, win)) { dump_on_failure(ctx, win); goto cleanup; }

    log_write(LG_STEP, L"Downloading and repacking the driver");
    if (!wait_page(ctx, win, PAGE_FINISHED, T_DOWNLOAD_MS, cancel)) { dump_on_failure(ctx, win); goto cleanup; }

    if (cfg->dry_run) {
        log_write(LG_OK, L"Dry run: reached the Finished page. Stopping before build.");
        result = RUN_ABORTED;
        goto cleanup;
    }

    GetLocalTime(&st);
    _snwprintf(pkg, NVDK_MAX_PATH,
               L"%s\\NVIDIA-%s-clean-%04u%02u%02u-%02u%02u%02u.exe",
               cfg->package_dir, avail, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    pkg[NVDK_MAX_PATH - 1] = L'\0';

    log_write(LG_STEP, L"Building the installer package");
    if (!page_build(ctx, win, pid, pkg, cancel)) { dump_on_failure(ctx, win); goto cleanup; }

    /* ---- install ---- */
    log_write(LG_STEP, L"Installing");

    if (!sec_sha256_file(pkg, hash, 80)) {
        log_write(LG_FAIL, L"Could not hash the built package");
        goto cleanup;
    }
    log_write(LG_OK, L"Package saved to %s", pkg);
    log_write(LG_INFO, L"sha256 %s", hash);

    {
        const wchar_t *leaf = wcsrchr(pkg, L'\\');
        wchar_t hash2[80];
        BOOL writable = TRUE;

        /* Clear anything an earlier run left here. Each package is ~800 MB,
         * so leftovers accumulate fast on the system drive. */
        {
            wchar_t glob[NVDK_MAX_PATH];
            WIN32_FIND_DATAW fd;
            HANDLE h;

            _snwprintf(glob, NVDK_MAX_PATH, L"%s\\*.exe", cfg->exec_dir);
            glob[NVDK_MAX_PATH - 1] = L'\0';

            h = FindFirstFileW(glob, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    wchar_t old[NVDK_MAX_PATH];
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    _snwprintf(old, NVDK_MAX_PATH, L"%s\\%s", cfg->exec_dir, fd.cFileName);
                    old[NVDK_MAX_PATH - 1] = L'\0';
                    if (DeleteFileW(old))
                        log_write(LG_INFO, L"Removed leftover staged package %s", fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }

        _snwprintf(runpkg, NVDK_MAX_PATH, L"%s\\%s", cfg->exec_dir,
                   leaf ? leaf + 1 : L"package.exe");
        runpkg[NVDK_MAX_PATH - 1] = L'\0';

        if (!CopyFileW(pkg, runpkg, FALSE)) {
            log_write(LG_FAIL, L"Could not stage the package for execution (%lu)",
                      GetLastError());
            goto cleanup;
        }
        if (!sec_sha256_file(runpkg, hash2, 80) || _wcsicmp(hash, hash2) != 0) {
            log_write(LG_FAIL, L"Staged copy does not match the package that was built");
            goto cleanup;
        }
        if (!sec_path_is_nonadmin_writable(runpkg, &writable) || writable) {
            log_write(LG_FAIL, L"Run directory is writable by a non-administrator - refusing");
            goto cleanup;
        }
        log_write(LG_OK, L"Running a hash-verified copy from the admin-only directory");
    }

    close_nvcleanstall(hproc, pid);
    CloseHandle(hproc);
    hproc = NULL;

    log_write(LG_INFO, L"Installing the driver - the screen may go black for a moment.");
    if (!proc_run(runpkg, NULL, FALSE, T_INSTALL_MS, &exitcode, NULL))
        goto cleanup;
    log_write(LG_INFO, L"Installer exit code %lu", exitcode);

    /* The staged copy has served its purpose and the Desktop package is the
     * one that is kept, so drop it rather than leaving ~800 MB per run on
     * the system drive. */
    if (DeleteFileW(runpkg))
        log_write(LG_OK, L"Removed the staged copy (%s)", cfg->exec_dir);
    else
        log_write(LG_WARN, L"Could not remove the staged copy from %s (%lu)",
                  cfg->exec_dir, GetLastError());

    /* ---- verify the install actually landed ---- */
    log_write(LG_STEP, L"Verifying the installed driver");
    Sleep(3000);
    if (!nv_get_installed(&after)) {
        log_write(LG_FAIL, L"Could not read the driver version after install");
        goto cleanup;
    }
    log_write(LG_INFO, L"Driver is now %s (%s)", after.nv_version, after.raw_version);

    if (nv_compare_versions(after.nv_version, avail) != 0) {
        log_write(LG_FAIL, L"Expected %s but found %s. Not running the post-install "
                           L"script - restore your settings manually once the driver "
                           L"is correct.", avail, after.nv_version);
        result = RUN_ABORTED;
        goto cleanup;
    }
    log_write(LG_OK, L"Driver %s installed", after.nv_version);

    /* ---- post-install script ---- */
    log_write(LG_STEP, L"Restoring driver settings (NvPostInstall.ps1)");
    _snwprintf(pshell, NVDK_MAX_PATH,
               L"%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
               _wgetenv(L"SystemRoot") ? _wgetenv(L"SystemRoot") : L"C:\\Windows");
    pshell[NVDK_MAX_PATH - 1] = L'\0';

    _snwprintf(args, NVDK_MAX_PATH,
               L"-NoProfile -ExecutionPolicy Bypass -File \"%s\"", script);
    args[NVDK_MAX_PATH - 1] = L'\0';

    /* No console window: output is relayed into this program's own log, and
     * "n" is fed to the script's reboot prompt so it cannot block. The reboot
     * question is asked once, by this program, at the end. */
    if (!proc_run_logged(pshell, args, "n\r\n", T_INSTALL_MS, &exitcode)) {
        log_write(LG_FAIL, L"Post-install script did not complete");
        goto cleanup;
    }
    log_write(LG_INFO, L"Script exit code %lu", exitcode);

    /* ---- interrupt priority ----
     * NVCleanstall's combo cannot be automated (it exposes no UIA patterns),
     * so the value the manifest asks for is written to the GPU's device key
     * directly and read back. The driver only consults it when the device
     * restarts, which the reboot below covers. */
    log_write(LG_STEP, L"Setting GPU interrupt priority");
    {
        int want = tw_int(L"MSIPriority", 3);
        wchar_t dev[512] = L"";
        static const wchar_t *names[] = { L"Undefined", L"Low", L"Normal", L"High" };

        if (nv_set_interrupt_priority(want, dev, 512)) {
            log_write(LG_OK, L"DevicePriority = %d (%s) on %s", want,
                      (want >= 0 && want <= 3) ? names[want] : L"?", dev);
            log_write(LG_INFO, L"Takes effect at the next restart.");
        } else {
            log_write(LG_WARN, L"Could not set DevicePriority. Message Signaled "
                               L"Interrupts are still enabled; only the priority "
                               L"is unset.");
        }
    }

    log_write(LG_OK, L"Done.");
    result = RUN_INSTALLED;

cleanup:
    if (win) uia_release(win);
    if (ctx) uia_shutdown(ctx);
    if (hproc) {
        close_nvcleanstall(hproc, pid);
        CloseHandle(hproc);
    }
    return result;
}

/* ====================================================================
 *  Fetch Settings - save what the driver is set to right now, so a later
 *  clean install has something to restore.
 * ==================================================================== */

/* Counts "<ProfileSetting" in a .nip, so the outcome is reported from what
 * reached the disk rather than from what the script said it did.
 *
 * Profile Inspector writes the file as UTF-8 with a BOM while declaring
 * encoding="utf-16" in the XML header. The bytes really are UTF-8, so a byte
 * search is right here despite what the declaration claims. Returns -1 when
 * the file cannot be read. */
static int nip_setting_count(const wchar_t *path)
{
    HANDLE      h;
    DWORD       size, got = 0;
    char       *buf;
    const char *p;
    int         n = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > (8u << 20)) {
        CloseHandle(h);
        return -1;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        CloseHandle(h);
        return -1;
    }
    if (!ReadFile(h, buf, size, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    buf[got] = '\0';

    for (p = buf; (p = strstr(p, "<ProfileSetting")) != NULL; p++)
        n++;

    free(buf);
    return n;
}

/* Documents\NvPostInstall\<leaf>, following a relocated Documents folder the
 * same way the script does. */
static BOOL docs_file(const wchar_t *leaf, wchar_t *out)
{
    wchar_t *docs = NULL;

    if (FAILED(SHGetKnownFolderPath(&k_FOLDERID_Documents, 0, NULL, &docs)) || !docs)
        return FALSE;
    _snwprintf(out, NVDK_MAX_PATH, L"%s\\NvPostInstall\\%s", docs, leaf);
    CoTaskMemFree(docs);
    out[NVDK_MAX_PATH - 1] = L'\0';
    return TRUE;
}

/* Logs size and write time for one saved file. The timestamp is the tell that
 * the script wrote it now rather than leaving an older copy in place. */
static BOOL report_saved(const wchar_t *path, const wchar_t *what)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILETIME    local;
    SYSTEMTIME  st;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return FALSE;

    FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    log_write(LG_OK, L"%s: %lu bytes, written %04d-%02d-%02d %02d:%02d",
              what, fad.nFileSizeLow,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return TRUE;
}

RunResult wizard_capture(const RunConfig *cfg, volatile LONG *cancel)
{
    wchar_t script[NVDK_MAX_PATH], pshell[NVDK_MAX_PATH], args[NVDK_MAX_PATH];
    wchar_t nip[NVDK_MAX_PATH], dsr[NVDK_MAX_PATH];
    DWORD   exitcode = 0;
    int     count;

    log_write(LG_STEP, L"Fetching your current driver settings");
    log_write(LG_INFO, L"This reads the driver and saves what it finds. "
                       L"Nothing is installed and no setting is changed.");

    if (cancelled(cancel)) return RUN_ABORTED;

    /* Staged exactly like the install path: hash pinned, copied into an
     * admin-only directory and run from there, never from Downloads. */
    if (!stage_input(cfg, cfg->post_script, L"NvPostInstall.ps1", FALSE,
                     L"postinstall_sha256", script))
        return RUN_ABORTED;

    if (cancelled(cancel)) return RUN_ABORTED;

    _snwprintf(pshell, NVDK_MAX_PATH,
               L"%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
               _wgetenv(L"SystemRoot") ? _wgetenv(L"SystemRoot") : L"C:\\Windows");
    pshell[NVDK_MAX_PATH - 1] = L'\0';

    _snwprintf(args, NVDK_MAX_PATH,
               L"-NoProfile -ExecutionPolicy Bypass -File \"%s\" -Capture", script);
    args[NVDK_MAX_PATH - 1] = L'\0';

    /* -Capture never calls Read-Host, so there is nothing to feed it. Its
     * output is relayed into this program's log, which is where the table of
     * captured settings appears. */
    log_write(LG_STEP, L"Running NvPostInstall.ps1 -Capture");
    if (!proc_run_logged(pshell, args, NULL, T_CAPTURE_MS, &exitcode)) {
        log_write(LG_FAIL, L"The capture script did not complete");
        return RUN_ERROR;
    }
    if (exitcode != 0) {
        log_write(LG_FAIL, L"Capture script exited with code %lu", exitcode);
        return RUN_ERROR;
    }

    /* Confirm against the disk rather than trusting the script's own output. */
    log_write(LG_STEP, L"Checking what was saved");
    if (!docs_file(L"MySettings.nip", nip) || !docs_file(L"DsrSettings.json", dsr)) {
        log_write(LG_FAIL, L"Could not locate your Documents folder");
        return RUN_ERROR;
    }

    if (!report_saved(nip, L"Driver profile (MySettings.nip)")) {
        log_write(LG_FAIL, L"Nothing was written to %s", nip);
        return RUN_ERROR;
    }

    count = nip_setting_count(nip);
    if (count < 0) {
        log_write(LG_FAIL, L"The saved profile could not be read back");
        return RUN_ERROR;
    }
    if (count == 0) {
        log_write(LG_FAIL, L"No customized settings found. Change something in "
                           L"the NVIDIA Control Panel, click Apply, then fetch "
                           L"again.");
        return RUN_ABORTED;
    }
    log_write(LG_OK, L"Found %d customized driver setting(s)", count);

    /* DSR is optional: the registry values only exist once DSR has been set
     * at least once, so a miss here is worth saying but is not a failure. */
    if (!report_saved(dsr, L"DSR factors and smoothness (DsrSettings.json)"))
        log_write(LG_WARN, L"No DSR data saved. Set DSR once in the Control "
                           L"Panel, click Apply, then fetch again.");

    log_write(LG_OK, L"Settings saved. A clean install can now restore them.");
    return RUN_CAPTURED;
}

/* ====================================================================
 *  Probe - dump the control tree so the UI can be re-mapped after an
 *  NVCleanstall update without guessing.
 * ==================================================================== */

static BOOL CALLBACK enum_by_title_cb(HWND h, LPARAM lp)
{
    HWND *out = (HWND *)lp;
    wchar_t title[256] = L"";

    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL)
        return TRUE;
    GetWindowTextW(h, title, 256);
    if (wcsncmp(title, L"NVCleanstall", 12) == 0) {
        *out = h;
        return FALSE;
    }
    return TRUE;
}

BOOL wizard_probe_attach(void)
{
    HWND hwnd = NULL;
    UiaCtx *ctx = NULL;
    UiaEl *win = NULL;
    wchar_t title[256] = L"";
    int n;

    log_write(LG_STEP, L"Probe (attach) - dumping an already-open NVCleanstall");

    EnumWindows(enum_by_title_cb, (LPARAM)&hwnd);
    if (!hwnd) {
        log_write(LG_FAIL, L"No visible NVCleanstall window found. Open it first.");
        return FALSE;
    }
    GetWindowTextW(hwnd, title, 256);
    log_write(LG_INFO, L"Attached to \"%s\"", title);

    if (!uia_init(&ctx)) return FALSE;
    win = uia_from_hwnd(ctx, hwnd);
    if (!win) {
        log_write(LG_FAIL, L"Could not attach UI Automation to that window");
        uia_shutdown(ctx);
        return FALSE;
    }
    n = uia_dump_tree(ctx, win, 12);
    log_write(LG_OK, L"%d elements dumped", n);
    uia_release(win);
    uia_shutdown(ctx);
    return TRUE;
}

/* Walk backwards from whatever page is open, dumping each one. Back is pure
 * navigation in NVCleanstall - nothing is downloaded, built or installed - so
 * this maps the middle pages without a 1 GB round trip. */
BOOL wizard_probe_back(int steps)
{
    HWND hwnd = NULL;
    UiaCtx *ctx = NULL;
    UiaEl *win = NULL;
    int i;

    log_write(LG_STEP, L"Probe (walk back %d page(s))", steps);

    EnumWindows(enum_by_title_cb, (LPARAM)&hwnd);
    if (!hwnd) {
        log_write(LG_FAIL, L"No visible NVCleanstall window found.");
        return FALSE;
    }
    if (!uia_init(&ctx)) return FALSE;
    win = uia_from_hwnd(ctx, hwnd);
    if (!win) {
        uia_shutdown(ctx);
        return FALSE;
    }

    for (i = 0; i <= steps; i++) {
        UiaEl *back;

        log_write(LG_STEP, L"---- dump %d ----", i);
        uia_dump_tree(ctx, win, 12);

        if (i == steps) break;

        back = uia_find_by_name(ctx, win, L"Back", CT_BUTTON);
        if (!back) {
            log_write(LG_WARN, L"No Back button on this page; stopping.");
            break;
        }
        {
            BOOL on = FALSE;
            if (uia_is_enabled(back, &on) && !on) {
                log_write(LG_WARN, L"Back is disabled on this page; stopping.");
                uia_release(back);
                break;
            }
        }
        if (!uia_invoke(back)) {
            log_write(LG_WARN, L"Could not click Back; stopping.");
            uia_release(back);
            break;
        }
        uia_release(back);
        Sleep(1500);
    }

    uia_release(win);
    uia_shutdown(ctx);
    log_write(LG_OK, L"Walk-back probe complete");
    return TRUE;
}

BOOL wizard_probe(const RunConfig *cfg)
{
    HANDLE hproc = NULL;
    DWORD pid, start;
    HWND hwnd = NULL;
    UiaCtx *ctx = NULL;
    UiaEl *win = NULL;
    int n;

    log_write(LG_STEP, L"Probe mode - dumping the NVCleanstall control tree");
    if (!proc_run(cfg->nvcleanstall, NULL, FALSE, 0, NULL, &hproc))
        return FALSE;
    pid = GetProcessId(hproc);

    start = GetTickCount();
    while (!proc_main_window(pid, &hwnd)) {
        if (GetTickCount() - start > T_WINDOW_MS) {
            CloseHandle(hproc);
            return FALSE;
        }
        Sleep(300);
    }
    Sleep(1500);

    if (!uia_init(&ctx)) { CloseHandle(hproc); return FALSE; }
    win = uia_from_hwnd(ctx, hwnd);
    if (win) {
        n = uia_dump_tree(ctx, win, 12);
        log_write(LG_OK, L"%d elements dumped to the log", n);
        uia_release(win);
    }
    uia_shutdown(ctx);
    log_write(LG_INFO, L"NVCleanstall left open so you can step pages and probe again.");
    CloseHandle(hproc);
    return TRUE;
}
