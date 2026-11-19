#!/usr/bin/env python3
"""Print the caret, the character count and the caret's line of a focused text.

Usage: tests/atspi-text.py APP_NAME_SUBSTRING
Finds the focused object with Text in the application and prints one line
without the whole text, so it suits large documents:
    caret=63 count=1548288 line='The quick ... 00002\\n'
"""
import sys

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


def focused_text(obj):
    if (obj.get_role() != Atspi.Role.APPLICATION and
            obj.get_state_set().contains(Atspi.StateType.FOCUSED) and
            obj.get_text_iface() is not None):
        return obj
    for i in range(obj.get_child_count()):
        child = obj.get_child_at_index(i)
        found = focused_text(child) if child is not None else None
        if found is not None:
            return found
    return None


def find_app(pattern):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        if app is not None and pattern in (app.get_name() or ""):
            return app
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    app = find_app(sys.argv[1])
    obj = focused_text(app) if app is not None else None
    if obj is None:
        print("%s: no focused text" % sys.argv[1])
        sys.exit(1)
    text = obj.get_text_iface()
    caret = Atspi.Text.get_caret_offset(text)
    line = Atspi.Text.get_string_at_offset(text, caret, Atspi.TextGranularity.LINE)
    print("caret=%d count=%d line=%r" % (
        caret, Atspi.Text.get_character_count(text), line.content))


if __name__ == "__main__":
    main()
