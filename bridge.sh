#!/bin/bash
# Run a Windows program under the bridge for a manual test with Orca.
# Usage: ./bridge.sh PROGRAM [ARGS...]   e.g. ./bridge.sh notepad
#
# Installs the bridge in the test prefix (HKLM RunServices), which also
# starts it unless it already runs there, then runs the program in the
# foreground. From then on every Wine session of the prefix starts the
# bridge, and it ends with the session. Bridge traces are appended to
# build/bridge.log. `wine build/wine-atspi.exe.so uninstall` undoes it.

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

# A new prefix would ask to install Wine Mono and Gecko, and that dialog
# blocks the first program for minutes. Neither is needed for the tests.
if [ ! -d "$WINEPREFIX" ]; then
    echo "creating prefix $WINEPREFIX"
    WINEDLLOVERRIDES="mscoree=;mshtml=" wineboot -u
    wineserver -w
fi

wine $BUILD/wine-atspi.exe.so install --log $BUILD/bridge.log || exit 1
echo "log: $BUILD/bridge.log"
exec wine "$@"
