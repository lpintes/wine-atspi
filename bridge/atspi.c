/*
 * AT-SPI side of wine-atspi: GDBus on the accessibility bus.
 *
 * Linux headers only, no Windows headers. Compiled with the host gcc
 * against glibc, then linked into the Winelib binary by winegcc.
 *
 * Do not call C library functions directly from this file (strcmp,
 * printf, ...). The Win32 core is linked against Wine's msvcrt, and an
 * unqualified libc symbol could bind to the msvcrt import thunk, which
 * uses the ms_abi calling convention. Use the GLib equivalents.
 *
 * Model: one struct app per Wine process, each with its own D-Bus
 * connection, so Orca sees one application per process. Its nodes form
 * a shallow tree: root (application) -> frames (top-level windows) ->
 * objects that have had focus. Everything here runs on the AT-SPI
 * thread; other threads only queue work with atspi_post_*.
 */
#include <gio/gio.h>

#include "bridge.h"

#define ATSPI_PREFIX "/org/a11y/atspi/accessible"
#define ATSPI_ROOT_PATH ATSPI_PREFIX "/root"
#define ATSPI_NULL_PATH "/org/a11y/atspi/null"
#define EVENT_OBJECT "org.a11y.atspi.Event.Object"
#define EVENT_WINDOW "org.a11y.atspi.Event.Window"

static const char introspection_xml[] =
    "<node>"
    " <interface name='org.a11y.atspi.Accessible'>"
    "  <property name='Name' type='s' access='read'/>"
    "  <property name='Description' type='s' access='read'/>"
    "  <property name='Parent' type='(so)' access='read'/>"
    "  <property name='ChildCount' type='i' access='read'/>"
    "  <property name='Locale' type='s' access='read'/>"
    "  <property name='AccessibleId' type='s' access='read'/>"
    "  <method name='GetChildAtIndex'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='GetChildren'><arg direction='out' type='a(so)'/></method>"
    "  <method name='GetIndexInParent'><arg direction='out' type='i'/></method>"
    "  <method name='GetRelationSet'><arg direction='out' type='a(ua(so))'/></method>"
    "  <method name='GetRole'><arg direction='out' type='u'/></method>"
    "  <method name='GetRoleName'><arg direction='out' type='s'/></method>"
    "  <method name='GetLocalizedRoleName'><arg direction='out' type='s'/></method>"
    "  <method name='GetState'><arg direction='out' type='au'/></method>"
    "  <method name='GetAttributes'><arg direction='out' type='a{ss}'/></method>"
    "  <method name='GetApplication'><arg direction='out' type='(so)'/></method>"
    "  <method name='GetInterfaces'><arg direction='out' type='as'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Application'>"
    "  <property name='ToolkitName' type='s' access='read'/>"
    "  <property name='Version' type='s' access='read'/>"
    "  <property name='AtspiVersion' type='s' access='read'/>"
    "  <property name='Id' type='i' access='readwrite'/>"
    "  <method name='GetLocale'>"
    "   <arg direction='in' name='lctype' type='u'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    " </interface>"
    "</node>";

struct app;

struct node
{
    struct app *app;
    char *id;               /* name below ATSPI_PREFIX: "root" or "w<hwnd>_<child>" */
    char *path;
    struct node *parent;    /* NULL for the root */
    GPtrArray *children;    /* struct node *, not owned */
    guint32 role;
    guint64 states;
    char *name;
    char *value;
    char *window_class;
};

struct app
{
    guint pid;
    GDBusConnection *conn;
    char *parent_bus;
    char *parent_path;
    gint32 id;
    GHashTable *nodes;      /* id -> struct node *, owned */
    struct node *root;
};

static GMainContext *context;
static char *a11y_address;
static GDBusNodeInfo *introspection;
static GHashTable *apps;            /* pid -> struct app * */
static struct node *active_frame;
static struct node *focused;

