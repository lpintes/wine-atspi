/*
 * Interface between the Win32 core and the AT-SPI side of wine-atspi.
 *
 * This header must include neither Windows nor Linux headers: the Win32
 * core sees it through Wine's msvcrt headers, the AT-SPI side through
 * glibc. Only plain C types cross the boundary; strings are UTF-8.
 *
 * Threading: atspi_init and atspi_run belong to the AT-SPI side's own
 * thread setup; atspi_post_* may be called from any thread. They copy
 * their arguments and hand them to the AT-SPI thread, which owns the
 * whole accessible model. The Win32 side never touches that model.
 */
#ifndef WINE_ATSPI_BRIDGE_H
#define WINE_ATSPI_BRIDGE_H

/* AT-SPI role numbers (AtspiRole), so the Win32 core can map MSAA roles. */
enum bridge_role
{
    BRIDGE_ROLE_CHECK_BOX = 7,
    BRIDGE_ROLE_COLUMN_HEADER = 10,
    BRIDGE_ROLE_COMBO_BOX = 11,
    BRIDGE_ROLE_DIALOG = 16,
    BRIDGE_ROLE_FILLER = 20,
    BRIDGE_ROLE_FRAME = 23,
    BRIDGE_ROLE_IMAGE = 27,
    BRIDGE_ROLE_LABEL = 29,
    BRIDGE_ROLE_LIST = 31,
    BRIDGE_ROLE_LIST_ITEM = 32,
    BRIDGE_ROLE_MENU = 33,
    BRIDGE_ROLE_MENU_BAR = 34,
    BRIDGE_ROLE_MENU_ITEM = 35,
    BRIDGE_ROLE_PAGE_TAB = 37,
    BRIDGE_ROLE_PAGE_TAB_LIST = 38,
    BRIDGE_ROLE_PANEL = 39,
    BRIDGE_ROLE_PASSWORD_TEXT = 40,
    BRIDGE_ROLE_POPUP_MENU = 41,
    BRIDGE_ROLE_PROGRESS_BAR = 42,
    BRIDGE_ROLE_PUSH_BUTTON = 43,
    BRIDGE_ROLE_RADIO_BUTTON = 44,
    BRIDGE_ROLE_SCROLL_BAR = 48,
    BRIDGE_ROLE_SEPARATOR = 50,
    BRIDGE_ROLE_SLIDER = 51,
    BRIDGE_ROLE_SPIN_BUTTON = 52,
    BRIDGE_ROLE_STATUS_BAR = 54,
    BRIDGE_ROLE_TABLE = 55,
    BRIDGE_ROLE_TABLE_CELL = 56,
    BRIDGE_ROLE_TEXT = 61,
    BRIDGE_ROLE_TOOL_BAR = 63,
    BRIDGE_ROLE_TREE = 65,
    BRIDGE_ROLE_UNKNOWN = 67,
    BRIDGE_ROLE_WINDOW = 69,
    BRIDGE_ROLE_APPLICATION = 75,
    BRIDGE_ROLE_ENTRY = 79,
    BRIDGE_ROLE_LINK = 88,
    BRIDGE_ROLE_TABLE_ROW = 90,
    BRIDGE_ROLE_TREE_ITEM = 91,
    BRIDGE_ROLE_GROUPING = 99,
};

/* AT-SPI state numbers (AtspiStateType); use as bit positions. */
enum bridge_state
{
    BRIDGE_STATE_ACTIVE = 1,
    BRIDGE_STATE_CHECKED = 4,
    BRIDGE_STATE_EDITABLE = 7,
    BRIDGE_STATE_ENABLED = 8,
    BRIDGE_STATE_EXPANDABLE = 9,
    BRIDGE_STATE_EXPANDED = 10,
    BRIDGE_STATE_FOCUSABLE = 11,
    BRIDGE_STATE_FOCUSED = 12,
    BRIDGE_STATE_MULTI_LINE = 17,
    BRIDGE_STATE_MULTISELECTABLE = 18,
    BRIDGE_STATE_PRESSED = 20,
    BRIDGE_STATE_SELECTABLE = 22,
    BRIDGE_STATE_SELECTED = 23,
    BRIDGE_STATE_SENSITIVE = 24,
    BRIDGE_STATE_SHOWING = 25,
    BRIDGE_STATE_SINGLE_LINE = 26,
    BRIDGE_STATE_VISIBLE = 30,
    BRIDGE_STATE_INDETERMINATE = 32,
    BRIDGE_STATE_READ_ONLY = 43,
};

#define BRIDGE_STATE_BIT(s) (1ULL << (s))

/* One accessible object as seen by the Win32 core at one moment. */
struct bridge_object
{
    /* Identity: window handle and MSAA child id (0 for the window). */
    unsigned long long hwnd;
    unsigned int child;
    unsigned int role;          /* enum bridge_role */
    unsigned long long states;  /* BRIDGE_STATE_BIT of enum bridge_state */
    const char *name;           /* may be NULL */
    const char *value;          /* current value, e.g. a combo box text; may be NULL */
    const char *window_class;   /* GetClassName of hwnd, may be NULL */
    /*
     * Content of a text control, NULL for objects without text. Line
     * breaks are "\n". Offsets count characters (code points), as AT-SPI
     * does; selection_start == selection_end means no selection.
     */
    const char *text;
    int caret;
    int selection_start;
    int selection_end;
};

/* Focus moved to object, which lives in top-level window frame. */
struct bridge_focus
{
    unsigned int pid;
    const char *app_name;       /* executable name, e.g. "notepad.exe" */
    struct bridge_object frame;
    struct bridge_object object;
};

/*
 * One key press or release, in X11 terms, as GTK's atk-bridge reports it
 * to the registry's DeviceEventController.
 */
struct bridge_key
{
    int pressed;                /* 1 press, 0 release */
    unsigned int keysym;        /* X keysym, e.g. 0xff51 for Left */
    unsigned int keycode;       /* X keycode: evdev code + 8 */
    unsigned int modifiers;     /* X modifier mask: ShiftMask, ControlMask, ... */
    unsigned int timestamp;     /* milliseconds */
    const char *string;         /* typed text or key name, may be NULL */
    int is_text;                /* string is typed text */
};

/*
 * Prepares the AT-SPI side. Call once before any other function here.
 * Returns 0 on success, non-zero if the a11y bus was not reachable.
 */
int atspi_init(void);

/* Runs the AT-SPI main loop on the calling thread; does not return. */
void atspi_run(void);

/* Reports a focus change. Copies everything; callable from any thread. */
void atspi_post_focus(const struct bridge_focus *focus);

/*
 * Reports a fresh look at an object of process pid, e.g. after a key
 * press: a changed name, state, text or caret is announced. Objects not in the model
 * are ignored. Copies everything; callable from any thread.
 */
void atspi_post_update(unsigned int pid, const struct bridge_object *object);

/*
 * Reports a key to the registry, for screen readers that get keys only
 * from applications (libatspi's legacy device: Wayland without a
 * compositor keyboard monitor, e.g. WSLg). Copies everything; callable
 * from any thread.
 */
void atspi_post_key(const struct bridge_key *key);

/*
 * Reports that window hwnd was hidden or destroyed: every object of that
 * window leaves the model. Unknown windows are ignored, so it may be
 * called for any window. Callable from any thread.
 */
void atspi_post_window_gone(unsigned long long hwnd);

/*
 * Reports that process pid has exited: its application leaves the
 * desktop and its D-Bus connection is closed. Unknown processes are
 * ignored. Callable from any thread.
 */
void atspi_post_app_gone(unsigned int pid);

#endif
