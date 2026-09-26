#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX1="$TEST_TMUX -Lmotion-inner-$$ -f/dev/null"
TMUX2="$TEST_TMUX -Lmotion-outer-$$ -f/dev/null"
tmp=$(mktemp -d) || exit 1
TMUX_TMPDIR=$tmp
XDG_CONFIG_HOME=$tmp/config
XDG_CACHE_HOME=$tmp/cache
XDG_DATA_HOME=$tmp/data
XDG_RUNTIME_DIR=$tmp/runtime
mkdir -m 700 "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
    "$XDG_RUNTIME_DIR" || { rm -r "$tmp"; exit 1; }
export TERM TMUX_TMPDIR XDG_CONFIG_HOME XDG_CACHE_HOME XDG_DATA_HOME \
    XDG_RUNTIME_DIR

cleanup()
{
	$TMUX1 kill-server >/dev/null 2>&1
	$TMUX2 kill-server >/dev/null 2>&1
	rm -rf "$tmp"
}
fail()
{
	echo "$1" >&2
	exit 1
}
motion()
{
	set -- $(printf '\033[<35;%s;%sM' "$1" "$2" | od -An -v -tx1)
	$TMUX2 send-keys -t "$outer" -H "$@" || fail "send motion failed"
	sleep 0.2
}

trap cleanup EXIT HUP INT TERM
$TMUX1 new-session -d -s inner -x 80 -y 24 'cat' || fail "inner session failed"
[ "$($TMUX1 show -gv mouse-motion)" = off ] || fail "mouse-motion default is not off"
$TMUX1 set -g mouse on || fail "mouse option failed"
$TMUX1 set-hook -g pane-mouse-moved \
    'set -gF @events "#{@events}|#{hook_x},#{hook_y},#{hook_scroll_position},#{hook_history_generation}" ; set -gF @subject "#{hook_client}:#{hook_pane}"' ||
    fail "hook failed"
$TMUX1 bind -n MouseMovePane 'set -gF @binding "#{mouse_x},#{mouse_y}"' ||
    fail "binding failed"
$TMUX2 new-session -d -x 80 -y 24 "$TMUX1 attach -t inner" ||
    fail "outer session failed"
outer=$($TMUX2 display-message -p '#{pane_id}')
$TMUX2 pipe-pane -t "$outer" "cat > '$tmp/bytes'" || fail "pipe failed"
sleep 0.5
esc=$(printf '\033')
if grep -aF "$esc[?1003h" "$tmp/bytes" >/dev/null; then
	fail "mode 1003 emitted by default"
fi
$TMUX1 set -g mouse-motion on || fail "mouse-motion option failed"
sleep 0.3
$TMUX1 set -g mouse-motion off || fail "disable failed"
sleep 0.3
$TMUX1 set -g mouse-motion on || fail "enable failed"
sleep 0.3
grep -aF "$esc[?1003h" "$tmp/bytes" >/dev/null || fail "mode 1003 not emitted"
grep -aF "$esc[?1003l" "$tmp/bytes" >/dev/null || fail "mode 1003 not disabled"
motion 3 1
[ "$($TMUX1 show -gv @binding)" = "2,0" ] || fail "binding did not fire"
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0" ] || fail "first event missing"
case "$($TMUX1 show -gv @subject)" in
?*:%0) ;;
*) fail "client or pane missing from event" ;;
esac
motion 3 1
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0" ] || fail "duplicate event"
motion 4 1
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0|3,0,0,0" ] || fail "cell change missing"
$TMUX1 set -g mouse-motion off || fail "disable failed"
sleep 0.3
motion 5 1
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0|3,0,0,0" ] || fail "event while disabled"
$TMUX1 set -g mouse-motion on || fail "reenable failed"
sleep 0.3
motion 5 1
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0|3,0,0,0|4,0,0,0" ] ||
    fail "event after reenable missing"
motion 5 24
motion 5 1
[ "$($TMUX1 show -gv @events)" = "|2,0,0,0|3,0,0,0|4,0,0,0|4,0,0,0" ] ||
    fail "event after pane reentry missing"
$TMUX1 send-keys -t inner:0.0 -l "$(seq 1 50)" || fail "pane output failed"
$TMUX1 copy-mode -t inner:0.0 || fail "copy-mode failed"
$TMUX1 send -X -t inner:0.0 page-up || fail "page-up failed"
sleep 0.3
motion 5 1
case "$($TMUX1 show -gv @events)" in
*"|4,0,"[1-9]*,*) ;;
*) fail "viewport change missing" ;;
esac
$TMUX1 split-window -v -t inner:0 'cat' || fail "split failed"
active=$($TMUX1 display-message -pt inner:0 '#{pane_id}')
motion 5 1
[ "$($TMUX1 display-message -pt inner:0 '#{pane_id}')" = "$active" ] ||
    fail "motion changed active pane"
events=$($TMUX1 show -gv @events)
$TMUX1 set -g mouse off || fail "mouse disable failed"
motion 5 1
[ "$($TMUX1 show -gv @events)" = "$events" ] || fail "event with mouse off"
$TMUX1 set -g mouse on || fail "mouse reenable failed"
motion 5 1
[ "$($TMUX1 show -gv @events)" != "$events" ] ||
    fail "event after mouse reenable missing"
