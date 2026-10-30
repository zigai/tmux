#!/bin/sh

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
TMUX="$TEST_TMUX -Lranges-text-$$ -f/dev/null"
serial=0
cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
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
output_file()
{
	serial=$((serial + 1))
	printf '\033]2;range-sync-%s\007' "$serial" >>"$tmp/data.bin"
	$TMUX load-buffer -b input "$tmp/data.bin" || fail load
	$TMUX paste-buffer -S -r -d -b input -t "$pane" || fail paste
	wait_format '#{pane_title}' "range-sync-$serial"
}
output()
{
	printf "$1" >"$tmp/data.bin"
	output_file
}
fresh()
{
	pane=$($TMUX new-window -dP -F '#{pane_id}' -n "$1" \
	    'stty raw -echo; printf READY; exec cat') || fail 'new window'
	wait_format '#{cursor_x}' 5
	output '\r\033[2K'
}
new()
{
	$TMUX new-range -t "$pane" -P "$@" || fail 'new range'
}
field()
{
	$TMUX list-ranges -t "$pane" -f "#{==:#{range_id},$1}" -F "$2"
}
assert_field()
{
	got=$(field "$1" "$2") || fail 'list failed'
	[ "$got" = "$3" ] || fail "range $1 $2: expected [$3], got [$got]"
}
events()
{
	$TMUX display -p '#{@events}'
}
count_events()
{
	$TMUX set -g @events 0 || fail events
	$TMUX set-hook -p -t "$pane" pane-ranges-changed \
	    'set -gF @events "#{e|+:#{@events},1}"' || fail hook
}
assert_events()
{
	output ''
	got=$(events)
	[ "$got" = "$1" ] || fail "events: expected [$1], got [$got]"
}
assert_error()
{
	expected=$1
	shift
	$TMUX "$@" 2>"$tmp/error" && fail "accepted: $*"
	got=$(cat "$tmp/error")
	[ "$got" = "$expected" ] || fail "$*: expected [$expected], got [$got]"
}
span='#{range_state}|#{range_start_line}:#{range_start_x}-#{range_end_line}:#{range_end_x}|#{range_visible_y}'
trap cleanup EXIT HUP INT TERM
$TMUX new-session -d -s ranges -x 40 -y 12 'sleep 10000' || fail start
$TMUX set -g status off || fail status
$TMUX set -g history-limit 2000 || fail history

# Anchors, commands and errors.
fresh commands
output 'PRIMARY TEXT\r\n'
a=$(new -r 0:0-0:6)
assert_field "$a" '#{range_anchor}' grid
b=$(new -a text -r 0:0-0:11)
assert_field "$b" "#{range_anchor}|#{range_text}|$span" \
    'text|PRIMARY TEXT|live|0:0-0:11|0'
c=$(new -m 'TEXT')
assert_field "$c" "#{range_anchor}|#{range_text}|$span" 'text|TEXT|live|0:8-0:11|0'
d=$(new -m '  ABSENT   TEXT ')
assert_field "$d" "#{range_text}|$span" '  ABSENT   TEXT |hidden|:-:|'
assert_error 'invalid anchor: bogus' new-range -t "$pane" -a bogus -m x
assert_error '-m, -b and -e need a text anchor' new-range -t "$pane" -a grid -m x
assert_error '-m, -b and -e need a text anchor' new-range -t "$pane" -b x -r 0:0-0:1
assert_error 'exactly one of -S, -r or -m is required' new-range -t "$pane" -m x -r 0:0-0:1
assert_error 'range has no text' new-range -t "$pane" -m '  '
assert_error 'range has no text' new-range -t "$pane" -a text -r 5:0-5:3
assert_error 'not a text range' set-range -t "$pane" -b x "$a"
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 4 ] || fail 'range count'
$TMUX kill-range -t "$pane" "$b" || fail kill
assert_field "$c" '#{range_state}' live
$TMUX set-range -t "$pane" -n moved -r 0:0-0:6 "$c" || fail set
assert_field "$c" "#{range_name}|#{range_text}|$span" 'moved|PRIMARY|live|0:0-0:6|0'
$TMUX new-range -t "$pane" -g group -m PRIMARY >/dev/null || fail group
$TMUX new-range -t "$pane" -g group -r 0:8-0:11 >/dev/null || fail group
$TMUX kill-range -t "$pane" -g group || fail killgroup
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 3 ] || fail 'group not killed'
assert_field "$a" '#{range_state}' live

