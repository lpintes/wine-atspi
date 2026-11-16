/*
 * wine-atspi bridge: what the files of the Win32 core share.
 *
 * Windows headers only, no Linux headers; the AT-SPI side is reached
 * through bridge.h. Each .c file of the core holds one area: util.c
 * (tracing, strings, memory in the target), object.c (role, states,
 * extents, the handler of a class), one file per control family
 * (text.c, range.c, labels.c, listview.c, listbox.c, treeview.c, tab.c,
 * toolbar.c, console.c, statusbar.c), wintree.c (the window tree),
 * events.c (WinEvents, focus, key check), menu.c, keys.c and main.c.
 */
#ifndef WINE_ATSPI_CORE_H
#define WINE_ATSPI_CORE_H

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <oleacc.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include "bridge.h"
#include "menuhook.h"

#define NAME_MAX_UTF8 1024
/* Columns of a list view in report view beyond this are left out. */
#define MAX_COLUMNS 16
/* Selected rows of a table listed by index; beyond this only counted. */
#define MAX_SELECTED_ROWS 64
/* Texts of a window that name no control, see describe_labels. */
#define MAX_LABELS 16
#define LABEL_MAX_UTF8 2048
/* All parts of a status bar, in UTF-16 units; see report_status_bar. */
#define STATUS_TEXT_MAX 1024
/* A status bar is read after each key: do not wait long for a busy target. */
#define STATUS_TIMEOUT_MS 100

/* UTF-8 buffers backing the strings of one bridge_focus. */
struct focus_strings
{
    char app[MAX_PATH * 3];
    char frame[NAME_MAX_UTF8];
    char container[NAME_MAX_UTF8];
    char object[NAME_MAX_UTF8];
    char value[NAME_MAX_UTF8];
    char frame_class[256 * 3];
    char object_class[256 * 3];
    /* Column texts of a table: cells of the focused row, or the headers. */
    char cells[MAX_COLUMNS][NAME_MAX_UTF8];
    const char *cell_ptrs[MAX_COLUMNS];
    int cell_extents[MAX_COLUMNS * 4];
    char headers[MAX_COLUMNS][NAME_MAX_UTF8];
    const char *header_ptrs[MAX_COLUMNS];
    int selected_rows[MAX_SELECTED_ROWS];
    char labels[MAX_LABELS][LABEL_MAX_UTF8];
    struct bridge_object label_objects[MAX_LABELS];
    char *text;     /* heap, see free_strings */
};

/* Room for the structure in front of the text in the target. */
#define REMOTE_HEADER 128
/* Text of one list view cell or column header, in WCHARs. */
#define CELL_TEXT_MAX (NAME_MAX_UTF8 / 3)

/*
 * Wine does not marshal list view messages with pointers across
 * processes. Like NVDA, put the structure and its text buffer into a
 * block of the target's memory: the structure at the start, the text
 * at REMOTE_HEADER.
 */
struct remote
{
    HANDLE process;
    char *base;
    BOOL wow64;
};

/*
 * Index of the focused item of a list view, -1 if none. LIST_BUSY if the
 * target did not answer in time: the Open dialog's list keeps its thread
 * busy for over 500 ms after an arrow key (WSL, 2026-11-06).
 */
#define LIST_BUSY (-2)

/*
 * What all items of a list view share. The window tree describes many
 * rows of one list, and each message to the target is a round trip.
 */
struct listview_info
{
    int columns;            /* from listview_columns */
    int order[MAX_COLUMNS];
    DWORD_PTR count;        /* items */
    /*
     * Cell x and width of each column from the first row described: in
     * report view all rows have the same columns, so a later row asks
     * only for its own top and height.
     */
    BOOL have_cells;
    int cells[4 * MAX_COLUMNS];
};

/*
 * Level and siblings of the parents met in a walk over the visible items
 * of a tree (TVGN_FIRSTVISIBLE, then TVGN_NEXTVISIBLE). Visible items come
 * in tree order, so the next item with the same parent is the next
 * sibling: its place follows without walking all siblings again, which
 * costs one round trip each.
 */
