#!/bin/sh

# Test OSC 21 colour control from panes: set, query and reset colours, and
# check that each pane has its own colours.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"

stop_server $TMUX || exit 1

TMP=$(mktemp)
trap 'rm -f "$TMP"; stop_server $TMUX || exit 1' 0 1 15

$TMUX new-session -d -x 80 -y 24 -s osc21 \; \
    set-window-option -g remain-on-exit on \; \
    set-window-option -g pane-colours[3] '#334455' \; \
    split-window -d -t osc21:0 || exit 1

exit_status=0

query()
{
	name=$1
	pane=$2
	seq=$3
	expected=$4

	$TMUX respawn-pane -k -t osc21:0.$pane \
	    "stty raw -echo min 0 time 10; printf '$seq'; dd bs=1 count=1024 2>/dev/null | cat -v >$TMP"
	wait_for_output 1 $TMUX display -p -t osc21:0.$pane '#{pane_dead}' || {
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

query defaults 0 \
    '\033]21;foreground=?;background=?;cursor=?;cursor_text=?;3=?;7=?\033\\' \
    '^[]21;foreground=;background=;cursor=;cursor_text=;3=rgb:33/44/55;7=^[\'
query unknown 0 '\033]21;bogus=?;256=1\033\\' \
    '^[]21;unknown=Ym9ndXM;unknown=MjU2^[\'

query set 0 \
    '\033]21;foreground=rgb:ee/ee/ee;background=rgb:10/20/60;cursor=#ff8800;5=rgbi:1/0/0;selection_background=red;transparent_background_color2=#102030@0.5;foreground=?;background=?;cursor=?;5=?;selection_background=?;transparent_background_color2=?\007' \
    '^[]21;foreground=rgb:ee/ee/ee;background=rgb:10/20/60;cursor=rgb:ff/88/00;5=rgb:ff/00/00;selection_background=rgb:ff/00/00;transparent_background_color2=rgb:10/20/30@0.5000^G'
query osc-11-12-4 0 '\033]11;?\033\\\033]12;?\033\\\033]4;5;?\033\\' \
    '^[]11;rgb:1010/2020/6060^[\^[]12;rgb:ffff/8888/0000^[\^[]4;5;rgb:ffff/0000/0000^[\'
query other-pane 1 \
    '\033]21;background=?;cursor=?;5=?;selection_background=?\033\\' \
    '^[]21;background=;cursor=;5=;selection_background=^[\'

query reset 0 \
    '\033]21;foreground=;background;cursor=;5;selection_background=;transparent_background_color2\033\\\033]21;foreground=?;background=?;cursor=?;5=?;selection_background=?;transparent_background_color2=?\033\\' \
    '^[]21;foreground=;background=;cursor=;5=;selection_background=;transparent_background_color2=^[\'

exit $exit_status
