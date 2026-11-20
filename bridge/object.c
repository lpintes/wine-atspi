/*
 * wine-atspi bridge, Win32 core: describing a window or an MSAA object
 * (role, states, extents) and choosing the handler of its class.
 */
#define COBJMACROS
#include <windows.h>
/* Wine's libuuid lacks IID_IAccessible, so define it here, in one file. */
#include <initguid.h>
#include "core.h"

static unsigned int map_role(LONG role)
{
    switch (role)
    {
    case ROLE_SYSTEM_MENUBAR: return BRIDGE_ROLE_MENU_BAR;
    case ROLE_SYSTEM_SCROLLBAR: return BRIDGE_ROLE_SCROLL_BAR;
    case ROLE_SYSTEM_WINDOW: return BRIDGE_ROLE_WINDOW;
    case ROLE_SYSTEM_CLIENT: return BRIDGE_ROLE_PANEL;
    case ROLE_SYSTEM_MENUPOPUP: return BRIDGE_ROLE_POPUP_MENU;
    case ROLE_SYSTEM_MENUITEM: return BRIDGE_ROLE_MENU_ITEM;
    case ROLE_SYSTEM_PANE: return BRIDGE_ROLE_PANEL;
    case ROLE_SYSTEM_DIALOG: return BRIDGE_ROLE_DIALOG;
    case ROLE_SYSTEM_GROUPING: return BRIDGE_ROLE_GROUPING;
    case ROLE_SYSTEM_SEPARATOR: return BRIDGE_ROLE_SEPARATOR;
    case ROLE_SYSTEM_TOOLBAR: return BRIDGE_ROLE_TOOL_BAR;
    case ROLE_SYSTEM_STATUSBAR: return BRIDGE_ROLE_STATUS_BAR;
    case ROLE_SYSTEM_TABLE: return BRIDGE_ROLE_TABLE;
    case ROLE_SYSTEM_COLUMNHEADER: return BRIDGE_ROLE_COLUMN_HEADER;
    case ROLE_SYSTEM_ROW: return BRIDGE_ROLE_TABLE_ROW;
    case ROLE_SYSTEM_CELL: return BRIDGE_ROLE_TABLE_CELL;
    case ROLE_SYSTEM_LINK: return BRIDGE_ROLE_LINK;
    case ROLE_SYSTEM_LIST: return BRIDGE_ROLE_LIST;
    case ROLE_SYSTEM_LISTITEM: return BRIDGE_ROLE_LIST_ITEM;
    case ROLE_SYSTEM_OUTLINE: return BRIDGE_ROLE_TREE;
    case ROLE_SYSTEM_OUTLINEITEM: return BRIDGE_ROLE_TREE_ITEM;
    case ROLE_SYSTEM_PAGETAB: return BRIDGE_ROLE_PAGE_TAB;
    case ROLE_SYSTEM_PROPERTYPAGE: return BRIDGE_ROLE_PANEL;
    case ROLE_SYSTEM_GRAPHIC: return BRIDGE_ROLE_IMAGE;
    case ROLE_SYSTEM_STATICTEXT: return BRIDGE_ROLE_LABEL;
    case ROLE_SYSTEM_TEXT: return BRIDGE_ROLE_TEXT;
    case ROLE_SYSTEM_PUSHBUTTON:
    case ROLE_SYSTEM_BUTTONDROPDOWN:
    case ROLE_SYSTEM_BUTTONMENU:
    case ROLE_SYSTEM_SPLITBUTTON: return BRIDGE_ROLE_PUSH_BUTTON;
    case ROLE_SYSTEM_CHECKBUTTON: return BRIDGE_ROLE_CHECK_BOX;
    case ROLE_SYSTEM_RADIOBUTTON: return BRIDGE_ROLE_RADIO_BUTTON;
    case ROLE_SYSTEM_COMBOBOX:
    case ROLE_SYSTEM_DROPLIST: return BRIDGE_ROLE_COMBO_BOX;
    case ROLE_SYSTEM_PROGRESSBAR: return BRIDGE_ROLE_PROGRESS_BAR;
    case ROLE_SYSTEM_SLIDER: return BRIDGE_ROLE_SLIDER;
    case ROLE_SYSTEM_SPINBUTTON: return BRIDGE_ROLE_SPIN_BUTTON;
    case ROLE_SYSTEM_PAGETABLIST: return BRIDGE_ROLE_PAGE_TAB_LIST;
    }
    return BRIDGE_ROLE_UNKNOWN;
}

