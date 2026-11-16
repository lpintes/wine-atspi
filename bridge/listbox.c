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
            if (SendMessageTimeoutW(hwnd, LB_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG, 500, &item) &&
                (int)item >= 0)
            {
                str->selected_rows[0] = (int)item;
                obj->selected_count = obj->selected_listed = 1;
            }
            return;
        }
        if (!SendMessageTimeoutW(hwnd, LB_GETSELCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) ||
            (int)count <= 0 ||
            !SendMessageTimeoutW(hwnd, LB_GETSELITEMS, MAX_SELECTED_ROWS,
                                 (LPARAM)str->selected_rows, SMTO_ABORTIFHUNG, 500, &item) ||
            (int)item < 0)
            return;
        obj->selected_count = (int)count;
        obj->selected_listed = (int)item;
        return;
    }
    if (!SendMessageTimeoutW(hwnd, LVM_GETSELECTEDCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) ||
        (LONG)count <= 0)
        return;
    obj->selected_count = (int)count;
    while (obj->selected_listed < obj->selected_count &&
           obj->selected_listed < MAX_SELECTED_ROWS &&
           SendMessageTimeoutW(hwnd, LVM_GETNEXTITEM, item, LVNI_SELECTED,
                               SMTO_ABORTIFHUNG, 500, &item) && (int)item >= 0)
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
    if (SendMessageTimeoutW(hwnd, listview ? LVM_GETITEMCOUNT : LB_GETCOUNT, 0, 0,
                            SMTO_ABORTIFHUNG, 500, &count) && (LONG)count >= 0)
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
 * The list window as parent of its focused item, so that Orca presents
 * the list's label when focus enters it. No MSAA: Wine's oleacc names a
 * list only by its window text. A tree view is a tree; its items are its
 * children in the order they had focus, nesting is told by their level.
 */
void describe_container(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    WCHAR cls[256];

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    memset(obj, 0, sizeof(*obj));
    if (!is_item_control(cls) && !is_toolbar(cls))
        return;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->window_class = str->object_class;
    /* A toolbar has no label; Wine's oleacc names it by its window text. */
    if (is_toolbar(cls))
        get_window_text(hwnd, str->container, sizeof(str->container));
    else
        find_label(hwnd, str->container, sizeof(str->container));
    obj->name = str->container;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE);
    if (is_toolbar(cls))
        obj->role = BRIDGE_ROLE_TOOL_BAR;
    else if (is_tree_view(cls))
        obj->role = BRIDGE_ROLE_TREE;
    else if (is_tab_control(cls))
    {
        DWORD_PTR count = 0;

        obj->role = BRIDGE_ROLE_PAGE_TAB_LIST;
        if (SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) &&
            (LONG)count >= 0)
            obj->set_size = (int)count;
    }
    else
        describe_list_box(hwnd, !is_list_box(cls), obj, str);
    get_extents(hwnd, obj);
}

/*
 * Wine's winefile draws its tree and file panes as owner-drawn list boxes
 * without strings (programs/winefile/winefile.c, create_tree_window). The pane's
 * parent has class WFS_Tree, the process is winefile.exe (another program
 * with that class would have another layout) and the item data points to an Entry in the
 * target: three pointers, three ints, then the WIN32_FIND_DATAW whose
 * cFileName is the text it draws (draw_item). Reads that name into buf.
 */
static BOOL winefile_entry_name(HWND hwnd, int item, char *buf, int size)
{
    WCHAR cls[32], name[MAX_PATH];
    char app[32];
    DWORD_PTR entry = 0;
    DWORD pid = 0;
    HANDLE process;
    BOOL wow64 = FALSE, ok;
    SIZE_T offset, done = 0;

    if (!GetClassNameW(GetParent(hwnd), cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"WFS_Tree"))
        return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    get_app_name(pid, app, sizeof(app));
    if (lstrcmpiA(app, "winefile"))
        return FALSE;
    if (!SendMessageTimeoutW(hwnd, LB_GETITEMDATA, item, 0, SMTO_ABORTIFHUNG, 500, &entry) ||
        !entry || (LONG_PTR)entry == LB_ERR)
        return FALSE;
    if (!(process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
        return FALSE;
    IsWow64Process(process, &wow64);
    offset = 3 * (wow64 ? 4 : sizeof(void *)) + 3 * sizeof(int) +
             offsetof(WIN32_FIND_DATAW, cFileName);
    ok = ReadProcessMemory(process, (void *)(entry + offset), name, sizeof(name), &done) &&
         done == sizeof(name);
    CloseHandle(process);
    if (!ok)
        return FALSE;
    name[MAX_PATH - 1] = 0;
    to_utf8(name, buf, size);
    return TRUE;
}

/*
 * Wine's oleacc has only a stub for ListBox too: name and role of an item
 * fail with E_INVALIDARG (client.c). LB_GETTEXT is marshalled across
 * processes. An owner-drawn list without LBS_HASSTRINGS keeps no text;
 * its item is left unnamed unless it is a winefile pane. TRUE if obj was
 * filled, FALSE if the list has no such item, LIST_BUSY if it did not
 * answer.
 */
int describe_listbox_item(HWND hwnd, int item, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR count = 0, len = 0, sel = 0, ret = 0;
    WCHAR *text;
    RECT rect;

    if (!SendMessageTimeoutW(hwnd, LB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
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
        if (!SendMessageTimeoutW(hwnd, LB_GETTEXTLEN, item, 0, SMTO_ABORTIFHUNG, 500, &len))
            return LIST_BUSY;
        if ((LONG)len > 0 && (text = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR))))
        {
            text[0] = 0;
            if (SendMessageTimeoutW(hwnd, LB_GETTEXT, item, (LPARAM)text,
                                    SMTO_ABORTIFHUNG, 500, &len) && (LONG)len >= 0)
            {
                text[len] = 0;
                to_utf8(text, str->object, sizeof(str->object));
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    else
        winefile_entry_name(hwnd, item, str->object, sizeof(str->object));
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (SendMessageTimeoutW(hwnd, LB_GETSEL, item, 0, SMTO_ABORTIFHUNG, 500, &sel) &&
        (LONG)sel > 0)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    obj->has_extents = FALSE;
    if (SendMessageTimeoutW(hwnd, LB_GETITEMRECT, item, (LPARAM)&rect,
                            SMTO_ABORTIFHUNG, 500, &ret) && (LONG)ret != LB_ERR)
        set_item_extents(hwnd, rect, obj);
    obj->position = item + 1;
    obj->set_size = (int)count;
    return TRUE;
}
