/* UI Automation helpers.
 *
 * Every GUID is defined locally rather than pulled from libuuid, so this
 * links identically under MinGW and MSVC with no extra import library.
 */
#include "common.h"
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <stdio.h>

static const GUID k_CLSID_CUIAutomation =
    {0xff48dba4,0x60ef,0x4201,{0xaa,0x87,0x54,0x10,0x3e,0xef,0x59,0x4e}};
static const GUID k_IID_IUIAutomation =
    {0x30cbe57d,0xd9d0,0x452a,{0xab,0x13,0x7a,0xc5,0xac,0x48,0x25,0xee}};
static const GUID k_IID_InvokePattern =
    {0xfb377fbe,0x8ea6,0x46d5,{0x9c,0x73,0x64,0x99,0x64,0x2d,0x30,0x59}};
static const GUID k_IID_TogglePattern =
    {0x94cf8058,0x9b8d,0x4ab9,{0x8b,0xfd,0x4c,0xd0,0xa3,0x3c,0x8c,0x70}};
static const GUID k_IID_ValuePattern =
    {0xa94cd8b1,0x0844,0x4cd6,{0x9d,0x2d,0x64,0x05,0x37,0xab,0x39,0xe9}};
static const GUID k_IID_SelectionItemPattern =
    {0xa8efa66a,0x0fda,0x421a,{0x91,0x94,0x38,0x02,0x1f,0x35,0x78,0xea}};
static const GUID k_IID_ScrollItemPattern =
    {0xb488300f,0xd015,0x4f19,{0x9c,0x29,0xbb,0x59,0x5e,0x36,0x45,0xef}};
static const GUID k_IID_LegacyAccPattern =
    {0x828055ad,0x355b,0x4435,{0x88,0x90,0x29,0xa1,0xd2,0xb2,0x8a,0x1e}};
static const GUID k_IID_SelectionPattern =
    {0x5ed5202e,0xb2ac,0x4c90,{0xb4,0x58,0x7e,0xc6,0xb7,0xa9,0xaf,0x41}};
static const GUID k_IID_ExpandCollapsePattern =
    {0x619be086,0x1f4e,0x4ee4,{0xba,0xfa,0x21,0x01,0x28,0x73,0x87,0x30}};

#define PID_NAME          30005
#define PID_CONTROL_TYPE  30003
#define PID_AUTOMATION_ID 30011
#define PID_IS_ENABLED    30010

#define PAT_INVOKE        10000
#define PAT_SELECTION     10001
#define PAT_VALUE         10002
#define PAT_EXPANDCOLLAPSE 10011
#define PAT_SELECTIONITEM 10010
#define PAT_TOGGLE        10015
#define PAT_SCROLLITEM    10017
#define PAT_LEGACYACC     10018

#define SCOPE_DESCENDANTS 4

struct UiaCtx { IUIAutomation *a; BOOL com_owned; };
struct UiaEl  { IUIAutomationElement *e; };

/* ------------------------------------------------------------ lifecycle */

BOOL uia_init(UiaCtx **out)
{
    UiaCtx *c;
    HRESULT hr;

    *out = NULL;
    c = (UiaCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UiaCtx));
    if (!c) return FALSE;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    c->com_owned = SUCCEEDED(hr);

    hr = CoCreateInstance(&k_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &k_IID_IUIAutomation, (void **)&c->a);
    if (FAILED(hr) || !c->a) {
        log_write(LG_FAIL, L"CoCreateInstance(CUIAutomation) failed: 0x%08lX",
                  (unsigned long)hr);
        if (c->com_owned) CoUninitialize();
        HeapFree(GetProcessHeap(), 0, c);
        return FALSE;
    }
    *out = c;
    return TRUE;
}

void uia_shutdown(UiaCtx *ctx)
{
    if (!ctx) return;
    if (ctx->a) IUIAutomation_Release(ctx->a);
    if (ctx->com_owned) CoUninitialize();
    HeapFree(GetProcessHeap(), 0, ctx);
}

static UiaEl *wrap(IUIAutomationElement *e)
{
    UiaEl *w;
    if (!e) return NULL;
    w = (UiaEl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UiaEl));
    if (!w) { IUIAutomationElement_Release(e); return NULL; }
    w->e = e;
    return w;
}

