#!/bin/bash
# Run the bridge against tests/controls.c and click its buttons through
# the AT-SPI Action interface, as Orca's object navigator does it.
# Usage: tests/bridge-action.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.
#
# Expected: Run and Close list the action "click", list items and other
# controls none; "Run" fills the progress bar, which then appears below
# the frame; "Close" closes the window and the application leaves.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-action.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

wine $BUILD/controls.exe.so &
sleep 3
{
    echo "--- controls"
    python3 tests/atspi-dump.py -v controls
    echo "--- click Run"
    python3 tests/atspi-dump.py --click Run controls
    sleep 3
    python3 tests/atspi-dump.py -v controls
    echo "--- click Close"
    python3 tests/atspi-dump.py --click Close controls
    sleep 2
    echo "--- applications"
    python3 tests/atspi-dump.py
} > "$LOG" 2>&1

kill $bridge
wineserver -k
wait
echo "log: $LOG"