static const char *role_name(guint32 role)
{
    switch (role)
    {
    case BRIDGE_ROLE_CHECK_BOX: return "check box";
    case BRIDGE_ROLE_COLUMN_HEADER: return "column header";
    case BRIDGE_ROLE_COMBO_BOX: return "combo box";
    case BRIDGE_ROLE_DIALOG: return "dialog";
    case BRIDGE_ROLE_FILLER: return "filler";
    case BRIDGE_ROLE_FRAME: return "frame";
    case BRIDGE_ROLE_IMAGE: return "image";
    case BRIDGE_ROLE_LABEL: return "label";
    case BRIDGE_ROLE_LIST: return "list";
    case BRIDGE_ROLE_LIST_ITEM: return "list item";
    case BRIDGE_ROLE_MENU: return "menu";
    case BRIDGE_ROLE_MENU_BAR: return "menu bar";
    case BRIDGE_ROLE_MENU_ITEM: return "menu item";
    case BRIDGE_ROLE_PAGE_TAB: return "page tab";
    case BRIDGE_ROLE_PAGE_TAB_LIST: return "page tab list";
    case BRIDGE_ROLE_PANEL: return "panel";
    case BRIDGE_ROLE_PASSWORD_TEXT: return "password text";
    case BRIDGE_ROLE_POPUP_MENU: return "popup menu";
    case BRIDGE_ROLE_PROGRESS_BAR: return "progress bar";
    case BRIDGE_ROLE_PUSH_BUTTON: return "push button";
    case BRIDGE_ROLE_RADIO_BUTTON: return "radio button";
    case BRIDGE_ROLE_SCROLL_BAR: return "scroll bar";
    case BRIDGE_ROLE_SEPARATOR: return "separator";
    case BRIDGE_ROLE_SLIDER: return "slider";
    case BRIDGE_ROLE_SPIN_BUTTON: return "spin button";
    case BRIDGE_ROLE_STATUS_BAR: return "status bar";
    case BRIDGE_ROLE_TABLE: return "table";
    case BRIDGE_ROLE_TABLE_CELL: return "table cell";
    case BRIDGE_ROLE_TEXT: return "text";
    case BRIDGE_ROLE_TOOL_BAR: return "tool bar";
    case BRIDGE_ROLE_TREE: return "tree";
    case BRIDGE_ROLE_WINDOW: return "window";
    case BRIDGE_ROLE_APPLICATION: return "application";
    case BRIDGE_ROLE_ENTRY: return "entry";
    case BRIDGE_ROLE_LINK: return "link";
    case BRIDGE_ROLE_TABLE_ROW: return "table row";
    case BRIDGE_ROLE_TREE_ITEM: return "tree item";
    case BRIDGE_ROLE_GROUPING: return "grouping";
    }
    return "unknown";
}

static GVariant *node_ref(struct node *node)
{
    return g_variant_new("(so)", g_dbus_connection_get_unique_name(node->app->conn),
                         node->path);
}

static GVariant *parent_ref(struct node *node)
{
    struct app *app = node->app;

    if (node->parent)
        return node_ref(node->parent);
    if (!app->parent_bus)
        return g_variant_new("(so)", "", ATSPI_NULL_PATH);
    return g_variant_new("(so)", app->parent_bus, app->parent_path);
}

static gint index_in_parent(struct node *node)
{
    guint i;

    if (node->parent && g_ptr_array_find(node->parent->children, node, &i))
        return i;
    return -1;
}

