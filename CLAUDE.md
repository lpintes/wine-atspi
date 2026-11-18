# CLAUDE.md

## What this is

`wine-atspi` is a bridge that makes Windows programs running under Wine
accessible to Linux screen readers (Orca) through AT-SPI. It does **not**
modify Wine. It is a separate program that runs inside the same Wine
prefix as the target applications. It observes them the way a Windows
screen reader would and publishes what it learns on the AT-SPI D-Bus.

The user is blind and works with a screen reader (NVDA on Windows, Orca on
Linux). The point of the project is practical use, so working speech for real
programs beats completeness.

## Approach

The bridge copies NVDA's layered approach instead of relying on a single
source:

1. **MSAA / UI Automation** that Wine already implements (`oleacc`,
   `uiautomationcore`). Used for dialogs, buttons and menus, wherever the
   result is good enough.
2. **Window-class handlers that query controls with their own messages.**
   The handler is chosen by `GetClassName`. This is the primary source for
   content.
   - Standard Windows controls first: `Edit`, `Button`, `ListBox`,
     `ComboBox`, `Static`, `SysListView32`, `SysTreeView32`,
     `SysTabControl32`, `msctls_trackbar32`, `msctls_progress32`, menus
     (`#32768`), `RichEdit*`.
   - Then `Scintilla`.
   - Then other popular controls that give useful information through
     messages.
3. **Off-screen model** (hooking `ExtTextOutW` and similar inside the
   target process). This is the fallback for owner-drawn UIs. It is the last
   priority and has not been designed yet.

WinEvents from Wine act as *triggers* ("something changed, focus is in
window X"). Content is read through messages. Where Wine emits no event,
the bridge checks state after each keystroke from a `WH_KEYBOARD_LL` hook,
the same way NVDA does.

## Architecture

- **One Winelib process** (ELF `.so` built with `winegcc`), running in the
  target prefix (hooks and `WM_GETOBJECT` only work within one
  `wineserver`). One bridge instance runs per prefix.
- **Two threads:** a Win32 thread (message loop, WinEvent and keyboard
  hooks, handlers) and an AT-SPI thread (GLib main loop, D-Bus). They share
  a model of accessible objects and a queue of events.
- **Strict module boundary:** the Win32 core must not include any Linux
  headers. It has to compile with mingw as well, so it can run on real
  Windows and be compared with what NVDA sees. The AT-SPI side must not
  include Windows headers.
- **Source layout:** the Win32 core is split by area into `bridge/*.c`
  (one file per control family, plus `object.c`, `wintree.c`,
  `events.c`, `menu.c`, `keys.c`, `script.c`, `main.c`); what they share is in
  `bridge/core.h`, whose top comment lists the files. A control with
  items (list box, list view, tree view, tab control, toolbar) has a
  `struct item_handler` in its file, found by class with
  `find_item_handler` (`object.c`); focus, key check and window tree go
  through it. The AT-SPI side is `bridge/atspi.c`, the interface
  between the two `bridge/bridge.h`. Pure helpers without Windows API
  or D-Bus live in `bridge/utf16.c` (core: text offsets, mnemonics,
  menu text) and `bridge/textrange.c` (AT-SPI side: word and line
  ranges), so `tests/unit.c` checks them natively.
- **Application scripts** (decided 2026-11-18): like NVDA's app modules,
  fixes for one program live in `scripts/<exe>.lua`, run by Lua 5.4
  (`third_party/lua`, built into the Win32 core) in `bridge/script.c`,
  whose top comment is the reference of the script API. Lua is plain C,
  so it builds with winegcc and mingw and keeps the module boundary;
  CPython would be a large runtime with the msvcrt / libc clash.
  Decisions:
  - one table `wa` like SandFly's `sf` (`wa.onDescribe(fn)`,
    `wa.sendMessage`, ...);
  - target structures are read as bytes with `wa.readMemory` and
    `string.unpack`, not with a C FFI (LuaJIT's FFI is 5.1-only and
    generates calls with its own ABI assumptions);
  - roles are AT-SPI role names, states a set of names;
  - an onDescribe hook may change name, value, role, states, position,
    set size, level and columns (headers of a table, cells of a row); it
    is called for a `ListBox` and its items only so far
    (`describe_listbox_item`, `listbox_describe_self`), other describers
    get it when a script needs them;
  - hooks for events (focus, key, window) come when a script needs them;
  - the interface grows from real cases.
- **One AT-SPI application per Wine process**, each with its own D-Bus
  connection. Orca keeps per-application settings and scripts and announces
  application switches, so lumping all Wine programs into one application
  would make it unable to tell them apart.
- Language: **C**.

## Verified facts about Wine

Checked in the Wine source (`C:\vcs\github.com\wine-mirror\wine`, Wine 11.21,
2026-11-04). Re-verify after large Wine upgrades.

- `winegcc` still produces ELF Winelib binaries (`tools/winegcc/winegcc.c`,
  `is_pe` false yields `<name>.so`), so one process can call Win32 and link
  `libatspi` / `libdbus` / GLib.
- A global hook with a module loads that DLL into the target process
  (`dlls/user32/hook.c:345`, `LoadLibraryExW` in `get_hook_proc`). DLL
  injection for the off-screen model is therefore possible.
- `SetWinEventHook` with `WINEVENT_OUTOFCONTEXT` works across processes in
  one `wineserver`.
- WinEvents that Wine **does** emit: focus (`win32u/input.c:2110`),
  foreground, show/hide/create/destroy/location of windows, name change
  (`win32u/defwnd.c`), button state, listbox focus and selection, tab focus
  and selection, scrollbar / trackbar / updown / progress / datetime value
  change, value change in the comctl32 v6 `Edit` only, and dialog end.
- WinEvents that Wine does **not** emit (found with `grep` over `dlls/`,
  excluding tests):
  - caret (`OBJID_CARET`) events;
  - menu events (`EVENT_SYSTEM_MENUSTART`, `MENUPOPUPSTART` and so on);
  - focus or selection in `SysListView32`, which only emits
    create/destroy/reorder;
  - anything at all from `SysTreeView32`;
  - value change in the user32 `Edit`;
  - anything from `ComboBox` itself (`user32/combo.c`);
  - keyboard navigation in `ListBox` / `ComboLBox`:
    `LISTBOX_HandleKeyDown` -> `LISTBOX_MoveCaret` notifies nothing. The
    listbox focus and selection events come only from a mouse click,
    `LB_SETCURSEL` and `WM_SETFOCUS`. The bridge therefore re-reads the
    focused object 50 ms after each key (`check_after_key` in
    `bridge/events.c`) and asks a list box for `LB_GETCARETINDEX`.
- Standard messages with pointers (`WM_GETTEXT`, `EM_GETLINE`, ...) are
  marshalled across processes by Wine like on Windows. Custom messages with
  pointers (`SCI_GETTEXTRANGE`, `LVM_GETITEMTEXTW`, `TVM_GETITEMW`, ...) are
  not. For those, allocate memory in the target with `VirtualAllocEx`, then
  use `WriteProcessMemory`, `SendMessage` and `ReadProcessMemory`, as NVDA
  does.
- UI Automation adds nothing over MSAA for standard controls. No Wine
  control implements a UIA provider (no `IRawElementProviderSimple` or
  `UiaReturnRawElementProvider` in `comctl32`, `user32`, `win32u`).
  `uiautomationcore` wraps the MSAA object
  (`create_msaa_provider` in `uia_provider.c`), adds the base HWND
  properties, and the only pattern it offers is `LegacyIAccessible`
  (no `Text`, `Value`, `Selection`, ...).
