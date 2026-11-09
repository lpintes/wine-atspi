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
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include "bridge.h"
#include "menuhook.h"

#define NAME_MAX_UTF8 1024
/* Columns of a list view in report view beyond this are left out. */
#define MAX_COLUMNS 16
/* Texts of a window that name no control, see describe_labels. */
#define MAX_LABELS 16
#define LABEL_MAX_UTF8 2048
/* All parts of a status bar, in UTF-16 units; see report_status_bar. */
#define STATUS_TEXT_MAX 1024
/* A status bar is read after each key: do not wait long for a busy target. */
#define STATUS_TIMEOUT_MS 100

/* Held by the running bridge: one bridge per prefix. */
#define BRIDGE_MUTEX L"WineAtspiBridge"
/* wineboot --init starts these at every session start (not Run). */
#define RUN_SERVICES_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices"
#define RUN_SERVICES_VALUE L"WineAtspi"
/* ProcessWineMakeProcessSystem, declared only under __WINESRC__. */
#define PROCESS_WINE_MAKE_PROCESS_SYSTEM ((PROCESSINFOCLASS)1000)

static BOOL debug;
/* Where trace() writes: stderr, or the file given with --log. */
static HANDLE trace_handle;

/* Writes a line to stderr (or the log file) when debugging is on. */
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
    WriteFile(trace_handle, utf8, len, &written, NULL);
}

/* Writes a line to stdout, for the install and uninstall commands. */
static void print_line(const WCHAR *text)
{
    char utf8[3072];
    DWORD written;
    int len;

    len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (len <= 0)
        return;
    utf8[len - 1] = '\n';
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), utf8, len, &written, NULL);
}

/* UTF-8 buffers backing the strings of one bridge_focus. */
struct focus_strings
{
    char app[MAX_PATH * 3];
    char frame[NAME_MAX_UTF8];
    char container[NAME_MAX_UTF8];
    char object[NAME_MAX_UTF8];
    char value[NAME_MAX_UTF8];
    char frame_class[256 * 3];
    char object_class[256 * 3];
    /* Column texts of a table: cells of the focused row, or the headers. */
    char cells[MAX_COLUMNS][NAME_MAX_UTF8];
    const char *cell_ptrs[MAX_COLUMNS];
    int cell_extents[MAX_COLUMNS * 4];
    char headers[MAX_COLUMNS][NAME_MAX_UTF8];
    const char *header_ptrs[MAX_COLUMNS];
    char labels[MAX_LABELS][LABEL_MAX_UTF8];
    struct bridge_object label_objects[MAX_LABELS];
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
        { L"ScrollBar", BRIDGE_ROLE_SCROLL_BAR },
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

/* Screen rectangle of a whole window. */
static void get_extents(HWND hwnd, struct bridge_object *obj)
{
    RECT rect;

    obj->has_extents = GetWindowRect(hwnd, &rect);
    if (!obj->has_extents)
        return;
    obj->x = rect.left;
    obj->y = rect.top;
    obj->width = rect.right - rect.left;
    obj->height = rect.bottom - rect.top;
}

/* rect in the client coordinates of hwnd as x, y, width and height on the screen. */
static void client_to_screen(HWND hwnd, RECT rect, int *out)
{
    MapWindowPoints(hwnd, HWND_DESKTOP, (POINT *)&rect, 2);
    out[0] = rect.left;
    out[1] = rect.top;
    out[2] = rect.right - rect.left;
    out[3] = rect.bottom - rect.top;
}

/* Screen rectangle of an item, from rect in the client coordinates of its list. */
static void set_item_extents(HWND hwnd, RECT rect, struct bridge_object *obj)
{
    int r[4];

    client_to_screen(hwnd, rect, r);
    obj->has_extents = TRUE;
    obj->x = r[0];
    obj->y = r[1];
    obj->width = r[2];
    obj->height = r[3];
}

/*
 * UDM_GETRANGE32 writes through two pointers, which Wine does not marshal,
 * so they point into memory in the target.
 */
static BOOL updown_range(HWND hwnd, DWORD_PTR *low, DWORD_PTR *high)
{
    INT range[2] = { 0, 0 };
    DWORD pid = 0;
    HANDLE process;
    char *base;
    SIZE_T done;
    BOOL ok;

    GetWindowThreadProcessId(hwnd, &pid);
    process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                          FALSE, pid);
    if (!process)
        return FALSE;
    base = VirtualAllocEx(process, NULL, sizeof(range), MEM_COMMIT, PAGE_READWRITE);
    ok = base &&
         WriteProcessMemory(process, base, range, sizeof(range), &done) &&
         SendMessageTimeoutW(hwnd, UDM_GETRANGE32, (WPARAM)base, (LPARAM)(base + sizeof(INT)),
                             SMTO_ABORTIFHUNG, 500, NULL) &&
         ReadProcessMemory(process, base, range, sizeof(range), &done);
    if (base)
        VirtualFreeEx(process, base, 0, MEM_RELEASE);
    CloseHandle(process);
    *low = (DWORD_PTR)range[0];
    *high = (DWORD_PTR)range[1];
    return ok;
}

/*
 * The up-down control whose buddy is hwnd, or NULL. Focus stays in the
 * buddy; the up-down subclasses it and handles its arrow keys
 * (comctl32/updown.c), so the pair is one spin button.
 */
static HWND find_updown(HWND hwnd)
{
    HWND parent = GetParent(hwnd), sibling;
    WCHAR cls[32];
    DWORD_PTR buddy;

    if (!parent)
        return NULL;
    for (sibling = GetWindow(parent, GW_CHILD); sibling;
         sibling = GetWindow(sibling, GW_HWNDNEXT))
    {
        if (!GetClassNameW(sibling, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"msctls_updown32"))
            continue;
        if (SendMessageTimeoutW(sibling, UDM_GETBUDDY, 0, 0, SMTO_ABORTIFHUNG, 500, &buddy) &&
            (HWND)buddy == hwnd)
            return sibling;
    }
    return NULL;
}

/*
 * Position and range of a trackbar, progress bar or up-down. Wine's oleacc
 * has no proxy for any of them. Their messages carry no pointers, except
 * UDM_GETRANGE32.
 */
static void describe_range(HWND hwnd, const WCHAR *cls, struct bridge_object *obj)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR pos, low, high, step = 0;
    BOOL vertical;

    if (!lstrcmpiW(cls, L"msctls_trackbar32"))
    {
        if (!SendMessageTimeoutW(hwnd, TBM_GETPOS, 0, 0, SMTO_ABORTIFHUNG, 500, &pos) ||
            !SendMessageTimeoutW(hwnd, TBM_GETRANGEMIN, 0, 0, SMTO_ABORTIFHUNG, 500, &low) ||
            !SendMessageTimeoutW(hwnd, TBM_GETRANGEMAX, 0, 0, SMTO_ABORTIFHUNG, 500, &high))
            return;
        SendMessageTimeoutW(hwnd, TBM_GETLINESIZE, 0, 0, SMTO_ABORTIFHUNG, 500, &step);
        vertical = (style & TBS_VERT) != 0;
    }
    else if (!lstrcmpiW(cls, L"msctls_progress32"))
    {
        if (!SendMessageTimeoutW(hwnd, PBM_GETPOS, 0, 0, SMTO_ABORTIFHUNG, 500, &pos) ||
            !SendMessageTimeoutW(hwnd, PBM_GETRANGE, TRUE, 0, SMTO_ABORTIFHUNG, 500, &low) ||
            !SendMessageTimeoutW(hwnd, PBM_GETRANGE, FALSE, 0, SMTO_ABORTIFHUNG, 500, &high))
            return;
        vertical = (style & PBS_VERTICAL) != 0;
    }
    else if (!lstrcmpiW(cls, L"msctls_updown32"))
    {
        if (!updown_range(hwnd, &low, &high) ||
            !SendMessageTimeoutW(hwnd, UDM_GETPOS32, 0, 0, SMTO_ABORTIFHUNG, 500, &pos))
            return;
        step = 1;
        vertical = !(style & UDS_HORZ);
    }
    else
        return;
    obj->has_range = TRUE;
    obj->current = (int)pos;
    obj->minimum = (int)low;
    obj->maximum = (int)high;
    obj->increment = (int)step;
    obj->states |= BRIDGE_STATE_BIT(vertical ? BRIDGE_STATE_VERTICAL : BRIDGE_STATE_HORIZONTAL);
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
 * A RichEdit (2.0 and later) ends a paragraph with one "\r" inside, so its
 * EM_GETSEL counts the "\r\n" that WM_GETTEXT gives as one character
 * (riched20/editor.c). Turn such a position into an index into text.
 */
