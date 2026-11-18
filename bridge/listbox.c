/*
 * wine-atspi bridge, Win32 core: list boxes, item controls as containers
 * and their selection.
 */
#include "core.h"

/*
 * Selected rows of a list view in report view, for the table's Selection
 * interface: with it Orca's flat review reads only the visible cells
 * (ax_utilities.py, _get_on_screen_objects), and Where Am I counts the
 * selected rows from the table. A list box gets the same for its
 * Selection, with item indices.
 */
static void describe_list_selection(HWND hwnd, BOOL listview, struct bridge_object *obj,
                                    struct focus_strings *str)
{
    DWORD_PTR count = 0, item = (DWORD_PTR)-1;

    obj->selected_count = obj->selected_listed = 0;
    obj->selected_rows = str->selected_rows;
    if (!listview)
    {
        /*
         * Wine marshals LB_GETSELITEMS; it and LB_GETSELCOUNT fail with
         * LB_ERR on a single-selection list.
         */
        if (!(obj->states & BRIDGE_STATE_BIT(BRIDGE_STATE_MULTISELECTABLE)))
        {
            if (send_message(hwnd, LB_GETCURSEL, 0, 0, SEND_TIMEOUT_MS, &item) &&
                (int)item >= 0)
            {
                str->selected_rows[0] = (int)item;
                obj->selected_count = obj->selected_listed = 1;
            }
            return;
        }
        if (!send_message(hwnd, LB_GETSELCOUNT, 0, 0, SEND_TIMEOUT_MS, &count) ||
            (int)count <= 0 ||
            !send_message(hwnd, LB_GETSELITEMS, MAX_SELECTED_ROWS,
                          (LPARAM)str->selected_rows, SEND_TIMEOUT_MS, &item) ||
            (int)item < 0)
            return;
        obj->selected_count = (int)count;
        obj->selected_listed = (int)item;
        return;
    }
    if (!send_message(hwnd, LVM_GETSELECTEDCOUNT, 0, 0, SEND_TIMEOUT_MS, &count) ||
        (LONG)count <= 0)
        return;
    obj->selected_count = (int)count;
    while (obj->selected_listed < obj->selected_count &&
           obj->selected_listed < MAX_SELECTED_ROWS &&
           send_message(hwnd, LVM_GETNEXTITEM, item, LVNI_SELECTED,
                        SEND_TIMEOUT_MS, &item) && (int)item >= 0)
        str->selected_rows[obj->selected_listed++] = (int)item;
}

/*
 * Role, states and item count of a ListBox or SysListView32 as a whole:
 * focused without a focused item, or the parent of its items. Orca never
 * speaks the role of a focused "list" and says nothing for an unnamed
 * one, and skips a "list" ancestor by default; for a "list box" it says
 * "n items" from setsize, or the role if the list is empty
 * (speech_generator.py). A list view in report view is a table with its
 * column headers instead; set_size then counts the rows.
 */
void describe_list_box(HWND hwnd, BOOL listview, struct bridge_object *obj,
                              struct focus_strings *str)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR count = 0;
    WCHAR text[CELL_TEXT_MAX];
    int order[MAX_COLUMNS], i;
    struct remote r;

    obj->role = BRIDGE_ROLE_LIST_BOX;
    if (listview ? !(style & LVS_SINGLESEL) : (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_MULTISELECTABLE);
    if (send_message(hwnd, listview ? LVM_GETITEMCOUNT : LB_GETCOUNT, 0, 0,
                     SEND_TIMEOUT_MS, &count) && (LONG)count >= 0)
        obj->set_size = (int)count;
    describe_list_selection(hwnd, listview, obj, str);
    if (!listview || !remote_open(hwnd, &r))
        return;
    obj->column_count = listview_columns(hwnd, &r, order);
    for (i = 0; i < obj->column_count; i++)
    {
        if (!get_listview_header(hwnd, &r, order[i], text))
            text[0] = 0;
        to_utf8(text, str->headers[i], sizeof(str->headers[i]));
        str->header_ptrs[i] = str->headers[i];
    }
    remote_close(&r);
    if (obj->column_count)
    {
        obj->role = BRIDGE_ROLE_TABLE;
        obj->columns = str->header_ptrs;
    }
}

