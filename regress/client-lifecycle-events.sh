#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
export PATH TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(mktemp -d) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR
TMUX="$TEST_TMUX -Llifecycle-$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	exit 1
}

wait_for()
{
	i=0
	while [ "$i" -lt 50 ]; do
		grep -aF "$2" "$1" >/dev/null 2>&1 && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

cleanup()
{
	$TMUX kill-server 2>/dev/null
	exec 3>&- 4>&- 5>&-
	rm -rf "$DIR"
}
trap cleanup 0 1 15

$TMUX new-session -d -s events -x 80 -y 24 'cat' || fail 'session failed'
mkfifo "$DIR/one" "$DIR/two" "$DIR/three" || fail 'fifo failed'
$TMUX -C attach -t events <"$DIR/one" >"$DIR/out" 2>&1 &
exec 3>"$DIR/one"
wait_for "$DIR/out" '%session-changed' || fail 'control client did not attach'

printf 'refresh-client -E client-lifecycle\n' >&3
wait_for "$DIR/out" '%end' || fail 'subscription was not accepted'

$TMUX -C attach -t events <"$DIR/two" >"$DIR/two-out" 2>&1 &
exec 4>"$DIR/two"
i=0
while [ "$i" -lt 50 ]; do
	[ "$($TMUX list-clients -F '#{client_name}' | wc -l)" -eq 2 ] && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'second client did not attach'
second=$($TMUX list-clients -F '#{client_name}' | tail -n 1)
pid=$($TMUX display-message -p -c "$second" -F '#{client_pid}')
wait_for "$DIR/out" "%client-created $second $pid" ||
    fail 'client-created notification missing'
$TMUX detach-client -t "$second" || fail 'detach failed'
wait_for "$DIR/out" "%client-closed $second $pid" ||
    fail 'client-closed notification missing'

printf "refresh-client -E '!client-lifecycle'\n" >&3
sleep 0.1
$TMUX -C attach -t events <"$DIR/three" >"$DIR/three-out" 2>&1 &
exec 5>"$DIR/three"
i=0
while [ "$i" -lt 50 ]; do
	[ "$($TMUX list-clients -F '#{client_name}' | wc -l)" -eq 2 ] && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'third client did not attach'
third=$($TMUX list-clients -F '#{client_name}' | tail -n 1)
$TMUX detach-client -t "$third" || fail 'third detach failed'
sleep 0.2
[ "$(grep -ac '^%client-created ' "$DIR/out")" -eq 1 ] ||
    fail 'notification emitted after unsubscribe'
[ "$(grep -ac '^%client-closed ' "$DIR/out")" -eq 1 ] ||
    fail 'close notification emitted after unsubscribe'

exit 0
