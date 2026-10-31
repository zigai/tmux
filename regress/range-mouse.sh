#!/bin/sh

# Range mouse location, mouse_range_* formats and copy mode range commands.
# Mouse events are real: an inner client is attached inside a pane of an outer
# server and SGR mouse sequences are written to that pane.

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
TMUX="$TEST_TMUX -Lrange-mouse-$$ -f/dev/null"
TMUX2="$TEST_TMUX -Lrange-mouse-outer-$$ -f/dev/null"
serial=0
cleanup()
{
	$TMUX kill-server >/dev/null 2>&1
	$TMUX2 kill-server >/dev/null 2>&1
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
	while [ "$attempt" -lt 200 ]; do
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
	printf "$1"'\033]2;range-sync-%s\007' "$serial" >"$tmp/data.bin"
	$TMUX load-buffer -b input "$tmp/data.bin" || fail load
	$TMUX paste-buffer -S -r -d -b input -t "$pane" || fail paste
	wait_format '#{pane_title}' "range-sync-$serial"
}
fresh()
{
	$TMUX kill-pane -a -t "$base" || fail 'kill panes'
	pane=$($TMUX split-window -dP -F '#{pane_id}' -t "$base" "$@") ||
	    fail 'new pane'
	$TMUX kill-pane -t "$base" || fail 'kill base'
	base=$pane
}
new()
{
	$TMUX new-range -t "$pane" -P "$@" || fail 'new range'
}

# Write SGR mouse sequences for button B at 1-based COL and ROW: press,
# release, or click for both.
press()
{
	printf '\033[<%s;%s;%sM' "$1" "$2" "$3"
}
release()
{
	printf '\033[<%s;%s;%sm' "$1" "$2" "$3"
}
mouse()
{
	# Wait out the double click time so each press is a new click.
	sleep 0.4
	$TMUX2 send-keys -t "$outer" -l "$1" || fail 'send mouse'
}
click()
{
	mouse "$(press "$1" "$2" "$3")$(release "$1" "$2" "$3")"
}

# Record a format in @got from a binding, then wait for it after an event.
record()
{
	$TMUX set -g @got '' || fail reset
	$TMUX bind "$@" || fail bind
}
recorder()
{
	echo "set -gF @got '$1'"
}
wait_got()
{
	attempt=0
	while [ "$attempt" -lt 300 ]; do
		got=$($TMUX show -gv @got)
		[ -n "$got" ] && break
		attempt=$((attempt + 1))
		sleep 0.01
	done
	[ "$got" = "$1" ] || fail "$2: expected [$1], got [$got]"
	$TMUX set -g @got '' || fail reset
}

# Wait for the application to receive mouse reports ending with these.
wait_app()
{
	attempt=0
	while [ "$attempt" -lt 300 ]; do
		got=$(tr '\033' E <"$tmp/app.in" 2>/dev/null)
		case "$got" in
		*"$1") return ;;
		esac
		attempt=$((attempt + 1))
		sleep 0.01
	done
	fail "$2: application expected [$1], got [$got]"
}

trap cleanup EXIT HUP INT TERM
$TMUX new-session -d -s mouse -x 80 -y 23 'sleep 10000' || fail start
$TMUX set -g status off || fail status
$TMUX set -g mouse on || fail mouse
$TMUX set -g history-limit 2000 || fail history
base=$($TMUX display -p '#{pane_id}')

$TMUX2 new-session -d -x 80 -y 23 "$TMUX attach -t mouse" || fail outer
$TMUX2 set -g status off || fail 'outer status'
outer=$($TMUX2 display -p '#{pane_id}')
attempt=0
while [ "$($TMUX list-clients | wc -l)" -ne 1 ]; do
	attempt=$((attempt + 1))
	[ "$attempt" -lt 200 ] || fail 'client did not attach'
	sleep 0.01
done

