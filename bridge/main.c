/*
 * wine-atspi bridge: Win32 core.
 *
 * Windows headers only, no Linux headers. Starts the AT-SPI side on its
 * own thread and runs the Win32 message loop on the main thread. A
 * WinEvent hook reports focus changes; each one is described through
 * MSAA and handed to the AT-SPI side as a plain snapshot.
 *
 * This file holds the start, the message loop and the install and
 * uninstall commands; the rest of the core is described in core.h.
 */
#include "core.h"

/* Held by the running bridge: one bridge per prefix. */
#define BRIDGE_MUTEX L"WineAtspiBridge"
/* wineboot --init starts these at every session start (not Run). */
#define RUN_SERVICES_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices"
#define RUN_SERVICES_VALUE L"WineAtspi"
/* ProcessWineMakeProcessSystem, declared only under __WINESRC__. */
#define PROCESS_WINE_MAKE_PROCESS_SYSTEM ((PROCESSINFOCLASS)1000)
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
