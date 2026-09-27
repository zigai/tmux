#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(mktemp -d) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR
TMUX="$TEST_TMUX -Loutput$$ -f/dev/null"

fail()
{
	echo "$*" >&2
	exit 1
}

cleanup()
{
	$TMUX kill-server 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup 0 1 15

cat >"$DIR/fixture.sh" <<'EOF'
#!/bin/sh
printf '\033]133;A\007$ \033]133;B\007printf one\n'
printf '\033]133;C\007first\nsecond\n'
printf '\033]133;D;7\007'
printf '\033]133;A\007$ \033]133;B\007sleep\n'
printf '\033]133;C\007running\n'
sleep 100
EOF
chmod +x "$DIR/fixture.sh"

$TMUX new-session -d -s output -x 40 -y 8 "$DIR/fixture.sh" || exit 1
i=0
while [ "$i" -lt 50 ]; do
	$TMUX capture-pane -p -t output: | grep -q running && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'fixture did not finish'

$TMUX capture-pane -p -O -t output: 1 >"$DIR/captured" ||
	fail 'completed capture failed'
printf 'first\nsecond\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" || fail 'completed capture mismatch'
$TMUX capture-pane -p -O -t output: 0 >/dev/null 2>&1 &&
	fail 'running capture should fail'
$TMUX capture-pane -paO -t output: 0 >"$DIR/captured" ||
	fail 'running capture with -a failed'
printf '[running] running\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" || fail 'running capture mismatch'

$TMUX copy-mode -t output: || fail 'copy-mode failed'
state=$($TMUX display-message -p -t output: '#{copy_command_state}|#{copy_command_status}')
[ "$state" = 'running|' ] || fail "running format: $state"
$TMUX send-keys -t output: -X select-output || fail 'select-output failed'
$TMUX send-keys -t output: -X copy-selection-no-clear ||
	fail 'copy output failed'
$TMUX save-buffer - >"$DIR/captured" || fail 'save output failed'
printf 'running\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" || fail 'selected output mismatch'
$TMUX send-keys -t output: -X select-command || fail 'select-command failed'
$TMUX send-keys -t output: -X copy-selection-no-clear ||
	fail 'copy command failed'
$TMUX save-buffer - >"$DIR/captured" || fail 'save command failed'
printf 'sleep\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" || fail 'selected command mismatch'

$TMUX send-keys -t output: -X previous-prompt -o ||
	fail 'previous output failed'
state=$($TMUX display-message -p -t output: '#{copy_command_state}|#{copy_command_status}')
[ "$state" = 'completed|7' ] || fail "completed format: $state"
$TMUX send-keys -t output: -X select-output ||
	fail 'completed select-output failed'
$TMUX send-keys -t output: -X copy-selection-no-clear ||
	fail 'completed selection copy failed'
$TMUX save-buffer - >"$DIR/captured" || fail 'completed save failed'
printf 'first\nsecond\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" ||
	fail 'completed selection mismatch'

cat >"$DIR/missing.sh" <<'EOF'
#!/bin/sh
printf 'plain output\n'
printf '\033]133;C\007unknown\n'
printf '\033]133;D;3\007'
sleep 100
EOF
chmod +x "$DIR/missing.sh"
$TMUX new-session -d -s missing -x 40 -y 8 "$DIR/missing.sh" ||
	fail 'missing fixture failed'
i=0
while [ "$i" -lt 50 ]; do
	[ "$($TMUX display-message -p -t missing: '#{pane_command_running}')" = 0 ] &&
	    $TMUX capture-pane -p -t missing: | grep -q unknown && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'missing fixture did not finish'
$TMUX capture-pane -pO -t missing: >/dev/null 2>"$DIR/error" &&
	fail 'unknown-boundary capture should fail'
grep -q boundary-unknown "$DIR/error" || fail 'unknown state not reported'

cat >"$DIR/no-status.sh" <<'EOF'
#!/bin/sh
printf '\033]133;A\007$ \033]133;B\007command\n'
printf '\033]133;C\007no-status\n'
printf '\033]133;D\007'
sleep 100
EOF
chmod +x "$DIR/no-status.sh"
$TMUX new-session -d -s no-status -x 40 -y 8 "$DIR/no-status.sh" ||
	fail 'no-status fixture failed'
i=0
while [ "$i" -lt 50 ]; do
	[ "$($TMUX display-message -p -t no-status: '#{pane_command_running}')" = 0 ] &&
	    $TMUX capture-pane -p -t no-status: | grep -q no-status && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'no-status fixture did not finish'
$TMUX capture-pane -pO -t no-status: >/dev/null 2>"$DIR/error" &&
	fail 'missing status should fail'
grep -q boundary-unknown "$DIR/error" ||
	fail 'missing status state not reported'

cat >"$DIR/truncated.sh" <<'EOF'
#!/bin/sh
printf '\033]133;A\007$ \033]133;B\007long\n'
printf '\033]133;C\007'
i=0
while [ "$i" -lt 20 ]; do
	printf 'line-%02d\n' "$i"
	i=$((i + 1))
done
printf '\033]133;D;5\007'
sleep 100
EOF
chmod +x "$DIR/truncated.sh"
$TMUX set-option -g history-limit 3 || fail 'history-limit failed'
$TMUX new-session -d -s truncated -x 20 -y 4 "$DIR/truncated.sh" ||
	fail 'truncated fixture failed'
i=0
while [ "$i" -lt 50 ]; do
	$TMUX capture-pane -p -t truncated: | grep -q line-19 && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'truncated fixture did not finish'
$TMUX capture-pane -pO -t truncated: >/dev/null 2>&1 &&
	fail 'truncated capture should fail without -a'
$TMUX capture-pane -paO -t truncated: >"$DIR/captured" ||
	fail 'truncated capture with -a failed'
grep -q '^\[truncated\]' "$DIR/captured" ||
	fail 'truncated state prefix missing'
grep -q 'line-19' "$DIR/captured" || fail 'truncated output missing'

cat >"$DIR/wrapped.sh" <<'EOF'
#!/bin/sh
printf '\033]133;A\007$ \033]133;B\007wrap\n'
printf '\033]133;C\007abcdefghijklmno\n'
printf '\033]133;D;0\007'
sleep 100
EOF
chmod +x "$DIR/wrapped.sh"
$TMUX new-session -d -s wrapped -x 10 -y 4 "$DIR/wrapped.sh" ||
	fail 'wrapped fixture failed'
i=0
while [ "$i" -lt 50 ]; do
	$TMUX capture-pane -p -t wrapped: | grep -q klmno && break
	sleep 0.1
	i=$((i + 1))
done
[ "$i" -lt 50 ] || fail 'wrapped fixture did not finish'
$TMUX capture-pane -pO -t wrapped: >"$DIR/captured" ||
	fail 'wrapped capture failed'
printf 'abcdefghijklmno\n' >"$DIR/expected"
cmp "$DIR/captured" "$DIR/expected" || fail 'wrapped capture mismatch'
$TMUX copy-mode -t wrapped: || fail 'wrapped copy-mode failed'
$TMUX send-keys -t wrapped: -X select-output ||
	fail 'wrapped select-output failed'
$TMUX send-keys -t wrapped: -X copy-selection-no-clear ||
	fail 'wrapped copy failed'
$TMUX save-buffer - >"$DIR/captured" || fail 'wrapped save failed'
cmp "$DIR/captured" "$DIR/expected" || fail 'wrapped selection mismatch'

exit 0
