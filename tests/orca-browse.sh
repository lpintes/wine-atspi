#!/bin/bash
# What Orca says in the folder picker of the common item dialog
# (build/browse.exe item, as the Eureka A4 emulator opens it). Runs Orca
# with a debug file and the bridge without --always-on, sends keys, and
# keeps Orca's speech lines. Run by tests/vm.sh with --orca (Orca stopped
# by vm.sh would not matter: this script starts its own).
# Usage: tests/orca-browse.sh [LOG]
# Orca's whole debug log goes to build/orca-browse-debug.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/orca-browse.log}
BUILD=build
DEBUG=$BUILD/orca-browse-debug.log

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

pkill -x orca; sleep 1
rm -f "$DEBUG"
setsid orca --debug-file="$PWD/$DEBUG" > /dev/null 2>&1 < /dev/null &
sleep 5

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

wine $BUILD/browse.exe.so item > $BUILD/browse-out.log 2>&1 &
sleep 5
# Tab through the dialog once, then into the file list and down.
timeout 30 wine $BUILD/sendkeys.exe.so f:#32770 w:500 \
    k:tab w:1500 k:tab w:1500 k:tab w:1500 k:tab w:1500 k:tab w:1500 \
    k:tab w:1500 k:tab w:1500 k:tab w:1500 k:tab w:1500 \
    k:down w:1500 k:down w:1500
timeout 10 wine $BUILD/sendkeys.exe.so f:#32770 k:escape w:1000

kill $bridge
wineserver -k
pkill -x orca
wait 2> /dev/null
grep -E "SPEECH OUTPUT:|KEYBOARD_EVENT: *Key '|^ *event_string" "$DEBUG" \
    | grep -E "SPEECH OUTPUT|KEYBOARD" | sed -E "s/\{'(family|established).*//" > "$LOG"
echo "log: $LOG"