static unsigned long long map_states(LONG state)
{
    unsigned long long s = 0;

    if (!(state & STATE_SYSTEM_UNAVAILABLE))
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
             BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE);
    if (!(state & STATE_SYSTEM_INVISIBLE))
    {
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE);
        if (!(state & STATE_SYSTEM_OFFSCREEN))
            s |= BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    }
    if (state & STATE_SYSTEM_SELECTED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    if (state & STATE_SYSTEM_FOCUSED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (state & STATE_SYSTEM_PRESSED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_PRESSED);
    if (state & STATE_SYSTEM_CHECKED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_CHECKED);
    if (state & STATE_SYSTEM_MIXED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_INDETERMINATE);
    if (state & STATE_SYSTEM_READONLY)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_READ_ONLY);
    if (state & STATE_SYSTEM_EXPANDED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE) |
             BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDED);
    if (state & STATE_SYSTEM_COLLAPSED)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE);
    if (state & STATE_SYSTEM_FOCUSABLE)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE);
    if (state & STATE_SYSTEM_SELECTABLE)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE);
    if (state & STATE_SYSTEM_MULTISELECTABLE)
        s |= BRIDGE_STATE_BIT(BRIDGE_STATE_MULTISELECTABLE);
    return s;
}

static BOOL is_combo_class(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, L"ComboBoxEx32");
}

/*
 * Window text of a standard control of class cls. Of the classes in
 * role_from_class only the combo boxes (user32/combo.c,
 * comctl32_v6/combo.c, comctl32/comboex.c) and the status bar
 * (comctl32/status.c) handle WM_GETTEXT themselves; the others leave
 * their text to DefWindowProc, which keeps it on the server, so
 * InternalGetWindowText reads it without a message and a busy target
 * does not wait. FALSE if the message timed out.
 */
static BOOL get_class_text(HWND hwnd, const WCHAR *cls, WCHAR *buf, int len)
{
    buf[0] = 0;
    if (!is_combo_class(cls) && lstrcmpiW(cls, L"msctls_statusbar32"))
    {
        InternalGetWindowText(hwnd, buf, len);
        return TRUE;
    }
    if (!send_message(hwnd, WM_GETTEXT, len, (LPARAM)buf, SEND_TIMEOUT_MS, NULL))
        return FALSE;
    buf[len - 1] = 0;
    return TRUE;
}

/*
 * The outermost combo box that hwnd is part of, e.g. the ComboBoxEx32
 * around the ComboBox around the Open dialog's file name Edit; NULL if
 * the parent of hwnd is no combo box.
 */
HWND outer_combo(HWND hwnd)
{
    WCHAR cls[64];
    HWND parent, combo = NULL;

    while ((parent = GetParent(hwnd)) && GetClassNameW(parent, cls, ARRAYSIZE(cls)) &&
           is_combo_class(cls))
        combo = hwnd = parent;
    return combo;
}

/*
 * The visible Edit of combo box combo, also inside the ComboBox of a
 * ComboBoxEx32 (which has a hidden Edit of its own); NULL for a drop-down
 * list. No message: CB_GETCOMBOBOXINFO would need memory in the target.
 */