# Key names parse and print.
for key in MouseDown1Range MouseUp1Range MouseDown3Range DoubleClick1Range \
    WheelUpRange MouseDragEnd1Range M-MouseDown1Range; do
	$TMUX bind -n "$key" display x || fail "bind $key"
	$TMUX list-keys -T root "$key" | grep -q "$key" || fail "list $key"
	$TMUX unbind -n "$key" || fail "unbind $key"
done

# An application with mouse reporting on, in the alternate screen. Unbound
# Range keys use the Pane binding, so the default MouseDown1Pane binding still
# sends the click on.
cat >"$tmp/app.sh" <<EOF
stty raw -echo
printf '\033[?1049h\033[Halpha beta gamma\r\ndelta epsilon\r\n'
printf '\033[?1000h\033[?1006h'
exec cat >>"$tmp/app.in"
EOF
fresh sh "$tmp/app.sh"
wait_format '#{mouse_any_flag}' 1
: >"$tmp/app.in"
beta=$(new -n beta -g words -r 0:6-0:9)
gamma=$(new -n gamma -g other -r 0:11-0:15)
overlap=$(new -n overlap -r 0:8-0:12)
click 0 8 1
wait_app 'E[<0;8;1ME[<0;8;1m' 'unbound range click'

# A Range binding gets the click instead, with the range formats. Outside a
# range, or with no Range binding, the Pane binding is used.
: >"$tmp/app.in"
record -n MouseDown1Range "$(recorder '#{mouse_range_id}|#{mouse_range_name}|#{mouse_range_group}|#{mouse_any_flag}|#{mouse_x}')"
click 0 8 1
wait_got "$beta|beta|words|1|7" 'beta click'
click 0 11 1
wait_got "$overlap|overlap||1|10" 'overlap click'
click 0 15 1
wait_got "$gamma|gamma|other|1|14" 'gamma click'
record -n MouseDown1Pane "$(recorder 'pane:#{mouse_range_id}')"
click 0 3 2
wait_got 'pane:' 'click outside ranges'
$TMUX unbind -n MouseDown1Range || fail unbind
click 0 8 1
wait_got "pane:$beta" 'unbound range click'
$TMUX bind -n MouseDown1Pane 'select-pane -t= ; send -M' || fail rebind
case $(tr '\033' E <"$tmp/app.in") in
*M*) fail "application got bound clicks: $(tr '\033' E <"$tmp/app.in")" ;;
esac

# A Range binding can pass the event on to the application, which sees the
# same report as without ranges.
$TMUX bind -n MouseDown1Range "if -F '#{mouse_any_flag}' 'send -M' 'display x'" ||
    fail 'bind send'
: >"$tmp/app.in"
click 0 7 1
wait_app 'E[<0;7;1ME[<0;7;1m' 'range binding send -M'
$TMUX unbind -n MouseDown1Range || fail unbind

# Other buttons and events: MouseDown3Range replaces the pane menu,
# MouseUp1Range and DoubleClick1Range are separate keys.
record -n MouseDown3Range "$(recorder 'menu:#{mouse_range_name}')"
click 2 15 1
wait_got 'menu:gamma' 'right click'
$TMUX unbind -n MouseDown3Range || fail unbind
record -n MouseUp1Range "$(recorder 'up:#{mouse_range_name}')"
click 0 8 1
wait_got 'up:beta' 'mouse up'
$TMUX unbind -n MouseUp1Range || fail unbind
record -n DoubleClick1Range "$(recorder 'double:#{mouse_range_name}')"
mouse "$(press 0 8 1)$(release 0 8 1)$(press 0 8 1)$(release 0 8 1)"
wait_got 'double:beta' 'double click'
$TMUX unbind -n DoubleClick1Range || fail unbind