- UIA is useful for programs that implement their own provider and answer
  `WM_GETOBJECT` with `UiaRootObjectId` (Qt, Chromium/Electron, WPF and
  similar). For those the Wine UIA client should deliver the program's real
  tree, text and patterns. Not verified yet.

## Verified by experiment

Found with `probe` against Wine's notepad (`tests/probe-notepad.sh`,
Wine 11.21, 2026-11-05).

- `GetGUIThreadInfo` on a thread of another process works: it returns
  `hwndFocus`, `hwndCaret` and the caret rectangle, and in menu mode
  `GUI_INMENUMODE` with `hwndMenuOwner`.
- `EM_GETSEL`, `EM_LINEFROMCHAR`, `EM_LINEINDEX`, `EM_LINELENGTH` and
  `EM_GETLINE` on an `Edit` of another process give the caret line and
  column correctly. Notepad's `Edit` emits `EVENT_OBJECT_VALUECHANGE` on
  `OBJID_CLIENT` for every typed character.
- Menus cannot be read from another process:
  - the popup is a visible top-level `#32768` window and `MN_GETHMENU`
    returns its `HMENU`, but `GetMenuItemCount` / `GetMenuItemInfo` on that
    handle (and on `GetMenu` of the owner) fail with error 1400;
  - MSAA does not help: `AccessibleObjectFromWindow(owner, OBJID_MENU)`
    returns `E_NOTIMPL` and the popup's `OBJID_CLIENT` has no children;
  - the only WinEvents are `EVENT_SYSTEM_CAPTURESTART/END` on the owner and
    create/show/hide/destroy of the `#32768` window.

  Menus therefore need code inside the target process (a hook DLL).
- MSAA for focus events works for `Edit` (role text, states) and the main
  window client (name = window title).

Found with the bridge spike (`bridge/`, `tests/atspi-dump.py`, Wine 11.21,
2026-11-06).

- One winegcc ELF can hold both sides: the Win32 core is built with
  `winegcc -mno-cygwin`, `bridge/atspi.c` with the host `gcc` and
  `pkg-config gio-2.0`, and `winegcc` links both with `-lgio-2.0`. A
  thread created with `CreateThread` runs a GLib main loop and GDBus.
- The AT-SPI side talks D-Bus directly through GDBus instead of
  `atk-bridge`, because `atk-bridge` allows only one application per
  process. Registration: `org.a11y.Bus.GetAddress` on the session bus,
  a private connection to that address, objects exported at
  `/org/a11y/atspi/accessible/root`, then `org.a11y.atspi.Socket.Embed`
  on `org.a11y.atspi.Registry`. `python-atspi` then lists the application
  with its name and toolkit.
- One GDBus connection per Wine process, each with
  `g_dbus_connection_register_subtree` on `/org/a11y/atspi/accessible`,
  gives one AT-SPI application per process. python-atspi receives
  `object:state-changed:focused`, `window:activate` and
  `object:children-changed` with signal signature `(siiva{sv})`
  (`tests/bridge-notepad.sh`, `tests/atspi-events.py`).
- The Win32 thread hands snapshots to the AT-SPI thread with a GLib idle
  source attached to the AT-SPI thread's `GMainContext`; no separate
  queue or lock is needed.
- Wine notifies `EVENT_OBJECT_FOCUS` for a window before sending it
  `WM_SETFOCUS` (`win32u/input.c`, `set_focus_window`), but the
  out-of-context event for the parent arrives *after* the child's when
  the handler moves focus on (notepad: Edit, then the main window). The
  bridge drops a window focus event when `GetGUIThreadInfo().hwndFocus`
  is another window.
- Wine's `oleacc` has no proxy for `Button` or `ComboBox`: both report
  `ROLE_SYSTEM_CLIENT`. The bridge falls back to a role from the window
  class and style. A combo box's MSAA name is its current text.
- Wine's `oleacc` names only a bare `Edit` from the preceding `Static`
  (`edit_find_label` in `dlls/oleacc/client.c`, by `GW_HWNDPREV`). The
  `Edit` inside a combo box (file name field of the Open dialog, inside
  `ComboBoxEx32` after a hidden `Edit`) and other controls get no name.
  The bridge's `find_label` does it for them: climb out of `ComboBox` /
  `ComboBoxEx32`, skip hidden windows and empty or picture `Static`s,
  take the nearest text `Static`, drop the `&` of the mnemonic.
- Wine's `oleacc` has only stubs for most classes: in `client.c`
  `classes[]` only `Edit` has its own vtbl. `SysListView32` items are
  therefore read with `LVM_GETNEXTITEM` (`LVNI_FOCUSED`),
  `LVM_GETITEMSTATE`, `LVM_GETITEMCOUNT` and `LVM_GETITEMTEXTW` through
  memory in the target (`get_listview_text`). Orca says "x of y" from
  the attributes `posinset` (from 1) and `setsize` (`ax_utilities.py`).
- `ListBox` is a stub in `oleacc` too: name and role of an item
  (child > 0) fail with `E_INVALIDARG`. Items are read with
  `LB_GETCOUNT`, `LB_GETTEXTLEN`, `LB_GETTEXT` (marshalled by Wine) and
  `LB_GETSEL` (`describe_listbox_item`). On `WM_SETFOCUS` Wine reports
  child `focus_item + 1`, which is 1 even for an empty list.
- A focused list item gets the list window as its AT-SPI parent (role
  list box, label from `find_label`, `bridge_focus.container`), so Orca
  says the list's label when focus enters it.
- Wine's `LVM_SETVIEW` changes the list view mode without touching
  the window style, so report view is detected with `LVM_GETVIEW`
  (`LV_VIEW_DETAILS`). Header texts come from `LVM_GETCOLUMNW`, the
  display order from `LVM_GETCOLUMNORDERARRAY`, which fails if its
  count is smaller than the number of columns
  (`HEADER_GetOrderArray`). Both need memory in the target.
- `SysTabControl32` has no `oleacc` proxy either. Without
  `TCS_BUTTONS` an arrow key emits only `EVENT_OBJECT_SELECTION`
  (child item + 1); `EVENT_OBJECT_FOCUS` for a tab comes only with
  `TCS_BUTTONS` (`comctl32/tab.c`, `TAB_SetCurFocus`). The bridge reads
  the focused tab with `TCM_GETCURFOCUS`, its text with `TCM_GETITEMW`
  through memory in the target, and checks it after each key.
- After an arrow key in the Open dialog's file list the dialog thread is
  busy for 0.3 to over 0.5 s (WSL, folder on `/mnt/c`), so list view
  queries can time out; the key check retries a busy list.
- Wine's `oleacc` sends `WM_GETOBJECT` and `WM_GETTEXT` with plain
  `SendMessageW` (`dlls/oleacc/main.c`, `client.c`), so MSAA on a busy
  target blocks the bridge. For a whole window of a class with a known
  role `describe_object` skips MSAA and does what `oleacc` would
  (`describe_window`: name from `WM_GETTEXT` with a timeout, an `Edit`
  named by its `Static` as in `edit_find_label`, states from the style);
  other classes ask with `WM_NULL` and a timeout before MSAA. The gain
  in time is small (35 objects 190 -> 182 ms): the cost per window is
  the cross-process messages. All messages go through `send_message`
  (`util.c`): after a timeout (Wine sets `ERROR_TIMEOUT` for a timeout
  and for a hung thread) the thread is skipped for 40 ms, so one pass
  waits once. Ctrl+O in notepad keeps its thread busy for over 500 ms
  while the Open dialog is created; a focus or key check that finds the
  target busy is retried (`check_busy_focus`, `check_after_key`). A
  window tree built while the target is busy lacks windows and the frame
  name (Up in the Open dialog's file type combo refills the list), so
  `report_tree` drops it and tries again after 150 ms, up to 10 times.
- `atspi.c` uses only GLib functions, never libc ones directly. The
  binary imports msvcrt through Wine, and a libc name could bind to an
  ms_abi msvcrt thunk. Not proven to break, just avoided.

Found with the bridge's autostart (`build/exp-autostart/`, prefix
`~/.wine-atspi-exp`, Wine 11.21, 2026-11-07).

