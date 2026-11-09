#!/usr/bin/env python3
"""Dump the AT-SPI desktop: one line per application, then its subtree.

Usage: tests/atspi-dump.py [-v] [APP_NAME_SUBSTRING]
       tests/atspi-dump.py --click NAME APP_NAME_SUBSTRING
Without an argument all applications are listed without subtrees.
With -v each object also shows its rect in window coordinates, as Orca's
flat review asks for it, its showing and focused states and its actions.
With --click the first object named NAME gets its "click" action, as
Orca's object navigator does it, and the result is printed.
"""
import sys

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

VERBOSE = False


def details(obj):
    parts = []
    if obj.get_component_iface() is not None:
        r = Atspi.Component.get_extents(obj, Atspi.CoordType.WINDOW)
        parts.append("rect=%d,%d %dx%d" % (r.x, r.y, r.width, r.height))
    states = obj.get_state_set()
    for state, label in ((Atspi.StateType.SHOWING, "showing"),
                         (Atspi.StateType.FOCUSED, "focused")):
        if states.contains(state):
            parts.append(label)
    action = obj.get_action_iface()
    if action is not None:
        names = [Atspi.Action.get_name(action, i)
                 for i in range(Atspi.Action.get_n_actions(action))]
        parts.append("actions=" + ",".join(names))
    return " " + " ".join(parts) if parts else ""


def describe(obj):
    if obj.get_role() == Atspi.Role.APPLICATION:
        extra = obj.get_toolkit_name()
    else:
        extra = (obj.get_attributes() or {}).get("class", "")
    line = "%s [%s] %r" % (obj.get_role_name(), extra, obj.get_name())
    if obj.get_role() in (Atspi.Role.STATUS_BAR, Atspi.Role.TERMINAL):
        text = obj.get_text_iface()
        line += " text=%r" % (Atspi.Text.get_text(text, 0, -1) if text else None)
    if VERBOSE and obj.get_role() != Atspi.Role.APPLICATION:
        line += details(obj)
        if obj.get_role() in (Atspi.Role.TEXT, Atspi.Role.ENTRY, Atspi.Role.TERMINAL):
            line += text_lines(obj)
    return line


def text_lines(obj):
    """Each line of a text object with its range rect, as flat review asks."""
    text = obj.get_text_iface()
    if text is None:
        return ""
    out = []
    count = Atspi.Text.get_character_count(text)
    offset = 0
    while offset < count and len(out) < 80:
        r = Atspi.Text.get_string_at_offset(text, offset, Atspi.TextGranularity.LINE)
        if r.end_offset <= offset:
            break
        e = Atspi.Text.get_range_extents(text, r.start_offset, r.end_offset,
                                         Atspi.CoordType.WINDOW)
        out.append("\n    line %d-%d %r rect=%d,%d %dx%d" % (
            r.start_offset, r.end_offset, r.content, e.x, e.y, e.width, e.height))
        offset = r.end_offset
    if count:
        c = Atspi.Text.get_character_extents(text, 0, Atspi.CoordType.WINDOW)
        out.append("\n    char 0 rect=%d,%d %dx%d" % (c.x, c.y, c.width, c.height))
    return "".join(out)


def dump(obj, depth):
    print("  " * depth + describe(obj))
    for i in range(obj.get_child_count()):
        child = obj.get_child_at_index(i)
        if child is not None:
            dump(child, depth + 1)


def find(obj, name):
    if obj.get_role() != Atspi.Role.APPLICATION and obj.get_name() == name:
        return obj
    for i in range(obj.get_child_count()):
        child = obj.get_child_at_index(i)
        found = find(child, name) if child is not None else None
        if found is not None:
            return found
    return None


def click(name, pattern):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        if app is None or pattern not in (app.get_name() or ""):
            continue
        obj = find(app, name)
        if obj is None:
            continue
        action = obj.get_action_iface()
        if action is None:
            print("click %r: no Action" % name)
            return
        for j in range(Atspi.Action.get_n_actions(action)):
            if Atspi.Action.get_name(action, j) == "click":
                print("click %r: %s" % (name, Atspi.Action.do_action(action, j)))
                return
        print("click %r: no click action" % name)
        return
    print("click %r: not found" % name)


def main():
    global VERBOSE
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--click":
        click(args[1], args[2])
        return
    if args and args[0] == "-v":
        VERBOSE = True
        args = args[1:]
    pattern = args[0] if args else None
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
