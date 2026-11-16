/*
 * wine-atspi bridge, Win32 core: labels of controls and the texts of a
 * window that label no control.
 */
#include "core.h"

/* Roles that take their name from a label instead of their own text. */
BOOL needs_label(unsigned int role)
{
    switch (role)
    {
    case BRIDGE_ROLE_ENTRY:
    case BRIDGE_ROLE_TEXT:
    case BRIDGE_ROLE_PASSWORD_TEXT:
    case BRIDGE_ROLE_COMBO_BOX:
    case BRIDGE_ROLE_LIST:
    case BRIDGE_ROLE_TREE:
    case BRIDGE_ROLE_TABLE:
    case BRIDGE_ROLE_SLIDER:
    case BRIDGE_ROLE_SPIN_BUTTON:
    case BRIDGE_ROLE_PROGRESS_BAR:
    case BRIDGE_ROLE_PAGE_TAB_LIST:
        return TRUE;
    }
    return FALSE;
}

/* A Static that shows text, not an icon, bitmap, frame or rectangle. */
static BOOL is_text_static(HWND hwnd)
{
    switch (GetWindowLongW(hwnd, GWL_STYLE) & SS_TYPEMASK)
    {
    case SS_LEFT:
    case SS_CENTER:
    case SS_RIGHT:
    case SS_SIMPLE:
    case SS_LEFTNOWORDWRAP:
    case SS_OWNERDRAW:
        return TRUE;
    }
    return FALSE;
}

/*
 * Name of an unnamed control from the Static before it in the dialog, as
 * oleacc does on Windows. Wine's oleacc does this only for a bare Edit,
 * not for the Edit inside a combo box or for other controls. The label
 * must be the nearest visible sibling before the control (hidden windows
 * and empty or picture Statics are skipped); the "&" of the mnemonic is
 * dropped.
 */
void find_label(HWND hwnd, char *dst, int size)
{
    WCHAR cls[64], buf[NAME_MAX_UTF8 / 3];
    HWND parent, cur;

    dst[0] = 0;
    while ((parent = GetParent(hwnd)) && GetClassNameW(parent, cls, ARRAYSIZE(cls)) &&
           (!lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, L"ComboBoxEx32")))
        hwnd = parent;
    for (cur = GetWindow(hwnd, GW_HWNDPREV); cur; cur = GetWindow(cur, GW_HWNDPREV))
    {
        if (!(GetWindowLongW(cur, GWL_STYLE) & WS_VISIBLE))
            continue;
        if (!GetClassNameW(cur, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"Static"))
            return;
        if (!is_text_static(cur))
            continue;
        /*
         * No WM_GETTEXT: the dialog thread may be busy (the Open dialog
         * refills its file list after a file type change) and a timeout
         * would lose the label. A Static keeps its text in the window.
         */
        if (!InternalGetWindowText(cur, buf, ARRAYSIZE(buf)))
            continue;
        break;
    }
    if (!cur)
        return;
    strip_mnemonic(cur, buf);
    to_utf8(buf, dst, size);
}

/*
 * A text Static that names the control after it, as find_label sees it:
 * the nearest visible sibling after it that is not a picture or empty
 * Static is a control that takes its name from a label.
 */
static BOOL is_label_of_control(HWND hwnd)
{
    WCHAR cls[64], buf[2];
    HWND cur;

    for (cur = GetWindow(hwnd, GW_HWNDNEXT); cur; cur = GetWindow(cur, GW_HWNDNEXT))
    {
        if (!(GetWindowLongW(cur, GWL_STYLE) & WS_VISIBLE))
            continue;
        if (!GetClassNameW(cur, cls, ARRAYSIZE(cls)))
            return FALSE;
        if (lstrcmpiW(cls, L"Static"))
            return needs_label(role_from_class(cur, cls));
        if (is_text_static(cur) && InternalGetWindowText(cur, buf, ARRAYSIZE(buf)))
            return FALSE;
    }
    return FALSE;
}

struct label_search
{
    struct bridge_focus *focus;
    struct focus_strings *str;
};

/*
 * Describes hwnd as a label if it is a visible text Static that names no
 * control; its text goes to dst. FALSE for any other window.
 */
BOOL describe_label(HWND hwnd, char *dst, int size, struct bridge_object *obj)
{
    WCHAR cls[64], buf[LABEL_MAX_UTF8 / 3];

    if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
        lstrcmpiW(cls, L"Static") || !is_text_static(hwnd))
        return FALSE;
    /* No WM_GETTEXT, see find_label. */
    if (!InternalGetWindowText(hwnd, buf, ARRAYSIZE(buf)) || is_label_of_control(hwnd))
        return FALSE;
    strip_mnemonic(hwnd, buf);
    to_utf8(buf, dst, size);
    memset(obj, 0, sizeof(*obj));
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->role = BRIDGE_ROLE_LABEL;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    obj->name = dst;
    obj->window_class = "Static";
    get_extents(hwnd, obj);
    return TRUE;
}

static BOOL CALLBACK add_label(HWND hwnd, LPARAM param)
{
    struct label_search *search = (struct label_search *)param;
    int n = search->focus->label_count;

    if (n >= MAX_LABELS)
        return FALSE;
    if (describe_label(hwnd, search->str->labels[n], sizeof(search->str->labels[n]),
                       &search->str->label_objects[n]))
        search->focus->label_count = n + 1;
    return TRUE;
}

/*
 * The texts of top-level window top that name no control, such as the
 * message of a message box. Orca reads them when focus enters a dialog
 * (speech_generator.py, _generate_dialog: unrelated_labels).
 */
void describe_labels(HWND top, struct bridge_focus *focus, struct focus_strings *str)
{
    struct label_search search = { focus, str };

    focus->label_count = 0;
    focus->labels = str->label_objects;
    EnumChildWindows(top, add_label, (LPARAM)&search);
}
