#!/bin/bash
# Run tests in the Arch test VM instead of WSLg, so that test windows do
# not take the focus on the Windows desktop and keys typed there do not
# reach the target.
#
# Usage (Git Bash, from anywhere): tests/vm.sh [--orca] [--no-copy] TEST...
#   e.g. tests/vm.sh bridge-menu bridge-wordpad
# TEST is a script name in tests/ without ".sh". Build first (make in WSL).
#
# Needs the VM running (C:\linux\run.sh, ssh alias arch-vm) and a MATE
# session in it (log in on its console and run run-mate). The tests use
# that session's X display and D-Bus. Orca is stopped first unless
# --orca is given; all tests run their bridge with --always-on, so they
# need no screen reader.
#
# The repository's files (tracked and untracked, not ignored) and the
# built programs go to ~/wine-atspi in the VM, the logs of build/ come
# back to build/vm/.
set -u
cd "$(dirname "$0")/.."

orca=no
copy=yes
while [ $# -gt 0 ]; do
    case $1 in
    --orca) orca=yes ;;
    --no-copy) copy=no ;;
    -*) echo "Unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
    shift
done
[ $# -gt 0 ] || { echo "Usage: tests/vm.sh [--orca] [--no-copy] TEST..." >&2; exit 2; }

if [ $copy = yes ]; then
    # Into a staging directory, then renamed into place: overwriting a
    # running .so in place crashes the bridge (CLAUDE.md, Environment).
    { git ls-files -z --cached --others --exclude-standard
      printf '%s\0' build/*.exe build/*.exe.so build/*.dll; } |
        tar --null -T - -cf - |
        ssh arch-vm 'set -e
            mkdir -p ~/wine-atspi && cd ~/wine-atspi
            rm -rf .incoming && mkdir .incoming
            tar -C .incoming -xf -
            cd .incoming
            find . -type f -print0 | while IFS= read -r -d "" f; do
                mkdir -p "../${f%/*}" && mv -f "$f" "../$f"
            done
            cd .. && rm -rf .incoming' || exit 1
fi

ssh arch-vm bash -s -- "$orca" "$@" <<'EOF'
set -u
orca=$1
shift
cd ~/wine-atspi
pid=$(pgrep -u "$USER" -x mate-session | head -n 1)
if [ -z "$pid" ]; then
    echo "No MATE session in the VM: log in on its console and run run-mate." >&2
    exit 2
fi
while IFS= read -r -d '' var; do
    case $var in
    DISPLAY=* | XAUTHORITY=* | DBUS_SESSION_BUS_ADDRESS=* | XDG_RUNTIME_DIR=*)
        export "$var" ;;
    esac
done < "/proc/$pid/environ"
if [ "$orca" = no ] && pkill -x orca; then
    echo "Orca stopped"
    sleep 1
fi
status=0
for t; do
    echo "== $t"
    bash "tests/$t.sh"
    rc=$?
    echo "exit $rc"
    [ $rc -eq 0 ] || status=1
done
exit $status
EOF
status=$?

mkdir -p build/vm
scp -q 'arch-vm:wine-atspi/build/*.log' build/vm/
echo "Logs in build/vm/"
exit $status
