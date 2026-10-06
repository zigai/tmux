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
TMUX="$TEST_TMUX -Lranges-$$ -f/dev/null"
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
assert_cells()
{
	$TMUX capture-pane -pN -t "$pane" -S - -E - >"$tmp/cells"
	$TMUX capture-pane -pNF -t "$pane" -S - -E - >"$tmp/flags"
	$TMUX list-ranges -t "$pane" -f '#{==:#{range_state},live}' \
	    -F '#{range_id} #{range_start_line} #{range_start_x} #{range_end_line} #{range_end_x}' >"$tmp/positions"
	while read -r rid sl sx el ex; do
		field "$rid" '#{range_text}' >"$tmp/expected"
		awk -v sl="$sl" -v sx="$sx" -v el="$el" -v ex="$ex" \
		    -v flags="$tmp/flags" '
		    BEGIN { while ((getline f < flags) > 0) { split(f, p, " "); wrap[++n] = (p[1] ~ /W/) } }
		    NR - 1 >= sl && NR - 1 <= el {
		        if (!wrap[NR]) sub(/ +$/, "")
		        start = NR - 1 == sl ? sx : 0
		        end = NR - 1 == el ? ex + 1 : length($0)
		        text = text substr($0, start + 1, end - start)
		        if (NR - 1 < el && !wrap[NR]) text = text "\n"
		    }
		    END { print text }' "$tmp/cells" >"$tmp/actual"
		cmp -s "$tmp/expected" "$tmp/actual" || {
			diff -u "$tmp/expected" "$tmp/actual" >&2
			fail "live range $rid changed text at width $width"
		}
	done <"$tmp/positions"
}
states()
{
	$TMUX list-ranges -t "$pane" -F '#{range_state}' | tr '\n' ' '
}
assert_states()
{
	got=$(states)
	[ "$got" = "$1 " ] || fail "states: expected [$1], got [$got]"
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
trap cleanup EXIT HUP INT TERM
$TMUX new-session -d -s ranges -x 40 -y 12 'sleep 10000' || fail start
$TMUX set -g status off || fail status
$TMUX set -g history-limit 2000 || fail history

# Commands, formats, groups, filters and argument errors.
fresh commands
output 'ABCDEFGHIJ\r\nsecond line\r\nthird line'
a=$(new -n bookmark -g marks -r 0:1-0:4)
assert_field "$a" '#{range_name}|#{range_group}|#{range_state}|#{range_text}|#{range_visible_y}' \
    'bookmark|marks|live|BCDE|0'
assert_field "$a" '#{range_start_line}:#{range_start_x}-#{range_end_line}:#{range_end_x}' '0:1-0:4'
$TMUX set-range -t "$pane" -n renamed -r 1:0-1:5 "$a" || fail set
assert_field "$a" '#{range_name}|#{range_text}|#{range_start_line}' 'renamed|second|1'
b=$(new -g others -r 0:0-0:0)
[ "$b" -gt "$a" ] || fail 'ids do not increase'
[ "$($TMUX list-ranges -t "$pane" -g marks -F '#{range_id}')" = "$a" ] || fail group
[ "$($TMUX list-ranges -t "$pane" -f '#{==:#{range_name},renamed}' -F '#{range_id}')" = "$a" ] || fail filter
[ "$($TMUX new-range -t "$pane" -P -F '#{range_state}:#{range_text}' -r 2:0-2:4)" = 'live:third' ] || fail format
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 3 ] || fail count
$TMUX kill-range -t "$pane" "$b" || fail kill
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 2 ] || fail 'count after kill'
$TMUX kill-range -t "$pane" -g marks || fail 'kill group'
[ "$($TMUX list-ranges -t "$pane" -F x | wc -l)" -eq 1 ] || fail 'group kill left wrong ranges'
c=$(new -g marks -r 0:0-0:2)
$TMUX kill-range -t "$pane" -g '' || fail 'kill empty group'
assert_states live
for coordinates in '0:3-0:1' '0:0-99:0' '0:40-0:40' '0:0-0:0junk' '-1:0-0:0' '0:0-0:0:1'; do
	$TMUX new-range -t "$pane" -r "$coordinates" 2>/dev/null && fail "accepted $coordinates"
