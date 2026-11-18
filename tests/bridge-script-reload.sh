#!/bin/bash
# Application scripts: a script error is reported once, and a script
# changed while the bridge runs is read again. The bridge runs from a copy
# in build/reload/ with its own scripts/winefile.lua: first scripts/winefile.lua
# plus an onDescribe that raises an error, then, while winefile runs,
# scripts/winefile.lua plus an onDescribe that appends " reloaded" to names.
# Usage: tests/bridge-script-reload.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-script-reload.log}
BUILD=build
DIR=$BUILD/reload

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

rm -rf $DIR
mkdir -p $DIR/scripts
cp $BUILD/wine-atspi.exe.so $BUILD/wine-atspi-hook.dll $DIR/
{ cat scripts/winefile.lua; echo 'wa.onDescribe(function(obj) error("injected error") end)'; } \
    > $DIR/scripts/winefile.lua

WINE_ATSPI_DEBUG=1 wine $DIR/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
: > "$LOG"
python3 tests/atspi-events.py 25 >> "$LOG" 2>&1 &
events=$!
sleep 1
wine winefile &
sleep 5

# Several items described with the failing script.
timeout 30 wine $BUILD/sendkeys.exe.so w:1000 k:down w:1000 k:down w:1000 k:up w:1000
echo "--- reload" >> "$LOG"
{ cat scripts/winefile.lua
  echo 'wa.onDescribe(function(obj) if obj.name ~= "" then obj.name = obj.name .. " reloaded" end end)'
} > $DIR/scripts/winefile.lua.new
mv $DIR/scripts/winefile.lua.new $DIR/scripts/winefile.lua
sleep 2
timeout 30 wine $BUILD/sendkeys.exe.so k:down w:1000 k:up w:1000
wait $events

kill $bridge
wineserver -k
wait
echo "--- check" >> "$LOG"
errors=$(grep -c "injected error" $BUILD/bridge.log)
echo "injected error reported $errors times" >> "$LOG"
[ "$errors" -eq 1 ] || echo "FAIL: injected error not reported exactly once" >> "$LOG"
grep -q "winefile: file changed, reading it again" $BUILD/bridge.log ||
    echo "FAIL: script not read again" >> "$LOG"
focused="focused(1) app='winefile' src="
before=$(sed -n '/^--- reload/q;p' "$LOG" | grep -c "$focused[a-z ]* '[^']*reloaded'")
after=$(sed -n '/^--- reload/,$p' "$LOG" | grep -c "$focused[a-z ]* '[^']* reloaded'")
echo "focused names with 'reloaded': $before before, $after after the change" >> "$LOG"
[ "$before" -eq 0 ] && [ "$after" -gt 0 ] || echo "FAIL: reloaded script not used" >> "$LOG"
echo "log: $LOG"
! grep -q "^FAIL" "$LOG"