static int rich_text_index(const WCHAR *text, int len, int pos)
{
    int i = 0;

    for (; pos > 0 && i < len; pos--)
        i += text[i] == '\r' && i + 1 < len && text[i + 1] == '\n' ? 2 : 1;
    return i;
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
static void describe_edit_text(HWND hwnd, BOOL rich, struct bridge_object *obj,
                               struct focus_strings *str)
{
    DWORD_PTR len = 0;
    DWORD start = 0, end = 0, start_index, end_index;
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

static BOOL has_edit_text(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, L"RICHEDIT50W") ||
           !lstrcmpiW(cls, L"RichEdit20A") || !lstrcmpiW(cls, L"RichEdit20W");
}

/* A range out of the client area lies this far above or below the window. */
#define TEXT_FAR_AWAY 100000

/* Lines below the first visible one whose packed EM_POSFROMCHAR y is sure not to wrap. */
#define TEXT_SAFE_LINES 1000

/*
 * Index into the text of an Edit (rich FALSE) or RichEdit of AT-SPI
 * offset in chars: astral code points take two UTF-16 units, and "\n" was
 * "\r\n" in an Edit but one "\r" in a RichEdit (see rich_text_index).
 */
static int control_index(const unsigned int *chars, int count, int offset, BOOL rich)
{
    int i, index = 0;

    for (i = 0; i < offset && i < count; i++)
        index += (chars[i] > 0xffff ? 2 : 1) + (!rich && chars[i] == '\n');
    return index;
}

/* Client position of the character at index; FALSE past the end of the text. */
static BOOL text_pos(HWND hwnd, int index, POINT *pt)
{
    DWORD_PTR pos;

    if (!SendMessageTimeoutW(hwnd, EM_POSFROMCHAR, index, 0, SMTO_ABORTIFHUNG, 500, &pos) ||
        pos == (DWORD_PTR)-1)
        return FALSE;
    pt->x = (short)LOWORD(pos);
    pt->y = (short)HIWORD(pos);
    return TRUE;
}

static int text_send(HWND hwnd, UINT msg, WPARAM wparam)
{
    DWORD_PTR result = 0;

    SendMessageTimeoutW(hwnd, msg, wparam, 0, SMTO_ABORTIFHUNG, 500, &result);
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
 * end; the last line has none, so its last character gets the average
 * width of the line.
 */
static int text_line_end(HWND hwnd, int index, int len, int height)
{
    int start = text_send(hwnd, EM_LINEINDEX, text_send(hwnd, EM_LINEFROMCHAR, index));
    int end = start + text_send(hwnd, EM_LINELENGTH, index);
    POINT first, last;

    if (end < len && text_pos(hwnd, end, &last))
        return last.x;
    if (end <= start || !text_pos(hwnd, end - 1, &last))
        return 0;
    if (end - 1 > start && text_pos(hwnd, start, &first) && last.x > first.x)
        return last.x + (last.x - first.x) / (end - 1 - start);
    return last.x + height / 2;
}

static int console_text_extents(HWND hwnd, const unsigned int *chars, int count,
                                int start, int end, int *rect);

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

int bridge_click(unsigned long long handle)
{
    HWND hwnd = (HWND)(ULONG_PTR)handle;
    WCHAR cls[16];

    if (!hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"Button") ||
        (GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX ||
        !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd))
        return 0;
    /*
     * Wine's BM_CLICK sends the button WM_LBUTTONDOWN and WM_LBUTTONUP
     * (user32/button.c), which focus it and notify BN_CLICKED. Posted,
     * because the click may open a modal dialog.
     */
    return PostMessageW(hwnd, BM_CLICK, 0, 0);
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

/* Drops the "&" that marks a mnemonic; "&&" stays as one "&". */
static void strip_ampersands(WCHAR *buf)
{
    int i, j;

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

/* Drops the "&" of the mnemonic from the text buf of Static hwnd. */
static void strip_mnemonic(HWND hwnd, WCHAR *buf)
{
    if (!(GetWindowLongW(hwnd, GWL_STYLE) & SS_NOPREFIX))
        strip_ampersands(buf);
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
static BOOL describe_label(HWND hwnd, char *dst, int size, struct bridge_object *obj)
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
static void describe_labels(HWND top, struct bridge_focus *focus, struct focus_strings *str)
{
    struct label_search search = { focus, str };

    focus->label_count = 0;
    focus->labels = str->label_objects;
    EnumChildWindows(top, add_label, (LPARAM)&search);
}

/* LVITEMW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct lvitem32
{
    UINT mask;
    int iItem;
    int iSubItem;
    UINT state;
    UINT stateMask;
    ULONG pszText;
    int cchTextMax;
};

/* TVITEMW as a 32-bit (WoW64) process sees it. */
struct tvitem32
{
    UINT mask;
    ULONG hItem;
    UINT state;
    UINT stateMask;
    ULONG pszText;
    int cchTextMax;
    int iImage;
    int iSelectedImage;
    int cChildren;
    ULONG lParam;
};

/* LVCOLUMNW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct lvcolumn32
{
    UINT mask;
    int fmt;
    int cx;
    ULONG pszText;
    int cchTextMax;
};

/* TCITEMW as a 32-bit (WoW64) process sees it, up to cchTextMax. */
struct tcitem32
{
    UINT mask;
    DWORD dwState;
    DWORD dwStateMask;
    ULONG pszText;
    int cchTextMax;
};

/* Room for the structure in front of the text in the target. */
#define REMOTE_HEADER 128
/* Text of one list view cell or column header, in WCHARs. */
#define CELL_TEXT_MAX (NAME_MAX_UTF8 / 3)

/*
 * Wine does not marshal list view messages with pointers across
 * processes. Like NVDA, put the structure and its text buffer into a
 * block of the target's memory: the structure at the start, the text
 * at REMOTE_HEADER.
 */
struct remote
{
    HANDLE process;
    char *base;
    BOOL wow64;
};

static BOOL remote_open(HWND hwnd, struct remote *r)
{
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    r->wow64 = FALSE;
    r->process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                             PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!r->process)
        return FALSE;
    IsWow64Process(r->process, &r->wow64);
    r->base = VirtualAllocEx(r->process, NULL, REMOTE_HEADER + CELL_TEXT_MAX * sizeof(WCHAR),
                             MEM_COMMIT, PAGE_READWRITE);
    if (r->base)
        return TRUE;
    CloseHandle(r->process);
    return FALSE;
}

static void remote_close(struct remote *r)
{
    VirtualFreeEx(r->process, r->base, 0, MEM_RELEASE);
    CloseHandle(r->process);
}

/*
 * Sends msg with a pointer to header, copied to the target, and reads
 * the text the target wrote at REMOTE_HEADER into buf (CELL_TEXT_MAX).
 */
static BOOL remote_text(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam,
                        const BYTE *header, WCHAR *buf)
{
    static const WCHAR empty = 0;
    DWORD_PTR ret;
    SIZE_T done;

    buf[0] = 0;
    /* Clear the text, a control may leave an empty one unwritten. */
    if (!WriteProcessMemory(r->process, r->base, header, REMOTE_HEADER, &done) ||
        !WriteProcessMemory(r->process, r->base + REMOTE_HEADER, &empty, sizeof(empty), &done) ||
        !SendMessageTimeoutW(hwnd, msg, wparam, (LPARAM)r->base, SMTO_ABORTIFHUNG, 500, &ret) ||
        !ReadProcessMemory(r->process, r->base + REMOTE_HEADER, buf,
                           CELL_TEXT_MAX * sizeof(WCHAR), &done))
        return FALSE;
    buf[CELL_TEXT_MAX - 1] = 0;
    return TRUE;
}

/*
 * Sends msg with a pointer to a RECT in the target that starts as rect,
 * and reads it back. Item rect messages of comctl32 take their input in
 * the RECT (LVM_GETSUBITEMRECT, TVM_GETITEMRECT).
 */
static BOOL remote_rect(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam, RECT *rect)
{
    DWORD_PTR ret = 0;
    SIZE_T done;

    return WriteProcessMemory(r->process, r->base, rect, sizeof(*rect), &done) &&
           SendMessageTimeoutW(hwnd, msg, wparam, (LPARAM)r->base, SMTO_ABORTIFHUNG, 500, &ret) &&
           ret && ReadProcessMemory(r->process, r->base, rect, sizeof(*rect), &done);
}

/* Text of item's subitem (0 for the item itself) of a list view. */
static BOOL get_listview_text(HWND hwnd, struct remote *r, int item, int subitem, WCHAR *buf)
{
    BYTE header[REMOTE_HEADER];

    memset(header, 0, sizeof(header));
    if (r->wow64)
    {
        struct lvitem32 *lv = (struct lvitem32 *)header;

        lv->iSubItem = subitem;
        lv->pszText = (ULONG)(ULONG_PTR)(r->base + REMOTE_HEADER);
        lv->cchTextMax = CELL_TEXT_MAX;
    }
    else
    {
        LVITEMW *lv = (LVITEMW *)header;

        lv->iSubItem = subitem;
        lv->pszText = (WCHAR *)(r->base + REMOTE_HEADER);
        lv->cchTextMax = CELL_TEXT_MAX;
    }
    return remote_text(hwnd, r, LVM_GETITEMTEXTW, item, header, buf);
}

/* Header text of column (a subitem index) of a list view. */
static BOOL get_listview_header(HWND hwnd, struct remote *r, int column, WCHAR *buf)
{
    BYTE header[REMOTE_HEADER];

    memset(header, 0, sizeof(header));
    if (r->wow64)
    {
        struct lvcolumn32 *lv = (struct lvcolumn32 *)header;

        lv->mask = LVCF_TEXT;
        lv->pszText = (ULONG)(ULONG_PTR)(r->base + REMOTE_HEADER);
        lv->cchTextMax = CELL_TEXT_MAX;
    }
    else
    {
        LVCOLUMNW *lv = (LVCOLUMNW *)header;

        lv->mask = LVCF_TEXT;
        lv->pszText = (WCHAR *)(r->base + REMOTE_HEADER);
        lv->cchTextMax = CELL_TEXT_MAX;
    }
    return remote_text(hwnd, r, LVM_GETCOLUMNW, column, header, buf);
}

/*
 * Columns of a list view in report view: their subitem indices in display
 * order (the user may drag columns). 0 in other views. Wine changes the
 * view with LVM_SETVIEW without touching the style, so ask for the view.
 */
static int listview_columns(HWND hwnd, struct remote *r, int *order)
{
    DWORD_PTR view = 0, header = 0, count = 0, ok = 0;
    SIZE_T done;
    int i;

    if (!SendMessageTimeoutW(hwnd, LVM_GETVIEW, 0, 0, SMTO_ABORTIFHUNG, 500, &view) ||
        view != LV_VIEW_DETAILS ||
        !SendMessageTimeoutW(hwnd, LVM_GETHEADER, 0, 0, SMTO_ABORTIFHUNG, 500, &header) ||
        !header ||
        !SendMessageTimeoutW((HWND)header, HDM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) ||
        (LONG)count <= 0)
        return 0;
    for (i = 0; i < MAX_COLUMNS; i++)
        order[i] = i;
    /* The header wants room for all columns (header.c, HEADER_GetOrderArray). */
    if (count > MAX_COLUMNS)
        return MAX_COLUMNS;
    if (SendMessageTimeoutW(hwnd, LVM_GETCOLUMNORDERARRAY, count, (LPARAM)r->base,
                            SMTO_ABORTIFHUNG, 500, &ok) && ok &&
        ReadProcessMemory(r->process, r->base, order, count * sizeof(int), &done))
    {
        for (i = 0; i < (int)count; i++)
            if (order[i] < 0 || order[i] >= (int)count)
                order[i] = i;
    }
    return (int)count;
}

/*
 * Index of the focused item of a list view, -1 if none. LIST_BUSY if the
 * target did not answer in time: the Open dialog's list keeps its thread
 * busy for over 500 ms after an arrow key (WSL, 2026-11-06).
 */
#define LIST_BUSY (-2)

static int listview_focus(HWND hwnd)
{
    DWORD_PTR item = (DWORD_PTR)-1;

    if (!SendMessageTimeoutW(hwnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED,
                             SMTO_ABORTIFHUNG, 500, &item))
        return LIST_BUSY;
    return (int)item;
}

/*
 * Wine's oleacc has only a stub for SysListView32, and the list view
 * emits no focus or selection events, so describe the item through list
 * view messages. The MSAA child id is item + 1, as on Windows. In report
 * view the item is the cell of the first column of a table row, and the
 * texts of the whole row go along, as Orca reads the full row of a GUI
 * table (generator.py, _cells_to_present). FALSE if the text could not
 * be read.
 */
static BOOL describe_listview_item(HWND hwnd, int item, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR state = 0, count = 0;
    int order[MAX_COLUMNS], columns, i;
    struct remote r;
    RECT rect;
    BOOL ok, has_rect = FALSE;

    if (!remote_open(hwnd, &r))
        return FALSE;
    columns = listview_columns(hwnd, &r, order);
    ok = get_listview_text(hwnd, &r, item, columns ? order[0] : 0, text);
    for (i = 0; ok && i < columns; i++)
    {
        if (i && !get_listview_text(hwnd, &r, item, order[i], text))
            text[0] = 0;
        to_utf8(text, str->cells[i], sizeof(str->cells[i]));
        str->cell_ptrs[i] = str->cells[i];
        /* Subitem 0 gives the whole row; its own part is the label. */
        SetRect(&rect, order[i] ? LVIR_BOUNDS : LVIR_LABEL, order[i], 0, 0);
        if (!remote_rect(hwnd, &r, order[i] ? LVM_GETSUBITEMRECT : LVM_GETITEMRECT, item, &rect))
            SetRectEmpty(&rect);
        client_to_screen(hwnd, rect, &str->cell_extents[4 * i]);
    }
    if (ok && !columns)
    {
        SetRect(&rect, LVIR_BOUNDS, 0, 0, 0);
        has_rect = remote_rect(hwnd, &r, LVM_GETITEMRECT, item, &rect);
    }
    remote_close(&r);
    if (!ok)
        return FALSE;
    if (has_rect)
        set_item_extents(hwnd, rect, obj);
    else if (columns)
    {
        obj->has_extents = TRUE;
        obj->x = str->cell_extents[0];
        obj->y = str->cell_extents[1];
        obj->width = str->cell_extents[2];
        obj->height = str->cell_extents[3];
    }
    else
        obj->has_extents = FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = columns ? BRIDGE_ROLE_TABLE_CELL : BRIDGE_ROLE_LIST_ITEM;
    obj->value = NULL;
    obj->column_count = columns;
    obj->columns = columns ? str->cell_ptrs : NULL;
    obj->column_extents = columns ? str->cell_extents : NULL;
    if (columns)
        lstrcpyA(str->object, str->cells[0]);
    else
        to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE);
    SendMessageTimeoutW(hwnd, LVM_GETITEMSTATE, item, LVIS_FOCUSED | LVIS_SELECTED,
                        SMTO_ABORTIFHUNG, 500, &state);
    if (state & LVIS_FOCUSED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (state & LVIS_SELECTED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    SendMessageTimeoutW(hwnd, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count);
    obj->position = item + 1;
    obj->set_size = (int)count;
    return TRUE;
}

static BOOL is_list_box(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"ListBox") || !lstrcmpiW(cls, L"ComboLBox");
}

static BOOL is_tree_view(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"SysTreeView32");
}

static BOOL is_tab_control(const WCHAR *cls)
{
    return !lstrcmpiW(cls, L"SysTabControl32");
}

/* A control whose items the bridge reads with messages: a list, list view, tree view or tab control. */
static BOOL is_item_control(const WCHAR *cls)
{
    return is_list_box(cls) || !lstrcmpiW(cls, L"SysListView32") || is_tree_view(cls) ||
           is_tab_control(cls);
}

/*
 * The focused tab of a tab control, -1 if none; LIST_BUSY if it did not
 * answer. Without TCS_BUTTONS it is the selected tab as well.
 */
static int tab_focus(HWND hwnd)
{
    DWORD_PTR item = (DWORD_PTR)-1;

    if (!SendMessageTimeoutW(hwnd, TCM_GETCURFOCUS, 0, 0, SMTO_ABORTIFHUNG, 500, &item))
        return LIST_BUSY;
    return (int)item;
}

/*
 * Wine's oleacc has no proxy for SysTabControl32, and arrow keys emit only
 * EVENT_OBJECT_SELECTION (comctl32/tab.c, TAB_SetCurFocus; a focus event
 * only with TCS_BUTTONS), so the tab is read with tab control messages.
 * The MSAA child id is item + 1, as on Windows. FALSE if the text could
 * not be read.
 */
static BOOL describe_tab_item(HWND hwnd, int item, struct bridge_object *obj,
                              struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR selected = (DWORD_PTR)-1, count = 0;
    struct remote r;
    RECT rect;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    memset(header, 0, sizeof(header));
    if (r.wow64)
    {
        struct tcitem32 *tc = (struct tcitem32 *)header;

        tc->mask = TCIF_TEXT;
        tc->pszText = (ULONG)(ULONG_PTR)(r.base + REMOTE_HEADER);
        tc->cchTextMax = CELL_TEXT_MAX;
    }
    else
    {
        TCITEMW *tc = (TCITEMW *)header;

        tc->mask = TCIF_TEXT;
        tc->pszText = (WCHAR *)(r.base + REMOTE_HEADER);
        tc->cchTextMax = CELL_TEXT_MAX;
    }
    ok = remote_text(hwnd, &r, TCM_GETITEMW, item, header, text);
    SetRectEmpty(&rect);
    obj->has_extents = FALSE;
    if (ok && remote_rect(hwnd, &r, TCM_GETITEMRECT, item, &rect))
        set_item_extents(hwnd, rect, obj);
    remote_close(&r);
    if (!ok)
        return FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = BRIDGE_ROLE_PAGE_TAB;
    obj->value = NULL;
    to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (SendMessageTimeoutW(hwnd, TCM_GETCURSEL, 0, 0, SMTO_ABORTIFHUNG, 500, &selected) &&
        (int)selected == item)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count);
    obj->position = item + 1;
    obj->set_size = (int)count;
    return TRUE;
}

/*
 * Role, states and item count of a ListBox or SysListView32 as a whole:
 * focused without a focused item, or the parent of its items. Orca never
 * speaks the role of a focused "list" and says nothing for an unnamed
 * one, and skips a "list" ancestor by default; for a "list box" it says
 * "n items" from setsize, or the role if the list is empty
 * (speech_generator.py). A list view in report view is a table with its
 * column headers instead; set_size then counts the rows.
 */
static void describe_list_box(HWND hwnd, BOOL listview, struct bridge_object *obj,
                              struct focus_strings *str)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR count = 0;
    WCHAR text[CELL_TEXT_MAX];
    int order[MAX_COLUMNS], i;
    struct remote r;

    obj->role = BRIDGE_ROLE_LIST_BOX;
    if (listview ? !(style & LVS_SINGLESEL) : (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_MULTISELECTABLE);
    if (SendMessageTimeoutW(hwnd, listview ? LVM_GETITEMCOUNT : LB_GETCOUNT, 0, 0,
                            SMTO_ABORTIFHUNG, 500, &count) && (LONG)count >= 0)
        obj->set_size = (int)count;
    if (!listview || !remote_open(hwnd, &r))
        return;
    obj->column_count = listview_columns(hwnd, &r, order);
    for (i = 0; i < obj->column_count; i++)
    {
        if (!get_listview_header(hwnd, &r, order[i], text))
            text[0] = 0;
        to_utf8(text, str->headers[i], sizeof(str->headers[i]));
        str->header_ptrs[i] = str->headers[i];
    }
    remote_close(&r);
    if (obj->column_count)
    {
        obj->role = BRIDGE_ROLE_TABLE;
        obj->columns = str->header_ptrs;
    }
}

/*
 * The list window as parent of its focused item, so that Orca presents
 * the list's label when focus enters it. No MSAA: Wine's oleacc names a
 * list only by its window text. A tree view is a tree; its items are its
 * children in the order they had focus, nesting is told by their level.
 */
static void describe_container(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    WCHAR cls[256];

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    memset(obj, 0, sizeof(*obj));
    if (!is_list_box(cls) && lstrcmpiW(cls, L"SysListView32") && !is_tree_view(cls) &&
        !is_tab_control(cls))
        return;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->window_class = str->object_class;
    find_label(hwnd, str->container, sizeof(str->container));
    obj->name = str->container;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE);
    if (is_tree_view(cls))
        obj->role = BRIDGE_ROLE_TREE;
    else if (is_tab_control(cls))
    {
        DWORD_PTR count = 0;

        obj->role = BRIDGE_ROLE_PAGE_TAB_LIST;
        if (SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count) &&
            (LONG)count >= 0)
            obj->set_size = (int)count;
    }
    else
        describe_list_box(hwnd, !is_list_box(cls), obj, str);
    get_extents(hwnd, obj);
}

