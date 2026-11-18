#!/bin/bash
# The a11y bus may be missing when the bridge starts and may go away
# later. The test runs Wine on a private session bus without service
# files (tests/session-bus.conf), so org.a11y.Bus cannot be started on
# demand and the test decides when a bus launcher runs:
#   1. bridge and notepad start without an a11y bus: the bridge waits;
#   2. a launcher starts: notepad gets registered;
#   3. the registry is killed and started again: notepad registered again;
#   4. the a11y bus daemon is killed: the bridge drops the bus and
#      registers notepad once a bus is there again. What the launcher does
#      then is logged as INFO.
# Usage: tests/bridge-a11ybus.sh [LOG]   (from the repository root, or by tests/vm.sh)
# Prints one PASS or FAIL line per check; exits 1 if any failed.

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-a11ybus.log}
BUILD=build
LAUNCHER=/usr/lib/at-spi-bus-launcher
failed=0

check() {
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1 (expected $3, got $2)"
        failed=1
    fi
}

# Address of the a11y bus on the private session bus, empty without one.
a11y_address() {
    dbus-send --session --print-reply=literal --reply-timeout=2000 --dest=org.a11y.Bus \
        /org/a11y/bus org.a11y.Bus.GetAddress 2>/dev/null | tr -d ' '
}

# Whether notepad is on that bus's desktop. libatspi takes the address
# from AT_SPI_BUS_ADDRESS before the X root window property of the desktop.
has_notepad() {
    local addr
    addr=$(a11y_address)
    [ -n "$addr" ] || { echo no; return; }
    AT_SPI_BUS_ADDRESS=$addr timeout 10 python3 tests/atspi-dump.py notepad 2>&1 |
        grep -c "notepad" | sed 's/^[1-9][0-9]*$/yes/;s/^0$/no/'
}

# Lines of the bridge's log matching $1.
logged() {
    grep -c "$1" $BUILD/bridge.log
}

# Without DISPLAY the launcher leaves the desktop's X property alone.
start_launcher() {
    env -u DISPLAY -u WAYLAND_DISPLAY $LAUNCHER > $BUILD/bridge-a11ybus-launcher.log 2>&1 &
    launcher=$!
    # The checks run in a subshell (tee), so the cleanup reads the pids here.
    echo $launcher >> $BUILD/bridge-a11ybus.pids
}

registryds() {
    pgrep -f /usr/lib/at-spi2-registryd | sort
}

wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

bus=$(dbus-daemon --config-file=tests/session-bus.conf --fork --print-address=1 --print-pid=1)
export DBUS_SESSION_BUS_ADDRESS=$(echo "$bus" | sed -n 1p)
bus_pid=$(echo "$bus" | sed -n 2p)
desktop_registryds=$(registryds)
launcher=
: > $BUILD/bridge-a11ybus.pids

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
sleep 2
wine notepad &
sleep 3

{
    check "bridge waits without an a11y bus" "$(logged 'no a11y bus yet')" 1
    check "bridge still runs" "$(pgrep -fc 'wine-atspi.exe.so --always-on' | sed 's/^[1-9][0-9]*$/yes/')" yes

    start_launcher
    sleep 3
    check "bridge connects to the new bus" "$(logged 'a11y bus at')" 1
    check "notepad registered on the new bus" "$(has_notepad)" yes

    registry=$(comm -13 <(echo "$desktop_registryds") <(registryds))
    echo "INFO: registry of the private a11y bus: ${registry:-none}"
    [ -n "$registry" ] && kill $registry
    sleep 1
    check "registry loss seen" "$(logged 'registry gone')" 1
    has_notepad > /dev/null     # starts a new registry
    sleep 2
    check "registry back seen" "$(logged 'registry back')" 1
    check "notepad registered with the new registry" "$(has_notepad)" yes

    daemon=$(pgrep -P "$launcher" | head -1)
    echo "INFO: a11y bus daemon: $(ps -o args= -p "$daemon" 2>/dev/null)"
    kill "$daemon"
    sleep 3
    check "bus loss seen" "$(logged 'a11y bus closed')" 1
    if kill -0 "$launcher" 2>/dev/null; then
        echo "INFO: launcher survived its bus; GetAddress gives '$(a11y_address)'"
    else
        echo "INFO: launcher exited with its bus; starting another one"
        start_launcher
    fi
    sleep 4
    check "bridge connects again" "$(logged 'a11y bus at')" 2
    check "notepad registered again" "$(has_notepad)" yes
} | tee "$LOG"
grep -q FAIL "$LOG" && failed=1

grep "^wine-atspi:" $BUILD/bridge.log >> "$LOG"
wineserver -k
for pid in $(cat $BUILD/bridge-a11ybus.pids); do
    pkill -P "$pid" 2>/dev/null
    kill "$pid" 2>/dev/null
done
# Registries started on the private a11y bus, never the desktop's.
for pid in $(comm -13 <(echo "$desktop_registryds") <(registryds)); do
    kill "$pid" 2>/dev/null
done
kill "$bus_pid" 2>/dev/null
wait
echo "log: $LOG"
exit $failed
