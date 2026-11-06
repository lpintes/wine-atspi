/*
 * wine-atspi bridge: Win32 core.
 *
 * Windows headers only, no Linux headers. Starts the AT-SPI side on its
 * own thread and runs the Win32 message loop on the main thread. A
 * WinEvent hook reports focus changes; each one is described through
 * MSAA and handed to the AT-SPI side as a plain snapshot.
 */
#define COBJMACROS
#include <windows.h>
/* Wine's libuuid lacks IID_IAccessible, so define it here. */
#include <initguid.h>
#include <oleacc.h>

#include "bridge.h"

#define NAME_MAX_UTF8 1024

static BOOL debug;

/* Writes a line to stderr when WINE_ATSPI_DEBUG is set. */
static void WINAPIV trace(const WCHAR *format, ...)
{
    WCHAR buf[1024];
    char utf8[3072];
    va_list args;
    DWORD written;
    int len;

    if (!debug)
        return;
    va_start(args, format);
    wvsprintfW(buf, format, args);
    va_end(args);
    len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (len <= 0)
        return;
    utf8[len - 1] = '\n';
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), utf8, len, &written, NULL);
}

/* UTF-8 buffers backing the strings of one bridge_focus. */
struct focus_strings
{
    char app[MAX_PATH * 3];
    char frame[NAME_MAX_UTF8];
    char object[NAME_MAX_UTF8];
    char value[NAME_MAX_UTF8];
    char frame_class[256 * 3];
    char object_class[256 * 3];
    char *text;     /* heap, see free_strings */
};

static void free_strings(struct focus_strings *str)
{
    HeapFree(GetProcessHeap(), 0, str->text);
    str->text = NULL;
}

static void to_utf8(const WCHAR *src, char *dst, int size)
{
    if (!src || !WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, size, NULL, NULL))
        dst[0] = 0;
    dst[size - 1] = 0;
}

/* GetWindowText does not ask windows of other processes, WM_GETTEXT does. */
static void get_window_text(HWND hwnd, char *dst, int size)
{
    WCHAR buf[NAME_MAX_UTF8 / 3];
    DWORD_PTR res;

    buf[0] = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, ARRAYSIZE(buf), (LPARAM)buf,
                             SMTO_ABORTIFHUNG, 500, &res))
        buf[0] = 0;
    buf[ARRAYSIZE(buf) - 1] = 0;
    to_utf8(buf, dst, size);
}

/* Executable name without directory and ".exe", e.g. "notepad". */
static void get_app_name(DWORD pid, char *dst, int size)
{
    WCHAR path[MAX_PATH], *base, *dot;
    DWORD len = ARRAYSIZE(path);
    HANDLE process;

    path[0] = 0;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process)
    {
        if (!QueryFullProcessImageNameW(process, 0, path, &len))
            path[0] = 0;
        CloseHandle(process);
    }
    base = path;
    for (dot = path; *dot; dot++)
        if (*dot == '\\' || *dot == '/')
            base = dot + 1;
    dot = base + lstrlenW(base) - 4;
    if (dot > base && !lstrcmpiW(dot, L".exe"))
        *dot = 0;
    if (!*base)
        wsprintfW(path, L"pid %lu", pid), base = path;
    to_utf8(base, dst, size);
}

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

/*
 * Wine's oleacc has no proxy for most standard controls and calls them
 * "client". For a whole window the class tells more.
 */
static unsigned int role_from_class(HWND hwnd, const WCHAR *cls)
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
    SendMessageTimeoutW(hwnd, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG, 500, &check);
    if (check == BST_CHECKED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_CHECKED);
    else if (check == BST_INDETERMINATE)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_INDETERMINATE);
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

/* Text beyond this many UTF-16 units is cut off. */
#define TEXT_MAX_CHARS (1 << 20)

/*
 * Character offset of UTF-16 position pos in text as the AT-SPI side sees
 * it: surrogate pairs count once and the "\r" of "\r\n" is dropped.
 */
