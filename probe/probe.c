/*
 * probe - milestone 1 of wine-atspi.
 *
 * Logs what a Windows screen reader would see in the current Wine prefix:
 * focus and foreground changes (window class, MSAA name and role) and,
 * after each key press, the caret, the menu state and the current line of
 * a focused Edit control. Output is UTF-8 on stdout.
 *
 * Pure Win32: builds with winegcc and with mingw.
 *
 * Usage: probe [-v]
 *   -v  also log every WinEvent, not just focus and foreground
 */

#define COBJMACROS
#include <windows.h>
/* Wine's libuuid lacks IID_IAccessible, so define it here. */
#include <initguid.h>
#include <oleacc.h>
#include <shellapi.h>
#include <stdarg.h>

#ifndef MN_GETHMENU
#define MN_GETHMENU 0x01E1
#endif

#define WM_PROBE_KEY (WM_APP + 1)
#define KEY_DELAY_MS 50

static DWORD main_thread_id;
static DWORD start_tick;
static BOOL verbose;
static UINT_PTR key_timer;

/*
 * CRT stdio is avoided: in a Winelib binary msvcrt and ucrtbase get mixed.
 * wvsprintfW takes %s for WCHAR strings and truncates at 1024 characters.
 * WINAPIV is required: in Winelib on x86_64 va_list is the ms_abi one.
 */
static void WINAPIV logw(const WCHAR *fmt, ...)
{
    WCHAR wbuf[1100];
    char buf[3300];
    va_list args;
    int len, n;
    DWORD written;
    DWORD ms = GetTickCount() - start_tick;

    n = wsprintfW(wbuf, L"%6lu.%03lu ", ms / 1000, ms % 1000);
    va_start(args, fmt);
    len = n + wvsprintfW(wbuf + n, fmt, args);
    va_end(args);
    wbuf[len++] = L'\n';

    n = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, buf, sizeof(buf), NULL, NULL);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &written, NULL);
}

static const WCHAR *event_name(DWORD event)
{
    switch (event)
    {
    case EVENT_SYSTEM_SOUND: return L"system.sound";
    case EVENT_SYSTEM_ALERT: return L"system.alert";
    case EVENT_SYSTEM_FOREGROUND: return L"system.foreground";
    case EVENT_SYSTEM_MENUSTART: return L"system.menuStart";
    case EVENT_SYSTEM_MENUEND: return L"system.menuEnd";
    case EVENT_SYSTEM_MENUPOPUPSTART: return L"system.menuPopupStart";
    case EVENT_SYSTEM_MENUPOPUPEND: return L"system.menuPopupEnd";
    case EVENT_SYSTEM_CAPTURESTART: return L"system.captureStart";
    case EVENT_SYSTEM_CAPTUREEND: return L"system.captureEnd";
    case EVENT_SYSTEM_DIALOGSTART: return L"system.dialogStart";
    case EVENT_SYSTEM_DIALOGEND: return L"system.dialogEnd";
    case EVENT_SYSTEM_SCROLLINGSTART: return L"system.scrollingStart";
    case EVENT_SYSTEM_SCROLLINGEND: return L"system.scrollingEnd";
    case EVENT_SYSTEM_MINIMIZESTART: return L"system.minimizeStart";
    case EVENT_SYSTEM_MINIMIZEEND: return L"system.minimizeEnd";
    case EVENT_OBJECT_CREATE: return L"object.create";
    case EVENT_OBJECT_DESTROY: return L"object.destroy";
    case EVENT_OBJECT_SHOW: return L"object.show";
    case EVENT_OBJECT_HIDE: return L"object.hide";
    case EVENT_OBJECT_REORDER: return L"object.reorder";
    case EVENT_OBJECT_FOCUS: return L"object.focus";
    case EVENT_OBJECT_SELECTION: return L"object.selection";
    case EVENT_OBJECT_SELECTIONADD: return L"object.selectionAdd";
    case EVENT_OBJECT_SELECTIONREMOVE: return L"object.selectionRemove";
    case EVENT_OBJECT_SELECTIONWITHIN: return L"object.selectionWithin";
    case EVENT_OBJECT_STATECHANGE: return L"object.stateChange";
    case EVENT_OBJECT_LOCATIONCHANGE: return L"object.locationChange";
    case EVENT_OBJECT_NAMECHANGE: return L"object.nameChange";
    case EVENT_OBJECT_DESCRIPTIONCHANGE: return L"object.descriptionChange";
    case EVENT_OBJECT_VALUECHANGE: return L"object.valueChange";
    case EVENT_OBJECT_PARENTCHANGE: return L"object.parentChange";
    case EVENT_OBJECT_HELPCHANGE: return L"object.helpChange";
    case EVENT_OBJECT_DEFACTIONCHANGE: return L"object.defactionChange";
    case EVENT_OBJECT_ACCELERATORCHANGE: return L"object.acceleratorChange";
    }
    return NULL;
}

