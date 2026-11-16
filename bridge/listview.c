/*
 * wine-atspi bridge, Win32 core: list view (SysListView32) items, cells
 * and column headers.
 */
#include "core.h"

/* LVITEMW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct lvitem32
{
    UINT mask;
    int iItem;
    int iSubItem;
    UINT state;
    UINT stateMask;
    ULONG pszText;
    int cchTextMax;
};

/* LVCOLUMNW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct lvcolumn32
{
    UINT mask;
    int fmt;
    int cx;
    ULONG pszText;
    int cchTextMax;
};

/*
 * What all items of a list view share. The window tree describes many
 * rows of one list, and each message to the target is a round trip.
 */
struct listview_info
{
    int columns;            /* from listview_columns */
    int order[MAX_COLUMNS];
    DWORD_PTR count;        /* items */
    /*
     * Cell x and width of each column from the first row described: in
     * report view all rows have the same columns, so a later row asks
     * only for its own top and height.
     */
    BOOL have_cells;
    int cells[4 * MAX_COLUMNS];
};

/* Text of item's subitem (0 for the item itself) of a list view. */
static BOOL get_listview_text(HWND hwnd, struct remote *r, int item, int subitem, WCHAR *buf)
{
    BYTE header[REMOTE_HEADER];

    memset(header, 0, sizeof(header));
    REMOTE_STRUCT(r, header, LVITEMW, struct lvitem32, lv,
                  lv->iSubItem = subitem;
                  REMOTE_PTR(lv->pszText, REMOTE_TEXT(r));
                  lv->cchTextMax = CELL_TEXT_MAX;);
    return remote_text(hwnd, r, LVM_GETITEMTEXTW, item, header, buf);
}

/* Header text of column (a subitem index) of a list view. */
BOOL get_listview_header(HWND hwnd, struct remote *r, int column, WCHAR *buf)
{
    BYTE header[REMOTE_HEADER];

    memset(header, 0, sizeof(header));
    REMOTE_STRUCT(r, header, LVCOLUMNW, struct lvcolumn32, lv,
                  lv->mask = LVCF_TEXT;
                  REMOTE_PTR(lv->pszText, REMOTE_TEXT(r));
                  lv->cchTextMax = CELL_TEXT_MAX;);
    return remote_text(hwnd, r, LVM_GETCOLUMNW, column, header, buf);
}

/*
 * Columns of a list view in report view: their subitem indices in display
 * order (the user may drag columns). 0 in other views. Wine changes the
 * view with LVM_SETVIEW without touching the style, so ask for the view.
 */
int listview_columns(HWND hwnd, struct remote *r, int *order)
{
    DWORD_PTR view = 0, header = 0, count = 0, ok = 0;
    SIZE_T done;
    int i;

    if (!send_message(hwnd, LVM_GETVIEW, 0, 0, SEND_TIMEOUT_MS, &view) ||
        view != LV_VIEW_DETAILS ||
        !send_message(hwnd, LVM_GETHEADER, 0, 0, SEND_TIMEOUT_MS, &header) ||
        !header ||
        !send_message((HWND)header, HDM_GETITEMCOUNT, 0, 0, SEND_TIMEOUT_MS, &count) ||
        (LONG)count <= 0)
        return 0;
    for (i = 0; i < MAX_COLUMNS; i++)
        order[i] = i;
    /* The header wants room for all columns (header.c, HEADER_GetOrderArray). */
    if (count > MAX_COLUMNS)
        return MAX_COLUMNS;
    if (send_message(hwnd, LVM_GETCOLUMNORDERARRAY, count, (LPARAM)r->base,
                     SEND_TIMEOUT_MS, &ok) && ok &&
        ReadProcessMemory(r->process, r->base, order, count * sizeof(int), &done))
    {
        for (i = 0; i < (int)count; i++)
            if (order[i] < 0 || order[i] >= (int)count)
                order[i] = i;
    }
    return (int)count;
}

/* Index of the focused item, -1 if none; LIST_BUSY if the list did not answer. */
static int listview_focus_index(HWND hwnd)
{
    DWORD_PTR item = (DWORD_PTR)-1;

    if (!send_message(hwnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED,
                      SEND_TIMEOUT_MS, &item))
        return LIST_BUSY;
    return (int)item;
}

static void get_listview_info(HWND hwnd, struct remote *r, struct listview_info *info)
{
    info->columns = listview_columns(hwnd, r, info->order);
    info->count = 0;
    send_message(hwnd, LVM_GETITEMCOUNT, 0, 0, SEND_TIMEOUT_MS, &info->count);
    info->have_cells = FALSE;
}

/* Screen rects of the cells of a report view row into cells (4 ints each). */
static void get_listview_cells(HWND hwnd, struct remote *r, struct listview_info *info, int item,
                               int *cells)
{
    const int *order = info->order;
    RECT rect;
    int i, row[4];

    if (info->have_cells)
    {
        SetRect(&rect, LVIR_BOUNDS, 0, 0, 0);
        if (!remote_rect(hwnd, r, LVM_GETITEMRECT, item, &rect))
            SetRectEmpty(&rect);
        client_to_screen(hwnd, rect, row);
        for (i = 0; i < info->columns; i++)
        {
            cells[4 * i] = info->cells[4 * i];
            cells[4 * i + 1] = row[1];
            cells[4 * i + 2] = info->cells[4 * i + 2];
            cells[4 * i + 3] = row[3];
        }
        return;
    }
    for (i = 0; i < info->columns; i++)
    {
        /* Subitem 0 gives the whole row; its own part is the label. */
        SetRect(&rect, order[i] ? LVIR_BOUNDS : LVIR_LABEL, order[i], 0, 0);
        if (!remote_rect(hwnd, r, order[i] ? LVM_GETSUBITEMRECT : LVM_GETITEMRECT, item, &rect))
            SetRectEmpty(&rect);
        client_to_screen(hwnd, rect, &cells[4 * i]);
    }
    memcpy(info->cells, cells, info->columns * 4 * sizeof(int));
    info->have_cells = TRUE;
}