- Every session start runs `wineboot.exe --init` (`ntdll/unix/env.c`,
  `run_wineboot`), which starts the values of HKLM `RunServices` but not
  `Run` (`programs/wineboot/wineboot.c`). `wine-atspi install` writes
  `"<exe>" --service [--log FILE]` there, so any program that starts a
  session (Steam, Lutris, a `.desktop` file) gets the bridge. The ELF
  starts directly from that value; `GetModuleFileNameW` gives
  `...\build\wine-atspi.exe` without `.so`, and that works too.
- Started so, the bridge has no Win32 std handles and does not see the
  environment of the first `wine` command, but inherits its Unix fd 2
  (GLib's `g_printerr` lines show there). Hence `--log`, appended,
  because even `wine reg ...` starts a session with a bridge.
- A normal process keeps the session alive. With `--service` the bridge
  calls `NtSetInformationProcess` with class 1000
  (`ProcessWineMakeProcessSystem`, declared only under `__WINESRC__`),
  like `services.exe`. Its threads then do not count as desktop users,
  and the returned event is signaled when the session shuts down
  (`server/process.c`, about 3 s after the last user process). The bridge
  waits for it with `MsgWaitForMultipleObjects`; both hooks keep working.
- One bridge per prefix: a named mutex `WineAtspiBridge`. A second
  bridge exits at once. Tests run their own bridge, so they call
  `uninstall` and `wineserver -k` first.
- Confirmed with Orca (2026-11-07): after `wine-atspi install`, a plain
  `wine regedit` is accessible without `bridge.sh`.

Found with the listener watch (`tests/bridge-listeners.sh`,
at-spi2-core 2.62.1, Orca 51.1, 2026-11-16).

- The bridge hooks nothing while no AT-SPI client listens to events: a
  global `WH_CALLWNDPROC` costs every GUI process two wineserver
  requests per sent message (`dlls/win32u/hook.c`,
  `call_message_hooks`). `org.a11y.Status` cannot tell: it has only
  `IsEnabled` (no `ScreenReaderEnabled`), which Orca sets at start
  (`orca.py`, `_ensure_accessibility_enabled`) and never clears.
- The registry tells instead: `GetRegisteredEvents` on
  `/org/a11y/atspi/registry` (`a(ss)`: bus, event such as
  `Object:StateChanged:Focused`) and the signals
  `EventListenerRegistered` / `EventListenerDeregistered`. A client that
  dies (even `kill -9`) is deregistered with an empty event. The AT-SPI
  side asks again on each signal (`watch_listeners` in `atspi.c`) and
  calls `bridge_set_listening`; the main thread then installs all hooks
  and reports the current focus, or removes them, forgets its state and
  drops every application (`start_watching`, `stop_watching` in
  `main.c`).
- Tests that only dump the tree have no listener, so all tests start
  the bridge with `--always-on`.
- Confirmed with Orca in the Arch image (MATE, 2026-11-18): with Orca
  running, `bridge.sh notepad` speaks at once ("hooks installed");
  `pkill -x orca` gives "hooks removed"; Orca started again (over ssh,
  environment of `mate-session` from `/proc`) installs the hooks and
  speaks the focused notepad without a key.

## Verified in Orca's source

Orca 51.1 (`/usr/lib/python3.14/site-packages/orca/` in Arch WSL,
2026-11-06). Re-check after Orca upgrades.

- `object:property-change:accessible-name` on the focused object is
  presented again, **except for a combo box**, which is skipped
  (`scripts/default.py`, `_on_name_changed`).
- A combo box value comes from the attribute `valuetext` first, then from
  a text child or the selected child, then from the name
  (`generator.py`, `_get_combo_box_value`). The bridge exposes the combo
  text as `valuetext` and leaves the name for a label.
- `object:selection-changed` whose source is the focused combo box makes
  Orca present the combo box again (`_on_selection_changed`). The bridge
  emits it when the combo value changes.
- Orca never speaks the role of a focused `list` (`_should_speak_role`
  drops LIST and LIST_ITEM outside Where Am I); an unnamed list then
  gives no speech at all. For a `list box` it says "n items" from the
  attribute `setsize`, or the role if there is no count
  (`_generate_list_box`). The bridge reports a list view without a
  focused item as a list box. Speaking "x of y" for items is off by
  default (`speech_presenter.py`, `speak-position-in-set`).
- On a focus change Orca speaks the new ancestors (`_generate_ancestors`
  in `speech_generator.py`), but skips `list`, `panel`, `table` and
  other context roles unless enabled in its settings
  (`_get_enabled_and_disabled_context_roles`); a `list box` ancestor is
  spoken.
- For a `list box` Orca says "list with n available items" instead of
  "n items" when fewer than `setsize` children are sensitive
  (`_generate_number_of_children`, `get_sensitive_child_count`; above 50
  children it assumes all are). The bridge therefore gives a list box
  one child slot per item and fills unknown ones with placeholders
  (`is_item_list`, `item_at` in `atspi.c`).
- Orca announces `state-changed:selected` only after Space (or Up/Down
  in a table cell), and a repeated focus on the same object is ignored
  (`set_locus_of_focus` without force). So the first Down in a Wine
  `ListBox`, which only selects the already focused item 0, would be
  silent. The bridge then emits `object:announcement` (polite) with the
  item's name, unless the last key was Space (`update_node`); Orca
  presents an announcement's text (`_on_announcement`).
- In a GUI table Orca reads the full row when focus moves to another
  row (`should_read_full_row`, setting on by default): it asks
  `Table.GetAccessibleAt` for every column of the focused cell's row.
  A cell's coordinates come from `TableCell.Position`, else from its
  index in parent with `Table.GetRowAtIndex` / `GetColumnAtIndex`. The
  table needs role table, tree table or tree plus the `Table`
  interface. The bridge lays a table out like GTK 3: column headers
  first, then cells row by row (`cell_index` in `atspi.c`). Confirmed
  with Orca on a report-view list view (2026-11-07), including
  Ctrl+arrows and Ctrl+Space.
- A tree item gets its level from the attribute `level` (from 1) when it
  has a `tree` ancestor (`script_utilities.py`, `node_level`); the level
  is spoken only when it changes. `state-changed:expanded` on the focused
  object presents it again (`_on_expanded_changed`). As an ancestor, a
  `tree` or `tree table` is never spoken, name included
  (`_generate_tree` -> `_generate_default_presentation` returns only the
  empty prefix for an ancestor), so a tree's label is heard only through
  Where Am I. Wine's tree view selects its first item on `WM_SETFOCUS`,
  so focus goes straight to an item. Decided (2026-11-07): left as in
  GTK, no workaround in the bridge.
- For a focused page tab Orca says name and role only ("General
  karta"): `_generate_ancestors` returns nothing when the new focus is a
  page tab, so the page tab list and its label are never spoken, as in
  GTK. "x of y" again only with `speak-position-in-set`. Confirmed with
  Orca on `tests/controls.c` (2026-11-07).
- A slider's value comes from the attribute `valuetext` first, then
  `Value.Text`, then the formatted `CurrentValue` (`ax_value.py`); Orca
  adds the percentage from minimum and maximum. It presents
  `object:property-change:accessible-value` for the focused object and
  for any progress bar, except one without size or inside a status bar
  (`_on_value_changed`, `is_presentable_value_change`). The bridge
  therefore gives a trackbar no `valuetext`. Wine's `oleacc` has no
  proxy for `msctls_trackbar32`; `TBM_GETPOS` / `TBM_GETRANGEMIN` /
  `TBM_GETRANGEMAX` work across processes (`describe_range`).
- "Without size" means `Component.GetExtents(WINDOW)` gives width and
  height 0, which is also the answer for an object without the
  `Component` interface (`ax_component.py`, `get_rect`). So every
  whole-window object gets `Component` with its `GetWindowRect`
  (`get_extents` in `object.c`); list items, cells and tabs get none.
  Progress bar speech is on by default, at most every 10 s except at
  100 %, and only for the active application
  (`speak-progress-bar-updates`, `progress-bar-speech-interval`,
  `progress-bar-speech-verbosity` in `org.gnome.Orca.gschema.xml`).
- A progress bar never has focus. Wine's comctl32 emits
  `EVENT_OBJECT_VALUECHANGE` (`OBJID_CLIENT`) on `PBM_SETPOS`, and
  `PBM_GETPOS` / `PBM_GETRANGE` work across processes. The bridge hooks
  that event and adds the bar below its frame on its first change
  (`on_value_change`, `atspi_post_value`). Confirmed with Orca on
  `tests/controls.c` "Run" (2026-11-07): 0 %, 55 % after 10 s, 100 %.
  A bar that changes together with a focused slider is not heard: Orca
  interrupts its speech for the slider's value at once.
- Orca's "present status bar" command (Orca+KP_Enter twice, laptop
  Orca+/ twice; `where_am_i_presenter.py`) takes the first `status bar`
  descendant of the focus's frame and speaks its name, role, then its
  descendants or its whole text (`_generate_status_bar`). Wine's
  `msctls_statusbar32` emits no WinEvent on `SB_SETTEXTW`
  (`comctl32/status.c`). `SB_GETPARTS`, `SB_ISSIMPLE` and
  `SB_GETTEXTLENGTHW` work across processes, `SB_GETTEXTW` needs memory
  in the target. The bridge reads the status bar on each focus change
  and after each key and puts it below the frame as one object with the
  text of all parts joined by spaces, as NVDA does (`report_status_bar`).
  Confirmed with Orca on regedit (2026-11-08); `tests/bridge-statusbar.sh`.
- An up-down (`msctls_updown32`) never has focus: it subclasses its
  buddy `Edit` and handles the buddy's Up and Down keys (Wine: no Page keys)
  (`comctl32/updown.c`, `UPDOWN_Buddy_SubclassProc`). Those keys change
  the buddy text through `UPDOWN_DoAction` without a WinEvent; only
  `UDM_SETPOS` emits `EVENT_OBJECT_VALUECHANGE`. The buddy cannot name
  its up-down, so the bridge looks for a sibling up-down whose
  `UDM_GETBUDDY` is the `Edit` (`find_updown`) and reports the pair as
  one spin button with `Value` and `Text`, as GTK does. `UDM_GETPOS32`
  works across processes, `UDM_GETRANGE32` needs memory in the target.
  Orca ignores text and caret events of a spin button after Up / Down /
  Page keys and speaks its `accessible-value` change instead
  (`SPIN_BUTTON_VALUE_CHANGE` in `ax_utilities_event.py`); a spin button
  speaks its text before the value (`_generate_spin_button`).
- When focus enters a dialog, Orca speaks the dialog as an ancestor
  together with its "unrelated labels" (`_generate_dialog` ->
  `unrelated_labels` in `script_utilities.py`): descendants with role
  label or static that are showing, not focusable, without relations,
  of at least 3 words and not part of the dialog name, sorted by
  position. Wine's message box text is a `Static` (id `0xffff`,
  `SS_NOPREFIX`) after the buttons (`user32.rc`, `MSGBOX`). The bridge
  gives each top-level window its visible text `Static`s that do not
  label the next control as label children (`describe_labels`,
  `set_labels`). Confirmed with Orca on notepad's save-changes box
  (2026-11-08).
