#!/usr/bin/env python3
"""Move the caret or the selection of a focused text, as Orca does it.

Usage: tests/atspi-select.py APP_NAME_SUBSTRING caret OFFSET
       tests/atspi-select.py APP_NAME_SUBSTRING set START END
       tests/atspi-select.py APP_NAME_SUBSTRING add START END
       tests/atspi-select.py APP_NAME_SUBSTRING remove
Finds the focused object with Text in the application, calls
SetCaretOffset, SetSelection (selection 0), AddSelection or
RemoveSelection (selection 0) and prints its result. After half a second,
when the bridge has read the control again, prints the caret offset, the
selections and the text:
    caret 6: True
    caret=6 selections=[] text='hello world\\nsecond line'
"""
import sys
import time

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


def state(text):
    selections = []
    for i in range(Atspi.Text.get_n_selections(text)):
        r = Atspi.Text.get_selection(text, i)
        selections.append((r.start_offset, r.end_offset))
    return "caret=%d selections=%r text=%r" % (
        Atspi.Text.get_caret_offset(text), selections,
        Atspi.Text.get_text(text, 0, -1))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    app = find_app(args[0])
    obj = focused_text(app) if app is not None else None
    if obj is None:
        print("%s: no focused text" % args[0])
        sys.exit(1)
    text = obj.get_text_iface()
    op, numbers = args[1], [int(a) for a in args[2:]]
    if op == "caret":
        result = Atspi.Text.set_caret_offset(text, numbers[0])
    elif op == "set":
        result = Atspi.Text.set_selection(text, 0, numbers[0], numbers[1])
    elif op == "add":
        result = Atspi.Text.add_selection(text, numbers[0], numbers[1])
    elif op == "remove":
        result = Atspi.Text.remove_selection(text, 0)
    else:
        sys.exit(__doc__)
    print("%s: %s" % (" ".join(args[1:]), result))
    time.sleep(0.5)
    print(state(text))


if __name__ == "__main__":
    main()