# An alternate screen application that scrolls its viewport.
fresh scroll
output '\033[?1049h\033[Hfirst row\r\nsecond row\r\ntarget words here\r\nfourth row'
a=$(new -r 2:0-2:16)
assert_field "$a" "#{range_anchor}|$span" 'text|live|2:0-2:16|2'
count_events
output '\033[S'
assert_field "$a" "$span" 'live|1:0-1:16|1'
assert_events 1
output '\033[3T'
assert_field "$a" "$span" 'live|4:0-4:16|4'
output '\033[2J\033[7;5Htarget words here'
assert_field "$a" "$span" 'live|6:4-6:20|6'
output '\033[2J\033[Htarget\r\nwords here'
assert_field "$a" "$span" 'live|0:0-1:9|0'
output '\033[S'
assert_field "$a" "$span" 'clipped|0:0-0:9|0'
output '\033[S'
assert_field "$a" "$span" 'hidden|:-:|'
output '\033[12;1Htarget words'
assert_field "$a" "$span" 'clipped|11:0-11:11|11'
output '\033[S'
assert_field "$a" "$span" 'hidden|:-:|'
output '\033[12;1Hhere'
assert_field "$a" "$span" 'live|10:0-11:3|10'

# Unrelated changes raise no event.
count_events
output '\033[2;1Hunrelated'
output '\033[2;1H\033[2K'
assert_events 0

# Text that is re-wrapped by the application or by tmux.
fresh rewrap
output '\033[?1049h\033[2;1Hthe quick brown fox jumps over the lazy dog'
a=$(new -r 1:4-1:18)
assert_field "$a" "#{range_text}|$span" 'quick brown fox|live|1:4-1:18|1'
$TMUX resize-window -t "$pane" -x 20 -y 12 || fail resize
output '\033[2J\033[Hthe quick brown\r\n  fox jumps over\r\nthe lazy dog'
assert_field "$a" "$span" 'live|0:4-1:4|0'
output '\033[2J\033[Hxxxxxxxxxxxxxxxxquick brown fox'
assert_field "$a" "$span" 'live|0:16-1:10|0'
output '\033[2J\033[Hxxxxxxxxxxxxxxxquick\r\nbrown    fox'
assert_field "$a" "$span" 'live|0:15-1:11|0'
output '\033[2J\033[Hquickbrown fox'
assert_field "$a" "$span" 'hidden|:-:|'
$TMUX resize-window -t "$pane" -x 40 -y 12 || fail resize

# Repeated text: the match nearest the last position wins and a tie is
# ambiguous.
fresh repeat
output '\033[?1049h\033[3;1Hbuild ERROR here\033[8;1Hbuild ERROR here'
a=$(new -r 2:6-2:10)
assert_field "$a" "$span" 'live|2:6-2:10|2'
output '\033[T'
assert_field "$a" "$span" 'live|3:6-3:10|3'
output '\033[2J\033[2;1Hbuild ERROR here\033[6;1Hbuild ERROR here'
assert_field "$a" "$span" 'ambiguous|:-:|'
output '\033[2;1H\033[2K'
assert_field "$a" "$span" 'live|5:6-5:10|5'
output '\033[2J\033[1;9HERROR\033[6;1HERROR'
assert_field "$a" "$span" 'live|5:0-5:4|5'
output '\033[6;1H\033[2K\033[6;3HERROR'
assert_field "$a" "$span" 'live|5:2-5:6|5'

# Context before and after the text.
fresh context
output '\033[?1049h\033[Hfirst: ERROR one\r\nsecond:\r\n   ERROR   two'
a=$(new -m ERROR)
assert_field "$a" "$span" 'ambiguous|:-:|'
$TMUX set-range -t "$pane" -b second: "$a" || fail before
assert_field "$a" "$span" 'live|2:3-2:7|2'
$TMUX set-range -t "$pane" -b '' -e 'one' "$a" || fail after
assert_field "$a" "$span" 'live|0:7-0:11|0'
b=$(new -m ERROR -b 'first:' -e two)
assert_field "$b" "$span" 'hidden|:-:|'
c=$(new -m ERROR -b 'st:' -e 'one')
assert_field "$c" "$span" 'live|0:7-0:11|0'
d=$(new -m 'ERROR two' -b 'second: ')
assert_field "$d" "$span" 'live|2:3-2:13|2'
assert_error 'range outside grid' \
    set-range -t "$pane" -b second: -r 99:0-99:3 "$c"