static int char_offset(const WCHAR *text, int len, int pos)
{
    int i, offset = 0;

    for (i = 0; i < pos && i < len; i++)
    {
        if (IS_LOW_SURROGATE(text[i]) && i > 0 && IS_HIGH_SURROGATE(text[i - 1]))
            continue;
        if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
            continue;
        offset++;
    }
    return offset;
}

/*
 * EM_GETSEL orders start and end, so it does not tell at which end the
 * caret is. Compare the caret rectangle with the position of the start.
 */
static BOOL caret_at_start(HWND hwnd, DWORD start)
{
    GUITHREADINFO gti;
    DWORD_PTR pos;

    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti) ||
        gti.hwndCaret != hwnd ||
        !SendMessageTimeoutW(hwnd, EM_POSFROMCHAR, start, 0, SMTO_ABORTIFHUNG, 500, &pos) ||
        pos == (DWORD_PTR)-1)
        return FALSE;
    return (short)LOWORD(pos) == gti.rcCaret.left && (short)HIWORD(pos) == gti.rcCaret.top;
}

/* Text, caret and selection of an Edit or RichEdit; str->text owns the text. */
static void describe_edit_text(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    DWORD_PTR len = 0;
    DWORD start = 0, end = 0;
    WCHAR *buf, *src, *dst;
    int size, i, n;

    if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 500, &len))
        return;
    if (len > TEXT_MAX_CHARS)
        len = TEXT_MAX_CHARS;
    buf = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buf)
        return;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, len + 1, (LPARAM)buf, SMTO_ABORTIFHUNG, 500, &len))
        len = 0;
    buf[len] = 0;
    n = len;
    /* Pointer arguments of EM_GETSEL are marshalled like on Windows. */
    SendMessageTimeoutW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end, SMTO_ABORTIFHUNG, 500, NULL);
    obj->selection_start = char_offset(buf, n, start);
    obj->selection_end = char_offset(buf, n, end);
    obj->caret = start != end && caret_at_start(hwnd, start)
                     ? obj->selection_start : obj->selection_end;

    if (obj->role == BRIDGE_ROLE_PASSWORD_TEXT)
    {
        /* Like GTK, show the hidden text as one mask character each. */
        for (i = 0; i < n; i++)
            buf[i] = '*';
    }
    for (src = dst = buf; *src; src++)
        if (src[0] != '\r' || src[1] != '\n')
            *dst++ = *src;
    *dst = 0;
    size = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    str->text = HeapAlloc(GetProcessHeap(), 0, size > 0 ? size : 1);
    if (str->text)
    {
        str->text[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, str->text, size, NULL, NULL);
        obj->text = str->text;
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

static BOOL has_edit_text(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, L"RICHEDIT50W") ||
           !lstrcmpiW(cls, L"RichEdit20A") || !lstrcmpiW(cls, L"RichEdit20W");
}

static void get_class(HWND hwnd, WCHAR *cls, int len, char *dst, int size)
{
    if (!GetClassNameW(hwnd, cls, len))
        cls[0] = 0;
    to_utf8(cls, dst, size);
}

/* Roles that take their name from a label instead of their own text. */
static BOOL needs_label(unsigned int role)
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
static void find_label(HWND hwnd, char *dst, int size)
{
    WCHAR cls[64], buf[NAME_MAX_UTF8 / 3];
    HWND parent, cur;
    int i, j;

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
    if (!(GetWindowLongW(cur, GWL_STYLE) & SS_NOPREFIX))
    {
        for (i = j = 0; buf[i]; i++)
        {
            if (buf[i] == '&')
            {
                if (buf[i + 1] != '&')
                    continue;
                i++;
            }
            buf[j++] = buf[i];
        }
        buf[j] = 0;
    }
    to_utf8(buf, dst, size);
}

/*
 * Fills obj from MSAA and the window class; the strings point into str.
 * FALSE if MSAA failed.
 */
