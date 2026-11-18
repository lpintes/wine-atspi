/*
 * wine-atspi bridge, Win32 core: tree view (SysTreeView32) items.
 */
#include "core.h"

/* TVITEMW as a 32-bit (WoW64) process sees it. */
struct tvitem32
{
    UINT mask;
    ULONG hItem;
    UINT state;
    UINT stateMask;
    ULONG pszText;
    int cchTextMax;
    int iImage;
    int iSelectedImage;
    int cChildren;
    ULONG lParam;
};

/*
 * Wine's SysTreeView32 emits no WinEvents at all (comctl32/treeview.c)
 * and oleacc has no proxy for it, so its items are read with tree view
 * messages. TVM_GETNEXTITEM carries no pointer; an HTREEITEM is a pointer
 * in the target, only passed back.
 */

/* Bounds the walks over parents and siblings of a tree item. */
#define TREE_WALK_MAX 10000

/*
 * Places of items in the last tree view described. Each sibling walked
 * costs one round trip (about 0.75 ms; regedit's HKEY_CLASSES_ROOT with
 * 308 subkeys took 327 messages and 250 ms per arrow key, a tree of its
 * visible items 451 messages). So the level and number of children of
 * each parent met are kept, and the position of each item described.
 * A new item's position comes from a walk in both directions that stops
 * at an item with a known position or at either end: one or two
 * messages for an arrow key, for a jump at most the distance to the
 * nearest visible item or end. All is forgotten when TVM_GETCOUNT
 * changes (items added or deleted, a folder expanded the first time);
 * an item moved by sorting with the same count is not noticed.
 */
#define TREE_SETS 16
#define TREE_ANCHORS 128

static struct
{
    HWND hwnd;
    DWORD_PTR count;
    struct
    {
        HTREEITEM parent;
        int level, set_size;
    } sets[TREE_SETS];
    struct
    {
        HTREEITEM item;
        int position;
    } anchors[TREE_ANCHORS];
    int set_count, set_next, anchor_count, anchor_next;
} places;

static HTREEITEM tree_next(HWND hwnd, UINT flag, HTREEITEM item)
{
    DWORD_PTR next = 0;

    if (!send_message(hwnd, TVM_GETNEXTITEM, flag, (LPARAM)item,
                      SEND_TIMEOUT_MS, &next))
        return NULL;
    return (HTREEITEM)next;
}

/* The caret item of a tree view into *item, NULL if none; LIST_BUSY if it did not answer. */
static int tree_focus_item(HWND hwnd, HTREEITEM *item)
{
    DWORD_PTR caret = 0;

    *item = NULL;
    if (!send_message(hwnd, TVM_GETNEXTITEM, TVGN_CARET, 0,
                      SEND_TIMEOUT_MS, &caret))
        return LIST_BUSY;
    *item = (HTREEITEM)caret;
    return TRUE;
}

/*
 * MSAA child id of a tree item, made from its handle: the items are heap
 * blocks in the target, at least 16 bytes apart. Positive, as child ids
 * of items are elsewhere.
 */
static LONG tree_child_id(HTREEITEM item)
{
    LONG id = (LONG)(((ULONG_PTR)item >> 4) & 0x7fffffff);

    return id ? id : 1;
}

/* Keeps the places of hwnd if its item count did not change, else forgets them. */
static void check_places(HWND hwnd)
{
    DWORD_PTR count = 0;

    if (send_message(hwnd, TVM_GETCOUNT, 0, 0, SEND_TIMEOUT_MS, &count) &&
        hwnd == places.hwnd && count == places.count)
        return;
    memset(&places, 0, sizeof(places));
    places.hwnd = hwnd;
    places.count = count;
}

/* Position of item (from 1) if known, else 0. */
static int known_position(HTREEITEM item)
{
    int i;

    for (i = 0; i < places.anchor_count; i++)
        if (places.anchors[i].item == item)
            return places.anchors[i].position;
    return 0;
}