assert_error 'range has no text' \
    set-range -t "$pane" -e two -r 1:10-1:12 "$c"
assert_field "$c" "$span" 'live|0:7-0:11|0'

# Synchronized output: the range is resolved when the update is released.
fresh sync
output '\033[?1049h\033[3;1Hstatus line ready'
a=$(new -m 'line ready')
assert_field "$a" "$span" 'live|2:7-2:16|2'
count_events
output '\033[?2026h\033[2J'
assert_field "$a" "$span" 'live|2:7-2:16|2'
output '\033[9;3Hstatus line ready'
assert_field "$a" "$span" 'live|2:7-2:16|2'
output '\033[?2026l'
assert_field "$a" "$span" 'live|8:9-8:18|8'
assert_events 1
output '\033[2J'
assert_field "$a" "$span" 'hidden|:-:|'
output '\033[5;1Hstatus line ready'
assert_field "$a" "$span" 'live|4:7-4:16|4'
assert_events 3
i=0
while [ "$i" -lt 30 ]; do
	printf '\033[?2026h\033[2J\033[%s;%sHstatus line ready\033[?2026l' \
	    $((i % 12 + 1)) $((i % 20 + 1)) >>"$tmp/data.bin"
	i=$((i + 1))
done
output_file
assert_field "$a" "$span" 'live|5:16-5:25|5'
output '\033[?2026h\033[2J\033[12;1Hstatus line ready'
assert_field "$a" "$span" 'live|5:16-5:25|5'
attempt=0
while [ "$(field "$a" "$span")" != 'live|11:7-11:16|11' ]; do
	attempt=$((attempt + 1))
	[ "$attempt" -lt 100 ] || fail 'synchronized update did not time out'
	sleep 0.05
done

# Text ranges belong to the screen they were created on.
fresh screens
output 'shell prompt text'
a=$(new -a text -m 'prompt text')
assert_field "$a" "$span" 'live|0:6-0:16|0'
output '\033[?1049h\033[Hprompt text in app'
assert_field "$a" "$span" 'suspended|0:6-0:16|'
b=$(new -m 'in app')
assert_field "$b" "$span" 'live|0:12-0:17|0'
output '\033[?1049l'
assert_field "$a" "$span" 'live|0:6-0:16|0'
assert_field "$b" "$span" 'suspended|0:12-0:17|'
output '\033[?1049h\033[3;1Hstarted again in app'
assert_field "$b" "$span" 'live|2:14-2:19|2'
assert_field "$a" '#{range_state}' suspended

# A reset of the screen without output from the pane is seen.
fresh reset
output '\033[?1049h\033[4;1Hhello world'
a=$(new -m 'hello world')
assert_field "$a" "$span" 'live|3:0-3:10|3'
$TMUX send-keys -R -t "$pane" || fail reset
assert_field "$a" "$span" 'hidden|:-:|'

# A text range on the primary screen keeps its history line as text scrolls
# and is hidden when the text leaves the screen.
fresh primary
output 'a\r\nb\r\nkeep this line\r\n'
a=$(new -a text -r 2:0-2:13)
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n'
assert_field "$a" "$span" 'live|2:0-2:13|0'
output '\r\n'
assert_field "$a" "$span" 'hidden|:-:|'
$TMUX clear-history -t "$pane" || fail clear
output '\033[Hkeep this line'
top=$($TMUX display -pt "$pane" '#{history_collected}')
assert_field "$a" "$span" "live|$top:0-$top:13|0"

# Wide characters.
fresh wide
output '\033[?1049h\033[H前の行 wide 文字 end'
a=$(new -m 'wide 文字')
assert_field "$a" "$span" 'live|0:7-0:15|0'
b=$(new -m '行 wide')
assert_field "$b" "$span" 'live|0:4-0:10|0'

echo 'pane-ranges-text: PASS'
