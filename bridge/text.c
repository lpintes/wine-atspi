/*
 * wine-atspi bridge, Win32 core: text of Edit and RichEdit controls,
 * caret and text extents.
 */
#include "core.h"

/*
 * Text beyond this many UTF-16 units is cut off. A cross-process
 * WM_GETTEXT of 1,048,570 units or more never returns in Wine 11.21
 * (1,040,000 work), so the cap stays below that.
 */
#define TEXT_MAX_CHARS 1000000

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
        !send_message(hwnd, EM_POSFROMCHAR, start, 0, SEND_TIMEOUT_MS, &pos) ||
        pos == (DWORD_PTR)-1)
        return FALSE;
    return (short)LOWORD(pos) == gti.rcCaret.left && (short)HIWORD(pos) == gti.rcCaret.top;
}

/* Texts of windows as the hook DLL handed them over last (hook_text). */
#define MAX_TEXT_COPIES 4

static struct text_copy
{
    HWND hwnd;
    DWORD pid;
    UINT64 generation;
    WCHAR *text;
    UINT32 length;
    DWORD used;
} copies[MAX_TEXT_COPIES];

static void free_copy(struct text_copy *copy)
{
    HeapFree(GetProcessHeap(), 0, copy->text);
    memset(copy, 0, sizeof(*copy));
}

/* The copy of hwnd in pid, else a free or the oldest slot, cleared. */
static struct text_copy *find_copy(HWND hwnd, DWORD pid)
{
    struct text_copy *slot = NULL;
    int i;

    for (i = 0; i < MAX_TEXT_COPIES; i++)
        if (copies[i].hwnd == hwnd && copies[i].pid == pid)
            return &copies[i];
    for (i = 0; i < MAX_TEXT_COPIES; i++)
        if (!slot || !copies[i].hwnd || (slot->hwnd && copies[i].used < slot->used))
            slot = &copies[i];
    free_copy(slot);
    return slot;
}

/* Bytes per ReadProcessMemory in read_remote. */
#define READ_CHUNK (1 << 20)

/* Reads size bytes from the target in pieces of READ_CHUNK. */
static BOOL read_remote(HANDLE process, const char *addr, char *buf, SIZE_T size)
{
    SIZE_T done, part;

    while (size)
    {
        part = min(size, READ_CHUNK);
        if (!ReadProcessMemory(process, addr, buf, part, &done) || done != part)
        {
            trace(L"text: ReadProcessMemory of %lu bytes failed, error %lu", (ULONG)part,
                  GetLastError());
            return FALSE;
        }
        addr += part;
        buf += part;
        size -= part;
    }
    return TRUE;
}

/*
 * Text of hwnd read by the hook DLL inside the target (TEXT_MESSAGE in
 * wahook.h): only the change against the bridge's copy crosses the
 * process. Returns a heap copy of the text of *len units and the EM_GETSEL
 * range, or NULL without the hook (32-bit target) or on an error.
 */