static void emit(struct node *node, const char *iface, const char *member,
                 const char *detail, gint d1, gint d2, GVariant *any)
{
    g_dbus_connection_emit_signal(
        node->app->conn, NULL, node->path, iface, member,
        g_variant_new("(siiv@a{sv})", detail, d1, d2, any,
                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
        NULL);
}

/* The detail string of StateChanged; NULL for states never announced. */
static const char *state_name(guint state)
{
    switch (state)
    {
    case BRIDGE_STATE_ACTIVE: return "active";
    case BRIDGE_STATE_CHECKED: return "checked";
    case BRIDGE_STATE_EDITABLE: return "editable";
    case BRIDGE_STATE_ENABLED: return "enabled";
    case BRIDGE_STATE_EXPANDABLE: return "expandable";
    case BRIDGE_STATE_EXPANDED: return "expanded";
    case BRIDGE_STATE_FOCUSED: return "focused";
    case BRIDGE_STATE_INDETERMINATE: return "indeterminate";
    case BRIDGE_STATE_PRESSED: return "pressed";
    case BRIDGE_STATE_READ_ONLY: return "read-only";
    case BRIDGE_STATE_SELECTED: return "selected";
    case BRIDGE_STATE_SENSITIVE: return "sensitive";
    }
    return NULL;
}

static void set_state(struct node *node, guint state, gboolean on)
{
    guint64 bit = BRIDGE_STATE_BIT(state);
    const char *name = state_name(state);

    if (!!(node->states & bit) == !!on)
        return;
    node->states ^= bit;
    if (name)
        emit(node, EVENT_OBJECT, "StateChanged", name, on, 0, g_variant_new_int32(0));
}

static void method_call(GDBusConnection *conn, const char *sender,
                        const char *path, const char *iface,
                        const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    struct node *node = data;
    GVariant *ret = NULL;
    guint i;

    if (!g_strcmp0(method, "GetChildAtIndex"))
    {
        gint index;

        g_variant_get(params, "(i)", &index);
        if (index >= 0 && index < (gint)node->children->len)
            ret = g_variant_new("(@(so))", node_ref(node->children->pdata[index]));
        else
            ret = g_variant_new("((so))", "", ATSPI_NULL_PATH);
    }
    else if (!g_strcmp0(method, "GetChildren"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
        for (i = 0; i < node->children->len; i++)
            g_variant_builder_add_value(&b, node_ref(node->children->pdata[i]));
        ret = g_variant_new("(a(so))", &b);
    }
    else if (!g_strcmp0(method, "GetIndexInParent"))
        ret = g_variant_new("(i)", index_in_parent(node));
    else if (!g_strcmp0(method, "GetRelationSet"))
        ret = g_variant_new("(@a(ua(so)))",
                            g_variant_new_array(G_VARIANT_TYPE("(ua(so))"), NULL, 0));
    else if (!g_strcmp0(method, "GetRole"))
        ret = g_variant_new("(u)", node->role);
    else if (!g_strcmp0(method, "GetRoleName") ||
             !g_strcmp0(method, "GetLocalizedRoleName"))
        ret = g_variant_new("(s)", role_name(node->role));
    else if (!g_strcmp0(method, "GetState"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
        g_variant_builder_add(&b, "u", (guint32)node->states);
        g_variant_builder_add(&b, "u", (guint32)(node->states >> 32));
        ret = g_variant_new("(au)", &b);
    }
    else if (!g_strcmp0(method, "GetAttributes"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
        if (node->window_class)
            g_variant_builder_add(&b, "{ss}", "class", node->window_class);
        /* Orca takes a combo box value from here (generator.py). */
        if (node->value)
            g_variant_builder_add(&b, "{ss}", "valuetext", node->value);
        ret = g_variant_new("(a{ss})", &b);
    }
    else if (!g_strcmp0(method, "GetApplication"))
        ret = g_variant_new("(@(so))", node_ref(node->app->root));
    else if (!g_strcmp0(method, "GetInterfaces"))
    {
        const char *ifaces[] = { "org.a11y.atspi.Accessible",
                                 "org.a11y.atspi.Application" };
        ret = g_variant_new("(@as)",
                            g_variant_new_strv(ifaces, node->parent ? 1 : 2));
    }
    else if (!g_strcmp0(method, "GetLocale"))
        ret = g_variant_new("(s)", "");

    if (ret)
        g_dbus_method_invocation_return_value(inv, ret);
    else
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "%s not supported", method);
}

static GVariant *get_property(GDBusConnection *conn, const char *sender,
                              const char *path, const char *iface,
                              const char *prop, GError **error, gpointer data)
{
    struct node *node = data;

    if (!g_strcmp0(prop, "Name"))
        return g_variant_new_string(node->name ? node->name : "");
    if (!g_strcmp0(prop, "Description") || !g_strcmp0(prop, "Locale") ||
        !g_strcmp0(prop, "AccessibleId"))
        return g_variant_new_string("");
    if (!g_strcmp0(prop, "Parent"))
        return parent_ref(node);
    if (!g_strcmp0(prop, "ChildCount"))
        return g_variant_new_int32(node->children->len);
    if (!g_strcmp0(prop, "ToolkitName"))
        return g_variant_new_string("wine-atspi");
    if (!g_strcmp0(prop, "Version"))
        return g_variant_new_string("0.1");
    if (!g_strcmp0(prop, "AtspiVersion"))
        return g_variant_new_string("2.1");
    if (!g_strcmp0(prop, "Id"))
        return g_variant_new_int32(node->app->id);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "%s not supported", prop);
    return NULL;
}

static gboolean set_property(GDBusConnection *conn, const char *sender,
                             const char *path, const char *iface,
                             const char *prop, GVariant *value,
                             GError **error, gpointer data)
{
    struct node *node = data;

    if (!g_strcmp0(prop, "Id"))
    {
        node->app->id = g_variant_get_int32(value);
        return TRUE;
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
                "%s is read-only", prop);
    return FALSE;
}

static const GDBusInterfaceVTable vtable = {
    method_call, get_property, set_property
};

/* Subtree callbacks: node is the path element below ATSPI_PREFIX. */
static char **subtree_enumerate(GDBusConnection *conn, const char *sender,
                                const char *path, gpointer data)
{
    struct app *app = data;
    GPtrArray *names = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init(&iter, app->nodes);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        g_ptr_array_add(names, g_strdup(key));
    g_ptr_array_add(names, NULL);
    return (char **)g_ptr_array_free(names, FALSE);
}

static GDBusInterfaceInfo **subtree_introspect(GDBusConnection *conn,
                                               const char *sender,
                                               const char *path,
                                               const char *name, gpointer data)
{
    struct app *app = data;
    struct node *node = name ? g_hash_table_lookup(app->nodes, name) : NULL;
    GDBusInterfaceInfo **ret;
    int i, count;

    if (!node)
        return NULL;
    count = node->parent ? 1 : 2;
    ret = g_new0(GDBusInterfaceInfo *, count + 1);
    for (i = 0; i < count; i++)
        ret[i] = g_dbus_interface_info_ref(introspection->interfaces[i]);
    return ret;
}

static const GDBusInterfaceVTable *subtree_dispatch(GDBusConnection *conn,
                                                    const char *sender,
                                                    const char *path,
                                                    const char *iface,
                                                    const char *name,
                                                    gpointer *out_data,
                                                    gpointer data)
{
    struct app *app = data;
    struct node *node = name ? g_hash_table_lookup(app->nodes, name) : NULL;

    if (!node)
        return NULL;
    if (!g_strcmp0(iface, "org.a11y.atspi.Application") && node->parent)
        return NULL;
    *out_data = node;
    return &vtable;
}

static const GDBusSubtreeVTable subtree_vtable = {
    subtree_enumerate, subtree_introspect, subtree_dispatch
};

static struct node *node_new(struct app *app, const char *id, struct node *parent)
{
    struct node *node = g_new0(struct node, 1);

    node->app = app;
    node->id = g_strdup(id);
    node->path = g_strconcat(ATSPI_PREFIX "/", id, NULL);
    node->parent = parent;
    node->children = g_ptr_array_new();
    g_hash_table_insert(app->nodes, node->id, node);
    if (parent)
    {
        g_ptr_array_add(parent->children, node);
        emit(parent, EVENT_OBJECT, "ChildrenChanged", "add",
             parent->children->len - 1, 0, node_ref(node));
    }
    return node;
}

/* Frees node and its subtree; only the caller's node is announced. */
static void node_free(struct node *node)
{
    guint i;

    for (i = 0; i < node->children->len; i++)
        node_free(node->children->pdata[i]);
    if (focused == node)
        focused = NULL;
    if (active_frame == node)
        active_frame = NULL;
    g_hash_table_remove(node->app->nodes, node->id);
    g_ptr_array_free(node->children, TRUE);
    g_free(node->id);
    g_free(node->path);
    g_free(node->name);
    g_free(node->value);
    g_free(node->window_class);
    g_free(node);
}

static void node_remove(struct node *node)
{
    struct node *parent = node->parent;
    gint index = index_in_parent(node);

    if (parent && index >= 0)
    {
        g_ptr_array_remove_index(parent->children, index);
        emit(parent, EVENT_OBJECT, "ChildrenChanged", "remove", index, 0,
             node_ref(node));
    }
    node_free(node);
}

static char *object_id(const struct bridge_object *obj)
{
    return g_strdup_printf("w%" G_GINT64_MODIFIER "x_%u",
                           (guint64)obj->hwnd, obj->child);
}

/* ACTIVE and FOCUSED are ours: they change only with focus events. */
#define OWN_STATES (BRIDGE_STATE_BIT(BRIDGE_STATE_ACTIVE) | \
                    BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED))

