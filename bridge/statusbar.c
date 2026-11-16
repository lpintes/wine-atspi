/*
 * wine-atspi bridge, Win32 core: status bars (msctls_statusbar32).
 */
#include "core.h"

/*
 * The status bar of a top-level window. Wine's comctl32 emits no WinEvent
 * when its text changes (status.c, SB_SETTEXTW), so it is read on each
 * focus change and after each key, and reported below the frame when it
 * changed. Orca reads it on its "present status bar" command: role, then
 * the whole text (speech_generator.py, _generate_status_bar).
 */
struct status_state last_status;

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
    if (!SendMessageTimeoutW(hwnd, SB_GETPARTS, 0, 0, SMTO_ABORTIFHUNG, STATUS_TIMEOUT_MS,
                             &parts))
        return FALSE;
    if (SendMessageTimeoutW(hwnd, SB_ISSIMPLE, 0, 0, SMTO_ABORTIFHUNG, STATUS_TIMEOUT_MS,
                            &simple) && simple)
        parts = 1;
    if (!remote_open(hwnd, &r))
        return FALSE;
    for (i = 0; ok && i < (int)parts && i < 256; i++)
    {
        if (!SendMessageTimeoutW(hwnd, SB_GETTEXTLENGTHW, i, 0, SMTO_ABORTIFHUNG,
                                 STATUS_TIMEOUT_MS, &len))
        {
            ok = FALSE;
            break;
        }
        /* The text must fit the block, SB_GETTEXTW takes no size. */
        if (!LOWORD(len) || (HIWORD(len) & SBT_OWNERDRAW) || LOWORD(len) >= CELL_TEXT_MAX)
            continue;
        ok = WriteProcessMemory(r.process, r.base, &empty, sizeof(empty), &done) &&
             SendMessageTimeoutW(hwnd, SB_GETTEXTW, i, (LPARAM)r.base, SMTO_ABORTIFHUNG,
                                 STATUS_TIMEOUT_MS, &len) &&
             ReadProcessMemory(r.process, r.base, buf, sizeof(buf), &done);
        if (!ok)
            break;
        buf[CELL_TEXT_MAX - 1] = 0;
        append_status_part(text, size, buf);
    }
    remote_close(&r);
    return ok;
}

/* Reports the status bar of top-level window top if its text changed. */
void report_status_bar(HWND top)
{
    struct bridge_focus change;
    struct focus_strings str;
    struct bridge_object *obj = &change.object;
    WCHAR text[STATUS_TEXT_MAX];
    HWND hwnd = NULL;
    DWORD pid = 0;

    if (!top)
        return;
    EnumChildWindows(top, find_status_bar, (LPARAM)&hwnd);
    if (!hwnd || !read_status_bar(hwnd, text, ARRAYSIZE(text)))
        return;
    if (hwnd == last_status.hwnd && !lstrcmpW(text, last_status.text))
        return;
    last_status.hwnd = hwnd;
    lstrcpyW(last_status.text, text);

    memset(&change, 0, sizeof(change));
    str.text = HeapAlloc(GetProcessHeap(), 0, STATUS_TEXT_MAX * 3);
    if (!str.text)
        return;
    to_utf8(text, str.text, STATUS_TEXT_MAX * 3);
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
}
