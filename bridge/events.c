/*
 * wine-atspi bridge, Win32 core: WinEvents, focus, watched processes and
 * the check after each key.
 */
#include "core.h"

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
#define MAX_WATCHED 256

DWORD main_thread_id;
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

void watch_process(DWORD pid)
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

void on_process_exited(DWORD pid)
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

struct focus_state last_focus;

static UINT_PTR console_timer;

/* Reads the focused console again; runs while a console has focus. */
static void CALLBACK poll_console(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    struct bridge_object obj;
    struct focus_strings str;
    HWND hwnd = last_focus.hwnd;

    if (!hwnd || !is_console(hwnd) || GetForegroundWindow() != hwnd)
    {
        KillTimer(NULL, id);
        console_timer = 0;
        return;
    }
    memset(&obj, 0, sizeof(obj));
    str.text = NULL;
    if (!describe_console(hwnd, &obj, &str))
        return;
    /* The AT-SPI side announces only what differs from its copy. */
    atspi_post_update(last_focus.pid, &obj);
    free_strings(&str);
}

void on_focus(HWND hwnd, LONG id_object, LONG id_child)
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
    describe_labels(top, &focus, &str);
    if (focus.object.child && id_object == OBJID_CLIENT &&
        focus.object.role != BRIDGE_ROLE_TERMINAL)
        describe_container(hwnd, &focus.container, &str);
    GetWindowThreadProcessId(hwnd, &pid);
    /* A console window belongs to conhost.exe; the application is its program. */
    if (focus.object.role == BRIDGE_ROLE_TERMINAL && console_client.hwnd == hwnd)
        pid = console_client.pid;
    focus.pid = pid;
    get_app_name(pid, str.app, sizeof(str.app));
    focus.app_name = str.app;
    watch_process(pid);
    /*
     * Before the focus, so that the focused object gets its tree parent.
     * Without items, which would delay the speech (controls.c: 45 instead
     * of 190 ms); they follow right after it.
     */
    if (top != tree_top)
    {
        tree_top = top;
        report_tree(top, NULL, FALSE);
        tree_timer = SetTimer(NULL, tree_timer, 0, refresh_tree);
    }
    atspi_post_focus(&focus);
    trace(L"focus:%p %ld %ld name \"%S\"", hwnd, id_object, id_child, focus.object.name);
    free_strings(&str);
    report_status_bar(top);

    last_focus.hwnd = hwnd;
    last_focus.id_object = id_object;
    /* A list view's own focus event stands for its focused item. */
    last_focus.id_child = id_child ? id_child : (LONG)focus.object.child;
    last_focus.pid = pid;
    if (focus.object.role == BRIDGE_ROLE_TERMINAL && !console_timer)
        console_timer = SetTimer(NULL, 0, CONSOLE_POLL_MS, poll_console);
}

/*
 * No item control tells its focused item on a key: a ListBox moves its
 * caret without a focus event, the others emit none at all or only
 * events that are not hooked (see the focus member of each item_handler).
 * Reports the new item as focused and returns TRUE. FALSE if hwnd is no
 * item control or the focus did not move, LIST_BUSY if it did not answer.
 */
static int check_list_caret(HWND hwnd)
{
    const struct item_handler *handler;
    WCHAR cls[256];
    LONG child;

    if (last_focus.id_object != OBJID_CLIENT || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
        !(handler = find_item_handler(cls)))
        return FALSE;
    child = handler->focus(hwnd);
    if (child == LIST_BUSY)
        return LIST_BUSY;
    if (child <= 0 || child == last_focus.id_child)
        return FALSE;
    on_focus(hwnd, OBJID_CLIENT, child);
    return TRUE;
}

/*
 * A focus event can get lost: the Save changes? message box after Alt+F4
 * reported none (WSLg, 2026-11-06), so Orca stayed silent until the next
 * key. Like NVDA, compare the real focus of the foreground thread with the
 * last reported one and report it if they differ. The late event, if it
 * still comes, is then dropped in win_event_proc.
 */
UINT_PTR focus_timer;   /* a focus event waits, see FOCUS_SETTLE_MS */

