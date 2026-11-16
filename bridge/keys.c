/*
 * wine-atspi bridge, Win32 core: the low-level keyboard hook, which
 * reports keys to the AT-SPI registry and schedules the key check.
 */
#include "core.h"

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

LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
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