static BOOL describe_object(HWND hwnd, LONG id_object, LONG id_child,
                            struct bridge_object *obj, struct focus_strings *str)
{
    IAccessible *acc = NULL;
    VARIANT child, role, state;
    BSTR name = NULL;
    LONG msaa_state = 0;
    WCHAR cls[256];

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    obj->window_class = str->object_class;
    obj->text = NULL;
    str->text = NULL;
    VariantInit(&child);
    if (FAILED(AccessibleObjectFromEvent(hwnd, id_object, id_child, &acc, &child)) || !acc)
        return FALSE;

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = V_VT(&child) == VT_I4 ? V_I4(&child) : 0;
    if (FAILED(IAccessible_get_accName(acc, child, &name)))
        name = NULL;
    to_utf8(name, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->value = NULL;
    VariantInit(&role);
    IAccessible_get_accRole(acc, child, &role);
    obj->role = BRIDGE_ROLE_UNKNOWN;
    if (V_VT(&role) == VT_I4 && V_I4(&role) != ROLE_SYSTEM_CLIENT)
        obj->role = map_role(V_I4(&role));
    VariantInit(&state);
    if (SUCCEEDED(IAccessible_get_accState(acc, child, &state)) && V_VT(&state) == VT_I4)
        msaa_state = V_I4(&state);
    obj->states = map_states(msaa_state);
    if (obj->role == BRIDGE_ROLE_UNKNOWN && !obj->child)
    {
        obj->role = role_from_class(hwnd, cls);
        refine_class_states(hwnd, obj);
    }
    if (obj->role == BRIDGE_ROLE_UNKNOWN)
        obj->role = BRIDGE_ROLE_PANEL;
    if (obj->role == BRIDGE_ROLE_TEXT)
        refine_text(hwnd, msaa_state, obj);
    if (!obj->child && has_edit_text(cls) &&
        (obj->role == BRIDGE_ROLE_TEXT || obj->role == BRIDGE_ROLE_ENTRY ||
         obj->role == BRIDGE_ROLE_PASSWORD_TEXT))
        describe_edit_text(hwnd, obj, str);
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
    if (!str->object[0] && !obj->child && needs_label(obj->role))
        find_label(hwnd, str->object, sizeof(str->object));

    SysFreeString(name);
    VariantClear(&role);
    VariantClear(&state);
    VariantClear(&child);
    IAccessible_Release(acc);
    return TRUE;
}

static void describe_frame(HWND hwnd, struct bridge_object *obj,
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
    get_window_text(hwnd, str->frame, sizeof(str->frame));
    obj->name = str->frame;
}

/*
 * Wine notifies focus on a window before sending it WM_SETFOCUS, whose
 * handler often passes focus on to a child (win32u/input.c,
 * set_focus_window). The out-of-context event for the parent can then
 * arrive after the child's. Like NVDA, drop a window focus event when
 * the window no longer has focus.
 */
static BOOL is_stale_focus(HWND hwnd, LONG id_object, LONG id_child)
{
    GUITHREADINFO gti;

    if (id_object != OBJID_CLIENT || id_child != CHILDID_SELF)
        return FALSE;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti))
        return FALSE;
    return gti.hwndFocus && gti.hwndFocus != hwnd;
}

/*
 * Processes whose application is on the AT-SPI side. A thread pool wait
 * tells the main thread with WM_PROCESS_EXITED when one of them ends.
 */
#define WM_PROCESS_EXITED (WM_APP + 1)
#define MAX_WATCHED 256

static DWORD main_thread_id;
static struct
{
    DWORD pid;
    HANDLE process;
    HANDLE wait;
} watched[MAX_WATCHED];

static void CALLBACK process_exited(void *arg, BOOLEAN timed_out)
{
    PostThreadMessageW(main_thread_id, WM_PROCESS_EXITED, (WPARAM)arg, 0);
}

