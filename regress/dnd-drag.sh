#!/bin/sh

# Kitty drag and drop (OSC 72) drags from panes: the start gesture goes to the
# pane under the pointer if it offers drags, and the offer, data and events are
# passed between that pane and the client.

. ./dnd-common.inc

start
$INNER set -g status on || exit 1
$INNER set -g status-position top || exit 1
$INNER set -as terminal-features ',screen*:dnd' || exit 1
OFF=$($INNER display -p -t $RIGHT '#{pane_left}')
$OUTER resize-window -t outer -y 25 || exit 1
attach outer $DIR/out
expect $DIR/out 't=q' "startup query"
CW=$($INNER display -p '#{client_cell_width}')
CH=$($INNER display -p '#{client_cell_height}')

# Cell (x, y) of the client with a pixel offset of 5 into the cell. The status
# line is at the top, so pane rows start at client row 1.
at()
{
	echo "x=$1:y=$2:X=$(($1 * CW + 5)):Y=$(($2 * CH + 5))"
}

# Offering drags registers the terminal.
pane $LEFT 't=o:x=1;1:fedcba9876543210fedc'
expect $DIR/out 't=o:x=1' "offer"

# A gesture over a pane not offering drags, or the status line, is ignored.
terminal outer "t=o:$(at $((OFF + 4)) 3)"
terminal outer "t=o:$(at 4 0)"
expect_none $DIR/right "gesture over pane not offering"
expect_none $DIR/left "gesture over status line"

# A gesture over the pane goes to it relative to the pane, after its machine
# id is given to the terminal.
terminal outer "t=o:$(at 5 3)"
expect $DIR/out 't=o:x=1;1:fedcba9876543210fedc' "machine id"
expect $DIR/left "t=o:$(at 5 2)" "gesture"

# The offer, data sent ahead in chunks and the start are passed on.
pane $LEFT 't=o:o=3:i=9;text/plain'
pane $LEFT 't=p:x=0:m=1;aGk='
pane $LEFT 'm=0;'
pane $LEFT 't=P:x=-1'
expect $DIR/out "$(printf '%s\n' 't=o:o=3;text/plain' 't=p:x=0:m=1;aGk=' \
    'm=0;' 't=P:x=-1')" "drag offer"

# Events from the terminal carry the i key from the offer.
terminal outer 't=E;OK'
terminal outer 't=e:x=1:y=0'
terminal outer 't=e:x=5:y=0'
expect $DIR/left "$(printf '%s\n' 't=E:i=9;OK' 't=e:x=1:y=0:i=9' \
    't=e:x=5:y=0:i=9')" "drag events"
pane $LEFT 't=e:y=0:m=0;aGk='
expect $DIR/out 't=e:y=0:m=0;aGk=' "drag data"

# Once the drag finishes, more drag commands are refused.
terminal outer 't=e:x=4:y=0'
expect $DIR/left 't=e:x=4:y=0:i=9' "drag finished"
pane $LEFT 't=P:x=0'
expect $DIR/left 't=E:i=9;EINVAL' "drag over"
expect_none $DIR/out "sent after drag"

# A pane that stops offering during a drag cancels it.
terminal outer "t=o:$(at 5 3)"
expect $DIR/left "t=o:$(at 5 2):i=9" "second gesture"
pane $LEFT 't=o:x=2'
expect $DIR/out "$(printf '%s\n' 't=E:y=-1' 't=o:x=2')" "stop offering"

# The terminal is unregistered when the client detaches.
pane $LEFT 't=o:x=1'
expect $DIR/out 't=o:x=1' "offer again"
$INNER detach-client -s inner || exit 1
expect $DIR/out 't=o:x=2' "detach"

exit 0