void uia_release(UiaEl *el)
{
    if (!el) return;
    if (el->e) IUIAutomationElement_Release(el->e);
    HeapFree(GetProcessHeap(), 0, el);
}

UiaEl *uia_from_hwnd(UiaCtx *ctx, HWND hwnd)
{
    IUIAutomationElement *e = NULL;
    if (FAILED(IUIAutomation_ElementFromHandle(ctx->a, (UIA_HWND)hwnd, &e)))
        return NULL;
    return wrap(e);
}

/* ------------------------------------------------------------- searching */

/* Build "name == X" optionally AND "controltype == T". Caller releases. */
static IUIAutomationCondition *name_cond(UiaCtx *ctx, const wchar_t *name,
                                         int ctrl_type)
{
    IUIAutomationCondition *cn = NULL, *ct = NULL, *both = NULL;
    VARIANT v;

    VariantInit(&v);
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(name);
    if (!V_BSTR(&v)) return NULL;

    if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_NAME, v, &cn))) {
        VariantClear(&v);
        return NULL;
    }
    VariantClear(&v);

    if (ctrl_type == CT_ANY)
        return cn;

    VariantInit(&v);
    V_VT(&v) = VT_I4;
    V_I4(&v) = ctrl_type;
    if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_CONTROL_TYPE, v, &ct))) {
        VariantClear(&v);
        IUIAutomationCondition_Release(cn);
        return NULL;
    }
    VariantClear(&v);

    if (FAILED(IUIAutomation_CreateAndCondition(ctx->a, cn, ct, &both))) {
        IUIAutomationCondition_Release(cn);
        IUIAutomationCondition_Release(ct);
        return NULL;
    }
    IUIAutomationCondition_Release(cn);
    IUIAutomationCondition_Release(ct);
    return both;
}

UiaEl *uia_find_by_name(UiaCtx *ctx, UiaEl *root, const wchar_t *name, int ctrl_type)
{
    IUIAutomationCondition *cond;
    IUIAutomationElement   *found = NULL;

    if (!ctx || !root) return NULL;
    cond = name_cond(ctx, name, ctrl_type);
    if (!cond) return NULL;

    IUIAutomationElement_FindFirst(root->e, SCOPE_DESCENDANTS, cond, &found);
    IUIAutomationCondition_Release(cond);
    return wrap(found);
}

/* Shared by the prefix and substring searches. */
static UiaEl *find_matching(UiaCtx *ctx, UiaEl *root, const wchar_t *text,
                            int ctrl_type, BOOL prefix_only)
{
    IUIAutomationCondition  *cond = NULL;
    IUIAutomationElementArray *arr = NULL;
    IUIAutomationElement    *hit = NULL;
    VARIANT v;
    int n = 0, i;
    size_t plen = wcslen(text);

    if (!ctx || !root) return NULL;

    if (ctrl_type == CT_ANY) {
        if (FAILED(IUIAutomation_CreateTrueCondition(ctx->a, &cond)))
            return NULL;
    } else {
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = ctrl_type;
        if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_CONTROL_TYPE, v, &cond))) {
            VariantClear(&v);
            return NULL;
        }
        VariantClear(&v);
    }

    if (FAILED(IUIAutomationElement_FindAll(root->e, SCOPE_DESCENDANTS, cond, &arr)) || !arr) {
        IUIAutomationCondition_Release(cond);
        return NULL;
    }
    IUIAutomationCondition_Release(cond);
    IUIAutomationElementArray_get_Length(arr, &n);

    for (i = 0; i < n; i++) {
        IUIAutomationElement *e = NULL;
        BSTR nm = NULL;
        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e)
            continue;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentName(e, &nm)) && nm) {
            BOOL match = prefix_only ? (wcsncmp(nm, text, plen) == 0)
                                     : (wcsstr(nm, text) != NULL);
            SysFreeString(nm);
            if (match) { hit = e; break; }
        }
        IUIAutomationElement_Release(e);
    }

    IUIAutomationElementArray_Release(arr);
    return wrap(hit);
}

UiaEl *uia_find_by_prefix(UiaCtx *ctx, UiaEl *root, const wchar_t *prefix, int ctrl_type)
{
    return find_matching(ctx, root, prefix, ctrl_type, TRUE);
}

/* Match on AutomationId. NVCleanstall is WPF and gives every control a stable
 * id, so this survives label wording and localisation in a way name matching
 * does not. Preferred everywhere an id is known. */