static WCHAR *hook_text(HWND hwnd, DWORD *len, DWORD *sel_start, DWORD *sel_end)
{
    static UINT message;
    struct text_request req;
    struct text_copy *copy;
    struct remote r;
    WCHAR *text = NULL, *result = NULL;
    DWORD pid = 0;
    DWORD_PTR ret;
    SIZE_T done;

    if (!message)
        message = RegisterWindowMessageW(TEXT_MESSAGE);
    GetWindowThreadProcessId(hwnd, &pid);
    if (!message || !remote_open(hwnd, &r))
        return NULL;
    copy = find_copy(hwnd, pid);
    memset(&req, 0, sizeof(req));
    req.tag = TEXT_REQUEST_TAG;
    req.generation = copy->hwnd ? copy->generation : 0;
    if (!WriteProcessMemory(r.process, r.base, &req, sizeof(req), &done) ||
        !send_message(hwnd, message, 0, (LPARAM)r.base, SEND_TIMEOUT_MS, &ret) ||
        !ReadProcessMemory(r.process, r.base, &req, sizeof(req), &done) ||
        req.tag != TEXT_REPLY_TAG)
    {
        trace(L"text: no answer from the hook");
        goto done;
    }
    if (!(req.flags & TEXT_REPLY_FULL) &&
        (!copy->hwnd || req.start > copy->length || req.removed > copy->length - req.start ||
         req.length != copy->length - req.removed + req.inserted))
    {
        /* Not the change of our copy; ask for the whole text next time. */
        free_copy(copy);
        goto done;
    }
    if (req.flags & TEXT_REPLY_FULL)
        req.start = req.removed = 0;
    if (!(req.flags & TEXT_REPLY_FULL) && !req.removed && !req.inserted)
        text = copy->text;
    else
    {
        if (!(text = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)req.length + 1) * sizeof(WCHAR))))
            goto done;
        if (req.start)
            memcpy(text, copy->text, req.start * sizeof(WCHAR));
        if (!read_remote(r.process, (const char *)(ULONG_PTR)req.data, (char *)(text + req.start),
                         (SIZE_T)req.inserted * sizeof(WCHAR)))
        {
            HeapFree(GetProcessHeap(), 0, text);
            free_copy(copy);
            goto done;
        }
        if (!(req.flags & TEXT_REPLY_FULL))
            memcpy(text + req.start + req.inserted, copy->text + req.start + req.removed,
                   (req.length - req.start - req.inserted) * sizeof(WCHAR));
        text[req.length] = 0;
        HeapFree(GetProcessHeap(), 0, copy->text);
        copy->text = text;
    }
    copy->hwnd = hwnd;
    copy->pid = pid;
    copy->generation = req.generation;
    copy->length = req.length;
    copy->used = GetTickCount();
    if ((result = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)req.length + 1) * sizeof(WCHAR))))
    {
        memcpy(result, text, ((SIZE_T)req.length + 1) * sizeof(WCHAR));
        *len = req.length;
        *sel_start = req.sel_start;
        *sel_end = req.sel_end;
    }
    trace(L"text: hook %lu units, %lu removed and %lu inserted at %lu%s", req.length,
          req.removed, req.inserted, req.start, (req.flags & TEXT_REPLY_FULL) ? L" (full)" : L"");
done:
    remote_close(&r);
    return result;
}

/* Text, caret and selection of an Edit or RichEdit; str->text owns the text. */
void describe_edit_text(HWND hwnd, BOOL rich, struct bridge_object *obj,
                               struct focus_strings *str)
{
    DWORD_PTR len = 0;
    DWORD start = 0, end = 0, start_index, end_index, hook_len;
    WCHAR *buf, *src, *dst;
    int size, i, n;

    if ((buf = hook_text(hwnd, &hook_len, &start, &end)))
        n = hook_len;
    else
    {
        if (!send_message(hwnd, WM_GETTEXTLENGTH, 0, 0, SEND_TIMEOUT_MS, &len))
            return;
        if (len > TEXT_MAX_CHARS)
            len = TEXT_MAX_CHARS;
        buf = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
        if (!buf)
            return;
        if (!send_message(hwnd, WM_GETTEXT, len + 1, (LPARAM)buf, SEND_TIMEOUT_MS, &len))
            len = 0;
        buf[len] = 0;
        n = len;
        /* Pointer arguments of EM_GETSEL are marshalled like on Windows. */
        send_message(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end, SEND_TIMEOUT_MS, NULL);
    }
    start_index = rich ? rich_text_index(buf, n, start) : start;
    end_index = rich ? rich_text_index(buf, n, end) : end;
    obj->selection_start = char_offset(buf, n, start_index);
    obj->selection_end = char_offset(buf, n, end_index);
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

BOOL has_edit_text(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, L"RICHEDIT50W") ||
           !lstrcmpiW(cls, L"RichEdit20A") || !lstrcmpiW(cls, L"RichEdit20W");
}

/* Lines below the first visible one whose packed EM_POSFROMCHAR y is sure not to wrap. */
#define TEXT_SAFE_LINES 1000

/* Client position of the character at index; FALSE past the end of the text. */
static BOOL text_pos(HWND hwnd, int index, POINT *pt)
{
    DWORD_PTR pos;

    if (!send_message(hwnd, EM_POSFROMCHAR, index, 0, SEND_TIMEOUT_MS, &pos) ||
        pos == (DWORD_PTR)-1)
        return FALSE;
    pt->x = (short)LOWORD(pos);
    pt->y = (short)HIWORD(pos);
    return TRUE;
}

static int text_send(HWND hwnd, UINT msg, WPARAM wparam)
{
    DWORD_PTR result = 0;

    send_message(hwnd, msg, wparam, 0, SEND_TIMEOUT_MS, &result);
    return (int)result;
}

