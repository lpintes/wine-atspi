/*
 * wine-atspi bridge, Win32 core: the window tree of the foreground
 * window, for object navigation and flat review.
 */
#include "core.h"

/*
 * The window tree of the foreground top-level window, for Orca's object
 * navigation and flat review: every visible child window, described by
 * the same handlers as a focus. It is reported when focus enters another
 * top-level window, before that focus, and again TREE_REFRESH_MS after
 * the last show, hide, move or name change in it.
 */
#define MAX_TREE 512
#define TREE_REFRESH_MS 150
/* How often a tree found busy is asked again before giving up. */
#define TREE_BUSY_RETRIES 10

enum tree_kind
{
    TREE_SKIP,      /* neither the window nor its children */
    TREE_LEAF,      /* the window, but not its children */
    TREE_NODE,      /* the window and its children */
};

HWND tree_top;
UINT_PTR tree_timer;
/* Only the items of one control, see schedule_items. */
static HWND items_control;
static UINT_PTR items_timer;
static int tree_busy_retries;

static enum tree_kind tree_kind(HWND hwnd, const WCHAR *cls)
{
    DWORD_PTR buddy = 0;
    WCHAR buddy_cls[16];
    unsigned int role;

    /* report_status_bar owns the status bar. */
    if (!lstrcmpiW(cls, L"msctls_statusbar32"))
        return TREE_SKIP;
    /* An up-down is part of its buddy's spin button, see find_updown. */
    if (!lstrcmpiW(cls, L"msctls_updown32") &&
        send_message(hwnd, UDM_GETBUDDY, 0, 0, 100, &buddy) &&
        buddy && GetClassNameW((HWND)buddy, buddy_cls, ARRAYSIZE(buddy_cls)) &&
        !lstrcmpiW(buddy_cls, L"Edit"))
        return TREE_SKIP;
    /* The size grip in a window corner, e.g. of the Open dialog. */
    if (!lstrcmpiW(cls, L"ScrollBar") &&
        (GetWindowLongW(hwnd, GWL_STYLE) & (SBS_SIZEGRIP | SBS_SIZEBOX)))
        return TREE_SKIP;
    role = role_from_class(hwnd, cls);
    /*
     * The windows inside a known control are its parts (the inner
     * ComboBox of a ComboBoxEx32, the header of a list view); only the
     * Edit of a combo box is added, see add_tree_children. A toolbar can
     * hold real controls, e.g. wordpad's font combo boxes.
     */
    if (role != BRIDGE_ROLE_UNKNOWN && role != BRIDGE_ROLE_TOOL_BAR)
        return TREE_LEAF;
    return TREE_NODE;
}

/* Describes child window hwnd of a window tree; FALSE to leave it out. */
static BOOL describe_tree_window(HWND hwnd, const WCHAR *cls, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    memset(obj, 0, sizeof(*obj));
    if (!lstrcmpiW(cls, L"Static"))
        return describe_label(hwnd, str->object, sizeof(str->object), obj);
    if (find_item_handler(cls))
    {
        describe_container(hwnd, obj, str);
        return obj->hwnd != 0;
    }
    if (!describe_object(hwnd, OBJID_CLIENT, CHILDID_SELF, obj, str) || obj->child)
        return FALSE;
    /* A child dialog, e.g. a property sheet page, only groups controls. */
    if (obj->role == BRIDGE_ROLE_DIALOG || obj->role == BRIDGE_ROLE_FRAME ||
        obj->role == BRIDGE_ROLE_WINDOW || !lstrcmpW(cls, L"#32770"))
        obj->role = BRIDGE_ROLE_PANEL;
    return TRUE;
}

static char *dup_utf8(const char *s)
{
    char *d;

    if (!s || !(d = HeapAlloc(GetProcessHeap(), 0, lstrlenA(s) + 1)))
        return NULL;
    lstrcpyA(d, s);
    return d;
}

/* Copies the strings of obj, which point into a reused focus_strings. */
static void keep_strings(struct bridge_object *obj)
{
    const char **columns = NULL;
    int *extents = NULL, *rows = NULL;
    int i;

    if (obj->column_count > 0 && obj->column_extents &&
        (extents = HeapAlloc(GetProcessHeap(), 0, obj->column_count * 4 * sizeof(int))))
        memcpy(extents, obj->column_extents, obj->column_count * 4 * sizeof(int));
    obj->column_extents = extents;
    if (obj->selected_listed > 0 && obj->selected_rows &&
        (rows = HeapAlloc(GetProcessHeap(), 0, obj->selected_listed * sizeof(int))))
        memcpy(rows, obj->selected_rows, obj->selected_listed * sizeof(int));
    else
        obj->selected_listed = 0;
    obj->selected_rows = rows;
    obj->name = dup_utf8(obj->name);
    obj->value = dup_utf8(obj->value);
    obj->window_class = dup_utf8(obj->window_class);
    obj->shortcut = dup_utf8(obj->shortcut);
    bridge_text_ref(obj->text);
    if (obj->column_count > 0 &&
        (columns = HeapAlloc(GetProcessHeap(), 0, obj->column_count * sizeof(*columns))))
        for (i = 0; i < obj->column_count; i++)
            columns[i] = dup_utf8(obj->columns[i]);
    else
        obj->column_count = 0;
    obj->columns = columns;
}

