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

- `Edit` / RichEdit: large texts (books) are slow. The hook DLL hands
  over only the changed range (`hook_text`), but the core still strips
  `\r\n`, converts to UTF-8 and counts characters over the whole text,
  and the AT-SPI side converts and diffs it all (about 25 ms per MB per
  key and caret move, `tests/perf-text.sh`). Next: the core keeps its
  UTF-16 copy and sends the AT-SPI side the change (start, deleted,
  inserted) instead of the whole text; keep the remote block per
  window, since small texts got slower. 32-bit targets (no hook) are
  still cut at `TEXT_MAX_CHARS`.
- RichEdit: text attributes (`EM_GETCHARFORMAT`).
- Tabs: `TCS_BUTTONS` untested.
- 32-bit targets: a cross-process message costs about twice as much
  (tree of `tests/controls.c`: 162 messages, 420-530 ms against 210-250
  ms for the 64-bit build). Not looked into; measure where the time goes
  (WoW64 message thunks, `VirtualAllocEx` / `ReadProcessMemory`).
- Console: a full buffer (150 rows) shifts lines up, so the diff gives
  one huge delete + insert; wide (CJK) cells untested.
- Menus: the `Edit` context menu (Shift+F10) is silent until the first
  arrow (`TPM_NONOTIFY`, nothing highlighted).

## 3. Later

- Menus in 32-bit programs: a 32-bit hook DLL and a 32-bit process that
  installs it.
- Move key-check heuristics into the hook DLL ("Move heuristics into the
  hook DLL" in `CLAUDE.md`).
- UIA provider experiment (open question in `CLAUDE.md`).
- Off-screen model, when a needed program requires it.
