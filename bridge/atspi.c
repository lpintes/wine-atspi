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
    "</node>";

#define IFACE_ACCESSIBLE "org.a11y.atspi.Accessible"
#define IFACE_APPLICATION "org.a11y.atspi.Application"
#define IFACE_TEXT "org.a11y.atspi.Text"
#define IFACE_TABLE "org.a11y.atspi.Table"
#define IFACE_TABLE_CELL "org.a11y.atspi.TableCell"

/* AtspiTextGranularity */
enum { GRANULARITY_CHAR, GRANULARITY_WORD, GRANULARITY_SENTENCE, GRANULARITY_LINE,
       GRANULARITY_PARAGRAPH };
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
    /* Text interface; chars is NULL for nodes without one. */
    gunichar *chars;
    glong count;
    gint caret;
    gint selection_start;
    gint selection_end;
    gint position;          /* from 1, 0 if not a list item */
    gint set_size;          /* items of a list, rows of a table */
    gboolean placeholder;   /* list item not described yet, see item_at */
    /* Table: columns and their header texts. Cell or header: its place, from 0. */
    gint column_count;
    char **headers;
    gint row;
    gint column;
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

static gboolean is_word_char(gunichar c)
{
    return g_unichar_isalnum(c) || g_unichar_ismark(c);
}

/*
 * Range [*start, *end) of the unit of granularity at offset, with the ATK
 * meaning: a word runs to the start of the next word, a line includes its
 * "\n". At the end of text ending in "\n" the line is empty. Sentences
 * and paragraphs are logical lines for now.
 */
static void text_range(struct node *node, gint offset, guint granularity,
                       gint *start, gint *end)
{
    const gunichar *c = node->chars;
    gint n = node->count, i;

    offset = CLAMP(offset, 0, n);
    switch (granularity)
    {
    case GRANULARITY_CHAR:
        *start = offset;
        *end = MIN(offset + 1, n);
        return;
    case GRANULARITY_WORD:
        for (i = offset; i > 0; i--)
            if (i < n && is_word_char(c[i]) && !is_word_char(c[i - 1]))
                break;
        *start = i;
        for (i = offset + 1; i < n; i++)
            if (is_word_char(c[i]) && !is_word_char(c[i - 1]))
                break;
        *end = MIN(i, n);
        return;
    default:
        for (i = offset; i > 0 && c[i - 1] != '\n'; i--)
            ;
        *start = i;
        for (i = offset; i < n && c[i] != '\n'; i++)
            ;
        *end = MIN(i + 1, n);
        return;
    }
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

    text_range(node, offset, granularity, &start, &end);
    if (which < 0)
    {
        if (start > 0)
            text_range(node, start - 1, granularity, &start, &end);
        else
            start = end = 0;
    }
    else if (which > 0)
    {
        if (end < node->count && end > start)
            text_range(node, end, granularity, &start, &end);
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
        text_range(node, offset, type, &start, &end);
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
        return g_variant_new("(iiii)", 0, 0, 0, 0);
    if (!g_strcmp0(method, "GetOffsetAtPoint"))
        return g_variant_new("(i)", -1);
    /* Changing the caret or selection would need the Win32 thread; not yet. */
    if (!g_strcmp0(method, "SetCaretOffset") || !g_strcmp0(method, "AddSelection") ||
        !g_strcmp0(method, "RemoveSelection") || !g_strcmp0(method, "SetSelection") ||
        !g_strcmp0(method, "ScrollSubstringTo") || !g_strcmp0(method, "ScrollSubstringToPoint"))
        return g_variant_new("(b)", FALSE);
    return NULL;
}

/* Interfaces of node, the first always Accessible. */
static const char *const *node_interfaces(struct node *node, guint *count)
{
    static const char *const app_ifaces[] = { IFACE_ACCESSIBLE, IFACE_APPLICATION };
    static const char *const text_ifaces[] = { IFACE_ACCESSIBLE, IFACE_TEXT };
    static const char *const table_ifaces[] = { IFACE_ACCESSIBLE, IFACE_TABLE };
    static const char *const cell_ifaces[] = { IFACE_ACCESSIBLE, IFACE_TABLE_CELL };

    *count = 2;
    if (!node->parent)
        return app_ifaces;
    if (node->role == BRIDGE_ROLE_TABLE)
        return table_ifaces;
    if (node->role == BRIDGE_ROLE_TABLE_CELL)
        return cell_ifaces;
    *count = node->chars ? 2 : 1;
    return text_ifaces;
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
 * Only rows that had focus are known, and their state may be old, so
 * the selection is told per row and the lists of selected rows stay
 * empty rather than wrong.
 */
static gboolean row_selected(struct node *table, gint row)
{
    gint index = cell_index(table, row, 0);
    struct node *cell = index >= 0 ? table->children->pdata[index] : NULL;

    return cell && (cell->states & BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED));
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
    if (!g_strcmp0(method, "GetSelectedRows") || !g_strcmp0(method, "GetSelectedColumns"))
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

static void method_call(GDBusConnection *conn, const char *sender,
                        const char *path, const char *iface,
                        const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    struct node *node = data;
    GVariant *ret = NULL;
    guint i;

    if (!g_strcmp0(iface, IFACE_TEXT))
        ret = text_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_TABLE))
        ret = table_method(node, method, params);
    else if (!g_strcmp0(iface, IFACE_TABLE_CELL))
        ret = table_cell_method(node, method);
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
    if (!g_strcmp0(prop, "NSelectedRows") || !g_strcmp0(prop, "NSelectedColumns"))
        return g_variant_new_int32(0);
    if (!g_strcmp0(prop, "ColumnSpan") || !g_strcmp0(prop, "RowSpan"))
        return g_variant_new_int32(1);
    if (!g_strcmp0(prop, "Position"))
        return g_variant_new("(ii)", node->row, node->column);
    if (!g_strcmp0(prop, "Table"))
        return parent_ref(node);
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
    g_free(node->chars);
    g_strfreev(node->headers);
    g_free(node);
}