/* Copies obj into an existing node, announcing a changed name or state. */
static void update_node(struct node *node, const struct bridge_object *obj)
{
    guint64 changed = (node->states ^ obj->states) & ~OWN_STATES;
    guint state;

    node->role = obj->role;
    if (g_strcmp0(node->window_class, obj->window_class))
    {
        g_free(node->window_class);
        node->window_class = g_strdup(obj->window_class);
    }
    for (state = 0; state < 64; state++)
        if (changed & BRIDGE_STATE_BIT(state))
            set_state(node, state, !!(obj->states & BRIDGE_STATE_BIT(state)));
    if (g_strcmp0(node->name, obj->name))
    {
        g_free(node->name);
        node->name = g_strdup(obj->name);
        emit(node, EVENT_OBJECT, "PropertyChange", "accessible-name", 0, 0,
             g_variant_new_string(node->name ? node->name : ""));
    }
    if (g_strcmp0(node->value, obj->value))
    {
        g_free(node->value);
        node->value = g_strdup(obj->value);
        /* Orca presents a focused combo box again on selection-changed. */
        if (node->role == BRIDGE_ROLE_COMBO_BOX)
            emit(node, EVENT_OBJECT, "SelectionChanged", "", 0, 0, g_variant_new_int32(0));
    }
}

