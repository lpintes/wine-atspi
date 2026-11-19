#!/bin/bash
# Run the bridge against notepad with a text above TEXT_MAX_CHARS (1,000,000
# UTF-16 units, bridge/text.c): 24576 lines of 64 units ("\r\n"
# included), 1.5M units, 1548288 AT-SPI characters. Each step checks the
# caret, the character count and the caret's line through AT-SPI against
# the real text, so the test fails while the bridge cuts the text off.
# The key check lines of the trace (caret, length, messages, ms) are kept.
# Usage: tests/bridge-text-large.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-text-large.log}
BUILD=build
FILE=$BUILD/text-large.txt
LINES=24576
PREFIX="The quick brown fox jumps over the lazy dog, line number"

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

: > "$LOG"
fail=0

awk -v n=$LINES -v s="$PREFIX" \
    'BEGIN { for (i = 1; i <= n; i++) printf "%s %05d\r\n", s, i }' > $FILE
count=$(( LINES * 63 ))

# step WANTED KEYS...: press the keys, compare the state line.
step() {
    local want=$1 got start
    shift
    start=$(wc -l < $BUILD/bridge.log)
    timeout 30 wine $BUILD/sendkeys.exe.so f:Notepad w:300 "$@" w:1500
    got=$(python3 tests/atspi-text.py notepad 2>&1 | tail -n 1)
    echo "keys: $*" >> "$LOG"
    tail -n +$((start + 1)) $BUILD/bridge.log | grep -a '^key check' >> "$LOG"
    echo "$got" >> "$LOG"
    if [ "$got" = "$want" ]; then
        echo "PASS: $*" >> "$LOG"
    else
        echo "FAIL: $*: want $want" >> "$LOG"
        fail=1
    fi
}

wine notepad "$(winepath -w $FILE)" &
sleep 10

step "caret=0 count=$count line='$PREFIX 00001\\n'" k:ctrl+home
step "caret=$count count=$count line=''" k:ctrl+end
step "caret=$(( count - 63 )) count=$count line='$PREFIX $LINES\\n'" k:up
step "caret=$(( count - 60 )) count=$(( count + 3 )) line='abc$PREFIX $LINES\\n'" t:abc
step "caret=0 count=$(( count + 3 )) line='$PREFIX 00001\\n'" k:ctrl+home

# Alt+F4 asks to save; Right and Return choose "No".
timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:300 k:alt+f4 w:1500 k:right w:300 k:return w:2000

kill $bridge
wineserver -k
wait
cat "$LOG"
echo "log: $LOG"
exit $fail
