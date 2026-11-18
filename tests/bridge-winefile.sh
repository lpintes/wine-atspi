#!/bin/bash
# Run the bridge against Wine's winefile, whose panes are owner-drawn list
# boxes named only by the application script scripts/winefile.lua. The
# focused items and the dump must have file names, not empty names.
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

# Focus starts in the folder tree; Tab goes to the file list, Down moves in it.
timeout 20 wine $BUILD/sendkeys.exe.so w:1000 k:down w:1000 k:tab w:1000 \
    k:down w:1000 k:down w:1000
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
named=$(grep -c "focused(1) app='winefile' src=list item '[^']" "$LOG")
unnamed=$(grep -c "focused(1) app='winefile' src=list item ''" "$LOG")
echo "focused list items: $named named, $unnamed unnamed" >> "$LOG"
[ "$named" -gt 0 ] && [ "$unnamed" -eq 0 ] || echo "FAIL: unnamed or no list items" >> "$LOG"
echo "log: $LOG"
! grep -q "^FAIL" "$LOG"
