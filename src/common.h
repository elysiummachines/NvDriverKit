/* NvDriverKit - shared declarations.
 *
 * Drives NVCleanstall through a verified clean-install of the NVIDIA driver,
 * then runs NvPostInstall.ps1 to restore saved driver settings.
 *
 * Design rule for the whole program: never click blind. Every control we
 * touch is read back and compared against an explicit manifest before the
 * wizard is allowed to advance.
 */
#ifndef NVDK_COMMON_H
#define NVDK_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
/* COM in C: gives us the IFoo_Method(this, ...) vtable macros. */
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define NVDK_NAME     L"NvDriverKit"
#define NVDK_VERSION  L"1.1.0"

/* --------------------------------------------------------------------------
 * Pins. The UI automation below is written against one exact NVCleanstall
 * build. If any of these stop matching we refuse to drive the wizard rather
 * than clicking at coordinates that may now mean something else.
 * -------------------------------------------------------------------------- */
#define NVCS_WINDOW_TITLE       L"NVCleanstall v1.19.0"
#define NVCS_SIGNER_THUMBPRINT  L"67E2A5706E605E7594D82CC9D00C804742D307B7"
#define NVCS_SIGNER_CN          L"TechPowerUp LLC"

/* AutomationIds confirmed by --probe against NVCleanstall 1.19.0.
 * Page 1 and the Finished page are mapped; the Components and Tweaks pages
 * still match by label and need a --probe pass of their own. */
#define ID_RADIO_AUTODETECT  L"RadioAutodetect"
#define ID_BEST_DRIVER       L"BestDriver"
#define ID_CURRENT_VERSION   L"CurrentDriverVersion"
#define ID_DEVICE_NAME       L"DeviceName"
#define ID_FALLBACK_SERVERS  L"UseFallbackServers"
#define ID_CHECK_BACKGROUND  L"CheckInBackground"
#define ID_BUTTON_NEXT       L"ButtonNext"
#define ID_BUTTON_PREV       L"ButtonPrev"
#define ID_BUILD_PACKAGE     L"BuildSFX"
#define ID_RUN_INSTALLER     L"RunInstaller"
#define ID_BUTTON_ALL        L"ButtonAll"
#define ID_BUTTON_RECOMMENDED L"ButtonRecommended"
#define ID_BUTTON_MINIMUM    L"ButtonMinimum"

/* The component list's checkboxes are unnamed; the label sits on the
 * enclosing ListItem, and reads "Display Driver" (no "(required)" suffix). */
#define COMPONENT_REQUIRED   L"Display Driver"

/* Wizard page headers, used as the page oracle. */
#define PAGE_DRIVER      L"Select Driver Version To Install"
#define PAGE_COMPONENTS  L"Select Components To Install"
#define PAGE_TWEAKS      L"Installation Tweaks"
#define PAGE_FINISHED    L"Finished"

/* Where NVCleanstall persists the tweak page. */
#define NVCS_REG_KEY     L"Software\\techPowerUp\\NVCleanstall"
#define NVCS_REG_VALUE   L"PreviousTweaks"

/* Display adapter class key - source of the installed driver version. */
#define DISPLAY_CLASS_KEY \
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}"

/* Service the post-install script needs; must not be disabled by a tweak. */
#define NV_CONTAINER_SVC L"NVDisplay.ContainerLocalSystem"

#define NVDK_MAX_PATH 32768

/* --------------------------------------------------------------------------
 * Logging (log.c)
 * -------------------------------------------------------------------------- */
typedef enum {
    LG_STEP,   /* phase heading */
    LG_INFO,
    LG_OK,
    LG_WARN,
    LG_FAIL
} LogLevel;

typedef void (*LogSink)(LogLevel lvl, const wchar_t *line);

BOOL log_open(const wchar_t *dir);
void log_close(void);
void log_set_sink(LogSink sink);
void log_write(LogLevel lvl, const wchar_t *fmt, ...);
int  log_fail_count(void);
void log_reset_counts(void);

/* --------------------------------------------------------------------------
 * Security primitives (sec.c)
 * -------------------------------------------------------------------------- */

/* Lowercase hex SHA-256 of a file. out needs 65 wchar_t. */
BOOL sec_sha256_file(const wchar_t *path, wchar_t *out_hex, size_t out_cch);

/* Authenticode chain validation (WinVerifyTrust, GENERIC_VERIFY_V2). */
BOOL sec_verify_authenticode(const wchar_t *path);

/* Uppercase hex SHA-1 thumbprint of the signer cert. out needs 41 wchar_t. */
BOOL sec_signer_thumbprint(const wchar_t *path, wchar_t *out_hex, size_t out_cch);

/* Signer subject CN. */
BOOL sec_signer_cn(const wchar_t *path, wchar_t *out, size_t out_cch);