done
$TMUX new-range -t "$pane" -r 5:0-5:3 2>"$tmp/error" && fail 'accepted blank range'
$TMUX new-range -t "$pane" 2>/dev/null && fail 'accepted no source'
$TMUX new-range -t "$pane" -S -r 0:0-0:0 2>/dev/null && fail 'accepted both sources'
$TMUX new-range -t "$pane" -S 2>/dev/null && fail 'accepted absent selection'
$TMUX kill-range -t "$pane" 2>/dev/null && fail 'kill without target'
$TMUX kill-range -t "$pane" -g marks "$c" 2>/dev/null && fail 'kill with both'
$TMUX kill-range -t "$pane" 9999 2>/dev/null && fail 'killed missing range'
$TMUX set-range -t "$pane" -r 5:0-5:3 "$c" 2>/dev/null && fail 'moved onto blank'
long=$(awk 'BEGIN { for (i = 0; i < 8000; i++) printf "x" }')
a=$(new -n "$long" -g "$long" -r 0:0-0:0)
[ "$(field "$a" '#{range_name}' | wc -c)" -eq 8001 ] || fail 'long name'

# Ranges follow their text into history and are not limited by it.
fresh history
output 'KEEP\r\nSTAY\r\n'
a=$(new -r 0:0-0:3)
b=$(new -r 0:0-1:3)
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n'
[ "$($TMUX display -pt "$pane" '#{history_size}')" -ge 2 ] || fail 'no history'
assert_field "$a" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|0|'
assert_field "$b" '#{range_state}|#{range_text}' 'live|KEEP
STAY'
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|0'
$TMUX copy-mode -t "$pane" || fail copy
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X cancel || fail cancel
assert_field "$a" '#{range_state}' live

# A range goes only when its lines are trimmed from the history, and lines
# keep their number.
$TMUX set -g history-limit 10 || fail historylimit
fresh trim
output 'AAAA\r\nBBBB\r\nCCCC\r\nDDDD'
a=$(new -r 0:0-0:3)
b=$(new -r 0:0-1:3)
c=$(new -r 3:0-3:3)
count_events
n=0
while [ "$($TMUX display -pt "$pane" '#{history_collected}')" -lt 1 ]; do
	output '\r\nrow'
	n=$((n + 1))
	[ "$n" -lt 100 ] || fail 'nothing trimmed'
done
assert_events 1
assert_field "$a" '#{range_state}' gone
assert_field "$b" '#{range_state}|#{range_start_line}' 'modified|1'
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|3'
assert_field "$a" '#{range_start_line}|#{range_visible_y}' '|'
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 2 ] || fail 'count with gone range'
while [ "$($TMUX display -pt "$pane" '#{history_collected}')" -lt 2 ]; do
	output '\r\nrow'
	n=$((n + 1))
	[ "$n" -lt 200 ] || fail 'second line not trimmed'
done
assert_events 2
assert_field "$b" '#{range_state}' gone
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|3'
$TMUX kill-range -t "$pane" "$a" || fail 'kill gone range'
$TMUX set -g history-limit 2000 || fail historylimit

# Overwriting with different text, attributes or colours drops the tag while
# rewriting identical cells keeps it.
fresh overwrite
output 'ABCDEFGHIJ'
a=$(new -r 0:2-0:5)
count_events
output '\033[1;3HCDEF'
assert_field "$a" '#{range_state}' live
assert_events 0
output '\033[1;3HCDXF'
assert_field "$a" '#{range_state}|#{range_text}' 'modified|CDEF'
assert_events 1
output '\033[1;3HCDEF'
assert_field "$a" '#{range_state}' modified
$TMUX set-range -t "$pane" -r 0:2-0:5 "$a" || fail reset
assert_field "$a" '#{range_state}' live
output '\033[1;3HWXYZ'
assert_field "$a" '#{range_state}' gone
fresh overwrite-attributes
output '\033[1mABCDEF\033[m'
a=$(new -r 0:1-0:4)
output '\033[1;2H\033[1mBCDE\033[m'
assert_field "$a" '#{range_state}' live
output '\033[1;2H\033[31;1mBCDE\033[m'
assert_field "$a" '#{range_state}' gone
fresh overwrite-colours
output '\033[38;2;10;20;30mABCDEF\033[m'
a=$(new -r 0:1-0:4)
output '\033[1;2H\033[38;2;10;20;30mBCDE\033[m'
assert_field "$a" '#{range_state}' live
output '\033[1;2H\033[38;2;10;20;31mBCDE\033[m'
assert_field "$a" '#{range_state}' gone
fresh overwrite-clear
output 'ABCDEFGHIJ'
a=$(new -r 0:2-0:5)
output '\033[1;6H\033[1K'
assert_field "$a" '#{range_state}' gone
output '\033[1;1HABCDEFGHIJ'
a=$(new -r 0:2-0:5)
output '\033[1;5H\033[K'
assert_field "$a" '#{range_state}' modified
output '\033[1;1HABCDEFGHIJ'
a=$(new -r 0:2-0:5)
output '\033[1;4H\033[2X'
assert_field "$a" '#{range_state}' modified