static void get_class(HWND hwnd, WCHAR *buf, int size)
{
    if (!GetClassNameW(hwnd, buf, size))
        lstrcpynW(buf, L"?", size);
}

/* GetWindowText does not ask windows of other processes, WM_GETTEXT does. */
static void get_text(HWND hwnd, WCHAR *buf, int size)
{
    DWORD_PTR res;

    buf[0] = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, size, (LPARAM)buf,
                             SMTO_ABORTIFHUNG, 500, &res))
        buf[0] = 0;
    buf[size - 1] = 0;
}

static void describe_window(const WCHAR *what, HWND hwnd)
{
    WCHAR cls[256], text[256];
    DWORD pid = 0, tid;

    if (!hwnd)
    {
        logw(L"%s: (null)", what);
        return;
    }
    tid = GetWindowThreadProcessId(hwnd, &pid);
    get_class(hwnd, cls, ARRAYSIZE(cls));
    get_text(hwnd, text, ARRAYSIZE(text));
    logw(L"%s: hwnd=%p pid=%lu tid=%lu class=\"%s\" text=\"%s\"",
         what, hwnd, pid, tid, cls, text);
}

static void describe_msaa(HWND hwnd, LONG id_object, LONG id_child)
{
    IAccessible *acc = NULL;
    VARIANT child, role, state;
    BSTR name = NULL, value = NULL;
    WCHAR role_text[128] = L"";
    HRESULT hr;

    VariantInit(&child);
    hr = AccessibleObjectFromEvent(hwnd, id_object, id_child, &acc, &child);
    if (FAILED(hr) || !acc)
    {
        logw(L"  msaa: AccessibleObjectFromEvent failed hr=%08lx", hr);
        return;
    }

    if (FAILED(IAccessible_get_accName(acc, child, &name)))
        name = NULL;
    if (FAILED(IAccessible_get_accValue(acc, child, &value)))
        value = NULL;
    VariantInit(&role);
    if (SUCCEEDED(IAccessible_get_accRole(acc, child, &role)) && V_VT(&role) == VT_I4)
        GetRoleTextW(V_I4(&role), role_text, ARRAYSIZE(role_text));
    VariantInit(&state);
    IAccessible_get_accState(acc, child, &state);

    logw(L"  msaa: name=\"%s\" role=%ld (%s) state=%08lx value=\"%s\"",
         name ? name : L"", V_VT(&role) == VT_I4 ? V_I4(&role) : -1L, role_text,
         V_VT(&state) == VT_I4 ? V_I4(&state) : 0L, value ? value : L"");

    SysFreeString(name);
    SysFreeString(value);
    VariantClear(&role);
    VariantClear(&state);
    VariantClear(&child);
    IAccessible_Release(acc);
}

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG id_object, LONG id_child,
                                    DWORD thread, DWORD time)
{
    const WCHAR *name;

    switch (event)
    {
    case EVENT_SYSTEM_FOREGROUND:
        describe_window(L"foreground", hwnd);
        describe_msaa(hwnd, id_object, id_child);
        return;
    case EVENT_OBJECT_FOCUS:
        describe_window(L"focus", hwnd);
        logw(L"  object=%ld child=%ld", id_object, id_child);
        describe_msaa(hwnd, id_object, id_child);
        return;
    }

    if (!verbose)
        return;
    name = event_name(event);
    if (name)
        logw(L"event %s hwnd=%p object=%ld child=%ld tid=%lu",
             name, hwnd, id_object, id_child, thread);
    else
        logw(L"event 0x%04lx hwnd=%p object=%ld child=%ld tid=%lu",
             event, hwnd, id_object, id_child, thread);
}