static void watch_process(DWORD pid)
{
    int i, free_slot = -1;
    HANDLE process;

    for (i = 0; i < MAX_WATCHED; i++)
    {
        if (watched[i].pid == pid)
            return;
        if (!watched[i].pid && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return;
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return;
    if (!RegisterWaitForSingleObject(&watched[free_slot].wait, process, process_exited,
                                     (void *)(ULONG_PTR)pid, INFINITE, WT_EXECUTEONLYONCE))
    {
        CloseHandle(process);
        return;
    }
    watched[free_slot].pid = pid;
    watched[free_slot].process = process;
}

static void on_process_exited(DWORD pid)
{
    int i;

    for (i = 0; i < MAX_WATCHED; i++)
    {
        if (watched[i].pid != pid)
            continue;
        UnregisterWait(watched[i].wait);
        CloseHandle(watched[i].process);
        watched[i].pid = 0;
    }
    trace(L"process %lu exited", pid);
    atspi_post_app_gone(pid);
}

/* The object of the last focus event, checked again after each key. */
static struct
{
    HWND hwnd;
    LONG id_object;
    LONG id_child;
    DWORD pid;
    BOOL recovered;     /* reported by check_missed_focus, not by an event */
} last_focus;

static void on_focus(HWND hwnd, LONG id_object, LONG id_child)
{
    struct bridge_focus focus;
    struct focus_strings str;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    DWORD pid = 0;

    if (is_stale_focus(hwnd, id_object, id_child))
        return;
    if (!top)
        top = hwnd;
    memset(&focus, 0, sizeof(focus));
    if (!describe_object(hwnd, id_object, id_child, &focus.object, &str))
        return;
    describe_frame(top, &focus.frame, &str);
    GetWindowThreadProcessId(hwnd, &pid);
    focus.pid = pid;
    get_app_name(pid, str.app, sizeof(str.app));
    focus.app_name = str.app;
    watch_process(pid);
    atspi_post_focus(&focus);
    trace(L"focus: %p %ld %ld name \"%S\"", hwnd, id_object, id_child, focus.object.name);
    free_strings(&str);

    last_focus.hwnd = hwnd;
    last_focus.id_object = id_object;
    last_focus.id_child = id_child;
    last_focus.pid = pid;
}

/*
 * Wine's ListBox moves its caret on arrow keys without a focus event
 * (listbox.c, LISTBOX_HandleKeyDown). Reports the new item as focused;
 * FALSE if hwnd is no list box or the caret did not move.
 */
static BOOL check_list_caret(HWND hwnd)
{
    WCHAR cls[256];
    DWORD_PTR caret = LB_ERR;

    if (last_focus.id_object != OBJID_CLIENT || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
        (lstrcmpiW(cls, L"ListBox") && lstrcmpiW(cls, L"ComboLBox")))
        return FALSE;
    if (!SendMessageTimeoutW(hwnd, LB_GETCARETINDEX, 0, 0, SMTO_ABORTIFHUNG, 500, &caret) ||
        (LONG)caret == LB_ERR || (LONG)caret + 1 == last_focus.id_child)
        return FALSE;
    on_focus(hwnd, OBJID_CLIENT, (LONG)caret + 1);
    return TRUE;
}

/*
 * A focus event can get lost: the Save changes? message box after Alt+F4
 * reported none (WSLg, 2026-11-06), so Orca stayed silent until the next
 * key. Like NVDA, compare the real focus of the foreground thread with the
 * last reported one and report it if they differ. The late event, if it
 * still comes, is then dropped in win_event_proc.
 */
static BOOL check_missed_focus(void)
{
    HWND foreground = GetForegroundWindow();
    GUITHREADINFO gti;

    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!foreground || !GetGUIThreadInfo(GetWindowThreadProcessId(foreground, NULL), &gti) ||
        (gti.flags & GUI_INMENUMODE) || !gti.hwndFocus || gti.hwndFocus == last_focus.hwnd)
        return FALSE;
    trace(L"missed focus: %p", gti.hwndFocus);
    on_focus(gti.hwndFocus, OBJID_CLIENT, CHILDID_SELF);
    if (last_focus.hwnd != gti.hwndFocus)
        return FALSE;
    last_focus.recovered = TRUE;
    return TRUE;
}

/* Delay after a foreground change, so the new window has set its focus. */
#define FOREGROUND_CHECK_DELAY_MS 100

static UINT_PTR foreground_timer;

static void CALLBACK check_after_foreground(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, id);
    foreground_timer = 0;
    check_missed_focus();
}

