#!/bin/sh

# Kitty OSC 99 replies from the terminal go back to the pane that sent the
# notification once, even with several clients. Activation focuses the pane
# and closes the notification in other clients. Queries from panes are
# answered with what the terminal said it supports.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestB$$ -f/dev/null"
trap 'stop_server $OUTER; stop_server $TMUX; rm -rf "$DIR"' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "FAIL: $*"
	exit 1
}

# Attach a client with a terminal type and capture what it writes.
attach()
{
	$OUTER new-session -d -s "$1" -x40 -y10 \
	    "while [ ! -e '$DIR/go-$1' ]; do sleep 0.01; done;
	    exec env TERM=$2 $TMUX attach-session -t $3" || exit 1
	$OUTER pipe-pane -t "$1" -O "cat >'$DIR/$1'" || exit 1
	touch "$DIR/go-$1"
}

# Run a pane which prints a sequence and keeps what it reads in a file.
listen()
{
	$TMUX respawn-pane -k -t "$1" \
	    "stty raw -echo; printf '$2'; exec cat -v >'$DIR/$3'" || exit 1
}

# Send a reply from a client's terminal.
reply()
{
	$OUTER send-keys -t "$1" -l "$(printf "$2")" || exit 1
}

has()
{
	grep -Fq "$(printf "$2")" "$DIR/$1"
}

read_file()
{
	cat "$DIR/$1"
}

# Check what a pane has read, waiting for it to arrive and then for anything
# more that should not.
check()
{
	wait_for_output "$2" read_file "$1" || fail "$3"
	sleep 0.3
	[ "$(read_file "$1")" = "$2" ] || fail "$3: more than once"
}

window()
{
	$TMUX display -p -t one: '#{window_index}'
}

# The identifier the terminal sees for a notification from a pane.
oid()
{
	printf 'tmux-%s-%s-%s' "$PID" "${1#%}" "$2"
}

A=$($TMUX new-session -d -P -F '#{pane_id}' -s one -x40 -y10 'sleep 1000') ||
    exit 1
B=$($TMUX new-window -d -P -F '#{pane_id}' -t one: 'sleep 1000') || exit 1
$TMUX set -g status off || exit 1
$TMUX set -as terminal-features 'xterm-256color:osc99' || exit 1
PID=$($TMUX display -p '#{pid}')

attach c1 xterm-256color one
attach c2 xterm-256color one
wait_for_clients 2 $TMUX || exit 1
wait_until has c1 '\033]99;i=tmux:p=?;\033\\' || fail "no query sent"

# A query has no answer until the terminal has said what it supports.
listen "$A" '\033]99;i=q:p=?;\033\\' q1
sleep 0.5
[ -z "$(read_file q1)" ] || fail "query answered without support"
reply c1 '\033]99;i=tmux:p=?;a=focus,report:c=1:p=title,body\033\\'
sleep 0.5
listen "$A" '\033]99;i=q:p=?;\033\\' q2
check q2 '^[]99;i=q:p=?;a=focus,report:c=1:p=title,body^[\' "query"
listen "$A" '\033]99;p=?;\007' q3
check q3 '^[]99;i=0:p=?;a=focus,report:c=1:p=title,body^G' "query with BEL"

# Activation is reported, focuses the pane and closes other copies.
listen "$B" '\033]99;i=r:a=report;Click\033\\' r
wait_until has c2 "\033]99;i=$(oid $B ir):a=focus,report" ||
    fail "notification not sent"
reply c1 "\033]99;i=$(oid $B ir);2\033\\\\"
reply c2 "\033]99;i=$(oid $B ir);1\033\\\\"
check r '^[]99;i=r;2^[\' "activation"
[ "$(window)" = 1 ] || fail "activation did not focus the pane"
has c2 "\033]99;i=$(oid $B ir):p=close;\033\\\\" ||
    fail "other client not closed"

$TMUX select-window -t one:0 || exit 1
listen "$B" '\033]99;i=nf:a=-focus,report;Stay\033\\' nf
wait_until has c1 "i=$(oid $B inf):a=-focus,report" ||
    fail "notification not sent"
reply c1 "\033]99;i=$(oid $B inf);\033\\\\"
check nf '^[]99;i=nf;^[\' "activation without focus"
[ "$(window)" = 0 ] || fail "activation without focus changed window"

listen "$B" '\033]99;i=f;Go\033\\' f
wait_until has c1 "i=$(oid $B if):a=focus,report" ||
    fail "notification not sent"
reply c1 "\033]99;i=$(oid $B if);\033\\\\"
wait_for_output 1 window || fail "activation did not focus the pane"
[ -z "$(read_file f)" ] || fail "activation reported without report"

# Close events reach the pane once and close the other copies.
listen "$A" '\033]99;i=k:c=1:a=-focus;Close me\033\\' k
wait_until has c1 "i=$(oid $A ik):a=-focus,report:c=1" ||
    fail "notification not sent"
reply c2 "\033]99;i=$(oid $A ik):p=close;\033\\\\"
reply c1 "\033]99;i=$(oid $A ik):p=close;\033\\\\"
check k '^[]99;i=k:p=close;^[\' "close"
has c1 "\033]99;i=$(oid $A ik):p=close;\033\\\\" ||
    fail "other client not closed"

listen "$A" '\033]99;i=u:c=1:a=-focus;U\033\\' u
wait_until has c1 "i=$(oid $A iu):a=-focus,report:c=1" ||
    fail "notification not sent"
reply c1 "\033]99;i=$(oid $A iu):p=close;untracked\033\\\\"
reply c2 "\033]99;i=$(oid $A iu):p=close;untracked\033\\\\"
check u '^[]99;i=u:p=close;untracked^[\' "untracked close"

# Alive queries are answered once with only the pane's own identifiers.
listen "$A" '\033]99;i=live:p=alive;\033\\' live
wait_until has c2 "\033]99;i=$(oid $A ilive):p=alive;\033\\\\" ||
    fail "alive query not sent"
ids="$(oid $A ik2),$(oid $B ix),$(oid $A n9),tmux-1-0-iy,$(oid $A iu)"
reply c1 "\033]99;i=$(oid $A ilive):p=alive;$ids\033\\\\"
reply c2 "\033]99;i=$(oid $A ilive):p=alive;$ids\033\\\\"
check live '^[]99;i=live:p=alive;k2,u^[\' "alive"

# Replies for other servers or unknown notifications are ignored.
listen "$A" '\033]99;i=z:a=report,-focus;Z\033\\' z
wait_until has c1 "i=$(oid $A iz):a=-focus,report" ||
    fail "notification not sent"
reply c1 "\033]99;i=tmux-1-${A#%}-iz;1\033\\\\"
reply c1 "\033]99;i=$(oid $A inothing);1\033\\\\"
reply c1 "\033]99;i=$(oid $A iz);3\033\\\\"
check z '^[]99;i=z;3^[\' "reply after ignored replies"

exit 0