/*
 * Wine's winefile draws its tree and file panes as owner-drawn list boxes
 * without strings (programs/winefile/winefile.c, create_tree_window). The pane's
 * parent has class WFS_Tree, the process is winefile.exe (another program
 * with that class would have another layout) and the item data points to an Entry in the
 * target: three pointers, three ints, then the WIN32_FIND_DATAW whose
 * cFileName is the text it draws (draw_item). Reads that name into buf.
 */
static BOOL winefile_entry_name(HWND hwnd, int item, char *buf, int size)
{
    WCHAR cls[32], name[MAX_PATH];
    char app[32];
    DWORD_PTR entry = 0;
    DWORD pid = 0;
    HANDLE process;
    BOOL wow64 = FALSE, ok;
    SIZE_T offset, done = 0;

    if (!GetClassNameW(GetParent(hwnd), cls, ARRAYSIZE(cls)) || lstrcmpiW(cls, L"WFS_Tree"))
        return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    get_app_name(pid, app, sizeof(app));
    if (lstrcmpiA(app, "winefile"))
        return FALSE;
    if (!SendMessageTimeoutW(hwnd, LB_GETITEMDATA, item, 0, SMTO_ABORTIFHUNG, 500, &entry) ||
        !entry || (LONG_PTR)entry == LB_ERR)
        return FALSE;
    if (!(process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
        return FALSE;
    IsWow64Process(process, &wow64);
    offset = 3 * (wow64 ? 4 : sizeof(void *)) + 3 * sizeof(int) +
             offsetof(WIN32_FIND_DATAW, cFileName);
    ok = ReadProcessMemory(process, (void *)(entry + offset), name, sizeof(name), &done) &&
         done == sizeof(name);
    CloseHandle(process);
    if (!ok)
        return FALSE;
    name[MAX_PATH - 1] = 0;
    to_utf8(name, buf, size);
    return TRUE;
}

/*
 * Wine's oleacc has only a stub for ListBox too: name and role of an item
 * fail with E_INVALIDARG (client.c). LB_GETTEXT is marshalled across
 * processes. An owner-drawn list without LBS_HASSTRINGS keeps no text;
 * its item is left unnamed unless it is a winefile pane. TRUE if obj was
 * filled, FALSE if the list has no such item, LIST_BUSY if it did not
 * answer.
 */
static int describe_listbox_item(HWND hwnd, int item, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR count = 0, len = 0, sel = 0, ret = 0;
    WCHAR *text;
    RECT rect;

    if (!SendMessageTimeoutW(hwnd, LB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
        return LIST_BUSY;
    if (item < 0 || item >= (LONG)count)
        return FALSE;
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = item + 1;
    obj->role = BRIDGE_ROLE_LIST_ITEM;
    obj->value = NULL;
    str->object[0] = 0;
    obj->name = str->object;
    if (!(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (style & LBS_HASSTRINGS))
    {
        if (!SendMessageTimeoutW(hwnd, LB_GETTEXTLEN, item, 0, SMTO_ABORTIFHUNG, 500, &len))
            return LIST_BUSY;
        if ((LONG)len > 0 && (text = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR))))
        {
            text[0] = 0;
            if (SendMessageTimeoutW(hwnd, LB_GETTEXT, item, (LPARAM)text,
                                    SMTO_ABORTIFHUNG, 500, &len) && (LONG)len >= 0)
            {
                text[len] = 0;
                to_utf8(text, str->object, sizeof(str->object));
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    else
        winefile_entry_name(hwnd, item, str->object, sizeof(str->object));
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (SendMessageTimeoutW(hwnd, LB_GETSEL, item, 0, SMTO_ABORTIFHUNG, 500, &sel) &&
        (LONG)sel > 0)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    obj->has_extents = FALSE;
    if (SendMessageTimeoutW(hwnd, LB_GETITEMRECT, item, (LPARAM)&rect,
                            SMTO_ABORTIFHUNG, 500, &ret) && (LONG)ret != LB_ERR)
        set_item_extents(hwnd, rect, obj);
    obj->position = item + 1;
    obj->set_size = (int)count;
    return TRUE;
}

/*
 * Wine's SysTreeView32 emits no WinEvents at all (comctl32/treeview.c)
 * and oleacc has no proxy for it, so its items are read with tree view
 * messages. TVM_GETNEXTITEM carries no pointer; an HTREEITEM is a pointer
 * in the target, only passed back.
 */

/* Bounds the walks over parents and siblings of a tree item. */
#define TREE_WALK_MAX 10000

static HTREEITEM tree_next(HWND hwnd, UINT flag, HTREEITEM item)
{
    DWORD_PTR next = 0;

    if (!SendMessageTimeoutW(hwnd, TVM_GETNEXTITEM, flag, (LPARAM)item,
                             SMTO_ABORTIFHUNG, 500, &next))
        return NULL;
    return (HTREEITEM)next;
}

/* The caret item of a tree view into *item, NULL if none; LIST_BUSY if it did not answer. */
static int tree_focus(HWND hwnd, HTREEITEM *item)
{
    DWORD_PTR caret = 0;

    *item = NULL;
    if (!SendMessageTimeoutW(hwnd, TVM_GETNEXTITEM, TVGN_CARET, 0,
                             SMTO_ABORTIFHUNG, 500, &caret))
        return LIST_BUSY;
    *item = (HTREEITEM)caret;
    return TRUE;
}

/*
 * MSAA child id of a tree item, made from its handle: the items are heap
 * blocks in the target, at least 16 bytes apart. Positive, as child ids
 * of items are elsewhere.
 */
static LONG tree_child_id(HTREEITEM item)
{
    LONG id = (LONG)(((ULONG_PTR)item >> 4) & 0x7fffffff);

    return id ? id : 1;
}

/*
 * Describes the tree item: text, selected and expanded state, its level
 * (from 1) and its place among its siblings. Orca takes the level from
 * the attribute "level" of an item inside a tree (script_utilities.py,
 * node_level) and says it when it changes. FALSE if the item could not be
 * read.
 */
static BOOL describe_treeview_item(HWND hwnd, HTREEITEM item, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    HTREEITEM next;
    struct remote r;
    RECT rect;
    UINT state;
    int children, i;
    SIZE_T done;
    BOOL ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    memset(header, 0, sizeof(header));
    if (r.wow64)
    {
        struct tvitem32 *tv = (struct tvitem32 *)header;

        tv->mask = TVIF_HANDLE | TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN;
        tv->hItem = (ULONG)(ULONG_PTR)item;
        tv->stateMask = TVIS_SELECTED | TVIS_EXPANDED;
        tv->pszText = (ULONG)(ULONG_PTR)(r.base + REMOTE_HEADER);
        tv->cchTextMax = CELL_TEXT_MAX;
    }
    else
    {
        TVITEMW *tv = (TVITEMW *)header;

        tv->mask = TVIF_HANDLE | TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN;
        tv->hItem = item;
        tv->stateMask = TVIS_SELECTED | TVIS_EXPANDED;
        tv->pszText = (WCHAR *)(r.base + REMOTE_HEADER);
        tv->cchTextMax = CELL_TEXT_MAX;
    }
    ok = remote_text(hwnd, &r, TVM_GETITEMW, 0, header, text) &&
         ReadProcessMemory(r.process, r.base, header, sizeof(header), &done);
    /* The item goes in the RECT, as a handle of the target's size; FALSE if scrolled away. */
    memset(&rect, 0, sizeof(rect));
    if (r.wow64)
    {
        ULONG item32 = (ULONG)(ULONG_PTR)item;

        memcpy(&rect, &item32, sizeof(item32));
    }
    else
        memcpy(&rect, &item, sizeof(item));
    obj->has_extents = FALSE;
    if (ok && remote_rect(hwnd, &r, TVM_GETITEMRECT, TRUE, &rect))
        set_item_extents(hwnd, rect, obj);
    remote_close(&r);
    if (!ok)
        return FALSE;
    if (r.wow64)
    {
        state = ((struct tvitem32 *)header)->state;
        children = ((struct tvitem32 *)header)->cChildren;
    }
    else
    {
        state = ((TVITEMW *)header)->state;
        children = ((TVITEMW *)header)->cChildren;
    }

    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = tree_child_id(item);
    obj->role = BRIDGE_ROLE_TREE_ITEM;
    obj->value = NULL;
    to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSED);
    if (state & TVIS_SELECTED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);
    /* cChildren may be a callback (I_CHILDRENCALLBACK), or 0 with children. */
    if (children || tree_next(hwnd, TVGN_CHILD, item))
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE) |
                       BRIDGE_STATE_BIT((state & TVIS_EXPANDED) ? BRIDGE_STATE_EXPANDED
                                                                : BRIDGE_STATE_COLLAPSED);

    obj->level = 1;
    for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_PARENT, next)); i++)
        obj->level++;
    obj->position = 1;
    for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_PREVIOUS, next)); i++)
        obj->position++;
    obj->set_size = obj->position;
    for (next = item, i = 0; i < TREE_WALK_MAX && (next = tree_next(hwnd, TVGN_NEXT, next)); i++)
        obj->set_size++;
    return TRUE;
}