- Menus: Orca's ancestors stop at a `menu bar`, and at a parent `menu`
  named like the prior focus if that was an expanded menu
  (`speech_generator.py`), so GTK3's frame > menu bar > menu "File"
  (expanded) > menu item "Open" says only "Open" after "File". A
  submenu entry is a `menu`, not a `menu item`. A menu item speaks name,
  role, checked (check / radio menu item), expanded, insensitive and the
  accelerator, from the attribute `keyshortcuts` first
  (`ax_utilities_action.py`). While focus is in a menu Orca ignores
  window deactivation (`default.py`). The bridge builds the same tree
  below the frame (`report_menu_item`).
- Orca reads text almost only through `Atspi.Text.get_string_at_offset`
  with a granularity (`ax_text.py`), plus caret offset, character count,
  `get_text` and selections. For multi-line text at the end offset it
  expects the empty last line, as GTK3 gives it.
- Flat review (`flat_review.py`, `_get_on_screen_objects` in
  `ax_utilities.py`) starts at the dialog or frame of the focus and
  takes objects that are showing and intersect its rect (all rects in
  WINDOW coordinates). An object with a name, description or text is a
  zone; zones are sorted into lines by y (5 px), then x. A single-line
  editable text is one zone with its component rect; multi-line text
  gives its visible lines (`get_visible_lines`: binary search with
  `Text.GetRangeExtents` of each line, compared by the line's middle
  with the clip rect), then words by `GetRangeExtents` too. The bridge
  answers it on the AT-SPI thread through `bridge_text_extents`
  (`EM_POSFROMCHAR`); lines scrolled out lie about 100000 px above or
  below the window. The review context is rebuilt after any other key
  or a caret move of the reviewed object. Confirmed with Orca in
  notepad and wordpad (2026-11-09).
- Flat review of a table with `Selection` takes only its visible cells
  (`iter_visible_cells`), no column headers: the range comes from
  `GetAccessibleAtPoint` at the table's corners (x+1, y+1) and
  (x+w-1, y+h-1), each cell's row and column from its index in parent.
  A header at the corner would give -1 and nothing is read, so the
  bridge's `child_at_point` answers only with cells, the nearest visible
  one when the point is on a header or below the rows. Where Am I
  selection (Orca+Shift+Up, `present_selection`) climbs from a cell to
  its table, says `Table.NSelectedRows` of the row count, then the names
  of the selected children; like GTK 3 the bridge gives the first cells
  of the selected rows (`describe_list_selection`). Plain Where Am I
  says nothing about the selection. Confirmed with Orca on
  `tests/controls.c` "Files" (2026-11-09).
- From a list item Where Am I selection climbs to a `list box` with
  `Selection` (`get_selection_container`); without one it speaks the
  selected text instead. The count of selectable items is the number of
  `list item` descendants (`selectable_child_count`, no `Collection`),
  which the bridge's item slots give. A `ListBox` gives its selection
  with `LB_GETCURSEL` (single) or `LB_GETSELCOUNT` / `LB_GETSELITEMS`
  (multiple; marshalled by Wine, `LB_ERR` on a single-selection list).
  The first Down in a `ListBox` selects item 0 without moving the focus,
  so the key's item refresh posts the list itself too (`report_tree`
  with a control). Confirmed with Orca on `tests/controls.c` "Colors"
  and "Fruits" (2026-11-10); `tests/bridge-tree.sh`.
