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
#include "textrange.h"

#define ATSPI_PREFIX "/org/a11y/atspi/accessible"
#define ATSPI_ROOT_PATH ATSPI_PREFIX "/root"
#define ATSPI_NULL_PATH "/org/a11y/atspi/null"
#define EVENT_OBJECT "org.a11y.atspi.Event.Object"
#define ATSPI_LIVE_POLITE 1
#define XK_SPACE 0x20
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
    " <interface name='org.a11y.atspi.Text'>"
    "  <property name='CharacterCount' type='i' access='read'/>"
    "  <property name='CaretOffset' type='i' access='read'/>"
    "  <method name='GetStringAtOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='granularity' type='u'/>"
    "   <arg direction='out' type='s'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetText'>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='SetCaretOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='GetTextBeforeOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='type' type='u'/>"
    "   <arg direction='out' type='s'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetTextAtOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='type' type='u'/>"
    "   <arg direction='out' type='s'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetTextAfterOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='type' type='u'/>"
    "   <arg direction='out' type='s'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetCharacterAtOffset'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetAttributeValue'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='attributeName' type='s'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetAttributes'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='out' type='a{ss}'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetDefaultAttributes'><arg direction='out' type='a{ss}'/></method>"
    "  <method name='GetCharacterExtents'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='coordType' type='u'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetOffsetAtPoint'>"
    "   <arg direction='in' name='x' type='i'/>"
    "   <arg direction='in' name='y' type='i'/>"
    "   <arg direction='in' name='coordType' type='u'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetNSelections'><arg direction='out' type='i'/></method>"
    "  <method name='GetSelection'>"
    "   <arg direction='in' name='selectionNum' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='AddSelection'>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='RemoveSelection'>"
    "   <arg direction='in' name='selectionNum' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='SetSelection'>"
    "   <arg direction='in' name='selectionNum' type='i'/>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='GetRangeExtents'>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='in' name='coordType' type='u'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetAttributeRun'>"
    "   <arg direction='in' name='offset' type='i'/>"
    "   <arg direction='in' name='includeDefaults' type='b'/>"
    "   <arg direction='out' type='a{ss}'/>"
    "   <arg direction='out' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetDefaultAttributeSet'><arg direction='out' type='as'/></method>"
    "  <method name='ScrollSubstringTo'>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='in' name='type' type='u'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='ScrollSubstringToPoint'>"
    "   <arg direction='in' name='startOffset' type='i'/>"
    "   <arg direction='in' name='endOffset' type='i'/>"
    "   <arg direction='in' name='type' type='u'/>"
    "   <arg direction='in' name='x' type='i'/>"
    "   <arg direction='in' name='y' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Table'>"
    "  <property name='NRows' type='i' access='read'/>"
    "  <property name='NColumns' type='i' access='read'/>"
    "  <property name='Caption' type='(so)' access='read'/>"
    "  <property name='Summary' type='(so)' access='read'/>"
    "  <property name='NSelectedRows' type='i' access='read'/>"
    "  <property name='NSelectedColumns' type='i' access='read'/>"
    "  <method name='GetAccessibleAt'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='GetIndexAt'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetRowAtIndex'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetColumnAtIndex'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetRowDescription'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetColumnDescription'>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetRowExtentAt'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetColumnExtentAt'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='i'/>"
    "  </method>"
    "  <method name='GetRowHeader'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='GetColumnHeader'>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='GetSelectedRows'><arg direction='out' type='ai'/></method>"
    "  <method name='GetSelectedColumns'><arg direction='out' type='ai'/></method>"
    "  <method name='IsRowSelected'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='IsColumnSelected'>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='IsSelected'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='AddRowSelection'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='AddColumnSelection'>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='RemoveRowSelection'>"
    "   <arg direction='in' name='row' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='RemoveColumnSelection'>"
    "   <arg direction='in' name='column' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='GetRowColumnExtentsAtIndex'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "   <arg direction='out' name='row' type='i'/>"
    "   <arg direction='out' name='col' type='i'/>"
    "   <arg direction='out' name='row_extents' type='i'/>"
    "   <arg direction='out' name='col_extents' type='i'/>"
    "   <arg direction='out' name='is_selected' type='b'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.a11y.atspi.TableCell'>"
    "  <property name='ColumnSpan' type='i' access='read'/>"
    "  <property name='Position' type='(ii)' access='read'/>"
    "  <property name='RowSpan' type='i' access='read'/>"
    "  <property name='Table' type='(so)' access='read'/>"
    "  <method name='GetRowColumnSpan'>"
    "   <arg direction='out' type='b'/>"
    "   <arg direction='out' name='row' type='i'/>"
    "   <arg direction='out' name='col' type='i'/>"
    "   <arg direction='out' name='row_extents' type='i'/>"
    "   <arg direction='out' name='col_extents' type='i'/>"
    "  </method>"
    "  <method name='GetColumnHeaderCells'><arg direction='out' type='a(so)'/></method>"
    "  <method name='GetRowHeaderCells'><arg direction='out' type='a(so)'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Value'>"
    "  <property name='MinimumValue' type='d' access='read'/>"
    "  <property name='MaximumValue' type='d' access='read'/>"
    "  <property name='MinimumIncrement' type='d' access='read'/>"
    "  <property name='CurrentValue' type='d' access='readwrite'/>"
    "  <property name='Text' type='s' access='read'/>"
    " </interface>"
    " <interface name='org.a11y.atspi.Component'>"
    "  <method name='Contains'>"
    "   <arg direction='in' name='x' type='i'/>"
    "   <arg direction='in' name='y' type='i'/>"
    "   <arg direction='in' name='coord_type' type='u'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='GetAccessibleAtPoint'>"
    "   <arg direction='in' name='x' type='i'/>"
    "   <arg direction='in' name='y' type='i'/>"
    "   <arg direction='in' name='coord_type' type='u'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='GetExtents'>"
    "   <arg direction='in' name='coord_type' type='u'/>"
    "   <arg direction='out' type='(iiii)'/>"
    "  </method>"
    "  <method name='GetPosition'>"
    "   <arg direction='in' name='coord_type' type='u'/>"
    "   <arg direction='out' name='x' type='i'/>"
    "   <arg direction='out' name='y' type='i'/>"
    "  </method>"
    "  <method name='GetSize'>"
    "   <arg direction='out' name='width' type='i'/>"
    "   <arg direction='out' name='height' type='i'/>"
    "  </method>"
    "  <method name='GetLayer'><arg direction='out' type='u'/></method>"
    "  <method name='GetMDIZOrder'><arg direction='out' type='n'/></method>"
    "  <method name='GrabFocus'><arg direction='out' type='b'/></method>"
    "  <method name='GetAlpha'><arg direction='out' type='d'/></method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Action'>"
    "  <property name='NActions' type='i' access='read'/>"
    "  <method name='GetDescription'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetName'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetLocalizedName'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetKeyBinding'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='s'/>"
    "  </method>"
    "  <method name='GetActions'><arg direction='out' type='a(sss)'/></method>"
    "  <method name='DoAction'>"
    "   <arg direction='in' name='index' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.a11y.atspi.Selection'>"
    "  <property name='NSelectedChildren' type='i' access='read'/>"
    "  <method name='GetSelectedChild'>"
    "   <arg direction='in' name='selectedChildIndex' type='i'/>"
    "   <arg direction='out' type='(so)'/>"
    "  </method>"
    "  <method name='SelectChild'>"
    "   <arg direction='in' name='childIndex' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='DeselectSelectedChild'>"
    "   <arg direction='in' name='selectedChildIndex' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='IsChildSelected'>"
    "   <arg direction='in' name='childIndex' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    "  <method name='SelectAll'><arg direction='out' type='b'/></method>"
    "  <method name='ClearSelection'><arg direction='out' type='b'/></method>"
    "  <method name='DeselectChild'>"
    "   <arg direction='in' name='childIndex' type='i'/>"
    "   <arg direction='out' type='b'/>"
    "  </method>"
    " </interface>"
    "</node>";

#define IFACE_ACCESSIBLE "org.a11y.atspi.Accessible"
#define IFACE_APPLICATION "org.a11y.atspi.Application"
#define IFACE_TEXT "org.a11y.atspi.Text"
#define IFACE_TABLE "org.a11y.atspi.Table"
#define IFACE_TABLE_CELL "org.a11y.atspi.TableCell"
#define IFACE_VALUE "org.a11y.atspi.Value"
#define IFACE_COMPONENT "org.a11y.atspi.Component"
#define IFACE_ACTION "org.a11y.atspi.Action"
#define IFACE_SELECTION "org.a11y.atspi.Selection"
#define ACTION_CLICK "click"

/* AtspiCoordType */
enum { COORD_SCREEN, COORD_WINDOW, COORD_PARENT };
/* AtspiComponentLayer */
enum { LAYER_WIDGET = 3, LAYER_WINDOW = 7 };

/* AtspiTextBoundaryType */
enum { BOUNDARY_CHAR, BOUNDARY_WORD_START, BOUNDARY_WORD_END, BOUNDARY_SENTENCE_START,
       BOUNDARY_SENTENCE_END, BOUNDARY_LINE_START, BOUNDARY_LINE_END };

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
    char *shortcut;         /* accelerator of a menu item, or NULL */
    guint64 hwnd;           /* window of the object, 0 for the root and placeholders */
    /* Text interface: text and its chars are NULL for nodes without one. */
    struct bridge_text *text;
    const gunichar *chars;
    glong count;
    gint caret;
    gint selection_start;
    gint selection_end;
    gint position;          /* from 1, 0 if not a list item */
    gint set_size;          /* items of a list, rows of a table */
    gint level;             /* of a tree item, from 1; 0 otherwise */
    gboolean placeholder;   /* list item not described yet, see item_at */
    /* Table: columns and their header texts. Cell or header: its place, from 0. */
    gint column_count;
    char **headers;
    gint row;
    gint column;
    /* Table or list box: selected rows or items, the first selected_listed known. */
    gint selected_count;
    gint selected_listed;
    gint *selected_rows;
    /* Value interface of a slider, progress bar or spin button, if has_range. */
    gboolean has_range;
    gdouble minimum;
    gdouble maximum;
    gdouble current;
    gdouble increment;
    /* Component interface: screen rectangle, if has_extents. */
    gboolean has_extents;
    gint x;
    gint y;
    gint width;
    gint height;
    /* Posted by the last window tree of its frame, at tree_index; see tree_idle. */
    gboolean in_tree;
    guint tree_index;
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
    guint subtree_id;
};

static GMainContext *context;
static char *a11y_address;
static GDBusNodeInfo *introspection;
static GHashTable *apps;            /* pid -> struct app * */
static struct node *active_frame;
static struct node *focused;
static guint last_keysym;           /* last key pressed, see key_idle */