/*
 * Wine's oleacc has only a stub for ListBox too: name and role of an item
 * fail with E_INVALIDARG (client.c). LB_GETTEXT is marshalled across
 * processes. An owner-drawn list without LBS_HASSTRINGS keeps no text;
 * its item is left unnamed for an application script (winefile.lua). TRUE if obj was
 * filled, FALSE if the list has no such item, LIST_BUSY if it did not
 * answer.
 */
static int describe_listbox_item(HWND hwnd, int item, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR count = 0, len = 0, sel = 0, ret = 0;
    WCHAR *text;
    RECT rect;

    if (!send_message(hwnd, LB_GETCOUNT, 0, 0, SEND_TIMEOUT_MS, &count))
        return LIST_BUSY;
    if (item < 0 || item >= (LONG)count)
        return FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = BRIDGE_ROLE_LIST_ITEM;
    obj->value = NULL;
    str->object[0] = 0;
    obj->name = str->object;
    if (!(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (style & LBS_HASSTRINGS))
    {
        if (!send_message(hwnd, LB_GETTEXTLEN, item, 0, SEND_TIMEOUT_MS, &len))
            return LIST_BUSY;
        if ((LONG)len > 0 && (text = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR))))
        {
            text[0] = 0;
            if (send_message(hwnd, LB_GETTEXT, item, (LPARAM)text,
                             SEND_TIMEOUT_MS, &len) && (LONG)len >= 0)
            {
                text[len] = 0;
                to_utf8(text, str->object, sizeof(str->object));
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (send_message(hwnd, LB_GETSEL, item, 0, SEND_TIMEOUT_MS, &sel) &&
        (LONG)sel > 0)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    obj->has_extents = FALSE;
    if (send_message(hwnd, LB_GETITEMRECT, item, (LPARAM)&rect,
                     SEND_TIMEOUT_MS, &ret) && (LONG)ret != LB_ERR)
        set_item_extents(hwnd, rect, obj);
    obj->position = item + 1;
    obj->set_size = (int)count;
    script_describe(obj, str);
    return TRUE;
}

/*
 * Wine reports focus_item + 1 even for an empty list (listbox.c,
 * WM_SETFOCUS); then the list itself is described.
 */
static int listbox_describe_focus(HWND hwnd, LONG id_child, struct bridge_object *obj,
                                  struct focus_strings *str)
{
    if (id_child <= 0)
        return FALSE;
    return describe_listbox_item(hwnd, id_child - 1, obj, str);
}

/*
 * Wine's ListBox moves its caret on arrow keys without a focus event
 * (listbox.c, LISTBOX_HandleKeyDown).
 */
static LONG listbox_focus(HWND hwnd)
{
    DWORD_PTR caret = LB_ERR;

    if (!send_message(hwnd, LB_GETCARETINDEX, 0, 0, SEND_TIMEOUT_MS, &caret))
        return LIST_BUSY;
    return (LONG)caret < 0 ? 0 : (LONG)caret + 1;
}

static void listbox_describe_self(HWND hwnd, struct bridge_object *obj,
                                  struct focus_strings *str)
{
    describe_list_box(hwnd, FALSE, obj, str);
    /* winefile.lua makes its panes a tree and a table. */
    script_describe(obj, str);
}

/* Items from the top one down to the first out of view. */
static void add_listbox_items(struct tree_build *b, HWND hwnd)
{
    DWORD_PTR top = 0, count = 0;
    struct bridge_object *obj;
    int i;

    if (!send_message(hwnd, LB_GETTOPINDEX, 0, 0, SEND_TIMEOUT_MS, &top) ||
        !send_message(hwnd, LB_GETCOUNT, 0, 0, SEND_TIMEOUT_MS, &count))
        return;
    for (i = (int)top; i < (int)count && (obj = tree_item_slot(b)); i++)
        if (!tree_item_done(b, describe_listbox_item(hwnd, i, obj, b->str) == TRUE))
            break;
}

const struct item_handler listbox_handler =
{
    listbox_describe_focus,
    listbox_focus,
    listbox_describe_self,
    add_listbox_items,
    FALSE,
    TRUE,
};
