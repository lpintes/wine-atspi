#!/bin/bash
# Run the bridge against notepad with and without word wrap and against
# wordpad, type one long paragraph and a short line, and dump the lines
# Orca would get (Text.GetStringAtOffset with LINE). A wrapping control
# gives the paragraph as several lines, each running to the start of the
# next; notepad without wrap gives it as one line.
# Usage: tests/bridge-text-wrap.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-text-wrap.log}
BUILD=build

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2

words=""
for i in $(seq 1 40); do
    words="$words word$i"
done
keys=(t:"start$words" k:return t:"end")
: > "$LOG"
fail=0

# The "line" entries of the dump of program's multi-line text object.
dump_lines() {
    python3 tests/atspi-dump.py -v "$1" |
        awk '/^ *text \[/ { on = 1; next } on && /^ +line [0-9]+-[0-9]+ / { print; next } { on = 0 }'
}

check() {
    if [ "$2" = 1 ]; then
        echo "PASS: $1" >> "$LOG"
    else
        echo "FAIL: $1" >> "$LOG"
        fail=1
    fi
}

# notepad keeps its word wrap setting in fWrap (programs/notepad/main.c).
notepad() {
    wine reg add 'HKCU\Software\Microsoft\Notepad' /v fWrap /t REG_DWORD /d "$1" /f > /dev/null 2>&1
    wine notepad &
    sleep 3
    timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 "${keys[@]}" w:1000
    dump_lines notepad > $BUILD/text-wrap-lines.txt
    {
        echo "--- notepad fWrap=$1"
        cat $BUILD/text-wrap-lines.txt
    } >> "$LOG"
    # Alt+F4 asks to save; Right and Return choose "No".
    timeout 10 wine $BUILD/sendkeys.exe.so f:Notepad w:300 k:alt+f4 w:1500 k:right w:300 k:return w:1500
}

# Number of lines, and whether every line but the last ends in a space or "\n".
count_lines() { wc -l < $BUILD/text-wrap-lines.txt; }
ends_ok() {
    head -n -1 $BUILD/text-wrap-lines.txt | grep -vcE "line [0-9]+-[0-9]+ '.*( |\\\\n)' "
}
# Lines whose range rect has width 0 (flat review would place no words).
zero_wide() { grep -cE ' rect=-?[0-9]+,-?[0-9]+ 0x' $BUILD/text-wrap-lines.txt; }

notepad 1
n=$(count_lines)
check "notepad wrapped: $n lines, more than 2" $([ "$n" -gt 2 ] && echo 1)
check "notepad wrapped: lines end in a space or break" $([ "$(ends_ok)" = 0 ] && echo 1)
check "notepad wrapped: no line 0 wide" $([ "$(zero_wide)" = 0 ] && echo 1)
grep -q "'end'" $BUILD/text-wrap-lines.txt
check "notepad wrapped: last line 'end'" $([ $? = 0 ] && echo 1)

notepad 0
n=$(count_lines)
check "notepad not wrapped: $n lines, 2" $([ "$n" = 2 ] && echo 1)

# The prefix has no App Paths entry for wordpad, so "wine wordpad" fails.
wine 'C:\Program Files\Windows NT\Accessories\wordpad.exe' &
sleep 3
timeout 60 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:500 "${keys[@]}" w:1000
dump_lines wordpad > $BUILD/text-wrap-lines.txt
{
    echo "--- wordpad"
    cat $BUILD/text-wrap-lines.txt
} >> "$LOG"
timeout 10 wine $BUILD/sendkeys.exe.so f:WORDPADTOP w:300 k:alt+f4 w:1500 k:right w:300 k:return w:1500
n=$(count_lines)
check "wordpad: $n lines, more than 2" $([ "$n" -gt 2 ] && echo 1)
check "wordpad: lines end in a space or break" $([ "$(ends_ok)" = 0 ] && echo 1)
check "wordpad: no line 0 wide" $([ "$(zero_wide)" = 0 ] && echo 1)

wine reg delete 'HKCU\Software\Microsoft\Notepad' /v fWrap /f > /dev/null 2>&1
kill $bridge
wineserver -k
wait
cat "$LOG"
echo "log: $LOG"
exit $fail
