-- Wine's winefile (programs/winefile/winefile.c).
--
-- Both panes, the folder tree and the file list, are owner-drawn list
-- boxes without LBS_HASSTRINGS, children of a WFS_Tree window
-- (create_tree_window), so LB_GETTEXT gives nothing. The item data is a
-- pointer to winefile's Entry in the target: three pointers, three ints,
-- then the WIN32_FIND_DATAW whose cFileName is the text it draws
-- (draw_item). The layout comes from Wine's source; re-check it after Wine
-- upgrades.

local LB_GETITEMDATA = 0x0199
local LB_ERR = -1
local MAX_PATH = 260
-- offsetof(WIN32_FIND_DATAW, cFileName): attributes, three FILETIMEs,
-- size high and low, two reserved DWORDs.
local CFILENAME = 4 + 3 * 8 + 4 * 4

wa.onDescribe(function(obj)
  if obj.role ~= 'list item' or obj.name ~= '' then
    return
  end
  local parent = wa.parent(obj.hwnd)
  if not parent or wa.className(parent) ~= 'WFS_Tree' then
    return
  end
  local entry = wa.sendMessage(obj.hwnd, LB_GETITEMDATA, obj.child - 1, 0)
  if not entry or entry == 0 or entry == LB_ERR then
    return
  end
  local offset = 3 * wa.pointerSize(obj.hwnd) + 3 * 4 + CFILENAME
  local name = wa.readWideString(obj.hwnd, entry + offset, MAX_PATH)
  if name then
    obj.name = name
  end
end)
