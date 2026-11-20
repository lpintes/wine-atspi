#!/bin/bash
# Run the bridge against the shell's "Browse for Folder" dialog
# (SHBrowseForFolderW, build/browse.exe), old style and new style with an
# edit box: dump the tree and log the events of the first keys.
# Usage: tests/bridge-browse.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-browse.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
: > "$LOG"

# One dialog: events from its start, the tree, then the events of KEYS
# (sendkeys arguments).
browse() {
    local name=$1 keys=$2; shift 2
    python3 tests/atspi-events.py 6 > $BUILD/browse-events.log 2>&1 &
    events=$!
    sleep 1
    wine $BUILD/browse.exe.so "$@" > $BUILD/browse-out.log 2>&1 &
    sleep 4
    wait $events
    {
        echo "=== $name: events of the start"
        cat $BUILD/browse-events.log
        echo "--- $name: tree"
        python3 tests/atspi-dump.py -v browse
    } >> "$LOG" 2>&1
    python3 tests/atspi-events.py 9 > $BUILD/browse-events.log 2>&1 &
    events=$!
    sleep 1
    timeout 20 wine $BUILD/sendkeys.exe.so f:#32770 w:500 $keys
    wait $events
    {
        echo "--- $name: events of $keys"
        cat $BUILD/browse-events.log
    } >> "$LOG" 2>&1
    timeout 10 wine $BUILD/sendkeys.exe.so f:#32770 k:escape w:1000
    echo "--- $name: output: $(cat $BUILD/browse-out.log)" >> "$LOG"
}

keys="k:tab w:1000 k:shift+tab w:1000 k:shift+tab w:1000 k:down w:1000"
browse "old style" "$keys"
browse "new style, edit box" "$keys" new edit
# The common item dialog picking a folder, as the Eureka A4 emulator
# opens it (IFileOpenDialog, FOS_PICKFOLDERS).
browse "item dialog, Tab" "$(for i in 1 2 3 4 5 6 7 8; do echo -n "k:tab w:800 "; done)" item
# In the file list: Down, Down, Return opens a folder, Down.
browse "item dialog, list" "k:tab w:500 k:tab w:500 k:tab w:1000 k:down w:1000 k:down w:1000 k:return w:1500 k:down w:1000" item

kill $bridge
wineserver -k
wait
echo "log: $LOG"
