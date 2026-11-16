/*
 * wine-atspi bridge, Win32 core: tab control (SysTabControl32) tabs.
 */
#include "core.h"

/* TCITEMW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct tcitem32
{
    UINT mask;
    DWORD dwState;
    DWORD dwStateMask;
    ULONG pszText;
    int cchTextMax;
};

/*
 * The focused tab of a tab control, -1 if none; LIST_BUSY if it did not
 * answer. Without TCS_BUTTONS it is the selected tab as well.
 */
int tab_focus(HWND hwnd)
{
    DWORD_PTR item = (DWORD_PTR)-1;

    if (!SendMessageTimeoutW(hwnd, TCM_GETCURFOCUS, 0, 0, SMTO_ABORTIFHUNG, 500, &item))
        return LIST_BUSY;
    return (int)item;
}

/*
 * Wine's oleacc has no proxy for SysTabControl32, and arrow keys emit only
 * EVENT_OBJECT_SELECTION (comctl32/tab.c, TAB_SetCurFocus; a focus event
 * only with TCS_BUTTONS), so the tab is read with tab control messages.
 * The MSAA child id is item + 1, as on Windows. FALSE if the text could
 * not be read. r is memory in the target.
 */
BOOL describe_tab_item_in(HWND hwnd, struct remote *r, int item, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR selected = (DWORD_PTR)-1, count = 0;
    RECT rect;
    BOOL ok;

    memset(header, 0, sizeof(header));
    if (r->wow64)
    {
        struct tcitem32 *tc = (struct tcitem32 *)header;

        tc->mask = TCIF_TEXT;
        tc->pszText = (ULONG)(ULONG_PTR)(r->base + REMOTE_HEADER);
        tc->cchTextMax = CELL_TEXT_MAX;
    }
    else
    {
        TCITEMW *tc = (TCITEMW *)header;

        tc->mask = TCIF_TEXT;
        tc->pszText = (WCHAR *)(r->base + REMOTE_HEADER);
        tc->cchTextMax = CELL_TEXT_MAX;
    }
    ok = remote_text(hwnd, r, TCM_GETITEMW, item, header, text);
    SetRectEmpty(&rect);
    obj->has_extents = FALSE;
    if (ok && remote_rect(hwnd, r, TCM_GETITEMRECT, item, &rect))
        set_item_extents(hwnd, rect, obj);
    if (!ok)
        return FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = BRIDGE_ROLE_PAGE_TAB;
    obj->value = NULL;
    to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (SendMessageTimeoutW(hwnd, TCM_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG, 500, &selected) &&
        (int)selected == item)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count);
    obj->position = item + 1;
    obj->set_size = (int)count;
    return TRUE;
}

BOOL describe_tab_item(HWND hwnd, int item, struct bridge_object *obj,
                              struct focus_strings *str)
{
    struct remote r;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    ok = describe_tab_item_in(hwnd, &r, item, obj, str);
    remote_close(&r);
    return ok;
}
