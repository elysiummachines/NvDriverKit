/* NvDriverKit - window, worker thread, reboot prompt. */
#include "common.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>

/* CHARFORMAT2W and EM_SETBKGNDCOLOR are Rich Edit 2.0 and later. */
#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER 0x0500
#endif
#include <richedit.h>
#ifndef MSFTEDIT_CLASS
#define MSFTEDIT_CLASS L"RICHEDIT50W"
#endif

#define IDC_CHECK   1001
#define IDC_LOG     1002
#define IDC_STATUS  1003
#define IDC_CANCEL  1004
#define IDC_FETCH   1005
#define IDC_THEME   1006

#define WM_LOGLINE  (WM_APP + 1)
#define WM_RUNDONE  (WM_APP + 2)

/* Which job the one worker thread is running. Both end in a RunResult and a
 * WM_RUNDONE, so only the dispatch differs. */
typedef enum { WORK_RUN, WORK_CAPTURE } WorkKind;

static HWND  g_main, g_btn, g_fetch, g_cancel, g_log, g_status, g_theme;
static WorkKind g_work = WORK_RUN;
static HFONT g_ui_font, g_mono_font;
static volatile LONG g_cancel_flag = 0;
static volatile LONG g_running = 0;
static RunConfig g_cfg;
static BOOL g_probe_mode = FALSE;
static BOOL g_probe_attach = FALSE;
static int  g_probe_back = 0;
static BOOL g_rich = FALSE;        /* log is a rich edit, not a plain EDIT */
static HBRUSH g_edit_brush = NULL; /* only used on the plain EDIT fallback */

/* ------------------------------------------------------- button hot state */

/* Owner-drawn buttons get no hover state of their own, so each one is
 * subclassed to track the pointer and repaint on enter and leave. */
#define BTN_SLOTS 4
static struct { HWND h; BOOL hot; } g_hot[BTN_SLOTS];

static BOOL *hot_slot(HWND h)
{
    int i;
    for (i = 0; i < BTN_SLOTS; i++)
        if (g_hot[i].h == h) return &g_hot[i].hot;
    return NULL;
}

