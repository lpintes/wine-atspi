#!/bin/bash
# Run the bridge against Wine's notepad and log the AT-SPI events it emits.
# Usage: tests/bridge-notepad.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-notepad.log}
BUILD=build

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 25 > "$LOG" 2>&1 &
events=$!
sleep 1
wine notepad &
sleep 3

# Typing and caret keys in the Edit must give text-changed and
# text-caret-moved events (no WinEvent for the caret, found by the key
# check); shift+home selects "Hello", backspace deletes it. Ctrl+O opens the Open dialog; Tab moves
# through its controls; the file name entry and the file type combo box
# must be named by their labels. Down in the file type combo box must
# change its value.
# The dialog is opened twice: once closed, it must leave the tree.
# Alt+F4 must focus a button of the "save changes?" message box.
timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 \
    t:"Hello" w:300 k:return w:300 t:"second line" w:300 \
    k:up w:300 k:ctrl+left w:300 k:ctrl+right w:300 k:shift+home w:300 \
    k:back w:300 k:ctrl+end w:300 \
    k:ctrl+o w:1500 k:tab w:500 k:down w:500 k:up w:500 \
    k:tab w:500 k:tab w:500 \
    k:escape w:1000 t:" world" w:500 \
    k:ctrl+o w:3000 k:escape w:1000 \
    k:alt+f4 w:1500 k:right w:500

wait $events
python3 tests/atspi-dump.py notepad >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
