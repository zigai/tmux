#!/bin/sh

# Kitty drag and drop (OSC 72) drops from the outer terminal go to the pane
# under the pointer with pane coordinates, and replies go back to the client
# with the drop. Outer tmux panes run the clients and capture their output.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
INNER="$TEST_TMUX -LtestI$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestO$$ -f/dev/null"
DIR=$(test_tmpdir)

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	stop_server $OUTER
	stop_server $INNER
	rm -rf "$DIR"
}
trap cleanup 0
trap 'exit 1' 1 2 15

osc72()
{
	tr '\033\007' '\n\n' <"$1" | sed -n 's/^]72;//p'
}

expect()
{
	wait_for_output "$2" osc72 "$1" || fail "$3: got '$(osc72 "$1")'"
	: >"$1"
}

expect_none()
{
	sleep 0.5
	[ -z "$(osc72 "$1")" ] || fail "$2: got '$(osc72 "$1")'"
}

pane()
{
	printf '\033]72;%s\033\\' "$2" >"$($INNER display -p -t "$1" \
	    '#{pane_tty}')"
}

# Ask for a status report, so all earlier output from the pane is handled.
pane_sync()
{
	printf '\033[5n' >"$($INNER display -p -t "$1" '#{pane_tty}')"
	wait_for_text '[0n' cat -v "$2" || fail "no status report"
	: >"$2"
}

terminal()
{
	$OUTER send-keys -t "$1" -l "$(printf '\033]72;%s\033\\' "$2")"
}

logger()
{
	echo "stty raw -echo; printf '\\033]2;ready\\007'; exec cat >>$DIR/$1"
}

$INNER new -d -s inner -x80 -y24 "$(logger left)" || exit 1
$INNER set -g status off || exit 1
$INNER set -as terminal-features ',screen*:dnd' || exit 1
$INNER splitw -h -t inner "$(logger right)" || exit 1
wait_for_output ready $INNER display -p -t inner:0.0 '#{pane_title}' ||
    exit 1
wait_for_output ready $INNER display -p -t inner:0.1 '#{pane_title}' ||
    exit 1
LEFT=$($INNER display -p -t inner:0.0 '#{pane_id}')
RIGHT=$($INNER display -p -t inner:0.1 '#{pane_id}')
OFF=$($INNER display -p -t $RIGHT '#{pane_left}')
pane $RIGHT 't=a:i=7;text/plain'
pane_sync $RIGHT $DIR/right

$OUTER new -d -s outer -x80 -y24 'sleep 100' || exit 1
$OUTER set -g status off || exit 1
$OUTER set -g default-terminal screen || exit 1
$OUTER pipe-pane -O -t outer:0 "cat >>$DIR/out1" || exit 1
$OUTER respawn-pane -k -t outer:0 "$INNER attach -t inner" || exit 1
wait_for_clients 1 $INNER || fail "client did not attach"
expect $DIR/out1 't=a;text/plain' "registered"
CW=$($INNER display -p '#{client_cell_width}')
CH=$($INNER display -p '#{client_cell_height}')

# Cell (x, y) of the client with a pixel offset of 5 into the cell.
at()
{
	echo "x=$1:y=$2:X=$(($1 * CW + 5)):Y=$(($2 * CH + 5))"
}
IN=$(at 4 3)

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
terminal outer:0 't=R:x=2;ENOENT'
expect $DIR/right "$(printf '%s\n' 't=r:x=1:i=7:m=1;aGVs' 'i=7:m=0;bG8=' \
    't=r:x=1:i=7:m=0;' 't=R:x=2:i=7;ENOENT')" "data"
pane $RIGHT 't=r:o=1'
expect $DIR/out1 't=r:o=1' "finish"
pane $RIGHT 't=r:x=1'
expect $DIR/right 't=R:x=1:i=7;EPERM' "request after finish"

# A drag leaving the window is passed on.
terminal outer:0 "t=m:$(at $((OFF + 4)) 3):o=3;text/plain"
expect $DIR/right "t=m:$IN:o=3:i=7;text/plain" "new drag"
expect $DIR/out1 't=m:o=0' "new drag refused until the pane answers"
terminal outer:0 't=m:x=-1:y=-1'
expect $DIR/right 't=m:x=-1:y=-1:i=7' "left window"

# A second client is registered too and has its own drag.
$OUTER neww -d -t outer:1 'sleep 100' || exit 1
$OUTER pipe-pane -O -t outer:1 "cat >>$DIR/out2" || exit 1
$OUTER respawn-pane -k -t outer:1 "$INNER attach -t inner" || exit 1
wait_for_clients 2 $INNER || fail "second client did not attach"
expect $DIR/out2 't=a;text/plain' "second client registered"
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