UiaEl *uia_find_by_id(UiaCtx *ctx, UiaEl *root, const wchar_t *autoid)
{
    IUIAutomationCondition *cond = NULL;
    IUIAutomationElement   *found = NULL;
    VARIANT v;

    if (!ctx || !root) return NULL;

    VariantInit(&v);
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = SysAllocString(autoid);
    if (!V_BSTR(&v)) return NULL;

    if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_AUTOMATION_ID, v, &cond))) {
        VariantClear(&v);
        return NULL;
    }
    VariantClear(&v);

    IUIAutomationElement_FindFirst(root->e, SCOPE_DESCENDANTS, cond, &found);
    IUIAutomationCondition_Release(cond);
    return wrap(found);
}

UiaEl *uia_wait_for_id(UiaCtx *ctx, UiaEl *root, const wchar_t *autoid, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        UiaEl *el = uia_find_by_id(ctx, root, autoid);
        if (el) return el;
        if (GetTickCount() - start >= timeout_ms) return NULL;
        Sleep(200);
    }
}

/* Wait until the element's Name stops being empty. The best-driver label
 * starts blank and fills in only after the online version lookup returns. */
BOOL uia_wait_nonempty_name(UiaEl *el, wchar_t *out, size_t cch, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        out[0] = L'\0';
        if (uia_get_name(el, out, cch) && out[0] != L'\0')
            return TRUE;
        if (GetTickCount() - start >= timeout_ms) return FALSE;
        Sleep(300);
    }
}

UiaEl *uia_find_by_substring(UiaCtx *ctx, UiaEl *root, const wchar_t *needle, int ctrl_type)
{
    return find_matching(ctx, root, needle, ctrl_type, FALSE);
}

/* Count checked checkboxes under root, collecting their names.
 * Page 2 is verified with this: exactly one box may be ticked. */
int uia_count_checked(UiaCtx *ctx, UiaEl *root, wchar_t *names_out, size_t cch)
{
    IUIAutomationCondition *cond = NULL;
    IUIAutomationElementArray *arr = NULL;
    VARIANT v;
    int n = 0, i, checked = 0;

    if (names_out && cch) names_out[0] = L'\0';
    if (!ctx || !root) return -1;

    VariantInit(&v);
    V_VT(&v) = VT_I4;
    V_I4(&v) = CT_CHECKBOX;
    if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_CONTROL_TYPE, v, &cond))) {
        VariantClear(&v);
        return -1;
    }
    VariantClear(&v);

    if (FAILED(IUIAutomationElement_FindAll(root->e, SCOPE_DESCENDANTS, cond, &arr)) || !arr) {
        IUIAutomationCondition_Release(cond);
        return -1;
    }
    IUIAutomationCondition_Release(cond);
    IUIAutomationElementArray_get_Length(arr, &n);

    for (i = 0; i < n; i++) {
        IUIAutomationElement *e = NULL;
        IUIAutomationTogglePattern *p = NULL;
        enum ToggleState st;

        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e)
            continue;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(e, PAT_TOGGLE,
                &k_IID_TogglePattern, (void **)&p)) && p) {
            if (SUCCEEDED(IUIAutomationTogglePattern_get_CurrentToggleState(p, &st))
                    && st != ToggleState_Off) {
                BSTR nm = NULL;
                checked++;
                if (names_out && cch &&
                    SUCCEEDED(IUIAutomationElement_get_CurrentName(e, &nm)) && nm) {
                    size_t len = wcslen(names_out);
                    if (len + wcslen(nm) + 3 < cch)
                        _snwprintf(names_out + len, cch - len, L"%s%s",
                                   len ? L", " : L"", nm);
                    SysFreeString(nm);
                }
            }
            IUIAutomationTogglePattern_Release(p);
        }
        IUIAutomationElement_Release(e);
    }
    IUIAutomationElementArray_Release(arr);
    return checked;
}

UiaEl *uia_wait_for_name(UiaCtx *ctx, UiaEl *root, const wchar_t *name,
                         int ctrl_type, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        UiaEl *el = uia_find_by_name(ctx, root, name, ctrl_type);
        if (el) return el;
        if (GetTickCount() - start >= timeout_ms) return NULL;
        Sleep(200);
    }
}

/* ------------------------------------------------------------- properties */

