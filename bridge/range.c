/*
 * wine-atspi bridge, Win32 core: trackbars and up-down controls with
 * their buddy Edit (spin buttons).
 */
#include "core.h"

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
         send_message(hwnd, UDM_GETRANGE32, (WPARAM)base, (LPARAM)(base + sizeof(INT)),
                      SEND_TIMEOUT_MS, NULL) &&
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
HWND find_updown(HWND hwnd)
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
        if (send_message(sibling, UDM_GETBUDDY, 0, 0, SEND_TIMEOUT_MS, &buddy) &&
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
void describe_range(HWND hwnd, const WCHAR *cls, struct bridge_object *obj)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    DWORD_PTR pos, low, high, step = 0;
    BOOL vertical;

    if (!lstrcmpiW(cls, L"msctls_trackbar32"))
    {
        if (!send_message(hwnd, TBM_GETPOS, 0, 0, SEND_TIMEOUT_MS, &pos) ||
            !send_message(hwnd, TBM_GETRANGEMIN, 0, 0, SEND_TIMEOUT_MS, &low) ||
            !send_message(hwnd, TBM_GETRANGEMAX, 0, 0, SEND_TIMEOUT_MS, &high))
            return;
        send_message(hwnd, TBM_GETLINESIZE, 0, 0, SEND_TIMEOUT_MS, &step);
        vertical = (style & TBS_VERT) != 0;
    }
    else if (!lstrcmpiW(cls, L"msctls_progress32"))
    {
        if (!send_message(hwnd, PBM_GETPOS, 0, 0, SEND_TIMEOUT_MS, &pos) ||
            !send_message(hwnd, PBM_GETRANGE, TRUE, 0, SEND_TIMEOUT_MS, &low) ||
            !send_message(hwnd, PBM_GETRANGE, FALSE, 0, SEND_TIMEOUT_MS, &high))
            return;
        vertical = (style & PBS_VERTICAL) != 0;
    }
    else if (!lstrcmpiW(cls, L"msctls_updown32"))
    {
        if (!updown_range(hwnd, &low, &high) ||
            !send_message(hwnd, UDM_GETPOS32, 0, 0, SEND_TIMEOUT_MS, &pos))
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