HWND combo_edit(HWND combo)
{
    WCHAR cls[64];
    HWND hwnd, edit;

    for (hwnd = GetWindow(combo, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)))
            continue;
        if (!lstrcmpiW(cls, L"Edit"))
            return hwnd;
        if (is_combo_class(cls) && (edit = combo_edit(hwnd)))
            return edit;
    }
    return NULL;
}

/*
 * Wine's oleacc has no proxy for most standard controls and calls them
 * "client". For a whole window the class tells more.
 */
unsigned int role_from_class(HWND hwnd, const WCHAR *cls)
{
    static const struct { const WCHAR *cls; unsigned int role; } map[] =
    {
        { L"ComboBox", BRIDGE_ROLE_COMBO_BOX },
        { L"ComboBoxEx32", BRIDGE_ROLE_COMBO_BOX },
        { L"Edit", BRIDGE_ROLE_TEXT },
        { L"ListBox", BRIDGE_ROLE_LIST },
        { L"ComboLBox", BRIDGE_ROLE_LIST },
        { L"msctls_progress32", BRIDGE_ROLE_PROGRESS_BAR },
        { L"msctls_statusbar32", BRIDGE_ROLE_STATUS_BAR },
        { L"msctls_trackbar32", BRIDGE_ROLE_SLIDER },
        { L"msctls_updown32", BRIDGE_ROLE_SPIN_BUTTON },
        { L"RICHEDIT50W", BRIDGE_ROLE_TEXT },
        { L"RichEdit20A", BRIDGE_ROLE_TEXT },
        { L"RichEdit20W", BRIDGE_ROLE_TEXT },
        { L"ScrollBar", BRIDGE_ROLE_SCROLL_BAR },
        { L"Static", BRIDGE_ROLE_LABEL },
        { L"SysListView32", BRIDGE_ROLE_LIST },
        { L"SysTabControl32", BRIDGE_ROLE_PAGE_TAB_LIST },
        { L"SysTreeView32", BRIDGE_ROLE_TREE },
        { L"ToolbarWindow32", BRIDGE_ROLE_TOOL_BAR },
    };
    unsigned int i;

    if (!lstrcmpiW(cls, L"Button"))
    {
        switch (GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK)
        {
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX:
        case BS_3STATE:
        case BS_AUTO3STATE:
            return BRIDGE_ROLE_CHECK_BOX;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON:
            return BRIDGE_ROLE_RADIO_BUTTON;
        case BS_GROUPBOX:
            return BRIDGE_ROLE_GROUPING;
        }
        return BRIDGE_ROLE_PUSH_BUTTON;
    }
    for (i = 0; i < ARRAYSIZE(map); i++)
        if (!lstrcmpiW(cls, map[i].cls))
            return map[i].role;
    return BRIDGE_ROLE_UNKNOWN;
}

/* States MSAA does not report for a window handled by role_from_class. */
static void refine_class_states(HWND hwnd, struct bridge_object *obj)
{
    DWORD_PTR check = 0;

    if (obj->role != BRIDGE_ROLE_CHECK_BOX && obj->role != BRIDGE_ROLE_RADIO_BUTTON)
        return;
    send_message(hwnd, BM_GETCHECK, 0, 0, SEND_TIMEOUT_MS, &check);
    if (check == BST_CHECKED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_CHECKED);
    else if (check == BST_INDETERMINATE)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_INDETERMINATE);
}

/* Screen rectangle of a whole window. */
void get_extents(HWND hwnd, struct bridge_object *obj)
{
    RECT rect;

    obj->has_extents = GetWindowRect(hwnd, &rect);
    if (!obj->has_extents)
        return;
    obj->x = rect.left;
    obj->y = rect.top;
    obj->width = rect.right - rect.left;
    obj->height = rect.bottom - rect.top;
}

