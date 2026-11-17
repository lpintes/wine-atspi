/*
 * msgbox-race - a modal message box shown right after the main window.
 *
 * Reproduces what caption-player (Go, walk) does when tools are missing:
 * the main window is shown, a message posted to it before the message
 * loop starts focuses its list box and opens MessageBoxW with the main
 * window as owner. The box disables its owner while the X window manager
 * may still be mapping the main window.
 *
 * Seen under Wine 11.21 (WSLg, also the user's session): sometimes the
 * box gets focus first and then the disabled main window gets
 * foreground and focus; keys then reach nothing and the box stays
 * without focus. With WINEDEBUG=+event the timing changes and it works.
 *
 * Usage: msgbox-race [delay-ms [show]]
 *   delay-ms  wait so long after showing the window before posting the
 *             message (default 0, as caption-player).
 *   show      show the window with SW_SHOW instead of SW_SHOWNA, which
 *             walk uses.
 *
 * After the box closes the title says which button was chosen and
 * "answer: Yes" or "answer: No" goes to stdout, so a script can send
 * Return and tell whether the box got it. Pure Win32: builds with
 * winegcc and with mingw.
 */

#include <windows.h>
#include <stdlib.h>

#define WM_APP_SHOW_BOX (WM_APP + 1)

static HWND list;

/* CRT stdio is unreliable in a Winelib binary (see CLAUDE.md). */
static void print(const char *text)
{
    DWORD written;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text),
              &written, NULL);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp,
                                    LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               WS_VSCROLL | LBS_NOTIFY,
                               10, 10, 300, 150, hwnd, (HMENU)100,
                               NULL, NULL);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"Apple");
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"Banana");
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"Cherry");
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        return 0;
    case WM_APP_SHOW_BOX:
    {
        int answer;

        SetFocus(list);
        answer = MessageBoxW(hwnd,
                             L"These tools are missing: ffmpeg. "
                             L"Download them now?",
                             L"Missing tools",
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        SetWindowTextW(hwnd, answer == IDYES ? L"Msgbox race, Yes"
                                             : L"Msgbox race, No");
        print(answer == IDYES ? "answer: Yes\n" : "answer: No\n");
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(list);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(void)
{
    WNDCLASSW wc = {0};
    HWND hwnd;
    MSG msg;
    int delay = 0;
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int show_cmd = SW_SHOWNA;

    if (argv && argc > 1)
        delay = _wtoi(argv[1]);
    if (argv && argc > 2 && !lstrcmpW(argv[2], L"show"))
        show_cmd = SW_SHOW;

    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WineAtspiMsgboxRace";
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Msgbox race",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 340, 220, NULL, NULL,
                           wc.hInstance, NULL);
    /* walk shows a form with SW_SHOWNA (setWindowVisible), so the
     * window is activated only later, by the window manager. */
    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);
    if (delay > 0)
        Sleep(delay);
    PostMessageW(hwnd, WM_APP_SHOW_BOX, 0, 0);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
