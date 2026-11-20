/*
 * controls - a window with standard controls for the bridge tests.
 *
 * Wine's own programs do not cover every control the bridge handles, so
 * this window holds labelled ones in a fixed order; Tab moves through
 * them. Pure Win32: builds with winegcc and with mingw.
 *
 * Controls, in Tab order:
 *   "Fruits:"  ListBox, single selection: Apple, Banana, Cherry
 *   "Empty:"   ListBox without items
 *   "Colors:"  ListBox, extended selection: Red, Green, Blue
 *   "Files:"   SysListView32 in report view, columns Name, Size, Type,
 *              rows readme.txt / notes.md / src / tests / docs / LICENSE /
 *              Makefile / todo.txt, more than it shows
 *   "Places:"  SysTreeView32 with check boxes, collapsed: Home
 *              (Documents, Music checked), Work (Reports), Trash
 *   "Sections:" SysTabControl32: General, Advanced, About
 *   "Volume:"  msctls_trackbar32, 0 to 100 at 50, page size 10
 *   "Level:"   msctls_progress32, 0 to 100, follows "Volume:"; no tab stop
 *   "Count:"   Edit with a msctls_updown32 buddy, 1 to 10 at 3
 *   "Run"      Button: fills "Level:" from 0 to 100 in 20 s
 *   "Close"    Button
 *   Static after "Close", no tab stop: "Idle", then "Step n of 20"
 *              while "Run" fills the bar, "Done" at the end; it labels
 *              no control, like the text of a progress dialog
 *   ToolbarWindow32 at the bottom, flat, with the buttons New, a
 *              separator, Bold (check), Wrap (check, checked), Undo
 *              (disabled, text from the string list) and Help, without
 *              text, named only by its tooltip (TTN_GETDISPINFOW); arrow
 *              keys move its hot button. Space on Bold sends its command
 *              (without checking it), which enables Undo, as an editor's
 *              first change would. A Static "Zoom 100 %" lies on
 *              the toolbar, a window of its own like wordpad's font combo
 *              boxes
 *   msctls_statusbar32 below it: "Ready", then "Level n %" while "Run"
 *              fills the bar (SB_SETTEXTW, no WinEvent)
 */

#include <windows.h>
#include <commctrl.h>

#define CLASS_NAME L"WineAtspiControls"

enum { ID_FRUITS = 100, ID_EMPTY, ID_COLORS, ID_FILES, ID_PLACES, ID_SECTIONS, ID_VOLUME,
       ID_LEVEL, ID_COUNT, ID_COUNT_SPIN, ID_RUN, ID_CLOSE, ID_STEP, ID_TOOLS, ID_NEW, ID_BOLD,
       ID_WRAP, ID_UNDO, ID_HELP, ID_STATUS, ID_ZOOM };

/* "Run" fills "Level:" from 0 to 100 in 20 s, as a long task would. */
#define RUN_STEP 5
#define RUN_INTERVAL_MS 1000

static HWND add_control(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style,
                        int x, int y, int w, int h, int id)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                           parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void add_list(HWND parent, const WCHAR *label, DWORD style, int y, int id,
                     const WCHAR *const *items)
{
    HWND list;

    add_control(parent, L"Static", label, SS_LEFT, 10, y, 100, 20, 0);
    list = add_control(parent, L"ListBox", NULL,
                       WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | style,
                       120, y, 200, 70, id);
    for (; items && *items; items++)
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)*items);
}

