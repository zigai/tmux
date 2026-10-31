#!/bin/sh

# send-keys types a key name the pane cannot encode as text, without a stray
# Escape before it.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
OUT=$(mktemp)
trap 'rm -f "$OUT"; stop_server $TMUX' 0
trap 'exit 1' 1 2 15

$TMUX new -d -x80 -y24 "stty raw -echo; exec cat >>'$OUT'" || exit 1
$TMUX send-keys C-Escape M-C-BSpace M-F13 Pause LeftShift x || exit 1
wait_for_output C-EscapeM-C-BSpaceM-F13PauseLeftShiftx cat "$OUT" || exit 1

exit 0
