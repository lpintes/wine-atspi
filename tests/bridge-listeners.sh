#!/bin/bash
# The bridge must watch the programs only while an AT-SPI client listens
# to events: no hooks and no application at start, hooks and the focused
# notepad once a listener registers, nothing again when it is gone.
# Usage: tests/bridge-listeners.sh [LOG]   (run from the repository root in WSL)
# Prints one PASS or FAIL line per check; exits 1 if any failed.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-listeners.log}
BUILD=build
failed=0

check() {
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1 (expected $3, got $2)"
        failed=1
    fi
}

# Applications of the bridge on the desktop, by name.
has_notepad() {
    python3 tests/atspi-dump.py notepad 2>&1 | grep -c "notepad" | sed 's/^[1-9][0-9]*$/yes/;s/^0$/no/'
}

# Hook traces of the bridge so far.
hooks() {
    grep -c "hooks $1" $BUILD/bridge.log
}

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
wine notepad &
sleep 3

{
    check "no hooks without a listener" "$(hooks installed)" 0
    check "no application without a listener" "$(has_notepad)" no

    python3 tests/atspi-events.py 5 > $BUILD/bridge-listeners-events.log 2>&1 &
    events=$!
    sleep 2
    check "hooks with a listener" "$(hooks installed)" 1
    check "focused notepad reported" "$(has_notepad)" yes
    check "focus event heard" \
        "$(grep -c "^object:state-changed:focused(1) app='notepad' src=text" \
            $BUILD/bridge-listeners-events.log)" 1
    wait $events
    sleep 1
    check "hooks removed after the listener" "$(hooks removed)" 1
    check "application gone after the listener" "$(has_notepad)" no
} | tee "$LOG"
grep -q FAIL "$LOG" && failed=1

wineserver -k
wait
echo "log: $LOG"
exit $failed