- Object navigator (`object_navigator_command_definitions.py`):
  Orca+Ctrl+arrows move, Orca+Ctrl+Return activates, Orca+Shift+A lists
  the object's actions. Activation tries the actions click, press, jump,
  open and activate (`try_all_clickable_actions` in
  `ax_event_synthesizer.py`), else a mouse click at the centre of the
  extents. A "click" action also keeps an object out of the layout-only
  ones and makes it a flat review zone when it has nothing below. The
  bridge gives Win32 buttons (push button, check box, radio button) the
  action "click", which posts `BM_CLICK` (`bridge_click`); Wine's
  `BM_CLICK` sends `WM_LBUTTONDOWN` / `WM_LBUTTONUP`
  (`user32/button.c`), so the button gets focus and notifies
  `BN_CLICKED`. Confirmed with Orca in the Arch image (2026-11-09); in
  WSLg the navigator keys did not work (legacy device).
- Toolbars: a `tool bar` ancestor is never spoken, like a `tree`: it is
  not layout-only nor a disabled context role, but `_generate_tool_bar`
  -> `_generate_default_presentation` gives only the empty prefix for an
  ancestor, so focus on a button says just "New tlačidlo". Decided
  (2026-11-10): left as in GTK, no workaround. A `toggle button` says
  pressed from the state checked or pressed. Wine's `comctl32/toolbar.c` emits no
  WinEvent and has no `oleacc` proxy. The bridge puts the buttons below
  the toolbar in the window tree (`describe_toolbar_button_in`, child
  index + 1): `TB_GETBUTTONINFOW` with `TBIF_BYINDEX` for command,
  state and style (`BTNS_CHECK` -> toggle button, separators and hidden
  buttons left out), `TB_GETBUTTONTEXTW` for the text, `TB_GETITEMRECT`;
  all need memory in the target. `TB_GETBUTTONINFOW` gives no text from
  the string list (`TB_ADDSTRING`), `TB_GETBUTTONTEXTW` does but takes
  no buffer size, so the length is asked first with lParam 0. A button
  without text has its name only in the tooltip: the toolbar asks its
  parent with `TTN_GETDISPINFO`, but Wine refuses `WM_NOTIFY` across
  processes (`win32u/message.c`). The bridge sends the toolbar the
  registered message `WineAtspiToolTip` (command id, buffer in the
  target); the hook DLL sees it in `WH_CALLWNDPROC` and sends the toolbar
  `WM_NOTIFY` `TTN_GETDISPINFOW` in-process, which tries `TBN_GETINFOTIP`,
  the hidden text and then the owner (`TOOLBAR_TTGetDispInfo`)
  (`get_toolbar_tooltip`, `answer_tooltip`). This names the Open
  dialog's icon buttons ("O úroveň vyššie", ...; dump, 2026-11-10).
  Wine's wordpad buttons have neither text nor tooltips. Confirmed with Orca
  flat review on `tests/controls.c` (2026-11-10): "New nestlačené Bold
  stlačené Wrap Undo tlačidlo". Flat review puts a state zone
  (`StateZone`) before the name of a checkable object, an unnamed object
  gives its role, and insensitive is not said
  (`_get_zones_from_object` in `flat_review.py`).
- A focused toolbar moves a hot button: `WM_SETFOCUS` makes button 0
  hot, Left/Up and Right/Down the previous or next one, skipping
  separators and disabled buttons and wrapping
  (`TOOLBAR_SetRelativeHotItem`); Space/Return send `WM_COMMAND` to the
  notify window without toggling a check button. No WinEvent tells.
  `TB_GETHOTITEM` works across processes but gives -1 unless the
  toolbar is `TBSTYLE_FLAT` or themed (`TOOLBAR_GetHotItem`). The bridge
  reports the hot button as the focus and follows it after each key
  (`toolbar_focus`). A toolbar answers `WM_GETDLGCODE` with 0, so
  `IsDialogMessage` takes its arrow keys; `tests/controls.c` subclasses
  it. Confirmed with Orca on `tests/controls.c` (2026-11-10).
- A window that appears without a key does not interrupt speech: notepad
  started from a terminal is spoken only after the queued terminal
  output. Orca sets the new frame as locus of focus through
  `set_active_window` without an event (from `_on_active_changed` or
  `_on_window_activated`), and `_should_interrupt_for_focus_change`
  (`presentation_manager.py`) returns False for "event is None"; the
  following focus on the `Edit` does not interrupt either, because the
  old focus is a named ancestor. GTK 3 windows behave the same (debug
  log, mate-terminal), although GTK sends `window:activate` before
  `state-changed:active` and the bridge the other way round. Orca
  interrupts on a key press. Not a bridge bug (2026-11-18, debug log
  with `orca --debug-file`).

Found with the bridge and notepad (`tests/bridge-notepad.sh`, 2026-11-06).

- Orca names the reason of a caret move or text insertion from the last
  key it saw itself; without a key the reason is UNKNOWN and it speaks
  nothing (only text selection changes are still presented). Orca gets
  keys from `Atspi.Device.new_full` (libatspi `atspi-device.c`): X11
  device only when `WAYLAND_DISPLAY` is unset, else the compositor's
  `org.freedesktop.a11y.KeyboardMonitor` (Mutter), else the *legacy*
  device. The legacy device (WSLg) only hears keys that applications
  report to the registry, as GTK's atk-bridge does. The bridge therefore
  reports every key from its `WH_KEYBOARD_LL` hook (`report_key`,
  `atspi_post_key`).
- With the legacy device Orca cannot consume keys, so its own commands
  reach the Wine program too: KP_7 / KP_8 / KP_9 with NumLock off are
  Home / Up / Page Up there. In flat review this moves the caret and
  scrolls the text back; in a list it moves the focus. Not a bridge
  bug; test review commands in the Arch image.
- The registry's `DeviceEventController.NotifyListenersSync`
  (`/org/a11y/atspi/registry/deviceeventcontroller` on
  `org.a11y.atspi.Registry`) accepts only one struct `(uinnisb)`: type
  (0 press, 1 release), X keysym, X keycode (evdev + 8), X modifier mask,
  timestamp, string, is_text. Its introspection says `(uiuuisb)`, which
  is rejected with InvalidArgs; an unwrapped argument list crashes the
  registry (at-spi2-core 2.62.1). Keycode 0 is not delivered.
- In WSLg the compositor takes Alt+F4 itself. Notepad's save-changes
  message box then gets no focus inside Wine: `GetGUIThreadInfo` keeps
  reporting the `Edit`, and Wine focuses the box only on the next key. In
  the Arch image (MATE, real X11) the box gets focus at once, so the bridge
  does not work around it.
- `EM_GETSEL` returns the selection ordered (`EDIT_EM_GetSel`), so it does
  not tell which end has the caret. `EM_POSFROMCHAR` of the start compared
  with `GetGUIThreadInfo().rcCaret` does, across processes.

Found with the bridge and Wine's explorer (`wine explorer`, 2026-11-08).

