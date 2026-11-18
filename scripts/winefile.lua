-- Wine's winefile (programs/winefile/winefile.c).
--
-- Both panes, the folder tree (control id IDW_TREE_LEFT) and the file
-- list (IDW_TREE_RIGHT), are owner-drawn list boxes without
-- LBS_HASSTRINGS, children of a WFS_Tree window (create_tree_window), so
-- LB_GETTEXT gives nothing. The item data is a pointer to winefile's Entry
-- in the target; its cFileName is the text it draws (draw_item).
--
-- The folder tree becomes a tree: each item has its level, and expandable
-- and expanded states. The file list becomes a table with the columns
-- name, size and modification date, headed by the texts of its header
-- control (IDW_HEADER_RIGHT); with View > Name only it stays a list.
--
-- The layouts of Entry and Pane come from Wine's source; re-check them
-- after Wine upgrades.

local LB_GETITEMDATA = 0x0199
local LB_ERR = -1
local GWLP_USERDATA = -21
local GWLP_ID = -12
local IDW_TREE_LEFT = 3
local IDW_TREE_RIGHT = 6
local IDW_HEADER_RIGHT = 5
local MAX_PATH = 260
local FILE_ATTRIBUTE_DIRECTORY = 0x10
-- Pane.visible_cols (enum COLUMN_FLAGS) and the header items by index
-- from 1; the first one, without text, is the icon (g_pos_names).
local COL_SIZE, COL_DATE, COL_TIME = 0x01, 0x02, 0x04
local HEADER_NAME, HEADER_SIZE, HEADER_MDATE = 2, 3, 6
-- Longest sibling chain walked; winefile sorts ".", ".." and the folders
-- first (compareType), so walks stop at the first file.
local WALK_MAX = 2000

-- Entry: next, down and up, then BOOL expanded, BOOL scanned, int level,
-- then WIN32_FIND_DATAW: attributes, the creation, access and write
-- FILETIMEs, size high and low, two reserved DWORDs, cFileName.
local function entry_layout(hwnd)
  local p = wa.pointerSize(hwnd)
  local ptr = p == 4 and 'I4' or 'I8'
  return {
    format = '<' .. ptr .. ptr .. ptr .. 'i4i4i4I4I8I8I8I4I4',
    size = 3 * p + 3 * 4 + 4 + 3 * 8 + 2 * 4,
    name = 3 * p + 3 * 4 + 4 + 3 * 8 + 4 * 4,
    pointer = p,
  }
end

local function read_entry(hwnd, l, address)
  local bytes = wa.readMemory(hwnd, address, l.size)
  if not bytes then
    return nil
  end
  local next_, down, up, expanded, scanned, level, attrs,
        _, _, mtime, size_high, size_low = string.unpack(l.format, bytes)
  return {
    next = next_, down = down, up = up,
    expanded = expanded ~= 0, scanned = scanned ~= 0, level = level,
    directory = attrs & FILE_ATTRIBUTE_DIRECTORY ~= 0,
    mtime = mtime, size = (size_high << 32) | size_low,
  }
end

local function entry_name(hwnd, l, address)
  return wa.readWideString(hwnd, address + l.name, MAX_PATH)
end

local function is_dot(name)
  return name == '.' or name == '..'
end

-- The folders in the entry chain starting at first, as the tree pane
-- shows them (insert_entries leaves out "." and "..", which come first):
-- calls fn(address) for each until fn returns true.
local function each_folder(hwnd, l, first, fn)
  local address, n = first, 0
  while address ~= 0 and n < WALK_MAX do
    local e = read_entry(hwnd, l, address)
    if not e or not e.directory then
      return
    end
    if (n >= 2 or not is_dot(entry_name(hwnd, l, address))) and fn(address) then
      return
    end
    address, n = e.next, n + 1
  end
end

-- Places of the folders below a parent entry, by address, and their
-- count. Each visible item would walk its siblings again, so the result
-- is kept while the parent's first child stays the same.
local siblings = {}
local sibling_count = 0

local function siblings_of(hwnd, l, parent_address, parent)
  -- One script serves all winefile processes, so the key has the window.
  local key = hwnd .. ':' .. parent_address
  local s = siblings[key]
  if s and s.down == parent.down then
    return s
  end
  if sibling_count >= 256 then
    siblings, sibling_count = {}, 0
  end
  s = { down = parent.down, places = {}, count = 0 }
  each_folder(hwnd, l, parent.down, function(a)
    s.count = s.count + 1
    s.places[a] = s.count
  end)
  siblings[key] = s
  sibling_count = sibling_count + 1
  return s
