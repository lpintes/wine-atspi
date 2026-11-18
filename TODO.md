# TODO

Planned work in the order it should be done. Verified facts go to
`CLAUDE.md`, not here; delete an entry once it is done.

## 1. Application scripts

See "Application scripts" in `CLAUDE.md` and the API in
`bridge/script.c`.

- Scintilla (milestone 5) comes after the scripting, so that
  program-specific parts can live in scripts.

## 2. Smaller gaps

Window tree (object navigation, flat review):

- `Selection` for trees; tables and list boxes have it. Orca counts the
  selectable items of a tree from its `tree item` descendants, and the
  bridge knows only the visible ones.
- `Action` for other objects than Win32 buttons (list items, tabs, combo
  boxes, toolbar buttons), if Orca's mouse click fallback is not good
  enough.
- On a new top-level window the tree is posted synchronously before the
  focus (up to about 80 ms delay before speech). Each child window costs
  about 6 ms to describe, mostly cross-process messages, not MSAA
  (measured 2026-11-16: skipping MSAA saved 190 -> 182 ms for 35 objects).
- A focused list without a focused item is described by MSAA in the
  focus but by `describe_container` in the tree, so its name or role may
  flip on each refresh. Not seen.

Toolbars:

- A toolbar that is neither flat nor themed gives no hot button
  (`TB_GETHOTITEM` -1); its focus stays on the toolbar itself.
- A key gives a toolbar no items-only refresh (`schedule_items` would
  remove its child windows in `tree_idle`), so a state changed from the
  keyboard waits for the next whole tree.
- Tooltip names need the hook DLL, so 32-bit targets get none.

Controls:

- `Edit`: lines are logical (no soft wrap), the whole text is re-read
  after each key, no `SetCaretOffset`.
- RichEdit: text attributes (`EM_GETCHARFORMAT`).
- Tree view: check boxes not read; removed items stay as nodes.
- Tabs and list items: an item left behind keeps "selected"; Ctrl+Tab
  inside a property sheet page is not followed; `TCS_BUTTONS` untested.
- 32-bit layouts (`LVITEMW`, `TVITEMW`, `TCITEMW`) untested.
- Console: a full buffer (150 rows) shifts lines up, so the diff gives
  one huge delete + insert; wide (CJK) cells untested.
- Menus: the `Edit` context menu (Shift+F10) is silent until the first
  arrow (`TPM_NONOTIFY`, nothing highlighted).

Bridge:

- Method calls get the node pointer from `subtree_dispatch`; a node freed
  in between would dangle. Not seen. Idea: pass `app` and look the node
  up by path in `method_call`. Unverified assumption: today GDBus method
  idles outrank the bridge's idle sources, which is the only protection.

## 3. Later

- Menus in 32-bit programs: a 32-bit hook DLL and a 32-bit process that
  installs it.
- Move key-check heuristics into the hook DLL ("Move heuristics into the
  hook DLL" in `CLAUDE.md`).
- UIA provider experiment (open question in `CLAUDE.md`).
- Off-screen model, when a needed program requires it.
