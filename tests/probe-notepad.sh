#!/bin/bash
# Run the probe against Wine's notepad and drive it with sendkeys.
# Usage: tests/probe-notepad.sh [LOG]   (run from the repository root in WSL)

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/probe-notepad.log}
BUILD=build

if [ ! -d "$WINEPREFIX" ]; then
    WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
    # Print crash backtraces to the terminal instead of opening a dialog.
    wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f
    wineserver -w
fi

wine $BUILD/probe.exe.so -v > "$LOG" 2>&1 &
probe=$!
sleep 2
wine notepad &
sleep 3

timeout 60 wine $BUILD/sendkeys.exe.so f:Notepad w:500 \
    t:"Hello world" k:return t:"Second line" \
    k:home k:right k:right k:up k:end \
    w:300 k:f10 w:300 k:down w:500 k:down w:300 k:right w:500 \
    k:escape w:300 k:escape w:300 k:escape w:500

kill $probe
wineserver -k
wait
echo "log: $LOG"
