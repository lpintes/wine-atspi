/*
 * wine-atspi bridge, Win32 core: Wine console windows (conhost.exe).
 */
#include "core.h"

/*
 * Wine's console window (conhost.exe, WineConsoleClass) emits no WinEvent
 * and draws its text with GDI. Like NVDA, the bridge attaches to the
 * console for a moment, reads the screen buffer and detaches again. The
 * window is the frame; the terminal inside it is child CONSOLE_CHILD.
 */
#define CONSOLE_CHILD 1
/* Screen buffer cells read at most; earlier rows are left out. */
#define CONSOLE_MAX_CELLS (1 << 18)

struct console_client_state console_client;

/*
 * Where the text of the last read console lies, for text extents asked on
 * the AT-SPI thread: line 0 of the text is buffer row first_row, the
 * window shows the buffer cells window, and cell (x, y) is drawn at client
 * ((x - window.Left) * cell.X, (y - window.Top) * cell.Y) (conhost's
 * window.c).
 */
static struct
{
    HWND hwnd;
    int first_row;
    SMALL_RECT window;
    COORD cell;
} console_view;
static SRWLOCK console_view_lock = SRWLOCK_INIT;

BOOL is_console(HWND hwnd)
{
    WCHAR cls[64];

    return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && !lstrcmpW(cls, CONSOLE_CLASS);
}

/* Ctrl+C and Ctrl+Break reach every process attached to the console. */
static BOOL WINAPI ignore_console_ctrl(DWORD type)
{
    return TRUE;
}

/*
 * Attaches to the console of window hwnd through process pid. Wine's
 * AttachConsole replaces the std handles and FreeConsole closes those, so
 * they are saved here and restored by detach_console.
 */
static HANDLE saved_std[3];

static BOOL attach_console(HWND hwnd, DWORD pid)
{
    static BOOL prepared;
    static const DWORD std[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    int i;

    if (!prepared)
    {
        SetConsoleCtrlHandler(ignore_console_ctrl, TRUE);
        /* A bridge started from a terminal has a console of its own. */
        FreeConsole();
        prepared = TRUE;
    }
    for (i = 0; i < 3; i++)
        saved_std[i] = GetStdHandle(std[i]);
    if (AttachConsole(pid))
    {
        if (GetConsoleWindow() == hwnd)
            return TRUE;
        FreeConsole();
    }
    for (i = 0; i < 3; i++)
        SetStdHandle(std[i], saved_std[i]);
    return FALSE;
}

static void detach_console(void)
{
    FreeConsole();
    SetStdHandle(STD_INPUT_HANDLE, saved_std[0]);
    SetStdHandle(STD_OUTPUT_HANDLE, saved_std[1]);
    SetStdHandle(STD_ERROR_HANDLE, saved_std[2]);
}

/* Attaches to the console of window hwnd; returns the client pid or 0. */
static DWORD attach_console_window(HWND hwnd)
{
    PROCESSENTRY32W pe;
    HANDLE snap;
    DWORD owner = 0;
    BOOL more;

    if (console_client.hwnd == hwnd && attach_console(hwnd, console_client.pid))
        return console_client.pid;
    console_client.hwnd = NULL;
    GetWindowThreadProcessId(hwnd, &owner);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof(pe);
    for (more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
    {
        if (pe.th32ProcessID == owner || pe.th32ProcessID == GetCurrentProcessId() ||
            !attach_console(hwnd, pe.th32ProcessID))
            continue;
        console_client.hwnd = hwnd;
        console_client.pid = pe.th32ProcessID;
        trace(L"console %p: client %lu %s", hwnd, pe.th32ProcessID, pe.szExeFile);
        break;
    }
    CloseHandle(snap);
    return console_client.hwnd == hwnd ? console_client.pid : 0;
}

/*
 * The screen buffer as text: rows without trailing spaces, joined by
 * "\n", up to the cursor row or the last non-empty row, whichever is
 * later. The cursor row keeps its spaces up to the cursor.
 */
BOOL describe_console(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    CONSOLE_FONT_INFO font = { 0 };
    COORD origin = { 0, 0 };
    WCHAR *cells = NULL, *buf = NULL;
    HANDLE out;
    DWORD read = 0;
    int width = 0, rows = 0, cursor_row, y, len, pos = 0, end = 0, caret = 0;
    BOOL ok = FALSE;

    if (!attach_console_window(hwnd))
        return FALSE;
    out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info) && info.dwSize.X > 0)
    {
        width = info.dwSize.X;
        rows = max(info.dwCursorPosition.Y, info.srWindow.Bottom) + 1;
        rows = min(rows, info.dwSize.Y);
        if (width * rows > CONSOLE_MAX_CELLS)
            origin.Y = rows - CONSOLE_MAX_CELLS / width;
        rows -= origin.Y;
        cells = HeapAlloc(GetProcessHeap(), 0, width * rows * sizeof(WCHAR));
        if (cells)
            ok = ReadConsoleOutputCharacterW(out, cells, width * rows, origin, &read);
        GetCurrentConsoleFont(out, FALSE, &font);
    }
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    detach_console();
    if (ok)
        buf = HeapAlloc(GetProcessHeap(), 0, (width + 1) * rows * sizeof(WCHAR));
    if (!buf)
    {
        HeapFree(GetProcessHeap(), 0, cells);
        return FALSE;
    }
    for (len = read; len < width * rows; len++)
        cells[len] = ' ';

    AcquireSRWLockExclusive(&console_view_lock);
    console_view.hwnd = hwnd;
    console_view.first_row = origin.Y;
    console_view.window = info.srWindow;
    console_view.cell = font.dwFontSize;
    ReleaseSRWLockExclusive(&console_view_lock);

    cursor_row = info.dwCursorPosition.Y - origin.Y;
    for (y = 0; y < rows; y++)
    {
        const WCHAR *row = cells + y * width;

        if (y)
            buf[pos++] = '\n';
        for (len = width; len > 0 && row[len - 1] == ' '; len--)
            ;
        if (y == cursor_row)
        {
            len = max(len, info.dwCursorPosition.X);
            caret = pos + info.dwCursorPosition.X;
        }
        memcpy(buf + pos, row, len * sizeof(WCHAR));
        pos += len;
        if (len || y <= cursor_row)
            end = pos;
    }
    buf[end] = 0;
    HeapFree(GetProcessHeap(), 0, cells);

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = CONSOLE_CHILD;
    obj->role = BRIDGE_ROLE_TERMINAL;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_MULTI_LINE);
    str->object[0] = 0;
    obj->name = str->object;
    obj->value = NULL;
    obj->caret = obj->selection_start = obj->selection_end = char_offset(buf, end, caret);
    str->text = new_text(buf, end);
    obj->text = str->text;
    HeapFree(GetProcessHeap(), 0, buf);
    get_extents(hwnd, obj);
    return TRUE;
}

