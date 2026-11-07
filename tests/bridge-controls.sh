#!/bin/bash
# Run the bridge against tests/controls.c and log the AT-SPI events it emits.
# Usage: tests/bridge-controls.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-controls.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 24 > "$LOG" 2>&1 &
events=$!
sleep 1
wine $BUILD/controls.exe.so &
sleep 2

# "Fruits:" list: focus on item 'Apple' 1/3 (no selection yet); Down must
# focus and select 'Banana' 2/3.
# Tab to "Empty:": the list itself, list box with setsize 0.
# Tab to "Colors:" (extended selection): Down, then shift+down must select
# 'Blue' as well.
# Tab to "Files:" (report view): the table itself, 3x3, no focused row;
# Down must focus cell 'readme.txt' 0,0 with row ['readme.txt', '2 KB',
# 'Text'] and headers ['Name', 'Size', 'Type'], Down then 'notes.md'.
# Tab to "Places:" (tree): Wine selects the first item, so focus on tree
# item 'Home' 1/3 level=1 collapsed in tree 'Places:'; Right expands it
# (state-changed:expanded), Down 'Documents' 1/2 level=2, Down 'Music'
# 2/2, Left back to 'Home', Left collapses it, Down 'Work' 2/3.
# Escape closes the window.
timeout 40 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 \
    k:down w:500 k:down w:500 \
    k:tab w:500 \
    k:tab w:500 k:down w:500 k:shift+down w:500 \
    k:tab w:500 k:down w:500 k:down w:500 \
    k:tab w:500 k:right w:500 k:down w:500 k:down w:500 \
    k:left w:500 k:left w:500 k:down w:500
python3 tests/atspi-dump.py controls > $BUILD/dump-running.log 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1000

wait $events
{
    echo "--- tree before exit"
    cat $BUILD/dump-running.log
} >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