BOOL uia_get_name(UiaEl *el, wchar_t *out, size_t cch)
{
    BSTR nm = NULL;
    if (!el || FAILED(IUIAutomationElement_get_CurrentName(el->e, &nm)) || !nm)
        return FALSE;
    wcsncpy(out, nm, cch - 1);
    out[cch - 1] = L'\0';
    SysFreeString(nm);
    return TRUE;
}

BOOL uia_is_enabled(UiaEl *el, BOOL *out)
{
    BOOL b = FALSE;
    if (!el || FAILED(IUIAutomationElement_get_CurrentIsEnabled(el->e, &b)))
        return FALSE;
    *out = b;
    return TRUE;
}

/* ---------------------------------------------------------------- patterns */

BOOL uia_get_toggle(UiaEl *el, int *state)
{
    IUIAutomationTogglePattern *p = NULL;
    enum ToggleState st;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_TOGGLE,
            &k_IID_TogglePattern, (void **)&p)) || !p)
        return FALSE;
    if (FAILED(IUIAutomationTogglePattern_get_CurrentToggleState(p, &st))) {
        IUIAutomationTogglePattern_Release(p);
        return FALSE;
    }
    *state = (int)st;
    IUIAutomationTogglePattern_Release(p);
    return TRUE;
}

BOOL uia_set_toggle(UiaEl *el, int want)
{
    IUIAutomationTogglePattern *p = NULL;
    int cur = -1, guard = 0;

    if (!uia_get_toggle(el, &cur)) return FALSE;
    if (cur == want) return TRUE;

    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_TOGGLE,
            &k_IID_TogglePattern, (void **)&p)) || !p)
        return FALSE;

    /* Toggle cycles; at most three steps covers the tri-state case. */
    while (guard++ < 3) {
        if (FAILED(IUIAutomationTogglePattern_Toggle(p))) break;
        Sleep(120);
        if (uia_get_toggle(el, &cur) && cur == want) {
            IUIAutomationTogglePattern_Release(p);
            return TRUE;
        }
    }
    IUIAutomationTogglePattern_Release(p);
    return FALSE;
}

BOOL uia_is_selected(UiaEl *el, BOOL *out)
{
    IUIAutomationSelectionItemPattern *p = NULL;
    BOOL b = FALSE;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_SELECTIONITEM,
            &k_IID_SelectionItemPattern, (void **)&p)) || !p)
        return FALSE;
    if (FAILED(IUIAutomationSelectionItemPattern_get_CurrentIsSelected(p, &b))) {
        IUIAutomationSelectionItemPattern_Release(p);
        return FALSE;
    }
    *out = b;
    IUIAutomationSelectionItemPattern_Release(p);
    return TRUE;
}

BOOL uia_select(UiaEl *el)
{
    IUIAutomationSelectionItemPattern *p = NULL;
    HRESULT hr;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_SELECTIONITEM,
            &k_IID_SelectionItemPattern, (void **)&p)) || !p)
        return FALSE;
    hr = IUIAutomationSelectionItemPattern_Select(p);
    IUIAutomationSelectionItemPattern_Release(p);
    return SUCCEEDED(hr);
}

BOOL uia_invoke(UiaEl *el)
{
    IUIAutomationInvokePattern *inv = NULL;
    IUIAutomationLegacyIAccessiblePattern *leg = NULL;
    HRESULT hr;

    if (!el) return FALSE;

    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_INVOKE,
            &k_IID_InvokePattern, (void **)&inv)) && inv) {
        hr = IUIAutomationInvokePattern_Invoke(inv);
        IUIAutomationInvokePattern_Release(inv);
        if (SUCCEEDED(hr)) return TRUE;
    }

    /* WinForms LinkLabels (the All / Recommended / Minimum links) often expose
     * no Invoke pattern; fall back to the accessibility default action. */
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_LEGACYACC,
            &k_IID_LegacyAccPattern, (void **)&leg)) && leg) {
        hr = IUIAutomationLegacyIAccessiblePattern_DoDefaultAction(leg);
        IUIAutomationLegacyIAccessiblePattern_Release(leg);
        return SUCCEEDED(hr);
    }
    return FALSE;
}

