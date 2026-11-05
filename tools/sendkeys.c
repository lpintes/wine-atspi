/*
 * sendkeys - drive Win32 programs in a Wine prefix with SendInput.
 *
 * Used by automated tests so nobody has to type into the target program.
 * Pure Win32: builds with winegcc and with mingw.
 *
 * Usage: sendkeys STEP...
 *   w:MS        wait MS milliseconds
 *   f:CLASS     bring the first top-level window of CLASS to the foreground
 *   t:TEXT      type TEXT as Unicode characters
 *   k:KEYS      press a key combination, e.g. k:down, k:alt+f, k:ctrl+shift+end
 *
 * Key names: a single letter or digit, f1..f12, up, down, left, right, home,
 * end, pgup, pgdn, return, escape, tab, space, back, delete, insert, and the
 * modifiers alt, ctrl, shift, win. Steps are separated by a short pause.
 */

#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <wchar.h>

#define STEP_DELAY_MS 100

/* CRT stdio is avoided: in a Winelib binary msvcrt and ucrtbase get mixed.
 * WINAPIV is required: in Winelib on x86_64 va_list is the ms_abi one. */
static void WINAPIV error(const WCHAR *fmt, ...)
{
    WCHAR wbuf[1024];
    char buf[3072];
    va_list args;
    int len;
    DWORD written;

    lstrcpyW(wbuf, L"sendkeys: ");
    va_start(args, fmt);
    wvsprintfW(wbuf + lstrlenW(wbuf), fmt, args);
    va_end(args);
    lstrcatW(wbuf, L"\n");
    len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), NULL, NULL);
    if (len > 0)
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, len - 1, &written, NULL);
}

struct key_name
{
    const WCHAR *name;
    WORD vk;
};

static const struct key_name key_names[] =
{
    { L"alt", VK_MENU }, { L"ctrl", VK_CONTROL }, { L"shift", VK_SHIFT },
    { L"win", VK_LWIN }, { L"up", VK_UP }, { L"down", VK_DOWN },
    { L"left", VK_LEFT }, { L"right", VK_RIGHT }, { L"home", VK_HOME },
    { L"end", VK_END }, { L"pgup", VK_PRIOR }, { L"pgdn", VK_NEXT },
    { L"return", VK_RETURN }, { L"escape", VK_ESCAPE }, { L"tab", VK_TAB },
    { L"space", VK_SPACE }, { L"back", VK_BACK }, { L"delete", VK_DELETE },
    { L"insert", VK_INSERT },
};

static WORD parse_key(const WCHAR *name)
{
    unsigned int i;

    if (name[0] && !name[1])
    {
        WCHAR c = towupper(name[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return c;
    }
    if ((name[0] == L'f' || name[0] == L'F') && name[1])
    {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    for (i = 0; i < ARRAYSIZE(key_names); i++)
        if (!lstrcmpiW(name, key_names[i].name))
            return key_names[i].vk;
    return 0;
}

static BOOL is_extended(WORD vk)
{
    switch (vk)
    {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_LWIN:
        return TRUE;
    }
    return FALSE;
}

static void fill_key(INPUT *input, WORD vk, BOOL up)
{
    memset(input, 0, sizeof(*input));
    input->type = INPUT_KEYBOARD;
    input->ki.wVk = vk;
    input->ki.wScan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (up)
        input->ki.dwFlags |= KEYEVENTF_KEYUP;
    if (is_extended(vk))
        input->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
}

static int press_keys(const WCHAR *spec)
{
    WCHAR buf[256], *name, *next;
    WORD vks[8];
    INPUT inputs[16];
    int count = 0, i;

    lstrcpynW(buf, spec, ARRAYSIZE(buf));
    for (name = buf; name; name = next)
    {
        next = wcschr(name, L'+');
        if (next)
            *next++ = 0;
        if (count == ARRAYSIZE(vks) || !(vks[count] = parse_key(name)))
        {
            error(L"bad key \"%s\"", name);
            return 0;
        }
        count++;
    }
    for (i = 0; i < count; i++)
        fill_key(&inputs[i], vks[i], FALSE);
    for (i = 0; i < count; i++)
        fill_key(&inputs[count + i], vks[count - 1 - i], TRUE);
    return SendInput(count * 2, inputs, sizeof(INPUT)) == (UINT)count * 2;
}

static int type_text(const WCHAR *text)
{
    for (; *text; text++)
    {
        INPUT inputs[2];

        memset(inputs, 0, sizeof(inputs));
        inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = inputs[1].ki.wScan = *text;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        if (SendInput(2, inputs, sizeof(INPUT)) != 2)
            return 0;
        Sleep(20);
    }
    return 1;
}

static int focus_class(const WCHAR *cls)
{
    HWND hwnd = FindWindowW(cls, NULL);

    if (!hwnd)
    {
        error(L"no window of class \"%s\"", cls);
        return 0;
    }
    SetForegroundWindow(hwnd);
    return 1;
}

/* wmain is not used: its argv is broken when msvcrt and ucrtbase get mixed. */
int main(void)
{
    int argc, i, ok;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (!argv || argc < 2)
    {
        error(L"usage: sendkeys w:MS | f:CLASS | t:TEXT | k:KEYS ...");
        return 2;
    }
    for (i = 1; i < argc; i++)
    {
        const WCHAR *arg = argv[i];

        if (!arg[0] || arg[1] != L':')
            ok = 0;
        else if (arg[0] == L'w')
        {
            Sleep(_wtoi(arg + 2));
            continue;
        }
        else if (arg[0] == L'f')
            ok = focus_class(arg + 2);
        else if (arg[0] == L't')
            ok = type_text(arg + 2);
        else if (arg[0] == L'k')
            ok = press_keys(arg + 2);
        else
            ok = 0;
        if (!ok)
        {
            error(L"step \"%s\" failed", arg);
            return 1;
        }
        Sleep(STEP_DELAY_MS);
    }
    return 0;
}