# Two ranges can share cells. Killing one leaves the other.
fresh overlap
output 'ABCDEFGHIJ'
a=$(new -r 0:0-0:5)
b=$(new -r 0:3-0:8)
c=$(new -r 0:4-0:4)
assert_states 'live live live'
$TMUX kill-range -t "$pane" "$a" || fail kill
assert_field "$b" '#{range_state}|#{range_text}' 'live|DEFGHI'
assert_field "$c" '#{range_state}|#{range_text}' 'live|E'
output '\033[1;5HEFG'
assert_field "$b" '#{range_state}' live
assert_field "$c" '#{range_state}' live
output '\033[1;5HZ'
assert_field "$b" '#{range_state}' modified
assert_field "$c" '#{range_state}' gone
$TMUX kill-range -t "$pane" "$c" || fail kill
$TMUX kill-range -t "$pane" "$b" || fail kill
output '\033[1;1HABCDEFGHIJ'
a=$(new -r 0:0-0:5)
b=$(new -r 0:3-0:8)
$TMUX set-range -t "$pane" -r 0:6-0:9 "$a" || fail move
assert_field "$a" '#{range_state}|#{range_text}' 'live|GHIJ'
assert_field "$b" '#{range_state}|#{range_text}' 'live|DEFGHI'
output '\033[1;1H\033[K'
assert_states 'gone gone'

# Insert and delete line move the lines with their tags.
fresh lines
output 'row0\r\nrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5'
a=$(new -r 2:0-2:3)
b=$(new -r 1:0-2:3)
c=$(new -r 5:0-5:3)
output '\033[3;1H\033[L'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|3'
assert_field "$b" '#{range_state}' modified
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|6'
output '\033[3;1H\033[M'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|2'
assert_field "$b" '#{range_state}' live
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|5'
output '\033[2;1H\033[M'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|1'
assert_field "$b" '#{range_state}|#{range_start_line}' 'modified|1'
output '\033[2;1H\033[M'
assert_field "$a" '#{range_state}' gone
assert_field "$b" '#{range_state}' gone
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|3'
width=40
assert_cells

fresh wrapped-lines
output 'row0\r\nrow1\r\nabcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
a=$(new -r 2:35-3:12)
output '\033[1;1H\033[L'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|3'
width=40
assert_cells
output '\033[1;1H\033[M'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|2'
assert_cells

# Scroll regions. A line scrolled out of a region that does not start at the
# top goes into the history and the lines above the region are renumbered.
fresh region
output 'row0\r\nrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5'
a=$(new -r 3:0-3:3)
b=$(new -r 1:0-1:3)
c=$(new -r 5:0-5:3)
count_events
output '\033[2;5r\033[5;1H\n'
assert_events 1
assert_field "$a" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|3|2'
assert_field "$b" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|0|'
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|6'
output '\033[2;1H\033M'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|4'
output '\033[2;1H\033[L'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|5'
assert_field "$c" '#{range_state}|#{range_start_line}' 'live|6'
output '\033[2;1H\033[M'
assert_field "$a" '#{range_state}|#{range_start_line}' 'live|4'
output '\033[2;1H\033[4M'
assert_field "$a" '#{range_state}' gone
output '\033[r'
width=40
assert_cells
fresh region-history
output 'row0\r\nrow1\r\nrow2\r\nrow3\r\nrow4\r\nrow5'
a=$(new -r 0:0-0:3)
b=$(new -r 2:0-2:3)
output '\033[1;5r\033[5;1H\n\n'
assert_field "$a" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|0|'
assert_field "$b" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|2|0'
[ "$($TMUX display -pt "$pane" '#{history_size}')" = 2 ] || fail 'region history'
output '\033[r'
assert_cells

# Clearing the screen.
fresh clear
$TMUX set -p -t "$pane" scroll-on-clear off || fail scrollclear
output 'one\r\ntwo\r\nthree\r\nfour\r\nfive'
for i in 0 1 2 3 4; do
	new -r "$i:0-$i:2" >/dev/null