/*
 * Wine's console window (conhost.exe, WineConsoleClass) emits no WinEvent
 * and draws its text with GDI. Like NVDA, the bridge attaches to the
 * console for a moment, reads the screen buffer and detaches again. The
 * window is the frame; the terminal inside it is child CONSOLE_CHILD.
 */
#define CONSOLE_CLASS L"WineConsoleClass"
#define CONSOLE_CHILD 1
/* Screen buffer cells read at most; earlier rows are left out. */
#define CONSOLE_MAX_CELLS (1 << 18)
/* Output comes without a key, so a focused console is read this often. */
#define CONSOLE_POLL_MS 100

/*
 * AttachConsole needs a process of the console, not conhost.exe itself
 * (error 6); one is found by trying every process and kept here. It also
 * names the application.
 */
static struct
{
    HWND hwnd;
    DWORD pid;
} console_client;

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

static BOOL is_console(HWND hwnd)
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
static BOOL describe_console(HWND hwnd, struct bridge_object *obj, struct focus_strings *str)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    CONSOLE_FONT_INFO font = { 0 };
    COORD origin = { 0, 0 };
    WCHAR *cells = NULL, *buf = NULL;
    HANDLE out;
    DWORD read = 0;
    int width = 0, rows = 0, cursor_row, y, len, pos = 0, end = 0, caret = 0, size;
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
    size = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    str->text = HeapAlloc(GetProcessHeap(), 0, size > 0 ? size : 1);
    if (str->text)
    {
        str->text[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, str->text, size, NULL, NULL);
        obj->text = str->text;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    get_extents(hwnd, obj);
    return TRUE;
}