/* Finds or creates the node for obj below parent and updates it. */
static struct node *get_node(struct app *app, const struct bridge_object *obj,
                             struct node *parent)
{
    char *id = object_id(obj);
    struct node *node = g_hash_table_lookup(app->nodes, id);

    if (!node)
    {
        node = node_new(app, id, parent);
        node->role = obj->role;
        node->states = obj->states & ~OWN_STATES;
        node->name = g_strdup(obj->name);
        node->value = g_strdup(obj->value);
        node->window_class = g_strdup(obj->window_class);
    }
    else
        update_node(node, obj);
    g_free(id);
    return node;
}

static struct app *get_app(guint pid, const char *name)
{
    struct app *app = g_hash_table_lookup(apps, GUINT_TO_POINTER(pid));
    GError *error = NULL;
    GVariant *ret;

    if (app)
        return app->conn ? app : NULL;

    app = g_new0(struct app, 1);
    app->pid = pid;
    app->nodes = g_hash_table_new(g_str_hash, g_str_equal);
    app->root = node_new(app, "root", NULL);
    app->root->role = BRIDGE_ROLE_APPLICATION;
    app->root->name = g_strdup(name);
    /* Remember failures too, so a broken process is not retried per event. */
    g_hash_table_insert(apps, GUINT_TO_POINTER(pid), app);

