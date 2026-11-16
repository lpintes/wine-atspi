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
static int tab_focus_index(HWND hwnd)
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
static BOOL describe_tab_item_in(HWND hwnd, struct remote *r, int item,
                                 struct bridge_object *obj, struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR selected = (DWORD_PTR)-1, count = 0;
    RECT rect;
    BOOL ok;

    memset(header, 0, sizeof(header));
    REMOTE_STRUCT(r, header, TCITEMW, struct tcitem32, tc,
                  tc->mask = TCIF_TEXT;
                  REMOTE_PTR(tc->pszText, REMOTE_TEXT(r));
                  tc->cchTextMax = CELL_TEXT_MAX;);
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

static BOOL describe_tab_item(HWND hwnd, int item, struct bridge_object *obj,
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

static int tab_describe_focus(HWND hwnd, LONG id_child, struct bridge_object *obj,
                              struct focus_strings *str)
{
    int item = id_child > 0 ? id_child - 1 : tab_focus_index(hwnd);

    if (item == LIST_BUSY)
        return LIST_BUSY;
    if (item < 0)
        return FALSE;
    return describe_tab_item(hwnd, item, obj, str) ? TRUE : LIST_BUSY;
}

/* Arrow keys emit only EVENT_OBJECT_SELECTION, which is not hooked. */
static LONG tab_focus(HWND hwnd)
{
    int item = tab_focus_index(hwnd);

    return item == LIST_BUSY ? LIST_BUSY : item < 0 ? 0 : item + 1;
}

static void tab_describe_self(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    DWORD_PTR count = 0;

    obj->role = BRIDGE_ROLE_PAGE_TAB_LIST;
    if (SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) &&
        (LONG)count >= 0)
        obj->set_size = (int)count;
}

static void add_tab_items(struct tree_build *b, HWND hwnd)
{
    struct bridge_object *obj;
    DWORD_PTR count = 0;
    struct remote r;
    int i;

    if (!remote_open(hwnd, &r))
        return;
    if (!SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
        count = 0;
    for (i = 0; i < (int)count && (obj = tree_item_slot(b)); i++)
        tree_item_done(b, describe_tab_item_in(hwnd, &r, i, obj, b->str));
    remote_close(&r);
}

const struct item_handler tab_handler =
{
    tab_describe_focus,
    tab_focus,
    tab_describe_self,
    add_tab_items,
    FALSE,
    TRUE,
};