done
count_events
output '\033[H\033[2J'
assert_states 'gone gone gone gone gone'
assert_events 1
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 0 ] || fail 'cleared count'
output 'plain\r\nplain'
assert_events 1
fresh clear-scroll
output 'keep-me\r\nother'
a=$(new -r 0:0-0:6)
output '\033[H\033[2J'
assert_field "$a" '#{range_state}|#{range_start_line}|#{range_visible_y}' 'live|0|'
$TMUX clear-history -t "$pane" || fail clearhistory
assert_field "$a" '#{range_state}' gone
fresh clear-history-lines
output '1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10\r\n11\r\n12\r\n13\r\n14\r\n15\r\n16\r\n17\r\n18\r\n19\r\n20\r\n21\r\n22\r\n23\r\n24\r\n25\r\n26\r\nvisible'
line=$(($($TMUX display -pt "$pane" '#{history_size}') + $($TMUX display -pt "$pane" '#{cursor_y}')))
a=$(new -r "$line:0-$line:6")
$TMUX clear-history -t "$pane" || fail clearhistory
assert_field "$a" '#{range_state}|#{range_start_line}' "live|$line"

# Plain output that leaves the ranges alone raises nothing.
fresh quiet
output 'tagged text'
a=$(new -r 0:0-0:5)
count_events
output '\r\nmore output\r\neven more\r\n'
output '\033[5;1Hlower'
assert_field "$a" '#{range_state}' live
assert_events 0
output '\033[1;1Hchanged'
assert_events 1
output 'again'
assert_events 1

# Reflow, narrower and wider, and a randomized sequence of widths.
fresh resize
output 'abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\nsecond-logical-line-0123456789\r\nthird'
a=$(new -r 0:5-1:12)
b=$(new -r 2:1-2:3)
c=$(new -r 1:8-2:3)
count_events
for width in 13 7 65 40 3 20; do
	$TMUX resize-window -t "$pane" -x "$width" -y 12 || fail resize
	assert_states 'live live live'
	assert_cells
done
assert_events 6
awk 'BEGIN { seed=1729; for(i=0;i<80;i++) { seed=(seed*48271)%2147483647; print 1+seed%70 } }' >"$tmp/widths"
while read -r width; do
	$TMUX resize-window -t "$pane" -x "$width" -y 12 || fail randomresize
	assert_states 'live live live'
	assert_cells
done <"$tmp/widths"
awk 'BEGIN { seed=4242; for(i=0;i<40;i++) { seed=(seed*48271)%2147483647; w=1+seed%60; seed=(seed*48271)%2147483647; print w, 6+seed%12 } }' >"$tmp/sizes"
while read -r width height; do
	$TMUX resize-window -t "$pane" -x "$width" -y "$height" || fail randomresize
	assert_states 'live live live'
	assert_cells
done <"$tmp/sizes"

fresh resize-wide
output 'ab\347\225\214cd\tTARGET-extra\r\nsecond'
a=$(new -r 0:8-0:13)
b=$(new -r 0:2-0:3)
assert_field "$a" '#{range_text}' TARGET
assert_field "$b" '#{range_text}' "$(printf '\347\225\214')"
for width in 5 13 40 3 9; do
	$TMUX resize-window -t "$pane" -x "$width" -y 12 || fail wideresize
	assert_states 'live live'
	assert_field "$b" '#{range_text}' "$(printf '\347\225\214')"
done

fresh resize-history
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\nlong line that will wrap when the window is made narrow'
a=$(new -r 14:5-14:30)
for width in 10 6 30 80; do
	$TMUX resize-window -t "$pane" -x "$width" -y 12 || fail historyresize
	assert_field "$a" '#{range_state}|#{range_text}' 'live|line that will wrap when t'
done
$TMUX resize-window -t "$pane" -x 40 -y 12 || fail resize

# Alternate screen.
fresh alternate
output 'PRIMARY-TEXT\r\nsecond-row'
a=$(new -r 0:0-0:11)
count_events
output '\033[?1049h\033[HALTERNATE'
assert_field "$a" '#{range_state}|#{range_start_line}|#{range_text}|#{range_visible_y}' 'suspended|0|PRIMARY-TEXT|'
assert_events 1
b=$(new -r 0:0-0:8)
assert_field "$b" '#{range_state}|#{range_text}' 'live|ALTERNATE'
output '\033[2;1Hnew'
assert_events 1
$TMUX resize-window -t "$pane" -x 7 -y 8 || fail altresize
assert_field "$a" '#{range_state}' suspended
output '\033[?1049l'
assert_field "$a" '#{range_state}|#{range_text}' 'live|PRIMARY-TEXT'
assert_field "$b" '#{range_state}' gone
assert_events 3
width=7
assert_cells
$TMUX resize-window -t "$pane" -x 40 -y 12 || fail resize

