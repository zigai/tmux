#!/bin/sh

# Check that mouse drag selection inside a display-popup overlay forwards
# out-of-bounds drag motion and release to the popup job rather than dropping it.

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
	cleanup
	exit 1
}

cleanup

# Write helper command to execute inside popup:
# Requests SGR mouse mode, reads stdin, and writes flag file upon receiving button release.
printf '#!/bin/sh\nprintf "\\033[?1000h\\033[?1006h"\nwhile read -r -n 1 c; do\n    if [ "$c" = "m" ]; then\n        echo OK > "%s"\n        exit 0\n    fi\ndone\n' "$FLAG" > "$HELPER"
chmod +x "$HELPER"

# 1. Start outer tmux session, 100x40
$TMUX new-session -d -s outer -x 100 -y 40 'sleep 100' || exit 1
$TMUX set -g mouse on || exit 1
$TMUX set -s escape-time 0 || exit 1

# 2. Attach client via secondary tmux session to provide active terminal
$TMUX2 new-session -d -s runner -x 100 -y 40 "$TMUX attach -t outer" || exit 1
sleep 1
OUTER=$($TMUX2 list-panes -t runner -F '#{pane_id}' | head -1)
[ -n "$OUTER" ] || fail "No outer client pane."

# 3. Open display-popup running the helper script
# Bounds: width 60, height 20, centered at (20, 10) in 100x40
$TMUX run-shell -b "tmux -LtestA$$ display-popup -w 60 -h 20 -x 20 -y 10 -E $HELPER"
sleep 1

# 4. Simulate SGR mouse press inside popup (35, 15), drag outside (5, 5), release outside (5, 5)
SEQ=$(printf '\033[<0;35;15M\033[<32;5;5M\033[<0;5;5m')
$TMUX2 send-keys -t "$OUTER" -l "$SEQ" 2>/dev/null
sleep 1

[ -f "$FLAG" ] || fail "Mouse release event outside popup was not forwarded to job."

cleanup
exit 0
