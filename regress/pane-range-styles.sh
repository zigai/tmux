#!/bin/sh

# Range styles as drawn by a real client: an inner tmux is attached inside an
# outer tmux pane, and the outer pane is captured with -e or its raw input is
# recorded with pipe-pane.

PATH=/bin:/usr/bin
TERM=screen
SHELL=/bin/sh
LC_ALL=C.UTF-8
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
unset TMUX TMUX_PANE
tmp=$(mktemp -d) || exit 1
TMUX_TMPDIR=$tmp
XDG_CONFIG_HOME=$tmp/config
XDG_CACHE_HOME=$tmp/cache
XDG_DATA_HOME=$tmp/data
XDG_RUNTIME_DIR=$tmp/runtime
mkdir -m 700 "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
    "$XDG_RUNTIME_DIR" || { rm -rf "$tmp"; exit 1; }
export TERM SHELL LC_ALL TMUX_TMPDIR XDG_CONFIG_HOME XDG_CACHE_HOME \
    XDG_DATA_HOME XDG_RUNTIME_DIR
TMUX="$TEST_TMUX -Lrange-styles-$$ -f/dev/null"
OUTER="$TEST_TMUX -Lrange-styles-outer-$$ -f/dev/null"
E=$(printf '\033')
serial=0
cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
	$OUTER kill-server >/dev/null 2>&1
	rm -rf "$tmp"
}
fail()
{
	echo "FAIL: $1" >&2
	exit 1
}
wait_format()
{
	attempt=0
	while [ "$attempt" -lt 100 ]; do
		actual=$($TMUX display -pt "$pane" "$1") || fail 'format failed'
		[ "$actual" = "$2" ] && return
		attempt=$((attempt + 1))
		sleep 0.01
	done
	fail "timeout: $1 expected [$2], got [$actual]"
}
output()
{
	serial=$((serial + 1))
	printf "$1" >"$tmp/data.bin"
	printf '\033]2;range-sync-%s\007' "$serial" >>"$tmp/data.bin"
	$TMUX load-buffer -b input "$tmp/data.bin" || fail load
	$TMUX paste-buffer -S -r -d -b input -t "$pane" || fail paste
	wait_format '#{pane_title}' "range-sync-$serial"
}
fresh()
{
	pane=$($TMUX new-window -P -F '#{pane_id}' -n "$1" \
	    'stty raw -echo; printf READY; exec cat') || fail 'new window'
	wait_format '#{cursor_x}' 5
	output '\r\033[2Khello world\r\nsecond line\r\nthird line'
}
new()
{
	$TMUX new-range -t "$pane" -P "$@" || fail 'new range'
}

# Wait until the outer pane shows a line with the expected styles, ignoring
# what copy mode draws on the right and colours reset from the line before.
assert_line()
{
	attempt=0
	while [ "$attempt" -lt 200 ]; do
		got=$($OUTER capture-pane -pe |
		    sed -n "$1s/^\(${E}\[[34]9m\)*//p")
		case "$got" in
		"$2"|"$2 "*)
			return
		esac
		attempt=$((attempt + 1))
		sleep 0.01
	done
	printf 'expected: %s\n     got: %s\n' "$2" "$got" | cat -v >&2
	fail "$3"
}

# Record the bytes the inner client sends while a command runs.
record()
{
	file=$tmp/record-$1
	shift
	sleep 0.3
	$OUTER pipe-pane -O "cat >$file; touch $file.done" || fail pipe
	"$@"
	sleep 0.3
	$OUTER pipe-pane || fail 'close pipe'
	attempt=0
	while [ ! -f "$file.done" ]; do
		attempt=$((attempt + 1))
		[ "$attempt" -lt 200 ] || fail 'pipe did not close'
		sleep 0.01
	done
}
same_bytes()
{
	cmp -s "$tmp/record-$1" "$tmp/record-$2" || {
		cat -v "$tmp/record-$1" >&2
		echo >&2
		cat -v "$tmp/record-$2" >&2
		echo >&2
		fail "$3"
	}
	[ -s "$tmp/record-$1" ] || fail "nothing recorded: $3"
}
refresh()
{
	$TMUX refresh-client || fail refresh
}
stream()
{
	output '\033[3;1H\033[31mred\033[0m text\r\n\033[1mbold\033[0m\033[K\r\nmore\033[5;3Hx'
}

