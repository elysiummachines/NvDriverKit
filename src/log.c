/* Append-only log. Everything the program executes, and every verification
 * result, lands here with a timestamp so a run can be audited afterwards. */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

static HANDLE        g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static BOOL          g_lock_ready = FALSE;
static LogSink       g_sink = NULL;
static int           g_fails = 0;

static const wchar_t *tag_of(LogLevel lvl)
{
    switch (lvl) {
    case LG_STEP: return L"==>";
    case LG_OK:   return L"[OK]  ";
    case LG_WARN: return L"[!!]  ";
    case LG_FAIL: return L"[FAIL]";
    default:      return L"      ";
    }
}

BOOL log_open(const wchar_t *dir)
{
    wchar_t path[NVDK_MAX_PATH];
    SYSTEMTIME st;

    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = TRUE;
    }

    GetLocalTime(&st);
    _snwprintf(path, NVDK_MAX_PATH, L"%s\\nvdriverkit-%04u%02u%02u.log",
               dir, st.wYear, st.wMonth, st.wDay);
    path[NVDK_MAX_PATH - 1] = L'\0';

    /* Append only; no truncation, so earlier runs survive. */
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE)
        return FALSE;

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        /* UTF-16LE BOM so Notepad and PowerShell read it correctly. */
        const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD wrote = 0;
        WriteFile(g_file, bom, 2, &wrote, NULL);
    }
    return TRUE;
}

void log_close(void)
{
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void log_set_sink(LogSink sink) { g_sink = sink; }

int  log_fail_count(void) { return g_fails; }
void log_reset_counts(void) { g_fails = 0; }

void log_write(LogLevel lvl, const wchar_t *fmt, ...)
{
    wchar_t body[4096];
    wchar_t line[4600];
    SYSTEMTIME st;
    va_list ap;
    DWORD wrote = 0;

    va_start(ap, fmt);
    _vsnwprintf(body, 4096, fmt, ap);
    va_end(ap);
    body[4095] = L'\0';

    if (lvl == LG_FAIL)
        g_fails++;

    GetLocalTime(&st);
    _snwprintf(line, 4600, L"%02u:%02u:%02u %s %s\r\n",
               st.wHour, st.wMinute, st.wSecond, tag_of(lvl), body);
    line[4599] = L'\0';

    if (g_lock_ready) EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE)
        WriteFile(g_file, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wrote, NULL);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);

    if (g_sink)
        g_sink(lvl, body);
}