static void add_files(HWND parent, int y)
{
    static const WCHAR *const columns[] = { L"Name", L"Size", L"Type" };
    static const WCHAR *const rows[][3] = {
        { L"readme.txt", L"2 KB", L"Text" },
        { L"notes.md", L"5 KB", L"Markdown" },
        { L"src", L"", L"Folder" },
        { L"tests", L"", L"Folder" },
        { L"docs", L"", L"Folder" },
        { L"LICENSE", L"1 KB", L"Text" },
        { L"Makefile", L"3 KB", L"Makefile" },
        { L"todo.txt", L"1 KB", L"Text" },
    };
    LVCOLUMNW col = { 0 };
    LVITEMW item = { 0 };
    HWND list;
    int i, j;

    add_control(parent, L"Static", L"F&iles:", SS_LEFT, 10, y, 100, 20, 0);
    list = add_control(parent, WC_LISTVIEWW, NULL,
                       WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
                       120, y, 300, 100, ID_FILES);
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 90;
    for (i = 0; i < ARRAYSIZE(columns); i++)
    {
        col.pszText = (WCHAR *)columns[i];
        col.iSubItem = i;
        SendMessageW(list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
    item.mask = LVIF_TEXT;
    for (i = 0; i < ARRAYSIZE(rows); i++)
    {
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = (WCHAR *)rows[i][0];
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        for (j = 1; j < ARRAYSIZE(columns); j++)
        {
            item.iSubItem = j;
            item.pszText = (WCHAR *)rows[i][j];
            SendMessageW(list, LVM_SETITEMTEXTW, i, (LPARAM)&item);
        }
    }
}

static HTREEITEM add_place(HWND tree, HTREEITEM parent, const WCHAR *text)
{
    TVINSERTSTRUCTW ins = { 0 };

    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT;
    ins.item.pszText = (WCHAR *)text;
    return (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
}

static void add_places(HWND parent, int y)
{
    TVITEMW check = { 0 };
    HWND tree;
    HTREEITEM item;

    add_control(parent, L"Static", L"&Places:", SS_LEFT, 10, y, 100, 20, 0);
    tree = add_control(parent, WC_TREEVIEWW, NULL,
                       WS_TABSTOP | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES |
                       TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                       120, y, 300, 100, ID_PLACES);
    /*
     * As documented for Windows, TVS_CHECKBOXES is set after creation and
     * before the items: Wine resets every state image to 1 when it makes
     * the check box images, on the first paint for the style given at
     * creation (comctl32/treeview.c, TREEVIEW_Paint).
     */
    SetWindowLongW(tree, GWL_STYLE, GetWindowLongW(tree, GWL_STYLE) | TVS_CHECKBOXES);
    item = add_place(tree, TVI_ROOT, L"Home");
    add_place(tree, item, L"Documents");
    /* State image 2 is the checked box; new items get 1, unchecked. */
    check.mask = TVIF_HANDLE | TVIF_STATE;
    check.hItem = add_place(tree, item, L"Music");
    check.state = INDEXTOSTATEIMAGEMASK(2);
    check.stateMask = TVIS_STATEIMAGEMASK;
    SendMessageW(tree, TVM_SETITEMW, 0, (LPARAM)&check);
    item = add_place(tree, TVI_ROOT, L"Work");
    add_place(tree, item, L"Reports");
    add_place(tree, TVI_ROOT, L"Trash");
}

static void add_sections(HWND parent, int y)
{
    static const WCHAR *const sections[] = { L"General", L"Advanced", L"About" };
    TCITEMW item = { 0 };
    HWND tabs;
    int i;

    add_control(parent, L"Static", L"&Sections:", SS_LEFT, 10, y, 100, 20, 0);
    tabs = add_control(parent, WC_TABCONTROLW, NULL, WS_TABSTOP, 120, y, 300, 60, ID_SECTIONS);
    item.mask = TCIF_TEXT;
    for (i = 0; i < ARRAYSIZE(sections); i++)
    {
        item.pszText = (WCHAR *)sections[i];
        SendMessageW(tabs, TCM_INSERTITEMW, i, (LPARAM)&item);
    }
}

static void add_volume(HWND parent, int y)
{
    HWND bar;

    add_control(parent, L"Static", L"&Volume:", SS_LEFT, 10, y, 100, 20, 0);
    bar = add_control(parent, TRACKBAR_CLASSW, NULL, WS_TABSTOP | TBS_HORZ, 120, y, 300, 30,
                      ID_VOLUME);
    SendMessageW(bar, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
    SendMessageW(bar, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(bar, TBM_SETPOS, TRUE, 50);
    add_control(parent, L"Static", L"Level:", SS_LEFT, 10, y + 40, 100, 20, 0);
    bar = add_control(parent, PROGRESS_CLASSW, NULL, 0, 120, y + 40, 300, 20, ID_LEVEL);
    SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    SendMessageW(bar, PBM_SETPOS, 50, 0);
}

static void add_count(HWND parent, int y)
{
    HWND updown;

    add_control(parent, L"Static", L"C&ount:", SS_LEFT, 10, y, 100, 20, 0);
    add_control(parent, L"Edit", NULL, WS_TABSTOP | WS_BORDER | ES_NUMBER, 120, y, 80, 22,
                ID_COUNT);
    /* UDS_AUTOBUDDY takes the Edit created just before as its buddy. */
    updown = add_control(parent, UPDOWN_CLASSW, NULL,
                         UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                         0, 0, 0, 0, ID_COUNT_SPIN);
    SendMessageW(updown, UDM_SETRANGE32, 1, 10);
    SendMessageW(updown, UDM_SETPOS32, 0, 3);
}

static WNDPROC toolbar_proc;

/*
 * A toolbar answers WM_GETDLGCODE with 0, so IsDialogMessage would take
 * its arrow keys; it moves its hot button with them (TOOLBAR_KeyDown).
 */
static LRESULT CALLBACK tools_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTARROWS;
    return CallWindowProcW(toolbar_proc, hwnd, msg, wparam, lparam);
}

static void add_tools(HWND parent, int y)
{
    TBBUTTON buttons[] = {
        { I_IMAGENONE, ID_NEW, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE, { 0 }, 0,
          (INT_PTR)L"New" },
        { 0, 0, 0, BTNS_SEP, { 0 }, 0, -1 },
        { I_IMAGENONE, ID_BOLD, TBSTATE_ENABLED, BTNS_CHECK | BTNS_AUTOSIZE, { 0 }, 0,
          (INT_PTR)L"Bold" },
        { I_IMAGENONE, ID_WRAP, TBSTATE_ENABLED | TBSTATE_CHECKED, BTNS_CHECK | BTNS_AUTOSIZE,
          { 0 }, 0, (INT_PTR)L"Wrap" },
        /* iString set below, an index into the string list. */
        { I_IMAGENONE, ID_UNDO, 0, BTNS_BUTTON | BTNS_AUTOSIZE, { 0 }, 0, 0 },
        { I_IMAGENONE, ID_HELP, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, -1 },
    };
    HWND tools;

    tools = add_control(parent, TOOLBARCLASSNAMEW, NULL,
                        WS_TABSTOP | CCS_NOPARENTALIGN | CCS_NORESIZE | CCS_NODIVIDER |
                        TBSTYLE_LIST | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS,
                        10, y, 420, 30, ID_TOOLS);
    toolbar_proc = (WNDPROC)SetWindowLongPtrW(tools, GWLP_WNDPROC, (LONG_PTR)tools_proc);
    SendMessageW(tools, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    /* The list ends with two NULs. */
    buttons[4].iString = SendMessageW(tools, TB_ADDSTRINGW, 0, (LPARAM)L"Undo\0");
    SendMessageW(tools, TB_ADDBUTTONSW, ARRAYSIZE(buttons), (LPARAM)buttons);
    add_control(tools, L"Static", L"Zoom 100 %", SS_LEFT, 330, 6, 80, 18, ID_ZOOM);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static const WCHAR *const fruits[] = { L"Apple", L"Banana", L"Cherry", NULL };
    static const WCHAR *const colors[] = { L"Red", L"Green", L"Blue", NULL };

    switch (msg)
    {
    case WM_CREATE:
        add_list(hwnd, L"&Fruits:", 0, 10, ID_FRUITS, fruits);
        add_list(hwnd, L"&Empty:", 0, 90, ID_EMPTY, NULL);
        add_list(hwnd, L"&Colors:", LBS_EXTENDEDSEL, 170, ID_COLORS, colors);
        add_files(hwnd, 250);
        add_places(hwnd, 360);
        add_sections(hwnd, 470);
        add_volume(hwnd, 540);
        add_count(hwnd, 620);
        add_control(hwnd, L"Button", L"Run", WS_TABSTOP | BS_PUSHBUTTON,
                    120, 660, 100, 25, ID_RUN);
        add_control(hwnd, L"Button", L"Close", WS_TABSTOP | BS_PUSHBUTTON,
                    240, 660, 100, 25, ID_CLOSE);
        add_control(hwnd, L"Static", L"Idle", SS_LEFT, 360, 663, 120, 20, ID_STEP);
        add_tools(hwnd, 700);
        add_control(hwnd, STATUSCLASSNAMEW, L"Ready", 0, 0, 0, 0, 0, ID_STATUS);
        return 0;
    case WM_TIMER:
    {
        HWND bar = GetDlgItem(hwnd, ID_LEVEL);
        int pos = (int)SendMessageW(bar, PBM_GETPOS, 0, 0) + RUN_STEP;
        WCHAR step[32];

        SendMessageW(bar, PBM_SETPOS, pos, 0);
        wsprintfW(step, L"Level %d %%", pos);
        SendDlgItemMessageW(hwnd, ID_STATUS, SB_SETTEXTW, 0, (LPARAM)step);
        wsprintfW(step, L"Step %d of %d", pos / RUN_STEP, 100 / RUN_STEP);
        if (pos >= 100)
        {
            KillTimer(hwnd, ID_RUN);
            lstrcpyW(step, L"Done");
        }
        SetDlgItemTextW(hwnd, ID_STEP, step);
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lparam == GetDlgItem(hwnd, ID_VOLUME))
            SendDlgItemMessageW(hwnd, ID_LEVEL, PBM_SETPOS,
                                SendMessageW((HWND)lparam, TBM_GETPOS, 0, 0), 0);
        return 0;
    case WM_NOTIFY:
    {
        NMTTDISPINFOW *info = (NMTTDISPINFOW *)lparam;

        /* The toolbar forwards the tooltip request of a button without text. */
        if (info->hdr.code == TTN_GETDISPINFOW && info->hdr.idFrom == ID_HELP)
            lstrcpyW(info->szText, L"Help");
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hwnd, ID_FRUITS));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_CLOSE || LOWORD(wparam) == IDCANCEL)
            DestroyWindow(hwnd);
        else if (LOWORD(wparam) == ID_RUN)
        {
            SendDlgItemMessageW(hwnd, ID_LEVEL, PBM_SETPOS, 0, 0);
            SetTimer(hwnd, ID_RUN, RUN_INTERVAL_MS, NULL);
        }
        else if (LOWORD(wparam) == ID_BOLD)
            SendDlgItemMessageW(hwnd, ID_TOOLS, TB_ENABLEBUTTON, ID_UNDO, TRUE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int main(void)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
                                              ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                              ICC_UPDOWN_CLASS };

    InitCommonControlsEx(&icc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"Controls", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 460, 800, NULL, NULL,
                           wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