static void keep_position(HTREEITEM item, int position)
{
    int i;

    for (i = 0; i < places.anchor_count; i++)
        if (places.anchors[i].item == item)
        {
            places.anchors[i].position = position;
            return;
        }
    i = places.anchor_next;
    places.anchors[i].item = item;
    places.anchors[i].position = position;
    places.anchor_next = (i + 1) % TREE_ANCHORS;
    if (places.anchor_count < TREE_ANCHORS)
        places.anchor_count++;
}

/* Index of the children of parent in places.sets, -1 if not known. */
static int known_set(HTREEITEM parent)
{
    int i;

    for (i = 0; i < places.set_count; i++)
        if (places.sets[i].parent == parent)
            return i;
    return -1;
}

/*
 * Position of item among the siblings of a known set of set_size items:
 * steps back and forth from it until an end or an item with a known
 * position. 0 if the walk found no answer that fits.
 */
static int walk_position(HWND hwnd, HTREEITEM item, int set_size)
{
    HTREEITEM prev = item, next = item;
    int k, known, position = 0;

    for (k = 1; k <= TREE_WALK_MAX && !position && (prev || next); k++)
    {
        if (prev)
        {
            prev = tree_next(hwnd, TVGN_PREVIOUS, prev);
            if (!prev)
                position = k;
            else if ((known = known_position(prev)))
                position = known + k;
        }
        if (!position && next)
        {
            next = tree_next(hwnd, TVGN_NEXT, next);
            if (!next)
                position = set_size - k + 1;
            else if ((known = known_position(next)))
                position = known - k;
        }
    }
    return position >= 1 && position <= set_size ? position : 0;
}

/*
 * Level (from 1), position and set size of item, a child of parent.
 * Walks all parents and siblings the first time a parent is met, see
 * places. Nothing is kept if the target timed out meanwhile.
 */
static void place_tree_item(HWND hwnd, HTREEITEM item, HTREEITEM parent,
                            struct bridge_object *obj)
{
    HTREEITEM next;
    int i, set = known_set(parent);

    if (set >= 0 && (obj->position = known_position(item)) == 0)
        obj->position = walk_position(hwnd, item, places.sets[set].set_size);
    if (set >= 0 && obj->position)
    {
        obj->level = places.sets[set].level;
        obj->set_size = places.sets[set].set_size;
    }
    else
    {
        obj->level = 1;
        for (next = parent, i = 0; next && i < TREE_WALK_MAX;
             next = tree_next(hwnd, TVGN_PARENT, next), i++)
            obj->level++;
        obj->position = 1;
        for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_PREVIOUS, next)); i++)
            obj->position++;
        obj->set_size = obj->position;
        for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_NEXT, next)); i++)
            obj->set_size++;
        if (is_busy(hwnd))
            return;
        if (set < 0)
        {
            set = places.set_next;
            places.set_next = (set + 1) % TREE_SETS;
            if (places.set_count < TREE_SETS)
                places.set_count++;
        }
        places.sets[set].parent = parent;
        places.sets[set].level = obj->level;
        places.sets[set].set_size = obj->set_size;
    }
    if (!is_busy(hwnd))
        keep_position(item, obj->position);
}

/*
 * Describes the tree item: text, selected and expanded state, its level
 * (from 1) and its place among its siblings. Orca takes the level from
 * the attribute "level" of an item inside a tree (script_utilities.py,
 * node_level) and says it when it changes. FALSE if the item could not be
 * read. r is memory in the target. The caller calls check_places first.
 */
