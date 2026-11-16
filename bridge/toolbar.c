/*
 * wine-atspi bridge, Win32 core: toolbar (ToolbarWindow32) buttons and
 * their tooltips.
 */
#include "core.h"

/* TBBUTTONINFOW of a 32-bit target. */
struct tbbuttoninfo32
{
    UINT cbSize;
    DWORD dwMask;
    int idCommand;
    int iImage;
    BYTE fsState;
    BYTE fsStyle;
    WORD cx;
    ULONG lParam;
    ULONG pszText;
    int cchText;
};

/*
 * Tooltip of a button without text, the only name most icon buttons have.
 * The toolbar gets it from its owner by WM_NOTIFY, which Wine refuses
 * across processes, so the hook DLL asks inside the target
 * (TOOLTIP_MESSAGE in menuhook.h). Without the hook (32-bit target) the
 * text stays empty.
 */
static void get_toolbar_tooltip(HWND hwnd, struct remote *r, int command, WCHAR *text)
{
    static UINT message;
    DWORD_PTR ret;
    SIZE_T done;

    C_ASSERT(TOOLTIP_TEXT_MAX <= CELL_TEXT_MAX);
    text[0] = 0;
    if (!message)
        message = RegisterWindowMessageW(TOOLTIP_MESSAGE);
    if (!message || !WriteProcessMemory(r->process, r->base, L"", sizeof(WCHAR), &done) ||
        !SendMessageTimeoutW(hwnd, message, command, (LPARAM)r->base, SMTO_ABORTIFHUNG, 500,
                             &ret) ||
        !ReadProcessMemory(r->process, r->base, text, TOOLTIP_TEXT_MAX * sizeof(WCHAR), &done))
        text[0] = 0;
    text[TOOLTIP_TEXT_MAX - 1] = 0;
}

/*
 * Button index of a toolbar. Wine's comctl32/toolbar.c emits no WinEvent
 * and has no oleacc proxy, so the button is read with toolbar messages;
 * all of them need memory in the target. TB_GETBUTTONINFOW gives no text
 * from the string list (TB_ADDSTRING), TB_GETBUTTONTEXTW does, but it
 * takes no buffer size, so the length is asked first. A button without
 * text is named by its tooltip (get_toolbar_tooltip). The MSAA child id
 * is index + 1, as on Windows. TRUE if described, FALSE if the toolbar
 * did not answer, -1 for a separator or hidden button.
 */
static int describe_toolbar_button_in(HWND hwnd, struct remote *r, int index,
                                      struct bridge_object *obj, struct focus_strings *str)
{
    BYTE header[REMOTE_HEADER];
    WCHAR text[CELL_TEXT_MAX];
    DWORD_PTR ret = 0, len = 0;
    int command;
    BYTE state, style;
    RECT rect;
    SIZE_T done;

    memset(header, 0, sizeof(header));
    if (r->wow64)
    {
        struct tbbuttoninfo32 *info = (struct tbbuttoninfo32 *)header;

        info->cbSize = sizeof(*info);
        info->dwMask = TBIF_BYINDEX | TBIF_COMMAND | TBIF_STATE | TBIF_STYLE;
    }
    else
    {
        TBBUTTONINFOW *info = (TBBUTTONINFOW *)header;

        info->cbSize = sizeof(*info);
        info->dwMask = TBIF_BYINDEX | TBIF_COMMAND | TBIF_STATE | TBIF_STYLE;
    }
    if (!WriteProcessMemory(r->process, r->base, header, sizeof(header), &done) ||
        !SendMessageTimeoutW(hwnd, TB_GETBUTTONINFOW, index, (LPARAM)r->base,
                             SMTO_ABORTIFHUNG, 500, &ret) || (int)ret != index ||
        !ReadProcessMemory(r->process, r->base, header, sizeof(header), &done))
        return FALSE;
    if (r->wow64)
    {
        struct tbbuttoninfo32 *info = (struct tbbuttoninfo32 *)header;

        command = info->idCommand;
        state = info->fsState;
        style = info->fsStyle;
    }
    else
    {
        TBBUTTONINFOW *info = (TBBUTTONINFOW *)header;

        command = info->idCommand;
        state = info->fsState;
        style = info->fsStyle;
    }
    if ((style & BTNS_SEP) || (state & TBSTATE_HIDDEN))
        return -1;

