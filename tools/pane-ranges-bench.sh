#!/bin/sh

# Measure the server CPU time used by a full-screen application that streams
# frames with synchronized output (?2026) into a pane with many text ranges.
# Usage: pane-ranges-bench.sh [tmux] [frames]

TEST_TMUX=$(readlink -f "${1:-./tmux}")
FRAMES=${2:-2000}
ROWS=50
COLUMNS=120
TOKENS=500

tmp=$(mktemp -d) || exit 1
TMUX_TMPDIR=$tmp
export TMUX_TMPDIR
unset TMUX TMUX_PANE
TMUX="$TEST_TMUX -Lranges-bench-$$ -f/dev/null"
trap '$TMUX kill-server >/dev/null 2>&1; rm -rf "$tmp"' EXIT HUP INT TERM

awk -v frames="$FRAMES" -v rows="$ROWS" -v tokens="$TOKENS" 'BEGIN {
	for (f = 0; f < frames; f++) {
		printf "\033[?2026h\033[H"
		for (r = 0; r < rows; r++) {
			printf "row %02d entry token-%03d status ok elapsed %d ms\033[K",
			    r, (f + r) % tokens, f * r % 997
			if (r < rows - 1)
				printf "\r\n"
		}
		printf "\033[?2026l"
	}
}' >"$tmp/frames"

ticks()
{
	awk '{ print $14 + $15 }' "/proc/$1/stat"
}

run()
{
	ranges=$1
	$TMUX new-session -d -x "$COLUMNS" -y "$ROWS" "stty raw -echo; \
	    printf '\\033[?1049h'; until [ -e '$tmp/go' ]; do sleep 0.01; done; \
	    cat '$tmp/frames'; \
	    printf '\\033]2;done\\007'; exec sleep 10000" || exit 1
	$TMUX set -g status off
	server=$($TMUX display -p '#{pid}')
	pane=$($TMUX display -p '#{pane_id}')
	$TMUX set -g @events 0
	$TMUX set-hook -p -t "$pane" pane-ranges-changed \
	    'set -gF @events "#{e|+:#{@events},1}"'
	: >"$tmp/commands"
	i=0
	while [ "$i" -lt "$ranges" ]; do
		printf 'new-range -t %s -m "token-%03d status"\n' "$pane" \
		    $((i % TOKENS)) >>"$tmp/commands"
		i=$((i + 1))
	done
	$TMUX source-file "$tmp/commands" || exit 1

	before=$(ticks "$server")
	start=$(date +%s.%N)
	touch "$tmp/go"
	while [ "$($TMUX display -pt "$pane" '#{pane_title}')" != done ]; do
		sleep 0.05
	done
	end=$(date +%s.%N)
	after=$(ticks "$server")
	events=$($TMUX display -p '#{@events}')
	states=$($TMUX list-ranges -t "$pane" -F '#{range_state}' |
	    sort | uniq -c | awk '{ printf "%s %s ", $2, $1 }')
	$TMUX kill-server
	rm -f "$tmp/go"
	while [ -e "/proc/$server" ]; do
		sleep 0.05
	done
	printf '%6s %8s %8s %8s  %s\n' "$ranges" \
	    "$(awk -v t=$((after - before)) -v hz="$(getconf CLK_TCK)" \
	    'BEGIN { printf "%.2f", t / hz }')" \
	    "$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.2f", e - s }')" \
	    "$events" "$states"
}

printf '%d frames of %dx%d, no client attached\n' "$FRAMES" "$COLUMNS" \
    "$ROWS"
printf '%6s %8s %8s %8s  %s\n' ranges cpu-s wall-s events states
for ranges in 0 0 100 500 1000; do
	run "$ranges"
done
