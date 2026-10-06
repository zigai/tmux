#!/bin/sh

# Check that display-popup -c without -t opens and closes the popup in the
# window the client is showing, not in the most recently active session.

PATH=/bin:/usr/bin
TERM=screen
export TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"

cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
	$TMUX2 kill-server >/dev/null 2>&1
}
trap cleanup 0 1 15

fail()
{
	echo "$*" >&2
	exit 1
}

popup_window()
{
	$TMUX list-panes -a -F '#{pane_modal_flag} #{session_name}' |
	    awk '$1 == 1 { print $2 }'
}

cleanup

$TMUX new-session -d -s viewed -x 80 -y 24 || exit 1
$TMUX2 new-session -d -s runner -x 80 -y 24 "$TMUX attach -t viewed" ||
    exit 1
sleep 1
$TMUX new-session -d -s idle || exit 1
CLIENT=$($TMUX list-clients -F '#{client_name}' | head -1)
[ -n "$CLIENT" ] || fail "No attached client."

$TMUX display-popup -c "$CLIENT" -w 20 -h 5 'sleep 30' &
sleep 1
[ "$(popup_window)" = "viewed" ] ||
    fail "Popup opened in '$(popup_window)', not the client's session."

$TMUX display-popup -C -c "$CLIENT"
sleep 1
[ -z "$(popup_window)" ] || fail "display-popup -C -c did not close popup."

$TMUX display-popup -c "$CLIENT" -t idle: -w 20 -h 5 'sleep 30' &
sleep 1
[ "$(popup_window)" = "idle" ] || fail "Explicit -t target was ignored."

exit 0
