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
 * Level and siblings of the parents met in a walk over the visible items
 * of a tree (TVGN_FIRSTVISIBLE, then TVGN_NEXTVISIBLE). Visible items come
 * in tree order, so the next item with the same parent is the next
 * sibling: its place follows without walking all siblings again, which
 * costs one round trip each.
 */
#define TREE_CACHE_SIZE 8

struct tree_cache
{
    struct
    {
        HTREEITEM parent;
        int level, position, set_size;
    } entries[TREE_CACHE_SIZE];
    int count, next;
};

static HTREEITEM tree_next(HWND hwnd, UINT flag, HTREEITEM item)
{
    DWORD_PTR next = 0;

    if (!SendMessageTimeoutW(hwnd, TVM_GETNEXTITEM, flag, (LPARAM)item,
                             SMTO_ABORTIFHUNG, 500, &next))
        return NULL;
    return (HTREEITEM)next;
}

/* The caret item of a tree view into *item, NULL if none; LIST_BUSY if it did not answer. */
static int tree_focus_item(HWND hwnd, HTREEITEM *item)
{
    DWORD_PTR caret = 0;

    *item = NULL;
    if (!SendMessageTimeoutW(hwnd, TVM_GETNEXTITEM, TVGN_CARET, 0,
                             SMTO_ABORTIFHUNG, 500, &caret))
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

/*
 * Describes the tree item: text, selected and expanded state, its level
 * (from 1) and its place among its siblings. Orca takes the level from
 * the attribute "level" of an item inside a tree (script_utilities.py,
 * node_level) and says it when it changes. FALSE if the item could not be
 * read. r is memory in the target. cache is NULL, or the cache of a walk
 * over the visible items that item is the next one of.
 */
static BOOL describe_treeview_item_in(HWND hwnd, struct remote *r, HTREEITEM item,
                                      struct tree_cache *cache, struct bridge_object *obj,
                                      struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    HTREEITEM next, parent;
    RECT rect;
    UINT state;
    int children, i;
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

    parent = tree_next(hwnd, TVGN_PARENT, item);
    for (i = 0; cache && i < cache->count; i++)
    {
        if (cache->entries[i].parent != parent)
            continue;
        obj->level = cache->entries[i].level;
        obj->position = ++cache->entries[i].position;
        obj->set_size = cache->entries[i].set_size;
        return TRUE;
    }
    obj->level = 1;
    for (next = parent, i = 0; next && i < TREE_WALK_MAX; next = tree_next(hwnd, TVGN_PARENT, next), i++)
        obj->level++;
    obj->position = 1;
    for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_PREVIOUS, next)); i++)
        obj->position++;
    obj->set_size = obj->position;
    for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_NEXT, next)); i++)
        obj->set_size++;
    if (cache)
    {
        i = cache->next;
        cache->entries[i].parent = parent;
        cache->entries[i].level = obj->level;
        cache->entries[i].position = obj->position;
        cache->entries[i].set_size = obj->set_size;
        cache->next = (i + 1) % TREE_CACHE_SIZE;
        if (cache->count < TREE_CACHE_SIZE)
            cache->count++;
    }
    return TRUE;
}

static BOOL describe_treeview_item(HWND hwnd, HTREEITEM item, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    struct remote r;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    ok = describe_treeview_item_in(hwnd, &r, item, NULL, obj, str);
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
    struct tree_cache cache;
    struct remote r;
    HTREEITEM item;

    /* One block of memory in the target for all items. */
    if (!remote_open(hwnd, &r))
        return;
    memset(&cache, 0, sizeof(cache));
    for (item = tree_next(hwnd, TVGN_FIRSTVISIBLE, NULL); item && (obj = tree_item_slot(b));
         item = tree_next(hwnd, TVGN_NEXTVISIBLE, item))
        if (!tree_item_done(b, describe_treeview_item_in(hwnd, &r, item, &cache, obj, b->str)))
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