BOOL uia_get_value(UiaEl *el, wchar_t *out, size_t cch)
{
    IUIAutomationValuePattern *p = NULL;
    BSTR v = NULL;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_VALUE,
            &k_IID_ValuePattern, (void **)&p)) || !p)
        return FALSE;
    if (FAILED(IUIAutomationValuePattern_get_CurrentValue(p, &v)) || !v) {
        IUIAutomationValuePattern_Release(p);
        return FALSE;
    }
    wcsncpy(out, v, cch - 1);
    out[cch - 1] = L'\0';
    SysFreeString(v);
    IUIAutomationValuePattern_Release(p);
    return TRUE;
}

BOOL uia_set_value(UiaEl *el, const wchar_t *text)
{
    IUIAutomationValuePattern *p = NULL;
    BSTR b;
    HRESULT hr;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_VALUE,
            &k_IID_ValuePattern, (void **)&p)) || !p)
        return FALSE;
    b = SysAllocString(text);
    if (!b) { IUIAutomationValuePattern_Release(p); return FALSE; }
    hr = IUIAutomationValuePattern_SetValue(p, b);
    SysFreeString(b);
    IUIAutomationValuePattern_Release(p);
    return SUCCEEDED(hr);
}

BOOL uia_scroll_into_view(UiaEl *el)
{
    IUIAutomationScrollItemPattern *p = NULL;
    HRESULT hr;

    if (!el) return FALSE;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_SCROLLITEM,
            &k_IID_ScrollItemPattern, (void **)&p)) || !p)
        return FALSE;
    hr = IUIAutomationScrollItemPattern_ScrollIntoView(p);
    IUIAutomationScrollItemPattern_Release(p);
    return SUCCEEDED(hr);
}

static IUIAutomationCondition *type_cond(UiaCtx *ctx, int t);

/* ------------------------------------------------------------ combo boxes
 *
 * WPF ComboBoxes expose no ValuePattern, so reading one means asking its
 * SelectionPattern what is selected, and setting one means expanding it and
 * selecting the matching ListItem.
 */

BOOL uia_get_combo_selection(UiaEl *el, wchar_t *out, size_t cch)
{
    IUIAutomationSelectionPattern *p = NULL;
    IUIAutomationElementArray *arr = NULL;
    IUIAutomationElement *item = NULL;
    BSTR nm = NULL;
    int n = 0;
    BOOL ok = FALSE;

    if (out && cch) out[0] = L'\0';
    if (!el) return FALSE;

    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_SELECTION,
            &k_IID_SelectionPattern, (void **)&p)) || !p)
        return FALSE;

    if (SUCCEEDED(IUIAutomationSelectionPattern_GetCurrentSelection(p, &arr)) && arr) {
        IUIAutomationElementArray_get_Length(arr, &n);
        if (n > 0 && SUCCEEDED(IUIAutomationElementArray_GetElement(arr, 0, &item)) && item) {
            if (SUCCEEDED(IUIAutomationElement_get_CurrentName(item, &nm)) && nm) {
                wcsncpy(out, nm, cch - 1);
                out[cch - 1] = L'\0';
                SysFreeString(nm);
                ok = TRUE;
            }
            IUIAutomationElement_Release(item);
        }
        IUIAutomationElementArray_Release(arr);
    }
    IUIAutomationSelectionPattern_Release(p);
    if (ok) return TRUE;

    /* A collapsed WPF ComboBox often reports no selection through
     * SelectionPattern because its items are virtualised. LegacyIAccessible
     * still carries the displayed text. */
    {
        IUIAutomationLegacyIAccessiblePattern *leg = NULL;
        BSTR v = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el->e, PAT_LEGACYACC,
                &k_IID_LegacyAccPattern, (void **)&leg)) && leg) {
            if (SUCCEEDED(IUIAutomationLegacyIAccessiblePattern_get_CurrentValue(leg, &v)) && v) {
                if (v[0]) {
                    wcsncpy(out, v, cch - 1);
                    out[cch - 1] = L'\0';
                    ok = TRUE;
                }
                SysFreeString(v);
            }
            IUIAutomationLegacyIAccessiblePattern_Release(leg);
        }
    }
    return ok;
}

/* Look for a ListItem named text anywhere under scope and select it.
 * seen is incremented for every ListItem inspected. */
