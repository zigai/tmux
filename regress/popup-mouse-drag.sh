#!/bin/sh

# Check that a mouse drag started inside a display-popup keeps going to the
# popup: motion and release outside the popup are forwarded (clamped to its
# edges), and losing focus mid-drag sends the popup a release.

PATH=/bin:/usr/bin
TERM=screen
export TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
. ./regress-common.inc
TMUX="$TEST_TMUX -Lpopup-drag-outer-$$ -f/dev/null"
TMUX2="$TEST_TMUX -Lpopup-drag-runner-$$ -f/dev/null"
INNER="$TEST_TMUX -Lpopup-drag-inner-$$ -f/dev/null"

cleanup()
{
	stop_server $TMUX
	stop_server $TMUX2
	stop_server $INNER
}
trap cleanup 0 1 15

fail()
{
	echo "$*" >&2
	exit 1
}

send_mouse()
{
	$TMUX2 send-keys -t "$OUTER" -l "$(printf "$1")" || fail "send failed"
	sleep 0.2
}

setup()
{
	cleanup
	$TMUX new-session -d -s outer -x 100 -y 40 'sleep 100' || exit 1
	$TMUX set -g mouse on || exit 1
	$TMUX set -g focus-events on || exit 1
	$TMUX set -g status-position "$1" || exit 1
	$TMUX set -s escape-time 0 || exit 1
	$TMUX2 new-session -d -s runner -x 100 -y 41 "$TMUX attach -t outer" || exit 1
	sleep 0.5
	OUTER=$($TMUX2 display-message -p -t runner '#{pane_id}')
	STATUS_ROW=$($TMUX list-clients -F '#{client_height}' | head -1)
	[ "$1" = top ] && STATUS_ROW=1
	$INNER new-session -d -s consumer -x 58 -y 18 \
	    'printf "abcdefghijklmnopqrstuvwxyz\\nabcdefghijklmnopqrstuvwxyz\\n"; sleep 100' || exit 1
	$INNER set -g status off || exit 1
	$INNER set -g mouse on || exit 1
	$INNER set -s escape-time 0 || exit 1
	$TMUX run-shell -b "$TMUX display-popup -w 60 -h 20 -x 20 -y 30 'TMUX= $INNER attach -t consumer'" || exit 1
	sleep 0.5
	POPUP=$($TMUX list-panes -t outer -F '#{?pane_modal_flag,#{pane_id},}' | grep '%')
	[ -n "$POPUP" ] || fail "No modal pane"
	LEFT=$($TMUX display -pt "$POPUP" '#{pane_left}')
	TOP=$($TMUX display -pt "$POPUP" '#{pane_top}')
	[ "$1" = top ] && TOP=$((TOP + 1))
	PRESS=$(printf '\033[<0;%s;%sM' "$((LEFT + 3))" "$((TOP + 1))")
	MOVE=$(printf '\033[<32;%s;%sM' "$((LEFT + 8))" "$((TOP + 1))")
}

start_drag()
{
	$TMUX2 send-keys -t "$OUTER" -l "$PRESS" || exit 1
	sleep 0.1
	$TMUX2 send-keys -t "$OUTER" -l "$MOVE" || exit 1
	sleep 0.2
	[ "$($INNER display -p '#{pane_in_mode}:#{selection_active}')" = '1:1' ] ||
	    fail "Nested consumer did not start selecting"
}

check_release()
{
	[ "$($INNER display -p '#{pane_in_mode}')" = 0 ] ||
	    fail "$1: nested copy-mode did not exit on release"
	$INNER show-buffer >/dev/null 2>&1 || fail "$1: selection was not copied"
}

check()
{
	setup "$1"
	start_drag
	if [ "$2" = status ]; then
		send_mouse "$(printf '\033[<32;5;%sM\033[<0;5;%sm' "$STATUS_ROW" "$STATUS_ROW")"
	else
		send_mouse "$2"
	fi
	check_release "$3"
}

CASE=${1:-all}
if [ "$CASE" = all ] || [ "$CASE" = outside ]; then
	check bottom '\033[<32;5;5M\033[<0;5;5m' outside