/*
 * Wine emits no WinEvent when a key changes a combo box, a list box
 * selection or a user32 Edit. Like NVDA, look at the focused object again
 * shortly after each key, once the target has handled it.
 */
#define KEY_CHECK_DELAY_MS 50

static UINT_PTR key_timer;

static void CALLBACK check_after_key(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    struct bridge_object obj;
    struct focus_strings str;
    GUITHREADINFO gti;
    HWND hwnd = last_focus.hwnd;

    KillTimer(NULL, id);
    key_timer = 0;
    if (!hwnd || !IsWindow(hwnd))
    {
        check_missed_focus();
        return;
    }
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    /* Focus moved elsewhere: normally its own focus event tells. */
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti) || gti.hwndFocus != hwnd)
    {
        trace(L"key check: focus %p is not %p", gti.hwndFocus, hwnd);
        check_missed_focus();
        return;
    }
    if (check_list_caret(hwnd))
        return;
    memset(&obj, 0, sizeof(obj));
    if (!describe_object(hwnd, last_focus.id_object, last_focus.id_child, &obj, &str))
        return;
    /*
     * describe_object waits for the target, and a focus event for another
     * window can be handled meanwhile (seen with the message box after
     * Alt+F4). Do not send a stale update after the new focus.
     */
    if (hwnd != last_focus.hwnd)
    {
        trace(L"key check: focus moved to %p meanwhile", last_focus.hwnd);
        free_strings(&str);
        return;
    }
    trace(L"key check: %p %ld %ld name \"%S\" value \"%S\" caret %d", hwnd,
          last_focus.id_object, last_focus.id_child, obj.name, obj.value ? obj.value : "",
          obj.text ? obj.caret : -1);
    atspi_post_update(last_focus.pid, &obj);
    free_strings(&str);
}

/* X keysym and name of keys that type no text. */
static const struct { BYTE vk; unsigned int keysym; const char *name; } special_keys[] =
{
    { VK_BACK, 0xff08, "BackSpace" }, { VK_TAB, 0xff09, "Tab" },
    { VK_RETURN, 0xff0d, "Return" }, { VK_PAUSE, 0xff13, "Pause" },
    { VK_ESCAPE, 0xff1b, "Escape" }, { VK_HOME, 0xff50, "Home" },
    { VK_LEFT, 0xff51, "Left" }, { VK_UP, 0xff52, "Up" },
    { VK_RIGHT, 0xff53, "Right" }, { VK_DOWN, 0xff54, "Down" },
    { VK_PRIOR, 0xff55, "Prior" }, { VK_NEXT, 0xff56, "Next" },
    { VK_END, 0xff57, "End" }, { VK_INSERT, 0xff63, "Insert" },
    { VK_APPS, 0xff67, "Menu" }, { VK_NUMLOCK, 0xff7f, "Num_Lock" },
    { VK_SCROLL, 0xff14, "Scroll_Lock" }, { VK_DELETE, 0xffff, "Delete" },
    { VK_LSHIFT, 0xffe1, "Shift_L" }, { VK_RSHIFT, 0xffe2, "Shift_R" },
    { VK_LCONTROL, 0xffe3, "Control_L" }, { VK_RCONTROL, 0xffe4, "Control_R" },
    { VK_CAPITAL, 0xffe5, "Caps_Lock" }, { VK_LMENU, 0xffe9, "Alt_L" },
    /* Right Alt is AltGr on most non-US layouts, Slovak included. */
    { VK_RMENU, 0xfe03, "ISO_Level3_Shift" }, { VK_LWIN, 0xffeb, "Super_L" },
    { VK_RWIN, 0xffec, "Super_R" },
};

