#!/bin/bash
# Run a Windows program under the bridge for a manual test with Orca.
# Usage: ./bridge.sh PROGRAM [ARGS...]   e.g. ./bridge.sh notepad
#
# Starts the bridge in the test prefix unless it already runs there, then
# runs the program in the foreground. When the program ends, a bridge
# started by this script is stopped again. Bridge output (with
# WINE_ATSPI_DEBUG traces) goes to build/bridge.log.

set -u
cd "$(dirname "$0")"
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
BUILD=build

if [ $# -eq 0 ]; then
    echo "usage: $0 PROGRAM [ARGS...]" >&2
    exit 2
fi
if [ ! -f $BUILD/wine-atspi.exe.so ]; then
    echo "bridge not built, run make first" >&2
    exit 1
fi
pgrep -x orca > /dev/null || echo "note: Orca is not running" >&2

bridge=
# The brackets keep the pattern from matching a command line quoting it.
if ! pgrep -f '[w]ine-atspi\.exe' > /dev/null; then
    WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so > $BUILD/bridge.log 2>&1 &
    bridge=$!
    sleep 2
    if ! kill -0 $bridge 2> /dev/null; then
        echo "bridge failed to start, see $BUILD/bridge.log" >&2
        exit 1
    fi
    echo "bridge started, log: $BUILD/bridge.log"
else
    echo "bridge already running"
fi

wine "$@"
status=$?

if [ -n "$bridge" ]; then
    kill $bridge 2> /dev/null
    wait $bridge 2> /dev/null
    echo "bridge stopped"
fi
exit $status
