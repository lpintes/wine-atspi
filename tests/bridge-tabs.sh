#!/bin/bash
# Run the bridge against the TCS_BUTTONS tab control "Views:" of
# tests/controls.c and log the AT-SPI events it emits. With TCS_BUTTONS
# Wine's arrow keys move only the focused tab and emit
# EVENT_OBJECT_FOCUS; the selection stays (comctl32/tab.c,
# TAB_SetCurFocus), and no key selects.
# Usage: tests/bridge-tabs.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log. Exit code 1 if the
# focus order is wrong.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-tabs.log}
BUILD=build
fail=0

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 14 > "$LOG" 2>&1 &
events=$!
sleep 1
wine $BUILD/controls.exe.so &
sleep 2

# Shift+Tab from "Fruits:" to "Views:": page tab 'List' 1/3 focused and
# selected in page tab list 'Views:'. Right 'Details' 2/3 focused, not
# selected; Right 'Icons' 3/3; Space does nothing; Left 'Details' again.
# Tab to "Fruits:" and Shift+Tab back: Wine resets the focused tab to the
# selected one, so 'List' again.
timeout 20 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 \
    k:shift+tab w:500 k:right w:500 k:right w:500 k:space w:500 k:left w:500 \
    k:tab w:500 k:shift+tab w:500 k:right w:500
python3 tests/atspi-dump.py controls > $BUILD/dump-running.log 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1000

wait $events
# The focused objects in order. Wine focuses the first tab when it is
# inserted, while the window is created and has no focus yet; that event
# must not reach AT-SPI, so the list starts with 'Apple'.
got=$(grep -o "focused(1) app='controls' src=[a-z ]* '[A-Za-z]*'[ 0-9/]*[a-z]*" "$LOG" |
      sed "s/.*src=//; s/ *$//" | grep -v '^frame' | tr '\n' ';')
want="list item 'Apple' 1/3;page tab 'List' 1/3 selected;page tab 'Details' 2/3;\
page tab 'Icons' 3/3;page tab 'Details' 2/3;list item 'Apple' 1/3;\
page tab 'List' 1/3 selected;page tab 'Details' 2/3;"
{
    if [ "$got" = "$want" ]; then
        echo "PASS: focus order"
    else
        echo "FAIL: focus order"
        echo "  got:  $got"
        echo "  want: $want"
        fail=1
    fi
    echo "--- tree before exit"
    cat $BUILD/dump-running.log
} >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
exit $fail