end

local function has_folder(hwnd, l, e)
  if not e.scanned then
    return true
  end
  local found = false
  each_folder(hwnd, l, e.down, function() found = true; return true end)
  return found
end

-- Which pane hwnd is: 'tree', 'files' or nil.
local function pane_of(hwnd)
  local parent = wa.parent(hwnd)
  if not parent or wa.className(parent) ~= 'WFS_Tree' then
    return nil
  end
  local id = wa.windowLong(hwnd, GWLP_ID)
  if id == IDW_TREE_LEFT then
    return 'tree'
  elseif id == IDW_TREE_RIGHT then
    return 'files'
  end
end

-- Pane.visible_cols: two pointers, widths[10], widths_shown[10],
-- positions[11], BOOL treePane, then visible_cols.
local function visible_cols(hwnd)
  local pane = wa.windowLong(hwnd, GWLP_USERDATA)
  if not pane or pane == 0 then
    return nil
  end
  local bytes = wa.readMemory(hwnd, pane + 2 * wa.pointerSize(hwnd) + 31 * 4 + 4, 4)
  return bytes and string.unpack('<i4', bytes)
end

-- Columns of the file list as {header index, function(entry) -> text}.
local function file_columns(hwnd)
  local cols = visible_cols(hwnd)
  local list = {}
  if not cols then
    return list
  end
  if cols & COL_SIZE ~= 0 then
    list[#list + 1] = { HEADER_SIZE, function(e)
      return e.directory and '' or tostring(e.size)
    end }
  end
  if cols & (COL_DATE | COL_TIME) ~= 0 then
    list[#list + 1] = { HEADER_MDATE, function(e)
      return wa.formatFileTime(e.mtime) or ''
    end }
  end
  if #list > 0 then
    table.insert(list, 1, { HEADER_NAME, nil })
  end
  return list
end

local function describe_list(obj, pane)
  if pane == 'tree' then
    obj.role = 'tree'
    return
  end
  local cols = file_columns(obj.hwnd)
  if #cols == 0 then
    return
  end
  local header = wa.dlgItem(wa.parent(obj.hwnd), IDW_HEADER_RIGHT)
  local texts = header and wa.headerTexts(header) or {}
  local columns = {}
  for i, c in ipairs(cols) do
    columns[i] = texts[c[1]] or ''
  end
  obj.role = 'table'
  obj.columns = columns
end

local function describe_tree_item(obj, l, address, e)
  obj.role = 'tree item'
  obj.level = e.level + 1
  obj.states.expandable = has_folder(obj.hwnd, l, e)
  obj.states.expanded = obj.states.expandable and e.expanded
  obj.states.collapsed = obj.states.expandable and not e.expanded
  -- Place among the folders of the same parent; the root has none.
  obj.position, obj.setSize = 1, 1
  local parent = e.up ~= 0 and read_entry(obj.hwnd, l, e.up)
  if parent then
    local s = siblings_of(obj.hwnd, l, e.up, parent)
    if s.places[address] then
      obj.position, obj.setSize = s.places[address], s.count
    end
  end
end

local function describe_file(obj, e)
  local cols = file_columns(obj.hwnd)
  if #cols == 0 then
    return
  end
  local columns = { obj.name }
  for i = 2, #cols do
    columns[i] = cols[i][2](e)
  end
  obj.role = 'table cell'
  obj.columns = columns
end

wa.onDescribe(function(obj)
  local pane = pane_of(obj.hwnd)
  if not pane then
    return
  end
  if obj.child == 0 then
    describe_list(obj, pane)
    return
  end
  local address = wa.sendMessage(obj.hwnd, LB_GETITEMDATA, obj.child - 1, 0)
  if not address or address == 0 or address == LB_ERR then
    return
  end
  local l = entry_layout(obj.hwnd)
  local name = entry_name(obj.hwnd, l, address)
  local e = read_entry(obj.hwnd, l, address)
  if not name or not e then
    return
  end
  obj.name = name
  if pane == 'tree' then
    describe_tree_item(obj, l, address, e)
  else
    describe_file(obj, e)
  end
end)
