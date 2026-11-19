#!/bin/bash
# tests/bridge-controls.sh against the 32-bit build of tests/controls.c,
# so the bridge reads list view, tree view, tab, header and toolbar items
# with their WoW64 layouts. Expected as in the 64-bit log, except that
# the toolbar's 'Help' has no name: its tooltip needs the hook DLL,
# which exists for 64-bit targets only.
# Usage: tests/bridge-controls32.sh [LOG]   (from the repository root, or by tests/vm.sh)
#
# Runs in its own prefix: the VM's ~/.wine-atspi has an empty syswow64,
# and wineboot -u cannot fill it (its Wow64Install step starts
# syswow64\rundll32.exe). A new prefix gets it from wineboot -i.

export WINEPREFIX=${WINEPREFIX32:-$HOME/.wine-atspi32}
export WINEDEBUG=${WINEDEBUG:--all}
if [ ! -f "$WINEPREFIX/drive_c/windows/syswow64/kernel32.dll" ]; then
    echo "creating $WINEPREFIX"
    # No Mono / Gecko: on a display wineboot would wait for their dialog.
    WINEDLLOVERRIDES=mscoree,mshtml= wineboot -i > /dev/null 2>&1
    wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog \
        /t REG_DWORD /d 0 /f > /dev/null 2>&1
    wineserver -w
fi

CONTROLS=build/controls32.exe exec bash tests/bridge-controls.sh \
    "${1:-build/bridge-controls32.log}"