trap cleanup EXIT HUP INT TERM
$TMUX new-session -d -s styles -x 30 -y 6 'sleep 10000' || fail start
$TMUX set -g status off || fail status
$OUTER new-session -d -x 30 -y 6 || fail 'outer start'
$OUTER set -g status off || fail 'outer status'
$OUTER set -g default-terminal tmux-256color || fail 'outer terminal'
$OUTER send -l "$TMUX attach" || fail 'outer send'
$OUTER send Enter || fail 'outer enter'
attempt=0
while [ -z "$($TMUX list-clients -F x)" ]; do
	attempt=$((attempt + 1))
	[ "$attempt" -lt 500 ] || fail 'no inner client'
	sleep 0.01
done

# The default style comes from pane-range-style and covers exactly the cells
# of the range.
fresh default
a=$(new -r 0:0-0:4)
assert_line 1 "${E}[4mhello${E}[0m world" 'default style'
assert_line 2 'second line' 'unranged line styled'
$TMUX setw pane-range-style 'fg=blue' || fail 'set style option'
assert_line 1 "${E}[34mhello${E}[39m world" 'pane-range-style not applied'
$TMUX setw -u pane-range-style || fail 'unset style option'
assert_line 1 "${E}[4mhello${E}[0m world" 'default style not restored'

# The style is not stored: captures and copied text are unchanged.
[ "$($TMUX capture-pane -pe -t "$pane" | sed -n 1p)" = 'hello world' ] ||
    fail 'style in capture'

# A style given with -s, changed with set-range and reset to the default.
b=$(new -s 'bg=red' -r 0:6-0:10)
assert_line 1 "${E}[4mhello${E}[0m ${E}[41mworld${E}[49m" 'range style'
[ "$($TMUX list-ranges -t "$pane" -f "#{==:#{range_id},$b}" \
    -F '#{range_style}')" = 'bg=red' ] || fail 'range_style format'
$TMUX set-range -t "$pane" -s 'fg=green' "$b" || fail 'set style'
assert_line 1 "${E}[4mhello${E}[0m ${E}[32mworld${E}[39m" 'set-range -s'
$TMUX set-range -t "$pane" -s '' "$b" || fail 'reset style'
assert_line 1 "${E}[4mhello${E}[0m ${E}[4mworld${E}[0m" 'set-range -s empty'
$TMUX new-range -t "$pane" -s 'bg=nosuchcolour' -r 1:0-1:1 2>/dev/null &&
    fail 'accepted bad style'
$TMUX set-range -t "$pane" -s 'nosuchattr' "$b" 2>/dev/null &&
    fail 'set bad style'

# Killing a range removes its style.
$TMUX kill-range -t "$pane" "$b" || fail kill
assert_line 1 "${E}[4mhello${E}[0m world" 'killed range still styled'
$TMUX kill-range -t "$pane" "$a" || fail kill
assert_line 1 'hello world' 'last killed range still styled'

# pane-ranges hides all range styles.
fresh option
a=$(new -s 'bg=red' -r 1:0-1:5)
assert_line 2 "${E}[41msecond${E}[49m line" 'style before off'
$TMUX setw pane-ranges off || fail off
assert_line 2 'second line' 'pane-ranges off'
$TMUX setw pane-ranges on || fail on
assert_line 2 "${E}[41msecond${E}[49m line" 'pane-ranges on'

# Overlapping ranges: the newest range wins where styles conflict.
fresh overlap
new -s 'bg=red' -r 0:0-0:10 >/dev/null
new -s 'fg=blue' -r 0:6-0:10 >/dev/null
assert_line 1 "${E}[41mhello ${E}[34mworld${E}[39m${E}[49m" 'overlap'
new -s 'bg=green' -r 0:0-0:4 >/dev/null
assert_line 1 "${E}[42mhello${E}[41m ${E}[34mworld${E}[39m${E}[49m" 'newest wins'

