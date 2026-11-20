#!/bin/bash
# Run the bridge against tests/controls.c and log the AT-SPI events it emits.
# Usage: tests/bridge-controls.sh [LOG]   (from the repository root, or by tests/vm.sh)
# The bridge's own stderr goes to build/bridge.log. CONTROLS picks another
# build of the window (tests/bridge-controls32.sh).

set -u
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-atspi}
export WINEDEBUG=${WINEDEBUG:--all}
LOG=${1:-build/bridge-controls.log}
BUILD=build
CONTROLS=${CONTROLS:-$BUILD/controls.exe.so}

# The test runs its own bridge with traces on stderr: drop one installed
# by bridge.sh, and the session its uninstall command started.
wine $BUILD/wine-atspi.exe.so uninstall > /dev/null 2>&1
wineserver -k; wineserver -w

WINE_ATSPI_DEBUG=1 wine $BUILD/wine-atspi.exe.so --always-on > $BUILD/bridge.log 2>&1 &
bridge=$!
sleep 2
python3 tests/atspi-events.py 44 > "$LOG" 2>&1 &
events=$!
sleep 1
wine "$CONTROLS" &
sleep 2

# "Fruits:" list: focus on item 'Apple' 1/3 (no selection yet); Down must
# focus and select 'Banana' 2/3.
# Tab to "Empty:": the list itself, list box with setsize 0.
# Tab to "Colors:" (extended selection): Down, then shift+down must select
# 'Blue' as well.
# Tab to "Files:" (report view): the table itself, 8x3, no focused row;
# Down must focus cell 'readme.txt' 0,0 with row ['readme.txt', '2 KB',
# 'Text'] and headers ['Name', 'Size', 'Type'], Down then 'notes.md'.
# Tab to "Places:" (tree): Wine selects the first item, so focus on tree
# item 'Home' 1/3 level=1 collapsed in tree 'Places:', checkable, not
# checked; Right expands it (state-changed:expanded), Down 'Documents' 1/2
# level=2, Down 'Music' 2/2 checkable checked, Space unchecks it
# (state-changed:checked 0), Space checks it again (checked 1), Left back
# to 'Home', Left collapses it, Down 'Work' 2/3.
# Tab to "Sections:" (tabs): page tab 'General' 1/3 selected in page tab
# list 'Sections:'; Right 'Advanced' 2/3, Right 'About' 3/3, Left
# 'Advanced' again, each selected.
# Tab to "Volume:" (trackbar): slider 'Volume:' value=50 [0..100 step 1];
# Right gives accessible-value 51, Page Up 41, End 100.
# The "Level:" progress bar follows it without focus: each change gives
# accessible-value on progress bar 'Level:' (51, 41, 100) with a rect.
# Tab to "Count:" (Edit with up-down): spin button 'Count:' value=3
# [1..10 step 1] with text '3'; Up gives accessible-value 4, Up 5, Down 4,
# each with the new text.
# Tab to "Run", Space starts it: the "Level:" bar fills and the label
# 'Idle' after "Close" gives accessible-name 'Step 1 of 20', 'Step 2 of
# 20' without focus (the tree dump at the end shows a later step).
# Tab past "Close" to the toolbar: button 'New' in tool bar
# (hot button 0); Right skips the separator to toggle 'Bold', Right
# 'Wrap' (checked), Right skips the disabled 'Undo' to 'Help', Right
# wraps to 'New', Left back to 'Help'. Left skips 'Undo' to 'Wrap', Left
# 'Bold'; Space sends its command, which enables 'Undo': the toolbar's
# items are read again after the key, so 'Undo' gets sensitive and
# enabled (state-changed) while the Static 'Zoom 100 %' on the toolbar
# stays in the tree; Right 'Wrap', Right now stops at 'Undo'.
# Escape closes the window.
timeout 55 wine $BUILD/sendkeys.exe.so f:WineAtspiControls w:500 \
    k:down w:500 k:down w:500 \
    k:tab w:500 \
    k:tab w:500 k:down w:500 k:shift+down w:500 \
    k:tab w:500 k:down w:500 k:down w:500 \
    k:tab w:500 k:right w:500 k:down w:500 k:down w:500 \
    k:space w:500 k:space w:500 \
    k:left w:500 k:left w:500 k:down w:500 \
    k:tab w:500 k:right w:500 k:right w:500 k:left w:500 \
    k:tab w:500 k:right w:500 k:pgup w:500 k:end w:500 \
    k:tab w:500 k:up w:500 k:up w:500 k:down w:500 \
    k:tab w:500 k:space w:2500 k:tab w:500 k:tab w:500 \
    k:right w:500 k:right w:500 k:right w:500 k:right w:500 k:left w:500 \
    k:left w:500 k:left w:500 k:space w:500 k:right w:500 k:right w:500
python3 tests/atspi-dump.py controls > $BUILD/dump-running.log 2>&1
timeout 10 wine $BUILD/sendkeys.exe.so k:escape w:1000

wait $events
{
    echo "--- tree before exit"
    cat $BUILD/dump-running.log
} >> "$LOG" 2>&1
kill $bridge
wineserver -k
wait
echo "log: $LOG"