/*
 * Text extents of the console text from describe_console, through the
 * view saved there. A range outside the window lies TEXT_FAR_AWAY above
 * or below it, as for an Edit. Each UTF-16 unit is taken as one cell.
 */
static int console_text_extents(HWND hwnd, const unsigned int *chars, int count,
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
    BOOL listview, listbox;
    HWND updown;

    get_class(hwnd, cls, ARRAYSIZE(cls), str->object_class, sizeof(str->object_class));
    obj->window_class = str->object_class;
    obj->text = NULL;
    str->text = NULL;
    obj->position = obj->set_size = obj->level = obj->column_count = 0;
    obj->columns = NULL;
    obj->has_range = FALSE;
    obj->has_extents = FALSE;
    if (id_object == OBJID_CLIENT && !lstrcmpW(cls, CONSOLE_CLASS))
        return describe_console(hwnd, obj, str);
    if (id_object == OBJID_CLIENT && is_tree_view(cls))
    {
        /* Only the caret item can have focus; the child id just names it. */
        HTREEITEM item;

        if (tree_focus(hwnd, &item) == LIST_BUSY)
            return FALSE;
        if (item)
            return describe_treeview_item(hwnd, item, obj, str);
    }
    if (id_object == OBJID_CLIENT && is_tab_control(cls))
    {
        int item = id_child > 0 ? id_child - 1 : tab_focus(hwnd);

        if (item == LIST_BUSY)
            return FALSE;
        if (item >= 0)
            return describe_tab_item(hwnd, item, obj, str);
    }
    listview = id_object == OBJID_CLIENT && !lstrcmpiW(cls, L"SysListView32");
    if (listview)
    {
        int item = id_child > 0 ? id_child - 1 : listview_focus(hwnd);

        if (item >= 0)
            return describe_listview_item(hwnd, item, obj, str);
    }
    listbox = id_object == OBJID_CLIENT && is_list_box(cls);
    if (listbox && id_child > 0)
    {
        /*
         * Wine reports focus_item + 1 even for an empty list
         * (listbox.c, WM_SETFOCUS); then the list itself is described.
         */
        int ret = describe_listbox_item(hwnd, id_child - 1, obj, str);

        if (ret == LIST_BUSY)
            return FALSE;
        if (ret)
            return TRUE;
        id_child = CHILDID_SELF;
    }
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
    if (!obj->child && obj->role == BRIDGE_ROLE_ENTRY && (updown = find_updown(hwnd)))
    {
        obj->role = BRIDGE_ROLE_SPIN_BUTTON;
        describe_range(updown, L"msctls_updown32", obj);
    }
    else if (!obj->child && (obj->role == BRIDGE_ROLE_SLIDER ||
                             obj->role == BRIDGE_ROLE_PROGRESS_BAR ||
                             obj->role == BRIDGE_ROLE_SPIN_BUTTON))
        describe_range(hwnd, cls, obj);
    if (!obj->child && has_edit_text(cls) &&
        (obj->role == BRIDGE_ROLE_TEXT || obj->role == BRIDGE_ROLE_ENTRY ||
         obj->role == BRIDGE_ROLE_PASSWORD_TEXT || obj->role == BRIDGE_ROLE_SPIN_BUTTON))
    {
        BOOL rich = lstrcmpiW(cls, L"Edit") != 0;

        /*
         * Wine's oleacc has no proxy for a RichEdit and names it by its
         * window text, the whole document. Orca presents every name change
         * of the focused object, so it would read the text after each key.
         */
        if (rich)
            str->object[0] = 0;
        describe_edit_text(hwnd, rich, obj, str);
    }
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
    if ((listview || listbox) && !obj->child)
        describe_list_box(hwnd, listview, obj, str);
    if (!obj->child)
        get_extents(hwnd, obj);

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
    get_extents(hwnd, obj);
}

/*
 * The status bar of a top-level window. Wine's comctl32 emits no WinEvent
 * when its text changes (status.c, SB_SETTEXTW), so it is read on each
 * focus change and after each key, and reported below the frame when it
 * changed. Orca reads it on its "present status bar" command: role, then
 * the whole text (speech_generator.py, _generate_status_bar).
 */
static struct
{
    HWND hwnd;
    WCHAR text[STATUS_TEXT_MAX];
} last_status;

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
static void report_status_bar(HWND top)
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

/*
 * The window tree of the foreground top-level window, for Orca's object
 * navigation and flat review: every visible child window, described by
 * the same handlers as a focus. It is reported when focus enters another
 * top-level window, before that focus, and again TREE_REFRESH_MS after
 * the last show, hide, move or name change in it.
 */
#define MAX_TREE 512
#define TREE_REFRESH_MS 150

enum tree_kind
{
    TREE_SKIP,      /* neither the window nor its children */
    TREE_LEAF,      /* the window, but not its children */
    TREE_NODE,      /* the window and its children */
};

struct tree_build
{
    struct bridge_object *objects;
    unsigned long long *parents;
    int count;
    struct focus_strings *str;
};

static HWND tree_top;
static UINT_PTR tree_timer;

static enum tree_kind tree_kind(HWND hwnd, const WCHAR *cls)
{
    DWORD_PTR buddy = 0;
    WCHAR buddy_cls[16];
    unsigned int role;

    /* report_status_bar owns the status bar. */
    if (!lstrcmpiW(cls, L"msctls_statusbar32"))
        return TREE_SKIP;
    /* An up-down is part of its buddy's spin button, see find_updown. */
    if (!lstrcmpiW(cls, L"msctls_updown32") &&
        SendMessageTimeoutW(hwnd, UDM_GETBUDDY, 0, 0, SMTO_ABORTIFHUNG, 100, &buddy) &&
        buddy && GetClassNameW((HWND)buddy, buddy_cls, ARRAYSIZE(buddy_cls)) &&
        !lstrcmpiW(buddy_cls, L"Edit"))
        return TREE_SKIP;
    /* The size grip in a window corner, e.g. of the Open dialog. */
    if (!lstrcmpiW(cls, L"ScrollBar") &&
        (GetWindowLongW(hwnd, GWL_STYLE) & (SBS_SIZEGRIP | SBS_SIZEBOX)))
        return TREE_SKIP;
    role = role_from_class(hwnd, cls);
    /*
     * The windows inside a known control are its parts (the Edit and list
     * of a combo box, the header of a list view); a toolbar can hold real
     * controls, e.g. wordpad's font combo boxes.
     */
    if (role != BRIDGE_ROLE_UNKNOWN && role != BRIDGE_ROLE_TOOL_BAR)
        return TREE_LEAF;
    return TREE_NODE;
}