static const char *role_name(guint32 role)
{
    switch (role)
    {
    case BRIDGE_ROLE_CHECK_BOX: return "check box";
    case BRIDGE_ROLE_CHECK_MENU_ITEM: return "check menu item";
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
    case BRIDGE_ROLE_RADIO_MENU_ITEM: return "radio menu item";
    case BRIDGE_ROLE_SCROLL_BAR: return "scroll bar";
    case BRIDGE_ROLE_SEPARATOR: return "separator";
    case BRIDGE_ROLE_SLIDER: return "slider";
    case BRIDGE_ROLE_SPIN_BUTTON: return "spin button";
    case BRIDGE_ROLE_STATUS_BAR: return "status bar";
    case BRIDGE_ROLE_TABLE: return "table";
    case BRIDGE_ROLE_TABLE_CELL: return "table cell";
    case BRIDGE_ROLE_TERMINAL: return "terminal";
    case BRIDGE_ROLE_TEXT: return "text";
    case BRIDGE_ROLE_TOGGLE_BUTTON: return "toggle button";
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
    case BRIDGE_STATE_CHECKABLE: return "checkable";
    case BRIDGE_STATE_CHECKED: return "checked";
    case BRIDGE_STATE_COLLAPSED: return "collapsed";
    case BRIDGE_STATE_EDITABLE: return "editable";
    case BRIDGE_STATE_ENABLED: return "enabled";
    case BRIDGE_STATE_EXPANDABLE: return "expandable";
    case BRIDGE_STATE_EXPANDED: return "expanded";
    case BRIDGE_STATE_FOCUSED: return "focused";
    case BRIDGE_STATE_HORIZONTAL: return "horizontal";
    case BRIDGE_STATE_INDETERMINATE: return "indeterminate";
    case BRIDGE_STATE_PRESSED: return "pressed";
    case BRIDGE_STATE_READ_ONLY: return "read-only";
    case BRIDGE_STATE_SELECTED: return "selected";
    case BRIDGE_STATE_SENSITIVE: return "sensitive";
    case BRIDGE_STATE_VERTICAL: return "vertical";
    }
    return NULL;
}

/* UTF-8 copy of characters [start, end) of node's text, clamped. */
static char *text_slice(struct node *node, gint start, gint end)
{
    if (end < 0 || end > node->count)
        end = node->count;
    if (start < 0)
        start = 0;
    if (start >= end)
        return g_strdup("");
    return g_ucs4_to_utf8(node->chars + start, end - start, NULL, NULL, NULL);
}

static void node_text_range(struct node *node, gint offset, guint granularity,
                            gint *start, gint *end)
{
    /* A wrapping text control knows where its lines break. */
    if (granularity == GRANULARITY_LINE && node->hwnd && !node->placeholder &&
        bridge_text_line(node->hwnd, node->chars, node->count, offset, start, end))
        return;
    text_range(node->chars, node->count, offset, granularity, start, end);
}

static guint boundary_granularity(guint boundary)
{
    switch (boundary)
    {
    case BOUNDARY_CHAR: return GRANULARITY_CHAR;
    case BOUNDARY_WORD_START:
    case BOUNDARY_WORD_END: return GRANULARITY_WORD;
    }
    return GRANULARITY_LINE;
}

/* GetText{Before,At,After}Offset: which -1, 0 or 1. */
static GVariant *text_near_offset(struct node *node, gint offset, guint boundary, int which)
{
    guint granularity = boundary_granularity(boundary);
    gint start, end;
    GVariant *ret;
    char *s;

    node_text_range(node, offset, granularity, &start, &end);
    if (which < 0)
    {
        if (start > 0)
            node_text_range(node, start - 1, granularity, &start, &end);
        else
            start = end = 0;
    }
    else if (which > 0)
    {
        if (end < node->count && end > start)
            node_text_range(node, end, granularity, &start, &end);
        else
            start = end = node->count;
    }
    s = text_slice(node, start, end);
    ret = g_variant_new("(sii)", s, start, end);
    g_free(s);
    return ret;
}

static GVariant *empty_attributes(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("{ss}"), NULL, 0);
}

static void coord_origin(struct node *node, guint coord_type, gint *x, gint *y);

/* Handles a Text method; NULL if method is none. */
static GVariant *text_method(struct node *node, const char *method, GVariant *params)
{
    gint offset, start, end;
    guint type;
    GVariant *ret;
    char *s;

    if (!g_strcmp0(method, "GetStringAtOffset"))
    {
        g_variant_get(params, "(iu)", &offset, &type);
        if (offset < 0 || offset > node->count)
            return g_variant_new("(sii)", "", -1, -1);
        node_text_range(node, offset, type, &start, &end);
        s = text_slice(node, start, end);
        ret = g_variant_new("(sii)", s, start, end);
        g_free(s);
        return ret;
    }
    if (!g_strcmp0(method, "GetText"))
    {
        g_variant_get(params, "(ii)", &start, &end);
        s = text_slice(node, start, end);
        ret = g_variant_new("(s)", s);
        g_free(s);
        return ret;
    }
    if (!g_strcmp0(method, "GetTextBeforeOffset"))
    {
        g_variant_get(params, "(iu)", &offset, &type);
        return text_near_offset(node, offset, type, -1);
    }
    if (!g_strcmp0(method, "GetTextAtOffset"))
    {
        g_variant_get(params, "(iu)", &offset, &type);
        return text_near_offset(node, offset, type, 0);
    }
    if (!g_strcmp0(method, "GetTextAfterOffset"))
    {
        g_variant_get(params, "(iu)", &offset, &type);
        return text_near_offset(node, offset, type, 1);
    }
    if (!g_strcmp0(method, "GetCharacterAtOffset"))
    {
        g_variant_get(params, "(i)", &offset);
        return g_variant_new("(i)", offset >= 0 && offset < node->count
                                        ? (gint32)node->chars[offset] : 0);
    }
    if (!g_strcmp0(method, "GetNSelections"))
        return g_variant_new("(i)", node->selection_start != node->selection_end);
    if (!g_strcmp0(method, "GetSelection"))
    {
        g_variant_get(params, "(i)", &offset);
        if (offset == 0 && node->selection_start != node->selection_end)
            return g_variant_new("(ii)", node->selection_start, node->selection_end);
        return g_variant_new("(ii)", 0, 0);
    }
    if (!g_strcmp0(method, "GetAttributes") || !g_strcmp0(method, "GetAttributeRun"))
        return g_variant_new("(@a{ss}ii)", empty_attributes(), 0, (gint)node->count);
    if (!g_strcmp0(method, "GetDefaultAttributes"))
        return g_variant_new("(@a{ss})", empty_attributes());
    if (!g_strcmp0(method, "GetDefaultAttributeSet"))
        return g_variant_new("(@as)", g_variant_new_strv(NULL, 0));
    if (!g_strcmp0(method, "GetAttributeValue"))
        return g_variant_new("(s)", "");
    if (!g_strcmp0(method, "GetCharacterExtents") || !g_strcmp0(method, "GetRangeExtents"))
    {
        gint rect[4] = { 0, 0, 0, 0 }, ox, oy;
        guint coord_type;

        if (!g_strcmp0(method, "GetCharacterExtents"))
        {
            g_variant_get(params, "(iu)", &start, &coord_type);
            end = start + 1;
        }
        else
            g_variant_get(params, "(iiu)", &start, &end, &coord_type);
        if (!bridge_text_extents(node->hwnd, node->chars, node->count, start, end, rect))
            return g_variant_new("(iiii)", 0, 0, 0, 0);
        coord_origin(node, coord_type, &ox, &oy);
        return g_variant_new("(iiii)", rect[0] - ox, rect[1] - oy, rect[2], rect[3]);
    }
    if (!g_strcmp0(method, "GetOffsetAtPoint"))
        return g_variant_new("(i)", -1);
    /*
     * A Win32 text control has one selection, so like a GTK 3 entry there
     * is at most selection 0. The new caret comes back with the next read
     * of the focus, not here.
     */
    if (!g_strcmp0(method, "SetCaretOffset"))
    {
        g_variant_get(params, "(i)", &offset);
        return g_variant_new("(b)", bridge_text_select(node->hwnd, node->chars, node->count,
                                                       offset, offset));
    }
    if (!g_strcmp0(method, "AddSelection"))
    {
        g_variant_get(params, "(ii)", &start, &end);
        return g_variant_new("(b)", node->selection_start == node->selection_end &&
                                        bridge_text_select(node->hwnd, node->chars,
                                                           node->count, start, end));
    }
    if (!g_strcmp0(method, "SetSelection"))
    {
        g_variant_get(params, "(iii)", &offset, &start, &end);
        return g_variant_new("(b)", offset == 0 && bridge_text_select(node->hwnd, node->chars,
                                                                      node->count, start, end));
    }
    if (!g_strcmp0(method, "RemoveSelection"))
    {
        g_variant_get(params, "(i)", &offset);
        return g_variant_new("(b)", offset == 0 &&
                                        node->selection_start != node->selection_end &&
                                        bridge_text_select(node->hwnd, node->chars, node->count,
                                                           node->caret, node->caret));
    }
    if (!g_strcmp0(method, "ScrollSubstringTo") || !g_strcmp0(method, "ScrollSubstringToPoint"))
        return g_variant_new("(b)", FALSE);
    return NULL;
}

/*
 * Interfaces of node, the first always Accessible. The array is valid
 * until the next call; only the AT-SPI thread calls this.
 */
/*
 * A Win32 button (push button, check box, radio button) has the one action
 * "click", so that Orca's object navigator and flat review can activate it
 * without the mouse (try_all_clickable_actions).
 */
static gboolean node_has_click(struct node *node)
{
    return node->hwnd && !node->placeholder && node->window_class &&
           !g_ascii_strcasecmp(node->window_class, "Button") &&
           (node->role == BRIDGE_ROLE_PUSH_BUTTON || node->role == BRIDGE_ROLE_CHECK_BOX ||
            node->role == BRIDGE_ROLE_RADIO_BUTTON);
}