/* rect in the client coordinates of hwnd as x, y, width and height on the screen. */
void client_to_screen(HWND hwnd, RECT rect, int *out)
{
    MapWindowPoints(hwnd, HWND_DESKTOP, (POINT *)&rect, 2);
    out[0] = rect.left;
    out[1] = rect.top;
    out[2] = rect.right - rect.left;
    out[3] = rect.bottom - rect.top;
}

/* Screen rectangle of an item, from rect in the client coordinates of its list. */
void set_item_extents(HWND hwnd, RECT rect, struct bridge_object *obj)
{
    int r[4];

    client_to_screen(hwnd, rect, r);
    obj->has_extents = TRUE;
    obj->x = r[0];
    obj->y = r[1];
    obj->width = r[2];
    obj->height = r[3];
}

/*
 * Drops showing from an item of list hwnd that has no rectangle or lies
 * outside the list's client area: scrolled away by the mouse wheel or by
 * a key that keeps the focus (Ctrl+Page Down in a tree view). Orca's flat
 * review skips objects that are not showing (ax_utilities.py,
 * is_on_screen). Call it after the states are set.
 */
void clip_item_showing(HWND hwnd, struct bridge_object *obj)
{
    RECT client, item;

    if (obj->has_extents)
    {
        GetClientRect(hwnd, &client);
        MapWindowPoints(hwnd, HWND_DESKTOP, (POINT *)&client, 2);
        SetRect(&item, obj->x, obj->y, obj->x + obj->width, obj->y + obj->height);
        if (IntersectRect(&item, &item, &client))
            return;
    }
    obj->states &= ~BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
}

/* MSAA only says "text"; the window style tells entry from multi-line. */
static void refine_text(HWND hwnd, LONG msaa_state, struct bridge_object *obj)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);

    if (!(msaa_state & STATE_SYSTEM_READONLY))
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_EDITABLE);
    if (style & ES_MULTILINE)
    {
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_MULTI_LINE);
        return;
    }
    obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SINGLE_LINE);
    obj->role = (msaa_state & STATE_SYSTEM_PROTECTED) || (style & ES_PASSWORD)
                    ? BRIDGE_ROLE_PASSWORD_TEXT : BRIDGE_ROLE_ENTRY;
}

int bridge_click(unsigned long long handle)
{
    HWND hwnd = (HWND)(ULONG_PTR)handle;
    WCHAR cls[16];

    if (!hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"Button") ||
        (GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX ||
        !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd))
        return 0;
    /*
     * Wine's BM_CLICK sends the button WM_LBUTTONDOWN and WM_LBUTTONUP
     * (user32/button.c), which focus it and notify BN_CLICKED. Posted,
     * because the click may open a modal dialog.
     */
    return PostMessageW(hwnd, BM_CLICK, 0, 0);
}

/* The handler of a control whose items the bridge reads, NULL for other classes. */
const struct item_handler *find_item_handler(const WCHAR *cls)
{
    static const struct { const WCHAR *cls; const struct item_handler *handler; } map[] =
    {
        { L"ComboLBox", &listbox_handler },
        { L"ListBox", &listbox_handler },
        { L"SysListView32", &listview_handler },
        { L"SysTabControl32", &tab_handler },
        { L"SysTreeView32", &treeview_handler },
        { L"ToolbarWindow32", &toolbar_handler },
    };
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(map); i++)
        if (!lstrcmpiW(cls, map[i].cls))
            return map[i].handler;
    return NULL;
}

/*
 * Name, role and states of an MSAA object; the role stays unknown for a
 * plain client. FALSE if MSAA failed.
 */
