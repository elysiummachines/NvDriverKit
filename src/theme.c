/* NvDriverKit - light and dark theme.
 *
 * Windows draws the pieces of this window four different ways, and none of
 * them follows the system dark setting on its own:
 *
 *   - the caption is drawn by DWM, outside the window, so it takes a window
 *     attribute rather than a colour
 *   - BUTTON with visual styles ignores WM_CTLCOLORBTN, so the push buttons
 *     cannot be tinted and are owner-drawn here instead
 *   - the client background is a brush, and the status text a WM_CTLCOLOR
 *     response
 *   - the log is a rich edit, which takes its background by message and its
 *     text colour per run, which is what gives each log level its own colour
 *
 * The usual shortcut to all of this is uxtheme's ordinal 135
 * (SetPreferredAppMode), which is undocumented, unnamed in the export table
 * and has already changed shape between Windows builds. A program that pins
 * hashes and signer thumbprints has no business calling it, so the dark look
 * is assembled by hand from documented calls.
 *
 * One concession: the theme class name "DarkMode_Explorer" handed to the
 * documented SetWindowTheme, which is the only way to darken a scrollbar
 * without drawing the whole thing. The name is not documented, but an
 * unrecognised one only leaves the control on its default theming, so the
 * worst a future Windows can do here is give the log light scrollbars again.
 * Measured on the log: #F0F0F0 without it, #171717 with it.
 */
#include "common.h"
#include <dwmapi.h>
#include <uxtheme.h>

/* Present in the Windows 11 SDK, absent from older MinGW headers. 20 is the
 * documented attribute; 19 is the pre-20H1 value, kept as a fallback because
 * the releases that shipped it are still in support. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define NVDK_DWMWA_DARK_PRE_20H1 19

#define PERSONALIZE_KEY \
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

static ThemeMode g_mode     = THEME_AUTO;
static BOOL      g_sys_dark = FALSE;
static BOOL      g_dark     = FALSE;
static HBRUSH    g_win_brush = NULL;
static wchar_t   g_ini[NVDK_MAX_PATH];

/* --------------------------------------------------------------- palettes */

/* Values follow the Windows 11 common controls so the window does not look
 * foreign beside a File Explorer in the same theme. Designated initialisers
 * so adding a colour to the struct cannot silently shift the others. */
static const ThemeColors C_DARK = {
    .window     = RGB(0x20, 0x20, 0x20),
    .text       = RGB(0xE6, 0xE6, 0xE6),
    .dim        = RGB(0xA0, 0xA0, 0xA0),
    .edit_bg    = RGB(0x18, 0x18, 0x18),
    .edit_text  = RGB(0xDC, 0xDC, 0xDC),
    .border     = RGB(0x3A, 0x3A, 0x3A),
    .btn_face   = RGB(0x2D, 0x2D, 0x2D),
    .btn_hot    = RGB(0x38, 0x38, 0x38),
    .btn_down   = RGB(0x27, 0x27, 0x27),
    .btn_border = RGB(0x45, 0x45, 0x45),
    .btn_text   = RGB(0xE6, 0xE6, 0xE6),
    .dis_face   = RGB(0x26, 0x26, 0x26),
    .dis_text   = RGB(0x6B, 0x6B, 0x6B),
    .dis_border = RGB(0x30, 0x30, 0x30),
    .def_face   = RGB(0x4C, 0xC2, 0xFF),
    .def_hot    = RGB(0x65, 0xCB, 0xFF),
    .def_down   = RGB(0x3E, 0xA8, 0xE0),
    .def_text   = RGB(0x10, 0x14, 0x18),
    .focus      = RGB(0xFF, 0xFF, 0xFF),
    .lv_step    = RGB(0x7F, 0xD0, 0xFF),
    .lv_info    = RGB(0xC8, 0xC8, 0xC8),
    .lv_ok      = RGB(0x6F, 0xD6, 0x6F),
    .lv_warn    = RGB(0xF2, 0xC9, 0x4C),
    .lv_fail    = RGB(0xFF, 0x6B, 0x6B)
};

