#!/bin/bash
# Measure what a focused tree item costs in a tree view with many
# siblings: regedit's HKEY_CLASSES_ROOT (319 subkeys in a fresh prefix,
# thousands with programs installed). The bridge traces each focus with
# its place ("2 of 319 level 3"), the number of messages it sent and its
# time, each tree of items likewise; the log keeps those lines.
# Usage: tests/perf-tree.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/perf-tree.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
wine regedit &
sleep 3

# Focus starts on the root item. Down selects HKEY_CLASSES_ROOT, Right
# expands it, Right again moves to its first subkey; then Down ten times
# (positions 2 to 11 of all subkeys), Page Down twice and Up (a jump
# placed from the items in view; Up one less), End to the last root key
# and Up.
timeout 20 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit w:1000 k:down w:1000 \
    k:right w:3000 k:right w:1000
# The bridge writes its log at its own offset: remember where the keys start.
start=$(wc -l < $BUILD/bridge.log)
keys="k:down w:500 k:down w:500 k:down w:500 k:down w:500 k:down w:500"
timeout 60 wine $BUILD/sendkeys.exe.so f:RegEdit_RegEdit $keys $keys \
    k:pgdn w:1000 k:pgdn w:1000 k:up w:1000 k:end w:2000 k:up w:1000

kill $bridge
wineserver -k
wait
tail -n +$((start + 1)) $BUILD/bridge.log | grep -a '^focus:\|^tree:\|busy' > "$LOG"
cat "$LOG"