static GVariant *action_method(struct node *node, const char *method, GVariant *params)
{
    gint index = -1;

    if (!g_strcmp0(method, "GetActions"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a(sss)"));
        g_variant_builder_add(&b, "(sss)", ACTION_CLICK, "", "");
        return g_variant_new("(a(sss))", &b);
    }
    if (g_variant_is_of_type(params, G_VARIANT_TYPE("(i)")))
        g_variant_get(params, "(i)", &index);
    if (!g_strcmp0(method, "GetName") || !g_strcmp0(method, "GetLocalizedName"))
        return g_variant_new("(s)", index == 0 ? ACTION_CLICK : "");
    if (!g_strcmp0(method, "GetDescription") || !g_strcmp0(method, "GetKeyBinding"))
        return g_variant_new("(s)", "");
    if (!g_strcmp0(method, "DoAction"))
        return g_variant_new("(b)", index == 0 && bridge_click(node->hwnd));
    return NULL;
}

static const char *const *node_interfaces(struct node *node, guint *count)
{
    static const char *ifaces[6];

    *count = 0;
    ifaces[(*count)++] = IFACE_ACCESSIBLE;
    if (!node->parent)
    {
        ifaces[(*count)++] = IFACE_APPLICATION;
        return ifaces;
    }
    /* A spin button has both, as in GTK. */
    if (node->has_range)
        ifaces[(*count)++] = IFACE_VALUE;
    if (node->role == BRIDGE_ROLE_TABLE)
    {
        ifaces[(*count)++] = IFACE_TABLE;
        ifaces[(*count)++] = IFACE_SELECTION;
    }
    else if (node->role == BRIDGE_ROLE_LIST_BOX || node->role == BRIDGE_ROLE_TREE)
        ifaces[(*count)++] = IFACE_SELECTION;
    else if (node->role == BRIDGE_ROLE_TABLE_CELL)
        ifaces[(*count)++] = IFACE_TABLE_CELL;
    else if (node->chars)
        ifaces[(*count)++] = IFACE_TEXT;
    if (node->has_extents)
        ifaces[(*count)++] = IFACE_COMPONENT;
    if (node_has_click(node))
        ifaces[(*count)++] = IFACE_ACTION;
    return ifaces;
}

static gboolean node_has_interface(struct node *node, const char *iface)
{
    const char *const *ifaces;
    guint i, count;

    ifaces = node_interfaces(node, &count);
    for (i = 0; i < count; i++)
        if (!g_strcmp0(ifaces[i], iface))
            return TRUE;
    return FALSE;
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

static gboolean is_item_list(struct node *node);
static struct node *child_at(struct node *node, guint index);

static GVariant *null_ref(void)
{
    return g_variant_new("(so)", "", ATSPI_NULL_PATH);
}

/*
 * A table's children are its column headers, then its cells row by row,
 * as in GTK 3; Orca finds a cell's coordinates from its index in parent
 * (ax_table.py, _get_cell_index). -1 for a cell outside the table.
 */
static gint cell_index(struct node *table, gint row, gint column)
{
    gint index = (row + 1) * table->column_count + column;

    if (row < 0 || column < 0 || row >= table->set_size || column >= table->column_count ||
        index >= (gint)table->children->len)
        return -1;
    return index;
}

/*
 * Child index of row (a table's row, a list box's item) as it is
 * selected: a table's first cell of the row, a list box's item. -1 if
 * there is none.
 */
static gint selection_index(struct node *list, gint row)
{
    if (list->role == BRIDGE_ROLE_TABLE)
        return cell_index(list, row, 0);
    return row >= 0 && row < (gint)list->children->len ? row : -1;
}

/*
 * The Win32 side lists the first selected rows or items; beyond them a
 * row is selected if its cell says so, which is known only for rows that
 * were described.
 */
static gboolean row_selected(struct node *list, gint row)
{
    gint index = selection_index(list, row), i;
    struct node *cell = index >= 0 ? list->children->pdata[index] : NULL;

    if (index < 0)
        return FALSE;
    for (i = 0; i < list->selected_listed; i++)
        if (list->selected_rows[i] == row)
            return TRUE;
    if (list->selected_listed == list->selected_count)
        return FALSE;
    return cell && (cell->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED));
}

/*
 * A tree's children are the items in view plus a focused item scrolled
 * away (tree_idle), each with its own selected state. Returns the
 * index-th selected child (index -1: none) and sets *count to the number
 * of selected children.
 */
static struct node *tree_selected_child(struct node *tree, gint index, gint *count)
{
    struct node *found = NULL;
    guint i;

    *count = 0;
    for (i = 0; i < tree->children->len; i++)
    {
        struct node *child = tree->children->pdata[i];

        if (!child || !(child->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED)))
            continue;
        if ((*count)++ == index)
            found = child;
    }
    return found;
}

/*
 * Selection of a table, as GTK 3's tree view gives it: the selected
 * children are the first cells of the selected rows. Orca's flat review
 * reads only the visible cells of a table with Table and Selection
 * (ax_utilities.py, _get_on_screen_objects). A list box's selected
 * children are its selected items; Where Am I selection (Orca+Shift+Up)
 * needs a container with Selection (get_selection_container in
 * ax_utilities_selection.py), else it speaks the selected text. A tree's
 * selected children are its selected items in view; Orca counts the
 * selectable ones from its tree item descendants
 * (selectable_child_count), which are only the items in view.
 * Changing the selection is not supported.
 */
static GVariant *selection_method(struct node *node, const char *method, GVariant *params)
{
    gint index = -1, n = node->column_count, count;

    if (g_variant_is_of_type(params, G_VARIANT_TYPE("(i)")))
        g_variant_get(params, "(i)", &index);
    if (node->role == BRIDGE_ROLE_TREE && !g_strcmp0(method, "GetSelectedChild"))
    {
        struct node *child = tree_selected_child(node, index, &count);

        return g_variant_new("(@(so))", child ? node_ref(child) : null_ref());
    }
    if (node->role == BRIDGE_ROLE_TREE && !g_strcmp0(method, "IsChildSelected"))
    {
        struct node *child = index >= 0 && index < (gint)node->children->len ?
                             node->children->pdata[index] : NULL;

        return g_variant_new("(b)", child &&
                                    (child->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED)));
    }
    if (!g_strcmp0(method, "GetSelectedChild"))
    {
        gint child = index >= 0 && index < node->selected_listed ?
                     selection_index(node, node->selected_rows[index]) : -1;

        return g_variant_new("(@(so))", child >= 0 ? node_ref(child_at(node, child)) : null_ref());
    }
    if (!g_strcmp0(method, "IsChildSelected"))
    {
        if (node->role != BRIDGE_ROLE_TABLE)
            return g_variant_new("(b)", row_selected(node, index));
        return g_variant_new("(b)", n && index >= n && index < (gint)node->children->len &&
                                    row_selected(node, index / n - 1));
    }
    if (!g_strcmp0(method, "SelectChild") || !g_strcmp0(method, "DeselectSelectedChild") ||
        !g_strcmp0(method, "SelectAll") || !g_strcmp0(method, "ClearSelection") ||
        !g_strcmp0(method, "DeselectChild"))
        return g_variant_new("(b)", FALSE);
    return NULL;
}

static GVariant *table_method(struct node *node, const char *method, GVariant *params)
{
    gint n = node->column_count, len = node->children->len, a = -1, b = -1, index;

    /* Arguments are a row, a column or an index, or a row and a column. */
    if (g_variant_is_of_type(params, G_VARIANT_TYPE("(ii)")))
        g_variant_get(params, "(ii)", &a, &b);
    else if (g_variant_is_of_type(params, G_VARIANT_TYPE("(i)")))
        g_variant_get(params, "(i)", &a);

    if (!g_strcmp0(method, "GetAccessibleAt"))
    {
        index = cell_index(node, a, b);
        return g_variant_new("(@(so))", index >= 0 ? node_ref(child_at(node, index)) : null_ref());
    }
    if (!g_strcmp0(method, "GetIndexAt"))
        return g_variant_new("(i)", cell_index(node, a, b));
    if (!g_strcmp0(method, "GetRowAtIndex"))
        return g_variant_new("(i)", n && a >= n && a < len ? a / n - 1 : -1);
    if (!g_strcmp0(method, "GetColumnAtIndex"))
        return g_variant_new("(i)", n && a >= n && a < len ? a % n : -1);
    if (!g_strcmp0(method, "GetRowDescription"))
        return g_variant_new("(s)", "");
    if (!g_strcmp0(method, "GetColumnDescription"))
        return g_variant_new("(s)", a >= 0 && a < n && node->headers ? node->headers[a] : "");
    if (!g_strcmp0(method, "GetRowExtentAt") || !g_strcmp0(method, "GetColumnExtentAt"))
        return g_variant_new("(i)", cell_index(node, a, b) >= 0 ? 1 : 0);
    if (!g_strcmp0(method, "GetRowHeader"))
        return g_variant_new("(@(so))", null_ref());
    if (!g_strcmp0(method, "GetColumnHeader"))
        return g_variant_new("(@(so))", a >= 0 && a < n && a < len ?
                                        node_ref(child_at(node, a)) : null_ref());
    if (!g_strcmp0(method, "GetSelectedRows"))
    {
        GVariantBuilder rows;
        gint i;

        g_variant_builder_init(&rows, G_VARIANT_TYPE("ai"));
        for (i = 0; i < node->selected_listed; i++)
            g_variant_builder_add(&rows, "i", node->selected_rows[i]);
        return g_variant_new("(ai)", &rows);
    }
    if (!g_strcmp0(method, "GetSelectedColumns"))
        return g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0));
    if (!g_strcmp0(method, "IsRowSelected"))
        return g_variant_new("(b)", row_selected(node, a));
    if (!g_strcmp0(method, "IsSelected"))
        return g_variant_new("(b)", cell_index(node, a, b) >= 0 && row_selected(node, a));
    if (!g_strcmp0(method, "IsColumnSelected") || !g_strcmp0(method, "AddRowSelection") ||
        !g_strcmp0(method, "AddColumnSelection") || !g_strcmp0(method, "RemoveRowSelection") ||
        !g_strcmp0(method, "RemoveColumnSelection"))
        return g_variant_new("(b)", FALSE);
    if (!g_strcmp0(method, "GetRowColumnExtentsAtIndex"))
    {
        if (!n || a < n || a >= len)
            return g_variant_new("(biiiib)", FALSE, -1, -1, 0, 0, FALSE);
        return g_variant_new("(biiiib)", TRUE, a / n - 1, a % n, 1, 1,
                             row_selected(node, a / n - 1));
    }
    return NULL;
}

