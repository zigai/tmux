#!/bin/sh

# Kitty drag and drop (OSC 72) drops from the outer terminal go to the pane
# under the pointer with pane coordinates, and replies go back to the client
# with the drop.

. ./dnd-common.inc

start
$INNER set -as terminal-features ',screen*:dnd' || exit 1
OFF=$($INNER display -p -t $RIGHT '#{pane_left}')
pane $RIGHT 't=a:i=7;text/plain'
pane_sync $RIGHT $DIR/right
attach outer:0 $DIR/out1
expect $DIR/out1 "$(printf '%s\n' 't=q' 't=a;text/plain')" "registered"
CW=$($INNER display -p '#{client_cell_width}')
CH=$($INNER display -p '#{client_cell_height}')

# Cell (x, y) of the client with a pixel offset of 5 into the cell.
at()
{
	echo "x=$1:y=$2:X=$(($1 * CW + 5)):Y=$(($2 * CH + 5))"
}
IN=$(at 4 3)

# A move split across two reads, with a gap longer than escape-time, is still
# one move and is not typed into the pane. The startup queries are answered
# first, so keys are not held for them.
$OUTER send-keys -t outer:0 -l "$(printf '\033]10;rgb:ffff/ffff/ffff\033\\')"
$OUTER send-keys -t outer:0 -l "$(printf '\033]11;rgb:0000/0000/0000\033\\')"
wait_for_text tmux $INNER display -p '#{client_termtype}' ||
    fail "no terminal type"
$OUTER send-keys -t outer:0 -l \
    "$(printf '\033]72;t=m:%s:o=3;text/pl' "$(at $((OFF + 4)) 3)")"
sleep 0.1
$OUTER send-keys -t outer:0 -l "$(printf 'ain\033\\')"
expect $DIR/right "t=m:$IN:o=3:i=7;text/plain" "split move"
expect $DIR/out1 't=m:o=0' "split move refused"
terminal outer:0 't=m:x=-1:y=-1'
expect $DIR/right 't=m:x=-1:y=-1:i=7' "split move left window"

# Moving over a pane that accepts drops: it gets the move relative to itself
# and its answer goes to the terminal. Until then the drop is refused.
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3;text/plain text/uri-list"
expect $DIR/out1 't=m:o=0' "refused until the pane answers"
expect $DIR/right "t=m:$IN:o=3:i=7;text/plain text/uri-list" "move"
pane $RIGHT 't=m:o=1:i=7;text/plain'
expect $DIR/out1 't=m:o=1;text/plain' "pane accepts"
terminal outer:0 "t=m:$(at $((OFF + 5)) 4):o=3"
expect $DIR/right "t=m:$(at 5 4):o=3:i=7" "move inside the pane"

# The active pane does not matter, only the pane under the pointer.
$INNER select-pane -t $LEFT || exit 1
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3"
expect $DIR/right "t=m:$IN:o=3:i=7" "move with another pane active"

# Moving to a pane that does not accept drops: the first is told the drag
# left it and the terminal is told the drop is refused.
terminal outer:0 "t=m:$(at 3 3):o=3"
expect $DIR/right 't=m:x=-1:y=-1:i=7' "leave pane"
expect $DIR/out1 't=m:o=0' "refused over other pane"
expect_none $DIR/left "pane not accepting drops"

# Coming back gives the MIME types again, as for a new drag.
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3"
expect $DIR/right "t=m:$IN:o=3:i=7;text/plain text/uri-list" "enter again"
expect $DIR/out1 't=m:o=0' "refused again until the pane answers"

# A zoomed pane hides the pane that accepts drops.
$INNER resize-pane -Z -t $LEFT || exit 1
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3"
expect $DIR/right 't=m:x=-1:y=-1:i=7' "leave hidden pane"
expect $DIR/out1 't=m:o=0' "refused over zoomed pane"
$INNER resize-pane -Z -t $LEFT || exit 1

# Drop, then data requests and replies, including chunks with no t key. The
# pane's machine id is given to the terminal first.
pane $RIGHT 't=a:x=1;1:0123456789abcdef0123'
pane_sync $RIGHT $DIR/right
terminal outer:0 "t=M:$(at $((OFF + 4)) 3):o=1;text/plain text/uri-list"
expect $DIR/out1 't=a:x=1;1:0123456789abcdef0123' "machine id"
expect $DIR/right "t=M:$IN:o=1:i=7;text/plain text/uri-list" "drop"
pane $RIGHT 't=r:x=1'
expect $DIR/out1 't=r:x=1' "request"
terminal outer:0 't=r:x=1:m=1;aGVs'
terminal outer:0 'm=0;bG8='
terminal outer:0 't=r:x=1:m=0;'
$OUTER send-keys -t outer:0 -l "$(printf '\033]72;t=R:x=2;ENOENT\007')"
expect $DIR/right "$(printf '%s\n' 't=r:x=1:i=7:m=1;aGVs' 'i=7:m=0;bG8=' \
    't=r:x=1:i=7:m=0;' 't=R:x=2:i=7;ENOENT')" "data"