- Wine's `explorer.exe` file browser cannot be used from the keyboard at
  all. Its `ExplorerWClass` frame keeps the focus: the window procedure
  has no `WM_SETFOCUS` / `WM_ACTIVATE` case that passes focus to the
  shell view, and the message loop has no `IsDialogMessage`, so Tab and
  arrows do nothing (`programs/explorer/explorer.c`). The title is the
  current folder (`SetWindowTextW` on navigation), so Orca says only
  "Desktop". The shell view would focus its list view on `WM_SETFOCUS`
  (`shell32/shlview.c`), but nothing sends it one short of a mouse click.
  Not a bridge bug; the bridge does not work around it.

Found with the bridge and Wine's winefile (`build/exp-winefile.sh`,
2026-11-08).

- Both panes of `winefile` (folder tree and file list) are `ListBox`es
  with `LBS_OWNERDRAWFIXED` and no `LBS_HASSTRINGS`, children of a
  `WFS_Tree` window (`create_tree_window` in
  `programs/winefile/winefile.c`). Arrow keys move the caret normally, but
  `LB_GETTEXT` gives nothing, so Orca heard unnamed items. The item data
  is a pointer to winefile's `Entry` (three pointers, three ints, then a
  `WIN32_FIND_DATAW`); the bridge reads its `cFileName` with
  `LB_GETITEMDATA` and `ReadProcessMemory`, since 2026-11-18 in the
  application script `scripts/winefile.lua` (`tests/bridge-winefile.sh`).
  The layout comes from Wine's source; re-check after Wine upgrades.
- Since 2026-11-18 the script makes the folder tree (control id 3,
  `IDW_TREE_LEFT`) a `tree` and the file list (id 6) a `table`. A tree
  item's level is `Entry.level` + 1, expandable if not scanned yet or it
  has a subfolder; winefile sorts ".", ".." and folders before files
  (`compareType`), so the walks stop at the first file. The table's
  columns are name, size and modification date, headed by the texts of
  the `SysHeader32` with id 5, whose item 0 is the empty icon column;
  `Pane.visible_cols` (the list's `GWLP_USERDATA`) 0 (View > Name) keeps
  it a list. Return in the tree expands or collapses `pane->cur`, which
  after Tab need not be the item with the caret (seen in
  `tests/bridge-winefile.sh`: Return expanded the current folder while
  the caret was on item 0); a winefile quirk.
- A large folder (`Z:\lib64`, 4000 files) keeps winefile busy for
  seconds; the key check then gives up on the busy list. The bridge is
  not the cause: winefile started in `/usr/bin` (3442 files) answers
  after about 6 s without a bridge, with an idle one and with all hooks
  installed alike (`tests/perf-winefile.sh` in the VM, 3 runs each,
  spread 5-6.6 s without, 5.9-6.4 s with; 2026-11-18). The global
  `WH_CALLWNDPROC` hook needs no narrowing for this.

Found with regedit's tree (`tests/perf-tree.sh` in the VM, 2026-11-18).

- A cross-process message costs about 0.75 ms. Walking all 319 subkeys
  of `HKEY_CLASSES_ROOT` for the place of the focused item took 327
  messages and 250 ms per arrow key, the tree of its visible items 451
  messages and 410 ms. The bridge keeps each parent's level and number
  of children and the positions of items described, forgotten when
  `TVM_GETCOUNT` changes (`places` in `treeview.c`): an arrow key now
  costs 6-7 messages and 11 ms, a tree of 28 items 122 messages and
  90-150 ms, Page Down at most one page of steps. The focus trace shows
  the messages, time and place of each focus.

Found with the bridge and Wine's wordpad (`tests/bridge-wordpad.sh`,
2026-11-08).

- Wordpad's document is a `RichEdit20W` (`riched20`). It emits no
  WinEvent at all; the key check reads it like an `Edit`.
- A RichEdit 2.0+ ends a paragraph with one `\r` inside: `EM_GETSEL`
  and `EM_POSFROMCHAR` count it as one character, while `WM_GETTEXT`
  gives `\r\n` (`GT_USECRLF` in `riched20/editor.c`). The bridge maps
  the positions with `rich_text_index` before `char_offset`.
- Wine's `oleacc` has no proxy for RichEdit and names it by its window
  text, the whole document; the bridge drops that name, else Orca would
  read the text again after each key.
- The test prefix has no `App Paths` entry for wordpad, so `wine wordpad`
  fails; start `C:\Program Files\Windows NT\Accessories\wordpad.exe`.

Found with the menu hook (`build/exp-menu/`, `tests/bridge-menu.sh`,
Wine 11.21, 2026-11-08).

- A global `WH_CALLWNDPROC` hook from a PE DLL built with mingw
  (`x86_64-w64-mingw32-gcc`, package `mingw-w64-gcc`) loads into every
  64-bit GUI process of the prefix, also when the Winelib bridge installs
  it. The DLL must pin itself (`GetModuleHandleExW` with
  `GET_MODULE_HANDLE_EX_FLAG_PIN`), else Wine frees it after each call
  and its state is lost. `WM_COPYDATA` to a hidden bridge window by
  `SendMessageTimeoutW` takes 2-3 ms per record. A 32-bit target would
  need a 32-bit DLL installed by a 32-bit process; not done.
- Wine sends the owner `WM_MENUSELECT` for each highlighted item (item id,
  or position for an item with a submenu; `0xffff` with no menu at the
  end), `WM_INITMENUPOPUP` (position of the opening item) before a popup
  opens and `WM_UNINITMENUPOPUP` when it closes, plus
  `WM_ENTERMENULOOP` / `WM_EXITMENULOOP`. Escape or Left back to the
  parent item sends no `WM_MENUSELECT` for it (`win32u/menu.c`), so the
  hook reports the parent item itself. Right on an open menu gives the
  next bar item and then the first item of its popup at once; the bridge
  waits 30 ms (`MENU_SETTLE_MS`) and reports the last one.
- Item states are right in `WM_MENUSELECT`, stale in `WM_INITMENUPOPUP`
  (the application fixes them while handling it). Item text is raw:
  `&Otvoriť...\tCtrl+O`.
- The Edit's context menu (Shift+F10) uses `TPM_NONOTIFY`
  (`user32/edit.c`): only `WM_MENUSELECT` and `WM_EXITMENULOOP`, and
  nothing is highlighted until the first arrow key, so it is silent until
  then.
- Wine's system menu (Alt+Space, or Left of the first bar item) hangs
  below a wrapper menu with one empty `MF_SYSMENU` item; its items carry
  `MF_SYSMENU`. The hook leaves the wrapper level out; the system menu
  has no name.
- Alt+letter and Alt+Space post a fake `WM_KEYDOWN` `VK_RETURN`
  (`menu.c`), seen by `WH_GETMESSAGE` but not by the LL hook.
- Wine bug: in regedit with focus in its tree view, Alt alone and F10 do
  not open the menu (`comctl32/treeview.c` eats `WM_SYSKEYDOWN`);
  Alt+letter works.

Found with Wine console windows (`build/exp-console/`,
`tests/bridge-console.sh`, Wine 11.21, 2026-11-09).

- `wineconsole cmd` opens a `WineConsoleClass` window owned by
  `conhost.exe`; its title is the client's exe path. conhost emits no
  WinEvent at all (no `NotifyWinEvent` in `programs/conhost`). A console
  program started from a Linux terminal (`wine cmd`) has no window
  (conhost in tty mode); Orca reads the Linux terminal then.
- `AttachConsole(conhost pid)` fails (error 6). The client is found by
  `AttachConsole` on every process of a toolhelp snapshot and comparing
  `GetConsoleWindow()` with the window (`attach_console_window`). conhost
  and the client are siblings, both children of `wineconsole.exe`.