# A style is a format that can depend on the range state.
fresh state
a=$(new -s '#{?#{==:#{range_state},live},bg=green,bg=yellow}' -r 1:0-1:5)
assert_line 2 "${E}[42msecond${E}[49m line" 'live style'
output '\033[2;1HX'
wait_format '#{pane_ranges}' 1
assert_line 2 "X${E}[43mecond${E}[49m line" 'modified style'

# Rewriting a cell with the same text keeps its range and so its style.
fresh rewrite
a=$(new -s 'bg=red' -r 0:0-0:4)
assert_line 1 "${E}[41mhello${E}[49m world" 'style before rewrite'
output '\033[1;1Hhello'
assert_line 1 "${E}[41mhello${E}[49m world" 'identical rewrite lost style'
output '\033[1;1Hh\033[1;3Hl'
assert_line 1 "${E}[41mhello${E}[49m world" 'identical cells lost style'
output '\033[1;1HJ'
assert_line 1 "J${E}[41mello${E}[49m world" 'overwritten cell still styled'

# Copy mode shows range styles below search matches and the selection, and
# shows ranges created while it is active.
fresh copy
a=$(new -s 'bg=red' -r 0:0-0:4)
$TMUX setw -g mode-keys vi || fail keys
$TMUX copy-mode -t "$pane" || fail 'copy mode'
assert_line 1 "${E}[41mhello${E}[49m world" 'copy mode style'
b=$(new -s 'bg=blue' -r 1:0-1:5)
assert_line 2 "${E}[44msecond${E}[49m line" 'range created in copy mode'
$TMUX set-range -t "$pane" -r 2:0-2:4 "$b" || fail 'move in copy mode'
assert_line 2 'second line' 'moved range left style in copy mode'
assert_line 3 "${E}[44mthird${E}[49m line" 'moved range not styled'
$TMUX setw copy-mode-match-style 'bg=cyan' || fail 'match style'
$TMUX setw copy-mode-current-match-style 'bg=cyan' || fail 'match style'
$TMUX send -t "$pane" -X search-backward 'ell' || fail search
assert_line 1 "${E}[41mh${E}[46mell${E}[41mo${E}[49m world" 'search precedence'
$TMUX send -t "$pane" -X clear-selection || fail 'clear selection'
$TMUX setw mode-style 'bg=magenta' || fail 'mode style'
$TMUX send -t "$pane" -X top-line || fail top
$TMUX send -t "$pane" -X start-of-line || fail start
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X cursor-right || fail right
assert_line 1 "${E}[45mhe${E}[41mllo${E}[49m world" 'selection precedence'
$TMUX send -t "$pane" -X cancel || fail cancel
assert_line 1 "${E}[41mhello${E}[49m world" 'style after copy mode'

# Copy mode of another pane does not draw that pane's ranges and survives the
# pane being killed.
fresh source
a=$(new -s 'bg=red' -r 0:0-0:4)
src=$pane
pane=$($TMUX split-window -P -F '#{pane_id}' -t "$src" 'exec cat') ||
    fail split
$TMUX copy-mode -t "$pane" -s "$src" || fail 'copy mode'
$TMUX kill-pane -t "$src" || fail 'kill source'
$TMUX send -t "$pane" -X top-line || fail top
$TMUX send -t "$pane" -X cursor-down || fail down
assert_line 1 'hello world' 'source range drawn in copy mode'
$TMUX display -t "$pane" -p ok >/dev/null || fail 'server died'

# With no range styles drawn, the client gets the same bytes as for a pane
# without ranges, both for a full redraw and for output.
fresh plain
record plain-refresh refresh
record plain-stream stream
fresh killed
a=$(new -r 0:0-0:4)
$TMUX kill-range -t "$pane" "$a" || fail kill
record killed-refresh refresh
same_bytes plain-refresh killed-refresh 'redraw after kill differs'
fresh hidden
a=$(new -r 0:0-1:5)
$TMUX setw pane-ranges off || fail off
record hidden-refresh refresh
same_bytes plain-refresh hidden-refresh 'redraw with hidden range differs'
record hidden-stream stream
same_bytes plain-stream hidden-stream 'output with hidden range differs'
fresh styled
a=$(new -r 0:0-0:4)
record styled-stream stream
same_bytes plain-stream styled-stream 'output beside a range differs'

exit 0
