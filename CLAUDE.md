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
    `bridge/main.c`) and asks a list box for `LB_GETCARETINDEX`.
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

- One winegcc ELF can hold both sides: `bridge/main.c` is built with
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
- After an arrow key in the Open dialog's file list the dialog thread is
  busy for 0.3 to over 0.5 s (WSL, folder on `/mnt/c`), so list view
  queries can time out; the key check retries a busy list.
- `atspi.c` uses only GLib functions, never libc ones directly. The
  binary imports msvcrt through Wine, and a libc name could bind to an
  ms_abi msvcrt thunk. Not proven to break, just avoided.

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
  default (`speak-position-in-set`).
- Orca reads text almost only through `Atspi.Text.get_string_at_offset`
  with a granularity (`ax_text.py`), plus caret offset, character count,
  `get_text` and selections. For multi-line text at the end offset it
  expects the empty last line, as GTK3 gives it.

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

## Open questions (to verify by experiment)

- How good are Wine's `oleacc` proxies for the other standard controls
  (list views, tree views, combo boxes, dialogs)?
- Does the Wine UIA client work across processes with a program that has
  its own UIA provider (tree, `TextPattern`, focus and property-change
  events)? Plan: write a small test program that implements
  `IRawElementProviderSimple` (and a `TextPattern`) and answers
  `WM_GETOBJECT` / `UiaRootObjectId`, then read it from the probe through
  `IUIAutomation`.

## Winelib pitfalls

- Win32 modules are built with `-mno-cygwin` (Wine's msvcrt headers). In
  the resulting ELF both `msvcrt` and `ucrtbase` get loaded, and `wmain`
  then receives a broken `argv`. Use `main` with `GetCommandLineW` and
  `CommandLineToArgvW`, and write output with `WriteFile` instead of CRT
  stdio.
- On x86_64 `va_list` is the ms_abi one. A variadic function that passes
  its `va_list` to Win32 (`wvsprintfW`) must be declared `WINAPIV`,
  otherwise it crashes.
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
  `wine` package. The source lives on the Windows side at
  `C:\vcs\github.com\lpintes\wine-atspi` (`/mnt/c/vcs/github.com/lpintes/wine-atspi`
  in WSL).
- **Quick tests in WSL** (Wine runs through WSLg). Verify automatically:
  a `python-atspi` script dumps the AT-SPI tree and events and compares them
  with expectations. Do not ask the user to listen to every iteration.
- **Tests with Orca in the Arch image** `C:\linux\arch.img`. Its build
  scripts and rules are in `C:\Users\pintes\pokus\archfile` (read its
  `CLAUDE.md` before touching the image).
  - Run it in native QEMU from Git Bash with `C:\linux\run.sh` (default
    `-snapshot`, ssh on port 2222). Start the desktop with Orca using
    `run-gnome` or `run-mate`.
  - In WSL the image can be mounted at `/mnt/arch`. Never write to it while
    QEMU runs.
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
5. Scintilla.
6. Other popular controls; the off-screen model if a needed program
   requires it.