static const ThemeColors C_LIGHT = {
    .window     = RGB(0xF3, 0xF3, 0xF3),
    .text       = RGB(0x1A, 0x1A, 0x1A),
    .dim        = RGB(0x5A, 0x5A, 0x5A),
    .edit_bg    = RGB(0xFF, 0xFF, 0xFF),
    .edit_text  = RGB(0x11, 0x11, 0x11),
    .border     = RGB(0xCF, 0xCF, 0xCF),
    .btn_face   = RGB(0xFD, 0xFD, 0xFD),
    .btn_hot    = RGB(0xF5, 0xF5, 0xF5),
    .btn_down   = RGB(0xED, 0xED, 0xED),
    .btn_border = RGB(0xD1, 0xD1, 0xD1),
    .btn_text   = RGB(0x1A, 0x1A, 0x1A),
    .dis_face   = RGB(0xF7, 0xF7, 0xF7),
    .dis_text   = RGB(0xA0, 0xA0, 0xA0),
    .dis_border = RGB(0xE2, 0xE2, 0xE2),
    .def_face   = RGB(0x0F, 0x6C, 0xBD),
    .def_hot    = RGB(0x1A, 0x7A, 0xCC),
    .def_down   = RGB(0x37, 0x8C, 0xD4),
    .def_text   = RGB(0xFF, 0xFF, 0xFF),
    .focus      = RGB(0x1A, 0x1A, 0x1A),
    .lv_step    = RGB(0x0A, 0x4D, 0x8C),
    .lv_info    = RGB(0x33, 0x33, 0x33),
    .lv_ok      = RGB(0x11, 0x77, 0x11),
    .lv_warn    = RGB(0x8A, 0x5A, 0x00),
    .lv_fail    = RGB(0xC0, 0x10, 0x10)
};

const ThemeColors *theme_colors(void)
{
    return g_dark ? &C_DARK : &C_LIGHT;
}

/* ------------------------------------------------------------ system read */

BOOL theme_system_is_dark(void)
{
    HKEY  key;
    DWORD val = 1, cb = sizeof(val), type = 0;
    BOOL  dark = FALSE;

    /* AppsUseLightTheme is the value behind Settings > Personalisation >
     * Colours > "Choose your mode". Absent on a stripped install, where light
     * is the right assumption. */
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PERSONALIZE_KEY, 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type,
                         (BYTE *)&val, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD)
        dark = (val == 0);
    RegCloseKey(key);
    return dark;
}

static void resolve(void)
{
    const ThemeColors *c;

    g_dark = (g_mode == THEME_DARK) ||
             (g_mode == THEME_AUTO && g_sys_dark);

    c = theme_colors();
    if (g_win_brush) DeleteObject(g_win_brush);
    g_win_brush = CreateSolidBrush(c->window);
}

/* ------------------------------------------------------------ persistence */

static ThemeMode mode_from_string(const wchar_t *s)
{
    if (_wcsicmp(s, L"dark")  == 0) return THEME_DARK;
    if (_wcsicmp(s, L"light") == 0) return THEME_LIGHT;
    return THEME_AUTO;
}

static const wchar_t *mode_to_string(ThemeMode m)
{
    switch (m) {
    case THEME_DARK:  return L"dark";
    case THEME_LIGHT: return L"light";
    default:          return L"auto";
    }
}

void theme_init(const wchar_t *ini_path)
{
    wchar_t buf[32];

    g_ini[0] = L'\0';
    if (ini_path && ini_path[0]) {
        wcsncpy(g_ini, ini_path, NVDK_MAX_PATH - 1);
        g_ini[NVDK_MAX_PATH - 1] = L'\0';
        GetPrivateProfileStringW(L"ui", L"Theme", L"auto", buf, 32, g_ini);
        g_mode = mode_from_string(buf);
        /* Written back so the file documents the key and its spelling, the
         * same way cfg_load does for the paths section. */
        WritePrivateProfileStringW(L"ui", L"Theme", mode_to_string(g_mode), g_ini);
    }

    g_sys_dark = theme_system_is_dark();
    resolve();
}

ThemeMode theme_mode(void)         { return g_mode; }
BOOL      theme_is_dark(void)      { return g_dark; }
HBRUSH    theme_window_brush(void) { return g_win_brush; }

void theme_set_mode(ThemeMode m)
{
    g_mode = m;
    if (g_ini[0])
        WritePrivateProfileStringW(L"ui", L"Theme", mode_to_string(m), g_ini);
    g_sys_dark = theme_system_is_dark();
    resolve();
}

ThemeMode theme_cycle(void)
{
    ThemeMode next;

    switch (g_mode) {
    case THEME_AUTO:  next = THEME_LIGHT; break;
    case THEME_LIGHT: next = THEME_DARK;  break;
    default:          next = THEME_AUTO;  break;
    }
    theme_set_mode(next);
    return next;
}

/* TRUE when the resolved theme actually flipped, so a WM_SETTINGCHANGE for
 * something unrelated does not cost a full repaint. */
BOOL theme_refresh(void)
{
    BOOL was = g_dark;

    g_sys_dark = theme_system_is_dark();
    resolve();
    return was != g_dark;
}

const wchar_t *theme_label(void)
{
    switch (g_mode) {
    case THEME_DARK:  return L"Theme: Dark";
    case THEME_LIGHT: return L"Theme: Light";
    default:          return g_sys_dark ? L"Theme: Auto (dark)"
                                        : L"Theme: Auto (light)";
    }
}