/* Describes child window hwnd of a window tree; FALSE to leave it out. */
static BOOL describe_tree_window(HWND hwnd, const WCHAR *cls, struct bridge_object *obj,
                                 struct focus_strings *str)
{
    memset(obj, 0, sizeof(*obj));
    if (!lstrcmpiW(cls, L"Static"))
        return describe_label(hwnd, str->object, sizeof(str->object), obj);
    if (is_item_control(cls))
    {
        describe_container(hwnd, obj, str);
        return obj->hwnd != 0;
    }
    if (!describe_object(hwnd, OBJID_CLIENT, CHILDID_SELF, obj, str) || obj->child)
        return FALSE;
    /* A child dialog, e.g. a property sheet page, only groups controls. */
    if (obj->role == BRIDGE_ROLE_DIALOG || obj->role == BRIDGE_ROLE_FRAME ||
        obj->role == BRIDGE_ROLE_WINDOW || !lstrcmpW(cls, L"#32770"))
        obj->role = BRIDGE_ROLE_PANEL;
    return TRUE;
}

static char *dup_utf8(const char *s)
{
    char *d;

    if (!s || !(d = HeapAlloc(GetProcessHeap(), 0, lstrlenA(s) + 1)))
        return NULL;
    lstrcpyA(d, s);
    return d;
}

/* Copies the strings of obj, which point into a reused focus_strings. */
static void keep_strings(struct bridge_object *obj)
{
    const char **columns = NULL;
    int *extents = NULL;
    int i;

    if (obj->column_count > 0 && obj->column_extents &&
        (extents = HeapAlloc(GetProcessHeap(), 0, obj->column_count * 4 * sizeof(int))))
        memcpy(extents, obj->column_extents, obj->column_count * 4 * sizeof(int));
    obj->column_extents = extents;
    obj->name = dup_utf8(obj->name);
    obj->value = dup_utf8(obj->value);
    obj->window_class = dup_utf8(obj->window_class);
    obj->shortcut = dup_utf8(obj->shortcut);
    obj->text = dup_utf8(obj->text);
    if (obj->column_count > 0 &&
        (columns = HeapAlloc(GetProcessHeap(), 0, obj->column_count * sizeof(*columns))))
        for (i = 0; i < obj->column_count; i++)
            columns[i] = dup_utf8(obj->columns[i]);
    else
        obj->column_count = 0;
    obj->columns = columns;
}

static void free_kept_strings(struct bridge_object *obj)
{
    HANDLE heap = GetProcessHeap();
    int i;

    HeapFree(heap, 0, (char *)obj->name);
    HeapFree(heap, 0, (char *)obj->value);
    HeapFree(heap, 0, (char *)obj->window_class);
    HeapFree(heap, 0, (char *)obj->shortcut);
    HeapFree(heap, 0, (char *)obj->text);
    for (i = 0; i < obj->column_count; i++)
        HeapFree(heap, 0, (char *)obj->columns[i]);
    HeapFree(heap, 0, (void *)obj->columns);
    HeapFree(heap, 0, (void *)obj->column_extents);
}

/*
 * Keeps the item just described into b->objects[b->count] if it lies in
 * the client area clip; FALSE if it does not.
 */
static BOOL keep_tree_item(struct tree_build *b, HWND list, const int *clip)
{
    struct bridge_object *obj = &b->objects[b->count];
    BOOL shown = obj->has_extents && obj->width > 0 && obj->height > 0 &&
                 obj->x < clip[0] + clip[2] && obj->x + obj->width > clip[0] &&
                 obj->y < clip[1] + clip[3] && obj->y + obj->height > clip[1];

    if (shown)
    {
        keep_strings(obj);
        b->parents[b->count++] = (ULONG_PTR)list;
    }
    free_strings(b->str);
    return shown;
}

/*
 * The items a list, list view, tree view or tab control shows, after the
 * control itself, so that flat review sees them. Only the items in view:
 * a list may hold thousands.
 */
