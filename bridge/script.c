/*
 * wine-atspi bridge, Win32 core: application scripts in Lua.
 *
 * Like NVDA's app modules, fixes for one program live in a script named
 * after its executable, scripts/<exe>.lua (winefile.lua for
 * winefile.exe), looked up in the folder of wine-atspi.exe and in its
 * parent. Each executable gets its own Lua state, shared by all its
 * processes, created when the bridge first describes one of its objects.
 * Everything runs on the Win32 thread.
 *
 * A script sees the libraries base (without dofile and loadfile),
 * string, table, math, utf8 and coroutine, plus the table wa:
 *
 *   wa.onDescribe(fn)    fn(obj) is called for each object the bridge
 *                        describes; it may change obj.name, obj.value,
 *                        obj.role, obj.states, obj.position, obj.setSize
 *                        and obj.level. obj.hwnd and obj.child (MSAA child
 *                        id, 0 for the window itself) say what it is.
 *                        Roles are AT-SPI role names ("list item"),
 *                        states a set of names (obj.states.selected).
 *   wa.className(hwnd)   window class, or nil
 *   wa.parent(hwnd)      parent window, or nil
 *   wa.sendMessage(hwnd, msg, wparam, lparam [, timeout_ms])
 *                        result as an integer, nil if the target did not
 *                        answer; only integer arguments, pointers would
 *                        point into the bridge
 *   wa.pointerSize(hwnd) 4 for a 32-bit target, 8 otherwise
 *   wa.readMemory(hwnd, address, size)
 *                        bytes of the target as a string, for
 *                        string.unpack; nil if not readable
 *   wa.readWideString(hwnd, address, max_chars)
 *                        UTF-16 text of the target up to its NUL, as UTF-8
 *   wa.log(...)          writes to the trace when WINE_ATSPI_DEBUG is set
 *
 * print also goes to the trace. Errors of a script go to stderr or the
 * --log file always. A call that runs too long is stopped with an error.
 *
 * bridge/luauser.h replaces va_list in this file (see there), so nothing
 * here may pass a va_list to Win32.
 */
#include "core.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Executables with or without a script, and processes seen recently. */
#define MAX_SCRIPTS 64
#define MAX_PIDS 64
#define SCRIPT_MAX_BYTES (1024 * 1024)
/* Lua VM instructions one call may run before it is stopped. */
#define SCRIPT_STEPS 10000000
/* Largest block wa.readMemory reads. */
#define READ_MAX 65536

struct script
{
    char app[MAX_PATH * 3];
    lua_State *L;           /* NULL: the executable has no script */
};

static struct script scripts[MAX_SCRIPTS];
static int script_count;

static struct
{
    DWORD pid;
    struct script *script;
} pids[MAX_PIDS];
static int pid_next;

/* Registry key of the list of onDescribe functions. */
static const char describe_key[] = "wine-atspi.describe";

static const struct
{
    unsigned int role;
    const char *name;
}
role_names[] =
{
    { BRIDGE_ROLE_CHECK_BOX, "check box" },
    { BRIDGE_ROLE_CHECK_MENU_ITEM, "check menu item" },
    { BRIDGE_ROLE_COLUMN_HEADER, "column header" },
    { BRIDGE_ROLE_COMBO_BOX, "combo box" },
    { BRIDGE_ROLE_DIALOG, "dialog" },
    { BRIDGE_ROLE_FILLER, "filler" },
    { BRIDGE_ROLE_FRAME, "frame" },
    { BRIDGE_ROLE_IMAGE, "image" },
    { BRIDGE_ROLE_LABEL, "label" },
    { BRIDGE_ROLE_LIST, "list" },
    { BRIDGE_ROLE_LIST_ITEM, "list item" },
    { BRIDGE_ROLE_MENU, "menu" },
    { BRIDGE_ROLE_MENU_BAR, "menu bar" },
    { BRIDGE_ROLE_MENU_ITEM, "menu item" },
    { BRIDGE_ROLE_PAGE_TAB, "page tab" },
    { BRIDGE_ROLE_PAGE_TAB_LIST, "page tab list" },
    { BRIDGE_ROLE_PANEL, "panel" },
    { BRIDGE_ROLE_PASSWORD_TEXT, "password text" },
    { BRIDGE_ROLE_POPUP_MENU, "popup menu" },
    { BRIDGE_ROLE_PROGRESS_BAR, "progress bar" },
    { BRIDGE_ROLE_PUSH_BUTTON, "push button" },
    { BRIDGE_ROLE_RADIO_BUTTON, "radio button" },
    { BRIDGE_ROLE_RADIO_MENU_ITEM, "radio menu item" },
    { BRIDGE_ROLE_SCROLL_BAR, "scroll bar" },
    { BRIDGE_ROLE_SEPARATOR, "separator" },
    { BRIDGE_ROLE_SLIDER, "slider" },
    { BRIDGE_ROLE_SPIN_BUTTON, "spin button" },
    { BRIDGE_ROLE_STATUS_BAR, "status bar" },
    { BRIDGE_ROLE_TABLE, "table" },
    { BRIDGE_ROLE_TABLE_CELL, "table cell" },
    { BRIDGE_ROLE_TERMINAL, "terminal" },
    { BRIDGE_ROLE_TEXT, "text" },
    { BRIDGE_ROLE_TOGGLE_BUTTON, "toggle button" },
    { BRIDGE_ROLE_TOOL_BAR, "tool bar" },
    { BRIDGE_ROLE_TREE, "tree" },
    { BRIDGE_ROLE_UNKNOWN, "unknown" },
    { BRIDGE_ROLE_WINDOW, "window" },
    { BRIDGE_ROLE_APPLICATION, "application" },
    { BRIDGE_ROLE_ENTRY, "entry" },
    { BRIDGE_ROLE_LINK, "link" },
    { BRIDGE_ROLE_TABLE_ROW, "table row" },
    { BRIDGE_ROLE_TREE_ITEM, "tree item" },
    { BRIDGE_ROLE_LIST_BOX, "list box" },
    { BRIDGE_ROLE_GROUPING, "grouping" },
};

