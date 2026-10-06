#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX_TEST="env -u TMUX -u TMUX_PANE $TEST_TMUX -Lalternate-screen-$$ -f/dev/null"
tmp=$(mktemp -d) || exit 1
TMUX_TMPDIR=$tmp
XDG_CONFIG_HOME=$tmp/config
XDG_CACHE_HOME=$tmp/cache
XDG_DATA_HOME=$tmp/data
XDG_RUNTIME_DIR=$tmp/runtime
mkdir -m 700 "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
    "$XDG_RUNTIME_DIR" || { rm -r "$tmp"; exit 1; }
export TERM TMUX_TMPDIR XDG_CONFIG_HOME XDG_CACHE_HOME XDG_DATA_HOME \
    XDG_RUNTIME_DIR

cleanup()
{
	$TMUX_TEST kill-server >/dev/null 2>&1
	rm -rf "$tmp"
}
fail()
{
	echo "$1" >&2
	exit 1
}
output()
{
	serial=$((serial + 1))
	printf '%s %s\n' "$serial" "$1" >&3 || fail "pane output failed"
	i=0
	while [ "$i" -lt 200 ]; do
		[ "$($TMUX_TEST show -gqv @done)" = "$serial" ] && return
		i=$((i + 1))
		sleep 0.01
	done
	fail "pane output was not parsed: $1"
}
check_events()
{
	actual=$($TMUX_TEST show -gqv @events)
	[ "$actual" = "$1" ] || fail "expected events '$1', got '$actual'"
}

trap cleanup EXIT HUP INT TERM
mkfifo "$tmp/input" || fail "fifo failed"
cat >"$tmp/driver" <<'EOF'
while read -r serial command; do
	eval "$command"
	printf '\033]2;%s\007' "$serial"
done <"$1"
EOF
$TMUX_TEST new-session -d -s alternate -x 80 -y 24 \
    "/bin/sh '$tmp/driver' '$tmp/input'" || fail "session failed"
pane=$($TMUX_TEST display -p '#{pane_id}') || fail "pane lookup failed"
$TMUX_TEST set-hook -p -t "$pane" pane-alternate-screen-changed \
    'set -gF @events "#{@events}|#{alternate_on}"' ||
    fail "alternate-screen hook failed"
$TMUX_TEST set-hook -p -t "$pane" pane-title-changed \
    'set -gF @done "#{hook_new_title}"' || fail "title hook failed"
exec 3>"$tmp/input"
serial=0

for mode in 1049 1047 47; do
	$TMUX_TEST set -g @events '' || fail "reset events failed"
	output "printf '\033[?${mode}h'"
	check_events '|1'
	output "printf '\033[?${mode}h'"
	check_events '|1'
	output "printf '\033[?${mode}l'"
	check_events '|1|0'
	output "printf '\033[?${mode}l'"
	check_events '|1|0'
done

$TMUX_TEST set -g @events '' || fail "reset events failed"
$TMUX_TEST copy-mode -t "$pane" || fail "copy-mode failed"
[ "$($TMUX_TEST display -p '#{pane_in_mode}')" = 1 ] ||
    fail "copy mode did not start"
output ':'
$TMUX_TEST send -X -t "$pane" cancel || fail "copy-mode cancel failed"
output ':'
check_events ''

exit 0