fresh alternate-history
output 'HISTORY\r\n'
a=$(new -r 0:0-0:6)
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n'
output '\033[HPRIMARY'
top=$($TMUX display -pt "$pane" '#{history_size}')
b=$(new -r "$top:0-$top:6")
c=$(new -r "$((top - 1)):0-$top:6")
output '\033[?1049h'
assert_field "$a" '#{range_state}' live
assert_field "$b" '#{range_state}' suspended
assert_field "$c" '#{range_state}' suspended
output '\033[?1049l'
assert_states 'live live live'
output '\033[Hchanged'
assert_field "$b" '#{range_state}' gone
assert_field "$c" '#{range_state}' modified
output '\033[?1049h'
assert_field "$c" '#{range_state}' modified
output '\033[?1049l'
assert_field "$c" '#{range_state}' modified

# Copy mode selection.
fresh selection
output 'ABCDEFGHIJ\r\nsecond line'
$TMUX setw -t "$pane" mode-keys vi || fail modekeys
$TMUX copy-mode -t "$pane" || fail copy
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X start-of-line || fail sol
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X -N 3 cursor-right || fail cursor
b=$($TMUX new-range -t "$pane" -P -S) || fail 'selection range'
assert_field "$b" '#{range_text}|#{range_start_line}:#{range_start_x}-#{range_end_line}:#{range_end_x}' 'ABCD|0:0-0:3'
$TMUX send -t "$pane" -X cancel || fail cancel
assert_field "$b" '#{range_state}' live
$TMUX setw -t "$pane" mode-keys emacs || fail modekeys
$TMUX copy-mode -t "$pane" || fail copy
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X start-of-line || fail sol
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X -N 3 cursor-right || fail cursor
c=$($TMUX new-range -t "$pane" -P -S) || fail 'emacs selection range'
assert_field "$c" '#{range_text}' ABC
$TMUX send -t "$pane" -X clear-selection || fail clearselection
$TMUX send -t "$pane" -X end-of-line || fail eol
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X -N 3 cursor-left || fail cursor
d=$($TMUX new-range -t "$pane" -P -S) || fail 'reverse selection range'
assert_field "$d" '#{range_text}|#{range_end_x}' 'HIJ|9'
$TMUX send -t "$pane" -X cancel || fail cancel

fresh selection-history
output 'ABCDEFGHIJ\r\nsecond line'
$TMUX setw -t "$pane" mode-keys vi || fail modekeys
$TMUX copy-mode -t "$pane" || fail copy
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X start-of-line || fail sol
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X -N 3 cursor-right || fail cursor
output '\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n'
b=$($TMUX new-range -t "$pane" -P -S) || fail 'selection after scroll'
assert_field "$b" '#{range_state}|#{range_text}|#{range_start_line}' 'live|ABCD|0'
$TMUX send -t "$pane" -X cancel || fail cancel

fresh selection-stale
output 'ABCDEFGHIJ'
$TMUX copy-mode -t "$pane" || fail copy
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X start-of-line || fail sol
$TMUX send -t "$pane" -X begin-selection || fail select
$TMUX send -t "$pane" -X -N 3 cursor-right || fail cursor
output '\033[Hchanged'
$TMUX new-range -t "$pane" -S 2>"$tmp/error" && fail 'accepted stale selection'
[ "$(cat "$tmp/error")" = 'selection has changed' ] || fail "stale selection error: $(cat "$tmp/error")"
$TMUX send -t "$pane" -X cancel || fail cancel
[ "$($TMUX display -pt "$pane" '#{pane_ranges}')" = 0 ] || fail 'stale selection left a range'

# Respawn clears the screen. Destroying a pane with ranges raises nothing.
fresh respawn
output 'RESPAWN'
a=$(new -r 0:0-0:6)
count_events
$TMUX respawn-pane -k -t "$pane" 'stty raw -echo; exec cat' || fail respawn
attempt=0
while [ "$(states)" != 'gone ' ]; do
	attempt=$((attempt + 1))
	[ "$attempt" -lt 200 ] || fail 'respawn did not clear the range'
	sleep 0.05
done
assert_events 1

fresh destroy
output 'DESTROY'
a=$(new -r 0:0-0:6)
count_events
other=$pane
$TMUX split-window -dt "$pane" 'sleep 10000' || fail split
$TMUX kill-pane -t "$other" || fail kill
$TMUX display -p '#{pane_id}' >/dev/null || fail 'server died'
[ "$($TMUX display -p '#{@events}')" = 0 ] || fail 'destroy raised an event'

echo 'pane-ranges: PASS'