/* TRUE in *out_writable if any principal outside SYSTEM / Administrators /
 * TrustedInstaller holds write, delete or DACL-write on the file OR on its
 * parent directory. An elevated process must never execute such a path. */
BOOL sec_path_is_nonadmin_writable(const wchar_t *path, BOOL *out_writable);

/* Replace a directory's DACL with SYSTEM+Administrators full control only. */
BOOL sec_harden_directory(const wchar_t *dir);

BOOL sec_is_elevated(void);

/* --------------------------------------------------------------------------
 * Installed driver info (nv.c)
 * -------------------------------------------------------------------------- */
typedef struct {
    wchar_t desc[160];         /* "NVIDIA GeForce RTX 3080" */
    wchar_t provider[80];      /* "NVIDIA" */
    wchar_t raw_version[64];   /* "32.0.16.2101" */
    wchar_t nv_version[16];    /* "621.01" */
} NvDriverInfo;

BOOL nv_get_installed(NvDriverInfo *out);

/* "32.0.16.2101" -> "621.01" (last five digits, dot before the final two). */
BOOL nv_raw_to_nv_version(const wchar_t *raw, wchar_t *out, size_t cch);

/* Leading version of "621.01-desktop-win10-win11-64bit-..." -> "621.01". */
BOOL nv_version_from_package_name(const wchar_t *pkg, wchar_t *out, size_t cch);

/* <0, 0, >0. Compares numerically, e.g. "621.01" vs "580.88". */
int nv_compare_versions(const wchar_t *a, const wchar_t *b);

/* Write the GPU's interrupt priority straight to its PCI device key
 * (Device Parameters\Interrupt Management\Affinity Policy\DevicePriority).
 *
 * NVCleanstall's Interrupt Policy/Priority combos are ComboBox controls that
 * expose NO UI Automation patterns and no children, so they cannot be read or
 * driven; see the README. This writes the same value the installer would and
 * reads it back to confirm. 0 Undefined, 1 Low, 2 Normal, 3 High.
 * device_out receives the PCI instance that was written. */
BOOL nv_set_interrupt_priority(int priority, wchar_t *device_out, size_t cch);

/* Look up an integer from the tweak manifest by key. */
int  tw_int(const wchar_t *key, int fallback);

/* --------------------------------------------------------------------------
 * Tweak manifest (nv.c)
 *
 * NVCleanstall stores the whole Installation Tweaks page as a flat JSON
 * object in NVCS_REG_KEY\PreviousTweaks. We write it, then verify the UI
 * agrees, instead of toggling fourteen checkboxes by hand.
 * -------------------------------------------------------------------------- */
typedef enum { TW_BOOL, TW_INT, TW_STR } TweakType;

typedef struct {
    const wchar_t *key;
    TweakType      type;
    int            num;        /* TW_BOOL: 0/1, TW_INT: value */
    const wchar_t *str;        /* TW_STR */
    const wchar_t *ui_name;    /* checkbox name on the Tweaks page, or NULL */
} Tweak;

const Tweak *tw_manifest(size_t *count);

/* Serialize the manifest and write it to the registry. */
BOOL tw_write_manifest(void);

/* Read PreviousTweaks back and confirm every manifest field matches.
 * Differences are appended to problems (may be NULL). */
BOOL tw_verify_registry(wchar_t *problems, size_t cch);

/* --------------------------------------------------------------------------
 * UI Automation helpers (uia.c)
 * -------------------------------------------------------------------------- */
typedef struct UiaCtx UiaCtx;
typedef struct UiaEl  UiaEl;

BOOL     uia_init(UiaCtx **out);
void     uia_shutdown(UiaCtx *ctx);

UiaEl   *uia_from_hwnd(UiaCtx *ctx, HWND hwnd);
void     uia_release(UiaEl *el);

/* Find a descendant by exact name, optionally restricted to a control type
 * (0 = any). Returns NULL if absent. Caller releases. */
UiaEl   *uia_find_by_name(UiaCtx *ctx, UiaEl *root, const wchar_t *name, int ctrl_type);

/* Match on AutomationId - preferred over names wherever an id is known. */
UiaEl   *uia_find_by_id(UiaCtx *ctx, UiaEl *root, const wchar_t *autoid);
UiaEl   *uia_wait_for_id(UiaCtx *ctx, UiaEl *root, const wchar_t *autoid, DWORD timeout_ms);

/* Poll until the element's Name is non-empty (labels that fill in async). */
BOOL     uia_wait_nonempty_name(UiaEl *el, wchar_t *out, size_t cch, DWORD timeout_ms);

/* Ticked rows of a list whose entries are ListItems holding an unnamed
 * CheckBox: the label is read from the ListItem, not the box, because the box
 * has no name. This is how the component list is built. names_out gets a
 * comma-separated list of the ticked rows. */
int      uia_list_checked_items(UiaCtx *ctx, UiaEl *root, wchar_t *names_out, size_t cch);

