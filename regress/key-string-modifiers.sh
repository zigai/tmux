#!/bin/sh

# Key names accept s- for Shift as well as S-, and Super- and Hyper- for the
# Super and Hyper modifiers.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
trap 'stop_server $TMUX' 0
trap 'exit 1' 1 2 15

root_keys()
{
	$TMUX list-keys -T root -F '#{key_string}' | sort | tr '\n' ' '
}

$TMUX new -d || exit 1
$TMUX unbind -a -T root || exit 1
$TMUX bind -n s-Up display a \; bind -n Super-a display b \; \
    bind -n C-Hyper-Super-b display c || exit 1
wait_for_output 'C-Super-Hyper-b S-Up Super-a ' root_keys || exit 1

exit 0
