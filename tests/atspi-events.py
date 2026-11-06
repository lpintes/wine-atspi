#!/usr/bin/env python3
"""Log AT-SPI focus and window events, one line each, like Orca would get.

Usage: tests/atspi-events.py SECONDS [TOOLKIT]
Only events from applications whose toolkit is TOOLKIT (default
"wine-atspi") are printed. Each line shows the event, the application,
and the source's role, name and parent chain.
"""
import sys

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib  # noqa: E402

EVENTS = [
    "object:state-changed",
    "object:children-changed",
    "object:property-change:accessible-name",
    "object:selection-changed",
    "window:activate",
    "window:deactivate",
]


def describe(obj):
    try:
        text = "%s %r" % (obj.get_role_name(), obj.get_name())
        value = (obj.get_attributes() or {}).get("valuetext")
        if value is not None:
            text += " value=%r" % value
        return text
    except GLib.Error as e:
        return "<%s>" % e.message


def chain(obj):
    parts = []
    while obj is not None and obj.get_role() != Atspi.Role.APPLICATION:
        obj = obj.get_parent()
        if obj is not None:
            parts.append(describe(obj))
    return " < ".join(parts)


def on_event(event, toolkit):
    # libatspi itself reports defunct states while it looks objects up.
    if event.type == "object:state-changed:defunct":
        return
    try:
        app = event.source.get_application()
        if app is None or app.get_toolkit_name() != toolkit:
            return
        print("%s(%d) app=%r src=%s | %s" % (
            event.type, event.detail1, app.get_name(), describe(event.source),
            chain(event.source)), flush=True)
    except GLib.Error as e:
        print("%s error %s" % (event.type, e.message), flush=True)


def main():
    seconds = float(sys.argv[1])
    toolkit = sys.argv[2] if len(sys.argv) > 2 else "wine-atspi"
    listener = Atspi.EventListener.new(on_event, toolkit)
    for name in EVENTS:
        listener.register(name)
    loop = GLib.MainLoop()
    GLib.timeout_add(int(seconds * 1000), loop.quit)
    loop.run()


if __name__ == "__main__":
    main()