static BOOL pick_item_under(UiaCtx *ctx, IUIAutomationElement *scope,
                            const wchar_t *text, int *seen)
{
    IUIAutomationCondition *cond = type_cond(ctx, CT_LISTITEM);
    IUIAutomationElementArray *arr = NULL;
    int n = 0, i;
    BOOL hit = FALSE;

    if (!cond) return FALSE;
    if (FAILED(IUIAutomationElement_FindAll(scope, SCOPE_DESCENDANTS, cond, &arr)) || !arr) {
        IUIAutomationCondition_Release(cond);
        return FALSE;
    }
    IUIAutomationElementArray_get_Length(arr, &n);
    *seen += n;

    for (i = 0; i < n && !hit; i++) {
        IUIAutomationElement *item = NULL;
        IUIAutomationSelectionItemPattern *sel = NULL;
        BSTR nm = NULL;

        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &item)) || !item)
            continue;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentName(item, &nm)) && nm) {
            if (wcscmp(nm, text) == 0 &&
                SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(item, PAT_SELECTIONITEM,
                    &k_IID_SelectionItemPattern, (void **)&sel)) && sel) {
                hit = SUCCEEDED(IUIAutomationSelectionItemPattern_Select(sel));
                IUIAutomationSelectionItemPattern_Release(sel);
            }
            SysFreeString(nm);
        }
        IUIAutomationElement_Release(item);
    }

    IUIAutomationElementArray_Release(arr);
    IUIAutomationCondition_Release(cond);
    return hit;
}

/* Expand the combo and select the item whose name matches.
 *
 * A WPF dropdown is rendered in a popup that is NOT a child of the ComboBox
 * in the UIA tree, and its items are virtualised until it opens - which is
 * why searching under the combo alone finds nothing. Open it first, then look
 * under the combo and, failing that, under the whole window. */
BOOL uia_set_focus(UiaEl *el)
{
    if (!el) return FALSE;
    return SUCCEEDED(IUIAutomationElement_SetFocus(el->e));
}

/* Wait for a control to become enabled. The interrupt combos are greyed out
 * until "Enable Message Signaled Interrupts" is ticked, and a disabled WPF
 * ComboBox exposes no patterns at all - which is exactly how the first real
 * run failed: ExpandCollapse was missing and no items existed. */
BOOL uia_wait_enabled(UiaEl *el, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    BOOL on = FALSE;

    for (;;) {
        if (uia_is_enabled(el, &on) && on) return TRUE;
        if (GetTickCount() - start >= timeout_ms) return FALSE;
        Sleep(200);
    }
}

/* Type-ahead: focus the combo and send the first letter, which selects the
 * first matching entry. Used when the dropdown cannot be opened. */
static BOOL combo_type_ahead(UiaEl *combo, const wchar_t *text)
{
    INPUT in[2];
    SHORT vk;

    if (!uia_set_focus(combo)) return FALSE;
    Sleep(200);

    vk = VkKeyScanW(text[0]);
    if (vk == -1) return FALSE;

    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = (WORD)(vk & 0xFF);
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
    Sleep(300);
    return TRUE;
}

BOOL uia_select_combo_item(UiaCtx *ctx, UiaEl *combo, UiaEl *win_root,
                           const wchar_t *text)
{
    IUIAutomationExpandCollapsePattern *ec = NULL;
    BOOL hit = FALSE, enabled;
    int seen = 0;

    if (!ctx || !combo) return FALSE;

    enabled = uia_wait_enabled(combo, 5000);
    if (!enabled) {
        log_write(LG_WARN, L"Combo is still disabled after 5 s - skipping it");
        return FALSE;
    }

    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(combo->e, PAT_EXPANDCOLLAPSE,
            &k_IID_ExpandCollapsePattern, (void **)&ec)) && ec) {
        IUIAutomationExpandCollapsePattern_Expand(ec);
        Sleep(500);
    } else {
        log_write(LG_WARN, L"Combo exposes no ExpandCollapse pattern; using type-ahead");
    }

    hit = pick_item_under(ctx, combo->e, text, &seen);
    if (!hit && win_root)
        hit = pick_item_under(ctx, win_root->e, text, &seen);

    if (ec) {
        Sleep(200);
        IUIAutomationExpandCollapsePattern_Collapse(ec);
        IUIAutomationExpandCollapsePattern_Release(ec);
    }

    if (!hit) {
        log_write(LG_WARN, L"No list item named \"%s\" (%d item(s) seen); "
                           L"trying type-ahead", text, seen);
        hit = combo_type_ahead(combo, text);
    }
    return hit;
}