#define TREE_CACHE_SIZE 8

struct tree_cache
{
    struct
    {
        HTREEITEM parent;
        int level, position, set_size;
    } entries[TREE_CACHE_SIZE];
    int count, next;
};

/* A range out of the client area lies this far above or below the window. */
#define TEXT_FAR_AWAY 100000

#define CONSOLE_CLASS L"WineConsoleClass"
/* Output comes without a key, so a focused console is read this often. */
#define CONSOLE_POLL_MS 100

/*
 * AttachConsole needs a process of the console, not conhost.exe itself
 * (error 6); one is found by trying every process and kept here. It also
 * names the application.
 */
struct console_client_state
{
    HWND hwnd;
    DWORD pid;
};

/* The last status bar text reported, see report_status_bar. */
struct status_state
{
    HWND hwnd;
    WCHAR text[STATUS_TEXT_MAX];
};

/* The object of the last focus event, checked again after each key. */
struct focus_state
{
    HWND hwnd;
    LONG id_object;
    LONG id_child;
    DWORD pid;
    BOOL recovered;     /* reported by check_missed_focus, not by an event */
};

/* Menu mode of a target, see report_menu_item. */
struct menu_state
{
    BOOL active;            /* a menu item was reported, no menu end yet */
    DWORD thread;           /* thread in menu mode */
    struct menu_record pending;
    BOOL has_pending;
    UINT_PTR timer;
};

/* Sent to the main thread when a watched process ends, see watch_process. */
#define WM_PROCESS_EXITED (WM_APP + 1)

/*
 * Wine emits no WinEvent when a key changes a combo box, a list box
 * selection or a user32 Edit. Like NVDA, look at the focused object again
 * shortly after each key, once the target has handled it.
 */
#define KEY_CHECK_DELAY_MS 50

/* util.c */
extern BOOL debug;
extern HANDLE trace_handle;
void WINAPIV trace(const WCHAR *format, ...);
void print_line(const WCHAR *text);
void free_strings(struct focus_strings *str);
void to_utf8(const WCHAR *src, char *dst, int size);
void get_window_text(HWND hwnd, char *dst, int size);
void get_app_name(DWORD pid, char *dst, int size);
void get_class(HWND hwnd, WCHAR *cls, int len, char *dst, int size);
void strip_ampersands(WCHAR *buf);
void strip_mnemonic(HWND hwnd, WCHAR *buf);
BOOL remote_open(HWND hwnd, struct remote *r);
void remote_close(struct remote *r);
BOOL remote_text(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam,
                 const BYTE *header, WCHAR *buf);
BOOL remote_rect(HWND hwnd, struct remote *r, UINT msg, WPARAM wparam, RECT *rect);

/* object.c */
unsigned int role_from_class(HWND hwnd, const WCHAR *cls);
void get_extents(HWND hwnd, struct bridge_object *obj);
void client_to_screen(HWND hwnd, RECT rect, int *out);
void set_item_extents(HWND hwnd, RECT rect, struct bridge_object *obj);
BOOL is_list_box(const WCHAR *cls);
BOOL is_tree_view(const WCHAR *cls);
BOOL is_tab_control(const WCHAR *cls);
BOOL is_item_control(const WCHAR *cls);
BOOL describe_object(HWND hwnd, LONG id_object, LONG id_child,
                     struct bridge_object *obj, struct focus_strings *str);
void describe_frame(HWND hwnd, struct bridge_object *obj, struct focus_strings *str);

/* range.c */
HWND find_updown(HWND hwnd);
void describe_range(HWND hwnd, const WCHAR *cls, struct bridge_object *obj);

/* text.c */
int char_offset(const WCHAR *text, int len, int pos);
void describe_edit_text(HWND hwnd, BOOL rich, struct bridge_object *obj,
                        struct focus_strings *str);
BOOL has_edit_text(const WCHAR *cls);