static const struct
{
    unsigned int state;
    const char *name;
}
state_names[] =
{
    { BRIDGE_STATE_ACTIVE, "active" },
    { BRIDGE_STATE_CHECKED, "checked" },
    { BRIDGE_STATE_COLLAPSED, "collapsed" },
    { BRIDGE_STATE_EDITABLE, "editable" },
    { BRIDGE_STATE_ENABLED, "enabled" },
    { BRIDGE_STATE_EXPANDABLE, "expandable" },
    { BRIDGE_STATE_EXPANDED, "expanded" },
    { BRIDGE_STATE_FOCUSABLE, "focusable" },
    { BRIDGE_STATE_FOCUSED, "focused" },
    { BRIDGE_STATE_HORIZONTAL, "horizontal" },
    { BRIDGE_STATE_MULTI_LINE, "multi_line" },
    { BRIDGE_STATE_MULTISELECTABLE, "multiselectable" },
    { BRIDGE_STATE_PRESSED, "pressed" },
    { BRIDGE_STATE_SELECTABLE, "selectable" },
    { BRIDGE_STATE_SELECTED, "selected" },
    { BRIDGE_STATE_SENSITIVE, "sensitive" },
    { BRIDGE_STATE_SHOWING, "showing" },
    { BRIDGE_STATE_SINGLE_LINE, "single_line" },
    { BRIDGE_STATE_VERTICAL, "vertical" },
    { BRIDGE_STATE_VISIBLE, "visible" },
    { BRIDGE_STATE_INDETERMINATE, "indeterminate" },
    { BRIDGE_STATE_READ_ONLY, "read_only" },
};

/* Writes one line to stderr or the --log file, also without debugging. */
static void write_line(const char *a, const char *b, const char *c)
{
    DWORD written;

    if (!trace_handle || trace_handle == INVALID_HANDLE_VALUE)
        return;
    WriteFile(trace_handle, a, lstrlenA(a), &written, NULL);
    WriteFile(trace_handle, b, lstrlenA(b), &written, NULL);
    WriteFile(trace_handle, c, lstrlenA(c), &written, NULL);
    WriteFile(trace_handle, "\n", 1, &written, NULL);
}

/* Reports the error on top of the stack and pops it. */
static void report_error(lua_State *L, const char *app)
{
    const char *msg = lua_tostring(L, -1);

    write_line("wine-atspi: script ", app, "");
    write_line("  ", msg ? msg : "(error object is not a string)", "");
    lua_pop(L, 1);
}

static void count_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "script ran too long");
}

/* lua_pcall with the step limit; on error reports it. TRUE if it worked. */
static BOOL call(lua_State *L, int nargs, int nresults, const char *app)
{
    int status;

    lua_sethook(L, count_hook, LUA_MASKCOUNT, SCRIPT_STEPS);
    status = lua_pcall(L, nargs, nresults, 0);
    lua_sethook(L, NULL, 0, 0);
    if (status == LUA_OK)
        return TRUE;
    report_error(L, app);
    return FALSE;
}

