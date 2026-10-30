#!/bin/sh

# Kitty keyboard protocol flags in a pane are a stack of eight entries for each
# of the main and alternate screens, changed with CSI > u, CSI < u and CSI = u
# and queried with CSI ? u.

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

replies()
{
	: >"$OUT"
	$TMUX respawn-pane -k "stty raw -echo; printf '$1'; exec cat >'$OUT'" ||
	    exit 1
	wait_for_output "$(printf "$2" | od -An -v -t x1 | tr -d ' \n')" \
	    hex "$OUT" || exit 1
}

$TMUX new -d -x80 -y24 'exec sleep 100' || exit 1

# Nothing is answered while extended-keys is off.
replies '\033[?u\033[c' '\033[?1;2c'

$TMUX set -g extended-keys on || exit 1
replies '\033[?u' '\033[?0u'
replies '\033[>1u\033[>3u\033[=4;2u\033[?u\033[=2;3u\033[?u' \
    '\033[?7u\033[?5u'

# Flags above 31 and unknown set modes are ignored, popping none does nothing
# and popping more than the stack holds empties it.
replies '\033[>1u\033[>32u\033[=40u\033[=2;4u\033[?u\033[<0u\033[?u\033[<9u\033[?u' \
    '\033[?1u\033[?1u\033[?0u'

# A full stack drops its oldest entry.
replies '\033[>1u\033[>2u\033[>3u\033[>4u\033[>5u\033[>6u\033[>7u\033[>8u\033[>9u\033[?u\033[<7u\033[?u\033[<u\033[?u' \
    '\033[?9u\033[?2u\033[?0u'

# The alternate screen has its own stack, kept between uses.
replies '\033[>1u\033[?1049h\033[?u\033[>8u\033[?u\033[?1049l\033[?u\033[?1049h\033[?u' \
    '\033[?0u\033[?8u\033[?1u\033[?8u'

# A reset empties both stacks.
replies '\033[>5u\033[?1049h\033[>6u\033c\033[?u\033[?1049l\033[?u' \
    '\033[?0u\033[?0u'

$TMUX respawn-pane -k "printf '\033[>5u'; exec sleep 100" || exit 1
wait_for_output 'Kitty 5' $TMUX display -p '#{pane_key_mode}' || exit 1

exit 0
