#!/usr/bin/env python3
"""Dump the AT-SPI desktop: one line per application, then its subtree.

Usage: tests/atspi-dump.py [APP_NAME_SUBSTRING]
Without an argument all applications are listed without subtrees.
"""
import sys

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


def describe(obj):
    return "%s [%s] %r" % (obj.get_role_name(), obj.get_toolkit_name()
                           if obj.get_role() == Atspi.Role.APPLICATION
                           else "", obj.get_name())


def dump(obj, depth):
    print("  " * depth + describe(obj))
    for i in range(obj.get_child_count()):
        child = obj.get_child_at_index(i)
        if child is not None:
            dump(child, depth + 1)


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else None
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        if app is None:
            continue
        if pattern is None:
            print(describe(app))
        elif pattern in (app.get_name() or ""):
            dump(app, 0)


if __name__ == "__main__":
    main()