static GVariant *table_cell_method(struct node *node, const char *method)
{
    struct node *table = node->parent;

    if (!g_strcmp0(method, "GetRowColumnSpan"))
        return g_variant_new("(biiii)", TRUE, node->row, node->column, 1, 1);
    if (!g_strcmp0(method, "GetColumnHeaderCells"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
        if (table && table->role == BRIDGE_ROLE_TABLE && node->column < table->column_count &&
            node->column < (gint)table->children->len)
            g_variant_builder_add_value(&b, node_ref(child_at(table, node->column)));
        return g_variant_new("(a(so))", &b);
    }
    if (!g_strcmp0(method, "GetRowHeaderCells"))
        return g_variant_new("(@a(so))", g_variant_new_array(G_VARIANT_TYPE("(so)"), NULL, 0));
    return NULL;
}

/*
 * Origin of coord_type in screen coordinates: the screen, the top-level
 * window (child of the application) or the parent.
 */
static void coord_origin(struct node *node, guint coord_type, gint *x, gint *y)
{
    struct node *origin = NULL;

    *x = *y = 0;
    if (coord_type == COORD_PARENT)
        origin = node->parent;
    else if (coord_type == COORD_WINDOW)
        for (origin = node; origin->parent && origin->parent->parent; origin = origin->parent)
            ;
    if (origin && origin->has_extents)
    {
        *x = origin->x;
        *y = origin->y;
    }
}

static gboolean rect_contains(struct node *node, gint x, gint y)
{
    return node->has_extents && x >= node->x && x < node->x + node->width &&
           y >= node->y && y < node->y + node->height;
}

/* Distance of the point x, y (screen) from node's rectangle, 0 inside it. */
static gint rect_distance(struct node *node, gint x, gint y)
{
    gint dx = MAX(MAX(node->x - x, x - (node->x + node->width - 1)), 0);
    gint dy = MAX(MAX(node->y - y, y - (node->y + node->height - 1)), 0);

    return dx + dy;
}

/*
 * A child on screen, for hit testing; no placeholders are made. The other
 * cells of a table row stay placeholders but get their rectangles from
 * set_row.
 */
static gboolean is_hit_candidate(struct node *child)
{
    return child && child->has_extents &&
           (child->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING));
}

/*
 * The cell of a table at x, y (screen). Orca finds a table's visible
 * cells by the cells at its top left and bottom right corners
 * (ax_utilities_table.py, _get_visible_cell_range), which lie in the
 * column headers or below the last row, so a point elsewhere in the table
 * gives the nearest visible cell; a header would end the search. A cell
 * counts only if its row's first cell is on screen: the other cells of a
 * row scrolled away keep their old rectangles (set_row).
 */
static struct node *table_cell_at_point(struct node *table, gint x, gint y)
{
    struct node *best = NULL;
    gint n = table->column_count, best_distance = G_MAXINT, i;

    if (!n || !rect_contains(table, x, y))
        return NULL;
    for (i = n; i < (gint)table->children->len; i++)
    {
        struct node *cell = table->children->pdata[i], *first = table->children->pdata[i - i % n];
        gint distance;

        if (!is_hit_candidate(cell) || !is_hit_candidate(first) || first->placeholder)
            continue;
        if ((distance = rect_distance(cell, x, y)) < best_distance)
        {
            best = cell;
            best_distance = distance;
        }
        if (!distance)
            break;
    }
    return best;
}

static struct node *child_at_point(struct node *node, gint x, gint y)
{
    guint i;

    if (node->role == BRIDGE_ROLE_TABLE)
        return table_cell_at_point(node, x, y);
    for (i = 0; i < node->children->len; i++)
    {
        struct node *child = node->children->pdata[i];

        if (is_hit_candidate(child) && rect_contains(child, x, y))
            return child;
    }
    return NULL;
}

static GVariant *component_method(struct node *node, const char *method, GVariant *params)
{
    gint x, y, ox, oy;
    guint coord_type;

    if (!g_strcmp0(method, "GetExtents"))
    {
        g_variant_get(params, "(u)", &coord_type);
        coord_origin(node, coord_type, &ox, &oy);
        return g_variant_new("((iiii))", node->x - ox, node->y - oy, node->width, node->height);
    }
    if (!g_strcmp0(method, "GetPosition"))
    {
        g_variant_get(params, "(u)", &coord_type);
        coord_origin(node, coord_type, &ox, &oy);
        return g_variant_new("(ii)", node->x - ox, node->y - oy);
    }
    if (!g_strcmp0(method, "GetSize"))
        return g_variant_new("(ii)", node->width, node->height);
    if (!g_strcmp0(method, "Contains"))
    {
        g_variant_get(params, "(iiu)", &x, &y, &coord_type);
        coord_origin(node, coord_type, &ox, &oy);
        x += ox;
        y += oy;
        return g_variant_new("(b)", x >= node->x && x < node->x + node->width &&
                                    y >= node->y && y < node->y + node->height);
    }
    if (!g_strcmp0(method, "GetAccessibleAtPoint"))
    {
        struct node *child;

        g_variant_get(params, "(iiu)", &x, &y, &coord_type);
        coord_origin(node, coord_type, &ox, &oy);
        child = child_at_point(node, x + ox, y + oy);
        return g_variant_new("(@(so))", child ? node_ref(child) : null_ref());
    }
    if (!g_strcmp0(method, "GetLayer"))
        return g_variant_new("(u)", node->parent && !node->parent->parent ? LAYER_WINDOW
                                                                          : LAYER_WIDGET);
    if (!g_strcmp0(method, "GetMDIZOrder"))
        return g_variant_new("(n)", (gint16)0);
    if (!g_strcmp0(method, "GrabFocus"))
        return g_variant_new("(b)", FALSE);
    if (!g_strcmp0(method, "GetAlpha"))
        return g_variant_new("(d)", 1.0);
    return NULL;
}

/*
 * The node of a call, looked up by path at call time: GDBus runs the
 * vtable from an idle source scheduled after subtree_dispatch, and the
 * node may be freed in between. data is the app, which GDBus keeps
 * alive while a call is pending (app_free is the subtree's destructor).
 */
static struct node *call_node(gpointer data, const char *path)
{
    struct app *app = data;

    /*
     * Parenthesized: with a constant prefix the g_str_has_prefix macro
     * calls strlen and strncmp, which bind to Wine's ms_abi msvcrt.
     */
    if (!(g_str_has_prefix)(path, ATSPI_PREFIX "/"))
        return NULL;
    return g_hash_table_lookup(app->nodes, path + sizeof(ATSPI_PREFIX));
}

static void method_call(GDBusConnection *conn, const char *sender,
                        const char *path, const char *iface,
                        const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    struct node *node = call_node(data, path);
    GVariant *ret = NULL;
    guint i;

    if (!node)
    {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_OBJECT,
                                              "%s no longer exists", path);
        return;
    }
    if (!g_strcmp0(iface, IFACE_TEXT))
        ret = text_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_TABLE))
        ret = table_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_TABLE_CELL))
        ret = table_cell_method(node, method);
    else if (!g_strcmp0(iface, IFACE_COMPONENT))
        ret = component_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_ACTION))
        ret = action_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_SELECTION))
        ret = selection_method(node, method, params);
    else if (!g_strcmp0(method, "GetChildAtIndex"))
    {
        gint index;

        g_variant_get(params, "(i)", &index);
        if (index >= 0 && index < (gint)node->children->len)
            ret = g_variant_new("(@(so))", node_ref(child_at(node, index)));
        else
            ret = g_variant_new("((so))", "", ATSPI_NULL_PATH);
    }
    else if (!g_strcmp0(method, "GetChildren"))
    {
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
        for (i = 0; i < node->children->len; i++)
            g_variant_builder_add_value(&b, node_ref(child_at(node, i)));
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
        /* Orca's accelerator of a menu item (ax_utilities_action.py). */
        if (node->shortcut)
            g_variant_builder_add(&b, "{ss}", "keyshortcuts", node->shortcut);
        /*
         * Orca says "x of y" for an item from these (ax_utilities.py) and
         * "n items" for a focused list from its setsize; it never speaks
         * the role of a focused list (speech_generator.py).
         */
        if (node->position > 0)
        {
            char num[16];

            g_snprintf(num, sizeof(num), "%d", node->position);
            g_variant_builder_add(&b, "{ss}", "posinset", num);
        }
        if (node->set_size > 0)
        {
            char num[16];

            g_snprintf(num, sizeof(num), "%d", node->set_size);
            g_variant_builder_add(&b, "{ss}", "setsize", num);
        }
        /* Orca's level of a tree item, as in ARIA (script_utilities.py). */
        if (node->level > 0)
        {
            char num[16];

            g_snprintf(num, sizeof(num), "%d", node->level);
            g_variant_builder_add(&b, "{ss}", "level", num);
        }
        ret = g_variant_new("(a{ss})", &b);
    }
    else if (!g_strcmp0(method, "GetApplication"))
        ret = g_variant_new("(@(so))", node_ref(node->app->root));
    else if (!g_strcmp0(method, "GetInterfaces"))
    {
        const char *const *ifaces = node_interfaces(node, &i);

        ret = g_variant_new("(@as)", g_variant_new_strv(ifaces, i));
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
    struct node *node = call_node(data, path);

    if (!node)
    {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT,
                    "%s no longer exists", path);
        return NULL;
    }
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
    if (!g_strcmp0(prop, "NActions"))
        return g_variant_new_int32(1);
    if (!g_strcmp0(prop, "CharacterCount"))
        return g_variant_new_int32(node->count);
    if (!g_strcmp0(prop, "CaretOffset"))
        return g_variant_new_int32(node->caret);
    if (!g_strcmp0(prop, "NRows"))
        return g_variant_new_int32(node->set_size);
    if (!g_strcmp0(prop, "NColumns"))
        return g_variant_new_int32(node->column_count);
    if (!g_strcmp0(prop, "Caption") || !g_strcmp0(prop, "Summary"))
        return null_ref();
    if (!g_strcmp0(prop, "NSelectedChildren") && node->role == BRIDGE_ROLE_TREE)
    {
        gint count;

        tree_selected_child(node, -1, &count);
        return g_variant_new_int32(count);
    }
    if (!g_strcmp0(prop, "NSelectedRows") || !g_strcmp0(prop, "NSelectedChildren"))
        return g_variant_new_int32(node->selected_count);
    if (!g_strcmp0(prop, "NSelectedColumns"))
        return g_variant_new_int32(0);
    if (!g_strcmp0(prop, "ColumnSpan") || !g_strcmp0(prop, "RowSpan"))
        return g_variant_new_int32(1);
    if (!g_strcmp0(prop, "Position"))
        return g_variant_new("(ii)", node->row, node->column);
    if (!g_strcmp0(prop, "Table"))
        return parent_ref(node);
    if (!g_strcmp0(prop, "MinimumValue"))
        return g_variant_new_double(node->minimum);
    if (!g_strcmp0(prop, "MaximumValue"))
        return g_variant_new_double(node->maximum);
    if (!g_strcmp0(prop, "MinimumIncrement"))
        return g_variant_new_double(node->increment);
    if (!g_strcmp0(prop, "CurrentValue"))
        return g_variant_new_double(node->current);
    /* Empty: Orca then speaks the number itself (ax_value.py). */
    if (!g_strcmp0(prop, "Text"))
        return g_variant_new_string("");
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
    const char *const *ifaces;
    guint i, count;

    if (!node)
        return NULL;
    ifaces = node_interfaces(node, &count);
    ret = g_new0(GDBusInterfaceInfo *, count + 1);
    for (i = 0; i < count; i++)
        ret[i] = g_dbus_interface_info_ref(
            g_dbus_node_info_lookup_interface(introspection, ifaces[i]));
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
    if (!node_has_interface(node, iface))
        return NULL;
    *out_data = app;
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
        if (node->children->pdata[i])
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
    g_free(node->shortcut);
    bridge_text_unref(node->text);
    g_strfreev(node->headers);
    g_free(node->selected_rows);
    g_free(node);
}