BOOL check_missed_focus(void)
{
    HWND foreground = GetForegroundWindow();
    GUITHREADINFO gti;

    /* A focus event is about to be reported anyway; menus have their own. */
    if (focus_timer || menu.active)
        return FALSE;
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

/*
 * Focus events wait this long for a following one. Shift+Tab into the
 * Open dialog's file list gives list, its SHELLDLL_DefView parent, list
 * again within a few milliseconds (Wine 11.21), and Orca then said
 * nothing; only the focus that stays is reported.
 */
#define FOCUS_SETTLE_MS 30

static struct
{
    HWND hwnd;
    LONG id_object;
    LONG id_child;
} pending_focus;

static void CALLBACK report_pending_focus(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    HWND hwnd = pending_focus.hwnd;
    LONG id_object = pending_focus.id_object, id_child = pending_focus.id_child;

    KillTimer(NULL, id);
    focus_timer = 0;
    if (last_focus.recovered && hwnd == last_focus.hwnd &&
        id_object == last_focus.id_object && id_child == last_focus.id_child)
    {
        /* Already reported by check_missed_focus. */
        last_focus.recovered = FALSE;
        return;
    }
    last_focus.recovered = FALSE;
    if (is_stale_focus(hwnd, id_object, id_child))
    {
        trace(L"transient focus: %p", hwnd);
        check_missed_focus();
        return;
    }
    on_focus(hwnd, id_object, id_child);
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

/* A busy target is asked again, up to this many times. */
#define KEY_CHECK_RETRIES 5

UINT_PTR key_timer;
int key_retries;

void CALLBACK check_after_key(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    const struct item_handler *handler;
    struct bridge_object obj;
    struct focus_strings str;
    GUITHREADINFO gti;
    HWND hwnd = last_focus.hwnd;
    WCHAR cls[256];
    int list;

    KillTimer(NULL, id);
    key_timer = 0;
    /* Menu keys are reported by the hook; the focused control waits. */
    if (still_in_menu())
        return;
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
    list = check_list_caret(hwnd);
    if (list == LIST_BUSY && key_retries++ < KEY_CHECK_RETRIES)
    {
        trace(L"key check: %p busy, retry %d", hwnd, key_retries);
        key_timer = SetTimer(NULL, 0, KEY_CHECK_DELAY_MS, check_after_key);
        return;
    }
    /*
     * A key may scroll a list or expand a tree item, which tells no event:
     * its items in the window tree are read again.
     */
    if (list != LIST_BUSY && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
        (handler = find_item_handler(cls)) && handler->keys_change_items)
        schedule_items(hwnd);
    if (list)
    {
        /* A key often changes the status bar, e.g. regedit's key path. */
        if (list != LIST_BUSY)
            report_status_bar(GetAncestor(hwnd, GA_ROOT));
        return;
    }
    memset(&obj, 0, sizeof(obj));
    if (!describe_object(hwnd, last_focus.id_object, last_focus.id_child, &obj, &str))
    {
        report_status_bar(GetAncestor(hwnd, GA_ROOT));
        return;
    }
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
    report_status_bar(GetAncestor(hwnd, GA_ROOT));
}

/*
 * A progress bar never has focus, yet Orca presents its value changes
 * (default.py, _on_value_changed). Wine's comctl32 notifies
 * EVENT_OBJECT_VALUECHANGE for it; other controls' value changes come
 * with the key check of the focused object.
 */
static void on_value_change(HWND hwnd, LONG id_object, LONG id_child)
{
    struct bridge_focus change;
    struct focus_strings str;
    WCHAR cls[64];
    HWND top;
    DWORD pid = 0;

    if (id_object != OBJID_CLIENT || id_child != CHILDID_SELF ||
        !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"msctls_progress32") ||
        !IsWindowVisible(hwnd))
        return;
    if (!(top = GetAncestor(hwnd, GA_ROOT)))
        return;
    memset(&change, 0, sizeof(change));
    if (!describe_object(hwnd, id_object, id_child, &change.object, &str))
        return;
    describe_frame(top, &change.frame, &str);
    GetWindowThreadProcessId(hwnd, &pid);
    change.pid = pid;
    get_app_name(pid, str.app, sizeof(str.app));
    change.app_name = str.app;
    watch_process(pid);
    atspi_post_value(&change);
    trace(L"value: %p name \"%S\" %d", hwnd, change.object.name, (int)change.object.current);
    free_strings(&str);
}

void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG id_object, LONG id_child,
                                    DWORD thread, DWORD time)
{
    if (!hwnd)
        return;
    switch (event)
    {
    case EVENT_OBJECT_VALUECHANGE:
        on_value_change(hwnd, id_object, id_child);
        break;
    case EVENT_OBJECT_FOCUS:
        pending_focus.hwnd = hwnd;
        pending_focus.id_object = id_object;
        pending_focus.id_child = id_child;
        focus_timer = SetTimer(NULL, focus_timer, FOCUS_SETTLE_MS, report_pending_focus);
        break;
    case EVENT_SYSTEM_FOREGROUND:
        foreground_timer = SetTimer(NULL, foreground_timer, FOREGROUND_CHECK_DELAY_MS,
                                    check_after_foreground);
        break;
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_DESTROY:
        /* The window may be gone already: no class, no process to ask. */
        if (id_object == OBJID_WINDOW && id_child == CHILDID_SELF)
        {
            atspi_post_window_gone((ULONG_PTR)hwnd);
            /* Shown again, it is reported again. */
            if (hwnd == last_status.hwnd)
                last_status.hwnd = NULL;
            if (hwnd == tree_top)
                tree_top = NULL;
            else if (event == EVENT_OBJECT_HIDE)
                schedule_tree(hwnd);
        }
        break;
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_NAMECHANGE:
        if (id_object == OBJID_WINDOW && id_child == CHILDID_SELF)
            schedule_tree(hwnd);
        break;
    }
}
