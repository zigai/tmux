#!/bin/sh

# OSC 9 notifications from any pane, visible or not, are sent to clients of the
# pane's session whose terminal has the osc9 feature. OSC 9 forms that are
# ConEmu sequences are not notifications and OSC 9;4 and OSC 9;9 still work.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestB$$ -f/dev/null"
trap 'stop_server $OUTER; stop_server $TMUX; rm -rf "$DIR"' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "FAIL: $*"
	exit 1
}

# Attach a client with a terminal type and capture what it writes.
attach()
{
	$OUTER new-session -d -s "$1" -x40 -y10 \
	    "while [ ! -e '$DIR/go-$1' ]; do sleep 0.01; done;
	    exec env TERM=$2 $TMUX attach-session -t $3" || exit 1
	$OUTER pipe-pane -t "$1" -O "cat >'$DIR/$1'" || exit 1
	touch "$DIR/go-$1"
}

emit()
{
	$TMUX send-keys -t "$1" -l "$(printf "$2")" || exit 1
}

has()
{
	grep -Fq "$(printf "$2")" "$DIR/$1"
}

# Check a notification is forwarded with ST.
check()
{
	emit "$1" "\033]9;$2\007"
	wait_until has full "\033]9;$2\033\\\\" || fail "'$2' not forwarded"
}

# Check something is not forwarded, using a later notification to know that
# tmux has handled it.
check_none()
{
	check_none_n=$((check_none_n + 1))
	emit "$1" "\033]9;$2\007\033]9;after-$check_none_n\007"
	wait_until has full "\033]9;after-$check_none_n\033\\\\" ||
	    fail "after '$2' not forwarded"
	has full "\033]9;$2\033\\\\" && fail "'$2' forwarded"
}
check_none_n=0

CAT="stty raw -echo; exec cat"
A=$($TMUX new-session -d -P -F '#{pane_id}' -s one -x40 -y10 "$CAT") ||
    exit 1
B=$($TMUX new-window -d -P -F '#{pane_id}' -t one: "$CAT") || exit 1
C=$($TMUX new-session -d -P -F '#{pane_id}' -s other -x40 -y10 "$CAT") ||
    exit 1
$TMUX set -g status off || exit 1
$TMUX set -as terminal-features 'xterm-256color:osc9' || exit 1

attach full xterm-256color one
attach plain screen one
attach elsewhere xterm-256color other
wait_for_clients 3 $TMUX || exit 1

check "$A" 'Build done'
check "$A" 'caf\303\251; 50%%'
check "$B" 'From hidden window'

# Text which only looks like ConEmu is still a notification.
for i in 1 '10;4' 11 '13;x' 2 '4;5' 4x; do
	check "$A" "$i"
done
for i in '1;100' 10 '10;2' '11;comment' 12 '2;box' '3;tab' '4;0' 5 \
    '5 later' '6;macro' '7;run' '8;VAR'; do
	check_none "$A" "$i"
done

$TMUX respawn-pane -k -t "$A" \
    "stty raw -echo; printf '\033]9;bad \377\007\033]9;after-bad\007'; exec cat"
wait_until has full '\033]9;after-bad\033\\' || fail "after-bad not forwarded"
has full '\033]9;bad' && fail "invalid UTF-8 forwarded"

emit "$A" '\033]9;4;1;40\007'
wait_for_output 40 $TMUX display -p -t "$A" '#{pane_pb_progress}' ||
    fail "progress not set"
check_none "$A" '9;/tmp'
wait_for_output "file://$(uname -n)/tmp" $TMUX display -p -t "$A" \
    '#{pane_path}' || fail "path not set"

# Nothing is sent from a pane where notifications are not allowed.
$TMUX set -p -t "$A" allow-notifications off || exit 1
emit "$A" '\033]9;Not allowed\007seen'
wait_for_text seen $TMUX capture-pane -p -t "$A" || exit 1
$TMUX set -p -t "$A" allow-notifications on || exit 1
check "$A" 'Allowed again'
has full '\033]9;Not allowed' && fail "sent when not allowed"

# Only clients of the pane's session with the feature get notifications.
emit "$C" '\033]9;Other\007'
wait_until has elsewhere '\033]9;Other\033\\' || fail "Other not forwarded"
sleep 0.5
has full '\033]9;Other' && fail "Other sent to another session"
has elsewhere '\033]9;Build' && fail "Build sent to another session"
has plain '\033]9;Build done' && fail "sent to client without osc9"
has plain '\033]9;From hidden' && fail "sent to client without osc9"

exit 0
