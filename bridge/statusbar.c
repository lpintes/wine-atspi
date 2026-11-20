/*
 * wine-atspi bridge, Win32 core: status bars (msctls_statusbar32).
 */
#include "core.h"

/*
 * The status bar of a top-level window. Wine's comctl32 emits no WinEvent
 * when its text changes (status.c, SB_SETTEXTW). The hook DLL sees the
 * change and posts STATUS_MESSAGE (wahook.h), so the status bar is read
 * on each focus change and when the hook tells; a target without the
 * hook (32-bit, or no hook DLL) is read after each key instead. It is
 * reported below the frame when it changed. Orca reads it on its
 * "present status bar" command: role, then the whole text
 * (speech_generator.py, _generate_status_bar).
 */
static struct status_state last_status;

/* Top-level window whose status bar the hook said changed. */
static struct
{
    HWND top;
    UINT_PTR timer;
    int retries;
} notice;

/* Whether process pid is 32-bit, for the last pid asked. */
static struct
{
    DWORD pid;
    BOOL wow64;
} bitness;

static BOOL CALLBACK find_status_bar(HWND hwnd, LPARAM param)
{
    WCHAR cls[64];

    if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
        lstrcmpiW(cls, L"msctls_statusbar32"))
        return TRUE;
    *(HWND *)param = hwnd;
    return FALSE;
}

/* Appends part to text without its outer blanks, after a space. */
static void append_status_part(WCHAR *text, int size, const WCHAR *part)
{
    int len = lstrlenW(text), end, i;

    while (*part == ' ' || *part == '\t')
        part++;
    end = lstrlenW(part);
    while (end && (part[end - 1] == ' ' || part[end - 1] == '\t'))
        end--;
    if (!end)
        return;
    if (len && len < size - 1)
        text[len++] = ' ';
    /* A tab aligns the rest of a part to the center or right. */
    for (i = 0; i < end && len < size - 1; i++)
        text[len++] = part[i] == '\t' ? ' ' : part[i];
    text[len] = 0;
}

/*
 * The texts of all parts of status bar hwnd, joined by spaces as NVDA
 * does. SB_GETTEXTW is not marshalled, so the text goes through memory
 * in the target; owner-drawn parts have no text.
 */
static BOOL read_status_bar(HWND hwnd, WCHAR *text, int size)
{
    static const WCHAR empty = 0;
    WCHAR buf[CELL_TEXT_MAX];
    struct remote r;
    DWORD_PTR parts, simple, len;
    SIZE_T done;
    BOOL ok = TRUE;
    int i;

    text[0] = 0;
    if (!send_message(hwnd, SB_GETPARTS, 0, 0, STATUS_TIMEOUT_MS, &parts))
        return FALSE;
    if (send_message(hwnd, SB_ISSIMPLE, 0, 0, STATUS_TIMEOUT_MS, &simple) && simple)
        parts = 1;
    if (!remote_open(hwnd, &r))
        return FALSE;
    for (i = 0; ok && i < (int)parts && i < 256; i++)
    {
        if (!send_message(hwnd, SB_GETTEXTLENGTHW, i, 0, STATUS_TIMEOUT_MS, &len))
        {
            ok = FALSE;
            break;
        }
        /* The text must fit the block, SB_GETTEXTW takes no size. */
        if (!LOWORD(len) || (HIWORD(len) & SBT_OWNERDRAW) || LOWORD(len) >= CELL_TEXT_MAX)
            continue;
        ok = WriteProcessMemory(r.process, r.base, &empty, sizeof(empty), &done) &&
             send_message(hwnd, SB_GETTEXTW, i, (LPARAM)r.base, STATUS_TIMEOUT_MS, &len) &&
             ReadProcessMemory(r.process, r.base, buf, sizeof(buf), &done);
        if (!ok)
            break;
        buf[CELL_TEXT_MAX - 1] = 0;
        append_status_part(text, size, buf);
    }
    remote_close(&r);
    return ok;
}

