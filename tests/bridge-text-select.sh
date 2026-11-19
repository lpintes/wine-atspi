#!/bin/bash
# Run the bridge against notepad (Edit) and wordpad (RichEdit), type two
# lines, then move the caret and the selection through AT-SPI as Orca's
# flat review, braille routing or say all do (SetCaretOffset,
# SetSelection, RemoveSelection, AddSelection). Each step checks the
# caret and selection the bridge reads back; typed keys after a move show
# that the control itself moved. The selection on the second line checks
# the offset mapping past a line break ("\r\n" in an Edit, "\r" in a
# RichEdit).
# Usage: tests/bridge-text-select.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-text-select.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

: > "$LOG"
fail=0

# step APP WANTED OP ARGS...: run the operation, compare the state line.
step() {
    local app=$1 want=$2 got
    shift 2
    python3 tests/atspi-select.py "$app" "$@" > $BUILD/text-select.txt 2>&1
    cat $BUILD/text-select.txt >> "$LOG"
    got=$(tail -n 1 $BUILD/text-select.txt)
    if [ "$got" = "$want" ]; then
        echo "PASS: $app $*" >> "$LOG"
    else
        echo "FAIL: $app $*: want $want" >> "$LOG"
        fail=1
    fi
}

# run APP WINDOW_CLASS COMMAND...: start the program, type, run the steps, close it.
run() {
    local app=$1 class=$2
    shift 2
    "$@" &
    sleep 3
    echo "--- $app" >> "$LOG"
    timeout 30 wine $BUILD/sendkeys.exe.so f:$class w:500 t:"hello world" k:return t:"second line" w:1000
    step $app "caret=6 selections=[] text='hello world\\nsecond line'" caret 6
    timeout 10 wine $BUILD/sendkeys.exe.so f:$class w:300 t:X w:800
    step $app "caret=19 selections=[(13, 19)] text='hello Xworld\\nsecond line'" set 13 19
    step $app "caret=19 selections=[] text='hello Xworld\\nsecond line'" remove
    step $app "caret=5 selections=[(0, 5)] text='hello Xworld\\nsecond line'" add 0 5
    timeout 10 wine $BUILD/sendkeys.exe.so f:$class w:300 t:Y w:800
    step $app "caret=1 selections=[] text='Y Xworld\\nsecond line'" caret 1
    # Alt+F4 asks to save; Right and Return choose "No".
    timeout 10 wine $BUILD/sendkeys.exe.so f:$class w:300 k:alt+f4 w:1500 k:right w:300 k:return w:1500
}

run notepad Notepad wine notepad
# The prefix has no App Paths entry for wordpad, so "wine wordpad" fails.
run wordpad WORDPADTOP wine 'C:\Program Files\Windows NT\Accessories\wordpad.exe'

kill $bridge
wineserver -k
wait
cat "$LOG"
echo "log: $LOG"
exit $fail
