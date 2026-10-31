#!/bin/sh

# Kitty clipboard paste events (mode 5522): the mode of the active pane is set
# in the terminal when the clipboard is requested from it, and paste events
# from the terminal go to the active pane if it has the mode.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"
TMP=$(mktemp -d) || exit 1
trap 'stop_servers; rm -rf $TMP' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "$*" >&2
	exit 1
}

# Print the last mode 5522 change sent to the terminal.
mode()
{
	grep -ao '?5522[hl]' $TMP/terminal | tail -1
}

: >$TMP/a
: >$TMP/b
$TMUX new -d -s kc -x40 -y10 "stty raw -echo; printf '\\033[?5522h'; \
    exec cat -v >$TMP/a" || exit 1
$TMUX set -g status off || exit 1
$TMUX set -s set-clipboard on || exit 1
$TMUX set -s get-clipboard request || exit 1
$TMUX set -s escape-time 10 || exit 1
$TMUX set -as terminal-features 'tmux*:kittyclipboard' || exit 1
$TMUX2 new -d -s outer -x40 -y10 \
    "while [ ! -e $TMP/go ]; do sleep 0.01; done; exec $TMUX attach -t kc" ||
    exit 1
$TMUX2 set -g status off || exit 1
# Colours are set so the client's colour queries are answered.
$TMUX2 set -g window-style fg=white,bg=black || exit 1
$TMUX2 pipe-pane -t outer: -O "cat >>$TMP/terminal" || exit 1
touch $TMP/go
wait_for_clients 1 $TMUX || fail "client did not attach"

wait_for_output '?5522h' mode || fail "mode not set"
$TMUX set -s get-clipboard buffer || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522l' mode || fail "mode set with get-clipboard buffer"
$TMUX set -s get-clipboard request || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522h' mode || fail "mode not set again"
$TMUX set -s set-clipboard external || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522l' mode || fail "mode set with set-clipboard external"
$TMUX set -s set-clipboard on || exit 1
$TMUX refresh-client || exit 1
wait_for_output '?5522h' mode || fail "mode not set with set-clipboard on"

$TMUX split-window -t kc: "stty raw -echo; exec cat -v >$TMP/b" || exit 1
wait_for_output '?5522l' mode || fail "mode set for other pane"

event=$(printf '\033]5522;type=read:status=OK:pw=b3Rw\033\\'; \
    printf '\033]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;'; \
    printf 'dGV4dC9wbGFpbgo=\033\\'; \
    printf '\033]5522;type=read:status=DONE:pw=b3Rw\033\\')
$TMUX2 send-keys -t outer: -l "$event" || exit 1
$TMUX2 send-keys -t outer: -l x || exit 1
wait_for_output x cat $TMP/b || fail "paste event sent to pane without mode"
$TMUX select-pane -t kc:.0 || exit 1
wait_for_output '?5522h' mode || fail "mode not set after select-pane"

# A prompt in the pane takes pastes itself.
client=$($TMUX list-clients -F '#{client_name}')
$TMUX bind -n F12 command-prompt -P || exit 1
$TMUX2 send-keys -t outer: F12 || exit 1
wait_for_output '?5522l' mode || fail "mode set at the prompt"
$TMUX2 send-keys -t outer: Escape || exit 1
wait_for_output '?5522h' mode || fail "mode not set after the prompt"
$TMUX2 send-keys -t outer: -l "$event" || exit 1
wait_for_output '^[]5522;type=read:status=OK:pw=b3Rw^[\'\
'^[]5522;type=read:status=DATA:mime=Lg==:pw=b3Rw;dGV4dC9wbGFpbgo=^[\'\
'^[]5522;type=read:status=DONE:pw=b3Rw^[\' cat $TMP/a ||
    fail "paste event not sent to active pane"
[ "$(cat $TMP/b)" = x ] || fail "paste event sent to pane without mode"

# With the mode set, Escape is not held as if it started a reply, so a key
# after it is not taken as Meta.
$TMUX bind -n M-x set -g @meta x || exit 1
$TMUX2 send-keys -t outer: Escape || exit 1
sleep 0.2
$TMUX2 send-keys -t outer: -l x || exit 1
wait_for_text '^[x' cat $TMP/a || fail "Escape held with the mode set"

# The read after a paste event goes to the client which sent the event, not
# the most recently used.
: >$TMP/terminal2
$TMUX2 new-window -d -t outer:1 "$TMUX attach -t kc" || exit 1
$TMUX2 pipe-pane -t outer:1 -O "cat >>$TMP/terminal2" || exit 1
wait_for_clients 2 $TMUX || fail "second client did not attach"
$TMUX respawn-pane -k -t kc:.0 "stty raw -echo; printf '\\033[?5522h'; \
    head -c ${#event} >/dev/null; \
    printf '\\033]5522;type=read:id=p1:pw=b3Rw:name=UGFzdGUgZXZlbnQ=;'; \
    printf 'dGV4dC9wbGFpbg==\\033\\\\'; exec cat >/dev/null" || exit 1
wait_for_text "$(printf '\033[?5522h')" cat $TMP/terminal2 ||
    fail "mode not set on second client"
$TMUX2 send-keys -t outer:0 -l "$event" || exit 1
wait_for_text "$(printf '\033]5522;type=read:id=tmux-')" cat $TMP/terminal ||
    fail "read after paste event not sent to its client"
grep -aq '5522;type=read:' $TMP/terminal2 &&
    fail "read after paste event sent to another client"

$TMUX detach-client -t "$client" || exit 1
wait_for_clients 1 $TMUX || exit 1
wait_for_output '?5522l' mode || fail "mode left set after detach"

exit 0
