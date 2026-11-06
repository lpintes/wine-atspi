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
};

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

static void get_class(HWND hwnd, WCHAR *cls, int len, char *dst, int size)
{
    if (!GetClassNameW(hwnd, cls, len))
        cls[0] = 0;
    to_utf8(cls, dst, size);
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

/* The object of the last focus event, checked again after each key. */
static struct
{
    HWND hwnd;
    LONG id_object;
    LONG id_child;
    DWORD pid;
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
    atspi_post_focus(&focus);
    trace(L"focus: %p %ld %ld name \"%S\"", hwnd, id_object, id_child, focus.object.name);

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
        return;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    /* Focus moved elsewhere: its own focus event will tell. */
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti) || gti.hwndFocus != hwnd)
    {
        trace(L"key check: focus %p is not %p", gti.hwndFocus, hwnd);
        return;
    }
    if (check_list_caret(hwnd))
        return;
    memset(&obj, 0, sizeof(obj));
    if (!describe_object(hwnd, last_focus.id_object, last_focus.id_child, &obj, &str))
        return;
    trace(L"key check: %p %ld %ld name \"%S\" value \"%S\"", hwnd, last_focus.id_object,
          last_focus.id_child, obj.name, obj.value ? obj.value : "");
    atspi_post_update(last_focus.pid, &obj);
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
{
    /* The key has not reached the target yet; check after a short delay. */
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
    {
        trace(L"key %lu", ((KBDLLHOOKSTRUCT *)lparam)->vkCode);
        key_timer = SetTimer(NULL, key_timer, KEY_CHECK_DELAY_MS, check_after_key);
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
        on_focus(hwnd, id_object, id_child);
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
    HWINEVENTHOOK hook;
    HHOOK keyboard;
    HANDLE thread;
    MSG msg;

    debug = GetEnvironmentVariableW(L"WINE_ATSPI_DEBUG", NULL, 0) > 0;
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
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(NULL), 0);
    if (!keyboard)
        return 1;

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(keyboard);
    UnhookWinEvent(hook);
    CoUninitialize();
    return 0;
}
