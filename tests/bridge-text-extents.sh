#!/bin/bash
# Run the bridge against notepad (Edit) and wordpad (RichEdit20W) with 60
# lines, more than fit, and dump each line's range rect, as Orca's flat
# review asks for it: visible lines inside the text's rect, lines scrolled
# out far above (y about -100000) or below (about 100000) the window.
# Usage: tests/bridge-text-extents.sh [LOG]   (run from the repository root in WSL)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-text-extents.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

lines=()
for i in $(seq 1 60); do
    lines+=(t:"line $i word" k:return)
done

# notepad: at the end of 60 lines the first ones are scrolled out above;
# after Ctrl+Home the last ones are out below.
wine notepad &
sleep 3
timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 "${lines[@]}" w:1000
{
    echo "--- notepad at the end"
    python3 tests/atspi-dump.py -v notepad
} > "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:300 k:ctrl+home w:1000
{
    echo "--- notepad at the start"
    python3 tests/atspi-dump.py -v notepad
} >> "$LOG" 2>&1
# Alt+F4 asks to save; Right and Return choose "No".
timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:300 k:alt+f4 w:1500 k:right w:300 k:return w:1500

# The prefix has no App Paths entry for wordpad, so "wine wordpad" fails.
wine 'C:\Program Files\Windows NT\Accessories\wordpad.exe' &
sleep 3
timeout 60 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:500 "${lines[@]}" w:1000
{
    echo "--- wordpad at the end"
    python3 tests/atspi-dump.py -v wordpad
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:300 k:ctrl+home w:1000
{
    echo "--- wordpad at the start"
    python3 tests/atspi-dump.py -v wordpad
} >> "$LOG" 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:300 k:alt+f4 w:1500 k:right w:300 k:return w:1500

kill $bridge
wineserver -k
wait
echo "log: $LOG"