static void free_kept_strings(struct bridge_object *obj)
{
    HANDLE heap = GetProcessHeap();
    int i;

    HeapFree(heap, 0, (char *)obj->name);
    HeapFree(heap, 0, (char *)obj->value);
    HeapFree(heap, 0, (char *)obj->window_class);
    HeapFree(heap, 0, (char *)obj->shortcut);
    bridge_text_unref(obj->text);
    for (i = 0; i < obj->column_count; i++)
        HeapFree(heap, 0, (char *)obj->columns[i]);
    HeapFree(heap, 0, (void *)obj->columns);
    HeapFree(heap, 0, (void *)obj->column_extents);
    HeapFree(heap, 0, (void *)obj->selected_rows);
}

/*
 * A cleared object to describe the next item of b->list into, with the
 * strings in b->str; NULL if the tree is full.
 */
struct bridge_object *tree_item_slot(struct tree_build *b)
{
    if (b->count >= MAX_TREE)
        return NULL;
    memset(&b->objects[b->count], 0, sizeof(b->objects[0]));
    return &b->objects[b->count];
}

/*
 * After the item in the slot was described, or not: keeps it if it was
 * and lies in the client area of b->list. FALSE if it was not kept.
 */
BOOL tree_item_done(struct tree_build *b, BOOL described)
{
    struct bridge_object *obj = &b->objects[b->count];
    const int *clip = b->clip;
    BOOL shown = described && obj->has_extents && obj->width > 0 && obj->height > 0 &&
                 obj->x < clip[0] + clip[2] && obj->x + obj->width > clip[0] &&
                 obj->y < clip[1] + clip[3] && obj->y + obj->height > clip[1];

    if (shown)
    {
        keep_strings(obj);
        b->parents[b->count++] = (ULONG_PTR)b->list;
    }
    free_strings(b->str);
    return shown;
}

/*
 * The items a list, list view, tree view, tab control or toolbar shows, after the
 * control itself, so that flat review sees them. Only the items in view:
 * a list may hold thousands.
 */
static void add_tree_items(struct tree_build *b, HWND hwnd, const struct item_handler *handler)
{
    RECT client;

    if (!GetClientRect(hwnd, &client))
        return;
    client_to_screen(hwnd, client, b->clip);
    b->list = hwnd;
    handler->add_items(b, hwnd);
}

static void add_tree_children(struct tree_build *b, HWND parent)
{
    WCHAR cls[256];
    HWND hwnd, edit;

    for (hwnd = GetWindow(parent, GW_CHILD); hwnd && b->count < MAX_TREE;
         hwnd = GetWindow(hwnd, GW_HWNDNEXT))
    {
        struct bridge_object *obj = &b->objects[b->count];
        const struct item_handler *handler;
        enum tree_kind kind;

        if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
            (kind = tree_kind(hwnd, cls)) == TREE_SKIP)
            continue;
        /* A window that did not answer is left out with its children. */
        if (!describe_tree_window(hwnd, cls, obj, b->str))
        {
            free_strings(b->str);
            continue;
        }
        keep_strings(obj);
        free_strings(b->str);
        b->parents[b->count++] = (ULONG_PTR)parent;
        /* The Edit of a combo box is its child, as the entry of a GTK one. */
        if (obj->role == BRIDGE_ROLE_COMBO_BOX && (edit = combo_edit(hwnd)) &&
            b->count < MAX_TREE)
        {
            struct bridge_object *entry = &b->objects[b->count];

            if (describe_tree_window(edit, L"Edit", entry, b->str))
            {
                keep_strings(entry);
                b->parents[b->count++] = (ULONG_PTR)hwnd;
            }
            free_strings(b->str);
        }
        /* A toolbar has both: its buttons and controls on it. */
        if (b->items && (handler = find_item_handler(cls)))
            add_tree_items(b, hwnd, handler);
        if (kind == TREE_NODE)
            add_tree_children(b, hwnd);
    }
}

/*
 * Reports the tree of top, with or without items; with control only the
 * items of that control in the tree, see bridge_tree.
 */
