#!/bin/sh

# Keys from a terminal using the Kitty keyboard protocol reach each pane
# encoded for the flags it asked for, key bindings still work, and a release
# goes to the pane which had the press. An outer tmux stands in for the
# terminal and is given the bytes Ghostty sends for the flags requested.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestI$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestO$$ -f/dev/null"
DIR=$(mktemp -d)
A=$DIR/a
B=$DIR/b
trap 'rm -rf "$DIR"; stop_servers' 0
trap 'exit 1' 1 2 15

hex()
{
	od -An -v -t x1 "$1" | tr -d ' \n'
}

probe()
{
	: >"$2"
	echo "stty raw -echo; printf '\\033[>$1u'; exec cat >>'$2'"
}

send()
{
	$TMUX2 send-keys -H $(printf "$1" | od -An -v -t x1) || exit 1
}

check()
{
	wait_for_output "$(printf "$2" | od -An -v -t x1 | tr -d ' \n')" \
	    hex "$1" || exit 1
	: >"$1"
}

use_flags()
{
	$TMUX respawn-pane -k -t:.0 "$(probe "$1" "$A")" || exit 1
	wait_for_output "Kitty $2" $TMUX2 display -p '#{pane_key_mode}' ||
	    exit 1
}

$TMUX new -d -x80 -y24 'exec sleep 100' || exit 1
$TMUX set -g extended-keys on || exit 1
$TMUX2 set -g extended-keys on \; new -d -x80 -y24 "$TMUX attach" || exit 1

# With every flag, each event is passed on as the terminal sent it.
use_flags 31 31
for seq in '\033[97;;97u' '\033[97;1:2;97u' '\033[97;1:3u' \
    '\033[97:65;2;65u' '\033[1;1:1A' '\033[1;1:3A' '\033[57441;2u' \
    '\033[57441;1:3u' '\033[13u' '\033[13;1:3u' '\033[97;133u'; do
	send "$seq"
	check "$A" "$seq"
done

# A bracketed paste reaches the pane as it was pasted.
: >"$A"
$TMUX respawn-pane -k -t:.0 \
    "stty raw -echo; printf '\\033[?2004h\\033[>1u'; exec cat >>'$A'" ||
    exit 1
wait_for_output 'Kitty 1' $TMUX display -p '#{pane_key_mode}' || exit 1
wait_for_output 'Kitty 1' $TMUX2 display -p '#{pane_key_mode}' || exit 1
send '\033[200~hi\033[201~'
check "$A" '\033[200~hi\033[201~'

# With fewer flags the terminal is still asked for everything, and each event
# is encoded again as the pane asked.
use_flags 3 31
send '\033[97;;97u\033[97;1:2;97u\033[97;1:3u\033[13u\033[13;1:3u'
send '\033[57441;2u\033[97:65;2;65u\033[97:65;2:3u\033[57441;1:3u'
check "$A" 'aa\033[97;1:3u\rA\033[97;2:3u'

use_flags 1 1
send '\033[97;5u\033[1;5A\033[27u'
check "$A" '\033[97;5u\033[1;5A\033[27u'

$TMUX respawn-pane -k -t:.0 "stty raw -echo; exec cat >>'$A'" || exit 1
wait_for_output 'Kitty 1' $TMUX2 display -p '#{pane_key_mode}' || exit 1
send '\033[97;5u\033[27u\033[57413;3u'
check "$A" '\001\033\033+'

# The prefix and bindings work when every key is reported: C-b c and C-b "
# with Shift held. The pane sees neither these keys nor their releases.
use_flags 31 31
send '\033[57442;5u\033[98;5u\033[98;5:3u\033[57442;1:3u'
send '\033[57441;2u\033[39:34;2;34u\033[39:34;2:3u\033[57441;1:3u'
check "$A" '\033[57442;5u\033[57442;1:3u'
wait_for_output 2 $TMUX display -p '#{window_panes}' || exit 1
$TMUX kill-pane -t:.1 || exit 1

# A release goes to the pane which had the press after the active pane changes.
$TMUX split-window -d "$(probe 3 "$B")" || exit 1
wait_for_output 'Kitty 3' $TMUX display -p -t:.1 '#{pane_key_mode}' || exit 1
send '\033[120;;120u'
check "$A" '\033[120;;120u'
$TMUX select-pane -t:.1 || exit 1
send '\033[120;1:3u\033[121;;121u\033[121;1:3u'
check "$A" '\033[120;1:3u'
check "$B" 'y\033[121;1:3u'

# A release is not given to the pane once it is in a mode, where it would act
# as a key press.
$TMUX bind -n F12 set -g @released 1 || exit 1
send '\033[113;;113u'
check "$B" 'q'
$TMUX choose-tree -t:.1 || exit 1
send '\033[113;1:3u\033[24~'
wait_for_output 1 $TMUX show -gv @released || exit 1
[ "$($TMUX display -p -t:.1 '#{pane_mode}')" = tree-mode ] || exit 1

exit 0