/* Build a "control type == t" condition. Caller releases. */
static IUIAutomationCondition *type_cond(UiaCtx *ctx, int t)
{
    IUIAutomationCondition *c = NULL;
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_I4;
    V_I4(&v) = t;
    if (FAILED(IUIAutomation_CreatePropertyCondition(ctx->a, PID_CONTROL_TYPE, v, &c)))
        c = NULL;
    VariantClear(&v);
    return c;
}

int uia_list_checked_items(UiaCtx *ctx, UiaEl *root, wchar_t *names_out, size_t cch)
{
    IUIAutomationCondition *item_cond, *cb_cond;
    IUIAutomationElementArray *arr = NULL;
    int n = 0, i, checked = 0;

    if (names_out && cch) names_out[0] = L'\0';
    if (!ctx || !root) return -1;

    item_cond = type_cond(ctx, CT_LISTITEM);
    cb_cond   = type_cond(ctx, CT_CHECKBOX);
    if (!item_cond || !cb_cond) {
        if (item_cond) IUIAutomationCondition_Release(item_cond);
        if (cb_cond)   IUIAutomationCondition_Release(cb_cond);
        return -1;
    }

    if (FAILED(IUIAutomationElement_FindAll(root->e, SCOPE_DESCENDANTS, item_cond, &arr)) || !arr) {
        IUIAutomationCondition_Release(item_cond);
        IUIAutomationCondition_Release(cb_cond);
        return -1;
    }
    IUIAutomationElementArray_get_Length(arr, &n);

    for (i = 0; i < n; i++) {
        IUIAutomationElement *item = NULL, *cb = NULL;
        IUIAutomationTogglePattern *p = NULL;
        enum ToggleState st;

        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &item)) || !item)
            continue;

        if (SUCCEEDED(IUIAutomationElement_FindFirst(item, SCOPE_DESCENDANTS, cb_cond, &cb)) && cb) {
            if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(cb, PAT_TOGGLE,
                    &k_IID_TogglePattern, (void **)&p)) && p) {
                if (SUCCEEDED(IUIAutomationTogglePattern_get_CurrentToggleState(p, &st))
                        && st != ToggleState_Off) {
                    BSTR nm = NULL;
                    checked++;
                    /* label comes from the ListItem, not the unnamed box */
                    if (names_out && cch &&
                        SUCCEEDED(IUIAutomationElement_get_CurrentName(item, &nm)) && nm) {
                        size_t len = wcslen(names_out);
                        if (len + wcslen(nm) + 3 < cch)
                            _snwprintf(names_out + len, cch - len, L"%s%s",
                                       len ? L", " : L"", nm);
                        SysFreeString(nm);
                    }
                }
                IUIAutomationTogglePattern_Release(p);
            }
            IUIAutomationElement_Release(cb);
        }
        IUIAutomationElement_Release(item);
    }

    IUIAutomationElementArray_Release(arr);
    IUIAutomationCondition_Release(item_cond);
    IUIAutomationCondition_Release(cb_cond);
    return checked;
}

/* -------------------------------------------------------------- probing */

/* Which control patterns an element actually exposes. Guessing at this is
 * what cost two failed attempts on the interrupt combos. */
static void pattern_summary(IUIAutomationElement *e, wchar_t *out, size_t cch)
{
    static const struct { int id; const GUID *iid; const wchar_t *nm; } pats[] = {
        { PAT_INVOKE,         &k_IID_InvokePattern,         L"Invoke"    },
        { PAT_SELECTION,      &k_IID_SelectionPattern,      L"Selection" },
        { PAT_VALUE,          &k_IID_ValuePattern,          L"Value"     },
        { PAT_SELECTIONITEM,  &k_IID_SelectionItemPattern,  L"SelItem"   },
        { PAT_EXPANDCOLLAPSE, &k_IID_ExpandCollapsePattern, L"Expand"    },
        { PAT_TOGGLE,         &k_IID_TogglePattern,         L"Toggle"    },
        { PAT_SCROLLITEM,     &k_IID_ScrollItemPattern,     L"ScrollItem"},
        { PAT_LEGACYACC,      &k_IID_LegacyAccPattern,      L"Legacy"    },
    };
    size_t i;

    out[0] = L'\0';
    for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        IUnknown *p = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(e, pats[i].id,
                pats[i].iid, (void **)&p)) && p) {
            size_t len = wcslen(out);
            if (len + wcslen(pats[i].nm) + 2 < cch)
                _snwprintf(out + len, cch - len, L"%s%s", len ? L"," : L"", pats[i].nm);
            IUnknown_Release(p);
        }
    }
    if (!out[0]) wcsncpy(out, L"none", cch - 1);
}

