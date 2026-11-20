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
- A focused list without a focused item is described by MSAA in the
  focus but by `describe_container` in the tree, so its name or role may
  flip on each refresh. Not seen.

Toolbars:

- A toolbar that is neither flat nor themed gives no hot button
  (`TB_GETHOTITEM` -1); its focus stays on the toolbar itself.
- Tooltip names need the hook DLL, so 32-bit targets get none.

Controls:

- `Edit` / RichEdit: large texts (books). Only the change crosses to
  the core and on to the AT-SPI side, but each key still costs about
  10-20 ms per MB (hook reads and compares the whole text in the
  target, `char_offset` of the caret, a copy of the code points on a
  change; `tests/perf-text.sh`; 10 KB 1 ms). 32-bit targets (no hook)
  are still cut at `TEXT_MAX_CHARS`.
- RichEdit: text attributes (`EM_GETCHARFORMAT`).
- 32-bit targets: a cross-process message costs about twice as much
  (tree of `tests/controls.c`: 162 messages, 420-530 ms against 210-250
  ms for the 64-bit build). Not looked into; measure where the time goes
  (WoW64 message thunks, `VirtualAllocEx` / `ReadProcessMemory`).
- Console: a full buffer (150 rows) shifts lines up, so the diff gives
  one huge delete + insert; wide (CJK) cells untested.

## 3. Later

- Menus in 32-bit programs: a 32-bit hook DLL and a 32-bit process that
  installs it.
- Move key-check heuristics into the hook DLL ("Move heuristics into the
  hook DLL" in `CLAUDE.md`).
- UIA provider experiment (open question in `CLAUDE.md`).
- Off-screen model, when a needed program requires it.