/*
 * Text extents of the console text from describe_console, through the
 * view saved there. A range outside the window lies TEXT_FAR_AWAY above
 * or below it, as for an Edit. Each UTF-16 unit is taken as one cell.
 */
int console_text_extents(HWND hwnd, const unsigned int *chars, int count,
                                int start, int end, int *rect)
{
    int i, line = 0, col = 0, first_line = 0, first_col = 0, top, bottom, cols;
    SMALL_RECT win;
    COORD cell;
    RECT r;

    AcquireSRWLockShared(&console_view_lock);
    i = console_view.hwnd == hwnd;
    top = console_view.first_row;
    win = console_view.window;
    cell = console_view.cell;
    ReleaseSRWLockShared(&console_view_lock);
    start = max(start, 0);
    end = min(end, count);
    if (!i || cell.X <= 0 || cell.Y <= 0 || start >= end)
        return 0;

    for (i = 0; i < end; i++)
    {
        if (i == start)
        {
            first_line = line;
            first_col = col;
        }
        if (i == end - 1)
            break;
        if (chars[i] == '\n')
        {
            line++;
            col = 0;
        }
        else
            col += chars[i] > 0xffff ? 2 : 1;
    }
    /* The last character, a line break included, takes one cell (two if astral). */
    col += chars[end - 1] > 0xffff ? 2 : 1;

    bottom = top + line;
    top += first_line;
    cols = win.Right - win.Left + 1;
    if (bottom < win.Top || top > win.Bottom)
    {
        r.left = r.right = 0;
        r.top = bottom < win.Top ? -TEXT_FAR_AWAY : TEXT_FAR_AWAY;
        r.bottom = r.top + cell.Y;
        client_to_screen(hwnd, r, rect);
        return 1;
    }
    r.top = (max(top, win.Top) - win.Top) * cell.Y;
    r.bottom = (min(bottom, win.Bottom) - win.Top + 1) * cell.Y;
    if (line != first_line)
    {
        r.left = 0;
        r.right = cols * cell.X;
    }
    else
    {
        r.left = min(max(first_col - win.Left, 0), cols) * cell.X;
        r.right = min(max(col - win.Left, 0), cols) * cell.X;
    }
    client_to_screen(hwnd, r, rect);
    return 1;
}
