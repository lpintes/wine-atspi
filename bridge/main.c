/*
 * wine-atspi bridge: Win32 core.
 *
 * Windows headers only, no Linux headers. Starts the AT-SPI side on its
 * own thread and runs the Win32 message loop on the main thread.
 *
 * Currently a spike: it registers one test application so the build of a
 * mixed Win32 + GDBus Winelib binary can be checked.
 */
#include <windows.h>

#include "bridge.h"

static DWORD WINAPI atspi_thread(void *arg)
{
    /*
     * Created with CreateThread, so the thread has a TEB and may call
     * Win32 later; the AT-SPI side itself only uses GLib.
     */
    return atspi_run("wine-atspi spike");
}

int main(void)
{
    HANDLE thread;
    MSG msg;

    thread = CreateThread(NULL, 0, atspi_thread, NULL, 0, NULL);
    if (!thread)
        return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