static void node_remove(struct node *node)
{
    struct node *parent = node->parent;
    gint index = index_in_parent(node);

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
 * column header and each cell, see cell_index.
 */
static gboolean is_item_list(struct node *node)
{
    return node->role == BRIDGE_ROLE_LIST_BOX || node->role == BRIDGE_ROLE_TABLE;
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
 * Copies the columns of obj; for a table, its header texts. Slots laid
 * out for another role or column count are dropped first.
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
    item->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                   BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                   BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                   BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    if (list->role == BRIDGE_ROLE_TABLE && index < n)
    {
        item->role = BRIDGE_ROLE_COLUMN_HEADER;
        item->name = g_strdup(list->headers ? list->headers[index] : "");
        item->row = -1;
        item->column = index;
        return item;
    }
    item->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                    BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE);
    item->role = BRIDGE_ROLE_LIST_ITEM;
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
 * Copies the text, caret and selection of obj into node. With announce,
 * a changed text goes out as one delete and one insert of the differing
 * middle part, then caret and selection events, in the order GTK uses.
 */
static void set_text(struct node *node, const struct bridge_object *obj, gboolean announce)
{
    glong count;
    gunichar *chars = g_utf8_to_ucs4_fast(obj->text, -1, &count);
    gint prefix = 0, suffix = 0, old_caret = node->caret;
    gboolean had_selection = node->selection_start != node->selection_end;
    gboolean has_selection = obj->selection_start != obj->selection_end;
    gboolean selection_changed;

    if (!node->chars)
        announce = FALSE;
    if (announce)
    {
        while (prefix < count && prefix < node->count && chars[prefix] == node->chars[prefix])
            prefix++;
        while (suffix < count - prefix && suffix < node->count - prefix &&
               chars[count - 1 - suffix] == node->chars[node->count - 1 - suffix])
            suffix++;
        emit_text_changed(node, "delete", prefix, node->count - prefix - suffix);
    }
    g_free(node->chars);
    node->chars = chars;
    node->count = count;
    if (announce)
        emit_text_changed(node, "insert", prefix, count - prefix - suffix);

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
    }
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
    if (g_strcmp0(node->window_class, obj->window_class))
    {
        g_free(node->window_class);
        node->window_class = g_strdup(obj->window_class);
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
        node->position = obj->position;
        node->set_size = obj->set_size;
        if (obj->text)
            set_text(node, obj, FALSE);
        set_row(node, obj);
    }
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

static gboolean focus_idle(gpointer data)
{
    const struct bridge_focus *f = data;
    struct app *app = get_app(f->pid, f->app_name);
    struct node *frame, *parent, *obj;

    if (!app)
        return G_SOURCE_REMOVE;
    frame = get_node(app, &f->frame, app->root);
    parent = f->container.hwnd ? get_node(app, &f->container, frame) : frame;
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
    g_free((char *)obj->text);
    g_strfreev((char **)obj->columns);
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
    free_object(&f->container);
    free_object(&f->object);
    g_free(f);
}

static void copy_object(struct bridge_object *dst, const struct bridge_object *src)
{
    *dst = *src;
    dst->name = g_strdup(src->name);
    dst->value = g_strdup(src->value);
    dst->window_class = g_strdup(src->window_class);
    dst->text = g_strdup(src->text);
    dst->columns = NULL;
    if (src->column_count > 0)
    {
        char **columns = g_new0(char *, src->column_count + 1);
        gint i;

        for (i = 0; i < src->column_count; i++)
            columns[i] = g_strdup(src->columns[i] ? src->columns[i] : "");
        dst->columns = (const char *const *)columns;
    }
    else
        dst->column_count = 0;
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
    post(focus_idle, f, free_focus);
}

void atspi_post_update(unsigned int pid, const struct bridge_object *object)
{
    struct update *u = g_new0(struct update, 1);

    u->pid = pid;
    copy_object(&u->object, object);
    post(update_idle, u, free_update);
}

/* Connection for key reports; not tied to any application. */
static GDBusConnection *key_conn;

static gboolean key_idle(gpointer data)
{
    const struct bridge_key *k = data;
    GError *error = NULL;

    if (k->pressed)
        last_keysym = k->keysym;
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