static HWND check_hwnd(lua_State *L, int arg)
{
    return (HWND)(ULONG_PTR)luaL_checkinteger(L, arg);
}

/* Opens the process of window hwnd for reading its memory. */
static HANDLE open_target(HWND hwnd)
{
    DWORD pid = 0;

    if (!GetWindowThreadProcessId(hwnd, &pid))
        return NULL;
    return OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

static int wa_class_name(lua_State *L)
{
    WCHAR cls[256];
    char utf8[256 * 3];

    if (!GetClassNameW(check_hwnd(L, 1), cls, ARRAYSIZE(cls)))
        return 0;
    to_utf8(cls, utf8, sizeof(utf8));
    lua_pushstring(L, utf8);
    return 1;
}

static int wa_parent(lua_State *L)
{
    HWND parent = GetParent(check_hwnd(L, 1));

    if (!parent)
        return 0;
    lua_pushinteger(L, (lua_Integer)(ULONG_PTR)parent);
    return 1;
}

static int wa_send_message(lua_State *L)
{
    HWND hwnd = check_hwnd(L, 1);
    UINT msg = (UINT)luaL_checkinteger(L, 2);
    WPARAM wparam = (WPARAM)luaL_checkinteger(L, 3);
    LPARAM lparam = (LPARAM)luaL_checkinteger(L, 4);
    lua_Integer timeout = luaL_optinteger(L, 5, SEND_TIMEOUT_MS);
    DWORD_PTR result = 0;

    if (timeout < 0 || timeout > SEND_TIMEOUT_MS)
        timeout = SEND_TIMEOUT_MS;
    if (!send_message(hwnd, msg, wparam, lparam, (UINT)timeout, &result))
        return 0;
    lua_pushinteger(L, (lua_Integer)(LONG_PTR)result);
    return 1;
}

static int wa_pointer_size(lua_State *L)
{
    HANDLE process = open_target(check_hwnd(L, 1));
    BOOL wow64 = FALSE;

    if (process)
    {
        IsWow64Process(process, &wow64);
        CloseHandle(process);
    }
    lua_pushinteger(L, wow64 ? 4 : (lua_Integer)sizeof(void *));
    return 1;
}

static int wa_read_memory(lua_State *L)
{
    HWND hwnd = check_hwnd(L, 1);
    ULONG_PTR address = (ULONG_PTR)luaL_checkinteger(L, 2);
    lua_Integer size = luaL_checkinteger(L, 3);
    HANDLE process;
    SIZE_T done = 0;
    char *buf;
    BOOL ok;

    luaL_argcheck(L, size > 0 && size <= READ_MAX, 3, "size out of range");
    if (!address || !(process = open_target(hwnd)))
        return 0;
    buf = HeapAlloc(GetProcessHeap(), 0, size);
    ok = buf && ReadProcessMemory(process, (void *)address, buf, size, &done) &&
         done == (SIZE_T)size;
    CloseHandle(process);
    if (ok)
        lua_pushlstring(L, buf, size);
    HeapFree(GetProcessHeap(), 0, buf);
    return ok ? 1 : 0;
}

static int wa_read_wide_string(lua_State *L)
{
    HWND hwnd = check_hwnd(L, 1);
    ULONG_PTR address = (ULONG_PTR)luaL_checkinteger(L, 2);
    lua_Integer chars = luaL_checkinteger(L, 3);
    HANDLE process;
    SIZE_T done = 0;
    WCHAR *buf;
    char *utf8;
    BOOL ok;
    int len;

    luaL_argcheck(L, chars > 0 && chars <= READ_MAX / 2, 3, "length out of range");
    if (!address || !(process = open_target(hwnd)))
        return 0;
    buf = HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(WCHAR));
    ok = buf && ReadProcessMemory(process, (void *)address, buf, chars * sizeof(WCHAR), &done) &&
         done == (SIZE_T)chars * sizeof(WCHAR);
    CloseHandle(process);
    if (ok)
    {
        buf[chars] = 0;
        len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
        if (len > 0 && (utf8 = HeapAlloc(GetProcessHeap(), 0, len)))
        {
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, len, NULL, NULL);
            lua_pushstring(L, utf8);
            HeapFree(GetProcessHeap(), 0, utf8);
        }
        else
            ok = FALSE;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok ? 1 : 0;
}