/*
 * Wine's oleacc has only a stub for SysListView32, and the list view
 * emits no focus or selection events, so describe the item through list
 * view messages. The MSAA child id is item + 1, as on Windows. In report
 * view the item is the cell of the first column of a table row, and the
 * texts of the whole row go along, as Orca reads the full row of a GUI
 * table (generator.py, _cells_to_present). FALSE if the text could not
 * be read. r is memory in the target, info comes from get_listview_info;
 * the tree passes them for all items of a list.
 */
static BOOL describe_listview_item_in(HWND hwnd, struct remote *r, struct listview_info *info,
                                      int item, struct bridge_object *obj,
                                      struct focus_strings *str)
{
    const int *order = info->order;
    int columns = info->columns;
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR state = 0;
    int i;
    RECT rect;
    BOOL ok, has_rect = FALSE;

    ok = get_listview_text(hwnd, r, item, columns ? order[0] : 0, text);
    for (i = 0; ok && i < columns; i++)
    {
        if (i && !get_listview_text(hwnd, r, item, order[i], text))
            text[0] = 0;
        to_utf8(text, str->cells[i], sizeof(str->cells[i]));
        str->cell_ptrs[i] = str->cells[i];
    }
    if (ok && columns)
        get_listview_cells(hwnd, r, info, item, str->cell_extents);
    if (ok && !columns)
    {
        SetRect(&rect, LVIR_BOUNDS, 0, 0, 0);
        has_rect = remote_rect(hwnd, r, LVM_GETITEMRECT, item, &rect);
    }
    if (!ok)
        return FALSE;
    if (has_rect)
        set_item_extents(hwnd, rect, obj);
    else if (columns)
    {
        obj->has_extents = TRUE;
        obj->x = str->cell_extents[0];
        obj->y = str->cell_extents[1];
        obj->width = str->cell_extents[2];
        obj->height = str->cell_extents[3];
    }
    else
        obj->has_extents = FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = columns ? BRIDGE_ROLE_TABLE_CELL : BRIDGE_ROLE_LIST_ITEM;
    obj->value = NULL;
    obj->column_count = columns;
    obj->columns = columns ? str->cell_ptrs : NULL;
    obj->column_extents = columns ? str->cell_extents : NULL;
    if (columns)
        lstrcpyA(str->object, str->cells[0]);
    else
        to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE);
    send_message(hwnd, LVM_GETITEMSTATE, item, LVIS_FOCUSED | LVIS_SELECTED,
                 SEND_TIMEOUT_MS, &state);
    if (state & LVIS_FOCUSED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (state & LVIS_SELECTED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    obj->position = item + 1;
    obj->set_size = (int)info->count;
    return TRUE;
}

static BOOL describe_listview_item(HWND hwnd, int item, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    struct listview_info info;
    struct remote r;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    get_listview_info(hwnd, &r, &info);
    ok = describe_listview_item_in(hwnd, &r, &info, item, obj, str);
    remote_close(&r);
    return ok;
}

static int listview_describe_focus(HWND hwnd, LONG id_child, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    int item = id_child > 0 ? id_child - 1 : listview_focus_index(hwnd);

    if (item == LIST_BUSY)
        return LIST_BUSY;
    if (item < 0)
        return FALSE;
    return describe_listview_item(hwnd, item, obj, str) ? TRUE : LIST_BUSY;
}

/* SysListView32 emits no focus events for its items. */
static LONG listview_focus(HWND hwnd)
{
    int item = listview_focus_index(hwnd);

    return item == LIST_BUSY ? LIST_BUSY : item < 0 ? 0 : item + 1;
}

static void listview_describe_self(HWND hwnd, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    describe_list_box(hwnd, TRUE, obj, str);
}

static void add_listview_items(struct tree_build *b, HWND hwnd)
{
    DWORD_PTR top = 0, page = 0;
    struct listview_info info;
    struct bridge_object *obj;
    struct remote r;
    int i, end;

    /* One block of memory in the target for all items. */
    if (!remote_open(hwnd, &r))
        return;
    /*
     * In icon views the top index is 0 and a page holds every item, so
     * each item is checked against the client area.
     */
    get_listview_info(hwnd, &r, &info);
    if (!send_message(hwnd, LVM_GETTOPINDEX, 0, 0, SEND_TIMEOUT_MS, &top) ||
        !send_message(hwnd, LVM_GETCOUNTPERPAGE, 0, 0, SEND_TIMEOUT_MS, &page))
        top = page = info.count = 0;
    /* One more for a row cut at the bottom. */
    end = (int)min(info.count, top + page + 1);
    for (i = (int)top; i < end && (obj = tree_item_slot(b)); i++)
        tree_item_done(b, describe_listview_item_in(hwnd, &r, &info, i, obj, b->str));
    remote_close(&r);
}

const struct item_handler listview_handler =
{
    listview_describe_focus,
    listview_focus,
    listview_describe_self,
    add_listview_items,
    FALSE,
    TRUE,
};