pane $RIGHT 't=r:o=1'
expect $DIR/out1 't=r:o=1' "finish"
pane $RIGHT 't=r:x=1'
expect $DIR/right 't=R:x=1:i=7;EPERM' "request after finish"

# A long chunked MIME list is kept, and given again in chunks when the drag
# comes back to the pane.
long1=a/$(printf '%03000d' 0)
long2=b/$(printf '%03000d' 0)
list="$long1 $long2 text/plain"
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3:m=1;$long1 "
terminal outer:0 "m=1;$long2 "
terminal outer:0 'm=0;text/plain'
expect $DIR/right "$(printf '%s\n' "t=m:$IN:o=3:i=7:m=1;$long1 " \
    "i=7:m=1;$long2 " 'i=7:m=0;text/plain')" "long list"
expect $DIR/out1 't=m:o=0' "long list refused until the pane answers"
terminal outer:0 "t=m:$(at 3 3):o=3"
expect $DIR/right 't=m:x=-1:y=-1:i=7' "long list leave"
expect $DIR/out1 't=m:o=0' "long list refused over other pane"
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3"
expect $DIR/right "$(printf 't=m:%s:o=3:i=7:m=1;%s\ni=7:m=0;%s' "$IN" \
    "$(printf %s "$list" | cut -c1-4096)" \
    "$(printf %s "$list" | cut -c4097-)")" "long list again"
expect $DIR/out1 't=m:o=0' "long list refused again"

# A drag leaving the window is passed on.
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3;text/plain"
expect $DIR/right "t=m:$IN:o=3:i=7;text/plain" "new drag"
terminal outer:0 't=m:x=-1:y=-1'
expect $DIR/right 't=m:x=-1:y=-1:i=7' "left window"

# A second client is registered too and has its own drag.
$OUTER neww -d -t outer:1 'sleep 100' || exit 1
attach outer:1 $DIR/out2
expect $DIR/out2 "$(printf '%s\n' 't=q' 't=a;text/plain')" \
    "second client registered"
terminal outer:1 "t=m:$(at $((OFF + 4)) 3):o=1;text/plain"
expect $DIR/out2 't=m:o=0' "second client refused until the pane answers"
expect $DIR/right "t=m:$IN:o=1:i=7;text/plain" "second client move"
pane $RIGHT 't=m:o=1;text/plain'
expect $DIR/out2 't=m:o=1;text/plain' "answer to second client"
expect_none $DIR/out1 "answer sent to first client"

# Changing to a window with no drop targets unregisters both, and back.
$INNER neww -d -t inner:1 'sleep 100' || exit 1
$INNER selectw -t inner:1 || exit 1
expect $DIR/out1 't=A' "window changed"
expect $DIR/out2 't=A' "window changed, second client"
$INNER selectw -t inner:0 || exit 1
expect $DIR/out1 't=a;text/plain' "window back"
expect $DIR/out2 't=a;text/plain' "window back, second client"

# Moving a pane that accepts drops into or out of the window changes the
# registration.
$INNER neww -d -t inner:2 "$(logger third)" || exit 1
wait_for_output ready $INNER display -p -t inner:2.0 '#{pane_title}' ||
    exit 1
THIRD=$($INNER display -p -t inner:2.0 '#{pane_id}')
pane $THIRD 't=a;text/x-moved'
pane_sync $THIRD $DIR/third
expect_none $DIR/out1 "registered for another window"
$INNER join-pane -v -s $THIRD -t $LEFT || exit 1
expect $DIR/out1 't=a;text/x-moved text/plain' "pane joined"
expect $DIR/out2 't=a;text/x-moved text/plain' "pane joined, second client"
$INNER break-pane -d -s $THIRD || exit 1
expect $DIR/out1 't=a;text/plain' "pane broken out"
expect $DIR/out2 't=a;text/plain' "pane broken out, second client"
$INNER swap-pane -s $THIRD -t $LEFT || exit 1
expect $DIR/out1 't=a;text/x-moved text/plain' "pane swapped in"
expect $DIR/out2 't=a;text/x-moved text/plain' "pane swapped in, second client"
$INNER swap-pane -s $THIRD -t $LEFT || exit 1
expect $DIR/out1 't=a;text/plain' "pane swapped out"
expect $DIR/out2 't=a;text/plain' "pane swapped out, second client"

# A pane that goes away cancels its drop.
terminal outer:0 "t=M:$(at $((OFF + 4)) 3):o=1;text/plain"
expect $DIR/out1 "$(printf '%s\n' 't=a:x=1;1:0123456789abcdef0123')" \
    "machine id after registering again"
expect $DIR/right "t=M:$IN:o=1:i=7;text/plain" "drop again"
$INNER kill-pane -t $RIGHT || exit 1
expect $DIR/out1 "$(printf '%s\n' 't=r:o=0' 't=A')" "pane killed"
expect $DIR/out2 "$(printf '%s\n' 't=m:o=0' 't=A')" \
    "pane killed, second client"

exit 0
