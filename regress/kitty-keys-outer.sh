#!/bin/sh

# tmux asks a terminal with the Kitty keyboard protocol for what the active
# pane of each client needs, pops it again when the client detaches, and sends
# nothing to a terminal without it. An outer tmux stands in for the terminal.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestI$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestO$$ -f/dev/null"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"; stop_servers' 0
trap 'exit 1' 1 2 15

# The Kitty keyboard sequences written to the terminal, in order.
sent()
{
	od -An -v -t x1 "$1" | tr -s ' \n' '  ' |
	    grep -Eo '1b 5b (3c|3d|3e|3f)( 3[0-9])* 75' | tr -d ' ' |
	    tr '\n' ' '
}

# Attach a client to an inner session from a new outer session, capturing
# everything the client writes.
attach()
{
	$TMUX2 new -d -s "$1" -x80 -y24 \
	    "while [ ! -e '$DIR/go-$1' ]; do sleep 0.01; done; exec $TMUX attach -t '$2'" ||
	    exit 1
	$TMUX2 pipe-pane -O -t "$1" "cat >>'$DIR/$1'" || exit 1
	touch "$DIR/go-$1"
	wait_for_output "$2" $TMUX list-clients -F '#{client_session}' \
	    -f "#{==:#{client_session},$2}" || exit 1
}

outer_mode()
{
	$TMUX2 display -p -t "$1" '#{pane_key_mode}'
}

$TMUX set -g extended-keys on \; new -d -s s1 -x80 -y24 \
    "printf '\\033[>3u'; exec sleep 100" || exit 1
$TMUX new -d -s s2 -x80 -y24 'exec sleep 100' || exit 1
$TMUX2 new -d -s o0 'exec sleep 100' \; set -g extended-keys on || exit 1

# Each client follows its own active pane.
attach o1 s1
attach o2 s2
wait_for_output 'Kitty 31' outer_mode o1 || exit 1
wait_for_output 'Kitty 1' outer_mode o2 || exit 1
$TMUX respawn-pane -k -t s2 "printf '\\033[>8u'; exec sleep 100" || exit 1
wait_for_output 'Kitty 29' outer_mode o2 || exit 1

# Switching windows or entering copy mode changes what is asked for.
$TMUX new-window -t s1 'exec sleep 100' || exit 1
wait_for_output 'Kitty 1' outer_mode o1 || exit 1
$TMUX select-window -t s1:0 || exit 1
wait_for_output 'Kitty 31' outer_mode o1 || exit 1
$TMUX copy-mode -t s1:0 || exit 1
wait_for_output 'Kitty 1' outer_mode o1 || exit 1
$TMUX send-keys -t s1:0 -X cancel || exit 1
wait_for_output 'Kitty 31' outer_mode o1 || exit 1
$TMUX detach-client -s s1 || exit 1
wait_for_output '1b5b3f75 1b5b3e3175 1b5b3d333175 1b5b3d3175 1b5b3d333175 1b5b3d3175 1b5b3d333175 1b5b3c75 ' \
    sent "$DIR/o1" || exit 1

# A terminal which does not answer the query is left alone, unless the
# kittykeys feature says it has the protocol.
$TMUX2 set -g extended-keys off || exit 1
attach o3 s1
wait_for_output 'Ext' $TMUX list-clients -F '#{client_key_mode}' \
    -f '#{==:#{client_session},s1}' || exit 1
[ "$(sent "$DIR/o3")" = '1b5b3f75 ' ] || exit 1
$TMUX detach-client -s s1 || exit 1

$TMUX set -as terminal-features ',*:kittykeys' || exit 1
attach o4 s1
wait_for_output 'Kitty 31' $TMUX list-clients -F '#{client_key_mode}' \
    -f '#{==:#{client_session},s1}' || exit 1
$TMUX detach-client -s s1 || exit 1
wait_for_output '1b5b3f75 1b5b3e333175 1b5b3c75 ' sent "$DIR/o4" || exit 1

exit 0