/* X modifier masks. */
#define X_SHIFT_MASK 0x01
#define X_CONTROL_MASK 0x04
#define X_MOD1_MASK 0x08    /* Alt */
#define X_MOD4_MASK 0x40    /* Super */
#define X_MOD5_MASK 0x80    /* ISO_Level3_Shift (AltGr) */

static BOOL key_down(int vk)
{
    return GetAsyncKeyState(vk) < 0;
}

/*
 * X keycode (evdev code + 8) from a PC set 1 scan code, which is what
 * Wine puts into KBDLLHOOKSTRUCT. Extended keys have their own codes.
 */
static unsigned int x_keycode(const KBDLLHOOKSTRUCT *kb)
{
    static const struct { BYTE scan; BYTE evdev; } extended[] =
    {
        { 0x1c, 96 }, { 0x1d, 97 }, { 0x35, 98 }, { 0x38, 100 }, { 0x47, 102 },
        { 0x48, 103 }, { 0x49, 104 }, { 0x4b, 105 }, { 0x4d, 106 }, { 0x4f, 107 },
        { 0x50, 108 }, { 0x51, 109 }, { 0x52, 110 }, { 0x53, 111 }, { 0x5b, 125 },
        { 0x5c, 126 }, { 0x5d, 127 },
    };
    unsigned int i;

    if (kb->flags & LLKHF_EXTENDED)
    {
        for (i = 0; i < ARRAYSIZE(extended); i++)
            if (extended[i].scan == kb->scanCode)
                return extended[i].evdev + 8;
    }
    return kb->scanCode + 8;
}

/*
 * Reports the key to the AT-SPI side the way GTK's atk-bridge does, so a
 * screen reader without its own keyboard source learns about it. The
 * text comes from ToUnicodeEx with only Shift and AltGr: control keys
 * should still be named by their letter.
 */
