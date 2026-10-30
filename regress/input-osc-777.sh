#!/bin/sh

# OSC 777;notify notifications from any pane, visible or not, are sent to
# clients of the pane's session whose terminal has the osc777 feature.

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

CAT="stty raw -echo; exec cat"
A=$($TMUX new-session -d -P -F '#{pane_id}' -s one -x40 -y10 "$CAT") ||
    exit 1
B=$($TMUX new-window -d -P -F '#{pane_id}' -t one: "$CAT") || exit 1
$TMUX set -g status off || exit 1
$TMUX set -as terminal-features 'xterm-256color:osc777' || exit 1

attach full xterm-256color one
attach plain screen one
wait_for_clients 2 $TMUX || exit 1

emit "$A" '\033]777;notify;Title;Body; with ;\007'
wait_until has full '\033]777;notify;Title;Body; with ;\033\\' ||
    fail "notification not forwarded"
emit "$B" '\033]777;notify;Hidden;\033\\'
wait_until has full '\033]777;notify;Hidden;\033\\' ||
    fail "notification from hidden window not forwarded"

# Without a body or with another extension it is not a notification.
emit "$A" '\033]777;notify;No body\007\033]777;other;x;y\007'
emit "$A" '\033]777;notify;After;\007'
wait_until has full '\033]777;notify;After;\033\\' ||
    fail "notification not forwarded"
has full '\033]777;notify;No body' && fail "notification without body"
has full '\033]777;other' && fail "other extension forwarded"

sleep 0.5
has plain '\033]777;' && fail "sent to client without osc777"

exit 0
