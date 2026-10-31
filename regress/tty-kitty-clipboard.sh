#!/bin/sh

# Kitty clipboard protocol (OSC 5522) between a pane and the terminal. The
# client runs in a pane of a second server, which records what it is sent and
# answers the requests like any terminal with the protocol.

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

# Print $1 from the pane and wait for the replies it reads, shown by cat -v,
# to be $2.
check()
{
	: >$TMP/out
	$TMUX respawn-pane -k -t kc: "stty raw -echo; printf '$1'; \
	    exec cat -v >$TMP/out" || exit 1
	wait_for_output "$2" cat $TMP/out || fail "$3"
}

# Wait for the terminal in window $1 to have been sent $2.
sent()
{
	wait_for_text "$(printf "$2")" cat $TMP/terminal$1 || fail "$3"
}

# Attach a client in a new window $1 of the terminal server, making $2 clients.
attach()
{
	: >$TMP/terminal$1
	$TMUX2 new-window -d -t outer:$1 "$TMUX attach -t kc" || exit 1
	$TMUX2 pipe-pane -t outer:$1 -O "cat >>$TMP/terminal$1" || exit 1
	wait_for_clients $2 $TMUX || fail "client $1 did not attach"
}

PLAIN=dGV4dC9wbGFpbg==

$TMUX new -d -s kc -x40 -y10 "sleep 100" || exit 1
$TMUX set -g status off || exit 1
$TMUX set -s set-clipboard on || exit 1
$TMUX set -as terminal-features 'tmux*:kittyclipboard' || exit 1
$TMUX2 new -d -s outer -x40 -y10 "sleep 100" || exit 1
$TMUX2 set -g status off || exit 1
attach 1 1

# A write is sent as one transaction under an id made by tmux. The reply comes
# from the terminal, which refuses it here, and gets the pane's id.
check "\\033]5522;type=write:id=w1\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGVsbG8=\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=EPERM:id=w1^[\' "write refused by terminal"
sent 1 "\\033]5522;type=write:id=tmux-1\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGVsbG8=\\033\\\\\
\\033]5522;type=wdata\\033\\\\" "write transaction"
[ "$($TMUX show-buffer)" = hello ] || fail "write buffer"

# Chunks are joined and split again at 4096 bytes, and the location, password
# and name go to the terminal.
$TMUX2 set -s set-clipboard on || exit 1
data=YWFh
for i in 1 2 3 4 5 6 7 8 9 10; do
	data=$data$data
done
check "\\033]5522;type=write:id=w2:loc=primary:pw=cHc=:name=YXBw\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGVsb\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;G8=\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;IHdvcmxk\\033\\\\\
\\033]5522;type=wdata:mime=dGV4dC9odG1s;$data$data\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=DONE:id=w2^[\' "write done by terminal"
sent 1 "\\033]5522;type=write:loc=primary:id=tmux-2:pw=cHc=:name=YXBw\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGVsbG8gd29ybGQ=\\033\\\\" "joined chunks"
[ "$(grep -ao 'type=wdata:mime=dGV4dC9odG1s;' $TMP/terminal1 | wc -l)" -eq 2 ] ||
    fail "split chunks"
[ "$($TMUX2 show-buffer)" = "hello world" ] || fail "terminal buffer"

# Reads go to the terminal with request and the reply is routed back; both
# also makes a buffer.
$TMUX set-buffer 'inner text' || exit 1
$TMUX2 set-buffer 'outer text' || exit 1
$TMUX set -s get-clipboard request || exit 1
: >$TMP/terminal1
check "\\033]5522;type=read:id=r1;$PLAIN\\007" \
    "^[]5522;type=read:status=OK:id=r1^G\
^[]5522;type=read:status=DATA:id=r1:mime=$PLAIN;b3V0ZXIgdGV4dA==^G\
^[]5522;type=read:status=DONE:id=r1^G" "read from terminal"
sent 1 "\\033]5522;type=read:id=tmux-3;$PLAIN\\033\\\\" "read request"
[ "$($TMUX show-buffer)" = "inner text" ] || fail "buffer from request"
$TMUX set -s get-clipboard both || exit 1
check "\\033]5522;type=read;$PLAIN\\033\\\\" \
    "^[]5522;type=read:status=OK^[\\\
^[]5522;type=read:status=DATA:mime=$PLAIN;b3V0ZXIgdGV4dA==^[\\\
^[]5522;type=read:status=DONE^[\\" "read with both"
[ "$($TMUX show-buffer)" = "outer text" ] || fail "buffer from both"

# Without the feature the text is set with OSC 52 and the pane is answered by
# tmux, and mode 5522 is never set or reset in the terminal, even when a theme
# change makes tmux send every mode again. The second write is sent after any
# mode change before it.
$TMUX2 kill-window -t outer:1 || exit 1
wait_for_clients 0 $TMUX || exit 1
$TMUX set -su terminal-features || exit 1
attach 2 1
wait_for_text clipboard $TMUX list-clients -F '#{client_termfeatures}' ||
    fail "no clipboard feature"
check "\\033[?5522h\\033]5522;type=write:id=w4\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;b25l\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=DONE:id=w4^[\' "write without feature"
sent 2 "\\033]52;;b25l\\007" "OSC 52 without feature"
$TMUX set -s theme dark || exit 1
check "\\033]5522;type=write:id=w5\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;dHdv\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=DONE:id=w5^[\' "second write without feature"
sent 2 "\\033]52;;dHdv\\007" "second OSC 52 without feature"
grep -Eaq '5522;|\?5522[hl]' $TMP/terminal2 && fail "OSC 5522 without feature"

exit 0