static void node_remove(struct node *node)
{
    struct node *parent = node->parent;
    gint index = index_in_parent(node), n, i;

    /*
     * The first cell of a table row takes the other cells along: they live
     * by its description (set_row) and would keep their old rectangles.
     */
    if (parent && parent->role == BRIDGE_ROLE_TABLE && (n = parent->column_count) &&
        index >= n && index % n == 0)
        for (i = index + n - 1; i > index; i--)
            if (i < (gint)parent->children->len && parent->children->pdata[i])
                node_remove(parent->children->pdata[i]);
    if (parent && index >= 0)
    {
        /* An item list keeps its slots, see is_item_list. */
        if (is_item_list(parent))
            parent->children->pdata[index] = NULL;
        else
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

/*
 * The children of a list box are its items in list order, one slot per
 * item (set_size), so that Orca gets the right count: for a list box it
 * says "list with n available items" when fewer children than setsize
 * are sensitive (speech_generator.py, _generate_number_of_children).
 * Only items that had focus are known; the other slots are NULL until
 * asked for, then filled by placeholders. A table has a slot for each
 * column header and each cell, see cell_index. A page tab list has a slot
 * for each tab.
 */
static gboolean is_item_list(struct node *node)
{
    return node->role == BRIDGE_ROLE_LIST_BOX || node->role == BRIDGE_ROLE_TABLE ||
           node->role == BRIDGE_ROLE_PAGE_TAB_LIST;
}

/* Keeps count slots; the known children beyond them leave, announced. */
static void resize_items(struct node *list, guint count)
{
    GPtrArray *children = list->children;

    while (children->len > count)
    {
        struct node *child = children->pdata[children->len - 1];

        g_ptr_array_set_size(children, children->len - 1);
        if (child)
        {
            emit(list, EVENT_OBJECT, "ChildrenChanged", "remove", children->len, 0,
                 node_ref(child));
            node_free(child);
        }
    }
    g_ptr_array_set_size(children, count);
}

static void sync_items(struct node *list)
{
    guint count = MAX(list->set_size, 0);

    if (!is_item_list(list))
        return;
    if (list->role == BRIDGE_ROLE_TABLE)
        count = (count + 1) * list->column_count;
    resize_items(list, count);
}

/*
 * Copies the columns of obj; for a table, its header texts and selected
 * rows, for a list box its selected items. Slots laid out for another
 * role or column count are dropped first.
 */
static void set_columns(struct node *node, const struct bridge_object *obj)
{
    gint i;

    if (is_item_list(node) &&
        (node->role != obj->role || node->column_count != obj->column_count))
        resize_items(node, 0);
    node->column_count = obj->column_count;
    g_strfreev(node->headers);
    node->headers = NULL;
    g_free(node->selected_rows);
    node->selected_rows = NULL;
    node->selected_count = node->selected_listed = 0;
    if (obj->role != BRIDGE_ROLE_TABLE && obj->role != BRIDGE_ROLE_LIST_BOX)
        return;
    node->selected_count = obj->selected_count;
    if (obj->selected_listed > 0 && obj->selected_rows)
    {
        node->selected_listed = obj->selected_listed;
        node->selected_rows = g_memdup2(obj->selected_rows, obj->selected_listed * sizeof(int));
    }
    if (obj->role != BRIDGE_ROLE_TABLE)
        return;
    node->headers = g_new0(char *, obj->column_count + 1);
    for (i = 0; i < obj->column_count; i++)
    {
        struct node *header = i < (gint)node->children->len ? node->children->pdata[i] : NULL;

        node->headers[i] = g_strdup(obj->columns[i] ? obj->columns[i] : "");
        if (header)
        {
            g_free(header->name);
            header->name = g_strdup(node->headers[i]);
        }
    }
}

/*
 * Item index of an item list as a node; a placeholder if not known yet.
 * Ids: "w<hwnd>_<item + 1>" as for an item from the Win32 side, which in
 * a table is the cell of the first column; "w<hwnd>_<row + 1>_<column>"
 * for the other cells, "w<hwnd>_h<column>" for column headers.
 */
static struct node *item_at(struct node *list, guint index)
{
    struct node *item = list->children->pdata[index];
    int prefix = (int)(g_strrstr(list->id, "_") + 1 - list->id);
    guint n = list->column_count;
    char *id;

    if (item)
        return item;
    if (list->role != BRIDGE_ROLE_TABLE)
        id = g_strdup_printf("%.*s%u", prefix, list->id, index + 1);
    else if (index < n)
        id = g_strdup_printf("%.*sh%u", prefix, list->id, index);
    else if (index % n)
        id = g_strdup_printf("%.*s%u_%u", prefix, list->id, index / n, index % n);
    else
        id = g_strdup_printf("%.*s%u", prefix, list->id, index / n);
    item = node_new(list->app, id, NULL);
    g_free(id);
    item->parent = list;
    list->children->pdata[index] = item;
    item->window_class = g_strdup(list->window_class);
    item->placeholder = TRUE;
    /*
     * Not showing: it may be scrolled away, and Orca's flat review skips
     * objects that are not showing (ax_utilities.py, is_on_screen). The
     * window tree describes the items on screen.
     */
    item->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                   BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                   BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE);
    if (list->role == BRIDGE_ROLE_TABLE && index < n)
    {
        /* Column headers stay on screen while the rows scroll. */
        item->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
        item->role = BRIDGE_ROLE_COLUMN_HEADER;
        item->name = g_strdup(list->headers ? list->headers[index] : "");
        item->row = -1;
        item->column = index;
        return item;
    }
    item->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                    BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE);
    item->role = list->role == BRIDGE_ROLE_PAGE_TAB_LIST ? BRIDGE_ROLE_PAGE_TAB
                                                          : BRIDGE_ROLE_LIST_ITEM;
    item->position = index + 1;
    if (list->role == BRIDGE_ROLE_TABLE)
    {
        item->role = BRIDGE_ROLE_TABLE_CELL;
        item->row = index / n - 1;
        item->column = index % n;
        item->position = item->row + 1;
    }
    item->set_size = list->set_size;
    return item;
}

static struct node *child_at(struct node *node, guint index)
{
    if (is_item_list(node))
        return item_at(node, index);
    return node->children->pdata[index];
}

/* ACTIVE and FOCUSED are ours: they change only with focus events. */
#define OWN_STATES (BRIDGE_STATE_BIT(BRIDGE_STATE_ACTIVE) | \
                    BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED))

/* Announces characters [start, start + len) of node's text as inserted or deleted. */
static void emit_text_changed(struct node *node, const char *detail, gint start, gint len)
{
    char *s;

    if (len <= 0)
        return;
    s = text_slice(node, start, start + len);
    emit(node, EVENT_OBJECT, "TextChanged", detail, start, len, g_variant_new_string(s));
    g_free(s);
}

/*
 * Takes the text, caret and selection of obj into node. With announce, a
 * changed text goes out as one delete and one insert of the changed part,
 * then caret and selection events, in the order GTK uses. A text made
 * from the node's one tells that part; else the differing middle part is
 * looked for.
 */
static void set_text(struct node *node, const struct bridge_object *obj, gboolean announce)
{
    struct bridge_text *text = obj->text;
    const gunichar *chars = text->chars;
    glong count = text->count;
    gint prefix = 0, removed = 0, inserted = 0, suffix = 0, old_caret = node->caret;
    gboolean had_selection = node->selection_start != node->selection_end;
    gboolean has_selection = obj->selection_start != obj->selection_end;
    gboolean selection_changed;

    if (!node->text)
        announce = FALSE;
    if (announce && text != node->text)
    {
        if (text->base == node->text->id)
        {
            prefix = text->start;
            removed = text->removed;
            inserted = text->inserted;
        }
        else
        {
            while (prefix < count && prefix < node->count && chars[prefix] == node->chars[prefix])
                prefix++;
            while (suffix < count - prefix && suffix < node->count - prefix &&
                   chars[count - 1 - suffix] == node->chars[node->count - 1 - suffix])
                suffix++;
            removed = node->count - prefix - suffix;
            inserted = count - prefix - suffix;
        }
        emit_text_changed(node, "delete", prefix, removed);
    }
    bridge_text_ref(text);
    bridge_text_unref(node->text);
    node->text = text;
    node->chars = chars;
    node->count = count;
    if (announce)
        emit_text_changed(node, "insert", prefix, inserted);

    node->caret = CLAMP(obj->caret, 0, count);
    selection_changed = (had_selection || has_selection) &&
                        (node->selection_start != obj->selection_start ||
                         node->selection_end != obj->selection_end);
    node->selection_start = CLAMP(obj->selection_start, 0, count);
    node->selection_end = CLAMP(obj->selection_end, node->selection_start, count);
    if (!announce)
        return;
    if (node->caret != old_caret)
        emit(node, EVENT_OBJECT, "TextCaretMoved", "", node->caret, 0, g_variant_new_int32(0));
    if (selection_changed)
        emit(node, EVENT_OBJECT, "TextSelectionChanged", "", 0, 0, g_variant_new_int32(0));
}

/*
 * The Win32 side describes only the first cell of a table row; the
 * other cells of the row take their texts from it and share its states,
 * so that Orca can read the whole row (ax_utilities_table.py,
 * get_showing_cells_in_same_row).
 */
static void set_row(struct node *cell, const struct bridge_object *obj)
{
    struct node *table = cell->parent;
    gint i, index;

    if (cell->role != BRIDGE_ROLE_TABLE_CELL || !table || table->role != BRIDGE_ROLE_TABLE)
        return;
    for (i = 1; i < obj->column_count; i++)
    {
        struct node *other;

        if ((index = cell_index(table, cell->row, i)) < 0)
            break;
        other = item_at(table, index);
        g_free(other->name);
        other->name = g_strdup(obj->columns[i]);
        other->states = cell->states & ~OWN_STATES;
        other->position = cell->position;
        other->set_size = cell->set_size;
        if (obj->column_extents)
        {
            const int *r = obj->column_extents + 4 * i;

            other->has_extents = TRUE;
            other->x = r[0];
            other->y = r[1];
            other->width = r[2];
            other->height = r[3];
        }
    }
}