static LRESULT CALLBACK ll_keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
    {
        const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)lparam;
        PostThreadMessageW(main_thread_id, WM_PROBE_KEY, key->vkCode, key->flags);
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static void check_edit(HWND hwnd)
{
    DWORD start = 0, end = 0;
    LRESULT line, line_start, line_len, count;
    WCHAR buf[1024];

    SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    line = SendMessageW(hwnd, EM_LINEFROMCHAR, start, 0);
    line_start = SendMessageW(hwnd, EM_LINEINDEX, line, 0);
    line_len = SendMessageW(hwnd, EM_LINELENGTH, line_start, 0);

    *(WORD *)buf = ARRAYSIZE(buf) - 1;
    count = SendMessageW(hwnd, EM_GETLINE, line, (LPARAM)buf);
    if (count < 0 || count >= ARRAYSIZE(buf))
        count = 0;
    buf[count] = 0;

    logw(L"  edit: sel=%lu-%lu line=%ld col=%ld len=%ld text=\"%s\"",
         start, end, (long)line, (long)(start - line_start), (long)line_len, buf);
}

/* Lists the children of an MSAA object, marking focused and hot ones. */
static void dump_msaa_children(const WCHAR *what, HWND hwnd, LONG id_object)
{
    IAccessible *acc = NULL;
    LONG count = 0, i;
    HRESULT hr;

    hr = AccessibleObjectFromWindow(hwnd, id_object, &IID_IAccessible, (void **)&acc);
    if (FAILED(hr) || !acc)
    {
        logw(L"  %s msaa: AccessibleObjectFromWindow failed hr=%08lx", what, hr);
        return;
    }
    IAccessible_get_accChildCount(acc, &count);
    logw(L"  %s msaa: children=%ld", what, count);
    for (i = 1; i <= count; i++)
    {
        VARIANT child, role, state;
        BSTR name = NULL;
        WCHAR role_text[64] = L"";

        VariantInit(&child);
        V_VT(&child) = VT_I4;
        V_I4(&child) = i;
        if (FAILED(IAccessible_get_accName(acc, child, &name)))
            name = NULL;
        VariantInit(&role);
        if (SUCCEEDED(IAccessible_get_accRole(acc, child, &role)) && V_VT(&role) == VT_I4)
            GetRoleTextW(V_I4(&role), role_text, ARRAYSIZE(role_text));
        VariantInit(&state);
        IAccessible_get_accState(acc, child, &state);
        logw(L"    child %ld: name=\"%s\" role=%s state=%08lx%s", i,
             name ? name : L"", role_text, V_VT(&state) == VT_I4 ? V_I4(&state) : 0L,
             V_VT(&state) == VT_I4 && (V_I4(&state) & (STATE_SYSTEM_FOCUSED | STATE_SYSTEM_HOTTRACKED))
                 ? L" <==" : L"");
        SysFreeString(name);
        VariantClear(&role);
        VariantClear(&state);
    }
    IAccessible_Release(acc);
}

static void check_menu_items(const WCHAR *what, HMENU menu)
{
    int i, count = GetMenuItemCount(menu);

    if (count < 0)
    {
        logw(L"  %s: hmenu=%p GetMenuItemCount failed error=%lu", what, menu,
             GetLastError());
        return;
    }
    logw(L"  %s: hmenu=%p items=%d", what, menu, count);
    for (i = 0; i < count; i++)
    {
        MENUITEMINFOW mii;
        WCHAR text[256] = L"";

        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii))
        {
            logw(L"    item %d: GetMenuItemInfo failed error=%lu", i, GetLastError());
            continue;
        }
        if (mii.fState & MFS_HILITE)
            logw(L"    item %d HILITE: \"%s\" state=%04x submenu=%p",
                 i, text, mii.fState, mii.hSubMenu);
    }
}