static BOOL describe_msaa(HWND hwnd, LONG id_object, LONG id_child,
                          struct bridge_object *obj, struct focus_strings *str,
                          LONG *msaa_state)
{
    IAccessible *acc = NULL;
    VARIANT child, role, state;
    BSTR name = NULL;

    /*
     * Wine's oleacc sends WM_GETOBJECT and WM_GETTEXT without a timeout
     * (dlls/oleacc/main.c, client.c), so a busy target would block the
     * bridge. Ask it once with a timeout first.
     */
    if (!send_message(hwnd, WM_NULL, 0, 0, SEND_TIMEOUT_MS, NULL))
        return FALSE;
    VariantInit(&child);
    if (FAILED(AccessibleObjectFromEvent(hwnd, id_object, id_child, &acc, &child)) || !acc)
        return FALSE;

    obj->child = V_VT(&child) == VT_I4 ? V_I4(&child) : 0;
    if (FAILED(IAccessible_get_accName(acc, child, &name)))
        name = NULL;
    to_utf8(name, str->object, sizeof(str->object));
    VariantInit(&role);
    IAccessible_get_accRole(acc, child, &role);
    obj->role = BRIDGE_ROLE_UNKNOWN;
    if (V_VT(&role) == VT_I4 && V_I4(&role) != ROLE_SYSTEM_CLIENT)
        obj->role = map_role(V_I4(&role));
    VariantInit(&state);
    if (SUCCEEDED(IAccessible_get_accState(acc, child, &state)) && V_VT(&state) == VT_I4)
        *msaa_state = V_I4(&state);

    SysFreeString(name);
    VariantClear(&role);
    VariantClear(&state);
    VariantClear(&child);
    IAccessible_Release(acc);
    return TRUE;
}

/*
 * What Wine's oleacc gives for a whole window of a standard class, without
 * MSAA: it has no proxy for them (only the Edit has its own vtbl in
 * dlls/oleacc/client.c), and its WM_GETOBJECT and WM_GETTEXT have no
 * timeout. Name from the window text without the "&" of the mnemonic
 * (an Edit is named by its Static), states from the window style. The role
 * is left unknown, role_from_class gives it. FALSE if the target is busy.
 */
static BOOL describe_window(HWND hwnd, const WCHAR *cls, struct bridge_object *obj,
                            struct focus_strings *str, LONG *msaa_state)
{
    GUITHREADINFO info = { sizeof(info) };
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    WCHAR buf[NAME_MAX_UTF8 / 3];
    BOOL edit = !lstrcmpiW(cls, L"Edit");

    obj->child = 0;
    obj->role = BRIDGE_ROLE_UNKNOWN;
    str->object[0] = 0;
    buf[0] = 0;
    if (has_edit_text(cls))
    {
        /*
         * No message: a RichEdit's window text is the whole document, which
         * is not its name, and the text read in describe_object tells a busy
         * target (the Open dialog's file name field while the dialog is
         * created), which then fails and is asked again. A WM_NULL first
         * cost as much as half the read of a small text.
         */
        /* The Edit of a combo box is unnamed, see describe_object. */
        if (edit && !outer_combo(hwnd))
            edit_label(hwnd, str->object, sizeof(str->object));
    }
    else if (!lstrcmpiW(cls, L"Static") || !lstrcmpiW(cls, L"Button"))
    {
        /* They keep their text in the window, so no message is needed. */
        InternalGetWindowText(hwnd, buf, ARRAYSIZE(buf));
        strip_mnemonic(hwnd, buf);
    }
    else
    {
        if (!get_class_text(hwnd, cls, buf, ARRAYSIZE(buf)))
            return FALSE;
        /* A combo box's text becomes its value, see describe_object. */
        if (!is_combo_class(cls))
            strip_ampersands(buf);
    }
    if (buf[0])
        to_utf8(buf, str->object, sizeof(str->object));

    /* As client_get_state and edit_get_state in dlls/oleacc/client.c. */
    *msaa_state = style & WS_DISABLED ? STATE_SYSTEM_UNAVAILABLE : STATE_SYSTEM_FOCUSABLE;
    if (GetGUIThreadInfo(0, &info) && info.hwndFocus == hwnd)
        *msaa_state |= STATE_SYSTEM_FOCUSED;
    if (!(style & WS_VISIBLE))
        *msaa_state |= STATE_SYSTEM_INVISIBLE;
    if (edit && (style & ES_READONLY))
        *msaa_state |= STATE_SYSTEM_READONLY;
    if (edit && (style & ES_PASSWORD))
        *msaa_state |= STATE_SYSTEM_PROTECTED;
    return TRUE;
}

