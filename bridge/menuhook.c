/*
 * wine-atspi menu hook: a PE DLL that the bridge installs as a global
 * WH_CALLWNDPROC hook, so Wine loads it into every GUI process of the
 * prefix (user32/hook.c, get_hook_proc). It watches the menu messages
 * that Wine sends to the menu owner and reports each highlighted item to
 * the bridge as a struct menu_record (menuhook.h) by WM_COPYDATA.
 *
 * Built with mingw, not winegcc: a Winelib DLL cannot be loaded into a
 * PE process.
 */
#include <windows.h>

#include "menuhook.h"

/* The target's menu loop waits for the bridge this long at most. */
#define SEND_TIMEOUT_MS 200
#define MAX_OPEN 16

/*
 * Popup menus open in the current menu mode, in opening order: which
 * item of which menu opened each one. Menu mode is modal for one thread.
 */
static struct
{
    HMENU menu;
    HMENU parent;       /* NULL for a root popup (context menu) */
    UINT pos;           /* position of the opening item in parent */
} open_menus[MAX_OPEN];
static int open_count;
static DWORD menu_thread;
static HWND menu_owner;
static HMENU last_menu;     /* menu of the last highlighted item */
static UINT32 last_kind;

static void reset(void)
{
    open_count = 0;
    menu_thread = GetCurrentThreadId();
    menu_owner = NULL;
    last_menu = NULL;
    last_kind = MENU_KIND_BAR;
}

static void send_record(const struct menu_record *rec)
{
    HWND sink = FindWindowW(MENU_SINK_CLASS, NULL);
    COPYDATASTRUCT cds;
    DWORD_PTR result;
    DWORD pid = 0;

    if (!sink)
        return;
    /* The hook runs in the bridge as well, which has no menus. */
    GetWindowThreadProcessId(sink, &pid);
    if (pid == GetCurrentProcessId())
        return;
    cds.dwData = MENU_RECORD_TAG;
    cds.cbData = sizeof(*rec);
    cds.lpData = (void *)rec;
    SendMessageTimeoutW(sink, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG,
                        SEND_TIMEOUT_MS, &result);
}

static int find_open(HMENU menu)
{
    int i;

    for (i = 0; i < open_count; i++)
        if (open_menus[i].menu == menu)
            return i;
    return -1;
}

/* Text, type, state and submenu of the item at pos; FALSE if there is none. */
static BOOL get_item(HMENU menu, UINT pos, MENUITEMINFOW *mii, WCHAR *text)
{
    memset(mii, 0, sizeof(*mii));
    mii->cbSize = sizeof(*mii);
    mii->fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
    mii->dwTypeData = text;
    mii->cch = MENU_TEXT_MAX;
    text[0] = 0;
    if (!GetMenuItemInfoW(menu, pos, TRUE, mii))
        return FALSE;
    text[MENU_TEXT_MAX - 1] = 0;
    return TRUE;
}

/* Wine's system menu is the only item of a wrapper menu (win32u/menu.c). */
static BOOL is_system_wrapper(HMENU menu)
{
    return GetMenuItemCount(menu) == 1 && GetSubMenu(menu, 0) != NULL;
}

/*
 * Reports the item at pos of menu as highlighted. The path up to the root
 * menu comes from open_menus.
 */
static void send_select(HWND owner, HMENU menu, UINT pos, UINT32 kind)
{
    struct menu_record rec;
    MENUITEMINFOW mii;
    WCHAR text[MENU_TEXT_MAX];
    /* From the item up: menu and position of each level. */
    HMENU menus[MAX_OPEN + 1];
    UINT positions[MAX_OPEN + 1];
    int n = 0, i, level, count;

    memset(&rec, 0, sizeof(rec));
    if (!get_item(menu, pos, &mii, text))
        return;
    rec.event = MENU_EVENT_SELECT;
    rec.kind = kind;
    rec.owner = (ULONG_PTR)owner;
    rec.type = mii.fType;
    rec.state = mii.fState;
    rec.has_submenu = mii.hSubMenu != NULL;

    menus[n] = menu;
    positions[n++] = pos;
    while ((i = find_open(menus[n - 1])) >= 0 && open_menus[i].parent && n < (int)ARRAYSIZE(menus))
    {
        menus[n] = open_menus[i].parent;
        positions[n++] = open_menus[i].pos;
    }
    if (kind == MENU_KIND_SYSTEM && is_system_wrapper(menus[n - 1]))
        n--;
    rec.depth = n < MENU_MAX_DEPTH ? n : MENU_MAX_DEPTH;
    for (level = 0; level < (int)rec.depth; level++)
    {
        /* Too deep: the root levels and the item itself stay. */
        i = level == (int)rec.depth - 1 ? 0 : n - 1 - level;
        rec.path[level] = positions[i];
        get_item(menus[i], positions[i], &mii, rec.texts[level]);
    }

    count = GetMenuItemCount(menu);
    for (i = 0; i < count; i++)
    {
        if (!get_item(menu, i, &mii, text) || (mii.fType & MFT_SEPARATOR))
            continue;
        rec.count++;
        if (i <= (int)pos)
            rec.position = rec.count;
    }
    send_record(&rec);
}