fi
if [ "$CASE" = all ] || [ "$CASE" = status ]; then
	check bottom status bottom-status
	check top status top-status
fi
if [ "$CASE" = all ] || [ "$CASE" = viewport ]; then
	check bottom '\033[<32;150;60M\033[<0;150;60m' viewport
fi
if [ "$CASE" = all ] || [ "$CASE" = focus ]; then
	check bottom '\033[O' focus
	$TMUX display-popup -C || exit 1
	sleep 0.2
	$TMUX set -g focus-follows-mouse on || exit 1
	$TMUX split-window -h -t outer 'sleep 100' || exit 1
	RIGHT=$($TMUX display -pt outer '#{pane_id}')
	send_mouse '\033[I\033[<34;5;5M\033[<34;6;5M'
	[ "$($TMUX display -pt outer '#{pane_id}')" != "$RIGHT" ] ||
	    fail "Focus loss left an outer drag suppressing hover"
fi
if [ "$CASE" = all ] || [ "$CASE" = clicks ]; then
	setup bottom
	$INNER bind -n MouseDown1Pane 'set -gF @click_types "#{@click_types}first"; copy-mode; send -X begin-selection' || exit 1
	$INNER bind -n SecondClick1Pane 'set -gF @click_types "#{@click_types}second"; copy-mode; send -X begin-selection' || exit 1
	$INNER bind -n TripleClick1Pane 'set -gF @click_types "#{@click_types}third"; copy-mode; send -X begin-selection' || exit 1
	$INNER bind -T copy-mode MouseUp1Pane 'send -X cancel' || exit 1
	$INNER bind -T copy-mode SecondClick1Pane 'send -X begin-selection' || exit 1
	$INNER bind -T copy-mode TripleClick1Pane 'send -X begin-selection' || exit 1
	for click in first second third; do
		$TMUX2 send -t "$OUTER" -l "$PRESS" || exit 1
		sleep 0.05
		[ "$($INNER display -p '#{pane_in_mode}')" = 1 ] ||
		    fail "$click press did not enter nested copy-mode"
		send_mouse "$(printf '\033[<0;5;%sm' "$STATUS_ROW")"
		[ "$($INNER display -p '#{pane_in_mode}')" = 0 ] ||
		    fail "$click click release on status was not captured"
	done
	[ "$($INNER show -gv @click_types)" = firstsecondthird ] ||
	    fail "Click test did not exercise second and third presses"
fi

if [ "$CASE" = all ] || [ "$CASE" = focus-batch ]; then
	setup top
	$TMUX2 send -t "$OUTER" -l "$PRESS$MOVE$(printf '\033[O')" || exit 1
	sleep 0.5
	check_release focus-batch
fi