static BOOL describe_treeview_item_in(HWND hwnd, struct remote *r, HTREEITEM item,
                                      struct bridge_object *obj, struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    RECT rect;
    UINT state;
    int children;
    SIZE_T done;
    BOOL ok;

    memset(header, 0, sizeof(header));
    REMOTE_STRUCT(r, header, TVITEMW, struct tvitem32, tv,
                  tv->mask = TVIF_HANDLE | TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN;
                  REMOTE_PTR(tv->hItem, item);
                  tv->stateMask = TVIS_SELECTED | TVIS_EXPANDED;
                  REMOTE_PTR(tv->pszText, REMOTE_TEXT(r));
                  tv->cchTextMax = CELL_TEXT_MAX;);
    ok = remote_text(hwnd, r, TVM_GETITEMW, 0, header, text) &&
         ReadProcessMemory(r->process, r->base, header, sizeof(header), &done);
    /* The item goes in the RECT, as a handle of the target's size; FALSE if scrolled away. */
    memset(&rect, 0, sizeof(rect));
    REMOTE_STRUCT(r, &rect, HTREEITEM, ULONG, handle, REMOTE_PTR(*handle, item););
    obj->has_extents = FALSE;
    if (ok && remote_rect(hwnd, r, TVM_GETITEMRECT, TRUE, &rect))
        set_item_extents(hwnd, rect, obj);
    if (!ok)
        return FALSE;
    REMOTE_STRUCT(r, header, TVITEMW, struct tvitem32, tv,
                  state = tv->state;
                  children = tv->cChildren;);

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = tree_child_id(item);
    obj->role = BRIDGE_ROLE_TREE_ITEM;
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
    if (state & TVIS_SELECTED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    /* cChildren may be a callback (I_CHILDRENCALLBACK), or 0 with children. */
    if (children || tree_next(hwnd, TVGN_CHILD, item))
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE) |
                       BRIDGE_STATE_BIT((state & TVIS_EXPANDED) ? BRIDGE_STATE_EXPANDED
                                                                : BRIDGE_STATE_COLLAPSED);

    place_tree_item(hwnd, item, tree_next(hwnd, TVGN_PARENT, item), obj);
    return TRUE;
}

static BOOL describe_treeview_item(HWND hwnd, HTREEITEM item, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    struct remote r;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    check_places(hwnd);
    ok = describe_treeview_item_in(hwnd, &r, item, obj, str);
    remote_close(&r);
    return ok;
}

/* Only the caret item can have focus; the child id just names it. */
static int treeview_describe_focus(HWND hwnd, LONG id_child, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    HTREEITEM item;

    if (tree_focus_item(hwnd, &item) == LIST_BUSY)
        return LIST_BUSY;
    if (!item)
        return FALSE;
    return describe_treeview_item(hwnd, item, obj, str) ? TRUE : LIST_BUSY;
}

static LONG treeview_focus(HWND hwnd)
{
    HTREEITEM item;

    if (tree_focus_item(hwnd, &item) == LIST_BUSY)
        return LIST_BUSY;
    return item ? tree_child_id(item) : 0;
}

static void treeview_describe_self(HWND hwnd, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    obj->role = BRIDGE_ROLE_TREE;
}

/* The visible items from the first one down to the first out of view. */
static void add_treeview_items(struct tree_build *b, HWND hwnd)
{
    struct bridge_object *obj;
    struct remote r;
    HTREEITEM item;

    /* One block of memory in the target for all items. */
    if (!remote_open(hwnd, &r))
        return;
    /*
     * Visible items come in tree order, so the previous sibling of one is
     * often the item before: its position is known after one message.
     */
    check_places(hwnd);
    for (item = tree_next(hwnd, TVGN_FIRSTVISIBLE, NULL); item && (obj = tree_item_slot(b));
         item = tree_next(hwnd, TVGN_NEXTVISIBLE, item))
        if (!tree_item_done(b, describe_treeview_item_in(hwnd, &r, item, obj, b->str)))
            break;
    remote_close(&r);
}

const struct item_handler treeview_handler =
{
    treeview_describe_focus,
    treeview_focus,
    treeview_describe_self,
    add_treeview_items,
    FALSE,
    TRUE,
};