static LRESULT CALLBACK btn_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref)
{
    BOOL *hot = hot_slot(h);
    (void)id; (void)ref;

    switch (msg) {
    case WM_MOUSEMOVE:
        if (hot && !*hot) {
            TRACKMOUSEEVENT tme;
            tme.cbSize      = sizeof(tme);
            tme.dwFlags     = TME_LEAVE;
            tme.hwndTrack   = h;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            *hot = TRUE;
            InvalidateRect(h, NULL, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        if (hot && *hot) {
            *hot = FALSE;
            InvalidateRect(h, NULL, TRUE);
        }
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void track_button(HWND h)
{
    int i;
    for (i = 0; i < BTN_SLOTS; i++) {
        if (g_hot[i].h) continue;
        g_hot[i].h   = h;
        g_hot[i].hot = FALSE;
        SetWindowSubclass(h, btn_subclass, (UINT_PTR)i, 0);
        return;
    }
}

/* ------------------------------------------------------------------ log */

/* The rich edit holds every line in the colour of its level, and nothing in
 * the control records which line was a step and which a failure. A theme flip
 * therefore has to re-emit the text rather than recolour it in place, so the
 * lines are kept here as well. Touched only from the UI thread. */
typedef struct { LogLevel lvl; wchar_t *text; } LogLine;
static LogLine *g_lines = NULL;
static size_t   g_lines_n = 0, g_lines_cap = 0;

static void log_emit(LogLevel lvl, const wchar_t *text)
{
    int len = GetWindowTextLengthW(g_log);

    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    if (g_rich) {
        CHARFORMAT2W cf;
        ZeroMemory(&cf, sizeof(cf));
        cf.cbSize      = sizeof(cf);
        cf.dwMask      = CFM_COLOR;
        cf.crTextColor = theme_level_color(lvl);
        SendMessageW(g_log, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void log_ui_clear(void)
{
    size_t i;

    for (i = 0; i < g_lines_n; i++) free(g_lines[i].text);
    g_lines_n = 0;
    SetWindowTextW(g_log, L"");
}

static void log_append(LogLevel lvl, const wchar_t *text)
{
    if (g_lines_n == g_lines_cap) {
        size_t cap = g_lines_cap ? g_lines_cap * 2 : 128;
        LogLine *p = (LogLine *)realloc(g_lines, cap * sizeof(*p));
        /* Failing to grow costs the replay on a theme flip and nothing else,
         * so the line still goes to the control either way. */
        if (p) { g_lines = p; g_lines_cap = cap; }
    }
    if (g_lines_n < g_lines_cap) {
        wchar_t *copy = _wcsdup(text);
        if (copy) {
            g_lines[g_lines_n].lvl  = lvl;
            g_lines[g_lines_n].text = copy;
            g_lines_n++;
        }
    }
    log_emit(lvl, text);
}

static void log_replay(void)
{
    size_t i;

    SendMessageW(g_log, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_log, L"");
    for (i = 0; i < g_lines_n; i++)
        log_emit(g_lines[i].lvl, g_lines[i].text);
    SendMessageW(g_log, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_log, NULL, TRUE);
}

static void ui_sink(LogLevel lvl, const wchar_t *line)
{
    wchar_t buf[4200];
    const wchar_t *prefix;

    switch (lvl) {
    case LG_STEP: prefix = L"\r\n== "; break;
    case LG_OK:   prefix = L"   [ok]   "; break;
    case LG_WARN: prefix = L"   [!]    "; break;
    case LG_FAIL: prefix = L"   [FAIL] "; break;
    default:      prefix = L"          "; break;
    }
    _snwprintf(buf, 4200, L"%s%s\r\n", prefix, line);
    buf[4199] = L'\0';
    PostMessageW(g_main, WM_LOGLINE, (WPARAM)lvl, (LPARAM)_wcsdup(buf));
}

static void set_status(const wchar_t *text)
{
    SetWindowTextW(g_status, text);
}

/* ----------------------------------------------------------------- theme */

static void theme_apply(HWND hwnd)
{
    const ThemeColors *c = theme_colors();

    if (g_edit_brush) DeleteObject(g_edit_brush);
    g_edit_brush = CreateSolidBrush(c->edit_bg);

    theme_apply_titlebar(hwnd);
    theme_apply_scrollbars(g_log);

    if (g_rich)
        SendMessageW(g_log, EM_SETBKGNDCOLOR, 0, (LPARAM)c->edit_bg);

    SetWindowTextW(g_theme, theme_label());
    log_replay();

    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* --------------------------------------------------------------- reboot */

static BOOL reboot_now(void)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return FALSE;
    if (!LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return FALSE;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(tok);

    if (GetLastError() != ERROR_SUCCESS)
        return FALSE;

    return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                         SHTDN_REASON_MAJOR_SOFTWARE |
                         SHTDN_REASON_MINOR_INSTALLATION |
                         SHTDN_REASON_FLAG_PLANNED);
}

/* --------------------------------------------------------------- worker */

static DWORD WINAPI worker(LPVOID param)
{
    RunResult r;
    (void)param;

    log_reset_counts();
    if (g_probe_back > 0) {
        r = wizard_probe_back(g_probe_back) ? RUN_ABORTED : RUN_ERROR;
    } else if (g_probe_attach) {
        r = wizard_probe_attach() ? RUN_ABORTED : RUN_ERROR;
    } else if (g_probe_mode) {
        r = wizard_probe(&g_cfg) ? RUN_ABORTED : RUN_ERROR;
    } else if (g_work == WORK_CAPTURE) {
        r = wizard_capture(&g_cfg, &g_cancel_flag);
    } else {
        r = wizard_run(&g_cfg, &g_cancel_flag);
    }
    PostMessageW(g_main, WM_RUNDONE, (WPARAM)r, 0);
    return 0;
}

static void start_work(WorkKind kind)
{
    HANDLE th;

    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return;

    g_work = kind;
    g_cancel_flag = 0;
    log_ui_clear();
    EnableWindow(g_btn, FALSE);
    EnableWindow(g_fetch, FALSE);
    EnableWindow(g_cancel, TRUE);
    set_status(kind == WORK_CAPTURE ? L"Fetching settings..." : L"Working...");

    th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!th) {
        InterlockedExchange(&g_running, 0);
        EnableWindow(g_btn, TRUE);
        EnableWindow(g_fetch, TRUE);
        EnableWindow(g_cancel, FALSE);
        set_status(L"Could not start the worker thread.");
        return;
    }
    CloseHandle(th);
}

static void finish_run(RunResult r)
{
    InterlockedExchange(&g_running, 0);
    EnableWindow(g_btn, TRUE);
    EnableWindow(g_fetch, TRUE);
    EnableWindow(g_cancel, FALSE);

    switch (r) {
    case RUN_UP_TO_DATE:
        set_status(L"Up to date - nothing to do.");
        break;

    case RUN_CAPTURED:
        set_status(L"Settings fetched and saved.");
        break;

    case RUN_INSTALLED:
        set_status(L"Driver installed and settings restored.");
        {
            int ans = MessageBoxW(g_main,
                L"The driver was installed and your settings were restored.\n\n"
                L"A reboot is needed before the DSR factors and the GPU "
                L"interrupt priority take effect - the driver only reads both "
                L"at startup.\n\n"
                L"Reboot now?",
                L"NvDriverKit - Reboot", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (ans == IDYES) {
                log_write(LG_INFO, L"Rebooting at the user's request.");
                log_close();
                if (!reboot_now())
                    MessageBoxW(g_main, L"Could not initiate the reboot.",
                                L"NvDriverKit", MB_OK | MB_ICONERROR);
            } else {
                log_write(LG_INFO, L"Reboot declined. The Control Panel will show "
                                   L"the old DSR values until you restart.");
            }
        }
        break;

    case RUN_ABORTED:
        set_status(log_fail_count()
                   ? L"Stopped - see the log. Nothing was installed."
                   : L"Stopped before installing.");
        break;

    default:
        set_status(L"Failed - see the log.");
        break;
    }
}

/* ------------------------------------------------------------------- UI */

static void layout(HWND hwnd)
{
    RECT rc;
    int w, h, x, sw, pad = 12, bh = 34, sh = 22, tw = 150;

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    /* Walk the button row left to right so adding one does not mean redoing
     * the arithmetic for everything after it. */
    x = pad;
    MoveWindow(g_btn,    x, pad, 190, bh, TRUE); x += 190 + 8;
    MoveWindow(g_fetch,  x, pad, 150, bh, TRUE); x += 150 + 8;
    MoveWindow(g_cancel, x, pad, 100, bh, TRUE); x += 100 + 12;

    /* Theme sits hard right. The status label takes whatever is left between
     * the two groups, which the minimum track size keeps positive. */
    MoveWindow(g_theme, w - pad - tw, pad, tw, bh, TRUE);
    sw = (w - pad - tw - pad) - x;
    if (sw < 0) sw = 0;
    MoveWindow(g_status, x, pad + 7, sw, sh, TRUE);

    MoveWindow(g_log, pad, pad + bh + pad,
               w - pad * 2, h - (pad * 2 + bh + pad), TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        /* BS_OWNERDRAW on every button: a themed BUTTON ignores
         * WM_CTLCOLORBTN, so there is no way to tint one for dark mode. */
        g_btn = CreateWindowExW(0, L"BUTTON", L"Check for Driver",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CHECK, NULL, NULL);
        g_fetch = CreateWindowExW(0, L"BUTTON", L"Fetch Settings",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_FETCH, NULL, NULL);
        g_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CANCEL, NULL, NULL);
        g_theme = CreateWindowExW(0, L"BUTTON", theme_label(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_THEME, NULL, NULL);
        g_status = CreateWindowExW(0, L"STATIC", L"Ready.",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, NULL, NULL);

        /* Rich edit so each log level can carry its own colour. msftedit.dll
         * ships with every supported Windows, but the plain EDIT is kept as a
         * fallback: a monochrome log beats no log. No WS_EX_CLIENTEDGE either
         * way - that sunken edge is drawn from system colours and stays light
         * grey in dark mode, so theme.c draws a flat border instead. */
        if (LoadLibraryW(L"msftedit.dll")) {
            g_log = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, NULL, NULL);
            g_rich = (g_log != NULL);
        }
        if (!g_log)
            g_log = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)IDC_LOG, NULL, NULL);

        /* A long run can outrun the default cap, which silently drops the
         * tail of the log where it matters most. */
        if (g_rich)
            SendMessageW(g_log, EM_EXLIMITTEXT, 0, (LPARAM)(4 * 1024 * 1024));
        else
            SendMessageW(g_log, EM_SETLIMITTEXT, (WPARAM)(4 * 1024 * 1024), 0);

        SendMessageW(g_btn,    WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(g_fetch,  WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(g_cancel, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(g_theme,  WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(g_log,    WM_SETFONT, (WPARAM)g_mono_font, TRUE);

        track_button(g_btn);
        track_button(g_fetch);
        track_button(g_cancel);
        track_button(g_theme);
        return 0;

    case WM_SIZE:
        layout(hwnd);
        /* The log's border is drawn by the parent, so the old one has to go
         * when the control moves. */
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_GETMINMAXINFO:
        /* Wide enough that the status label keeps a usable width once the
         * theme button has taken its place on the right. */
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 780;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 400;
        return 0;

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, theme_window_brush());
        theme_frame_control(dc, g_log, hwnd);
        return 1;
    }

    /* A read-only edit sends WM_CTLCOLORSTATIC rather than
     * WM_CTLCOLOREDIT, so the log and the status label arrive together. */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        const ThemeColors *c = theme_colors();
        if ((HWND)lp == g_log) {
            SetTextColor(dc, c->edit_text);
            SetBkColor(dc, c->edit_bg);
            return (LRESULT)g_edit_brush;
        }
        SetTextColor(dc, c->text);
        SetBkColor(dc, c->window);
        return (LRESULT)theme_window_brush();
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lp;
        BOOL *hot;
        if (dis->CtlType != ODT_BUTTON) break;
        hot = hot_slot(dis->hwndItem);
        theme_draw_button(dis, hot && *hot, dis->hwndItem == g_btn);
        return TRUE;
    }

    case WM_SETTINGCHANGE:
        /* lParam is only a string for some notifications, so it is not read
         * at all: re-reading one registry value costs less than the risk, and
         * theme_refresh reports whether anything actually changed. */
        if (theme_refresh())
            theme_apply(hwnd);
        else
            SetWindowTextW(g_theme, theme_label());
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHECK)  start_work(WORK_RUN);
        if (LOWORD(wp) == IDC_FETCH)  start_work(WORK_CAPTURE);
        if (LOWORD(wp) == IDC_THEME) {
            theme_cycle();
            theme_apply(hwnd);
        }
        if (LOWORD(wp) == IDC_CANCEL) {
            InterlockedExchange(&g_cancel_flag, 1);
            set_status(L"Cancelling after the current step...");
            EnableWindow(g_cancel, FALSE);
        }
        return 0;

    case WM_LOGLINE: {
        wchar_t *s = (wchar_t *)lp;
        if (s) { log_append((LogLevel)wp, s); free(s); }
        return 0;
    }

    case WM_RUNDONE:
        finish_run((RunResult)wp);
        return 0;

    case WM_CLOSE:
        if (InterlockedCompareExchange(&g_running, 0, 0) != 0) {
            if (MessageBoxW(hwnd,
                    L"A driver run is in progress. Quitting now may leave the "
                    L"install half-finished.\n\nQuit anyway?",
                    L"NvDriverKit", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
    else
        g_ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_mono_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FF_DONTCARE | FIXED_PITCH, L"Consolas");
    if (!g_mono_font) g_mono_font = g_ui_font;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    int argc = 0, i;
    wchar_t **argv;
    BOOL dry = FALSE, force = FALSE, capture = FALSE;
    int want_theme = -1;

    (void)prev; (void)cmdline;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--probe") == 0)        g_probe_mode = TRUE;
        if (_wcsicmp(argv[i], L"--probe-attach") == 0) g_probe_attach = TRUE;
        if (_wcsicmp(argv[i], L"--probe-back") == 0 && i + 1 < argc)
            g_probe_back = _wtoi(argv[++i]);
        if (_wcsicmp(argv[i], L"--dry-run") == 0)      dry = TRUE;
        if (_wcsicmp(argv[i], L"--force") == 0)        force = TRUE;
        if (_wcsicmp(argv[i], L"--capture") == 0)      capture = TRUE;
        if (_wcsicmp(argv[i], L"--dark") == 0)         want_theme = THEME_DARK;
        if (_wcsicmp(argv[i], L"--light") == 0)        want_theme = THEME_LIGHT;
        if (_wcsicmp(argv[i], L"--theme-auto") == 0)   want_theme = THEME_AUTO;
    }
    if (argv) LocalFree(argv);

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    /* DPI awareness comes from the manifest, so no API call is needed here. */

    if (!cfg_load(&g_cfg)) {
        MessageBoxW(NULL, L"Could not prepare %ProgramData%\\NvDriverKit.",
                    L"NvDriverKit", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_cfg.dry_run = dry;
    g_cfg.force   = force;

    /* Before the window exists: WM_CREATE reads the palette and the theme
     * button's label straight out of this. */
    theme_init(g_cfg.config_ini);
    if (want_theme >= 0)
        theme_set_mode((ThemeMode)want_theme);

    make_fonts();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    /* No class brush: WM_ERASEBKGND fills from the theme instead, which a
     * system colour index cannot follow. */
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"NvDriverKitWnd";
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm       = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    g_main = CreateWindowExW(0, L"NvDriverKitWnd",
        NVDK_NAME L" " NVDK_VERSION,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 880, 580,
        NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    theme_apply(g_main);

    log_set_sink(ui_sink);
    if (!log_open(g_cfg.data_dir))
        MessageBoxW(g_main, L"Could not open the log file. Continuing without it.",
                    L"NvDriverKit", MB_OK | MB_ICONWARNING);

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    if (!sec_is_elevated()) {
        set_status(L"Not elevated - restart as administrator.");
        MessageBoxW(g_main,
            L"NvDriverKit must run as administrator. Installing a driver and "
            L"writing to %ProgramData% both require it.",
            L"NvDriverKit", MB_OK | MB_ICONERROR);
        EnableWindow(g_btn, FALSE);
        EnableWindow(g_fetch, FALSE);
    } else {
        NvDriverInfo info;
        if (nv_get_installed(&info)) {
            wchar_t s[256];
            _snwprintf(s, 256, L"%s - driver %s", info.desc, info.nv_version);
            set_status(s);
        }
        if (dry)   log_write(LG_WARN, L"Dry run: will stop before building a package.");
        if (force) log_write(LG_WARN, L"Force: will proceed even if the driver is current.");
        if (g_probe_mode || g_probe_attach || g_probe_back > 0) {
            /* Probe is a one-shot diagnostic: start it without waiting for a
             * click so it can be run unattended. */
            set_status(L"Probe mode: dumping the control tree...");
            PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDC_CHECK, 0), 0);
        } else if (capture) {
            /* --capture drives the Fetch Settings button rather than calling
             * the worker directly, so an unattended run exercises exactly the
             * path a click takes. */
            PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(IDC_FETCH, 0), 0);
        }
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* BS_OWNERDRAW shares the low style bits with BS_DEFPUSHBUTTON, so
         * the dialog manager can no longer tell which button Enter belongs
         * to. Route it the way it went before the buttons were owner-drawn:
         * to whichever button has focus, else to Check for Driver. */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND f = GetFocus();
            if (f == g_btn || f == g_fetch || f == g_cancel || f == g_theme) {
                if (IsWindowEnabled(f))
                    SendMessageW(g_main, WM_COMMAND,
                                 MAKEWPARAM(GetDlgCtrlID(f), BN_CLICKED),
                                 (LPARAM)f);
                continue;
            }
            if (IsWindowEnabled(g_btn)) {
                SendMessageW(g_main, WM_COMMAND,
                             MAKEWPARAM(IDC_CHECK, BN_CLICKED), (LPARAM)g_btn);
                continue;
            }
        }
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    log_close();
    return 0;
}