/*
 * Value of a slider or progress bar. Orca presents a value change of the
 * focused object, or of any progress bar (default.py, _on_value_changed).
 */
static void set_range(struct node *node, const struct bridge_object *obj, gboolean announce)
{
    gboolean changed = node->has_range && obj->has_range && node->current != obj->current;

    node->has_range = obj->has_range;
    node->minimum = obj->minimum;
    node->maximum = obj->maximum;
    node->current = obj->current;
    node->increment = obj->increment;
    if (changed && announce)
        emit(node, EVENT_OBJECT, "PropertyChange", "accessible-value", 0, 0,
             g_variant_new_double(node->current));
}

/*
 * Screen rectangle. An object described without one (a list item) keeps
 * the last known, so the Component interface does not come and go.
 */
/* An object without a rectangle (a tree item scrolled away) drops its old one. */
static void set_extents(struct node *node, const struct bridge_object *obj)
{
    node->has_extents = obj->has_extents;
    if (!obj->has_extents)
        return;
    node->x = obj->x;
    node->y = obj->y;
    node->width = obj->width;
    node->height = obj->height;
}

/* Copies obj into an existing node, announcing a changed name, state or text. */
static void update_node(struct node *node, const struct bridge_object *obj)
{
    guint64 changed = (node->states ^ obj->states) & ~OWN_STATES;
    guint state;

    set_columns(node, obj);
    node->role = obj->role;
    node->position = obj->position;
    node->set_size = obj->set_size;
    node->level = obj->level;
    if (g_strcmp0(node->window_class, obj->window_class))
    {
        g_free(node->window_class);
        node->window_class = g_strdup(obj->window_class);
    }
    if (g_strcmp0(node->shortcut, obj->shortcut))
    {
        g_free(node->shortcut);
        node->shortcut = g_strdup(obj->shortcut);
    }
    for (state = 0; state < 64; state++)
        if (changed & BRIDGE_STATE_BIT(state))
            set_state(node, state, !!(obj->states & BRIDGE_STATE_BIT(state)));
    /*
     * Down on a Wine ListBox whose focused item is not selected only
     * selects that item. Orca presents a selected change only after Space
     * and ignores a repeated focus (default.py, _on_selected_changed), so
     * that Down would be silent. Announce the item's name instead: Orca
     * says nothing for a selected item and "not selected" otherwise, so
     * the bare name tells that it is selected now.
     */
    if (node == focused && node->role == BRIDGE_ROLE_LIST_ITEM &&
        (changed & obj->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED)) &&
        last_keysym != XK_SPACE)
        emit(node, EVENT_OBJECT, "Announcement", "", ATSPI_LIVE_POLITE, 0,
             g_variant_new_string(obj->name ? obj->name : ""));
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
    if (obj->text)
        set_text(node, obj, TRUE);
    set_row(node, obj);
    set_range(node, obj, TRUE);
    set_extents(node, obj);
}

/* Finds or creates the node for obj below parent and updates it. */
static struct node *get_node(struct app *app, const struct bridge_object *obj,
                             struct node *parent)
{
    char *id = object_id(obj);
    struct node *node;

    if (parent && is_item_list(parent) && obj->child)
    {
        /* The item may know a newer count than its list. */
        if (obj->set_size > 0 && obj->set_size != parent->set_size)
        {
            parent->set_size = obj->set_size;
            sync_items(parent);
        }
        if (parent->role == BRIDGE_ROLE_TABLE)
        {
            gint index = cell_index(parent, obj->child - 1, 0);

            if (index >= 0)
                item_at(parent, index);
        }
        else if (obj->child <= parent->children->len)
            item_at(parent, obj->child - 1);
    }
    node = g_hash_table_lookup(app->nodes, id);
    if (node && !node->placeholder)
        update_node(node, obj);
    else
    {
        /* New, or a placeholder filled in quietly: it was there all along. */
        if (!node)
            node = node_new(app, id, parent);
        node->placeholder = FALSE;
        set_columns(node, obj);
        node->role = obj->role;
        node->states = obj->states & ~OWN_STATES;
        g_free(node->name);
        node->name = g_strdup(obj->name);
        node->value = g_strdup(obj->value);
        g_free(node->window_class);
        node->window_class = g_strdup(obj->window_class);
        g_free(node->shortcut);
        node->shortcut = g_strdup(obj->shortcut);
        node->position = obj->position;
        node->set_size = obj->set_size;
        node->level = obj->level;
        if (obj->text)
            set_text(node, obj, FALSE);
        set_row(node, obj);
        set_range(node, obj, FALSE);
        set_extents(node, obj);
    }
    node->hwnd = obj->hwnd;
    sync_items(node);
    g_free(id);
    return node;
}

static struct app *app_new(guint pid)
{
    struct app *app = g_new0(struct app, 1);

    app->pid = pid;
    app->nodes = g_hash_table_new(g_str_hash, g_str_equal);
    return app;
}

/* Subtree user data destructor: no more D-Bus calls can reach app. */
static void app_free(gpointer data)
{
    struct app *app = data;

    g_hash_table_destroy(app->nodes);
    g_free(app->parent_bus);
    g_free(app->parent_path);
    g_free(app);
}

/*
 * Frees app, which is no longer in apps. Closing its connection makes
 * the registry remove it from the desktop and tell the screen readers;
 * with a subtree, app is freed once GDBus is done with it.
 */
static void app_close(struct app *app)
{
    if (app->root)
        node_free(app->root);
    app->root = NULL;
    if (app->conn)
    {
        if (app->subtree_id)
            g_dbus_connection_unregister_subtree(app->conn, app->subtree_id);
        g_dbus_connection_close(app->conn, NULL, NULL, NULL);
        g_clear_object(&app->conn);
    }
    if (!app->subtree_id)
        app_free(app);
}

static struct app *get_app(guint pid, const char *name)
{
    struct app *app = g_hash_table_lookup(apps, GUINT_TO_POINTER(pid));
    GError *error = NULL;
    GVariant *ret;

    if (app)
        return app->conn ? app : NULL;
    /*
     * A window destroyed before it was described has no process. Without
     * an a11y bus no failure marker is kept: the process registers once
     * the bus is back (see connect_bus).
     */
    if (!pid || !a11y_address)
        return NULL;

    app = app_new(pid);
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
    app->subtree_id = g_dbus_connection_register_subtree(
        app->conn, ATSPI_PREFIX, &subtree_vtable,
        G_DBUS_SUBTREE_FLAGS_DISPATCH_TO_UNENUMERATED_NODES, app, app_free, &error);
    if (!app->subtree_id)
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
    /* Keep an empty app without a connection as the failure marker. */
    g_hash_table_replace(apps, GUINT_TO_POINTER(pid), app_new(pid));
    app_close(app);
    return NULL;
}

/*
 * Makes the labels of f the label children of frame: new ones are added,
 * known ones updated, and the ones no longer listed removed. Done before
 * the focus event, so the screen reader finds them under a new dialog.
 */
static void set_labels(struct app *app, struct node *frame, const struct bridge_focus *f)
{
    GPtrArray *kept = g_ptr_array_new();
    GPtrArray *gone = g_ptr_array_new();
    gint i;
    guint j;

    for (i = 0; i < f->label_count; i++)
        g_ptr_array_add(kept, get_node(app, &f->labels[i], frame));
    for (j = 0; j < frame->children->len; j++)
    {
        struct node *child = frame->children->pdata[j];

        /* The window tree keeps its own labels, see tree_idle. */
        if (child && child->role == BRIDGE_ROLE_LABEL && !child->in_tree &&
            !g_ptr_array_find(kept, child, NULL))
            g_ptr_array_add(gone, child);
    }
    for (j = 0; j < gone->len; j++)
        node_remove(gone->pdata[j]);
    g_ptr_array_free(gone, TRUE);
    g_ptr_array_free(kept, TRUE);
}

/*
 * Removes old, the focus before now, if it is an item of a control that
 * is scrolled away (not showing): the window tree has the items in view
 * only and kept it for the focus (tree_idle), and an item described by a
 * focus outside the view was never in a tree. Removing the first cell of
 * a row takes the row's other cells along (node_remove), so nothing is
 * removed while now is in old's row.
 */
static void drop_hidden_item(struct node *old, struct node *now)
{
    struct node *parent = old->parent;

    if (!parent || parent == now || old->hwnd != parent->hwnd ||
        (old->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING)))
        return;
    if (old->role != BRIDGE_ROLE_TREE_ITEM && old->role != BRIDGE_ROLE_LIST_ITEM &&
        old->role != BRIDGE_ROLE_TABLE_CELL && old->role != BRIDGE_ROLE_PAGE_TAB)
        return;
    if (parent->role == BRIDGE_ROLE_TABLE && now->parent == parent && now->row == old->row)
        return;
    node_remove(old);
}