static void report_key(WPARAM msg, const KBDLLHOOKSTRUCT *kb)
{
    struct bridge_key key;
    char utf8[16];
    WCHAR text[4];
    BYTE state[256];
    BOOL altgr = key_down(VK_RMENU);
    unsigned int i;
    int len;

    memset(&key, 0, sizeof(key));
    key.pressed = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    key.keycode = x_keycode(kb);
    key.timestamp = kb->time;
    if (key_down(VK_SHIFT))
        key.modifiers |= X_SHIFT_MASK;
    if (key_down(VK_CONTROL) && !altgr)
        key.modifiers |= X_CONTROL_MASK;
    if (key_down(VK_LMENU))
        key.modifiers |= X_MOD1_MASK;
    if (altgr)
        key.modifiers |= X_MOD5_MASK;
    if (key_down(VK_LWIN) || key_down(VK_RWIN))
        key.modifiers |= X_MOD4_MASK;

    for (i = 0; i < ARRAYSIZE(special_keys); i++)
        if (special_keys[i].vk == kb->vkCode)
            break;
    if (i < ARRAYSIZE(special_keys))
    {
        key.keysym = special_keys[i].keysym;
        key.string = special_keys[i].name;
        /* X sends ISO_Left_Tab for Shift+Tab. */
        if (kb->vkCode == VK_TAB && (key.modifiers & X_SHIFT_MASK))
            key.keysym = 0xfe20, key.string = "ISO_Left_Tab";
    }
    else if (kb->vkCode >= VK_F1 && kb->vkCode <= VK_F24)
    {
        key.keysym = 0xffbe + kb->vkCode - VK_F1;
        wsprintfA(utf8, "F%u", (unsigned int)(kb->vkCode - VK_F1 + 1));
        key.string = utf8;
    }
    else if (kb->vkCode == VK_PACKET)
    {
        /*
         * SendInput with KEYEVENTF_UNICODE: the scan code is the character.
         * The registry drops keycode 0, so take the key of the layout.
         */
        SHORT vk = VkKeyScanW(kb->scanCode);

        text[0] = kb->scanCode;
        text[1] = 0;
        key.keycode = vk == -1 ? 0 : MapVirtualKeyW(LOBYTE(vk), MAPVK_VK_TO_VSC) + 8;
        key.keysym = text[0] < 0x100 ? text[0] : 0x1000000 + text[0];
        to_utf8(text, utf8, sizeof(utf8));
        key.string = utf8;
        key.is_text = TRUE;
    }
    else
    {
        memset(state, 0, sizeof(state));
        if (key.modifiers & X_SHIFT_MASK)
            state[VK_SHIFT] = 0x80;
        if (altgr)
            state[VK_CONTROL] = state[VK_MENU] = 0x80;
        len = ToUnicodeEx(kb->vkCode, kb->scanCode, state, text, ARRAYSIZE(text), 0x4,
                          GetKeyboardLayout(0));
        if (len < 1 || text[0] < 0x20)
            return;
        /* Keysyms equal Latin-1 code points; others are 0x1000000 + code point. */
        key.keysym = text[0] < 0x100 ? text[0] : 0x1000000 + text[0];
        text[1] = 0;
        to_utf8(text, utf8, sizeof(utf8));
        key.string = utf8;
        key.is_text = !(key.modifiers & (X_CONTROL_MASK | X_MOD1_MASK | X_MOD4_MASK));
    }
    atspi_post_key(&key);
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION)
    {
        report_key(wparam, (KBDLLHOOKSTRUCT *)lparam);
        /* The key has not reached the target yet; check after a short delay. */
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)
        {
            trace(L"key %lu", ((KBDLLHOOKSTRUCT *)lparam)->vkCode);
            key_timer = SetTimer(NULL, key_timer, KEY_CHECK_DELAY_MS, check_after_key);
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG id_object, LONG id_child,
                                    DWORD thread, DWORD time)
{
    if (!hwnd)
        return;
    switch (event)
    {
    case EVENT_OBJECT_FOCUS:
        if (last_focus.recovered && hwnd == last_focus.hwnd &&
            id_object == last_focus.id_object && id_child == last_focus.id_child)
        {
            /* Already reported by check_missed_focus. */
            last_focus.recovered = FALSE;
            break;
        }
        last_focus.recovered = FALSE;
        on_focus(hwnd, id_object, id_child);
        break;
    case EVENT_SYSTEM_FOREGROUND:
        foreground_timer = SetTimer(NULL, foreground_timer, FOREGROUND_CHECK_DELAY_MS,
                                    check_after_foreground);
        break;
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_DESTROY:
        /* The window may be gone already: no class, no process to ask. */
        if (id_object == OBJID_WINDOW && id_child == CHILDID_SELF)
            atspi_post_window_gone((ULONG_PTR)hwnd);
        break;
    }
}

static DWORD WINAPI atspi_thread(void *arg)
{
    /*
     * Created with CreateThread, so the thread has a TEB and may call
     * Win32 later; the AT-SPI side itself only uses GLib.
     */
    atspi_run();
    return 0;
}

int main(void)
{
    HWINEVENTHOOK hook, foreground_hook;
    HHOOK keyboard;
    HANDLE thread;
    MSG msg;

    debug = GetEnvironmentVariableW(L"WINE_ATSPI_DEBUG", NULL, 0) > 0;
    main_thread_id = GetCurrentThreadId();
    if (atspi_init())
        return 1;
    thread = CreateThread(NULL, 0, atspi_thread, NULL, 0, NULL);
    if (!thread)
        return 1;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* EVENT_OBJECT_DESTROY .. EVENT_OBJECT_FOCUS includes HIDE. */
    hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_FOCUS, NULL,
                           win_event_proc, 0, 0,
                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook)
        return 1;
    foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                                      win_event_proc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!foreground_hook)
        return 1;
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(NULL), 0);
    if (!keyboard)
        return 1;

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (!msg.hwnd && msg.message == WM_PROCESS_EXITED)
        {
            on_process_exited((DWORD)msg.wParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(keyboard);
    UnhookWinEvent(foreground_hook);
    UnhookWinEvent(hook);
    CoUninitialize();
    return 0;
}