/* labels.c */
BOOL needs_label(unsigned int role);
void find_label(HWND hwnd, char *dst, int size);
BOOL describe_label(HWND hwnd, char *dst, int size, struct bridge_object *obj);
void describe_labels(HWND top, struct bridge_focus *focus, struct focus_strings *str);

/* listview.c */
BOOL get_listview_header(HWND hwnd, struct remote *r, int column, WCHAR *buf);
int listview_columns(HWND hwnd, struct remote *r, int *order);
int listview_focus(HWND hwnd);
void get_listview_info(HWND hwnd, struct remote *r, struct listview_info *info);
BOOL describe_listview_item_in(HWND hwnd, struct remote *r, struct listview_info *info,
                               int item, struct bridge_object *obj,
                               struct focus_strings *str);
BOOL describe_listview_item(HWND hwnd, int item, struct bridge_object *obj,
                            struct focus_strings *str);

/* tab.c */
int tab_focus(HWND hwnd);
BOOL describe_tab_item_in(HWND hwnd, struct remote *r, int item, struct bridge_object *obj,
                          struct focus_strings *str);
BOOL describe_tab_item(HWND hwnd, int item, struct bridge_object *obj,
                       struct focus_strings *str);

/* toolbar.c */
BOOL is_toolbar(const WCHAR *cls);
int describe_toolbar_button_in(HWND hwnd, struct remote *r, int index,
                               struct bridge_object *obj, struct focus_strings *str);
int describe_toolbar_button(HWND hwnd, int index, struct bridge_object *obj,
                            struct focus_strings *str);
int toolbar_focus(HWND hwnd);

/* listbox.c */
void describe_list_box(HWND hwnd, BOOL listview, struct bridge_object *obj,
                       struct focus_strings *str);
void describe_container(HWND hwnd, struct bridge_object *obj, struct focus_strings *str);
int describe_listbox_item(HWND hwnd, int item, struct bridge_object *obj,
                          struct focus_strings *str);

/* treeview.c */
HTREEITEM tree_next(HWND hwnd, UINT flag, HTREEITEM item);
int tree_focus(HWND hwnd, HTREEITEM *item);
LONG tree_child_id(HTREEITEM item);
BOOL describe_treeview_item_in(HWND hwnd, struct remote *r, HTREEITEM item,
                               struct tree_cache *cache, struct bridge_object *obj,
                               struct focus_strings *str);
BOOL describe_treeview_item(HWND hwnd, HTREEITEM item, struct bridge_object *obj,
                            struct focus_strings *str);

/* console.c */
extern struct console_client_state console_client;
BOOL is_console(HWND hwnd);
BOOL describe_console(HWND hwnd, struct bridge_object *obj, struct focus_strings *str);
int console_text_extents(HWND hwnd, const unsigned int *chars, int count,
                         int start, int end, int *rect);

/* statusbar.c */
extern struct status_state last_status;
void report_status_bar(HWND top);

/* wintree.c */
extern HWND tree_top;
extern UINT_PTR tree_timer;
void report_tree(HWND top, HWND control, BOOL items);
void CALLBACK refresh_tree(HWND unused, UINT msg, UINT_PTR id, DWORD time);
void schedule_tree(HWND hwnd);
void schedule_items(HWND hwnd);

/* events.c */
extern DWORD main_thread_id;
extern struct focus_state last_focus;
extern UINT_PTR focus_timer;
extern UINT_PTR key_timer;
extern int key_retries;
void watch_process(DWORD pid);
void on_process_exited(DWORD pid);
void on_focus(HWND hwnd, LONG id_object, LONG id_child);
BOOL check_missed_focus(void);
void CALLBACK check_after_key(HWND unused, UINT msg, UINT_PTR id, DWORD time);
void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                             LONG id_object, LONG id_child,
                             DWORD thread, DWORD time);

/* menu.c */
extern struct menu_state menu;
BOOL still_in_menu(void);
HHOOK install_menu_hook(void);

/* keys.c */
LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);

#endif
