#!/bin/sh

# Kitty clipboard protocol (OSC 5522) from a pane with no client attached:
# mode 5522, write transactions and reads answered from paste buffers. Each
# sequence ends with DECRQM so no further reply can follow unnoticed.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMP=$(mktemp -d) || exit 1
trap 'stop_server $TMUX; rm -rf $TMP' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "$*" >&2
	exit 1
}

# Print $1 from the pane followed by DECRQM and wait for the replies it
# reads, shown by cat -v, to be $2.
check()
{
	: >$TMP/out
	$TMUX respawn-pane -k -t kc: "stty raw -echo; \
	    printf '$1\\033[?5522\$p'; exec cat -v >$TMP/out" || exit 1
	wait_for_output "$2^[[?5522;2\$y" cat $TMP/out || fail "$3"
}

PLAIN=dGV4dC9wbGFpbg==

$TMUX new -d -s kc -x20 -y3 "sleep 100" || exit 1

: >$TMP/out
$TMUX respawn-pane -k -t kc: "stty raw -echo; \
    printf '\\033[?5522h\\033[?5522\$p\\033[?5522l\\033[?5522\$p'; \
    exec cat -v >$TMP/out" || exit 1
wait_for_output '^[[?5522;1$y^[[?5522;2$y' cat $TMP/out || fail "DECRQM 5522"

# Writes need set-clipboard on, like OSC 52.
check "\\033]5522;type=write:id=w1\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGVsbG8=\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=EPERM:id=w1^[\' "write with set-clipboard off"
[ -z "$($TMUX list-buffers)" ] || fail "buffer from refused write"

# Text split at any point, or sent as separately padded chunks, makes a
# buffer. With no client to forward it to, the write is done.
$TMUX set -s set-clipboard on || exit 1
check "\\033]5522;type=write:id=w2\\007\
\\033]5522;type=wdata:mime=$PLAIN;aGVsb\\007\
\\033]5522;type=wdata:mime=$PLAIN;G8=\\007\
\\033]5522;type=wdata:mime=$PLAIN;IHdvcmxk\\007\
\\033]5522;type=wdata:mime=dGV4dC9odG1s;PGI+aGk8L2I+\\007\
\\033]5522;type=wdata\\007" \
    '^[]5522;type=write:status=DONE:id=w2^G' "split write"
wait_for_output 'hello world' $TMUX show-buffer || fail "split write buffer"

# An alias of the data counts as text, and id characters outside the allowed
# set are removed.
check "\\033]5522;type=write:id=w*3\\033\\\\\
\\033]5522;type=wdata:mime=dGV4dC9odG1s;b25l\\033\\\\\
\\033]5522;type=walias:mime=dGV4dC9odG1s;$PLAIN\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=DONE:id=w3^[\' "aliased write"
wait_for_output 'one' $TMUX show-buffer || fail "aliased write buffer"

check "\\033]5522;type=write:id=w4\\033\\\\\
\\033]5522;type=wdata:mime=aW1hZ2UvcG5n;cG5n\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=ENOSYS:id=w4^[\' "write without text"

# Invalid base64 fails the write and the rest of it is ignored.
check "\\033]5522;type=write:id=w5\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGk =\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;dHdv\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=EINVAL:id=w5^[\' "invalid write"
check "\\033]5522;type=write:id=w6\\033\\\\\
\\033]5522;type=wdata:mime=$PLAIN;aGk\\033\\\\\
\\033]5522;type=wdata\\033\\\\" \
    '^[]5522;type=write:status=EINVAL:id=w6^[\' "unpadded write"
check "\\033]5522;type=wdata\\033\\\\\
\\033]5522;type=write:bad\\033\\\\" '' "data without a write"
[ "$($TMUX show-buffer)" = one ] || fail "buffer from failed write"

# Reads are answered from the newest buffer by default.
$TMUX set-buffer 'from buffer' || exit 1
check "\\033]5522;type=read:id=r1:loc=primary;LiB0ZXh0L3BsYWlu\\007" \
    "^[]5522;type=read:status=OK:loc=primary:id=r1^G\
^[]5522;type=read:status=DATA:id=r1:mime=Lg==;dGV4dC9wbGFpbgo=^G\
^[]5522;type=read:status=DATA:id=r1:mime=$PLAIN;ZnJvbSBidWZmZXI=^G\
^[]5522;type=read:status=DONE:id=r1^G" "read from buffer"
check "\\033]5522;type=read;not base64\\033\\\\" '' "invalid read"

$TMUX set -s get-clipboard off || exit 1
check "\\033]5522;type=read:id=r2;Lg==\\033\\\\" \
    '^[]5522;type=read:status=EPERM:id=r2^[\' "read with get-clipboard off"

exit 0
