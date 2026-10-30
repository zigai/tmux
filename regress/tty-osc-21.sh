#!/bin/sh

# Test colours set with OSC 21 that are passed to the outer terminal. An inner
# client with the kittycolours feature and one without are attached in panes of
# an outer server, which also answers their colour queries. Check what each
# client is sent when a pane sets colours, when the active pane changes and on
# detach, and that palette colours not set in the pane are asked of the outer
# terminal.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
INNER="$TEST_TMUX -LtestI$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestO$$ -f/dev/null"

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"; stop_server $INNER; stop_server $OUTER' 0 1 15

fail()
{
	echo "FAIL: $1"
	exit 1
}

$INNER new-session -d -x 80 -y 24 -s inner 'exec sleep 100' \; \
    set-option -g status off \; \
    set-window-option -g remain-on-exit on \; \
    split-window -d 'exec sleep 100' || exit 1

$OUTER new-session -d -x 80 -y 24 -s outer 'exec sleep 100' \; \
    set-option -g status off \; \
    set-window-option -g window-style 'fg=#eeeeee,bg=#111111' \; \
    set-window-option -g pane-colours[200] '#c840a0' || exit 1
$OUTER pipe-pane -t outer:0 -O "cat >'$DIR/plain'" || exit 1
$OUTER respawn-pane -k -t outer:0 "$INNER attach -t inner" || exit 1
wait_for_clients 1 $INNER || exit 1
$OUTER new-window -d -t outer:1 'exec sleep 100' || exit 1

CAP=
capture()
{
	$OUTER pipe-pane -t outer:1 || exit 1
	CAP=$DIR/$1
	: >"$CAP"
	$OUTER pipe-pane -t outer:1 -O "cat >>'$CAP'" || exit 1
}

sent()
{
	grep -Fq "$(printf "$1")" "$CAP"
}

expect()
{
	wait_until sent "$1" || fail "$2"
}

capture attach
$OUTER respawn-pane -k -t outer:1 "$INNER -T kittycolours attach -t inner" ||
    exit 1
wait_for_clients 2 $INNER || exit 1

capture set
$INNER respawn-pane -k -t inner:0.0 "printf '\033]21;selection_background=red;transparent_background_color2=#102030@0.5\033\\\\'; exec sleep 100" ||
    exit 1
expect '\033]21;selection_background=rgb:ff/00/00;transparent_background_color2=rgb:10/20/30@0.5000\033\\' \
    "set in active pane"

capture other
$INNER select-pane -t inner:0.1 || exit 1
expect '\033]21;selection_background;transparent_background_color2\033\\' \
    "select pane without colours"

capture back
$INNER select-pane -t inner:0.0 || exit 1
expect '\033]21;selection_background=rgb:ff/00/00;transparent_background_color2=rgb:10/20/30@0.5000\033\\' \
    "select pane with colours"

capture reset
$INNER respawn-pane -k -t inner:0.0 "printf '\033]21;selection_background=;transparent_background_color2\033\\\\'; exec sleep 100" ||
    exit 1
expect '\033]21;selection_background=;transparent_background_color2\033\\' \
    "reset in active pane"

$INNER respawn-pane -k -t inner:0.0 \
    "stty raw -echo min 0 time 10; printf '\033]21;200=?;201=?;foreground=?;background=?\033\\\\'; dd bs=1 count=1024 2>/dev/null | cat -v >$DIR/reply" ||
    exit 1
wait_for_output 1 $INNER display -p -t inner:0.0 '#{pane_dead}' ||
    fail "reply reader did not exit"
[ "$(cat "$DIR/reply")" = \
    '^[]21;200=rgb:c8/40/a0;201=;foreground=rgb:ee/ee/ee;background=rgb:11/11/11^[\' ] ||
    fail "palette from outer terminal: $(cat "$DIR/reply")"

capture detach
$INNER respawn-pane -k -t inner:0.0 "printf '\033]21;visual_bell=blue\033\\\\'; exec sleep 100" ||
    exit 1
expect '\033]21;visual_bell=rgb:00/00/ff\033\\' "set before detach"
$INNER detach-client -t "$($INNER list-clients -F '#{client_name} #{client_termfeatures}' | awk '/kittycolours/ { print $1 }')" ||
    exit 1
expect '\033]21;selection_background;visual_bell\033\\' "reset on detach"

CAP=$DIR/plain
sent '\033]21;' && fail "client without kittycolours was sent OSC 21"

exit 0