- After attaching, `CreateFileW("CONOUT$")`, `GetConsoleScreenBufferInfo`
  (buffer 80x150, cursor) and `ReadConsoleOutputCharacterW` give the text
  exactly. The bridge attaches only for each read (about 2 ms) and reads
  rows 0 up to the cursor or window bottom in one call
  (`describe_console`), every 100 ms while the console is foreground
  (`poll_console`). Wine's `AttachConsole` replaces the std handles and
  `FreeConsole` closes them (`kernelbase/console.c`), so the bridge saves
  and restores them; it calls `FreeConsole` once first, because a
  process with a console cannot attach. Only CTRL_C / CTRL_BREAK exist
  (no CTRL_CLOSE); the bridge ignores them.
- The text is diffed on buffer lines, not window rows (a scroll changes
  every row); `set_text` emits delete + insert. Known gap: a full buffer
  shifts lines up, giving one huge delete + insert.
- conhost ignores `SendInput` with `KEYEVENTF_UNICODE`; tests type real
  keys. `GetGUIThreadInfo` gives a caret rect sometimes one column stale;
  the buffer cursor is used instead.
- Orca 51.1 picks its terminal script by the focused object's role
  (`script_manager._script_for_role`), so the window is a frame with one
  `terminal` child. It speaks `text-changed:insert` as command output
  after Return (`treat_event_as_command`, extended to whole lines) and
  delays printable key echo until the text appears. Confirmed with Orca
  (2026-11-09): typing, command output, caret moves and Backspace.
- Flat review of a console: `GetCurrentConsoleFont` (attached) gives
  conhost's cell size in pixels, and cell (x, y) is drawn at client
  ((x - srWindow.Left) * width, (y - srWindow.Top) * height)
  (`programs/conhost/window.c`). `describe_console` saves this view for
  `console_text_extents`. Confirmed with Orca (2026-11-09).

Found in the Arch image without the bridge (`build/exp-focus/focus-test.py`,
XTest keys, Wine 11.21, 2026-11-09).

- Under MATE (marco, X11), closing notepad with unsaved text (Alt+F4, then
  No) leaves X input focus on `None` and `_NET_ACTIVE_WINDOW` on the
  destroyed message box. X drops every key, Alt+Tab included, and Orca
  says it cannot determine the location. It happens only when the message
  box and its owner go away together: an empty notepad, Cancel in the box
  and a GTK window all hand focus on normally. GNOME (Wayland, Wine under
  Xwayland) does not have the problem (checked by hand with Orca). Not a
  bridge bug. Recovery over ssh: ask the window manager to activate
  another window (`_NET_ACTIVE_WINDOW`, e.g. GdkX11
  `X11Window.foreign_new_for_display(...).focus(0)`).
- Cause, a marco bug: Wine's toplevels take focus only through
  `WM_TAKE_FOCUS` (input hint False). When notepad is unmanaged,
  `focus_ancestor_or_top_window` passes `NULL` instead of `not_this_one`
  to `meta_stack_get_default_focus_window` (`src/core/workspace.c`, since
  marco f96255be), picks the dying window and sends it `WM_TAKE_FOCUS`.
  metacity and mutter pass `not_this_one`. A marco 1.29.0 build with that
  change fixes it (automatic test and Orca by hand). Reported as
  mate-desktop/marco#847, fix in PR #848 (2026-11-09); related #703.

Found with changing texts and winecfg (`tests/bridge-controls.sh`,
`tests/bridge-propsheet.sh`, Wine 11.21, 2026-11-18).

- A `Static` passes `WM_SETTEXT` to `DefWindowProc` (`user32/static.c`),
  which notifies `EVENT_OBJECT_NAMECHANGE` on `OBJID_WINDOW`
  (`win32u/defwnd.c`). The bridge refreshes the window tree on it, so a
  label that changes without focus (progress dialog text; "Run" in
  `tests/controls.c`) emits `accessible-name` and Where Am I and flat
  review read the new text. It is not spoken: Orca presents a name
  change only for the locus of focus or a frame
  (`is_presentable_name_change`), NVDA only for the focus object
  (`NVDAObject.event_nameChange`). Left so.
- winecfg is a property sheet: each page is a child `#32770`, a panel
  in the tree, and Ctrl+Page Down hides one page and shows the next
  without a new top-level window. Labels sent with a focus went below
  the frame until the next tree moved them into the page, so the focus
  carries labels only when it enters a new top-level window; in a known
  one the tree places them (`on_focus`).

Found with a walk (Go) program and `tests/msgbox-race.c` (Wine 11.21,
2026-11-17).

- A main window shown with `SW_SHOWNA` and a `MessageBoxW` it owns
  opened right away: in WSLg the box loses the activation to its
  disabled owner and gets no keys (0 of 5 runs work, with or without
  the bridge). The explorer desktop thread's `set_foreground_window`
  posts `WM_WINE_SETACTIVEWINDOW` to the app thread; the box's modal
  loop delivers it late and `win32u/message.c` activates the owner
  without checking that it is disabled now. `SW_SHOW`, or the box 1 s
  later, works. In the Arch image under MATE and GNOME 5 of 5 runs
  work. Not a bridge bug. Reported as WineHQ bug 60809
  (https://bugs.winehq.org/show_bug.cgi?id=60809, 2026-11-17, with
  `tests/msgbox-race.c` attached). Fixed on the
  application side in walk (`FormBase.SetVisible` shows an enabled
  top-level form with `SW_SHOW` the first time, lpintes/walk 6e4e3fa).

## Open questions (to verify by experiment)

- How good are Wine's `oleacc` proxies for the other standard controls
  (list views, tree views, combo boxes, dialogs)?
- Does the Wine UIA client work across processes with a program that has
  its own UIA provider (tree, `TextPattern`, focus and property-change
  events)? Plan: write a small test program that implements
  `IRawElementProviderSimple` (and a `TextPattern`) and answers
  `WM_GETOBJECT` / `UiaRootObjectId`, then read it from the probe through
  `IUIAutomation`.
- Move heuristics into the hook DLL (idea, 2026-11-08). With code in the
  target, like NVDA's nvdaHelper:
  - the key check could run right after the control handled the key
    (`WH_CALLWNDPROCRET` on `WM_KEYDOWN` / `WM_CHAR`) instead of a 50 ms
    timer with retries for a busy target;
  - parent notifications show what Wine's WinEvents miss:
    `LVN_ITEMCHANGED`, `TVN_SELCHANGED`, `TCN_SELCHANGE`,
    `CBN_SELCHANGE`, `EN_CHANGE`, `WM_HSCROLL` / `WM_VSCROLL`
    (caret moves still need a look after the key);
  - list view, tree view and winefile items read without
    `VirtualAllocEx` / `ReadProcessMemory`, 32-bit layouts or send
    timeouts.

  Costs: a bug crashes the target, 32-bit targets need their own DLL. Keep
  the out-of-process path as fallback and for comparison with NVDA on
  Windows. The off-screen model needs the same DLL.

## Winelib pitfalls

