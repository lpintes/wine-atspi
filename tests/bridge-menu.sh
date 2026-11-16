#!/bin/bash
# Run the bridge against Wine's notepad and log the AT-SPI events of its
# menus, reported by the menu hook DLL (build/wine-atspi-hook.dll).
# Usage: tests/bridge-menu.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-menu.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 30 > "$LOG" 2>&1 &
events=$!
sleep 1
wine notepad &
sleep 3

# F10 focuses the first menu bar item (menu bar ancestor). Down opens its
# menu and focuses the first item (the bar item as expanded menu
# ancestor), Down moves on, with shortcuts. Right goes to the next menu
# and its first item, again Right to the third. Escape closes the menu:
# the bar item gets focus again; Escape again ends the menu and the Edit
# gets focus back. Shift+F10 opens the Edit's context menu (nothing
# highlighted until Down). Alt+Space opens the system menu.
timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 \
    t:"Hello" w:500 \
    k:f10 w:700 k:down w:700 k:down w:700 k:down w:700 \
    k:right w:700 k:right w:700 k:down w:700 \
    k:escape w:700 k:escape w:1000 \
    k:shift+f10 w:700 k:down w:700 k:down w:700 k:escape w:1000 \
    k:alt+space w:700 k:down w:700 k:escape w:700 k:escape w:1000
python3 tests/atspi-dump.py notepad > $BUILD/dump-running.log 2>&1

wait $events
{
    echo "--- tree"
    cat $BUILD/dump-running.log
} >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
