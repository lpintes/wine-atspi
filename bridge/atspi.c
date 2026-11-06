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
 */
#include <gio/gio.h>

#include "bridge.h"

#define ATSPI_ROOT_PATH "/org/a11y/atspi/accessible/root"
#define ATSPI_ROLE_APPLICATION 75

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

struct app
{
    GDBusConnection *conn;
    char *name;
    char *parent_bus;
    char *parent_path;
    gint32 id;
};

static GVariant *self_ref(struct app *app)
{
    return g_variant_new("(so)", g_dbus_connection_get_unique_name(app->conn),
                         ATSPI_ROOT_PATH);
}

static GVariant *parent_ref(struct app *app)
{
    if (!app->parent_bus)
        return g_variant_new("(so)", "", "/org/a11y/atspi/null");
    return g_variant_new("(so)", app->parent_bus, app->parent_path);
}

static void method_call(GDBusConnection *conn, const char *sender,
                        const char *path, const char *iface,
                        const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    struct app *app = data;
    GVariant *ret = NULL;

    if (!g_strcmp0(method, "GetChildren"))
        ret = g_variant_new("(@a(so))",
                            g_variant_new_array(G_VARIANT_TYPE("(so)"), NULL, 0));
    else if (!g_strcmp0(method, "GetIndexInParent"))
        ret = g_variant_new("(i)", -1);
    else if (!g_strcmp0(method, "GetRelationSet"))
        ret = g_variant_new("(@a(ua(so)))",
                            g_variant_new_array(G_VARIANT_TYPE("(ua(so))"), NULL, 0));
    else if (!g_strcmp0(method, "GetRole"))
        ret = g_variant_new("(u)", ATSPI_ROLE_APPLICATION);
    else if (!g_strcmp0(method, "GetRoleName") ||
             !g_strcmp0(method, "GetLocalizedRoleName"))
        ret = g_variant_new("(s)", "application");
    else if (!g_strcmp0(method, "GetState"))
    {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
        g_variant_builder_add(&b, "u", 0);
        g_variant_builder_add(&b, "u", 0);
        ret = g_variant_new("(au)", &b);
    }
    else if (!g_strcmp0(method, "GetAttributes"))
        ret = g_variant_new("(@a{ss})",
                            g_variant_new_array(G_VARIANT_TYPE("{ss}"), NULL, 0));
    else if (!g_strcmp0(method, "GetApplication"))
        ret = g_variant_new("(@(so))", self_ref(app));
    else if (!g_strcmp0(method, "GetInterfaces"))
    {
        const char *ifaces[] = { "org.a11y.atspi.Accessible",
                                 "org.a11y.atspi.Application" };
        ret = g_variant_new("(@as)", g_variant_new_strv(ifaces, 2));
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
    struct app *app = data;

    if (!g_strcmp0(prop, "Name"))
        return g_variant_new_string(app->name);
    if (!g_strcmp0(prop, "Description") || !g_strcmp0(prop, "Locale") ||
        !g_strcmp0(prop, "AccessibleId"))
        return g_variant_new_string("");
    if (!g_strcmp0(prop, "Parent"))
        return parent_ref(app);
    if (!g_strcmp0(prop, "ChildCount"))
        return g_variant_new_int32(0);
    if (!g_strcmp0(prop, "ToolkitName"))
        return g_variant_new_string("wine-atspi");
    if (!g_strcmp0(prop, "Version"))
        return g_variant_new_string("0.1");
    if (!g_strcmp0(prop, "AtspiVersion"))
        return g_variant_new_string("2.1");
    if (!g_strcmp0(prop, "Id"))
        return g_variant_new_int32(app->id);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "%s not supported", prop);
    return NULL;
}

static gboolean set_property(GDBusConnection *conn, const char *sender,
                             const char *path, const char *iface,
                             const char *prop, GVariant *value,
                             GError **error, gpointer data)
{
    struct app *app = data;

    if (!g_strcmp0(prop, "Id"))
    {
        app->id = g_variant_get_int32(value);
        return TRUE;
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
                "%s is read-only", prop);
    return FALSE;
}

static const GDBusInterfaceVTable vtable = {
    method_call, get_property, set_property
};

static char *get_a11y_bus_address(GError **error)
{
    GDBusConnection *session;
    GVariant *ret;
    char *address = NULL;

    session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!session)
        return NULL;
    ret = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus",
                                      "org.a11y.Bus", "GetAddress", NULL,
                                      G_VARIANT_TYPE("(s)"),
                                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (ret)
    {
        g_variant_get(ret, "(s)", &address);
        g_variant_unref(ret);
    }
    g_object_unref(session);
    return address;
}

static gboolean register_app(struct app *app, const char *address,
                             GDBusNodeInfo *info, GError **error)
{
    GVariant *ret;
    int i;

    app->conn = g_dbus_connection_new_for_address_sync(
        address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, error);
    if (!app->conn)
        return FALSE;

    for (i = 0; info->interfaces[i]; i++)
        if (!g_dbus_connection_register_object(app->conn, ATSPI_ROOT_PATH,
                                               info->interfaces[i], &vtable,
                                               app, NULL, error))
            return FALSE;

    ret = g_dbus_connection_call_sync(
        app->conn, "org.a11y.atspi.Registry", ATSPI_ROOT_PATH,
        "org.a11y.atspi.Socket", "Embed",
        g_variant_new("((so))", g_dbus_connection_get_unique_name(app->conn),
                      ATSPI_ROOT_PATH),
        G_VARIANT_TYPE("((so))"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (!ret)
        return FALSE;
    g_variant_get(ret, "((so))", &app->parent_bus, &app->parent_path);
    g_variant_unref(ret);
    return TRUE;
}

int atspi_run(const char *app_name)
{
    GError *error = NULL;
    GDBusNodeInfo *info;
    GMainContext *context;
    GMainLoop *loop;
    struct app app = { 0 };
    char *address;

    /* A private context: this thread owns all D-Bus dispatching. */
    context = g_main_context_new();
    g_main_context_push_thread_default(context);

    address = get_a11y_bus_address(&error);
    if (!address)
    {
        g_printerr("wine-atspi: no a11y bus: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    app.name = g_strdup(app_name);
    if (!register_app(&app, address, info, &error))
    {
        g_printerr("wine-atspi: registration failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_printerr("wine-atspi: registered %s as %s\n", app.name,
               g_dbus_connection_get_unique_name(app.conn));

    loop = g_main_loop_new(context, FALSE);
    g_main_loop_run(loop);
    return 0;
}
