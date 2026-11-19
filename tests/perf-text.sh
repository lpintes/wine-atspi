#!/bin/bash
# Measure what the key check costs in notepad with a large text: after
# each key the bridge reads the whole text of the Edit again. Opens files
# of several sizes, presses Down and types characters, and keeps the key
# check lines of the trace (caret, text length, messages, ms).
# Usage: tests/perf-text.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/perf-text.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
: > "$LOG"

# A file of about $1 KB in lines of 64 characters.
make_text() {
    local line="The quick brown fox jumps over the lazy dog, line number"
    awk -v n=$(( $1 * 1024 / 64 )) -v s="$line" \
        'BEGIN { for (i = 1; i <= n; i++) printf "%s %05d\r\n", s, i }' > "$2"
}

for kb in 10 100 1000; do
    file=$BUILD/perf-text-$kb.txt
    make_text $kb $file
    wine notepad "$(winepath -w $file)" &
    sleep $(( 3 + kb / 200 ))
    start=$(wc -l < $BUILD/bridge.log)
    keys="k:down w:500 k:down w:500 k:down w:500"
    timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 $keys t:abc w:1000 \
        k:ctrl+end w:1500 t:xyz w:1500
    echo "--- $kb KB" >> "$LOG"
    tail -n +$((start + 1)) $BUILD/bridge.log | grep -a '^key check\|busy' >> "$LOG"
    # Alt+F4 asks to save; Right and Return choose "No".
    timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:300 k:alt+f4 w:1500 k:right w:300 \
        k:return w:2000
done

kill $bridge
wineserver -k
wait
cat "$LOG"
