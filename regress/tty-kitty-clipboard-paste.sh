#!/bin/sh

# Kitty clipboard paste events (mode 5522): the mode of the active pane is set
# in the terminal when the clipboard is requested from it, and paste events
# from the terminal go to the active pane if it has the mode.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"
TMP=$(mktemp -d) || exit 1
trap 'stop_servers; rm -rf $TMP' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "$*" >&2
	exit 1
}

# Print the last mode 5522 change sent to the terminal.
mode()
{
	grep -ao '?5522[hl]' $TMP/terminal | tail -1
}

: >$TMP/a
: >$TMP/b
$TMUX new -d -s kc -x40 -y10 "stty raw -echo; printf '\\033[?5522h'; \
    exec cat -v >$TMP/a" || exit 1
$TMUX set -g status off || exit 1
$TMUX set -s get-clipboard request || exit 1
$TMUX set -as terminal-features 'tmux*:kittyclipboard' || exit 1
$TMUX2 new -d -s outer -x40 -y10 \
    "while [ ! -e $TMP/go ]; do sleep 0.01; done; exec $TMUX attach -t kc" ||
    exit 1
$TMUX2 set -g status off || exit 1
$TMUX2 pipe-pane -t outer: -O "cat >>$TMP/terminal" || exit 1
touch $TMP/go
wait_for_clients 1 $TMUX || fail "client did not attach"

wait_for_output '?5522h' mode || fail "mode not set"
$TMUX set -s get-clipboard buffer || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522l' mode || fail "mode set with get-clipboard buffer"
$TMUX set -s get-clipboard request || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522h' mode || fail "mode not set again"

$TMUX split-window -t kc: "stty raw -echo; exec cat -v >$TMP/b" || exit 1
wait_for_output '?5522l' mode || fail "mode set for other pane"

event=$(printf '\033]5522;type=read:status=OK:pw=b3Rw\033\\'; \
    printf '\033]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;'; \
    printf 'dGV4dC9wbGFpbgo=\033\\'; \
    printf '\033]5522;type=read:status=DONE:pw=b3Rw\033\\')
$TMUX2 send-keys -t outer: -l "$event" || exit 1
sleep 1
$TMUX select-pane -t kc:.0 || exit 1
wait_for_output '?5522h' mode || fail "mode not set after select-pane"
$TMUX2 send-keys -t outer: -l "$event" || exit 1
wait_for_output '^[]5522;type=read:status=OK:pw=b3Rw^[\'\
'^[]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;dGV4dC9wbGFpbgo=^[\'\
'^[]5522;type=read:status=DONE:pw=b3Rw^[\' cat $TMP/a ||
    fail "paste event not sent to active pane"
[ -s $TMP/b ] && fail "paste event sent to pane without mode"

exit 0
