#!/bin/bash
# Run the bridge against Wine's wordpad (RichEdit20W) and log the AT-SPI
# events it emits.
# Usage: tests/bridge-wordpad.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-wordpad.log}
BUILD=build

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 22 > "$LOG" 2>&1 &
events=$!
sleep 1
# The prefix has no App Paths entry for wordpad, so "wine wordpad" fails.
wine 'C:\Program Files\Windows NT\Accessories\wordpad.exe' &
sleep 3

# A RichEdit counts a paragraph end as one character in EM_GETSEL, while
# WM_GETTEXT gives "\r\n": caret offsets on the second line must still
# match the text ("second line" starts at offset 6). Up, ctrl+left and
# ctrl+right move the caret; shift+home selects "Hello", backspace
# deletes it. Alt+F4 asks to save; Right and Return choose "No" and end
# wordpad: its application must leave the desktop.
timeout 40 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:500 \
    t:"Hello" w:300 k:return w:300 t:"second line" w:300 \
    k:home w:300 k:right w:300 k:up w:300 k:ctrl+right w:300 k:ctrl+left w:300 \
    k:end w:300 k:shift+home w:300 k:back w:300 k:ctrl+end w:300 \
    k:alt+f4 w:1500
python3 tests/atspi-dump.py wordpad > $BUILD/dump-running.log 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:right w:300 k:return w:2000

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
