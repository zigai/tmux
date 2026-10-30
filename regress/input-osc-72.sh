#!/bin/sh

# Kitty drag and drop (OSC 72) from panes: query replies, refused requests and
# registering the outer terminal for drops and drags, with and without the dnd
# terminal feature. An outer tmux runs the client and captures its output.

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
}

logger()
{
	echo "stty raw -echo; printf '\\033]2;ready\\007'; exec cat >>$DIR/$1"
}

attach()
{
	$OUTER respawn-pane -k -t outer "$INNER attach -t inner" || exit 1
	wait_for_clients 1 $INNER || fail "client did not attach"
}

$INNER new -d -s inner -x80 -y24 "$(logger left)" || exit 1
$INNER set -g status off || exit 1
$INNER splitw -h -t inner "$(logger right)" || exit 1
wait_for_output ready $INNER display -p -t inner:0.0 '#{pane_title}' ||
    exit 1
wait_for_output ready $INNER display -p -t inner:0.1 '#{pane_title}' ||
    exit 1
LEFT=$($INNER display -p -t inner:0.0 '#{pane_id}')
RIGHT=$($INNER display -p -t inner:0.1 '#{pane_id}')

$OUTER new -d -s outer -x80 -y24 'sleep 100' || exit 1
$OUTER set -g status off || exit 1
$OUTER set -g default-terminal screen || exit 1
$OUTER pipe-pane -O -t outer "cat >>$DIR/out" || exit 1
attach

# Without the feature, queries are not answered and nothing is sent.
pane $RIGHT 't=q:i=5'
pane $RIGHT 't=r:x=1:i=7'
pane $RIGHT 't=a:i=7;text/plain'
pane_sync $RIGHT $DIR/right
expect $DIR/right '' "answered without dnd"
expect_none $DIR/out "registered without dnd"

# A client with the feature is registered for the pane on attach.
$INNER set -as terminal-features ',screen*:dnd' || exit 1
attach
wait_for_text dnd $INNER list-clients -F '#{client_termfeatures}' ||
    fail "no dnd feature"
expect $DIR/out 't=a;text/plain' "registered on attach"

pane $RIGHT 't=q:i=5'
expect $DIR/right 't=q:i=5' "query reply"
pane $RIGHT 't=q'
expect $DIR/right 't=q' "query reply without i"

# The MIME types of all panes are registered, including chunked lists.
pane $LEFT 't=a;text/uri-list text/plain'
expect $DIR/out 't=a;text/uri-list text/plain' "two panes"
pane $LEFT 't=A'
expect $DIR/out 't=a;text/plain' "one pane"
pane $RIGHT 't=a:i=7:m=1;text/html '
pane $RIGHT 'm=0;image/png'
expect $DIR/out 't=a;text/html image/png' "chunked list"

# Requests with no drop or drag are refused by tmux.
pane $RIGHT 't=r:x=1:i=7'
expect $DIR/right 't=R:x=1:i=7;EPERM' "request before drop"
pane $LEFT 't=o:o=1;text/plain'
expect $DIR/left 't=E;EPERM' "offer before gesture"

# Offering drags registers the terminal as a drag source.
pane $LEFT 't=o:x=1'
expect $DIR/out 't=o:x=1' "offer"
pane $LEFT 't=P:x=0'
expect $DIR/left 't=E;EINVAL' "drag image with no drag"

# Pane state goes when the pane is respawned or killed.
$INNER respawn-pane -k -t $LEFT "$(logger left)" || exit 1
expect $DIR/out 't=o:x=2' "respawned pane"
$INNER kill-pane -t $RIGHT || exit 1
expect $DIR/out 't=A' "killed pane"

exit 0
