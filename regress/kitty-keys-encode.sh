#!/bin/sh

# Keys sent to a pane are encoded for the Kitty keyboard protocol flags that
# pane asked for, as Ghostty encodes them.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
A=$(mktemp)
B=$(mktemp)
trap 'rm -f "$A" "$B"; stop_server $TMUX' 0
trap 'exit 1' 1 2 15

hex()
{
	od -An -v -t x1 "$1" | tr -d ' \n'
}

probe()
{
	: >"$2"
	echo "stty raw -echo; printf '$1'; exec cat >>'$2'"
}

check()
{
	wait_for_output "$(printf "$2" | od -An -v -t x1 | tr -d ' \n')" \
	    hex "$1" || exit 1
}

encode()
{
	$TMUX respawn-pane -k -t:.0 "$(probe "\\033[>$1u" "$A")" || exit 1
	wait_for_output "Kitty $1" $TMUX display -p -t:.0 '#{pane_key_mode}' ||
	    exit 1
	shift
	expected=$1
	shift
	$TMUX send-keys -t:.0 "$@" || exit 1
	check "$A" "$expected"
}

$TMUX new -d -x80 -y24 'exec sleep 100' || exit 1
$TMUX set -g extended-keys on || exit 1

# Associated text, alternate keys and all keys as escapes.
encode 31 '\033[97:65;2;65u' A
encode 24 '\033[97;;97u' a
encode 9 '\033[97;5u\033[9u\033[13u\033[97;2u' C-a Tab Enter A

# With only disambiguation, modified keys and Super are escapes, unmodified
# Enter and keypad text keys stay as they are, and a modified F3 does not use
# its legacy form, which looks like a cursor position report.
encode 1 '\033[97;6u\033[97;9u\033[13;2u\r\033[13;2~' C-S-a Super-a S-Enter \
    Enter S-F3
encode 1 '1+.\033[57414u' KP1 KP+ KP. KPEnter
encode 8 '\033[97u' a

# Event types: a press of a special key gets an explicit event, plain text,
# Enter, Tab and Backspace stay as they are.
encode 3 '\ra\t\177\033[1;1:1A' Enter a Tab BSpace Up

# Cursor keys do not depend on application cursor mode.
$TMUX respawn-pane -k -t:.0 "$(probe '\033[?1h\033[>1u' "$A")" || exit 1
wait_for_output 'Kitty 1' $TMUX display -p -t:.0 '#{pane_key_mode}' || exit 1
$TMUX send-keys -t:.0 Up Escape || exit 1
check "$A" '\033[A\033[27u'

# Each pane gets the encoding it asked for.
$TMUX respawn-pane -k -t:.0 "$(probe '\033[>11u' "$A")" || exit 1
$TMUX split-window -d "$(probe '' "$B")" || exit 1
wait_for_output 'Kitty 11' $TMUX display -p -t:.0 '#{pane_key_mode}' || exit 1
$TMUX send-keys -t:.0 C-a a || exit 1
$TMUX send-keys -t:.1 C-a a || exit 1
check "$A" '\033[97;5u\033[97u'
check "$B" '\001a'

exit 0
