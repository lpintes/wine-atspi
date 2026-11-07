#!/usr/bin/env python3
"""Log AT-SPI focus and window events, one line each, like Orca would get.

Usage: tests/atspi-events.py SECONDS [TOOLKIT]
Only events from applications whose toolkit is TOOLKIT (default
"wine-atspi") are printed. Each line shows the event, the application,
and the source's role, name and parent chain.
"""
import os
import sys

import gi

os.environ["ATSPI_USE_LEGACY_DEVICE"] = "1"

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib  # noqa: E402

EVENTS = [
    "object:state-changed",
    "object:children-changed",
    "object:property-change:accessible-name",
    "object:selection-changed",
    "object:announcement",
    "object:text-changed",
    "object:text-caret-moved",
    "object:text-selection-changed",
    "window:activate",
    "window:deactivate",
]


def describe(obj):
    try:
        text = "%s %r" % (obj.get_role_name(), obj.get_name())
        attrs = obj.get_attributes() or {}
        value = attrs.get("valuetext")
        if value is not None:
            text += " value=%r" % value
        if "posinset" in attrs:
            text += " %s/%s" % (attrs["posinset"], attrs.get("setsize"))
        elif "setsize" in attrs:
            text += " %s items" % attrs["setsize"]
        if obj.get_state_set().contains(Atspi.StateType.SELECTED):
            text += " selected"
        text += describe_text(obj)
        text += describe_table(obj)
        return text
    except GLib.Error as e:
        return "<%s>" % e.message


def describe_table(obj):
    """Size of a table; a cell's place, row and headers, as Orca reads a row."""
    if obj.get_table_iface() is not None:
        return " %dx%d" % (Atspi.Table.get_n_rows(obj), Atspi.Table.get_n_columns(obj))
    if obj.get_table_cell() is None:
        return ""
    ok, row, column = Atspi.TableCell.get_position(obj)
    table = Atspi.TableCell.get_table(obj)
    if not ok or table is None:
        return " cell?"
    cells = []
    headers = []
    for i in range(Atspi.Table.get_n_columns(table)):
        cell = Atspi.Table.get_accessible_at(table, row, i)
        cells.append(cell.get_name() if cell is not None else None)
        header = Atspi.Table.get_column_header(table, i)
        headers.append(header.get_name() if header is not None else None)
    own = [h.get_name() for h in Atspi.TableCell.get_column_header_cells(obj)]
    return " cell=%d,%d row=%r headers=%r own=%r" % (row, column, cells, headers, own)


def describe_text(obj):
    """Caret, selection, the caret's line and word, as Orca asks for them."""
    if obj.get_text_iface() is None:
        return ""
    # Module functions: on the object, get_selection is Accessible's.
    text = Atspi.Text
    caret = text.get_caret_offset(obj)
    line = text.get_string_at_offset(obj, caret, Atspi.TextGranularity.LINE)
    word = text.get_string_at_offset(obj, caret, Atspi.TextGranularity.WORD)
    out = " caret=%d/%d line=%r(%d-%d) word=%r" % (
        caret, text.get_character_count(obj), line.content, line.start_offset,
        line.end_offset, word.content)
    if text.get_n_selections(obj):
        sel = text.get_selection(obj, 0)
        out += " sel=%d-%d" % (sel.start_offset, sel.end_offset)
    return out


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
        detail = "%d" % event.detail1
        if event.type.startswith(("object:text-changed", "object:announcement")):
            detail += ",%d,%r" % (event.detail2, event.any_data)
        print("%s(%s) app=%r src=%s | %s" % (
            event.type, detail, app.get_name(), describe(event.source),
            chain(event.source)), flush=True)
    except GLib.Error as e:
        print("%s error %s" % (event.type, e.message), flush=True)


def on_key(device, pressed, keycode, keysym, modifiers, string):
    if pressed:
        print("key %r keysym=%#x keycode=%d modifiers=%#x" % (
            string, keysym, keycode, modifiers), flush=True)
    return False


def main():
    seconds = float(sys.argv[1])
    toolkit = sys.argv[2] if len(sys.argv) > 2 else "wine-atspi"
    listener = Atspi.EventListener.new(on_event, toolkit)
    for name in EVENTS:
        listener.register(name)
    # Keys the way Orca gets them without a compositor keyboard monitor
    # (WSLg): through the registry, from applications that report them.
    device = Atspi.Device.new()
    device.add_key_watcher(on_key)
    loop = GLib.MainLoop()
    GLib.timeout_add(int(seconds * 1000), loop.quit)
    loop.run()


if __name__ == "__main__":
    main()
