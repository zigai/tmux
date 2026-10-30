#!/bin/sh

# Kitty drag and drop (OSC 72) drags from panes: the start gesture goes to the
# pane under the pointer if it offers drags, and the offer, data and events are
# passed between that pane and the client. An outer tmux pane runs the client
# and captures its output.

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
	$OUTER send-keys -t outer -l "$(printf '\033]72;%s\033\\' "$1")"
}

logger()
{
	echo "stty raw -echo; printf '\\033]2;ready\\007'; exec cat >>$DIR/$1"
}

$INNER new -d -s inner -x80 -y24 "$(logger left)" || exit 1
$INNER set -g status on || exit 1
$INNER set -g status-position top || exit 1
$INNER set -as terminal-features ',screen*:dnd' || exit 1
$INNER splitw -h -t inner "$(logger right)" || exit 1
wait_for_output ready $INNER display -p -t inner:0.0 '#{pane_title}' ||
    exit 1
wait_for_output ready $INNER display -p -t inner:0.1 '#{pane_title}' ||
    exit 1
LEFT=$($INNER display -p -t inner:0.0 '#{pane_id}')
RIGHT=$($INNER display -p -t inner:0.1 '#{pane_id}')
OFF=$($INNER display -p -t $RIGHT '#{pane_left}')

$OUTER new -d -s outer -x80 -y25 'sleep 100' || exit 1
$OUTER set -g status off || exit 1
$OUTER set -g default-terminal screen || exit 1
$OUTER pipe-pane -O -t outer "cat >>$DIR/out" || exit 1
$OUTER respawn-pane -k -t outer "$INNER attach -t inner" || exit 1
wait_for_clients 1 $INNER || fail "client did not attach"
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
terminal "t=o:$(at $((OFF + 4)) 3)"
terminal "t=o:$(at 4 0)"
expect_none $DIR/right "gesture over pane not offering"
expect_none $DIR/left "gesture over status line"

# A gesture over the pane goes to it relative to the pane, after its machine
# id is given to the terminal.
terminal "t=o:$(at 5 3)"
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
terminal 't=E;OK'
terminal 't=e:x=1:y=0'
terminal 't=e:x=5:y=0'
expect $DIR/left "$(printf '%s\n' 't=E:i=9;OK' 't=e:x=1:y=0:i=9' \
    't=e:x=5:y=0:i=9')" "drag events"
pane $LEFT 't=e:y=0:m=0;aGk='
expect $DIR/out 't=e:y=0:m=0;aGk=' "drag data"

# Once the drag finishes, more drag commands are refused.
terminal 't=e:x=4:y=0'
expect $DIR/left 't=e:x=4:y=0:i=9' "drag finished"
pane $LEFT 't=P:x=0'
expect $DIR/left 't=E:i=9;EINVAL' "drag over"
expect_none $DIR/out "sent after drag"

# A pane that stops offering during a drag cancels it.
terminal "t=o:$(at 5 3)"
expect $DIR/left "t=o:$(at 5 2):i=9" "second gesture"
pane $LEFT 't=o:x=2'
expect $DIR/out "$(printf '%s\n' 't=E:y=-1' 't=o:x=2')" "stop offering"

# The terminal is unregistered when the client detaches.
pane $LEFT 't=o:x=1'
expect $DIR/out 't=o:x=1' "offer again"
$INNER detach-client -s inner || exit 1
expect $DIR/out 't=o:x=2' "detach"

exit 0