/* Describe one element in the log: type, enabled state, exposed patterns and
 * its immediate children. Used when a control will not behave as expected. */
void uia_log_element(UiaCtx *ctx, UiaEl *el, const wchar_t *label)
{
    wchar_t pats[256], nm[256] = L"";
    CONTROLTYPEID ct = 0;
    BOOL on = FALSE;
    IUIAutomationCondition *all = NULL;
    IUIAutomationElementArray *arr = NULL;
    int n = 0, i;

    if (!el) { log_write(LG_INFO, L"%s: (null)", label); return; }

    IUIAutomationElement_get_CurrentControlType(el->e, &ct);
    IUIAutomationElement_get_CurrentIsEnabled(el->e, &on);
    pattern_summary(el->e, pats, 256);
    uia_get_name(el, nm, 256);

    log_write(LG_INFO, L"%s: type=%ld enabled=%s patterns=%s name=\"%s\"",
              label, (long)ct, on ? L"yes" : L"no", pats, nm);

    if (ctx && SUCCEEDED(IUIAutomation_CreateTrueCondition(ctx->a, &all)) && all) {
        if (SUCCEEDED(IUIAutomationElement_FindAll(el->e, SCOPE_DESCENDANTS, all, &arr)) && arr) {
            IUIAutomationElementArray_get_Length(arr, &n);
            log_write(LG_INFO, L"%s: %d descendant(s)", label, n);
            for (i = 0; i < n && i < 12; i++) {
                IUIAutomationElement *c = NULL;
                BSTR cn = NULL;
                CONTROLTYPEID cct = 0;
                if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &c)) || !c)
                    continue;
                IUIAutomationElement_get_CurrentName(c, &cn);
                IUIAutomationElement_get_CurrentControlType(c, &cct);
                pattern_summary(c, pats, 256);
                log_write(LG_INFO, L"    child type=%ld pat=%s name=\"%s\"",
                          (long)cct, pats, cn ? cn : L"");
                if (cn) SysFreeString(cn);
                IUIAutomationElement_Release(c);
            }
            IUIAutomationElementArray_Release(arr);
        }
        IUIAutomationCondition_Release(all);
    }
}

int uia_dump_tree(UiaCtx *ctx, UiaEl *root, int max_depth)
{
    IUIAutomationCondition *cond = NULL;
    IUIAutomationElementArray *arr = NULL;
    int n = 0, i, count = 0;

    (void)max_depth;
    if (!ctx || !root) return 0;
    if (FAILED(IUIAutomation_CreateTrueCondition(ctx->a, &cond)))
        return 0;
    if (FAILED(IUIAutomationElement_FindAll(root->e, SCOPE_DESCENDANTS, cond, &arr)) || !arr) {
        IUIAutomationCondition_Release(cond);
        return 0;
    }
    IUIAutomationCondition_Release(cond);
    IUIAutomationElementArray_get_Length(arr, &n);

    for (i = 0; i < n; i++) {
        IUIAutomationElement *e = NULL;
        BSTR nm = NULL, aid = NULL;
        CONTROLTYPEID ct = 0;

        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &e)) || !e)
            continue;
        IUIAutomationElement_get_CurrentName(e, &nm);
        IUIAutomationElement_get_CurrentAutomationId(e, &aid);
        IUIAutomationElement_get_CurrentControlType(e, &ct);

        {
            wchar_t pats[256];
            BOOL on = FALSE;
            pattern_summary(e, pats, 256);
            IUIAutomationElement_get_CurrentIsEnabled(e, &on);
            log_write(LG_INFO, L"  type=%ld %s id=%-24s pat=%-28s name=\"%s\"",
                      (long)ct, on ? L"en" : L"DIS",
                      (aid && *aid) ? aid : L"-",
                      pats,
                      nm ? nm : L"");
        }
        count++;

        if (nm)  SysFreeString(nm);
        if (aid) SysFreeString(aid);
        IUIAutomationElement_Release(e);
    }
    IUIAutomationElementArray_Release(arr);
    return count;
}
