#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
export PATH TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(mktemp -d) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR
TMUX1="$TEST_TMUX -Ltoken-inner-$$ -f/dev/null"
TMUX2="$TEST_TMUX -Ltoken-outer-$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	$TMUX1 kill-server 2>/dev/null
	$TMUX2 kill-server 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup 0 1 15

$TMUX1 new-session -d -s inner -x 80 -y 24 'cat' || fail 'inner session failed'
$TMUX2 new-session -d -s one -x 80 -y 24 "$TMUX1 attach -t inner" ||
    fail 'first client failed'
$TMUX2 new-session -d -s two -x 80 -y 24 "$TMUX1 attach -t inner" ||
    fail 'second client failed'

i=0
while [ "$i" -lt 50 ]; do
	[ "$($TMUX1 list-clients -F '#{client_routing_token}' | wc -l)" -eq 2 ] && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'clients did not attach'

tokens=$($TMUX1 list-clients -F '#{client_routing_token}')
[ "$(printf '%s\n' "$tokens" | sort -u | wc -l)" -eq 2 ] ||
    fail 'routing tokens are not distinct'
if printf '%s\n' "$tokens" | grep -Ev '^[0-9a-f]{12}$' >/dev/null; then
    fail 'routing token format is invalid'
fi

first=$($TMUX2 display-message -p -t one: '#{pane_tty}')
token=$($TMUX1 display-message -p -c "$first" -F '#{client_routing_token}')
printf '%s\n' "$tokens" | grep -Fx "$token" >/dev/null ||
    fail 'client-specific token lookup failed'

outer=$($TMUX2 display-message -p -t one: '#{pane_id}')
$TMUX2 pipe-pane -t "$outer" "cat > '$DIR/title'" ||
    fail 'title capture failed'
$TMUX1 set -g set-titles-string '⟦anno:#{client_routing_token}⟧' ||
    fail 'title format failed'
$TMUX1 set -g set-titles on || fail 'title setting failed'
$TMUX1 refresh-client -R -t "$first" || fail 'client refresh failed'
sleep 0.2
grep -aF "⟦anno:$token⟧" "$DIR/title" >/dev/null ||
    fail 'title did not contain routing token'

exit 0