# Copy mode: positions are mapped through the scroll position and line
# numbers, and unbound Range keys use the copy mode Pane binding.
fresh sh -c 'stty raw -echo; printf READY; exec cat'
wait_format '#{cursor_x}' 5
lines=''
i=1
while [ "$i" -le 40 ]; do
	lines="$lines\\r\\nline $i"
	i=$((i + 1))
done
output "\\r\\033[2K$lines"
# The first line is empty, so "line N" is on history line N, and line 40 is
# on the last row.
a=$(new -g g -r 3:0-3:5)
b=$(new -g h -r 10:5-10:6)
c=$(new -g g -r 20:2-20:4)

# Text written over a range removes it from the mouse location.
d=$(new -r 40:0-40:3)
record -n MouseDown1Range "$(recorder 'range:#{mouse_range_id}')"
$TMUX bind -n MouseDown1Pane "$(recorder 'pane:#{mouse_range_id}')" ||
    fail 'bind pane'
click 0 2 23
wait_got "range:$d" 'before overwrite'
output '\rLINE'
click 0 2 23
wait_got 'pane:' 'after overwrite'
$TMUX unbind -n MouseDown1Range || fail unbind
$TMUX bind -n MouseDown1Pane 'select-pane -t= ; send -M' || fail rebind

$TMUX copy-mode -t "$pane" || fail 'copy mode'
$TMUX send -t "$pane" -X history-top || fail top
record -T copy-mode MouseDown1Range "$(recorder 'copy:#{mouse_range_id}:#{mouse_y}')"
$TMUX bind -T copy-mode MouseDown1Pane "$(recorder 'copy-pane:#{mouse_range_id}')" ||
    fail 'bind copy pane'
click 0 1 4
wait_got "copy:$a:3" 'copy mode click'
$TMUX send -t "$pane" -X scroll-down || fail scroll
click 0 1 3
wait_got "copy:$a:2" 'copy mode click after scroll'
click 0 1 4
wait_got 'copy-pane:' 'copy mode click off range'
$TMUX send -t "$pane" -X line-numbers-on || fail 'line numbers'
click 0 1 3
wait_got 'copy-pane:' 'copy mode click on line number'
# 42 lines need two digits, so the numbers take the minimum of four columns:
# column 9 is x 8 on the screen and x 4 in the line, which is inside a.
click 0 9 3
wait_got "copy:$a:2" 'copy mode click with line numbers'
$TMUX send -t "$pane" -X line-numbers-off || fail 'line numbers off'
$TMUX unbind -T copy-mode MouseDown1Range || fail unbind
click 0 1 3
wait_got "copy-pane:$a" 'unbound copy mode range click'

# Copy mode range commands.
cursor()
{
	$TMUX display -pt "$pane" \
	    '#{e|-:#{e|+:#{history_size},#{copy_cursor_y}},#{scroll_position}}:#{copy_cursor_x}:#{copy_cursor_range_id}'
}
assert_cursor()
{
	got=$(cursor)
	[ "$got" = "$1" ] || fail "$2: expected cursor [$1], got [$got]"
}
$TMUX send -t "$pane" -X history-top || fail top
assert_cursor '0:0:' 'top'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "3:0:$a" 'next to a'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "10:5:$b" 'next to b'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "20:2:$c" 'next to c'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "20:2:$c" 'no next after c'
$TMUX send -t "$pane" -X previous-range || fail previous
assert_cursor "10:5:$b" 'previous to b'
$TMUX send -t "$pane" -X previous-range g || fail previous
assert_cursor "3:0:$a" 'previous in group to a'
$TMUX send -t "$pane" -X previous-range || fail previous
assert_cursor "3:0:$a" 'no previous before a'
$TMUX send -t "$pane" -X next-range g || fail next
assert_cursor "20:2:$c" 'next in group to c'
$TMUX send -t "$pane" -X cursor-right || fail right
assert_cursor "20:3:$c" 'inside c'
$TMUX send -t "$pane" -X previous-range || fail previous
assert_cursor "20:2:$c" 'previous to start of c'
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X next-range nothing || fail next
assert_cursor '0:0:' 'next in empty group'
$TMUX send -t "$pane" -X cancel || fail cancel

