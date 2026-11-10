# TODO

Planned work in the order it should be done. Verified facts go to
`CLAUDE.md`, not here; delete an entry once it is done.

## 1. Whole window tree: object navigation and flat review

The bridge now exposes the focused object, its containers and a few
extras (labels, status bar, progress bars, menus), not every control of
a window. Orca 51.1 has two review modes that need the whole tree:

- Object navigator (`object_navigator.py`): moves from the locus of
  focus to the parent, first child and siblings through
  `Accessible.GetParent` / `GetChildAtIndex` and presents each object
  like a focus (`present_object`). With "simplify" on it skips
  layout-only objects. Its activate command tries the object's
  `Action` (`try_all_clickable_actions`), else clicks at the centre of
  `Component.GetExtents` (`click_object`).
- Flat review (`flat_review.py`): takes the on-screen objects below the
  top-level window (`_get_on_screen_objects` in `ax_utilities.py`):
  objects that are showing and inside the window rect, an object with a
  name, description or text is a zone, a table with `Selection` gives its
  visible cells, a closed combo box or menu is a leaf. Zones are sorted
  into lines by their extents. Text objects are split into visible lines
  (`get_visible_lines` in `ax_utilities_text.py`), which needs text
  extents (`Text.GetCharacterExtents` / `GetRangeExtents`).

Implementation, as far as seen now:

- Build the tree of a top-level window from `EnumChildWindows`
  (`GetParent` order), describing each child window with the existing
  handlers; hidden windows get no `showing` / `visible`.
- Keep it fresh with the WinEvents Wine does emit (create, destroy,
  show, hide, location change, name change) and emit
  `children-changed`.
- `Component.GetExtents` for every object, also items, cells and tabs
  (`LVM_GETITEMRECT`, `TVM_GETITEMRECT`, `TCM_GETITEMRECT`,
  `LB_GETITEMRECT`; check what this changes for Orca's progress bar
  rule, see `CLAUDE.md`).
- `Selection` for list boxes and trees, as GTK gives it; tables have it.
- `Action` for other objects than buttons (list items, tabs, combo
  boxes), if Orca's mouse click fallback is not good enough.
- Check first in the Orca source what the text side of flat review needs
  exactly, and what Orca does with an object outside the focus's
  application.

Toolbars (step 2) are mostly reachable only this way.

## 2. Toolbars (`ToolbarWindow32`)

The buttons are in the window tree with name, role, states and rect,
and a focused toolbar reports its hot button (see "Toolbars" in
`CLAUDE.md`). Left:

- A toolbar that is neither flat nor themed gives no hot button
  (`TB_GETHOTITEM` -1); its focus stays on the toolbar itself.
- A key gives a toolbar no items-only refresh (`schedule_items` would
  remove its child windows in `tree_idle`), so a state changed from the
  keyboard waits for the next whole tree.
- Tooltip names need the hook DLL, so 32-bit targets get none.
- Action "click" for toolbar buttons, if Orca's mouse click fallback is
  not good enough.

## 3. Smaller gaps

- `Edit`: lines are logical (no soft wrap), the whole text is re-read
  after each key, no `SetCaretOffset`.
- RichEdit: text attributes (`EM_GETCHARFORMAT`).
- Tree view: check boxes not read; removed items stay as nodes.
- Tabs and list items: an item left behind keeps "selected"; Ctrl+Tab
  inside a property sheet page is not followed; `TCS_BUTTONS` untested.
- A `Static` whose text changes without a focus change (progress
  dialogs) is not followed; labels of nested child dialogs hang below the
  frame.
- 32-bit layouts (`LVITEMW`, `TVITEMW`, `TCITEMW`) untested.
- Console: a full buffer (150 rows) shifts lines up, so the diff gives
  one huge delete + insert; wide (CJK) cells untested.
- Menus: the `Edit` context menu (Shift+F10) is silent until the first
  arrow (`TPM_NONOTIFY`, nothing highlighted).
- While the bridge waits in `SendMessageTimeout` (up to 500 ms) its LL
  keyboard hook is not served.
- Method calls get the node pointer from `subtree_dispatch`; a node freed
  in between would dangle. Not seen.

## 4. Application scripts

See "Application modules as scripts" in `CLAUDE.md` (Lua 5.4 in the
Win32 core).

- Move the winefile fix (`winefile_entry_name`) out of the C code into
  the first script. Then add level, expanded, size and date for its
  panes there.
- Scintilla (milestone 5) comes after the scripting, so that
  program-specific parts can live in scripts.

## 5. Later

- Menus in 32-bit programs: a 32-bit hook DLL and a 32-bit process that
  installs it.
- Move key-check heuristics into the hook DLL ("Move heuristics into the
  hook DLL" in `CLAUDE.md`).
- UIA provider experiment (open question in `CLAUDE.md`).
- Off-screen model, when a needed program requires it.