/* wa.log and print: the arguments joined by spaces, to the trace. */
static int wa_log(lua_State *L)
{
    luaL_Buffer b;
    int i, n = lua_gettop(L);
    const char *app;

    if (!debug)
        return 0;
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++)
    {
        if (i > 1)
            luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    app = lua_tostring(L, lua_upvalueindex(1));
    write_line(app, ": ", lua_tostring(L, -1));
    return 0;
}

static int wa_on_describe(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, describe_key);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    return 0;
}

/* Reads the script of app into a new Lua state; NULL if there is none. */
static lua_State *load_script(const char *app)
{
    static const luaL_Reg libs[] =
    {
        { LUA_GNAME, luaopen_base },
        { LUA_COLIBNAME, luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { NULL, NULL },
    };
    static const luaL_Reg wa[] =
    {
        { "onDescribe", wa_on_describe },
        { "className", wa_class_name },
        { "parent", wa_parent },
        { "sendMessage", wa_send_message },
        { "pointerSize", wa_pointer_size },
        { "readMemory", wa_read_memory },
        { "readWideString", wa_read_wide_string },
        { NULL, NULL },
    };
    static const WCHAR *const dirs[] = { L"\\scripts\\", L"\\..\\scripts\\" };
    WCHAR dir[MAX_PATH], path[MAX_PATH * 2], name[MAX_PATH], *slash;
    char chunk[MAX_PATH * 3 + 8];
    HANDLE file = INVALID_HANDLE_VALUE;
    const luaL_Reg *lib;
    DWORD size, done = 0;
    lua_State *L;
    char *buf;
    int i;
    BOOL ok;

    if (!MultiByteToWideChar(CP_UTF8, 0, app, -1, name, ARRAYSIZE(name)) ||
        !GetModuleFileNameW(NULL, dir, ARRAYSIZE(dir)) || !(slash = wcsrchr(dir, '\\')))
        return NULL;
    *slash = 0;
    for (i = 0; i < ARRAYSIZE(dirs) && file == INVALID_HANDLE_VALUE; i++)
    {
        lstrcpyW(path, dir);
        lstrcatW(path, dirs[i]);
        lstrcatW(path, name);
        lstrcatW(path, L".lua");
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (file == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(file, NULL);
    buf = size <= SCRIPT_MAX_BYTES ? HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
    ok = buf && ReadFile(file, buf, size, &done, NULL) && done == size;
    CloseHandle(file);
    if (!ok || !(L = luaL_newstate()))
    {
        HeapFree(GetProcessHeap(), 0, buf);
        write_line("wine-atspi: cannot read the script of ", app, "");
        return NULL;
    }
    trace(L"script %s", path);

    for (lib = libs; lib->func; lib++)
    {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    /* No files: these would open them with the CRT. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushstring(L, app);
    lua_pushcclosure(L, wa_log, 1);
    lua_setglobal(L, "print");

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, describe_key);
    luaL_newlib(L, wa);
    lua_pushstring(L, app);
    lua_pushcclosure(L, wa_log, 1);
    lua_setfield(L, -2, "log");
    lua_setglobal(L, "wa");

    lstrcpyA(chunk, "@");
    lstrcatA(chunk, app);
    lstrcatA(chunk, ".lua");
    if (luaL_loadbufferx(L, buf, size, chunk, "t") != LUA_OK)
    {
        report_error(L, app);
        ok = FALSE;
    }
    else
        ok = call(L, 0, 0, app);
    HeapFree(GetProcessHeap(), 0, buf);
    if (!ok)
    {
        lua_close(L);
        return NULL;
    }
    return L;
}

/* The script of the process that owns hwnd; NULL if it has none. */
static struct script *script_of(HWND hwnd)
{
    char app[MAX_PATH * 3];
    struct script *s = NULL;
    DWORD pid = 0;
    int i;

    if (!GetWindowThreadProcessId(hwnd, &pid))
        return NULL;
    for (i = 0; i < MAX_PIDS; i++)
        if (pids[i].pid == pid)
            return pids[i].script->L ? pids[i].script : NULL;
    get_app_name(pid, app, sizeof(app));
    for (i = 0; i < script_count && !s; i++)
        if (!lstrcmpiA(scripts[i].app, app))
            s = &scripts[i];
    if (!s)
    {
        if (script_count == MAX_SCRIPTS)
            return NULL;
        s = &scripts[script_count++];
        lstrcpynA(s->app, app, sizeof(s->app));
        s->L = load_script(app);
    }
    pids[pid_next].pid = pid;
    pids[pid_next].script = s;
    pid_next = (pid_next + 1) % MAX_PIDS;
    return s->L ? s : NULL;
}

void script_forget_process(DWORD pid)
{
    int i;

    for (i = 0; i < MAX_PIDS; i++)
        if (pids[i].pid == pid)
            pids[i].pid = 0;
}

static void set_string_field(lua_State *L, const char *key, const char *value)
{
    if (!value)
        return;
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}

static void set_integer_field(lua_State *L, const char *key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

/* Pushes obj as a Lua table. */
static void push_object(lua_State *L, const struct bridge_object *obj)
{
    int i;

    lua_createtable(L, 0, 10);
    set_integer_field(L, "hwnd", (lua_Integer)obj->hwnd);
    set_integer_field(L, "child", obj->child);
    for (i = 0; i < ARRAYSIZE(role_names); i++)
        if (role_names[i].role == obj->role)
            set_string_field(L, "role", role_names[i].name);
    set_string_field(L, "name", obj->name ? obj->name : "");
    set_string_field(L, "value", obj->value);
    set_integer_field(L, "position", obj->position);
    set_integer_field(L, "setSize", obj->set_size);
    set_integer_field(L, "level", obj->level);
    lua_createtable(L, 0, 8);
    for (i = 0; i < ARRAYSIZE(state_names); i++)
        if (obj->states & BRIDGE_STATE_BIT(state_names[i].state))
        {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, state_names[i].name);
        }
    lua_setfield(L, -2, "states");
}

/*
 * A string field of the table on top into buf if it is a string other
 * than old; TRUE if copied.
 */
static BOOL get_string_field(lua_State *L, const char *key, const char *old,
                             char *buf, int size)
{
    BOOL ok = lua_getfield(L, -1, key) == LUA_TSTRING &&
              lstrcmpA(lua_tostring(L, -1), old ? old : "");

    if (ok)
        lstrcpynA(buf, lua_tostring(L, -1), size);
    lua_pop(L, 1);
    return ok;
}

static void get_integer_field(lua_State *L, const char *key, int *value)
{
    int isnum = 0;
    lua_Integer n;

    lua_getfield(L, -1, key);
    n = lua_tointegerx(L, -1, &isnum);
    if (isnum && n >= 0 && n <= 0x7fffffff)
        *value = (int)n;
    lua_pop(L, 1);
}

/* Takes the fields a script may change from the table on top into obj. */
static void pull_object(lua_State *L, struct bridge_object *obj, struct focus_strings *str)
{
    char role[32];
    int i;

    if (get_string_field(L, "name", obj->name, str->object, sizeof(str->object)))
        obj->name = str->object;
    if (get_string_field(L, "value", obj->value, str->value, sizeof(str->value)))
        obj->value = str->value;
    if (get_string_field(L, "role", "", role, sizeof(role)))
        for (i = 0; i < ARRAYSIZE(role_names); i++)
            if (!lstrcmpA(role_names[i].name, role))
                obj->role = role_names[i].role;
    get_integer_field(L, "position", &obj->position);
    get_integer_field(L, "setSize", &obj->set_size);
    get_integer_field(L, "level", &obj->level);
    if (lua_getfield(L, -1, "states") == LUA_TTABLE)
        for (i = 0; i < ARRAYSIZE(state_names); i++)
        {
            lua_getfield(L, -1, state_names[i].name);
            if (lua_toboolean(L, -1))
                obj->states |= BRIDGE_STATE_BIT(state_names[i].state);
            else
                obj->states &= ~BRIDGE_STATE_BIT(state_names[i].state);
            lua_pop(L, 1);
        }
    lua_pop(L, 1);
}

/*
 * Lets the script of the object's program change obj before it is
 * reported. A changed name or value is kept in str->object or str->value.
 */
void script_describe(struct bridge_object *obj, struct focus_strings *str)
{
    struct script *s = script_of((HWND)(ULONG_PTR)obj->hwnd);
    lua_State *L;
    lua_Integer i, n;

    if (!s)
        return;
    L = s->L;
    lua_getfield(L, LUA_REGISTRYINDEX, describe_key);
    n = lua_rawlen(L, -1);
    if (!n)
    {
        lua_pop(L, 1);
        return;
    }
    push_object(L, obj);
    for (i = 1; i <= n; i++)
    {
        lua_rawgeti(L, -2, i);
        lua_pushvalue(L, -2);
        call(L, 1, 0, s->app);
    }
    pull_object(L, obj, str);
    lua_pop(L, 2);
}
