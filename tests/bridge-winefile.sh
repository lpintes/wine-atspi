#!/bin/bash
# Run the bridge against Wine's winefile, whose panes are owner-drawn list
# boxes described only by the application script scripts/winefile.lua: the
# folder tree as a tree with levels, the file list as a table. The focused
# items and the dump must have file names, not empty names.
# Usage: tests/bridge-winefile.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-winefile.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
: > "$LOG"
python3 tests/atspi-events.py 25 >> "$LOG" 2>&1 &
events=$!
sleep 1
wine winefile &
sleep 5

# Focus starts in the file list of the current folder (the repository);
# Tab goes to the folder tree, where Return expands the repository's
# folder and collapses it again.
timeout 30 wine $BUILD/sendkeys.exe.so w:1000 k:down w:1000 k:down w:1000 \
    k:tab w:1000 k:return w:1500 k:down w:1000 k:up w:1000 k:return w:1500
wait $events
echo "--- dump" >> "$LOG"
python3 tests/atspi-dump.py winefile >> "$LOG" 2>&1

kill $bridge
wineserver -k
wait
echo "--- check" >> "$LOG"
if grep -q "script .*winefile.lua" $BUILD/bridge.log; then
    echo "script loaded" >> "$LOG"
else
    echo "FAIL: script not loaded" >> "$LOG"
fi
if grep -q "wine-atspi: script" $BUILD/bridge.log; then
    echo "FAIL: script error" >> "$LOG"
    grep -A1 "wine-atspi: script" $BUILD/bridge.log >> "$LOG"
fi
focused="focused(1) app='winefile' src="
items=$(grep -c "$focused\(tree item\|table cell\) '[^']" "$LOG")
levels=$(grep -c "${focused}tree item '[^']*'.* level=[0-9]" "$LOG")
cells=$(grep -c "${focused}table cell '[^']" "$LOG")
unnamed=$(grep -c "$focused[a-z ]*item ''\|${focused}table cell ''" "$LOG")
lists=$(grep -c "${focused}list item" "$LOG")
echo "focused: $items named items, $levels tree items with level, $cells cells," \
     "$unnamed unnamed, $lists list items" >> "$LOG"
[ "$levels" -gt 0 ] || echo "FAIL: no tree item with a level" >> "$LOG"
[ "$cells" -gt 0 ] || echo "FAIL: no table cell" >> "$LOG"
[ "$unnamed" -eq 0 ] && [ "$lists" -eq 0 ] || echo "FAIL: unnamed or list items" >> "$LOG"
grep -q "expanded" "$LOG" || echo "FAIL: no expanded state" >> "$LOG"
echo "log: $LOG"
! grep -q "^FAIL" "$LOG"
