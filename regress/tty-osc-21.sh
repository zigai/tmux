#!/bin/sh

# Test colours set with OSC 21 that are passed to the outer terminal. An inner
# client with the kittycolours feature and one without are attached in panes of
# an outer server, which also answers their colour queries. The client with the
# feature finds it because the outer terminal is tmux; the one without uses a
# terminal tmux does not ask for its features. Check what each client is sent
# when a pane sets colours, when the active pane changes, on respawn and on
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
    set-window-option -g pane-colours[200] '#c840a0' \; \
    set-window-option -g pane-colours[201] '#30a050' || exit 1
$OUTER pipe-pane -t outer:0 -O "cat >'$DIR/plain'" || exit 1
$OUTER respawn-pane -k -t outer:0 "TERM=vt52 $INNER attach -t inner" ||
    exit 1
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
$OUTER respawn-pane -k -t outer:1 "$INNER attach -t inner" || exit 1
wait_for_clients 2 $INNER || exit 1
kittycolours()
{
	$INNER list-clients -F '#{client_termfeatures}' | grep -q kittycolours
}
wait_until kittycolours || fail "outer tmux has no kittycolours feature"

$INNER respawn-pane -k -t inner:0.0 "stty raw -echo; printf '\033]2;ready\007'; exec cat" ||
    exit 1
wait_for_output ready $INNER display -p -t inner:0.0 '#{pane_title}' ||
    exit 1

output()
{
	$INNER send-keys -t inner:0.0 -l "$(printf "$1")" || exit 1
}

capture set
output '\033]21;selection_background=red;transparent_background_color2=#102030@0.5\033\\'
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
output '\033]21;selection_background=;transparent_background_color2\033\\'
expect '\033]21;selection_background=;transparent_background_color2\033\\' \
    "reset in active pane"

capture respawn
output '\033]21;visual_bell=blue\033\\'
expect '\033]21;visual_bell=rgb:00/00/ff\033\\' "set before respawn"
$INNER respawn-pane -k -t inner:0.0 'exec sleep 100' || exit 1
expect '\033]21;selection_background;visual_bell\033\\' "reset on respawn"

query()
{
	$INNER respawn-pane -k -t inner:0.0 \
	    "stty raw -echo min 0 time 10; printf '$2'; dd bs=1 count=1024 2>/dev/null | cat -v >$DIR/reply" ||
	    exit 1
	wait_for_output 1 $INNER display -p -t inner:0.0 '#{pane_dead}' ||
	    fail "$1: reply reader did not exit"
	[ "$(cat "$DIR/reply")" = "$3" ] ||
	    fail "$1: $(cat "$DIR/reply")"
}

# Palette 200 and 201 are answered by the outer terminal, 202 is not.
query "palette from outer terminal" \
    '\033]21;200=?;201=?;foreground=?;background=?\033\\' \
    '^[]21;200=rgb:c8/40/a0;201=rgb:30/a0/50;foreground=rgb:ee/ee/ee;background=rgb:11/11/11^[\'
query "palette not answered" \
    '\033]21;202=?;200=?;foreground=?\033\\' \
    '^[]21;202=;200=rgb:c8/40/a0;foreground=rgb:ee/ee/ee^[\'
query "reply after earlier pending reply" \
    '\033]4;200;?\033\\\033]21;foreground=?\033\\' \
    '^[]4;200;rgb:c8c8/4040/a0a0^[\^[]21;foreground=rgb:ee/ee/ee^[\'

# A palette index asked twice in one sequence is asked of the outer terminal
# once.
asked()
{
	cat "$DIR/plain" "$CAP" | grep -aoF "$(printf "\033]4;$1;?")" | wc -l
}
capture twice
before=$(asked 200)
query "palette index asked twice" '\033]21;200=?;200=?\033\\' \
    '^[]21;200=rgb:c8/40/a0;200=rgb:c8/40/a0^[\'
[ $(($(asked 200) - before)) -eq 1 ] ||
    fail "palette 200 asked $(($(asked 200) - before)) times"

# An OSC 21 reply from the outer terminal is not passed to the pane.
$INNER respawn-pane -k -t inner:0.0 "stty raw -echo; cat -v >$DIR/typed" ||
    exit 1
$OUTER send-keys -t outer:1 -l "$(printf '\033]21;unknown=eA\033\\')" ||
    exit 1
$OUTER send-keys -t outer:1 Z || exit 1
wait_for_output Z cat "$DIR/typed" ||
    fail "OSC 21 reply passed to pane: $(cat "$DIR/typed")"

CAP=$DIR/plain
sent '\033]21;' && fail "client without kittycolours was sent OSC 21"
$INNER detach-client -t "$($INNER list-clients -F '#{client_name} #{client_termname}' | awk '/vt52/ { print $1 }')" ||
    exit 1
wait_for_clients 1 $INNER || exit 1

# A query still waiting for the outer terminal is answered when the client it
# was asked of detaches, and the colours the pane set are reset.
capture detach
$INNER respawn-pane -k -t inner:0.0 \
    "stty raw -echo min 0 time 10; printf '\033]21;visual_bell=blue;foreground=?;202=?\033\\\\'; dd bs=1 count=1024 2>/dev/null | cat -v >$DIR/reply" ||
    exit 1
expect '\033]21;visual_bell=rgb:00/00/ff\033\\' "set before detach"
expect '\033]4;202;?' "palette asked before detach"
$INNER detach-client || exit 1
expect '\033]21;visual_bell\033\\' "reset on detach"
wait_for_output 1 $INNER display -p -t inner:0.0 '#{pane_dead}' ||
    fail "detach with query: reply reader did not exit"
[ "$(cat "$DIR/reply")" = '^[]21;foreground=rgb:ee/ee/ee;202=^[\' ] ||
    fail "detach with query: $(cat "$DIR/reply")"

exit 0