    text[0] = 0;
    if (SendMessageTimeoutW(hwnd, TB_GETBUTTONTEXTW, command, 0, SMTO_ABORTIFHUNG, 500, &len) &&
        (int)len > 0 && len < CELL_TEXT_MAX &&
        WriteProcessMemory(r->process, r->base, L"", sizeof(WCHAR), &done) &&
        SendMessageTimeoutW(hwnd, TB_GETBUTTONTEXTW, command, (LPARAM)r->base,
                            SMTO_ABORTIFHUNG, 500, &ret) &&
        ReadProcessMemory(r->process, r->base, text, (len + 1) * sizeof(WCHAR), &done))
        text[len] = 0;
    if (!text[0])
        get_toolbar_tooltip(hwnd, r, command, text);

    SetRectEmpty(&rect);
    obj->has_extents = FALSE;
    if (remote_rect(hwnd, r, TB_GETITEMRECT, index, &rect))
        set_item_extents(hwnd, rect, obj);
    obj->hwnd = (ULONG_PTR)hwnd;
    obj->child = index + 1;
    obj->role = (style & BTNS_CHECK) ? BRIDGE_ROLE_TOGGLE_BUTTON : BRIDGE_ROLE_PUSH_BUTTON;
    obj->value = NULL;
    to_utf8(text, str->object, sizeof(str->object));
    obj->name = str->object;
    obj->states = BRIDGE_STATE_BIT(BRIDGE_STATE_VISIBLE) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_SHOWING) |
                  BRIDGE_STATE_BIT(BRIDGE_STATE_FOCUSABLE);
    if (state & TBSTATE_ENABLED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_ENABLED) |
                       BRIDGE_STATE_BIT(BRIDGE_STATE_SENSITIVE);
    if (state & TBSTATE_CHECKED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_CHECKED);
    if (state & TBSTATE_PRESSED)
        obj->states |= BRIDGE_STATE_BIT(BRIDGE_STATE_PRESSED);
    return TRUE;
}

static int describe_toolbar_button(HWND hwnd, int index, struct bridge_object *obj,
                                   struct focus_strings *str)
{
    struct remote r;
    int ok;

    if (!remote_open(hwnd, &r))
        return FALSE;
    ok = describe_toolbar_button_in(hwnd, &r, index, obj, str);
    remote_close(&r);
    return ok;
}

/*
 * The button a focused toolbar moves with the arrow keys: WM_SETFOCUS
 * makes button 0 hot, Left/Up and Right/Down the previous or next enabled
 * one (comctl32/toolbar.c, TOOLBAR_SetRelativeHotItem); no WinEvent tells.
 * TB_GETHOTITEM answers -1 unless the toolbar is flat or themed. -1 if
 * none, LIST_BUSY if the toolbar did not answer.
 */
static int toolbar_focus_index(HWND hwnd)
{
    DWORD_PTR item = (DWORD_PTR)-1;

    if (!SendMessageTimeoutW(hwnd, TB_GETHOTITEM, 0, 0, SMTO_ABORTIFHUNG, 500, &item))
        return LIST_BUSY;
    return (int)item;
}

/*
 * The focus of a toolbar is its hot button, see toolbar_focus_index. A
 * separator or a button that could not be read leaves the toolbar itself.
 */
static int toolbar_describe_focus(HWND hwnd, LONG id_child, struct bridge_object *obj,
                                  struct focus_strings *str)
{
    int item = id_child > 0 ? id_child - 1 : toolbar_focus_index(hwnd);

    if (item == LIST_BUSY)
        return LIST_BUSY;
    return item >= 0 && describe_toolbar_button(hwnd, item, obj, str) == TRUE;
}

static LONG toolbar_focus(HWND hwnd)
{
    int item = toolbar_focus_index(hwnd);

    return item == LIST_BUSY ? LIST_BUSY : item < 0 ? 0 : item + 1;
}

static void toolbar_describe_self(HWND hwnd, struct bridge_object *obj,
                                  struct focus_strings *str)
{
    obj->role = BRIDGE_ROLE_TOOL_BAR;
}

static void add_toolbar_items(struct tree_build *b, HWND hwnd)
{
    struct bridge_object *obj;
    DWORD_PTR count = 0;
    struct remote r;
    int i, ok;

    if (!remote_open(hwnd, &r))
        return;
    if (!SendMessageTimeoutW(hwnd, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &count))
        count = 0;
    for (i = 0; i < (int)count && (obj = tree_item_slot(b)); i++)
    {
        ok = describe_toolbar_button_in(hwnd, &r, i, obj, b->str);
        tree_item_done(b, ok == TRUE);
        if (!ok)
            break;
    }
    remote_close(&r);
}

/*
 * A toolbar has no label; Wine's oleacc names it by its window text. Its
 * keys only move the hot button.
 */
const struct item_handler toolbar_handler =
{
    toolbar_describe_focus,
    toolbar_focus,
    toolbar_describe_self,
    add_toolbar_items,
    TRUE,
    FALSE,
};
