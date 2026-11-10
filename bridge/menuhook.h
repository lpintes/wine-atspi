/*
 * Records that the menu hook DLL (menuhook.c), running inside a target
 * process, sends to the bridge with WM_COPYDATA. Menus cannot be read
 * from another process (GetMenuItemInfo fails with error 1400), so the
 * hook reads them where they live. Toolbar tooltips are asked for the
 * same way (TOOLTIP_MESSAGE).
 *
 * Windows headers only; included by the hook DLL and the Win32 core.
 * Fixed-size fields, so a 32-bit hook can talk to the 64-bit bridge.
 */
#ifndef WINE_ATSPI_MENUHOOK_H
#define WINE_ATSPI_MENUHOOK_H

#include <windows.h>

/* Hidden top-level window of the bridge that receives the records. */
#define MENU_SINK_CLASS L"WineAtspiMenuSink"
/* COPYDATASTRUCT.dwData of a struct menu_record. */
#define MENU_RECORD_TAG 0x554e454d

enum menu_event
{
    MENU_EVENT_SELECT = 1,  /* item highlighted, or current again after its submenu closed */
    MENU_EVENT_END = 2,     /* menu mode ended */
};

enum menu_kind
{
    MENU_KIND_BAR = 0,      /* menu bar of the owner window */
    MENU_KIND_SYSTEM = 1,   /* system menu (Alt+Space) */
    MENU_KIND_POPUP = 2,    /* context menu (TrackPopupMenu) */
};

/* Levels of nested menus that a record describes; deeper ones are cut. */
#define MENU_MAX_DEPTH 4
#define MENU_TEXT_MAX 256

struct menu_record
{
    UINT32 event;               /* enum menu_event */
    UINT32 kind;                /* enum menu_kind of the root menu */
    UINT64 owner;               /* window that gets the menu messages */
    /*
     * Positions (from 0, separators included) from the root menu down to
     * the item: path[0] in the root menu, path[depth - 1] the item, and
     * their raw texts, e.g. "&Open...\tCtrl+O". Wine's system menu hangs
     * below a wrapper item, which is left out; depth 0 is that wrapper,
     * the system menu itself. Levels below MENU_MAX_DEPTH - 1 are cut,
     * the item stays last.
     */
    UINT32 depth;
    UINT32 path[MENU_MAX_DEPTH];
    WCHAR texts[MENU_MAX_DEPTH][MENU_TEXT_MAX];
    UINT32 type;                /* MENUITEMINFO fType of the item */
    UINT32 state;               /* MENUITEMINFO fState of the item */
    UINT32 has_submenu;
    /* Among the items of its menu that are no separators, from 1. */
    INT32 position;
    INT32 count;
};

/*
 * Registered message that the bridge sends to a ToolbarWindow32 to get
 * the tooltip of a button without text. The tooltip text comes from the
 * toolbar's owner by WM_NOTIFY, which Wine refuses across processes, so
 * the hook asks for it inside the target. wParam is the button's command
 * id, lParam the address of a buffer in the target (allocated by the
 * bridge) for TOOLTIP_TEXT_MAX WCHARs. The hook writes the text there,
 * terminated; the buffer is left alone if there is no hook.
 */
#define TOOLTIP_MESSAGE L"WineAtspiToolTip"
#define TOOLTIP_TEXT_MAX 256

#endif