/*
 * Reports the status bar of top-level window top if its text changed.
 * FALSE if it could not be read.
 */
BOOL report_status_bar(HWND top)
{
    struct bridge_focus change;
    struct focus_strings str;
    struct bridge_object *obj = &change.object;
    WCHAR text[STATUS_TEXT_MAX];
    HWND hwnd = NULL;
    DWORD pid = 0;

    if (!top)
        return TRUE;
    EnumChildWindows(top, find_status_bar, (LPARAM)&hwnd);
    if (!hwnd)
        return TRUE;
    if (!read_status_bar(hwnd, text, ARRAYSIZE(text)))
        return FALSE;
    if (hwnd == last_status.hwnd && !lstrcmpW(text, last_status.text))
        return TRUE;
    last_status.hwnd = hwnd;
    lstrcpyW(last_status.text, text);

    memset(&change, 0, sizeof(change));
    str.text = new_text(text, lstrlenW(text));
    if (!str.text)
        return TRUE;
    describe_frame(top, &change.frame, &str);
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->role = BRIDGE_ROLE_STATUS_BAR;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    obj->window_class = "msctls_statusbar32";
    obj->text = str.text;
    get_extents(hwnd, obj);
    GetWindowThreadProcessId(hwnd, &pid);
    change.pid = pid;
    get_app_name(pid, str.app, sizeof(str.app));
    change.app_name = str.app;
    atspi_post_value(&change);
    trace(L"status bar: %p \"%s\"", hwnd, text);
    free_strings(&str);
    return TRUE;
}

static BOOL is_wow64_process(DWORD pid)
{
    HANDLE process;

    if (pid == bitness.pid)
        return bitness.wow64;
    bitness.pid = pid;
    bitness.wow64 = FALSE;
    if ((process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
    {
        IsWow64Process(process, &bitness.wow64);
        CloseHandle(process);
    }
    return bitness.wow64;
}

/*
 * After a key: reads the status bar of top unless the hook DLL tells of
 * its changes. The hook is a 64-bit DLL, so a 32-bit target is read.
 */
void check_status_bar(HWND top)
{
    DWORD pid = 0;

    if (!top)
        return;
    GetWindowThreadProcessId(top, &pid);
    if (hook_dll_installed() && !is_wow64_process(pid))
        return;
    report_status_bar(top);
}

static void CALLBACK status_changed(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, notice.timer);
    notice.timer = 0;
    if (report_status_bar(notice.top) || !is_busy(notice.top) ||
        notice.retries++ >= STATUS_RETRIES)
        return;
    trace(L"status bar: %p busy, retry %d", notice.top, notice.retries);
    notice.timer = SetTimer(NULL, 0, STATUS_SETTLE_MS, status_changed);
}

/*
 * The hook says status bar hwnd is about to change. Only the window of
 * the focus matters: another one is read when it gets the focus.
 */
void on_status_notice(HWND hwnd)
{
    HWND top = GetAncestor(hwnd, GA_ROOT);

    if (!top || !last_focus.hwnd || top != GetAncestor(last_focus.hwnd, GA_ROOT))
        return;
    if (notice.timer && notice.top == top)
        return;
    notice.top = top;
    notice.retries = 0;
    notice.timer = SetTimer(NULL, notice.timer, STATUS_SETTLE_MS, status_changed);
}

/*
 * Window hwnd was shown, or hidden or destroyed. A status bar shown
 * again is read again, even if its text did not change.
 */
void status_window_shown(HWND hwnd, BOOL shown)
{
    WCHAR cls[64];

    if (!shown)
    {
        if (hwnd == last_status.hwnd)
            last_status.hwnd = NULL;
        return;
    }
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && !lstrcmpiW(cls, L"msctls_statusbar32"))
        on_status_notice(hwnd);
}

/* The hooks are gone: forget what was reported and what waits. */
void forget_status(void)
{
    if (notice.timer)
        KillTimer(NULL, notice.timer);
    memset(&notice, 0, sizeof(notice));
    last_status.hwnd = NULL;
}