static gboolean focus_idle(gpointer data)
{
    const struct bridge_focus *f = data;
    struct app *app = get_app(f->pid, f->app_name);
    struct node *frame, *parent, *obj, *old;
    gint i;

    if (!app)
        return G_SOURCE_REMOVE;
    frame = get_node(app, &f->frame, app->root);
    set_labels(app, frame, f);
    parent = frame;
    for (i = 0; i < f->ancestor_count; i++)
        parent = get_node(app, &f->ancestors[i], parent);
    if (f->container.hwnd)
        parent = get_node(app, &f->container, parent);
    if (f->object.hwnd == f->frame.hwnd && f->object.child == f->frame.child)
        obj = frame;
    else
        obj = get_node(app, &f->object, parent);

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
    old = focused;
    if (old && old != obj)
        set_state(old, BRIDGE_STATE_FOCUSED, FALSE);
    focused = obj;
    if (old && old != obj)
        drop_hidden_item(old, obj);
    /* Announce even a repeated focus, e.g. returning from a menu. */
    obj->states &= ~BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    set_state(obj, BRIDGE_STATE_FOCUSED, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean value_idle(gpointer data)
{
    const struct bridge_focus *c = data;
    struct app *app = get_app(c->pid, c->app_name);
    struct node *frame, *obj;
    char *id;
    gboolean known;

    if (!app)
        return G_SOURCE_REMOVE;
    frame = get_node(app, &c->frame, app->root);
    id = object_id(&c->object);
    known = g_hash_table_lookup(app->nodes, id) != NULL;
    g_free(id);
    obj = get_node(app, &c->object, frame);
    /* get_node announces changes of known nodes only. */
    if (!known && obj->has_range)
        emit(obj, EVENT_OBJECT, "PropertyChange", "accessible-value", 0, 0,
             g_variant_new_double(obj->current));
    return G_SOURCE_REMOVE;
}

/*
 * Adds the ids of the nodes below node that came from a window tree;
 * with hwnd, only those of that window (the items of a control, not the
 * windows on a toolbar).
 */
static void collect_tree_ids(struct node *node, guint64 hwnd, GPtrArray *ids)
{
    guint i;

    for (i = 0; i < node->children->len; i++)
    {
        struct node *child = node->children->pdata[i];

        if (!child || (hwnd && child->hwnd != hwnd))
            continue;
        if (child->in_tree)
            g_ptr_array_add(ids, g_strdup(child->id));
        collect_tree_ids(child, hwnd, ids);
    }
}

static gboolean is_ancestor(struct node *ancestor, struct node *node)
{
    for (; node; node = node->parent)
        if (node == ancestor)
            return TRUE;
    return FALSE;
}

/* Moves node below parent, announcing it on both. */
static void node_move(struct node *node, struct node *parent)
{
    struct node *old = node->parent;
    gint index = index_in_parent(node);

    if (old && index >= 0)
    {
        if (is_item_list(old))
            old->children->pdata[index] = NULL;
        else
            g_ptr_array_remove_index(old->children, index);
        emit(old, EVENT_OBJECT, "ChildrenChanged", "remove", index, 0, node_ref(node));
    }
    node->parent = parent;
    g_ptr_array_add(parent->children, node);
    emit(parent, EVENT_OBJECT, "ChildrenChanged", "add", parent->children->len - 1, 0,
         node_ref(node));
}

static gint compare_tree_index(gconstpointer a, gconstpointer b)
{
    const struct node *x = *(struct node *const *)a, *y = *(struct node *const *)b;

    return x->tree_index < y->tree_index ? -1 : x->tree_index > y->tree_index;
}

/*
 * Puts the children of parent that came from the window tree first, in
 * Z order (the tab order of a dialog); the others (labels from a focus,
 * menus, the status bar) keep their order behind them. No event: a
 * changed order alone is not announced.
 */
static void order_children(struct node *parent)
{
    GPtrArray *tree, *rest;
    guint i;

    if (is_item_list(parent))
        return;
    tree = g_ptr_array_new();
    rest = g_ptr_array_new();
    for (i = 0; i < parent->children->len; i++)
    {
        struct node *child = parent->children->pdata[i];

        g_ptr_array_add(child && child->in_tree ? tree : rest, child);
    }
    g_ptr_array_sort(tree, compare_tree_index);
    for (i = 0; i < parent->children->len; i++)
        parent->children->pdata[i] =
            i < tree->len ? tree->pdata[i] : rest->pdata[i - tree->len];
    g_ptr_array_free(tree, TRUE);
    g_ptr_array_free(rest, TRUE);
}

static gboolean tree_idle(gpointer data)
{
    const struct bridge_tree *t = data;
    struct app *app = get_app(t->pid, t->app_name);
    struct node *frame, *root, *parent, *node;
    GPtrArray *old, *nodes;
    char *id;
    gint i;
    guint j;

    if (!app)
        return G_SOURCE_REMOVE;
    frame = root = get_node(app, &t->frame, app->root);
    /* Only the items of one control: its node is the root of the update. */
    if (t->control)
    {
        struct bridge_object window = { 0 };

        window.hwnd = t->control;
        id = object_id(&window);
        root = g_hash_table_lookup(app->nodes, id);
        g_free(id);
        if (!root || !root->in_tree || !is_ancestor(frame, root))
            return G_SOURCE_REMOVE;
    }
    /* Ids, not nodes: removing one node may free others as its subtree. */
    old = g_ptr_array_new_with_free_func(g_free);
    collect_tree_ids(root, t->control, old);
    for (j = 0; j < old->len; j++)
        ((struct node *)g_hash_table_lookup(app->nodes, old->pdata[j]))->in_tree = FALSE;

    nodes = g_ptr_array_new();
    for (i = 0; i < t->count; i++)
    {
        const struct bridge_object *obj = &t->objects[i];

        parent = frame;
        if (t->parents[i] != t->frame.hwnd)
        {
            struct bridge_object window = { 0 };

            window.hwnd = t->parents[i];
            id = object_id(&window);
            parent = g_hash_table_lookup(app->nodes, id);
            g_free(id);
            if (!parent || !parent->in_tree)
                parent = frame;
        }
        id = object_id(obj);
        node = g_hash_table_lookup(app->nodes, id);
        g_free(id);
        if (node == frame || (node && is_ancestor(node, parent)))
            continue;
        if (node && node->parent != parent)
            node_move(node, parent);
        node = get_node(app, obj, parent);
        node->in_tree = TRUE;
        node->tree_index = i;
        g_ptr_array_add(nodes, node);
    }

    /*
     * Tree nodes have tree nodes or the frame as parents, so none is freed
     * here. An item scrolled away leaves too, but not the focused one: the
     * focus stays where it is, no longer showing and without its old
     * rectangle (the tree has the items in view only); focus_idle removes it
     * when the focus moves on. A tree without items keeps the items of a
     * control that stays (items have the hwnd of their control).
     */
    for (j = 0; j < old->len; j++)
    {
        gboolean item;

        node = g_hash_table_lookup(app->nodes, old->pdata[j]);
        if (!node || node->in_tree)
            continue;
        item = node->parent && node->parent != frame && node->parent->in_tree &&
               node->hwnd == node->parent->hwnd;
        if (t->no_items && item)
            node->in_tree = TRUE;
        else if (node == focused)
        {
            if (item)
            {
                set_state(node, BRIDGE_STATE_SHOWING, FALSE);
                node->has_extents = FALSE;
            }
        }
        else
            node_remove(node);
    }
    order_children(root);
    for (j = 0; j < nodes->len; j++)
        order_children(nodes->pdata[j]);
    g_ptr_array_free(nodes, TRUE);
    g_ptr_array_free(old, TRUE);
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

static gboolean app_gone_idle(gpointer data)
{
    struct app *app = g_hash_table_lookup(apps, data);

    if (!app)
        return G_SOURCE_REMOVE;
    g_hash_table_remove(apps, data);
    if (app->conn)
        g_printerr("wine-atspi: pid %u exited, unregistering %s\n", app->pid,
                   g_dbus_connection_get_unique_name(app->conn));
    app_close(app);
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
    g_free((char *)obj->shortcut);
    bridge_text_unref(obj->text);
    g_strfreev((char **)obj->columns);
    g_free((int *)obj->column_extents);
    g_free((int *)obj->selected_rows);
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
    gint i;

    g_free((char *)f->app_name);
    free_object(&f->frame);
    free_object(&f->container);
    free_object(&f->object);
    for (i = 0; i < f->label_count; i++)
        free_object((struct bridge_object *)&f->labels[i]);
    g_free((struct bridge_object *)f->labels);
    for (i = 0; i < f->ancestor_count; i++)
        free_object((struct bridge_object *)&f->ancestors[i]);
    g_free((struct bridge_object *)f->ancestors);
    g_free(f);
}

static void copy_object(struct bridge_object *dst, const struct bridge_object *src)
{
    *dst = *src;
    dst->name = g_strdup(src->name);
    dst->value = g_strdup(src->value);
    dst->window_class = g_strdup(src->window_class);
    dst->shortcut = g_strdup(src->shortcut);
    bridge_text_ref(dst->text);
    dst->columns = NULL;
    if (src->column_count > 0)
    {
        char **columns = g_new0(char *, src->column_count + 1);
        gint i;

        for (i = 0; i < src->column_count; i++)
            columns[i] = g_strdup(src->columns[i] ? src->columns[i] : "");
        dst->columns = (const char *const *)columns;
        if (src->column_extents)
            dst->column_extents = g_memdup2(src->column_extents,
                                            src->column_count * 4 * sizeof(int));
    }
    else
        dst->column_count = 0;
    if (!dst->columns)
        dst->column_extents = NULL;
    dst->selected_rows = NULL;
    if (src->selected_listed > 0 && src->selected_rows)
        dst->selected_rows = g_memdup2(src->selected_rows, src->selected_listed * sizeof(int));
    else
        dst->selected_listed = 0;
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
    copy_object(&f->container, &focus->container);
    copy_object(&f->object, &focus->object);
    if (focus->label_count > 0)
    {
        struct bridge_object *labels = g_new0(struct bridge_object, focus->label_count);
        gint i;

        for (i = 0; i < focus->label_count; i++)
            copy_object(&labels[i], &focus->labels[i]);
        f->labels = labels;
        f->label_count = focus->label_count;
    }
    if (focus->ancestor_count > 0)
    {
        struct bridge_object *ancestors = g_new0(struct bridge_object, focus->ancestor_count);
        gint i;

        for (i = 0; i < focus->ancestor_count; i++)
            copy_object(&ancestors[i], &focus->ancestors[i]);
        f->ancestors = ancestors;
        f->ancestor_count = focus->ancestor_count;
    }
    post(focus_idle, f, free_focus);
}

void atspi_post_update(unsigned int pid, const struct bridge_object *object)
{
    struct update *u = g_new0(struct update, 1);

    u->pid = pid;
    copy_object(&u->object, object);
    post(update_idle, u, free_update);
}

void atspi_post_value(const struct bridge_focus *change)
{
    struct bridge_focus *c = g_new0(struct bridge_focus, 1);

    c->pid = change->pid;
    c->app_name = g_strdup(change->app_name);
    copy_object(&c->frame, &change->frame);
    copy_object(&c->object, &change->object);
    post(value_idle, c, free_focus);
}

static void free_tree(gpointer data)
{
    struct bridge_tree *t = data;
    gint i;

    g_free((char *)t->app_name);
    free_object(&t->frame);
    for (i = 0; i < t->count; i++)
        free_object((struct bridge_object *)&t->objects[i]);
    g_free((struct bridge_object *)t->objects);
    g_free((unsigned long long *)t->parents);
    g_free(t);
}

void atspi_post_tree(const struct bridge_tree *tree)
{
    struct bridge_tree *t = g_new0(struct bridge_tree, 1);
    struct bridge_object *objects;
    gint i;

    t->pid = tree->pid;
    t->app_name = g_strdup(tree->app_name);
    copy_object(&t->frame, &tree->frame);
    t->count = MAX(tree->count, 0);
    objects = g_new0(struct bridge_object, t->count);
    for (i = 0; i < t->count; i++)
        copy_object(&objects[i], &tree->objects[i]);
    t->objects = objects;
    t->parents = g_memdup2(tree->parents, t->count * sizeof(*tree->parents));
    t->no_items = tree->no_items;
    t->control = tree->control;
    post(tree_idle, t, free_tree);
}

/* Connection for key reports; not tied to any application. */
static GDBusConnection *key_conn;

static gboolean key_idle(gpointer data)
{
    const struct bridge_key *k = data;
    GError *error = NULL;

    if (k->pressed)
        last_keysym = k->keysym;
    if (!a11y_address)
        return G_SOURCE_REMOVE;
    if (!key_conn)
    {
        key_conn = g_dbus_connection_new_for_address_sync(
            a11y_address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, &error);
        if (!key_conn)
        {
            g_printerr("wine-atspi: no connection for keys: %s\n", error->message);
            g_error_free(error);
            return G_SOURCE_REMOVE;
        }
    }
    /*
     * Accessibility_DeviceEvent: type 0 press, 1 release; id keysym;
     * hw_code keycode; modifiers; timestamp; event_string; is_text. The
     * registry's introspection says (uiuuisb), but it only accepts
     * (uinnisb), as atk-bridge sends it; anything else is rejected and a
     * bare (non-struct) argument list crashes the registry. The reply
     * (consumed) is not waited for: the key has already gone on to the target.
     */
    g_dbus_connection_call(
        key_conn, "org.a11y.atspi.Registry", "/org/a11y/atspi/registry/deviceeventcontroller",
        "org.a11y.atspi.DeviceEventController", "NotifyListenersSync",
        g_variant_new("((uinnisb))", k->pressed ? 0 : 1, (gint32)k->keysym,
                      (gint16)k->keycode, (gint16)k->modifiers, (gint32)k->timestamp,
                      k->string ? k->string : "", (gboolean)k->is_text),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void free_key(gpointer data)
{
    struct bridge_key *k = data;

    g_free((char *)k->string);
    g_free(k);
}

void atspi_post_key(const struct bridge_key *key)
{
    struct bridge_key *k = g_new(struct bridge_key, 1);

    *k = *key;
    k->string = g_strdup(key->string);
    post(key_idle, k, free_key);
}

void atspi_post_window_gone(unsigned long long hwnd)
{
    guint64 *data = g_new(guint64, 1);

    *data = hwnd;
    post(window_gone_idle, data, g_free);
}

void atspi_post_app_gone(unsigned int pid)
{
    post(app_gone_idle, GUINT_TO_POINTER(pid), NULL);
}

/* Unregisters every application, failure markers included. */
static void close_all_apps(void)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, apps);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        g_hash_table_iter_steal(&iter);
        app_close(value);
    }
}

static gboolean reset_idle(gpointer data)
{
    close_all_apps();
    return G_SOURCE_REMOVE;
}

void atspi_post_reset(void)
{
    post(reset_idle, NULL, NULL);
}

/*
 * Connection that watches the registry's event listeners: without one
 * nobody hears the bridge, and its hooks only cost the targets time.
 */
static GDBusConnection *watch_conn;
static int listening = -1;          /* not known yet */

static void tell_listening(int on)
{
    if (on == listening)
        return;
    listening = on;
    g_printerr("wine-atspi: %s\n", on ? "event listeners present" : "no event listeners");
    bridge_set_listening(on);
}

static void got_listeners(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    GVariant *events;

    /* A reply over a connection already dropped (see drop_bus) is stale. */
    if (G_DBUS_CONNECTION(source) != watch_conn)
    {
        if (ret)
            g_variant_unref(ret);
        g_clear_error(&error);
        return;
    }
    if (!ret)
    {
        /* Better to watch for nobody than to stay silent for a screen reader. */
        g_printerr("wine-atspi: cannot list event listeners: %s\n", error->message);
        g_error_free(error);
        tell_listening(1);
        return;
    }
    events = g_variant_get_child_value(ret, 0);
    tell_listening(g_variant_n_children(events) > 0);
    g_variant_unref(events);
    g_variant_unref(ret);
}

static void ask_listeners(void)
{
    /* Replies come in call order, so the last one is the current state. */
    g_dbus_connection_call(watch_conn, "org.a11y.atspi.Registry", "/org/a11y/atspi/registry",
                           "org.a11y.atspi.Registry", "GetRegisteredEvents", NULL,
                           G_VARIANT_TYPE("(a(ss))"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           got_listeners, NULL);
}

/*
 * EventListenerRegistered (bus, event, properties) or
 * EventListenerDeregistered (bus, event); the registry also deregisters
 * all events of a client that leaves the bus, with an empty event.
 */
static void listeners_changed(GDBusConnection *conn, const char *sender, const char *path,
                              const char *iface, const char *signal, GVariant *params,
                              gpointer data)
{
    ask_listeners();
}

/*
 * The a11y bus may be missing when the bridge starts and may go away
 * later. The bridge watches the bus launcher's name org.a11y.Bus on the
 * session bus and asks it for the address whenever it appears; the
 * watch connection's "closed" signal tells that the a11y bus died, and a
 * watch of org.a11y.atspi.Registry on it that the registry restarted.
 * Either loss unregisters every application and says that nobody
 * listens; after the next connection the listener question decides
 * again, and the main thread then reports the focus, which registers the
 * applications on the new bus. All of it runs on the AT-SPI thread.
 */
#define BUS_RETRY_MS 2000

static GDBusConnection *session_conn;
static gboolean bus_owned;          /* org.a11y.Bus has an owner */
static gboolean connecting;         /* GetAddress in flight */
static gboolean bus_failure_told;   /* log a failure once until it works */
static guint retry_source;
static gulong closed_handler;
static guint listeners_subscription;
static guint registry_watch;
static gboolean registry_seen;
static gboolean registry_lost;

static void connect_bus(void);

static gboolean retry_connect(gpointer data)
{
    retry_source = 0;
    connect_bus();
    return G_SOURCE_REMOVE;
}

/* Tries again while the launcher is there; its new owner tries at once. */
static void schedule_retry(void)
{
    GSource *source;

    if (retry_source || !bus_owned)
        return;
    source = g_timeout_source_new(BUS_RETRY_MS);
    g_source_set_callback(source, retry_connect, NULL, NULL);
    retry_source = g_source_attach(source, context);
    g_source_unref(source);
}

static void bus_failed(const char *what, GError *error)
{
    if (!bus_failure_told)
        g_printerr("wine-atspi: %s: %s; waiting for it\n", what, error->message);
    bus_failure_told = TRUE;
    g_error_free(error);
    schedule_retry();
}

/* Forgets the a11y bus and everything registered on it. */
static void drop_bus(void)
{
    close_all_apps();
    if (registry_watch)
        g_bus_unwatch_name(registry_watch);
    registry_watch = 0;
    registry_seen = registry_lost = FALSE;
    if (watch_conn)
    {
        g_signal_handler_disconnect(watch_conn, closed_handler);
        g_dbus_connection_signal_unsubscribe(watch_conn, listeners_subscription);
        g_dbus_connection_close(watch_conn, NULL, NULL, NULL);
        g_clear_object(&watch_conn);
    }
    if (key_conn)
    {
        g_dbus_connection_close(key_conn, NULL, NULL, NULL);
        g_clear_object(&key_conn);
    }
    g_clear_pointer(&a11y_address, g_free);
    tell_listening(0);
}

static void bus_closed(GDBusConnection *conn, gboolean remote_peer_vanished, GError *error,
                       gpointer data)
{
    g_printerr("wine-atspi: a11y bus closed%s%s\n", error ? ": " : "",
               error ? error->message : "");
    drop_bus();
    schedule_retry();
}

static void registry_appeared(GDBusConnection *conn, const char *name, const char *owner,
                              gpointer data)
{
    registry_seen = TRUE;
    if (!registry_lost)
        return;
    registry_lost = FALSE;
    g_printerr("wine-atspi: registry back as %s\n", owner);
    bridge_bus_ready();
    ask_listeners();
}

/*
 * A new registry knows none of the applications embedded in the old one.
 * Not yet started at all is no loss: the first listener question starts it.
 */
static void registry_vanished(GDBusConnection *conn, const char *name, gpointer data)
{
    if (!registry_seen)
        return;
    registry_seen = FALSE;
    registry_lost = TRUE;
    g_printerr("wine-atspi: registry gone\n");
    close_all_apps();
    tell_listening(0);
}

static void got_address(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_finish(session_conn, result, &error);
    char *address;

    connecting = FALSE;
    if (!ret)
    {
        bus_failed("no a11y bus", error);
        return;
    }
    g_variant_get(ret, "(s)", &address);
    g_variant_unref(ret);
    /* Created on the AT-SPI thread, so its signals are dispatched here. */
    watch_conn = g_dbus_connection_new_for_address_sync(
        address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    if (!watch_conn)
    {
        g_free(address);
        bus_failed("cannot connect to the a11y bus", error);
        return;
    }
    a11y_address = address;
    bus_failure_told = FALSE;
    g_printerr("wine-atspi: a11y bus at %s\n", address);
    closed_handler = g_signal_connect(watch_conn, "closed", G_CALLBACK(bus_closed), NULL);
    /* Subscribed before the first question, so no change slips between. */
    listeners_subscription = g_dbus_connection_signal_subscribe(
        watch_conn, "org.a11y.atspi.Registry", "org.a11y.atspi.Registry", NULL,
        "/org/a11y/atspi/registry", NULL, G_DBUS_SIGNAL_FLAGS_NONE, listeners_changed, NULL,
        NULL);
    registry_watch = g_bus_watch_name_on_connection(
        watch_conn, "org.a11y.atspi.Registry", G_BUS_NAME_WATCHER_FLAGS_NONE,
        registry_appeared, registry_vanished, NULL, NULL);
    bridge_bus_ready();
    ask_listeners();
}

static void connect_bus(void)
{
    if (watch_conn || connecting)
        return;
    connecting = TRUE;
    /* May start the launcher, like libatspi does. */
    g_dbus_connection_call(session_conn, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                           "GetAddress", NULL, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, got_address, NULL);
}

static void launcher_appeared(GDBusConnection *conn, const char *name, const char *owner,
                              gpointer data)
{
    bus_owned = TRUE;
    connect_bus();
}

/* The bus daemon may outlive its launcher; "closed" tells when it goes. */
static void launcher_vanished(GDBusConnection *conn, const char *name, gpointer data)
{
    bus_owned = FALSE;
    if (retry_source)
        g_source_remove(retry_source);
    retry_source = 0;
    if (!watch_conn && !bus_failure_told)
    {
        g_printerr("wine-atspi: no a11y bus yet; waiting for org.a11y.Bus\n");
        bus_failure_told = TRUE;
    }
}

int atspi_init(void)
{
    GError *error = NULL;

    /* Without a session bus there is no a11y bus to wait for. */
    session_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!session_conn)
    {
        g_printerr("wine-atspi: no session bus: %s\n", error->message);
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
    g_bus_watch_name_on_connection(session_conn, "org.a11y.Bus",
                                   G_BUS_NAME_WATCHER_FLAGS_AUTO_START, launcher_appeared,
                                   launcher_vanished, NULL, NULL);
    g_main_loop_run(g_main_loop_new(context, FALSE));
}
