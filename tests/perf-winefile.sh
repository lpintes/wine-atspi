#!/bin/bash
# Measure how much the bridge slows winefile reading a large folder
# (/usr/bin, thousands of files, one LB_INSERTSTRING each). winefile is
# started in that folder three times in each mode:
#   none     no bridge;
#   idle     bridge without a listener (no hooks installed);
#   hooks    bridge with --always-on (hooks installed, focus reported).
# A run's time is from starting winefile until its frame answers messages,
# measured around sendkeys r:WFS_Frame, so it includes sendkeys' own start.
# Usage: tests/perf-winefile.sh [LOG] [FOLDER]   (from the repository root,
# or by tests/vm.sh)

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/perf-winefile.log}
FOLDER=${2:-/usr/bin}
BUILD=$PWD/build
RUNS=3

: > "$LOG"
echo "folder $FOLDER, $(ls "$FOLDER" | wc -l) entries" >> "$LOG"
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1

for mode in none idle hooks; do
    wineserver -k; wineserver -w
    wineserver -p
    bridge=
    case $mode in
    idle) wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 & bridge=$! ;;
    hooks) wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 & bridge=$! ;;
    esac
    # A first winefile warms up the prefix and the file cache.
    (cd "$FOLDER" && wine winefile > /dev/null 2>&1 &)
    wine $BUILD/sendkeys.exe.so r:WFS_Frame > /dev/null 2>&1
    pkill -f winefile.exe; sleep 2
    for run in $(seq $RUNS); do
        start=$(date +%s%N)
        (cd "$FOLDER" && wine winefile > /dev/null 2>&1 &)
        ready=$(wine $BUILD/sendkeys.exe.so r:WFS_Frame 2>&1 | tr -d '\r')
        end=$(date +%s%N)
        echo "$mode run $run: $(( (end - start) / 1000000 )) ms ($ready)" >> "$LOG"
        pkill -f winefile.exe; sleep 2
    done
    [ -n "$bridge" ] && kill $bridge
done
wineserver -k
wait
cat "$LOG"