/* Poll for an element by name until timeout_ms elapses. */
UiaEl   *uia_wait_for_name(UiaCtx *ctx, UiaEl *root, const wchar_t *name,
                           int ctrl_type, DWORD timeout_ms);

BOOL     uia_get_name(UiaEl *el, wchar_t *out, size_t cch);
BOOL     uia_is_enabled(UiaEl *el, BOOL *out);

/* Toggle pattern. state: 0 off, 1 on, 2 indeterminate. */
BOOL     uia_get_toggle(UiaEl *el, int *state);
BOOL     uia_set_toggle(UiaEl *el, int want);

BOOL     uia_is_selected(UiaEl *el, BOOL *out);   /* radio buttons */
BOOL     uia_select(UiaEl *el);

BOOL     uia_invoke(UiaEl *el);                   /* buttons, links */
BOOL     uia_get_value(UiaEl *el, wchar_t *out, size_t cch);
BOOL     uia_set_value(UiaEl *el, const wchar_t *text);
BOOL     uia_scroll_into_view(UiaEl *el);

/* Collect the names of every descendant of the given control type.
 * Used by --probe to re-map the UI after an NVCleanstall update. */
int      uia_dump_tree(UiaCtx *ctx, UiaEl *root, int max_depth);

/* Log one element's type, enabled state, exposed patterns and children. */
void     uia_log_element(UiaCtx *ctx, UiaEl *el, const wchar_t *label);

/* UIA control type ids we use. */
#define CT_ANY       0
#define CT_BUTTON    50000
#define CT_CHECKBOX  50002
#define CT_COMBOBOX  50003
#define CT_EDIT      50004
#define CT_HYPERLINK 50005
#define CT_LISTITEM  50007
#define CT_RADIO     50013
#define CT_TEXT      50020
#define CT_WINDOW    50032

/* --------------------------------------------------------------------------
 * Orchestration (wizard.c)
 * -------------------------------------------------------------------------- */
typedef enum {
    RUN_UP_TO_DATE,     /* installed driver already matches the best driver */
    RUN_INSTALLED,      /* package built, installed, post-install script ran */
    RUN_CAPTURED,       /* settings read back from the driver and saved */
    RUN_ABORTED,        /* a precondition or verification failed */
    RUN_ERROR
} RunResult;

typedef struct {
    wchar_t nvcleanstall[NVDK_MAX_PATH];  /* NVCleanstall_1.19.0.exe */
    wchar_t post_script[NVDK_MAX_PATH];   /* NvPostInstall.ps1 (source copy) */
    wchar_t data_dir[NVDK_MAX_PATH];      /* %ProgramData%\NvDriverKit */
    wchar_t package_dir[NVDK_MAX_PATH];   /* where the built package is saved */
    wchar_t exec_dir[NVDK_MAX_PATH];      /* admin-only dir the package runs from */
    BOOL    dry_run;                      /* drive the wizard, never install */
    BOOL    force;                        /* reinstall even if already current */
} RunConfig;

BOOL      cfg_load(RunConfig *cfg);
RunResult wizard_run(const RunConfig *cfg, volatile LONG *cancel_flag);

/* Fetch Settings: stages and runs NvPostInstall.ps1 -Capture so the current
 * driver settings are saved without the user opening a PowerShell prompt,
 * then reports what actually landed on disk. Reads the driver and writes only
 * the two files under Documents\NvPostInstall; it installs nothing. */
RunResult wizard_capture(const RunConfig *cfg, volatile LONG *cancel_flag);

/* Exposed for --probe. */
BOOL wizard_probe(const RunConfig *cfg);

/* --probe-attach: dump an NVCleanstall window that is already open, so the
 * later wizard pages can be mapped without triggering a driver download. */
BOOL wizard_probe_attach(void);

/* --probe-back N: dump the current page, then click Back and dump again,
 * N times. Back only navigates; nothing is downloaded or installed. */
BOOL wizard_probe_back(int steps);

/* --------------------------------------------------------------------------
 * Process helpers (wizard.c)
 * -------------------------------------------------------------------------- */

/* CreateProcessW with lpApplicationName set to an absolute, already verified
 * path. Never uses a shell and never resolves the image through PATH. */
BOOL proc_run(const wchar_t *exe, const wchar_t *args, BOOL show_console,
              DWORD timeout_ms, DWORD *out_exit, HANDLE *out_proc);

BOOL proc_main_window(DWORD pid, HWND *out);

/* Run a console program with NO window, write stdin_utf8 to its input (may be
 * NULL) and relay every line it prints into the log, so it shows up in the
 * application's own output rather than a separate console. */
BOOL proc_run_logged(const wchar_t *exe, const wchar_t *args,
                     const char *stdin_utf8, DWORD timeout_ms, DWORD *out_exit);

#endif /* NVDK_COMMON_H */
