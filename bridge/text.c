/*
 * wine-atspi bridge, Win32 core: text of Edit and RichEdit controls,
 * caret and text extents.
 */
#include "core.h"

/* Text beyond this many UTF-16 units is cut off. */
#define TEXT_MAX_CHARS (1 << 20)

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

/* Text, caret and selection of an Edit or RichEdit; str->text owns the text. */
void describe_edit_text(HWND hwnd, BOOL rich, struct bridge_object *obj,
                               struct focus_strings *str)
{
    DWORD_PTR len = 0;
    DWORD start = 0, end = 0, start_index, end_index;
    WCHAR *buf, *src, *dst;
    int size, i, n;

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
