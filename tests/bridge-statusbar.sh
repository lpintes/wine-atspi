#!/bin/bash
# Run the bridge against status bars, which change without a WinEvent:
# - Wine's regedit shows the path of the selected key and changes it
#   after each key in the tree;
# - tests/controls.c shows "Level n %" while "Run" fills its bar, with
#   no key at all. Only the hook DLL tells of that change.
# Usage: tests/bridge-statusbar.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log. Exit code 1 if a
# check failed.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-statusbar.log}
BUILD=build
fail=0

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
# regedit reopens the key it showed last (LastKey,
# programs/regedit/childwnd.c), so that is reset first.
wine reg delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Regedit' \
    /v LastKey /f > /dev/null 2>&1
wine regedit &
sleep 3

: > "$LOG"
# Dumps program $2's tree under heading $1 and prints its status bar line.
dump() {
    echo "--- $1" >> "$LOG"
    python3 tests/atspi-dump.py "$2" > $BUILD/statusbar-dump.txt 2>&1
    cat $BUILD/statusbar-dump.txt >> "$LOG"
    grep 'status bar \[' $BUILD/statusbar-dump.txt
}

# $1 description, $2 status bar line, $3 text it must hold.
check() {
    case "$2" in
    *"$3"*) echo "PASS: $1" >> "$LOG" ;;
    *) echo "FAIL: $1 (got: $2)" >> "$LOG"; fail=1 ;;
    esac
}

# Focus starts in the tree on the root item; the status bar shows its
# path. Down selects HKEY_CLASSES_ROOT, Down again HKEY_CURRENT_USER.
timeout 20 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:1000
dump start regedit > /dev/null
timeout 20 wine $BUILD/sendkeys.exe.so k:down w:1000
check "regedit after Down" "$(dump "after Down" regedit)" "\\HKEY_CLASSES_ROOT'"
timeout 20 wine $BUILD/sendkeys.exe.so k:down w:1000
check "regedit after Down Down" "$(dump "after Down Down" regedit)" "\\HKEY_CURRENT_USER'"
timeout 10 wine $BUILD/sendkeys.exe.so k:alt+f4 w:1000

# controls: Tab 8 times from "Fruits:" to "Run", Space starts it. Its
# timer sets the status bar every second; no key follows.
wine $BUILD/controls.exe.so &
sleep 2
timeout 20 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 \
    k:tab k:tab k:tab k:tab k:tab k:tab k:tab k:tab w:1000
check "controls before Run" "$(dump "controls before Run" controls)" "text='Ready'"
timeout 20 wine $BUILD/sendkeys.exe.so k:space w:3500
check "controls while Run, no key" "$(dump "controls while Run" controls)" "text='Level "
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1000

kill $bridge
wineserver -k
wait
echo "log: $LOG"
exit $fail
