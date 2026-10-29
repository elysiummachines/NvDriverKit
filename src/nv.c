/* Installed driver version, and the NVCleanstall tweak manifest. */
#include "common.h"
#include <stdio.h>

/* ====================================================================
 *  Installed driver version
 * ==================================================================== */

/* NVIDIA's marketing version is the last five digits of the Windows driver
 * version with a dot before the final two: 32.0.16.2101 -> 621.01 */
BOOL nv_raw_to_nv_version(const wchar_t *raw, wchar_t *out, size_t cch)
{
    wchar_t digits[64];
    size_t  n = 0, i;

    if (cch < 8) return FALSE;

    for (i = 0; raw[i] && n < 63; i++)
        if (raw[i] >= L'0' && raw[i] <= L'9')
            digits[n++] = raw[i];
    digits[n] = L'\0';

    if (n < 5) return FALSE;

    _snwprintf(out, cch, L"%.3s.%.2s", digits + n - 5, digits + n - 2);
    out[cch - 1] = L'\0';
    return TRUE;
}

/* "621.01-desktop-win10-win11-64bit-international-dch-whql" -> "621.01" */
BOOL nv_version_from_package_name(const wchar_t *pkg, wchar_t *out, size_t cch)
{
    size_t i = 0;

    if (!pkg || cch < 8) return FALSE;

    while (pkg[i] && ((pkg[i] >= L'0' && pkg[i] <= L'9') || pkg[i] == L'.')) {
        if (i >= cch - 1) return FALSE;
        out[i] = pkg[i];
        i++;
    }
    out[i] = L'\0';

    /* Must look like NNN.NN */
    return i >= 5 && wcschr(out, L'.') != NULL;
}

