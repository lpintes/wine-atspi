#!/bin/bash
# Run the bridge against tests/controls.c and log the AT-SPI events it emits.
# Usage: tests/bridge-controls.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-controls.log}
BUILD=build

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 16 > "$LOG" 2>&1 &
events=$!
sleep 1
wine $BUILD/controls.exe.so &
sleep 2

# "Fruits:" list: focus on item 'Apple' 1/3 (no selection yet); Down must
# focus and select 'Banana' 2/3.
# Tab to "Empty:": the list itself, list box with setsize 0.
# Tab to "Colors:" (extended selection): Down, then shift+down must select
# 'Blue' as well.
# Escape closes the window.
timeout 30 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 \
    k:down w:500 k:down w:500 \
    k:tab w:500 \
    k:tab w:500 k:down w:500 k:shift+down w:500
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
