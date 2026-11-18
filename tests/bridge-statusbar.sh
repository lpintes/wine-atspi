#!/bin/bash
# Run the bridge against Wine's regedit and dump its status bar, which
# shows the path of the selected key and changes after each key in the
# tree without a WinEvent.
# Usage: tests/bridge-statusbar.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-statusbar.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
wine regedit &
sleep 3

# Focus starts in the tree on the root item; the status bar shows its
# path. Down selects HKEY_CLASSES_ROOT, Down again HKEY_CURRENT_USER.
: > "$LOG"
dump() {
    echo "--- $1" >> "$LOG"
    python3 tests/atspi-dump.py regedit >> "$LOG" 2>&1
}
timeout 20 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:1000
dump start
timeout 20 wine $BUILD/sendkeys.exe.so k:down w:1000
dump "after Down"
timeout 20 wine $BUILD/sendkeys.exe.so k:down w:1000
dump "after Down Down"

kill $bridge
wineserver -k
wait
echo "log: $LOG"
