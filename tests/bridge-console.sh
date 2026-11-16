#!/bin/bash
# Run the bridge against a Wine console window (wineconsole cmd) and log
# the AT-SPI events it emits.
# Usage: tests/bridge-console.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-console.log}
BUILD=build
SK="timeout 60 wine $BUILD/sendkeys.exe.so"

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 25 > "$LOG" 2>&1 &
events=$!
sleep 1
wine wineconsole cmd &
sleep 4

# conhost drops KEYEVENTF_UNICODE input, so type with real keys:
# letters, digits and spaces only.
keys() {
    local s=$1 i c out=
    for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        if [ "$c" = " " ]; then out+=" k:space"; else out+=" k:$c"; fi
    done
    echo "$out"
}

# Typed characters and command output must come as text-changed:insert
# on the terminal, Left / Home / End as caret moves, Back as a delete.
$SK f:WineConsoleClass w:500 $(keys "echo hello") w:500 k:return w:1000
$SK $(keys "dir") w:300 k:return w:2000
# A second listing scrolls the first lines out of the window, so the dump
# shows them far above it.
$SK $(keys "dir") w:300 k:return w:2000
$SK $(keys "abc") w:300 k:left w:300 k:left w:300 k:home w:300 k:end w:300 \
    k:back w:500
python3 tests/atspi-dump.py -v cmd > $BUILD/dump-running.log 2>&1
$SK k:back w:300 k:back w:300 $(keys "exit") w:300 k:return w:2000

wait $events
{
    echo "--- tree before exit"
    cat $BUILD/dump-running.log
    echo "--- applications after exit"
    python3 tests/atspi-dump.py
} >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
