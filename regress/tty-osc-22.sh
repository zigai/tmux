#!/bin/sh

# A client with the pointershape feature is sent the OSC 22 pointer shape of
# the pane under the mouse, or of the active pane before any mouse event.
# Each inner client runs in an outer pane whose output is captured.

PATH=/bin:/usr/bin
TERM=screen
export PATH TERM

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
INNER="$TEST_TMUX -LtestI$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestO$$ -f/dev/null"
DIR=$(mktemp -d) || exit 1

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	stop_server $OUTER
	stop_server $INNER
	rm -rf "$DIR"
}
trap cleanup 0
trap 'exit 1' 1 2 15

attach()
{
	name=$1
	session=$2
	term=$3
	clients=$4

	$OUTER new-window -d -n "$name" \
	    "while [ ! -e '$DIR/go-$name' ]; do sleep 0.01; done; TERM=$term exec $INNER attach -t =$session" ||
	    exit 1
	$OUTER pipe-pane -t "=outer:=$name" -O "cat >>'$DIR/$name'" || exit 1
	touch "$DIR/go-$name"
	wait_for_output "$clients" $INNER display-message -p -t "=$session:" \
	    '#{session_attached}' || exit 1
}

shapes()
{
	cat -v "$DIR/$1" | grep -o '\^\[\]22;[^^]*' |
	    sed 's/^^\[\]22;//; s/^$/reset/' | paste -s -d ' ' -
}

check()
{
	wait_until output_is "$2" shapes "$1" ||
	    fail "$1: got '$(shapes "$1")', expected '$2'"
}

emit()
{
	$INNER send-keys -t "$1" -l "$(printf "$2")" || exit 1
}

move()
{
	bytes=$(printf '\033[<35;%d;%dM' "$2" "$3" | od -An -v -tx1)
	$OUTER send-keys -t "=outer:=$1" -H $bytes || exit 1
}

$INNER new-session -d -s a -x80 -y24 'stty raw -echo; exec cat' || exit 1
$INNER set-option -g mouse on || exit 1
$INNER set-option -s terminal-features 'screen:pointershape@' || exit 1
$INNER split-window -h -t =a: 'stty raw -echo; exec cat' || exit 1
$INNER new-session -d -s b -x80 -y24 'stty raw -echo; exec cat' || exit 1
left=$($INNER display-message -p -t =a:.0 '#{pane_id}')
right=$($INNER display-message -p -t =a:.1 '#{pane_id}')

$OUTER new-session -d -s outer -x80 -y24 'exec sleep 1000' || exit 1
$OUTER set-option -g status off || exit 1
$OUTER set-option -g remain-on-exit on || exit 1
attach a a tmux-256color 1
check a ''

# The active pane decides until the mouse moves.
emit "$left" '\033]22;pointer\033\\'
$INNER select-pane -t "$left" || exit 1
check a 'pointer'
cat -v "$DIR/a" | grep -q '\^\[\[?1003h' || fail "a: no motion mouse mode"

move a 60 5
check a 'pointer reset'
move a 10 5
check a 'pointer reset pointer'
move a 41 5
check a 'pointer reset pointer reset'
move a 10 5
move a 10 24
check a 'pointer reset pointer reset pointer reset'
move a 10 5
check a 'pointer reset pointer reset pointer reset pointer'
: >"$DIR/a"

emit "$left" '\033]22;>wait\033\\'
check a 'wait'
emit "$left" '\033]22;<\033\\'
check a 'wait pointer'
emit "$left" '\033[?1049h'
check a 'wait pointer reset'
emit "$left" '\033[?1049l'
check a 'wait pointer reset pointer'
: >"$DIR/a"

# Changing the mouse mode can reset the shape, so it is sent again after.
$INNER set-option -g mouse off || exit 1
check a 'pointer'
cat -v "$DIR/a" | tr -d '\n' | sed 's/.*?1003l//' | grep -q ']22;pointer' ||
    fail "a: shape not sent after mouse mode change"
$INNER set-option -g mouse on || exit 1
check a 'pointer pointer'
: >"$DIR/a"

# Clients follow their own panes; a client without the feature gets nothing.
attach b b tmux-256color 1
attach c b screen 2
emit =b: '\033]22;crosshair\033\\'
check b 'crosshair'
check a ''
wait_for_text '?1002h' cat -v "$DIR/c" || fail "c: no mouse mode"
if cat -v "$DIR/c" | grep -q '\]22;\|?1003h'; then
	fail "c: pointer shape sent without feature"
fi

# Switching windows uses the pane now under the mouse.
$INNER new-window -t =a: 'stty raw -echo; exec cat' || exit 1
check a 'reset'
$INNER previous-window -t =a: || exit 1
check a 'reset pointer'
$INNER kill-pane -t "$left" || exit 1
check a 'reset pointer reset'

# Queries are answered from the client's features.
$INNER new-window -d -t =b: \
    "stty raw -echo; printf '\\033]22;?pointer,no-such-shape\\033\\\\'; exec cat -v" ||
    exit 1
wait_for_output '^[]22;1,0^[\' $INNER capture-pane -p -t =b:\$ || exit 1

$INNER detach-client -s =b || exit 1
check b 'crosshair reset'

exit 0