- Win32 modules are built with `-mno-cygwin` (Wine's msvcrt headers). In
  the resulting ELF both `msvcrt` and `ucrtbase` get loaded, and `wmain`
  then receives a broken `argv`. Use `main` with `GetCommandLineW` and
  `CommandLineToArgvW`, and write output with `WriteFile` instead of CRT
  stdio.
- On x86_64 `va_list` is the ms_abi one. A variadic function that passes
  its `va_list` to Win32 (`wvsprintfW`) must be declared `WINAPIV`,
  otherwise it crashes.
- Wine's msvcrt `<vadefs.h>` makes `va_list` the ms_abi one
  (`__builtin_ms_va_list`) on x86_64, so `va_start` in an ordinary (sysv)
  variadic function reads garbage. The embedded Lua crashed in
  `luaO_pushvfstring`; `bridge/luauser.h` gives Lua GCC's sysv `va_list`,
  which it passes to no msvcrt function.
- `memset` / `memcpy` / `memmove` bind to Wine's ucrtbase (ms_abi). Calls
  written in the source use the ms_abi declarations of `<string.h>` and
  work, but calls GCC makes up itself (a loop turned into `memset`, a large
  struct copy) use the sysv ABI and crash (Lua's `tablerehash`, 2026-11-18).
  `STRINGOP_FLAGS` in the `Makefile` inline them for all winegcc code, and
  `tests/abi-check.sh` fails the link on a sysv call (an ms_abi one sets
  the size in `r8`). The same binding is a reason why `atspi.c` (host gcc)
  must not call libc.
- Lua raises errors with `__builtin_setjmp` / `__builtin_longjmp`
  (`bridge/luauser.h`): Wine's x86_64 `longjmp` unwinds with SEH, which
  need not work across ELF frames (not tested, avoided).
- Wine's `libuuid` lacks `IID_IAccessible`; include `<initguid.h>` before
  `<oleacc.h>` in one source file.
- `WINE_ATSPI_DEBUG=1` makes the bridge trace focus events and key checks
  to stderr (`tests/bridge-notepad.sh` sets it; see `build/bridge.log`).
  Non-ASCII names show as mojibake there, because `%S` uses the ANSI code
  page.
- In the test prefix the crash dialog is off
  (`HKCU\Software\Wine\WineDbg\ShowCrashDialog=0`); backtraces go to the
  terminal.

## Environment

- **Build in Arch WSL** (distro `archlinux`), using `winegcc` from the
  `wine` package, and `x86_64-w64-mingw32-gcc` (`mingw-w64-gcc`) for the
  menu hook DLL `build/wine-atspi-hook.dll`, which must lie next to
  `wine-atspi.exe`. The source lives on the Windows side at
  `C:\vcs\github.com\lpintes\wine-atspi` (`/mnt/c/vcs/github.com/lpintes/wine-atspi`
  in WSL).
- **Unit tests** of the pure helpers (`bridge/utf16.c` in the core,
  `bridge/textrange.c` on the AT-SPI side) build natively with the host
  gcc, no Wine and no window: `make check` in WSL (`tests/unit.c`).
  Code that needs no Windows API or D-Bus goes into such files.
- **Integration tests in the Arch VM**, not in WSLg: a test window in
  WSLg takes the focus on the user's Windows desktop, and keys the user
  types meanwhile reach the target (seen: W, Left, Down in test logs).
  The user starts the VM and logs in on its console and runs `run-mate`;
  then from Git Bash `tests/vm.sh bridge-menu bridge-wordpad ...` copies
  the repository and `build/` programs to `~/wine-atspi` in the VM,
  stops Orca (unless `--orca`), runs the tests in the MATE session's
  display and D-Bus and fetches the logs to `build/vm/`. `--no-copy`
  skips the copy. Verify automatically: a `python-atspi` script dumps
  the AT-SPI tree and events and compares them with expectations; most
  tests only log, so read the log. Do not ask the user to listen to
  every iteration. Window sizes differ from WSLg (notepad 760x570), so
  extents and visible lines do too. Controls missing from Wine's own
  programs are tested with `tests/controls.c` (window class
  `WineAtspiControls`) and `tests/bridge-controls.sh`. The test scripts
  still run in WSL as well.
- **Tests with Orca in the Arch image** `C:\linux\arch.img`. Its build
  scripts and rules are in `C:\Users\pintes\pokus\archfile` (read its
  `CLAUDE.md` before touching the image).
  - Run it in native QEMU from Git Bash with `C:\linux\run.sh` (default
    `-snapshot`, ssh on port 2222). Start the desktop with Orca using
    `run-gnome` or `run-mate`.
  - In WSL the image can be mounted at `/mnt/arch`. Never write to it while
    QEMU runs.
  - Into the image itself (QEMU off, so the files survive `-snapshot`)
    copy with `build/copy-to-image.sh` (untracked helper: loop-mounts
    the image at `/mnt/arch`, copies the bridge, hook DLL, controls, `scripts/` and
    `bridge.sh` to `~/wine-atspi`, unmounts). Run as root from Git Bash:
    `export MSYS_NO_PATHCONV=1; wsl.exe -d archlinux -u root -- bash
    /mnt/c/vcs/github.com/lpintes/wine-atspi/build/copy-to-image.sh`.
    Do not pass `$VAR` inline through `wsl.exe ... bash -c '...'`; use a
    script file.
  - Into a running VM copy with `bash build/copy-to-vm.sh` from Git Bash
    (untracked helper; `scp` to `~/wine-atspi`). It uses the host alias
    `arch-vm` in `~/.ssh/config`: key `~/.ssh/arch-vm` without passphrase,
    installed in the image. The main `id_rsa` has a passphrase and no agent
    runs, so it cannot be used from here.
  - For a manual test with Orca in the running VM, `bash build/vm-orca.sh
    PROGRAM` from Git Bash (untracked helper) takes the MATE session's
    environment from `/proc`, starts Orca if it does not run, then
    `bridge.sh PROGRAM`; the trace is `~/wine-atspi/build/bridge.log` in
    the VM.
  - Never overwrite a running bridge's `.so` in place: the bridge crashes
    and stays a zombie that keeps the `WineAtspiBridge` mutex, so every
    new bridge exits and programs run without it (seen 2026-11-10;
    `wineserver -k` cleans up). `copy-to-vm.sh` copies to a staging
    directory and renames into place. A bridge that already runs keeps
    the old, deleted file (`/proc/<pid>/maps` says `(deleted)`), so
    `wineserver -k` in the VM after copying, else Orca still talks to
    the old code (seen 2026-11-10).
  - Final checks: native boot through `restart-do-archu.cmd`.
- **Keep the Wine version identical** in WSL and in the image
  (`pacman -Q wine` on both). A Winelib binary is tied to the Wine it was
  built against. As of 2026-11-05 both are at 11.21.
- `sudo` in Arch WSL asks for a password. For package management run
  `wsl.exe -d archlinux -u root -- ...` instead.
- Upgrading the mounted image through `arch-chroot /mnt/arch` works but is
  slow (about 20 minutes for 300 packages), because the image sits on NTFS
  behind the WSL file bridge. Prefer upgrading inside the VM
  (`C:\linux\run.sh -w`).
- From Git Bash, `export MSYS_NO_PATHCONV=1` before passing Linux paths to
  `wsl.exe`.

## Milestones

1. Probe: a small Win32 program in the target prefix logs focus changes
   (window class, MSAA name and role) and, on each key, the caret position
   and the current line of an `Edit`. Output goes to the console. This
   answers the open questions.
2. Minimal AT-SPI side: register one application per Wine process and
   expose the focused object (name, role, text) with focus events. Check it
   with a `python-atspi` dump, then with Orca.
3. Standard controls one by one, each with a handler and an AT-SPI
   interface (`Text`, `Selection`, `Value`, ...).
4. Menus.
5. Application scripts (per-executable fixes, winefile first). The
   scripting itself and the winefile file names are done.
6. Scintilla.
7. Other popular controls; the off-screen model if a needed program
   requires it.

Milestones 1 to 4 are done (menus for 64-bit targets only). The planned
steps and known gaps are listed in `TODO.md`.
