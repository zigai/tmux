#!/bin/sh

# Test the OSC 30001 and OSC 30101 colour stack: pop restores the colours
# saved by push, the stack holds ten entries and RIS empties it.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"

stop_server $TMUX || exit 1

TMP=$(mktemp)
trap 'rm -f "$TMP"; stop_server $TMUX || exit 1' 0 1 15

$TMUX new-session -d -x 80 -y 24 -s stack \; \
    set-window-option -g remain-on-exit on || exit 1

exit_status=0

query()
{
	name=$1
	seq=$2
	expected=$3

	$TMUX respawn-pane -k -t stack:0.0 \
	    "stty raw -echo min 0 time 10; printf '$seq'; dd bs=1 count=1024 2>/dev/null | cat -v >$TMP"
	wait_for_output 1 $TMUX display -p -t stack:0.0 '#{pane_dead}' || {
		echo "FAIL: $name (reply reader did not exit)"
		exit_status=1
		return
	}
	actual=$(cat "$TMP")
	if [ "$actual" != "$expected" ]; then
		echo "FAIL: $name: expected '$expected', got '$actual'"
		exit_status=1
	fi
}

query push-pop \
    '\033]21;background=#102030;3=red;selection_foreground=blue\033\\\033]30001\033\\\033]21;background=#405060;3=green;selection_foreground\033\\\033]21;background=?;3=?;selection_foreground=?\033\\\033]30101\033\\\033]21;background=?;3=?;selection_foreground=?\033\\\033]30101\033\\\033]21;background=?\033\\' \
    '^[]21;background=rgb:40/50/60;3=rgb:00/ff/00;selection_foreground=^[\^[]21;background=rgb:10/20/30;3=rgb:ff/00/00;selection_foreground=rgb:00/00/ff^[\^[]21;background=rgb:10/20/30^[\'

# Eleven pushes drop the oldest entry, so the last pop finds nothing.
seq=
i=0
while [ $i -le 10 ]; do
	seq="$seq\\033]21;background=#0000$(printf %02x $i)\\033\\\\\\033]30001\\033\\\\"
	i=$((i + 1))
done
i=0
while [ $i -le 10 ]; do
	seq="$seq\\033]30101\\033\\\\"
	i=$((i + 1))
done
query overflow "$seq\\033]21;background=?\\033\\\\" \
    '^[]21;background=rgb:00/00/01^[\'

query ris '\033]21;3=red\033\\\033]30001\033\\\033c\033]30101\033\\\033]21;3=?\033\\' \
    '^[]21;3=^[\'

exit $exit_status