# Ranges that are moved to new cells are found at their new start.
$TMUX set-range -t "$pane" -r 30:1-30:3 "$a" || fail move
$TMUX copy-mode -t "$pane" || fail 'copy mode'
$TMUX send -t "$pane" -X history-top || fail top
$TMUX send -t "$pane" -X next-range g || fail next
assert_cursor "20:2:$c" 'next after move'
$TMUX send -t "$pane" -X next-range g || fail next
assert_cursor "30:1:$a" 'moved range'
$TMUX send -t "$pane" -X cancel || fail cancel

# Copy mode finds text ranges on the lines that show the text they were found
# on, with the mouse and the range commands.
fresh sh -c 'stty raw -echo; printf READY; exec cat'
wait_format '#{cursor_x}' 5
output '\r\033[2K\033[?1049h\033[Halpha beta gamma\r\ndelta beta'
t1=$(new -a text -m 'alpha beta')
t2=$(new -m delta)
g=$(new -a grid -r 0:11-0:15)
$TMUX copy-mode -t "$pane" || fail 'copy mode'
record -T copy-mode MouseDown1Range "$(recorder 'copy:#{mouse_range_id}')"
click 0 3 1
wait_got "copy:$t1" 'copy mode click on text range'
click 0 13 1
wait_got "copy:$g" 'copy mode click on grid range'
click 0 2 2
wait_got "copy:$t2" 'copy mode click on second text range'
click 0 9 2
wait_got 'copy-pane:' 'copy mode click off text ranges'
$TMUX send -t "$pane" -X history-top || fail top
assert_cursor "0:0:$t1" 'cursor on text range'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "0:11:$g" 'next to grid range'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "1:0:$t2" 'next to text range'
$TMUX send -t "$pane" -X previous-range || fail previous
assert_cursor "0:11:$g" 'previous to grid range'
$TMUX send -t "$pane" -X previous-range || fail previous
assert_cursor "0:0:$t1" 'previous to text range'
output '\033[2J\033[Hdelta'
$TMUX send -t "$pane" -X history-top || fail top
assert_cursor '0:0:' 'text range on old text'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "0:11:$g" 'next found text range on old text'
$TMUX send -t "$pane" -X next-range || fail next
assert_cursor "0:11:$g" 'next found moved text range'
click 0 2 1
wait_got 'copy-pane:' 'click on text range on old text'
$TMUX unbind -T copy-mode MouseDown1Range || fail unbind
$TMUX send -t "$pane" -X cancel || fail cancel

# Copy mode of another pane finds the ranges of that pane until it is
# destroyed, then finds none.
dst=$pane
src=$($TMUX split-window -dP -F '#{pane_id}' -t "$dst" \
    sh -c 'stty raw -echo; printf abc; exec cat') || fail 'source pane'
pane=$src
wait_format '#{cursor_x}' 3
e=$(new -r 0:1-0:2)
pane=$dst
$TMUX copy-mode -s "$src" -t "$dst" || fail 'copy mode source'
$TMUX send -t "$dst" -X history-top || fail top
$TMUX send -t "$dst" -X next-range || fail next
wait_format '#{copy_cursor_x}:#{copy_cursor_range_id}' "1:$e"
$TMUX kill-pane -t "$src" || fail 'kill source'
wait_format '#{window_panes}:#{copy_cursor_range_id}' '1:'
$TMUX send -t "$dst" -X history-top || fail top
$TMUX send -t "$dst" -X next-range || fail next
wait_format '#{copy_cursor_x}:#{copy_cursor_y}' '0:0'
click 0 2 1
wait_got 'copy-pane:' 'click after source destroyed'
$TMUX send -t "$dst" -X cancel || fail cancel

echo 'range-mouse: PASS'
