# Shared setup for the Kitty drag and drop (OSC 72) tests. The panes of an
# inner tmux log what they are sent, and outer tmux panes run the inner
# clients and capture what is sent to their terminal.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
INNER="$TEST_TMUX -LtestI$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestO$$ -f/dev/null"
DIR=$(test_tmpdir)

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	stop_server $OUTER
	stop_server $INNER
	rm -rf "$DIR"
}
trap cleanup 0
trap 'exit 1' 1 2 15

# Print the OSC 72 payloads in a file, one per line.
osc72()
{
	tr '\033\007' '\n\n' <"$1" | sed -n 's/^]72;//p'
}

expect()
{
	wait_for_output "$2" osc72 "$1" || fail "$3: got '$(osc72 "$1")'"
	: >"$1"
}

expect_none()
{
	sleep 0.5
	[ -z "$(osc72 "$1")" ] || fail "$2: got '$(osc72 "$1")'"
}

pane_tty()
{
	$INNER display -p -t "$1" '#{pane_tty}'
}

pane()
{
	printf '\033]72;%s\033\\' "$2" >"$(pane_tty "$1")"
}

# Ask for a status report, so all earlier output from the pane is handled,
# then clear the pane's log.
pane_sync()
{
	printf '\033[5n' >"$(pane_tty "$1")"
	wait_for_text '[0n' cat -v "$2" || fail "no status report"
	: >"$2"
}

terminal()
{
	$OUTER send-keys -t "$1" -l "$(printf '\033]72;%s\033\\' "$2")"
}

logger()
{
	echo "stty raw -echo; printf '\\033]2;ready\\007'; exec cat >>$DIR/$1"
}

# Start the inner session with left and right panes, and an outer session to
# attach clients from.
start()
{
	$INNER new -d -s inner -x80 -y24 "$(logger left)" || exit 1
	$INNER set -g status off || exit 1
	$INNER splitw -h -t inner "$(logger right)" || exit 1
	wait_for_output ready $INNER display -p -t inner:0.0 \
	    '#{pane_title}' || exit 1
	wait_for_output ready $INNER display -p -t inner:0.1 \
	    '#{pane_title}' || exit 1
	LEFT=$($INNER display -p -t inner:0.0 '#{pane_id}')
	RIGHT=$($INNER display -p -t inner:0.1 '#{pane_id}')

	$OUTER new -d -s outer -x80 -y24 'sleep 100' || exit 1
	$OUTER set -g status off || exit 1
	$OUTER set -g default-terminal screen || exit 1
}

# Attach a client from an outer pane, capturing its output to a file.
attach()
{
	: >"$2"
	$OUTER pipe-pane -O -t "$1" "cat >>$2" || exit 1
	$OUTER respawn-pane -k -t "$1" "$INNER attach -t inner" || exit 1
	wait_for_text "$(printf '\033]11;?')" cat "$2" ||
	    fail "client did not attach"
}