/* Height of one line; the edit control does not tell, so measure two lines. */
static int text_line_height(HWND hwnd, int first_visible, int lines, const RECT *client)
{
    GUITHREADINFO gti;
    POINT a, b;

    if (first_visible + 1 < lines &&
        text_pos(hwnd, text_send(hwnd, EM_LINEINDEX, first_visible), &a) &&
        text_pos(hwnd, text_send(hwnd, EM_LINEINDEX, first_visible + 1), &b) && b.y > a.y)
        return b.y - a.y;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti) && gti.hwndCaret == hwnd &&
        gti.rcCaret.bottom > gti.rcCaret.top)
        return gti.rcCaret.bottom - gti.rcCaret.top;
    return client->bottom - client->top;
}

/*
 * Client x just after the last character of the line holding index; len
 * is the length of the whole text. A line break has the position of that
 * end; the last line and a wrapped one have none (the next character
 * starts the next line), so their last character gets the average width
 * of the line.
 */
static int text_line_end(HWND hwnd, int index, int len, int height)
{
    int line = text_send(hwnd, EM_LINEFROMCHAR, index);
    int start = text_send(hwnd, EM_LINEINDEX, line);
    int end = start + text_send(hwnd, EM_LINELENGTH, index);
    POINT first, last;

    if (end < len && text_send(hwnd, EM_LINEFROMCHAR, end) == line && text_pos(hwnd, end, &last))
        return last.x;
    if (end <= start || !text_pos(hwnd, end - 1, &last))
        return 0;
    if (end - 1 > start && text_pos(hwnd, start, &first) && last.x > first.x)
        return last.x + (last.x - first.x) / (end - 1 - start);
    return last.x + height / 2;
}

int bridge_text_line(unsigned long long handle, const unsigned int *chars, int count,
                     int offset, int *start, int *end)
{
    HWND hwnd = (HWND)(ULONG_PTR)handle;
    DWORD_PTR line, first, next, lines;
    WCHAR cls[64];
    LONG style;
    BOOL rich;

    if (!chars || !hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || !has_edit_text(cls))
        return 0;
    rich = lstrcmpiW(cls, L"Edit") != 0;
    style = GetWindowLongW(hwnd, GWL_STYLE);
    /*
     * Wine's Edit wraps a multi-line text unless ES_AUTOHSCROLL
     * (user32/edit.c); whether a RichEdit wraps is up to its host
     * (TXTBIT_WORDWRAP, EM_SETTARGETDEVICE), so ask any multi-line one.
     */
    if (!(style & ES_MULTILINE) || (!rich && (style & ES_AUTOHSCROLL)))
        return 0;
    offset = max(0, min(offset, count));
    if (!send_message(hwnd, EM_LINEFROMCHAR, control_index(chars, count, offset, rich), 0,
                      SEND_TIMEOUT_MS, &line) ||
        !send_message(hwnd, EM_GETLINECOUNT, 0, 0, SEND_TIMEOUT_MS, &lines) ||
        !send_message(hwnd, EM_LINEINDEX, line, 0, SEND_TIMEOUT_MS, &first) ||
        (int)first < 0)
        return 0;
    if (line + 1 >= lines ||
        !send_message(hwnd, EM_LINEINDEX, line + 1, 0, SEND_TIMEOUT_MS, &next) || (int)next < 0)
        next = (DWORD_PTR)control_index(chars, count, count, rich);
    *start = control_offset(chars, count, (int)first, rich);
    *end = control_offset(chars, count, (int)next, rich);
    /* The text may have changed since chars was read; keep offset in its line. */
    if (*start > offset || *end < offset || (*end == offset && offset < count))
        return 0;
    return 1;
}

int bridge_text_select(unsigned long long handle, const unsigned int *chars, int count,
                       int start, int end)
{
    HWND hwnd = (HWND)(ULONG_PTR)handle;
    WCHAR cls[64];
    BOOL rich;

    if (!chars || !hwnd || start < 0 || end < 0 || start > count || end > count ||
        !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || !has_edit_text(cls))
        return 0;
    rich = lstrcmpiW(cls, L"Edit") != 0;
    /*
     * Wine's Edit keeps the caret at lParam, also before wParam, and
     * scrolls to it (EDIT_EM_SetSel, EDIT_EM_ScrollCaret in user32/edit.c);
     * a RichEdit orders the two and puts the caret at the end
     * (set_selection_cursors in riched20/caret.c). Posted, like a key: the
     * AT-SPI thread must not wait for a busy target. No WinEvent tells of
     * the caret, so the main thread looks after the target handled it.
     */
    if (!PostMessageW(hwnd, EM_SETSEL, control_index(chars, count, start, rich),
                      control_index(chars, count, end, rich)))
        return 0;
    PostThreadMessageW(main_thread_id, WM_TEXT_SELECTED, (WPARAM)hwnd, 0);
    return 1;
}

