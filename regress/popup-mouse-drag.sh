#!/bin/sh

# Check that a mouse drag started inside a display-popup keeps going to the
# popup: motion and release outside the popup are forwarded (clamped to its
# edges), and losing focus mid-drag sends the popup a release.

PATH=/bin:/usr/bin
TERM=screen
export TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"

FLAG="/tmp/tmux-test-flag-$$"
HELPER="/tmp/tmux-test-helper-$$.sh"

cleanup()
{
	rm -f "$FLAG" "$HELPER"
	$TMUX kill-server >/dev/null 2>&1
	$TMUX2 kill-server >/dev/null 2>&1
}
trap cleanup 0 1 15

fail()
{
	echo "$*" >&2
	exit 1
}

# Run one case: open a popup whose job enables SGR mouse mode and writes
# $FLAG when it reads a button release, then send each argument to the
# client in turn.
check()
{
	cleanup
	printf '#!/bin/sh\nprintf "\\033[?1000h\\033[?1006h"\nwhile read -r -n 1 c; do\n    if [ "$c" = "m" ]; then\n        echo OK > "%s"\n        exit 0\n    fi\ndone\n' "$FLAG" > "$HELPER"
	chmod +x "$HELPER"

	$TMUX new-session -d -s outer -x 100 -y 40 'sleep 100' || exit 1
	$TMUX set -g mouse on || exit 1
	$TMUX set -g focus-events on || exit 1
	$TMUX set -s escape-time 0 || exit 1

	$TMUX2 new-session -d -s runner -x 100 -y 40 "$TMUX attach -t outer" ||
	    exit 1
	sleep 1
	OUTER=$($TMUX2 list-panes -t runner -F '#{pane_id}' | head -1)
	[ -n "$OUTER" ] || fail "No outer client pane."

	# Popup is 60x20 at (20,10) in a 100x40 window.
	$TMUX run-shell -b "$TEST_TMUX -LtestA$$ display-popup -w 60 -h 20 -x 20 -y 10 -E $HELPER"
	sleep 1

	for keys in "$@"; do
		$TMUX2 send-keys -t "$OUTER" -l "$keys" 2>/dev/null
		sleep 1
	done
	[ -f "$FLAG" ]
}

# Press inside (35,15), drag outside (5,5), release outside (5,5).
check "$(printf '\033[<0;35;15M\033[<32;5;5M\033[<0;5;5m')" ||
    fail "Release outside popup was not forwarded to the popup job."

# Press inside, then the terminal loses focus before any release.
check "$(printf '\033[<0;35;15M')" "$(printf '\033[O')" ||
    fail "Losing focus mid-drag did not release the popup job's drag."

exit 0
