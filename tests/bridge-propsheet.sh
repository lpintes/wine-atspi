#!/bin/bash
# Run the bridge against winecfg, a property sheet whose pages are child
# dialogs (#32770 inside #32770), and dump the window trees it builds.
# Usage: tests/bridge-propsheet.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-propsheet.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

# Each page is a panel below the frame; the texts of a page that label no
# control are labels below that panel, not below the frame, and no label
# is there twice.
wine winecfg &
sleep 4
{
    echo "--- winecfg, first page"
    python3 tests/atspi-dump.py -v winecfg
} > "$LOG" 2>&1
# Ctrl+Page Down shows the next page: the labels of the old page leave
# with it, the new page's labels are below its own panel.
python3 tests/atspi-events.py 5 > $BUILD/propsheet-events.log 2>&1 &
events=$!
sleep 1
timeout 10 wine $BUILD/sendkeys.exe.so f:#32770 w:500 k:ctrl+pgdn w:3000
wait $events
{
    echo "--- events of Ctrl+Page Down"
    cat $BUILD/propsheet-events.log
    echo "--- winecfg, second page"
    python3 tests/atspi-dump.py -v winecfg
} >> "$LOG" 2>&1
# Ctrl+Tab with the focus in a control of the page (not on the tabs)
# turns the page too (PROPSHEET_IsDialogMessage): 'Libraries' loses
# selected, 'Graphics' gets it.
python3 tests/atspi-events.py 5 > $BUILD/propsheet-events.log 2>&1 &
events=$!
sleep 1
timeout 10 wine $BUILD/sendkeys.exe.so f:#32770 w:500 k:ctrl+tab w:3000
wait $events
{
    echo "--- events of Ctrl+Tab"
    cat $BUILD/propsheet-events.log
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1000

kill $bridge
wineserver -k
wait
echo "log: $LOG"
