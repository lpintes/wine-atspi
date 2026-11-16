/*
 * wine-atspi bridge, Win32 core: tracing, strings and memory in the
 * target process.
 */
#include "core.h"

BOOL debug;
/* Where trace() writes: stderr, or the file given with --log. */
HANDLE trace_handle;

/* Writes a line to stderr (or the log file) when debugging is on. */
void WINAPIV trace(const WCHAR *format, ...)
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
void print_line(const WCHAR *text)
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

void free_strings(struct focus_strings *str)
{
    HeapFree(GetProcessHeap(), 0, str->text);
    str->text = NULL;
}

void to_utf8(const WCHAR *src, char *dst, int size)
{
    if (!src || !WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, size, NULL, NULL))
        dst[0] = 0;
    dst[size - 1] = 0;
}

/* GetWindowText does not ask windows of other processes, WM_GETTEXT does. */
void get_window_text(HWND hwnd, char *dst, int size)
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
void get_app_name(DWORD pid, char *dst, int size)
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

void get_class(HWND hwnd, WCHAR *cls, int len, char *dst, int size)
{
    if (!GetClassNameW(hwnd, cls, len))
        cls[0] = 0;
    to_utf8(cls, dst, size);
}

/* Drops the "&" that marks a mnemonic; "&&" stays as one "&". */
void strip_ampersands(WCHAR *buf)
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
void strip_mnemonic(HWND hwnd, WCHAR *buf)
{
    if (!(GetWindowLongW(hwnd, GWL_STYLE) & SS_NOPREFIX))
        strip_ampersands(buf);
}

BOOL remote_open(HWND hwnd, struct remote *r)
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

void remote_close(struct remote *r)
{
    VirtualFreeEx(r->process, r->base, 0, MEM_RELEASE);
    CloseHandle(r->process);
}

/*
 * Sends msg with a pointer to header, copied to the target, and reads
 * the text the target wrote at REMOTE_HEADER into buf (CELL_TEXT_MAX).
 */
BOOL remote_text(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam,
                        const BYTE *header, WCHAR *buf)
{
    BYTE block[REMOTE_HEADER + sizeof(WCHAR)];
    DWORD_PTR ret;
    SIZE_T done;

    buf[0] = 0;
    /* Clear the text too, a control may leave an empty one unwritten. */
    memcpy(block, header, REMOTE_HEADER);
    memset(block + REMOTE_HEADER, 0, sizeof(WCHAR));
    if (!WriteProcessMemory(r->process, r->base, block, sizeof(block), &done) ||
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
BOOL remote_rect(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam, RECT *rect)
{
    DWORD_PTR ret = 0;
    SIZE_T done;

    return WriteProcessMemory(r->process, r->base, rect, sizeof(*rect), &done) &&
           SendMessageTimeoutW(hwnd, msg, wparam, (LPARAM)r->base, SMTO_ABORTIFHUNG, 500, &ret) &&
           ret && ReadProcessMemory(r->process, r->base, rect, sizeof(*rect), &done);
}