static void add_tree_items(struct tree_build *b, HWND hwnd, const WCHAR *cls)
{
    DWORD_PTR top = 0, count = 0, page = 0;
    RECT client;
    int clip[4], i, end;

    if (!GetClientRect(hwnd, &client))
        return;
    client_to_screen(hwnd, client, clip);
    if (is_tab_control(cls))
    {
        if (!SendMessageTimeoutW(hwnd, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
            return;
        for (i = 0; i < (int)count && b->count < MAX_TREE; i++)
        {
            memset(&b->objects[b->count], 0, sizeof(b->objects[0]));
            if (describe_tab_item(hwnd, i, &b->objects[b->count], b->str))
                keep_tree_item(b, hwnd, clip);
            else
                free_strings(b->str);
        }
    }
    else if (is_tree_view(cls))
    {
        HTREEITEM item = tree_next(hwnd, TVGN_FIRSTVISIBLE, NULL);

        for (; item && b->count < MAX_TREE; item = tree_next(hwnd, TVGN_NEXTVISIBLE, item))
        {
            memset(&b->objects[b->count], 0, sizeof(b->objects[0]));
            if (!describe_treeview_item(hwnd, item, &b->objects[b->count], b->str) ||
                !keep_tree_item(b, hwnd, clip))
            {
                free_strings(b->str);
                break;
            }
        }
    }
    else if (is_list_box(cls))
    {
        if (!SendMessageTimeoutW(hwnd, LB_GETTOPINDEX, 0, 0, SMTO_ABORTIFHUNG, 500, &top) ||
            !SendMessageTimeoutW(hwnd, LB_GETCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
            return;
        for (i = (int)top; i < (int)count && b->count < MAX_TREE; i++)
        {
            memset(&b->objects[b->count], 0, sizeof(b->objects[0]));
            if (describe_listbox_item(hwnd, i, &b->objects[b->count], b->str) != TRUE ||
                !keep_tree_item(b, hwnd, clip))
            {
                free_strings(b->str);
                break;
            }
        }
    }
    else
    {
        /*
         * In icon views the top index is 0 and a page holds every item, so
         * each item is checked against the client area.
         */
        if (!SendMessageTimeoutW(hwnd, LVM_GETTOPINDEX, 0, 0, SMTO_ABORTIFHUNG, 500, &top) ||
            !SendMessageTimeoutW(hwnd, LVM_GETCOUNTPERPAGE, 0, 0, SMTO_ABORTIFHUNG, 500, &page) ||
            !SendMessageTimeoutW(hwnd, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
            return;
        /* One more for a row cut at the bottom. */
        end = (int)min(count, top + page + 1);
        for (i = (int)top; i < end && b->count < MAX_TREE; i++)
        {
            memset(&b->objects[b->count], 0, sizeof(b->objects[0]));
            if (describe_listview_item(hwnd, i, &b->objects[b->count], b->str))
                keep_tree_item(b, hwnd, clip);
            else
                free_strings(b->str);
        }
    }
}

static void add_tree_children(struct tree_build *b, HWND parent)
{
    WCHAR cls[256];
    HWND hwnd;

    for (hwnd = GetWindow(parent, GW_CHILD); hwnd && b->count < MAX_TREE;
         hwnd = GetWindow(hwnd, GW_HWNDNEXT))
    {
        struct bridge_object *obj = &b->objects[b->count];
        enum tree_kind kind;

        if (!IsWindowVisible(hwnd) || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
            (kind = tree_kind(hwnd, cls)) == TREE_SKIP)
            continue;
        /* A window that did not answer is left out with its children. */
        if (!describe_tree_window(hwnd, cls, obj, b->str))
        {
            free_strings(b->str);
            continue;
        }
        keep_strings(obj);
        free_strings(b->str);
        b->parents[b->count++] = (ULONG_PTR)parent;
        if (kind == TREE_NODE)
            add_tree_children(b, hwnd);
        else if (is_item_control(cls))
            add_tree_items(b, hwnd, cls);
    }
}

static void report_tree(HWND top)
{
    HANDLE heap = GetProcessHeap();
    struct bridge_tree tree;
    struct tree_build b;
    DWORD start = GetTickCount(), pid = 0;
    int i;

    if (!top || is_console(top))
        return;
    b.objects = HeapAlloc(heap, HEAP_ZERO_MEMORY, MAX_TREE * sizeof(*b.objects));
    b.parents = HeapAlloc(heap, 0, MAX_TREE * sizeof(*b.parents));
    b.str = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*b.str));
    b.count = 0;
    if (b.objects && b.parents && b.str)
    {
        add_tree_children(&b, top);
        memset(&tree, 0, sizeof(tree));
        describe_frame(top, &tree.frame, b.str);
        GetWindowThreadProcessId(top, &pid);
        tree.pid = pid;
        get_app_name(pid, b.str->app, sizeof(b.str->app));
        tree.app_name = b.str->app;
        tree.count = b.count;
        tree.objects = b.objects;
        tree.parents = b.parents;
        atspi_post_tree(&tree);
        trace(L"tree: %p %d objects in %lu ms", top, b.count, GetTickCount() - start);
        for (i = 0; i < b.count; i++)
            free_kept_strings(&b.objects[i]);
    }
    HeapFree(heap, 0, b.str);
    HeapFree(heap, 0, b.parents);
    HeapFree(heap, 0, b.objects);
}

static void CALLBACK refresh_tree(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, id);
    tree_timer = 0;
    if (tree_top && IsWindow(tree_top))
        report_tree(tree_top);
}

/* Something changed in window hwnd: refresh the tree if hwnd is in it. */
static void schedule_tree(HWND hwnd)
{
    if (!tree_top || (hwnd != tree_top && !IsChild(tree_top, hwnd)))
        return;
    tree_timer = SetTimer(NULL, tree_timer, TREE_REFRESH_MS, refresh_tree);
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
    /* Before the focus, so that the focused object gets its tree parent. */
    if (top != tree_top)
    {
        tree_top = top;
        report_tree(top);
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
 * Wine's ListBox moves its caret on arrow keys without a focus event
 * (listbox.c, LISTBOX_HandleKeyDown), and SysListView32 and SysTreeView32
 * emit no focus events at all; SysTabControl32 emits only a selection
 * event, which is not hooked. Reports the new item as focused and returns TRUE.
 * FALSE if hwnd is no list or the caret did not move, LIST_BUSY if the
 * list did not answer.
 */
static int check_list_caret(HWND hwnd)
{
    WCHAR cls[256];
    DWORD_PTR caret = LB_ERR;

    if (last_focus.id_object != OBJID_CLIENT || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls)))
        return FALSE;
    if (is_tree_view(cls))
    {
        HTREEITEM item;

        if (tree_focus(hwnd, &item) == LIST_BUSY)
            return LIST_BUSY;
        if (!item || tree_child_id(item) == last_focus.id_child)
            return FALSE;
        on_focus(hwnd, OBJID_CLIENT, tree_child_id(item));
        return TRUE;
    }
    if (!lstrcmpiW(cls, L"SysListView32") || is_tab_control(cls))
    {
        caret = is_tab_control(cls) ? tab_focus(hwnd) : listview_focus(hwnd);
        if ((LONG)caret == LIST_BUSY)
            return LIST_BUSY;
    }
    else if (!is_list_box(cls))
        return FALSE;
    else if (!SendMessageTimeoutW(hwnd, LB_GETCARETINDEX, 0, 0, SMTO_ABORTIFHUNG, 500, &caret))
        return LIST_BUSY;
    if ((LONG)caret < 0 || (LONG)caret + 1 == last_focus.id_child)
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
static UINT_PTR focus_timer;   /* a focus event waits, see FOCUS_SETTLE_MS */

/* Menu mode of a target, see report_menu_item. */
static struct
{
    BOOL active;            /* a menu item was reported, no menu end yet */
    DWORD thread;           /* thread in menu mode */
    struct menu_record pending;
    BOOL has_pending;
    UINT_PTR timer;
} menu;

static BOOL check_missed_focus(void)
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

/*
 * Menus. The hook DLL (menuhook.c) reports each highlighted item; menu
 * focus lives only on the AT-SPI side, last_focus keeps the control that
 * gets focus back when the menu ends. Records wait MENU_SETTLE_MS, so that
 * only the last of a burst is reported: Right on an open menu gives the
 * next bar item and then the first item of its popup.
 */
#define MENU_SETTLE_MS 30

/*
 * Child ids of menu objects, in the owner's top-level window: high bit,
 * kind, then 7 bits per level for the position + 1 from the root down.
 * Path depth 0 is the root menu (menu bar, system menu or context menu).
 */
#define MENU_CHILD_BASE 0x80000000u

static unsigned int menu_child_id(const struct menu_record *rec, UINT32 depth)
{
    unsigned int id = MENU_CHILD_BASE | (rec->kind & 3) << 28;
    UINT32 i;

    for (i = 0; i < depth && i < MENU_MAX_DEPTH; i++)
        id |= (rec->path[i] < 126 ? rec->path[i] + 1 : 127) << (7 * i);
    return id;
}

/* Splits "&Open...\tCtrl+O" into name "Open..." and shortcut "Ctrl+O". */
static void menu_text(const WCHAR *raw, char *name, int name_size,
                      char *shortcut, int shortcut_size)
{
    WCHAR buf[MENU_TEXT_MAX], *tab;

    lstrcpynW(buf, raw, ARRAYSIZE(buf));
    for (tab = buf; *tab && *tab != '\t'; tab++)
        ;
    if (*tab)
        *tab++ = 0;
    else
        tab = NULL;
    strip_ampersands(buf);
    to_utf8(buf, name, name_size);
    if (shortcut)
        to_utf8(tab, shortcut, shortcut_size);
}

static void report_menu_item(const struct menu_record *rec)
{
    struct bridge_focus focus;
    struct focus_strings str;
    /* chain[0] the root menu, chain[k] the item at level k - 1. */
    struct bridge_object chain[MENU_MAX_DEPTH + 1], *obj;
    char names[MENU_MAX_DEPTH][MENU_TEXT_MAX * 3], shortcut[MENU_TEXT_MAX * 3];
    HWND owner = (HWND)(ULONG_PTR)rec->owner, top = GetAncestor(owner, GA_ROOT);
    UINT32 depth = min(rec->depth, MENU_MAX_DEPTH), k;
    DWORD pid = 0, thread;
    unsigned long long shown = BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                               BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING);
    unsigned long long enabled = BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                                 BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE);
    unsigned long long item = shown | BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE) |
                              BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTABLE) |
                              BRIDGE_STATE_BIT(BRIDGE_STATE_SELECTED);

    if (!top || !(thread = GetWindowThreadProcessId(top, &pid)))
        return;
    menu.active = TRUE;
    menu.thread = thread;
    memset(&focus, 0, sizeof(focus));
    memset(&str, 0, sizeof(str));
    memset(chain, 0, sizeof(chain));

    /*
     * The menu bar or root popup, then the items that opened the menus
     * down to the item, expanded now. Orca stops the ancestors at a menu
     * bar, and at a menu named like the expanded item that had focus
     * before (speech_generator.py), so going down from "File" says only
     * "Open". The system menu is unnamed: Wine gives it no text.
     */
    for (k = 0; k <= depth; k++)
    {
        chain[k].hwnd = (ULONG_PTR)top;
        chain[k].child = menu_child_id(rec, k);
        chain[k].states = shown | enabled;
        chain[k].name = "";
        if (!k)
        {
            chain[k].role = rec->kind == MENU_KIND_BAR ? BRIDGE_ROLE_MENU_BAR : BRIDGE_ROLE_MENU;
            continue;
        }
        menu_text(rec->texts[k - 1], names[k - 1], sizeof(names[k - 1]),
                  shortcut, sizeof(shortcut));
        chain[k].name = names[k - 1];
        chain[k].role = BRIDGE_ROLE_MENU;
        chain[k].states = item | enabled | BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE) |
                          BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDED);
    }

    obj = &focus.object;
    *obj = chain[depth];
    if (depth)
    {
        focus.container = chain[depth - 1];
        focus.ancestors = chain;
        focus.ancestor_count = depth - 1;
    }
    obj->shortcut = depth && shortcut[0] ? shortcut : NULL;
    if (rec->has_submenu)
        obj->role = BRIDGE_ROLE_MENU;
    else if (rec->type & MFT_RADIOCHECK)
        obj->role = BRIDGE_ROLE_RADIO_MENU_ITEM;
    else if (rec->state & MFS_CHECKED)
        obj->role = BRIDGE_ROLE_CHECK_MENU_ITEM;
    else
        obj->role = BRIDGE_ROLE_MENU_ITEM;
    obj->states = item;
    if (!(rec->state & MFS_GRAYED))
        obj->states |= enabled;
    if (rec->state & MFS_CHECKED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_CHECKED);
    if (rec->has_submenu)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_EXPANDABLE);
    obj->position = rec->position;
    obj->set_size = rec->count;

    describe_frame(top, &focus.frame, &str);
    describe_labels(top, &focus, &str);
    focus.pid = pid;
    get_app_name(pid, str.app, sizeof(str.app));
    focus.app_name = str.app;
    watch_process(pid);
    atspi_post_focus(&focus);
    trace(L"menu: %p %x name \"%S\" depth %u %d/%d", top, obj->child, obj->name, depth,
          obj->position, obj->set_size);
    free_strings(&str);
}

/* The menu ended: the control that had focus before gets it back. */
static void end_menu(void)
{
    GUITHREADINFO gti;
    HWND hwnd = last_focus.hwnd;

    if (!menu.active)
        return;
    menu.active = FALSE;
    trace(L"menu end");
    /* A command may have opened a dialog; its focus event will tell. */
    if (focus_timer)
        return;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (hwnd && IsWindow(hwnd) &&
        GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, NULL), &gti) && gti.hwndFocus == hwnd)
        on_focus(hwnd, last_focus.id_object, last_focus.id_child);
    else
        check_missed_focus();
}

static void CALLBACK report_pending_menu(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
    KillTimer(NULL, id);
    menu.timer = 0;
    if (!menu.has_pending)
        return;
    menu.has_pending = FALSE;
    if (menu.pending.event == MENU_EVENT_END)
        end_menu();
    else
        report_menu_item(&menu.pending);
}

/*
 * A menu end can get lost, e.g. when the target dies in its menu loop.
 * TRUE while the menu thread is still in menu mode.
 */