/*
 * Fills obj from the window class and, for other classes and objects
 * inside a window, from MSAA; the strings point into str. FALSE if the
 * target is busy or MSAA failed.
 */
BOOL describe_object(HWND hwnd, LONG id_object, LONG id_child,
                            struct bridge_object *obj, struct focus_strings *str)
{
    const struct item_handler *handler = NULL;
    LONG msaa_state = 0;
    WCHAR cls[256];
    HWND updown;
    BOOL ok;

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    obj->window_class = str->object_class;
    obj->text = NULL;
    str->text = NULL;
    obj->position = obj->set_size = obj->level = obj->column_count = 0;
    obj->columns = NULL;
    obj->has_range = FALSE;
    obj->has_extents = FALSE;
    if (id_object == OBJID_CLIENT && !lstrcmpW(cls, CONSOLE_CLASS))
        return describe_console(hwnd, obj, str);
    if (id_object == OBJID_CLIENT && (handler = find_item_handler(cls)))
    {
        int ret = handler->describe_focus(hwnd, id_child, obj, str);

        if (ret == LIST_BUSY)
            return FALSE;
        if (ret)
            return TRUE;
        id_child = CHILDID_SELF;
    }
    /*
     * A program may answer WM_GETOBJECT for a window of its own class, so
     * MSAA stays for those. A standard class answers nothing of its own.
     */
    if (id_object == OBJID_CLIENT && id_child == CHILDID_SELF &&
        role_from_class(hwnd, cls) != BRIDGE_ROLE_UNKNOWN)
        ok = describe_window(hwnd, cls, obj, str, &msaa_state);
    else
        ok = describe_msaa(hwnd, id_object, id_child, obj, str, &msaa_state);
    if (!ok)
        return FALSE;

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->name = str->object;
    obj->value = NULL;
    obj->states = map_states(msaa_state);
    if (obj->role == BRIDGE_ROLE_UNKNOWN && !obj->child)
    {
        obj->role = role_from_class(hwnd, cls);
        refine_class_states(hwnd, obj);
    }
    /*
     * A top-level window focused itself (walk's main window while its
     * modal box is up) is the frame node; keep the frame's role for it.
     */
    if (obj->role == BRIDGE_ROLE_UNKNOWN && !obj->child && GetAncestor(hwnd, GA_ROOT) == hwnd)
        obj->role = !lstrcmpW(cls, L"#32770") ? BRIDGE_ROLE_DIALOG : BRIDGE_ROLE_FRAME;
    if (obj->role == BRIDGE_ROLE_UNKNOWN)
        obj->role = BRIDGE_ROLE_PANEL;
    if (obj->role == BRIDGE_ROLE_TEXT)
        refine_text(hwnd, msaa_state, obj);
    if (!obj->child && obj->role == BRIDGE_ROLE_ENTRY && (updown = find_updown(hwnd)))
    {
        obj->role = BRIDGE_ROLE_SPIN_BUTTON;
        describe_range(updown, L"msctls_updown32", obj);
    }
    else if (!obj->child && (obj->role == BRIDGE_ROLE_SLIDER ||
                             obj->role == BRIDGE_ROLE_PROGRESS_BAR ||
                             obj->role == BRIDGE_ROLE_SPIN_BUTTON))
        describe_range(hwnd, cls, obj);
    if (!obj->child && has_edit_text(cls) &&
        (obj->role == BRIDGE_ROLE_TEXT || obj->role == BRIDGE_ROLE_ENTRY ||
         obj->role == BRIDGE_ROLE_PASSWORD_TEXT || obj->role == BRIDGE_ROLE_SPIN_BUTTON))
    {
        BOOL rich = lstrcmpiW(cls, L"Edit") != 0;

        /*
         * Wine's oleacc has no proxy for a RichEdit and names it by its
         * window text, the whole document. Orca presents every name change
         * of the focused object, so it would read the text after each key.
         */
        if (rich)
            str->object[0] = 0;
        if (!describe_edit_text(hwnd, rich, obj, str) && is_busy(hwnd))
            return FALSE;
    }
    if (obj->role == BRIDGE_ROLE_COMBO_BOX)
    {
        /*
         * Wine's oleacc names a combo box by its current text. Orca reads a
         * combo box value from "valuetext" and ignores name changes of a
         * focused combo box, so the text is the value, not the name.
         */
        lstrcpyA(str->value, str->object);
        obj->value = str->value;
        str->object[0] = 0;
    }
    /*
     * The Edit of a combo box is its child and stays unnamed, as the entry
     * of a GTK combo box: the combo box has the label, and Orca says
     * "editable combo box" for it as an ancestor of the entry.
     */
    if (!str->object[0] && !obj->child && needs_label(obj->role) &&
        !(!lstrcmpiW(cls, L"Edit") && outer_combo(hwnd)))
        find_label(hwnd, str->object, sizeof(str->object));
    if (handler && !obj->child)
        handler->describe_self(hwnd, obj, str);
    if (!obj->child)
        get_extents(hwnd, obj);
    return TRUE;
}

