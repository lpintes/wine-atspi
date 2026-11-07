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
 *   "Close"    Button
 */

#include <windows.h>

#define CLASS_NAME L"WineAtspiControls"

enum { ID_FRUITS = 100, ID_EMPTY, ID_COLORS, ID_CLOSE };

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
        add_control(hwnd, L"Button", L"Close", WS_TABSTOP | BS_PUSHBUTTON,
                    120, 250, 100, 25, ID_CLOSE);
        return 0;
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hwnd, ID_FRUITS));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_CLOSE || LOWORD(wparam) == IDCANCEL)
            DestroyWindow(hwnd);
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

    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"Controls", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 360, 330, NULL, NULL,
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