int bridge_text_extents(unsigned long long handle, const unsigned int *chars, int count,
                        int start, int end, int *rect)
{
    HWND hwnd = (HWND)(ULONG_PTR)handle;
    int first_index, end_index, len, lines, first_line, last_line, first_visible, height, i;
    WCHAR cls[64];
    BOOL rich, multi;
    RECT client, r;
    POINT pt;

    if (!chars || !hwnd || !IsWindow(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)))
        return 0;
    if (!lstrcmpW(cls, L"WineConsoleClass"))
        return console_text_extents(hwnd, chars, count, start, end, rect);
    if (!has_edit_text(cls))
        return 0;
    rich = lstrcmpiW(cls, L"Edit") != 0;
    start = max(start, 0);
    end = min(end, count);
    if (start >= end)
        return 0;
    first_index = control_index(chars, count, start, rich);
    end_index = control_index(chars, count, end, rich);
    len = control_index(chars, count, count, rich);
    if (!GetClientRect(hwnd, &client) || !text_pos(hwnd, first_index, &pt))
        return 0;

    multi = rich || (GetWindowLongW(hwnd, GWL_STYLE) & ES_MULTILINE);
    lines = multi ? text_send(hwnd, EM_GETLINECOUNT, 0) : 1;
    /* For a single-line Edit EM_GETFIRSTVISIBLELINE gives a character. */
    first_visible = multi ? text_send(hwnd, EM_GETFIRSTVISIBLELINE, 0) : 0;
    first_line = multi ? text_send(hwnd, EM_LINEFROMCHAR, first_index) : 0;
    last_line = multi ? text_send(hwnd, EM_LINEFROMCHAR, end_index - 1) : 0;
    height = text_line_height(hwnd, first_visible, lines, &client);

    /*
     * EM_POSFROMCHAR packs y into 16 bits, which wraps far from the first
     * visible line, so lines are placed by their number where it could.
     */
    if (last_line < first_visible)
        r.top = r.bottom = -TEXT_FAR_AWAY;
    else if (first_line - first_visible > TEXT_SAFE_LINES)
        r.top = r.bottom = TEXT_FAR_AWAY;
    else
    {
        r.top = first_line < first_visible ? client.top : pt.y;
        if (last_line - first_visible > TEXT_SAFE_LINES)
            r.bottom = client.bottom;
        else if (last_line == first_line)
            r.bottom = pt.y + height;
        else if (text_pos(hwnd, text_send(hwnd, EM_LINEINDEX, last_line), &pt))
            r.bottom = pt.y + height;
        else
            r.bottom = client.bottom;
        r.top = max(r.top, client.top);
        r.bottom = min(r.bottom, client.bottom);
        if (r.top >= r.bottom)
            r.top = r.bottom = r.top >= client.bottom ? TEXT_FAR_AWAY : -TEXT_FAR_AWAY;
    }
    if (r.top == r.bottom)
    {
        /* Keep the line height, so that the middle of the line is out as well. */
        r.left = client.left;
        r.right = client.left;
        r.bottom = r.top + height;
        client_to_screen(hwnd, r, rect);
        return 1;
    }

    if (first_line != last_line)
    {
        r.left = client.left;
        r.right = client.right;
    }
    else
    {
        text_pos(hwnd, first_index, &pt);
        r.left = pt.x;
        if (end_index < len && (!multi || text_send(hwnd, EM_LINEFROMCHAR, end_index) == first_line) &&
            text_pos(hwnd, end_index, &pt))
            r.right = pt.x;
        else
            r.right = text_line_end(hwnd, first_index, len, height);
        if (r.right < r.left)
        {
            i = r.left;
            r.left = r.right;
            r.right = i;
        }
        r.left = max(r.left, client.left);
        r.right = min(r.right, client.right);
        if (r.right < r.left)
            r.right = r.left;
    }
    client_to_screen(hwnd, r, rect);
    return 1;
}