    app->conn = g_dbus_connection_new_for_address_sync(
        a11y_address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    if (!app->conn)
        goto fail;
    if (!g_dbus_connection_register_subtree(
            app->conn, ATSPI_PREFIX, &subtree_vtable,
            G_DBUS_SUBTREE_FLAGS_DISPATCH_TO_UNENUMERATED_NODES, app, NULL, &error))
        goto fail;

    ret = g_dbus_connection_call_sync(
        app->conn, "org.a11y.atspi.Registry", ATSPI_ROOT_PATH,
        "org.a11y.atspi.Socket", "Embed",
        g_variant_new("((so))", g_dbus_connection_get_unique_name(app->conn),
                      ATSPI_ROOT_PATH),
        G_VARIANT_TYPE("((so))"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (!ret)
        goto fail;
    g_variant_get(ret, "((so))", &app->parent_bus, &app->parent_path);
    g_variant_unref(ret);
    g_printerr("wine-atspi: registered %s (pid %u) as %s\n", name, pid,
               g_dbus_connection_get_unique_name(app->conn));
    return app;

fail:
    g_printerr("wine-atspi: cannot register %s (pid %u): %s\n", name, pid,
               error->message);
    g_error_free(error);
    g_clear_object(&app->conn);
    return NULL;
}

static gboolean focus_idle(gpointer data)
{
    const struct bridge_focus *f = data;
    struct app *app = get_app(f->pid, f->app_name);
    struct node *frame, *obj;

    if (!app)
        return G_SOURCE_REMOVE;
    frame = get_node(app, &f->frame, app->root);
    if (f->object.hwnd == f->frame.hwnd && f->object.child == f->frame.child)
        obj = frame;
    else
        obj = get_node(app, &f->object, frame);

    if (active_frame != frame)
    {
        if (active_frame)
        {
            set_state(active_frame, BRIDGE_STATE_ACTIVE, FALSE);
            emit(active_frame, EVENT_WINDOW, "Deactivate", "", 0, 0,
                 g_variant_new_string(""));
        }
        active_frame = frame;
        set_state(frame, BRIDGE_STATE_ACTIVE, TRUE);
        emit(frame, EVENT_WINDOW, "Activate", "", 0, 0, g_variant_new_string(""));
    }
    if (focused && focused != obj)
        set_state(focused, BRIDGE_STATE_FOCUSED, FALSE);
    focused = obj;
    /* Announce even a repeated focus, e.g. returning from a menu. */
    obj->states &= ~BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    set_state(obj, BRIDGE_STATE_FOCUSED, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean window_gone_idle(gpointer data)
{
    char *prefix = g_strdup_printf("w%" G_GINT64_MODIFIER "x_", *(guint64 *)data);
    GHashTableIter apps_iter, iter;
    gpointer key, value;
    GPtrArray *ids;
    guint i;

    g_hash_table_iter_init(&apps_iter, apps);
    while (g_hash_table_iter_next(&apps_iter, NULL, &value))
    {
        struct app *app = value;

        /* Ids, not nodes: removing one node may free others as its subtree. */
        ids = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_iter_init(&iter, app->nodes);
        while (g_hash_table_iter_next(&iter, &key, NULL))
            if (g_str_has_prefix(key, prefix))
                g_ptr_array_add(ids, g_strdup(key));
        for (i = 0; i < ids->len; i++)
        {
            struct node *node = g_hash_table_lookup(app->nodes, ids->pdata[i]);

            if (node)
                node_remove(node);
        }
        g_ptr_array_free(ids, TRUE);
    }
    g_free(prefix);
    return G_SOURCE_REMOVE;
}

struct update
{
    guint pid;
    struct bridge_object object;
};

static gboolean update_idle(gpointer data)
{
    const struct update *u = data;
    struct app *app = g_hash_table_lookup(apps, GUINT_TO_POINTER(u->pid));
    char *id = object_id(&u->object);
    struct node *node = app ? g_hash_table_lookup(app->nodes, id) : NULL;

    if (node)
        update_node(node, &u->object);
    g_free(id);
    return G_SOURCE_REMOVE;
}

static void free_object(struct bridge_object *obj)
{
    g_free((char *)obj->name);
    g_free((char *)obj->value);
    g_free((char *)obj->window_class);
}

static void free_update(gpointer data)
{
    struct update *u = data;

    free_object(&u->object);
    g_free(u);
}

static void free_focus(gpointer data)
{
    struct bridge_focus *f = data;

    g_free((char *)f->app_name);
    free_object(&f->frame);
    free_object(&f->object);
    g_free(f);
}

static void copy_object(struct bridge_object *dst, const struct bridge_object *src)
{
    *dst = *src;
    dst->name = g_strdup(src->name);
    dst->value = g_strdup(src->value);
    dst->window_class = g_strdup(src->window_class);
}

static void post(GSourceFunc func, gpointer data, GDestroyNotify free_func)
{
    GSource *source = g_idle_source_new();

    g_source_set_callback(source, func, data, free_func);
    g_source_attach(source, context);
    g_source_unref(source);
}

void atspi_post_focus(const struct bridge_focus *focus)
{
    struct bridge_focus *f = g_new0(struct bridge_focus, 1);

    f->pid = focus->pid;
    f->app_name = g_strdup(focus->app_name);
    copy_object(&f->frame, &focus->frame);
    copy_object(&f->object, &focus->object);
    post(focus_idle, f, free_focus);
}

void atspi_post_update(unsigned int pid, const struct bridge_object *object)
{
    struct update *u = g_new0(struct update, 1);

    u->pid = pid;
    copy_object(&u->object, object);
    post(update_idle, u, free_update);
}

void atspi_post_window_gone(unsigned long long hwnd)
{
    guint64 *data = g_new(guint64, 1);

    *data = hwnd;
    post(window_gone_idle, data, g_free);
}

int atspi_init(void)
{
    GError *error = NULL;
    GDBusConnection *session;
    GVariant *ret;

    session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (session)
    {
        ret = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus",
                                          "org.a11y.Bus", "GetAddress", NULL,
                                          G_VARIANT_TYPE("(s)"),
                                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
        if (ret)
        {
            g_variant_get(ret, "(s)", &a11y_address);
            g_variant_unref(ret);
        }
        g_object_unref(session);
    }
    if (!a11y_address)
    {
        g_printerr("wine-atspi: no a11y bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    context = g_main_context_new();
    introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    apps = g_hash_table_new(g_direct_hash, g_direct_equal);
    return 0;
}

void atspi_run(void)
{
    /* All D-Bus dispatching of the per-process connections happens here. */
    g_main_context_push_thread_default(context);
    g_main_loop_run(g_main_loop_new(context, FALSE));
}