static void check_menus(const GUITHREADINFO *gti)
{
    HWND popup = NULL;

    if (gti->hwndMenuOwner)
    {
        describe_window(L"  menu owner", gti->hwndMenuOwner);
        if (GetMenu(gti->hwndMenuOwner))
            check_menu_items(L"menu bar", GetMenu(gti->hwndMenuOwner));
        dump_msaa_children(L"menu bar", gti->hwndMenuOwner, OBJID_MENU);
    }
    while ((popup = FindWindowExW(NULL, popup, L"#32768", NULL)))
    {
        HMENU menu;

        if (!IsWindowVisible(popup))
            continue;
        describe_window(L"  popup", popup);
        menu = (HMENU)SendMessageW(popup, MN_GETHMENU, 0, 0);
        if (menu)
            check_menu_items(L"popup menu", menu);
        else
            logw(L"  popup: MN_GETHMENU returned NULL");
        dump_msaa_children(L"popup", popup, OBJID_CLIENT);
    }
}

static void check_state(void)
{
    GUITHREADINFO gti;
    HWND fg = GetForegroundWindow();
    DWORD tid;
    WCHAR cls[256];

    if (!fg)
    {
        logw(L"  no foreground window");
        return;
    }
    tid = GetWindowThreadProcessId(fg, NULL);
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (!GetGUIThreadInfo(tid, &gti))
    {
        logw(L"  GetGUIThreadInfo(%lu) failed error=%lu", tid, GetLastError());
        return;
    }
    logw(L"  gui: tid=%lu flags=%08lx active=%p focus=%p capture=%p menuOwner=%p",
         tid, gti.flags, gti.hwndActive, gti.hwndFocus, gti.hwndCapture,
         gti.hwndMenuOwner);
    logw(L"  caret: hwnd=%p rect=(%ld,%ld)-(%ld,%ld)", gti.hwndCaret,
         gti.rcCaret.left, gti.rcCaret.top, gti.rcCaret.right, gti.rcCaret.bottom);

    if (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        check_menus(&gti);
    else if (verbose)
        check_menus(&gti);

    if (gti.hwndFocus)
    {
        get_class(gti.hwndFocus, cls, ARRAYSIZE(cls));
        if (!lstrcmpiW(cls, L"Edit"))
            check_edit(gti.hwndFocus);
    }
}

/* wmain is not used: its argv is broken when msvcrt and ucrtbase get mixed. */
int main(void)
{
    HWINEVENTHOOK event_hook;
    HHOOK key_hook;
    MSG msg;
    int argc = 0, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    start_tick = GetTickCount();
    for (i = 1; argv && i < argc; i++)
    {
        if (!lstrcmpW(argv[i], L"-v"))
            verbose = TRUE;
        else
        {
            logw(L"usage: probe [-v]");
            return 2;
        }
    }

    main_thread_id = GetCurrentThreadId();
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Make sure the thread has a message queue before hooks post to it. */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    event_hook = SetWinEventHook(EVENT_MIN, EVENT_MAX, NULL, win_event_proc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!event_hook)
        logw(L"SetWinEventHook failed error=%lu", GetLastError());
    key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_keyboard_proc,
                                 GetModuleHandleW(NULL), 0);
    if (!key_hook)
        logw(L"SetWindowsHookEx(WH_KEYBOARD_LL) failed error=%lu", GetLastError());
    logw(L"probe started, pid=%lu", GetCurrentProcessId());

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (msg.message == WM_PROBE_KEY)
        {
            logw(L"key vk=0x%02lx flags=%02lx", (DWORD)msg.wParam, (DWORD)msg.lParam);
            /* Let the target process the key before looking at its state. */
            if (key_timer)
                KillTimer(NULL, key_timer);
            key_timer = SetTimer(NULL, 0, KEY_DELAY_MS, NULL);
            continue;
        }
        if (msg.message == WM_TIMER && !msg.hwnd && msg.wParam == key_timer)
        {
            KillTimer(NULL, key_timer);
            key_timer = 0;
            check_state();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(key_hook);
    UnhookWinEvent(event_hook);
    CoUninitialize();
    return 0;
}
