#!/bin/bash
# Run the bridge against tests/controls.c and notepad and dump the window
# trees it builds for object navigation and flat review.
# Usage: tests/bridge-tree.sh [LOG]   (from the repository root, or by tests/vm.sh)
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

# regedit: the first subkey of HKEY_CLASSES_ROOT focused, then Ctrl+End
# scrolls the tree to its end without moving the focus. The focused item
# stays, not showing and without its old rect; Ctrl+Home brings it back
# with a rect. Ctrl+End and Tab then move the focus to the value list while
# the subkey is scrolled away: its node must leave with the next tree, so
# no tree item is left without showing. The tree's Selection gives the
# focused subkey while it is scrolled away too. Only the focused object,
# the first item lines, the tree's selected children and that count are
# kept. regedit reopens the key it showed
# last (LastKey, programs/regedit/childwnd.c), so that is reset first.
wine reg delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Regedit' \
    /v LastKey /f > /dev/null 2>&1
wine regedit &
sleep 3
timeout 20 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:1000 k:down w:1000 \
    k:right w:3000 k:right w:1000
for step in "first subkey focused:" "Ctrl+End:k:ctrl+end" "Ctrl+Home:k:ctrl+home" \
            "Ctrl+End, Tab:k:ctrl+end w:1000 k:tab"; do
    name=${step%%:*} keys=${step#*:}
    [ -n "$keys" ] && timeout 10 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:500 $keys w:1500
    {
        echo "--- regedit, $name"
        python3 tests/atspi-dump.py -v regedit > $BUILD/regedit-dump.txt
        grep -a ' focused' $BUILD/regedit-dump.txt
        grep -a 'tree item' $BUILD/regedit-dump.txt | head -2
        grep -a -A1 '^ *tree \[' $BUILD/regedit-dump.txt | grep -a 'selected children'
        echo "tree items not showing: $(grep -a 'tree item' $BUILD/regedit-dump.txt | grep -vc showing)"
        grep -a 'tree item' $BUILD/regedit-dump.txt | grep -v showing
    } >> "$LOG" 2>&1
done
timeout 10 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:500 k:alt+f4 w:2000

kill $bridge
wineserver -k
wait
echo "log: $LOG"