if [ "$CASE" = all ] || [ "$CASE" = phantom ] || [ "$CASE" = resume ] || [ "$CASE" = rapid-tabs ]; then
	cleanup
	$TMUX new-session -d -s outer -x 100 -y 40 'sleep 100' || exit 1
	$TMUX set -g mouse on || exit 1
	$TMUX set -g focus-follows-mouse on || exit 1
	$TMUX set -g status-position top || exit 1
	$TMUX set -g status-left '' || exit 1
	$TMUX set -g status-right '' || exit 1
	$TMUX set -g window-status-format '#I' || exit 1
	$TMUX set -g window-status-current-format '#I' || exit 1
	$TMUX set -g window-status-separator ' ' || exit 1
	$TMUX split-window -h -t outer 'sleep 100' || exit 1
	RIGHT=$($TMUX display -pt outer '#{pane_id}')
	$TMUX new-window -d -t outer -n target 'sleep 100' || exit 1
	$TMUX bind -n MouseMovePane 'set -gF @hover "#{mouse_x},#{mouse_y}"' || exit 1
	$TMUX2 new-session -d -s runner -x 100 -y 41 "$TMUX attach -t outer" || exit 1
	sleep 0.5
	OUTER=$($TMUX2 display -p -t runner '#{pane_id}')
	$TMUX2 send -t "$OUTER" -l "$(printf '\033[<0;3;1M\033[<0;3;1m')" || exit 1
	sleep 0.05
	[ "$($TMUX display -pt outer '#{window_index}')" = 1 ] ||
	    fail "First rapid tab click did not switch windows"
	$TMUX2 send -t "$OUTER" -l "$(printf '\033[<0;1;1M\033[<0;1;1m')" || exit 1
	sleep 0.05
	[ "$($TMUX display -pt outer '#{window_index}')" = 0 ] ||
	    fail "Rapid click on a different tab was treated as a second click"
	[ "$CASE" = rapid-tabs ] && exit 0
	sleep 0.35
	$TMUX bind -n MouseDown3Pane 'set -g @right_press yes' || exit 1
	$TMUX bind -n MouseDrag3Pane 'set -g @real_drag yes' || exit 1
	$TMUX set -g lock-command true || exit 1
	send_mouse '\033[<2;70;5M\033[<34;71;5M'
	[ "$($TMUX show -gv @real_drag)" = yes ] || fail "Resume setup did not start a drag"
	$TMUX lock-client || exit 1
	sleep 0.5
	$TMUX set -gu @real_drag || exit 1
	send_mouse '\033[<34;5;5M\033[<34;6;5M'
	[ "$($TMUX show -gv @hover 2>/dev/null)" = '5,3' ] ||
	    fail "Phantom motion after lock/resume was not hover"
	[ "$($TMUX show -gv @real_drag 2>/dev/null)" != yes ] ||
	    fail "Lock/resume retained a pressed button and rearmed drag"
	[ "$CASE" = resume ] && exit 0
	$TMUX select-pane -t "$RIGHT" || exit 1
	send_mouse '\033[<35;70;5M\033[<34;5;5M\033[<34;6;5M'
	[ "$($TMUX show -gv @hover)" = '5,3' ] || fail "Phantom motion was not hover"
	[ "$($TMUX display -pt outer '#{pane_id}')" != "$RIGHT" ] ||
	    fail "Phantom motion suppressed focus-follows-mouse"
	send_mouse '\033[<0;3;1M\033[<0;3;1m'
	[ "$($TMUX display -pt outer '#{window_index}')" = 1 ] ||
	    fail "One tab click after phantom motion did not switch windows"
	$TMUX select-window -t outer:0 || exit 1
	send_mouse '\033[<0;70;5m\033[<34;5;5M\033[<34;7;5M'
	[ "$($TMUX show -gv @hover)" = '6,3' ] || fail "Phantom motion after release was not hover"
	$TMUX bind -n MouseDown3Pane 'set -g @right_press yes' || exit 1
	$TMUX bind -n MouseDrag3Pane 'set -g @real_drag yes' || exit 1
	$TMUX bind -n MouseDragEnd3Pane 'set -gF @order "#{@order}end"' || exit 1
	$TMUX bind -n MouseDown1Status 'set -gF @order "#{@order}press"; select-window -t outer:1' || exit 1
	send_mouse '\033[<2;70;5M\033[<34;71;5M'
	[ "$($TMUX show -gv @real_drag)" = yes ] || fail "Real right drag was rejected"
	send_mouse '\033[<0;3;1M'
	[ "$($TMUX show -gv @order)" = endpress ] ||
	    fail "Lost-release recovery did not dispatch prior drag end then original press"
	[ "$($TMUX display -pt outer '#{window_index}')" = 1 ] ||
	    fail "New press ending a stale drag was swallowed"
fi

if [ "$CASE" = all ] || [ "$CASE" = modifiers ]; then
	setup top
	$INNER respawn-pane -k 'stty raw -echo; printf "\033[?1003h\033[?1006h"; dd bs=1 count=10 2>/dev/null | od -An -tx1; sleep 100' || exit 1
	sleep 0.3
	$TMUX2 send -t "$OUTER" -l "$(printf '\033[<62;%s;%sM' "$((LEFT + 3))" "$((TOP + 1))")" || exit 1
	sleep 0.3
	$INNER capture-pane -p | grep -F '1b 5b 3c 36 33 3b 33 3b 31 4d' >/dev/null ||
	    fail "Normalizing phantom motion lost Shift/Meta/Ctrl packet modifiers"
fi

exit 0