static BOOL still_in_menu(void)
{
    GUITHREADINFO gti;

    if (!menu.active)
        return FALSE;
    memset(&gti, 0, sizeof(gti));
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(menu.thread, &gti) && (gti.flags & GUI_INMENUMODE))
        return TRUE;
    if (!menu.timer)
        end_menu();
    return FALSE;
}

/* Window procedure of the hidden window that gets the hook's records. */
static LRESULT CALLBACK menu_sink_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lparam;

    if (msg != WM_COPYDATA)
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    if (cds->dwData != MENU_RECORD_TAG || cds->cbData != sizeof(struct menu_record))
        return FALSE;
    /* The target's menu loop waits for the answer: describe it later. */
    memcpy(&menu.pending, cds->lpData, sizeof(menu.pending));
    menu.has_pending = TRUE;
    menu.timer = SetTimer(NULL, menu.timer, MENU_SETTLE_MS, report_pending_menu);
    return TRUE;
}

/*
 * Creates the record sink and installs the hook DLL that lies next to the
 * executable. Without it the bridge runs on, just without menus.
 */
static HHOOK install_menu_hook(void)
{
    WNDCLASSW wc;
    WCHAR path[MAX_PATH], *base = path, *p;
    HMODULE module;
    HOOKPROC proc;
    HHOOK hook;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = menu_sink_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = MENU_SINK_CLASS;
    if (!RegisterClassW(&wc) ||
        !CreateWindowExW(0, MENU_SINK_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
                         wc.hInstance, NULL))
        return NULL;
    if (!GetModuleFileNameW(NULL, path, ARRAYSIZE(path) - 32))
        return NULL;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    lstrcpyW(base, L"wine-atspi-hook.dll");
    if (!(module = LoadLibraryW(path)) ||
        !(proc = (HOOKPROC)GetProcAddress(module, "CallWndProc")))
    {
        trace(L"no menu hook: cannot load %s", path);
        return NULL;
    }
    hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, module, 0);
    trace(L"menu hook %s: %p", path, hook);
    return hook;
}

/*
 * Wine emits no WinEvent when a key changes a combo box, a list box
 * selection or a user32 Edit. Like NVDA, look at the focused object again
 * shortly after each key, once the target has handled it.
 */
#define KEY_CHECK_DELAY_MS 50
/* A busy target is asked again, up to this many times. */
#define KEY_CHECK_RETRIES 5

static UINT_PTR key_timer;
static int key_retries;

static void CALLBACK check_after_key(HWND unused, UINT msg, UINT_PTR id, DWORD time)
{
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
     * the items in the window tree are read again.
     */
    if (list != LIST_BUSY && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && is_item_control(cls))
        schedule_tree(hwnd);
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
            key_retries = 0;
            key_timer = SetTimer(NULL, key_timer, KEY_CHECK_DELAY_MS, check_after_key);
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
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

static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
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

static DWORD WINAPI atspi_thread(void *arg)
{
    /*
     * Created with CreateThread, so the thread has a TEB and may call
     * Win32 later; the AT-SPI side itself only uses GLib.
     */
    atspi_run();
    return 0;
}

/*
 * Turns the bridge into a Wine system process, like services.exe. Its
 * threads then do not count as desktop users, so it does not keep the
 * session alive; the returned event is signaled when the session shuts
 * down. Returns NULL if Wine refuses.
 */
static HANDLE make_system_process(void)
{
    HANDLE event = NULL;
    NTSTATUS status;

    status = NtSetInformationProcess(GetCurrentProcess(), PROCESS_WINE_MAKE_PROCESS_SYSTEM,
                                     &event, sizeof(HANDLE *));
    trace(L"make system process: status %lx event %p", (long)status, event);
    return status ? NULL : event;
}

/* Runs the bridge until the session shuts down or WM_QUIT arrives. */
static int run_bridge(BOOL service)
{
    HWINEVENTHOOK hook, foreground_hook, value_hook;
    HHOOK keyboard, menu_hook;
    HANDLE thread, shutdown_event = NULL;
    DWORD wait;
    MSG msg;

    /* Never closed: Wine drops it when the process ends. */
    CreateMutexW(NULL, FALSE, BRIDGE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        trace(L"bridge already running in this prefix");
        return 0;
    }
    if (service && !(shutdown_event = make_system_process()))
        return 1;
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
    /* LOCATIONCHANGE .. VALUECHANGE includes NAMECHANGE, for the window tree. */
    value_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_VALUECHANGE, NULL,
                                 win_event_proc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!value_hook)
        return 1;
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(NULL), 0);
    if (!keyboard)
        return 1;
    menu_hook = install_menu_hook();

    for (;;)
    {
        /* Also serves the sent messages that deliver the hook callbacks. */
        wait = MsgWaitForMultipleObjects(shutdown_event ? 1 : 0, &shutdown_event, FALSE,
                                         INFINITE, QS_ALLINPUT);
        if (shutdown_event && wait == WAIT_OBJECT_0)
        {
            trace(L"session shuts down");
            break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                goto done;
            if (!msg.hwnd && msg.message == WM_PROCESS_EXITED)
            {
                on_process_exited((DWORD)msg.wParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
done:
    if (menu_hook)
        UnhookWindowsHookEx(menu_hook);
    UnhookWindowsHookEx(keyboard);
    UnhookWinEvent(value_hook);
    UnhookWinEvent(foreground_hook);
    UnhookWinEvent(hook);
    CoUninitialize();
    return 0;
}

/*
 * Registers the bridge in RunServices, so every session of this prefix
 * starts it, whatever program starts the session. Also starts it now
 * unless one runs already.
 */
static int install(const WCHAR *log_path)
{
    WCHAR exe[MAX_PATH], log[MAX_PATH], cmd[1024];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE mutex;
    HKEY key;
    LONG err;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return 1;
    if (log_path)
    {
        if (!GetFullPathNameW(log_path, MAX_PATH, log, NULL))
            return 1;
        wsprintfW(cmd, L"\"%s\" --service --log \"%s\"", exe, log);
    }
    else
        wsprintfW(cmd, L"\"%s\" --service", exe);

    err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, RUN_SERVICES_KEY, 0, NULL, 0, KEY_SET_VALUE,
                          NULL, &key, NULL);
    if (!err)
    {
        err = RegSetValueExW(key, RUN_SERVICES_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                             (lstrlenW(cmd) + 1) * sizeof(WCHAR));
        RegCloseKey(key);
    }
    if (err)
    {
        print_line(L"cannot write the RunServices value");
        return 1;
    }
    print_line(L"installed in HKLM\\" RUN_SERVICES_KEY L":");
    print_line(cmd);

    if ((mutex = OpenMutexW(SYNCHRONIZE, FALSE, BRIDGE_MUTEX)))
    {
        CloseHandle(mutex);
        print_line(L"bridge already running");
        return 0;
    }
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi))
    {
        print_line(L"cannot start the bridge");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    print_line(L"bridge started");
    return 0;
}

/* Removes the RunServices value; a running bridge stays until the session ends. */
static int uninstall(void)
{
    HKEY key;
    LONG err;

    err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, RUN_SERVICES_KEY, 0, KEY_SET_VALUE, &key);
    if (!err)
    {
        err = RegDeleteValueW(key, RUN_SERVICES_VALUE);
        RegCloseKey(key);
    }
    if (err == ERROR_FILE_NOT_FOUND)
    {
        print_line(L"not installed");
        return 0;
    }
    if (err)
    {
        print_line(L"cannot remove the RunServices value");
        return 1;
    }
    print_line(L"uninstalled; a running bridge stays until the session ends");
    return 0;
}

static int usage(void)
{
    print_line(L"usage: wine-atspi [--service] [--log FILE]   run the bridge");
    print_line(L"       wine-atspi install [--log FILE]       start it with every session");
    print_line(L"       wine-atspi uninstall");
    return 2;
}

/*
 * main, not wmain: in a Winelib ELF wmain gets a broken argv (both msvcrt
 * and ucrtbase are loaded), so the command line is parsed here.
 */
int main(void)
{
    const WCHAR *command = NULL, *log_path = NULL;
    BOOL service = FALSE;
    WCHAR **argv;
    int argc, i;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    for (i = 1; i < argc; i++)
    {
        if (!lstrcmpW(argv[i], L"--service"))
            service = TRUE;
        else if (!lstrcmpW(argv[i], L"--log") && i + 1 < argc)
            log_path = argv[++i];
        else if (!command && (!lstrcmpW(argv[i], L"install") || !lstrcmpW(argv[i], L"uninstall")))
            command = argv[i];
        else
            return usage();
    }
    if (command && !lstrcmpW(command, L"install"))
        return service ? usage() : install(log_path);
    if (command)
        return service || log_path ? usage() : uninstall();

    trace_handle = GetStdHandle(STD_ERROR_HANDLE);
    debug = GetEnvironmentVariableW(L"WINE_ATSPI_DEBUG", NULL, 0) > 0;
    /*
     * Started from RunServices the bridge has no Win32 std handles and
     * does not see the environment of the first wine command. Appended,
     * because even a short `wine reg ...` starts a session with a bridge.
     */
    if (log_path)
    {
        trace_handle = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (trace_handle == INVALID_HANDLE_VALUE)
            return 1;
        debug = TRUE;
    }
    trace(L"bridge %lu starts%s", GetCurrentProcessId(), service ? L" as a service" : L"");
    return run_bridge(service);
}
