#!/bin/sh

# send-keys drops a key name the pane cannot encode, rather than typing the
# name as text or leaving a stray Escape behind.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
OUT=$(mktemp)
trap 'rm -f "$OUT"; stop_server $TMUX' 0
trap 'exit 1' 1 2 15

hex()
{
	od -An -v -t x1 "$1" | tr -d ' \n'
}

$TMUX new -d -x80 -y24 "stty raw -echo; exec cat >>'$OUT'" || exit 1
$TMUX send-keys C-Escape C-BSpace M-C-BSpace M-F13 x || exit 1
wait_for_output 78 hex "$OUT" || exit 1

exit 0