COLORREF theme_level_color(LogLevel lvl)
{
    const ThemeColors *c = theme_colors();

    switch (lvl) {
    case LG_STEP: return c->lv_step;
    case LG_OK:   return c->lv_ok;
    case LG_WARN: return c->lv_warn;
    case LG_FAIL: return c->lv_fail;
    default:      return c->lv_info;
    }
}

/* --------------------------------------------------------------- titlebar */

void theme_apply_titlebar(HWND hwnd)
{
    BOOL on = g_dark ? TRUE : FALSE;

    /* Attribute 20 from 20H1 onward, 19 on 1809 and 1903. Passing the wrong
     * one is a failed HRESULT and nothing else, so try the current value and
     * fall back rather than sniffing the build number. */
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                     &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, NVDK_DWMWA_DARK_PRE_20H1,
                              &on, sizeof(on));

    /* The caption sits outside the client area, so it repaints on a frame
     * change and not on InvalidateRect. */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_DRAWFRAME);
}

/* ---------------------------------------------------------------- buttons */

void theme_draw_button(const DRAWITEMSTRUCT *dis, BOOL hot, BOOL is_default)
{
    const ThemeColors *c = theme_colors();
    wchar_t text[128];
    RECT r = dis->rcItem;
    BOOL down     = (dis->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
    BOOL focused  = (dis->itemState & ODS_FOCUS) != 0;
    COLORREF face, border, fg;
    HBRUSH   br;
    HPEN     pen;
    HGDIOBJ  old_br, old_pen;
    int      old_mode;

    /* is_default is passed in rather than read from the style: BS_OWNERDRAW
     * (11) and BS_DEFPUSHBUTTON (1) share the low four bits of the style word
     * (BS_TYPEMASK), so once a button is owner-drawn the style can no longer
     * answer the question. */
    if (disabled) {
        face   = c->dis_face;
        border = c->dis_border;
        fg     = c->dis_text;
    } else if (is_default) {
        face   = down ? c->def_down : (hot ? c->def_hot : c->def_face);
        border = c->def_face;
        fg     = c->def_text;
    } else {
        face   = down ? c->btn_down : (hot ? c->btn_hot : c->btn_face);
        border = c->btn_border;
        fg     = c->btn_text;
    }

    /* The DC arrives holding the button's own erase, not the parent's, so the
     * pixels outside the rounded corners have to be laid down here or the
     * corners keep the system button colour. */
    FillRect(dis->hDC, &r, g_win_brush);

    br  = CreateSolidBrush(face);
    pen = CreatePen(PS_SOLID, 1, border);
    old_br  = SelectObject(dis->hDC, br);
    old_pen = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, r.left, r.top, r.right, r.bottom, 7, 7);
    SelectObject(dis->hDC, old_br);
    SelectObject(dis->hDC, old_pen);
    DeleteObject(br);
    DeleteObject(pen);

    if (focused && !disabled) {
        RECT f = r;
        InflateRect(&f, -3, -3);
        pen = CreatePen(PS_SOLID, 1, is_default ? c->def_text : c->focus);
        old_pen = SelectObject(dis->hDC, pen);
        old_br  = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        RoundRect(dis->hDC, f.left, f.top, f.right, f.bottom, 5, 5);
        SelectObject(dis->hDC, old_pen);
        SelectObject(dis->hDC, old_br);
        DeleteObject(pen);
    }

    text[0] = L'\0';
    GetWindowTextW(dis->hwndItem, text, 128);
    old_mode = SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    DrawTextW(dis->hDC, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetBkMode(dis->hDC, old_mode);
}

/* A control's scrollbars are drawn by the control, not from any colour this
 * module hands out, so they stay in the light system scheme however dark the
 * rest of the window is. See the concession at the top of the file. */
void theme_apply_scrollbars(HWND ctl)
{
    if (!ctl) return;
    /* NULL restores default theming, which is what light mode wants. */
    SetWindowTheme(ctl, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
}

/* One pixel outline around a child, drawn by the parent. The log carries no
 * WS_EX_CLIENTEDGE: that edge is drawn from system colours this module does
 * not own, so it stays light grey however dark the rest of the window is. */
void theme_frame_control(HDC dc, HWND ctl, HWND parent)
{
    const ThemeColors *c = theme_colors();
    RECT r;
    HPEN pen;
    HGDIOBJ old_pen, old_br;

    if (!ctl || !GetWindowRect(ctl, &r)) return;
    MapWindowPoints(NULL, parent, (POINT *)&r, 2);
    InflateRect(&r, 1, 1);

    pen = CreatePen(PS_SOLID, 1, c->border);
    old_pen = SelectObject(dc, pen);
    old_br  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
    DeleteObject(pen);
}
