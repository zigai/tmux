#!/bin/sh

# Kitty clipboard protocol (OSC 5522) requests waiting for the terminal: only
# the client a request was sent to may answer it, a new request from a pane
# fails its waiting read but not a write, a reply split over two reads is not
# taken as keys, and a request is failed when its client goes. The client
# which answers runs in a pane of a second server, which itself asks a third
# server; stopping the third keeps requests waiting.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"
TMUX3="$TEST_TMUX -LtestC$$ -f/dev/null"
TMUX4="$TEST_TMUX -LtestD$$ -f/dev/null"
TMP=$(mktemp -d) || exit 1
trap 'kill -CONT $stopped 2>/dev/null; stop_servers; stop_server $TMUX3;
    stop_server $TMUX4; rm -rf $TMP' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "$*" >&2
	exit 1
}

# Print $1 from the pane and wait for the replies it reads, shown by cat -v,
# to be $2.
check()
{
	: >$TMP/out
	$TMUX respawn-pane -k -t kc: "stty raw -echo; printf '$1'; \
	    exec cat -v >$TMP/out" || exit 1
	wait_for_output "$2" cat $TMP/out || fail "$3"
}

# Start server $1 with a client attached to session $2 of server $3, which is
# sent what goes to $TMP/$4. The colours are set so the client's colour
# queries are answered and it does not wait for them.
terminal()
{
	$1 new -d -s t -x40 -y10 \
	    "while [ ! -e $TMP/$4.go ]; do sleep 0.01; done; exec $3 attach -t $2" ||
	    exit 1
	$1 set -g status off || exit 1
	$1 set -g window-style fg=white,bg=black || exit 1
	$1 pipe-pane -t t: -O "cat >>$TMP/$4" || exit 1
	touch $TMP/$4.go
}

# The id of the last request of type $1 sent to $TMP/$2.
sent_id()
{
	grep -ao "5522;type=$1[a-z=:]*:id=tmux-[0-9]*" $TMP/$2 | tail -1 |
	    sed 's/.*:id=//'
}

PLAIN=dGV4dC9wbGFpbg==

$TMUX new -d -s kc -x40 -y10 "sleep 100" || exit 1
$TMUX set -g status off || exit 1
$TMUX set -s escape-time 10 || exit 1
$TMUX set -s set-clipboard on || exit 1
$TMUX set -s get-clipboard request || exit 1
$TMUX set -as terminal-features 'tmux*:kittyclipboard' || exit 1

# The second client refuses writes at once. The first, attached last, asks a
# server which is stopped.
terminal "$TMUX4" kc "$TMUX" second
wait_for_clients 1 $TMUX || fail "second client did not attach"
terminal "$TMUX2" kc "$TMUX" first
wait_for_clients 2 $TMUX || fail "first client did not attach"
$TMUX2 set -s set-clipboard on || exit 1
$TMUX2 set -s get-clipboard request || exit 1
$TMUX2 set -as terminal-features 'tmux*:kittyclipboard' || exit 1
terminal "$TMUX3" t "$TMUX2" third
wait_for_clients 1 $TMUX2 || fail "third client did not attach"
$TMUX3 set -s set-clipboard on || exit 1
stopped=$($TMUX3 display -p '#{pid}')
kill -STOP $stopped || exit 1

# The write goes to both clients, but only the first client's reply counts.
check "\\033]5522;type=write:id=w1\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;b25l\\033\\\\\
\\033]5522;type=wdata\\033\\\\" '' "write"
wait_for_text "$(printf '\033]5522;type=wdata\033\\')" cat $TMP/second ||
    fail "write not sent to second client"
wait_for_text "$(printf '\033]5522;type=wdata\033\\')" cat $TMP/first ||
    fail "write not sent to first client"
[ "$(sent_id write second)" = "$(sent_id write first)" ] ||
    fail "write sent with different ids"
kill -CONT $stopped || exit 1
wait_for_output '^[]5522;type=write:status=DONE:id=w1^[\' cat $TMP/out ||
    fail "reply not from first client"

# A new request from the pane fails the one still waiting, so the pane is
# never left busy.
kill -STOP $stopped || exit 1
check "\\033]5522;type=read:id=r1;$PLAIN\\033\\\\\
\\033]5522;type=write:id=w2\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;dHdv\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=read:status=EBUSY:id=r1^[\' "read failed by write"
wait_for_text "$(printf '\033]5522;type=wdata\033\\')" cat $TMP/first ||
    fail "write not sent"
kill -CONT $stopped || exit 1
wait_for_output '^[]5522;type=read:status=EBUSY:id=r1^[\'\
'^[]5522;type=write:status=DONE:id=w2^[\' cat $TMP/out ||
    fail "write after failed read"

# A write already sent to the terminal is not failed by a later one, so each
# gets the terminal's answer.
kill -STOP $stopped || exit 1
: >$TMP/first
check "\\033]5522;type=write:id=w3\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;MQ==\\033\\\\\
\\033]5522;type=wdata\\033\\\\\
\\033]5522;type=write:id=w4\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;Mg==\\033\\\\\
\\033]5522;type=wdata\\033\\\\" '' "two writes"
wait_for_output 2 eval "grep -ao 'type=write:id=tmux-' $TMP/first | wc -l |
    tr -d ' '" || fail "writes not sent"
kill -CONT $stopped || exit 1
wait_for_output '^[]5522;type=write:status=DONE:id=w3^[\'\
'^[]5522;type=write:status=DONE:id=w4^[\' cat $TMP/out ||
    fail "write failed by write"

# A reply split over two reads is still a reply.
kill -STOP $stopped || exit 1
: >$TMP/first
check "\\033]5522;type=read:id=r3;$PLAIN\\033\\\\\
\\033]5522;type=read:id=r4;$PLAIN\\033\\\\" \
    '^[]5522;type=read:status=EBUSY:id=r3^[\' "read failed by read"
wait_for_output 2 eval "grep -ao 'type=read:id=tmux-' $TMP/first | wc -l |
    tr -d ' '" || fail "reads not sent"
id=$(sent_id read first)
$TMUX2 send-keys -t t: -l "$(printf '\033]5522;type=read:status=OK:id=%s' $id)" ||
    exit 1
sleep 0.2
$TMUX2 send-keys -t t: -l "$(printf '\033\\')" || exit 1
wait_for_output '^[]5522;type=read:status=EBUSY:id=r3^[\'\
'^[]5522;type=read:status=OK:id=r4^[\' cat $TMP/out ||
    fail "split reply"

# So is a reply split after its first two bytes, and the request fails when
# its client goes.
kill -STOP $stopped || exit 1
: >$TMP/first
check "\\033]5522;type=read:id=r5;$PLAIN\\033\\\\" '' "read"
wait_for_text 'type=read:id=tmux-' cat $TMP/first || fail "read not sent"
id=$(sent_id read first)
$TMUX2 send-keys -t t: -l "$(printf '\033]')" || exit 1
sleep 0.05
$TMUX2 send-keys -t t: -l "$(printf '5522;type=read:status=OK:id=%s\033\\' $id)" ||
    exit 1
wait_for_output '^[]5522;type=read:status=OK:id=r5^[\' cat $TMP/out ||
    fail "reply split after two bytes"

$TMUX2 respawn-pane -k -t t: "sleep 100" || exit 1
wait_for_output '^[]5522;type=read:status=OK:id=r5^[\'\
'^[]5522;type=read:status=EBUSY:id=r5^[\' cat $TMP/out ||
    fail "request not failed when client went"

exit 0
