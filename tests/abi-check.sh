#!/bin/sh
# Fail if a winegcc object calls memset/memcpy/memmove with the sysv ABI.
#
# In a Winelib ELF these symbols bind to Wine's ucrtbase, which is ms_abi.
# Calls written in the source use the ms_abi declarations of Wine's
# <string.h>, but calls that GCC makes up itself (a loop turned into
# memset, a large struct copy) use the sysv ABI and crash (seen in Lua's
# tablerehash). The Makefile's STRINGOP_FLAGS prevent them; this checks.
#
# An ms_abi call to these functions passes the size in r8 (sysv never
# uses r8 for them), so a call is accepted if r8 is written between it
# and the previous call.
#
# Usage: tests/abi-check.sh OBJECT...
status=0
for f in "$@"; do
    objdump -dr --no-show-raw-insn "$f" | awk -v f="$f" '
        /R_X86_64_PLT32[[:space:]]+mem(set|cpy|move)/ {
            if (!call_r8) { print f ": sysv call to " $NF " at " last; bad = 1 }
            next
        }
        /^[[:space:]]+[0-9a-f]+:/ {
            last = $1
            if ($0 ~ /call/) { call_r8 = r8; r8 = 0; next }
            if ($0 ~ /,%r8d?$/) r8 = 1
        }
        END { exit bad }' || status=1
done
exit $status
