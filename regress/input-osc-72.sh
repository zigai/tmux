#!/bin/sh

# Kitty drag and drop (OSC 72) from panes: query replies, refused requests,
# chunks and registering the outer terminal for drops and drags, with and
# without the dnd terminal feature.

. ./dnd-common.inc

start

# Without the feature, nothing is answered or sent on.
attach outer $DIR/out
pane $RIGHT 't=q:i=5'
pane $RIGHT 't=r:x=1:i=7'
pane $RIGHT 't=a:i=7;text/plain'
pane_sync $RIGHT $DIR/right
expect $DIR/right '' "answered without dnd"
expect $DIR/out 't=q' "startup query"
expect_none $DIR/out "registered without dnd"

# A client with the feature is registered for the pane on attach.
$INNER set -as terminal-features ',screen*:dnd' || exit 1
attach outer $DIR/out
expect $DIR/out "$(printf '%s\n' 't=q' 't=a;text/plain')" \
    "registered on attach"

pane $RIGHT 't=q:i=5'
expect $DIR/right 't=q:i=5' "query reply"
pane $RIGHT 't=q'
expect $DIR/right 't=q' "query reply without i"

# The MIME types of all panes are registered, including chunked lists. A query
# between chunks is answered and is not a chunk.
pane $LEFT 't=a;text/uri-list text/plain'
expect $DIR/out 't=a;text/uri-list text/plain' "two panes"
pane $LEFT 't=A'
expect $DIR/out 't=a;text/plain' "one pane"
pane $RIGHT 't=a:i=7:m=1;text/html '
pane $RIGHT 't=q:i=5'
pane $RIGHT 'm=0;image/png'
expect $DIR/right 't=q:i=5' "query between chunks"
expect $DIR/out 't=a;text/html image/png' "chunked list"

# A reset ends a chunked list but keeps the registration.
pane $RIGHT 't=a:i=7:m=1;text/x-lost'
printf '\033c' >"$(pane_tty $RIGHT)"
pane $RIGHT 't=q:i=5'
expect $DIR/right 't=q:i=5' "query after reset"
expect_none $DIR/out "registration changed by reset"

# Long lists go to the terminal in chunks of 4096 bytes.
long1=a/$(printf '%03000d' 0)
long2=b/$(printf '%03000d' 0)
list="$long1 $long2 text/html image/png"
pane $LEFT "t=a:m=1;$long1 "
pane $LEFT "m=0;$long2"
expect $DIR/out "$(printf 't=a:m=1;%s\nm=0;%s' \
    "$(printf %s "$list" | cut -c1-4096)" \
    "$(printf %s "$list" | cut -c4097-)")" "long list"
pane $LEFT 't=A'
expect $DIR/out 't=a;text/html image/png' "long list removed"

# Payloads with C1 controls are not sent to the terminal.
pane $LEFT "$(printf 't=a;x/y\302\23552;c;aGk=')"
pane_sync $LEFT $DIR/left
expect_none $DIR/out "control characters sent"

# Requests with no drop or drag are refused by tmux.
pane $RIGHT 't=r:x=1:i=7'
expect $DIR/right 't=R:x=1:i=7;EPERM' "request before drop"
pane $LEFT 't=o:o=1;text/plain'
expect $DIR/left 't=E;EPERM' "offer before gesture"

# Offering drags registers the terminal as a drag source.
pane $LEFT 't=o:x=1'
expect $DIR/out 't=o:x=1' "offer"
pane $LEFT 't=P:x=0'
expect $DIR/left 't=E;EINVAL' "drag image with no drag"

# Pane state goes when the pane is respawned, exits or is killed.
$INNER respawn-pane -k -t $LEFT "$(logger left)" || exit 1
expect $DIR/out 't=o:x=2' "respawned pane"
$INNER set -g remain-on-exit on || exit 1
$INNER respawn-pane -k -t $RIGHT 'cat >/dev/null' || exit 1
expect $DIR/out 't=A' "respawned accepting pane"
pane $RIGHT 't=a;text/plain'
expect $DIR/out 't=a;text/plain' "registered again"
$INNER send-keys -t $RIGHT C-d || exit 1
expect $DIR/out 't=A' "exited pane"
wait_for_output 1 $INNER display -p -t $RIGHT '#{pane_dead}' ||
    fail "pane did not exit"
$INNER kill-pane -t $RIGHT || exit 1
expect_none $DIR/out "killed dead pane"

# A list over 1 MB loses the chunks past that, and tmux stays responsive.
# Each chunk has 444 types, so 262 chunks fit.
start=$(date +%s)
awk 'BEGIN {
	for (c = 0; c < 270; c++) {
		s = ""
		for (i = 0; i < 444; i++)
			s = s sprintf("a/%06d ", c * 444 + i)
		printf "\033]72;%sm=1;%s\033\\", c == 0 ? "t=a:" : "", s
	}
	printf "\033]72;m=0;x/end\033\\"
}' >"$(pane_tty $LEFT)"
pane_sync $LEFT $DIR/left
$INNER neww -d -t inner:1 'sleep 100' || exit 1
$INNER selectw -t inner:1 || exit 1
$INNER selectw -t inner:0 || exit 1
[ $(($(date +%s) - start)) -le 5 ] || fail "long list too slow"
long_end()
{
	[ "$(osc72 $DIR/out | tail -1 | sed 's/.* //')" = "x/end" ]
}
wait_until long_end || fail "long list not sent"
[ "$(osc72 $DIR/out | sed 's/^[^;]*;//' | tr -d '\n' | tr ' ' '\n' |
    tail -2)" = "$(printf 'a/116327\nx/end')" ] ||
    fail "long list not cut at 1 MB"

exit 0