static void send_end(HWND owner)
{
    struct menu_record rec;

    memset(&rec, 0, sizeof(rec));
    rec.event = MENU_EVENT_END;
    rec.owner = (ULONG_PTR)owner;
    send_record(&rec);
}

/*
 * Position of the highlighted item. WM_MENUSELECT gives a position for
 * an item with a submenu, else its command id, which need not be unique.
 */
static int item_position(HMENU menu, UINT item, UINT flags)
{
    int count = GetMenuItemCount(menu), i, first = -1;

    if (flags & MF_POPUP)
        return item;
    for (i = 0; i < count; i++)
    {
        if (GetMenuItemID(menu, i) != item)
            continue;
        if (GetMenuState(menu, i, MF_BYPOSITION) & MF_HILITE)
            return i;
        if (first < 0)
            first = i;
    }
    return first;
}

static UINT32 menu_kind(HWND owner, HMENU menu, UINT flags)
{
    HMENU root = menu;
    int i;

    if (flags & MF_SYSMENU)
        return MENU_KIND_SYSTEM;
    while ((i = find_open(root)) >= 0 && open_menus[i].parent)
        root = open_menus[i].parent;
    if (GetMenu(owner) == root || GetMenu(GetAncestor(owner, GA_ROOT)) == root)
        return MENU_KIND_BAR;
    return MENU_KIND_POPUP;
}

static void on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HMENU menu;
    int i, pos;

    switch (msg)
    {
    case WM_ENTERMENULOOP:
        reset();
        break;
    case WM_INITMENUPOPUP:
        menu = (HMENU)wp;
        if (menu_thread != GetCurrentThreadId())
            reset();
        if (find_open(menu) >= 0 || open_count == MAX_OPEN)
            break;
        open_menus[open_count].menu = menu;
        open_menus[open_count].pos = LOWORD(lp);
        /* Opened by the item highlighted last, or a root popup. */
        open_menus[open_count].parent =
            last_menu && GetSubMenu(last_menu, LOWORD(lp)) == menu ? last_menu : NULL;
        open_count++;
        break;
    case WM_UNINITMENUPOPUP:
        /*
         * Escape or Left back to the parent item sends no WM_MENUSELECT
         * for it (win32u/menu.c), so report it here. If another item gets
         * highlighted instead, its own record follows at once.
         */
        if (menu_thread != GetCurrentThreadId() || (i = find_open((HMENU)wp)) < 0)
            break;
        open_count = i;
        if (open_menus[i].parent && menu_owner)
        {
            last_menu = open_menus[i].parent;
            send_select(menu_owner, open_menus[i].parent, open_menus[i].pos, last_kind);
        }
        break;
    case WM_MENUSELECT:
        menu = (HMENU)lp;
        if (HIWORD(wp) == 0xffff && !menu)
        {
            send_end(hwnd);
            reset();
            break;
        }
        if (!menu)
            break;
        if (menu_thread != GetCurrentThreadId())
            reset();
        if ((pos = item_position(menu, LOWORD(wp), HIWORD(wp))) < 0)
            break;
        menu_owner = hwnd;
        last_menu = menu;
        last_kind = menu_kind(hwnd, menu, HIWORD(wp));
        send_select(hwnd, menu, pos, last_kind);
        break;
    case WM_EXITMENULOOP:
        send_end(hwnd);
        reset();
        break;
    }
}

__declspec(dllexport) LRESULT CALLBACK CallWndProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const CWPSTRUCT *cwp = (const CWPSTRUCT *)lp;

        switch (cwp->message)
        {
        case WM_ENTERMENULOOP:
        case WM_EXITMENULOOP:
        case WM_INITMENUPOPUP:
        case WM_UNINITMENUPOPUP:
        case WM_MENUSELECT:
            on_message(cwp->hwnd, cwp->message, cwp->wParam, cwp->lParam);
            break;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HMODULE pinned;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        /*
         * Wine frees a hook module after each call unless it was loaded
         * already (user32/hook.c, get_hook_proc); keep it, with its state.
         */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)DllMain, &pinned);
    }
    return TRUE;
}
