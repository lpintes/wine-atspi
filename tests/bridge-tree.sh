#!/bin/bash
# Run the bridge against tests/controls.c and notepad and dump the window
# trees it builds for object navigation and flat review.
# Usage: tests/bridge-tree.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-tree.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

# controls: every control of the window below the frame in tab order,
# labels named, each with a rect inside the window and showing; the
# focused list item below its list box.
wine $BUILD/controls.exe.so &
sleep 3
{
    echo "--- controls"
    python3 tests/atspi-dump.py -v controls
} > "$LOG" 2>&1
# The Files table with rows 1 and 2 selected: Selection lists both, and
# the cells at the table's corners span all three rows.
timeout 10 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 k:tab k:tab k:tab w:500 \
    k:down w:300 k:down w:300 k:shift+down w:1000
{
    echo "--- controls, Files rows 1-2 selected"
    python3 tests/atspi-dump.py -v controls
} >> "$LOG" 2>&1
# End scrolls the table to its last row: the visible cells start below
# row 0, the rows scrolled away are no longer showing.
timeout 10 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 k:end w:1000
{
    echo "--- controls, Files scrolled to the end"
    python3 tests/atspi-dump.py -v controls
} >> "$LOG" 2>&1
# The list boxes have Selection: Colors (extended) two items, Fruits
# (single) one.
timeout 10 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 k:shift+tab w:300 \
    k:down w:300 k:shift+down w:300 k:shift+tab w:300 k:shift+tab w:300 k:down w:1000
{
    echo "--- controls, Colors and Fruits selected"
    python3 tests/atspi-dump.py -v controls
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 k:escape w:1000

# notepad: frame > text (and the menu bar once a menu was open).
# The Open dialog: its own frame with the nested windows of the dialog
# (combo boxes, the file list, buttons); after Escape it must be gone.
wine notepad &
sleep 3
{
    echo "--- notepad"
    python3 tests/atspi-dump.py -v notepad
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:500 k:ctrl+o w:3000
{
    echo "--- notepad with the Open dialog"
    python3 tests/atspi-dump.py -v notepad
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1500
{
    echo "--- notepad after Escape"
    python3 tests/atspi-dump.py -v notepad
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:500 k:alt+f4 w:2000

kill $bridge
wineserver -k
wait
echo "log: $LOG"
