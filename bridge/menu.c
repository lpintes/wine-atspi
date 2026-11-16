/*
 * wine-atspi bridge, Win32 core: menus, reported by the hook DLL
 * (menuhook.c).
 */
#include "core.h"

struct menu_state menu;

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
BOOL still_in_menu(void)
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
HHOOK install_menu_hook(void)
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