void report_tree(HWND top, HWND control, BOOL items)
{
    HANDLE heap = GetProcessHeap();
    struct bridge_tree tree;
    const struct item_handler *handler;
    struct tree_build b;
    DWORD start = GetTickCount(), pid = 0;
    ULONG sent = send_count;
    WCHAR cls[256];
    int i;

    if (!top || is_console(top))
        return;
    b.objects = HeapAlloc(heap, HEAP_ZERO_MEMORY, MAX_TREE * sizeof(*b.objects));
    b.parents = HeapAlloc(heap, 0, MAX_TREE * sizeof(*b.parents));
    b.str = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*b.str));
    b.count = 0;
    b.items = items;
    if (b.objects && b.parents && b.str)
    {
        GetWindowThreadProcessId(top, &pid);
        if (!control)
            add_tree_children(&b, top);
        else if (GetClassNameW(control, cls, ARRAYSIZE(cls)) &&
                 (handler = find_item_handler(cls)) && handler->keys_change_items)
        {
            /*
             * The control too: a key may select an item without moving
             * the focus (the first Down in a ListBox), or add items.
             */
            if (describe_tree_window(control, cls, &b.objects[0], b.str))
                atspi_post_update(pid, &b.objects[0]);
            free_strings(b.str);
            memset(&b.objects[0], 0, sizeof(b.objects[0]));
            add_tree_items(&b, control, handler);
        }
        memset(&tree, 0, sizeof(tree));
        describe_frame(top, &tree.frame, b.str);
        /*
         * The window went away while the tree was built (Escape closes it;
         * its destroy event ran nested in a send_message and removed the
         * frame): posting the tree would add the frame again.
         */
        if (!IsWindow(top))
        {
            trace(L"tree: %p gone after %lu ms", top, GetTickCount() - start);
            for (i = 0; i < b.count; i++)
                free_kept_strings(&b.objects[i]);
            b.count = -1;
        }
        /*
         * A busy target left windows out and gave the frame no name (the
         * Open dialog while its file type combo refills the list): such a
         * tree would remove them in Orca's view, so ask again later.
         */
        else if (is_busy(top))
        {
            trace(L"tree: %p busy after %lu ms", top, GetTickCount() - start);
            for (i = 0; i < b.count; i++)
                free_kept_strings(&b.objects[i]);
            if (top == tree_top && tree_busy_retries++ < TREE_BUSY_RETRIES)
                tree_timer = SetTimer(NULL, tree_timer, TREE_REFRESH_MS, refresh_tree);
            b.count = -1;
        }
    }
    if (b.objects && b.parents && b.str && b.count >= 0)
    {
        tree_busy_retries = 0;
        tree.pid = pid;
        get_app_name(pid, b.str->app, sizeof(b.str->app));
        tree.app_name = b.str->app;
        tree.count = b.count;
        tree.objects = b.objects;
        tree.parents = b.parents;
        tree.no_items = !items;
        tree.control = (ULONG_PTR)control;
        atspi_post_tree(&tree);
        if (control)
            trace(L"tree: %p %d items of %p in %lu ms, %lu messages", top, b.count, control,
                  GetTickCount() - start, send_count - sent);
        else
            trace(L"tree: %p %d objects%s in %lu ms, %lu messages", top, b.count,
                  items ? L"" : L" (no items)", GetTickCount() - start, send_count - sent);
        for (i = 0; i < b.count; i++)
            free_kept_strings(&b.objects[i]);
    }
    HeapFree(heap, 0, b.str);
    HeapFree(heap, 0, b.parents);
    HeapFree(heap, 0, b.objects);
}

void CALLBACK refresh_tree(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, id);
    tree_timer = 0;
    /* The whole tree has the items too. */
    if (items_timer)
        KillTimer(NULL, items_timer);
    items_timer = 0;
    if (tree_top && IsWindow(tree_top))
        report_tree(tree_top, NULL, TRUE);
}

/* Nobody listens any more: no tree is known, none waits. */
void forget_tree(void)
{
    if (tree_timer)
        KillTimer(NULL, tree_timer);
    if (items_timer)
        KillTimer(NULL, items_timer);
    tree_timer = items_timer = 0;
    tree_top = items_control = NULL;
}

/* Something changed in window hwnd: refresh the tree if hwnd is in it. */
void schedule_tree(HWND hwnd)
{
    if (!tree_top || (hwnd != tree_top && !IsChild(tree_top, hwnd)))
        return;
    tree_timer = SetTimer(NULL, tree_timer, TREE_REFRESH_MS, refresh_tree);
}

static void CALLBACK refresh_items(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, id);
    items_timer = 0;
    if (tree_top && IsChild(tree_top, items_control))
        report_tree(tree_top, items_control, TRUE);
}

/*
 * A key in list, tree or tab control hwnd may have scrolled it or
 * expanded an item: refresh only its items, unless the whole tree comes
 * anyway or another control waits for its items.
 */
void schedule_items(HWND hwnd)
{
    if (!tree_top || !IsChild(tree_top, hwnd) || tree_timer)
        return;
    if (items_timer && items_control != hwnd)
    {
        schedule_tree(hwnd);
        return;
    }
    items_control = hwnd;
    items_timer = SetTimer(NULL, items_timer, TREE_REFRESH_MS, refresh_items);
}