int nv_compare_versions(const wchar_t *a, const wchar_t *b)
{
    double da = _wtof(a);
    double db = _wtof(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

BOOL nv_get_installed(NvDriverInfo *out)
{
    HKEY  cls = NULL;
    DWORD idx = 0;
    BOOL  found = FALSE;

    ZeroMemory(out, sizeof(*out));

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DISPLAY_CLASS_KEY, 0,
                      KEY_READ | KEY_WOW64_64KEY, &cls) != ERROR_SUCCESS)
        return FALSE;

    for (;; idx++) {
        wchar_t sub[64];
        DWORD   sub_cch = 64;
        HKEY    dev = NULL;
        wchar_t provider[80] = L"";
        DWORD   cb, type;

        if (RegEnumKeyExW(cls, idx, sub, &sub_cch, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;

        if (RegOpenKeyExW(cls, sub, 0, KEY_READ | KEY_WOW64_64KEY, &dev)
                != ERROR_SUCCESS)
            continue;

        cb = sizeof(provider);
        if (RegQueryValueExW(dev, L"ProviderName", NULL, &type,
                             (BYTE *)provider, &cb) == ERROR_SUCCESS &&
            type == REG_SZ && wcsstr(provider, L"NVIDIA")) {

            wcsncpy(out->provider, provider, 79);

            cb = sizeof(out->raw_version);
            RegQueryValueExW(dev, L"DriverVersion", NULL, &type,
                             (BYTE *)out->raw_version, &cb);
            cb = sizeof(out->desc);
            RegQueryValueExW(dev, L"DriverDesc", NULL, &type,
                             (BYTE *)out->desc, &cb);

            if (nv_raw_to_nv_version(out->raw_version, out->nv_version, 16))
                found = TRUE;
        }
        RegCloseKey(dev);
        if (found) break;
    }

    RegCloseKey(cls);
    return found;
}

/* ====================================================================
 *  Tweak manifest
 *
 *  This is the single source of truth for the Installation Tweaks page.
 *  It mirrors the configuration captured from the working setup, with one
 *  deliberate change: UnattendedReboot is forced off so the NVIDIA installer
 *  cannot reboot the machine before NvPostInstall.ps1 has run. This program
 *  owns the reboot decision.
 * ==================================================================== */

static const Tweak g_manifest[] = {
    /* key                        type      num  str   UI checkbox name */
    { L"DisableInstallerTelemetry", TW_BOOL, 1, NULL, L"Disable Installer Telemetry & Advertising" },
    { L"Unattended",                TW_BOOL, 1, NULL, L"Unattended Express Installation" },
    { L"UnattendedReboot",          TW_BOOL, 0, NULL, L"Allow automatic reboot, if needed" },
    { L"CleanInstall",              TW_BOOL, 1, NULL, L"Perform a Clean Installation" },
    { L"InstallDCHControlPanel",    TW_BOOL, 0, NULL, NULL },
    { L"AddHardwareId",             TW_BOOL, 0, NULL, L"Add Hardware Support" },
    { L"ShowDlssIndicator",         TW_BOOL, 0, NULL, L"Enable DLSS Indicator" },
    { L"DisableMPO",                TW_BOOL, 1, NULL, L"Disable Multiplane Overlay (MPO)" },
    { L"DisableNvCamera",           TW_BOOL, 0, NULL, L"Disable Ansel" },
    { L"ShowExpertOptions",         TW_BOOL, 1, NULL, L"Show Expert Tweaks" },
    { L"DisableDriverTelemetry",    TW_BOOL, 1, NULL, L"Disable Driver Telemetry" },
    /* Must stay off: NvPostInstall.ps1 requires NVDisplay.ContainerLocalSystem
     * and the NVIDIA Control Panel will not open without it. */
    { L"DisableNvContainer",        TW_BOOL, 0, NULL, L"Disable NVIDIA Container" },
    { L"DisableHDAudioSleepTimer",  TW_BOOL, 0, NULL, L"Disable NVIDIA HD Audio device sleep timer" },
    { L"EnableMSI",                 TW_BOOL, 1, NULL, L"Enable Message Signaled Interrupts" },
    { L"DisableHDCP",               TW_BOOL, 1, NULL, L"Disable HDCP" },
    { L"NvEncPatch",                TW_BOOL, 1, NULL, L"Apply NVENC Video Encoding Session Limit Patch" },
    { L"RunProgram",                TW_BOOL, 0, NULL, L"Start external application" },
    { L"RebuildSignatureEAC",       TW_BOOL, 1, NULL, L"Use method compatible with Easy-Anti-Cheat" },
    { L"SkipUnsignedDriverWarning", TW_BOOL, 1, NULL, L"Automatically accept the \"driver unsigned\" warning" },
    { L"AddIdId",                   TW_STR,  0, L"",  NULL },
    { L"AddIdName",                 TW_STR,  0, L"NVIDIA Graphics Device", NULL },
    { L"RunBefore",                 TW_STR,  0, L"",  NULL },
    { L"RunAfter",                  TW_STR,  0, L"",  NULL },
    { L"AddIdTemplate",             TW_STR,  0, L"",  NULL },
    { L"MSIPolicy",                 TW_INT,  0, NULL, NULL },   /* Default */
    { L"MSIPriority",               TW_INT,  3, NULL, NULL },   /* High    */
    { L"NvEncPatchVersions",        TW_INT,  0, NULL, NULL },
};

const Tweak *tw_manifest(size_t *count)
{
    *count = sizeof(g_manifest) / sizeof(g_manifest[0]);
    return g_manifest;
}

int tw_int(const wchar_t *key, int fallback)
{
    size_t n, i;
    const Tweak *m = tw_manifest(&n);
    for (i = 0; i < n; i++)
        if (wcscmp(m[i].key, key) == 0)
            return m[i].num;
    return fallback;
}

/* ====================================================================
 *  Interrupt priority, written directly to the device key
 * ==================================================================== */

BOOL nv_set_interrupt_priority(int priority, wchar_t *device_out, size_t cch)
{
    HKEY  pci = NULL, ven = NULL, inst = NULL, ap = NULL;
    DWORD i;
    BOOL  done = FALSE;

    if (device_out && cch) device_out[0] = L'\0';

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0,
                      KEY_READ | KEY_WOW64_64KEY, &pci) != ERROR_SUCCESS)
        return FALSE;

    for (i = 0; !done; i++) {
        wchar_t vname[256];
        DWORD   vcch = 256, j;

        if (RegEnumKeyExW(pci, i, vname, &vcch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (_wcsnicmp(vname, L"VEN_10DE", 8) != 0)      /* NVIDIA */
            continue;
        if (RegOpenKeyExW(pci, vname, 0, KEY_READ | KEY_WOW64_64KEY, &ven) != ERROR_SUCCESS)
            continue;

        for (j = 0; !done; j++) {
            wchar_t iname[256], svc[64] = L"";
            DWORD   icch = 256, cb = sizeof(svc), type = 0;

            if (RegEnumKeyExW(ven, j, iname, &icch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExW(ven, iname, 0, KEY_READ | KEY_WOW64_64KEY, &inst)
                    != ERROR_SUCCESS)
                continue;

            /* The display adapter is the one bound to the nvlddmkm driver. */
            if (RegQueryValueExW(inst, L"Service", NULL, &type, (BYTE *)svc, &cb)
                    == ERROR_SUCCESS && _wcsicmp(svc, L"nvlddmkm") == 0) {
                wchar_t sub[640];
                _snwprintf(sub, 640,
                           L"%s\\%s\\Device Parameters\\Interrupt Management\\Affinity Policy",
                           vname, iname);
                sub[639] = L'\0';

                if (RegCreateKeyExW(pci, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                                    NULL, &ap, NULL) == ERROR_SUCCESS) {
                    DWORD want = (DWORD)priority, got = 0, gcb = sizeof(got);

                    if (RegSetValueExW(ap, L"DevicePriority", 0, REG_DWORD,
                                       (const BYTE *)&want, sizeof(want)) == ERROR_SUCCESS &&
                        RegQueryValueExW(ap, L"DevicePriority", NULL, &type,
                                         (BYTE *)&got, &gcb) == ERROR_SUCCESS &&
                        got == want)
                        done = TRUE;

                    RegCloseKey(ap);
                    ap = NULL;
                }
                if (device_out && cch)
                    _snwprintf(device_out, cch, L"%s\\%s", vname, iname);
            }
            RegCloseKey(inst);
            inst = NULL;
        }
        RegCloseKey(ven);
        ven = NULL;
    }

    RegCloseKey(pci);
    return done;
}

BOOL tw_write_manifest(void)
{
    wchar_t json[8192];
    size_t  n, i, used = 0;
    const Tweak *m = tw_manifest(&n);
    HKEY    key = NULL;
    LONG    rc;
    int     w;

    w = _snwprintf(json, 8192, L"{\r\n");
    if (w < 0) return FALSE;
    used = (size_t)w;

    for (i = 0; i < n; i++) {
        const wchar_t *comma = (i + 1 < n) ? L"," : L"";
        switch (m[i].type) {
        case TW_BOOL:
            w = _snwprintf(json + used, 8192 - used, L"  \"%s\": %s%s\r\n",
                           m[i].key, m[i].num ? L"true" : L"false", comma);
            break;
        case TW_INT:
            w = _snwprintf(json + used, 8192 - used, L"  \"%s\": %d%s\r\n",
                           m[i].key, m[i].num, comma);
            break;
        default:
            w = _snwprintf(json + used, 8192 - used, L"  \"%s\": \"%s\"%s\r\n",
                           m[i].key, m[i].str ? m[i].str : L"", comma);
            break;
        }
        if (w < 0) return FALSE;
        used += (size_t)w;
        if (used >= 8000) return FALSE;
    }
    _snwprintf(json + used, 8192 - used, L"}");
    json[8191] = L'\0';

    rc = RegCreateKeyExW(HKEY_CURRENT_USER, NVCS_REG_KEY, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        log_write(LG_FAIL, L"Cannot open %s (error %ld)", NVCS_REG_KEY, rc);
        return FALSE;
    }
    rc = RegSetValueExW(key, NVCS_REG_VALUE, 0, REG_SZ, (const BYTE *)json,
                        (DWORD)((wcslen(json) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS) {
        log_write(LG_FAIL, L"Cannot write %s (error %ld)", NVCS_REG_VALUE, rc);
        return FALSE;
    }
    log_write(LG_OK, L"Wrote tweak manifest (%u settings) to HKCU\\%s",
              (unsigned)n, NVCS_REG_KEY);
    return TRUE;
}

/* Minimal reader for the flat JSON object NVCleanstall stores. Looks up one
 * key and returns the raw token after the colon. Not a general parser; the
 * value shapes are bool, int and quoted string only. */
static BOOL json_lookup(const wchar_t *json, const wchar_t *key,
                        wchar_t *out, size_t cch)
{
    wchar_t pat[128];
    const wchar_t *p, *q;
    size_t i = 0;

    _snwprintf(pat, 128, L"\"%s\"", key);
    pat[127] = L'\0';

    p = wcsstr(json, pat);
    if (!p) return FALSE;
    p += wcslen(pat);

    while (*p == L' ' || *p == L'\t') p++;
    if (*p != L':') return FALSE;
    p++;
    while (*p == L' ' || *p == L'\t') p++;

    if (*p == L'"') {                      /* quoted string */
        p++;
        q = p;
        while (*q && *q != L'"') {
            if (i >= cch - 1) return FALSE;
            out[i++] = *q++;
        }
    } else {                               /* bool or number */
        q = p;
        while (*q && *q != L',' && *q != L'\r' && *q != L'\n' && *q != L'}') {
            if (i >= cch - 1) return FALSE;
            out[i++] = *q++;
        }
        while (i > 0 && (out[i - 1] == L' ' || out[i - 1] == L'\t')) i--;
    }
    out[i] = L'\0';
    return TRUE;
}

BOOL tw_verify_registry(wchar_t *problems, size_t cch)
{
    wchar_t *json = NULL;
    HKEY     key = NULL;
    DWORD    cb = 0, type = 0;
    size_t   n, i;
    const Tweak *m = tw_manifest(&n);
    BOOL     ok = TRUE;

    if (problems && cch) problems[0] = L'\0';

    if (RegOpenKeyExW(HKEY_CURRENT_USER, NVCS_REG_KEY, 0, KEY_QUERY_VALUE, &key)
            != ERROR_SUCCESS)
        return FALSE;

    if (RegQueryValueExW(key, NVCS_REG_VALUE, NULL, &type, NULL, &cb)
            != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(key);
        return FALSE;
    }
    json = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb + sizeof(wchar_t));
    if (!json) { RegCloseKey(key); return FALSE; }

    if (RegQueryValueExW(key, NVCS_REG_VALUE, NULL, &type, (BYTE *)json, &cb)
            != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, json);
        RegCloseKey(key);
        return FALSE;
    }
    RegCloseKey(key);

    for (i = 0; i < n; i++) {
        wchar_t got[256], want[256];

        if (!json_lookup(json, m[i].key, got, 256)) {
            ok = FALSE;
            log_write(LG_FAIL, L"Tweak %s missing from registry", m[i].key);
            continue;
        }
        switch (m[i].type) {
        case TW_BOOL: wcscpy(want, m[i].num ? L"true" : L"false"); break;
        case TW_INT:  _snwprintf(want, 256, L"%d", m[i].num); break;
        default:      wcsncpy(want, m[i].str ? m[i].str : L"", 255); want[255] = L'\0'; break;
        }
        if (wcscmp(got, want) != 0) {
            ok = FALSE;
            log_write(LG_FAIL, L"Tweak %s is \"%s\", expected \"%s\"",
                      m[i].key, got, want);
            if (problems && cch) {
                size_t len = wcslen(problems);
                _snwprintf(problems + len, cch - len, L"%s%s", len ? L", " : L"", m[i].key);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, json);
    return ok;
}