/*
 * The control as parent of its focused item, so that Orca presents the
 * control's label when focus enters it. No MSAA: Wine's oleacc names a
 * list only by its window text. A tree view is a tree; its items are its
 * children in the order they had focus, nesting is told by their level.
 * obj stays cleared for a window without an item handler.
 */
void describe_container(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    const struct item_handler *handler;
    WCHAR cls[256];

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    memset(obj, 0, sizeof(*obj));
    if (!(handler = find_item_handler(cls)))
        return;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->window_class = str->object_class;
    if (handler->named_by_text)
    {
        WCHAR buf[NAME_MAX_UTF8 / 3];

        get_class_text(hwnd, cls, buf, ARRAYSIZE(buf));
        to_utf8(buf, str->container, sizeof(str->container));
    }
    else
        find_label(hwnd, str->container, sizeof(str->container));
    obj->name = str->container;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE);
    handler->describe_self(hwnd, obj, str);
    get_extents(hwnd, obj);
}

void describe_frame(HWND hwnd, struct bridge_object *obj,
                           struct focus_strings *str)
{
    WCHAR cls[256];

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = 0;
    get_class(hwnd, cls, ARRAYSIZE(cls), str->frame_class, sizeof(str->frame_class));
    obj->window_class = str->frame_class;
    obj->role = !lstrcmpW(cls, L"#32770") ? BRIDGE_ROLE_DIALOG : BRIDGE_ROLE_FRAME;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    get_frame_text(hwnd, str->frame, sizeof(str->frame));
    obj->name = str->frame;
    get_extents(hwnd, obj);
}

/*
 * Title of a top-level window. DefWindowProc keeps a window's text on
 * the server (WM_NCCREATE and WM_SETTEXT, win32u/defwnd.c), and
 * InternalGetWindowText reads it from there for a window of another
 * process without a message, so a target busy creating the window does
 * not delay its focus (winecfg's frame waited 190 ms for WM_GETTEXT).
 * A window that keeps its text itself has none there and is asked.
 */
void get_frame_text(HWND hwnd, char *dst, int size)
{
    WCHAR buf[NAME_MAX_UTF8 / 3];

    if (InternalGetWindowText(hwnd, buf, ARRAYSIZE(buf)) > 0)
        to_utf8(buf, dst, size);
    else
        get_window_text(hwnd, dst, size);
}
