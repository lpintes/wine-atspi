#!/bin/sh
# Fail if an AT-SPI side object (host gcc) calls a libc function.
#
# In the Winelib ELF a libc name such as strlen binds to Wine's msvcrt,
# which is ms_abi, while host gcc calls it with the sysv ABI. GLib
# macros can expand to libc calls: g_str_has_prefix with a constant
# prefix becomes strlen + strncmp (GLib 2.76+) and crashed the bridge.
# Call such a function parenthesized, (g_str_has_prefix)(...), to get
# the real GLib function.
#
# Allowed undefined symbols: GLib (g_*, _g_*, glib_*), the core's
# bridge_* functions, symbols defined by the given objects and the
# compiler's own helpers.
#
# Usage: tests/libc-check.sh OBJECT...
defined=$(nm --defined-only "$@" | awk 'NF == 3 { print $3 }' | sort -u)
bad=$(nm -u "$@" | awk 'NF == 2 { print $2 }' | sort -u |
    grep -v -e '^g_' -e '^_g_' -e '^glib_' -e '^bridge_' \
        -e '^_GLOBAL_OFFSET_TABLE_$' -e '^__stack_chk_fail$' |
    grep -v -x -F "$defined")
if [ -n "$bad" ]; then
    echo "libc calls on the AT-SPI side (bind to Wine's ms_abi msvcrt):" $bad
    exit 1
fi
