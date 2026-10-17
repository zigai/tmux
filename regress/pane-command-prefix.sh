#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
export TERM

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
SOCKET=prefix$$
TMUX="$TEST_TMUX -L$SOCKET -f/dev/null"
TMUX2="$TEST_TMUX -Lprefix-client$$ -f/dev/null"
mkdir -p ../.cache || exit 1
DIR=$(mktemp -d "$(cd .. && pwd)/.cache/prefix.XXXXXXXX") || exit 1

cleanup()
{
	$TMUX2 kill-server 2>/dev/null
	$TMUX kill-server 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup 0 1 15

fail()
{
	echo "$*" >&2
	exit 1
}

cat >"$DIR/launcher" <<'EOF'
#!/bin/sh
record=$1
shift
printf '%s\n' "$@" >"$record.args"
printf '%s\n' "$$" >"$record.pid"
while [ "$1" != -- ]; do shift; done
shift
export PREFIX_ACTIVE=yes
exec "$@"
EOF
cat >"$DIR/probe" <<'EOF'
#!/bin/sh
record=$1
shift
printf '%s\n' "$@" >"$record.args"
printf '%s\n' "$$" "$PREFIX_ACTIVE" "$PROBE_ENV" "$TMUX_PANE" "$PWD" >"$record.state"
pwd >"$record.cwd"
"$TEST_TMUX" -L"$TEST_SOCKET" wait-for -S ready
exec sleep 1000
EOF
cat >"$DIR/shell" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"$SHELL_RECORD"
exec /bin/sh "$@"
EOF
chmod +x "$DIR/launcher" "$DIR/probe" "$DIR/shell"
mkdir "$DIR/working directory"

$TMUX new-session -d -s keeper 'sleep 1000' || exit 1
$TMUX set-environment -g TEST_TMUX "$TEST_TMUX"
$TMUX set-environment -g TEST_SOCKET "$SOCKET"
$TMUX set-environment -g PROBE_ENV inherited
$TMUX set-environment -g HOME "$DIR"
$TMUX set-environment -g HISTFILE /dev/null
$TMUX set-option -g default-shell /bin/sh
$TMUX set-option -g remain-on-exit on
SERVER=$($TMUX display -p '#{pid}')

check_probe()
{
	$TMUX wait-for ready
	PID=$($TMUX display -pt "$PANE" '#{pane_pid}')
	printf '%s\n' "$PID" "$ACTIVE" "$ENVIRONMENT" "$PANE" "$DIR/working directory" >"$DIR/expected.state"
	cmp "$DIR/$CASE.state" "$DIR/expected.state" || fail "$CASE: process, environment or cwd changed"
	[ "$(cat "$DIR/$CASE.cwd")" = "$DIR/working directory" ] || fail "$CASE: cwd changed"
}

ACTIVE=
ENVIRONMENT=inherited
for MODE in unset empty; do
	CASE=$MODE
	if [ "$MODE" = empty ]; then
		$TMUX set-option -g pane-command-prefix '' || exit 1
	fi
	PANE=$($TMUX new-window -dP -F '#{pane_id}' -c "$DIR/working directory" "$DIR/probe" "$DIR/$CASE") || exit 1
	check_probe
	CASE=$MODE-string
	PANE=$($TMUX new-window -dP -F '#{pane_id}' -c "$DIR/working directory" "exec '$DIR/probe' '$DIR/$CASE'") || exit 1
	check_probe
done

PREFIX="'$DIR/launcher' '$DIR/launch-#{s/%//:pane_id}' 'two words' \"double words\" escaped\\ space '' 'literal;value' 'server=#{pid}' 'pane=#{pane_id}' --"
$TMUX set-option -g pane-command-prefix "$PREFIX" || fail 'pane-command-prefix unavailable'

ACTIVE=yes
CASE=argv
PANE=$($TMUX new-session -dP -F '#{pane_id}' -s prefixed -c "$DIR/working directory" -e PROBE_ENV=override "$DIR/probe" "$DIR/$CASE" 'a b' '' '"; touch injected' '$HOME' '*') || exit 1
ENVIRONMENT=override
check_probe
printf '%s\n' 'a b' '' '"; touch injected' '$HOME' '*' >"$DIR/expected.args"
cmp "$DIR/argv.args" "$DIR/expected.args" || fail 'pane argv was reparsed'
printf '%s\n' 'two words' 'double words' 'escaped space' '' 'literal;value' "server=$SERVER" "pane=$PANE" -- "$DIR/probe" "$DIR/argv" 'a b' '' '"; touch injected' '$HOME' '*' >"$DIR/expected.launch"
cmp "$DIR/launch-${PANE#%}.args" "$DIR/expected.launch" || fail 'prefix quoting or new-pane formats incorrect'
[ "$(cat "$DIR/launch-${PANE#%}.pid")" = "$PID" ] || fail 'launcher changed pane PID'

$TMUX set-environment -t prefixed PROBE_ENV inherited
ENVIRONMENT=inherited
CASE=string
COMMAND="exec '$DIR/probe' '$DIR/string' 'one argument; still data'"
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory" "$COMMAND") || exit 1
check_probe
printf '%s\n' 'two words' 'double words' 'escaped space' '' 'literal;value' "server=$SERVER" "pane=$PANE" -- /bin/sh -c "$COMMAND" >"$DIR/expected.launch"
cmp "$DIR/launch-${PANE#%}.args" "$DIR/expected.launch" || fail 'string command was not passed intact to shell -c'

CASE=split
PANE=$($TMUX split-window -dP -F '#{pane_id}' -t prefixed:0 -c "$DIR/working directory" "$DIR/probe" "$DIR/split") || exit 1
check_probe

CASE=default
$TMUX set-option -t prefixed default-command "exec '$DIR/probe' '$DIR/default'"
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory") || exit 1
check_probe
$TMUX set-option -ut prefixed default-command

CASE=respawn
$TMUX respawn-pane -k -t "$PANE" "$DIR/probe" "$DIR/respawn" || exit 1
check_probe
OLD_PID=$PID
$TMUX respawn-pane -k -t "$PANE" || exit 1
check_probe
[ "$PID" != "$OLD_PID" ] || fail 'retained-command respawn did not replace process'
[ "$(cat "$DIR/launch-${PANE#%}.pid")" = "$PID" ] || fail 'respawn did not run launcher'
CASE=respawn-window
$TMUX respawn-window -k -t "$PANE" "$DIR/probe" "$DIR/respawn-window" || exit 1
PANE=$($TMUX display -pt "$PANE" '#{pane_id}')
check_probe

$TMUX set-option -t prefixed pane-command-prefix ''
CASE=override
ACTIVE=
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory" "$DIR/probe" "$DIR/override") || exit 1
check_probe
$TMUX set-option -ut prefixed pane-command-prefix
CASE=inherited
ACTIVE=yes
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory" "$DIR/probe" "$DIR/inherited") || exit 1
check_probe

$TMUX set-option -t prefixed default-shell "$DIR/shell"
$TMUX set-environment -t prefixed SHELL_RECORD "$DIR/login.args"
PANE=$($TMUX split-window -dP -F '#{pane_id}' -t "$PANE") || exit 1
$TMUX send-keys -t "$PANE" "'$TEST_TMUX' -L'$SOCKET' wait-for -S login" Enter
$TMUX wait-for login
printf '%s\n' -l >"$DIR/expected.login"
cmp "$DIR/login.args" "$DIR/expected.login" || fail 'login shell did not receive -l'
$TMUX set-option -ut prefixed default-shell

$TMUX set-hook -g client-attached 'wait-for -S attached'
$TMUX2 new-session -d -x 80 -y 24 "$TEST_TMUX" -L"$SOCKET" attach -t prefixed || exit 1
$TMUX wait-for attached
CASE=popup
ACTIVE=
PANE=$($TMUX display -pt prefixed '#{pane_id}')
$TMUX display-popup -t "$PANE" -d "$DIR/working directory" "exec '$DIR/probe' '$DIR/popup'" &
POPUP_CLIENT=$!
$TMUX wait-for ready
POPUP=$($TMUX list-panes -t prefixed -F '#{pane_modal_flag} #{pane_id}' | awk '$1 == 1 { print $2 }')
[ -n "$POPUP" ] || fail 'popup did not open'
[ ! -f "$DIR/launch-${POPUP#%}.args" ] || fail 'popup ran prefix'
[ "$(head -2 "$DIR/popup.state" | tail -1)" = '' ] || fail 'popup inherited launcher state'
$TMUX respawn-pane -k -t "$POPUP" "$DIR/probe" "$DIR/popup-respawn" || exit 1
$TMUX wait-for ready
[ ! -f "$DIR/launch-${POPUP#%}.args" ] || fail 'popup respawn ran prefix'
[ "$(head -2 "$DIR/popup-respawn.state" | tail -1)" = '' ] || fail 'popup respawn inherited launcher state'
$TMUX display-popup -C -t "$PANE"
wait "$POPUP_CLIENT"

$TMUX run-shell "printf '%s' \"\$PREFIX_ACTIVE\" >'$DIR/job'"
[ ! -s "$DIR/job" ] || fail 'run-shell ran prefix'
$TMUX if-shell 'test -z "$PREFIX_ACTIVE"' "set-option -g @job unaffected"
[ "$($TMUX show-option -gv @job)" = unaffected ] || fail 'if-shell ran prefix'
$TMUX pipe-pane -t "$PANE" -I "echo \"\$PREFIX_ACTIVE\" >'$DIR/pipe'; '$TEST_TMUX' -L'$SOCKET' wait-for -S pipe"
$TMUX wait-for pipe
[ "$(cat "$DIR/pipe")" = '' ] || fail 'pipe-pane ran prefix'
$TMUX pipe-pane -t "$PANE"

PANE=$($TMUX new-window -dEP -F '#{pane_id}' -t prefixed) || exit 1
[ "$($TMUX display -pt "$PANE" '#{pane_pid}')" = '' ] || fail 'empty pane started a process'
[ ! -f "$DIR/launch-${PANE#%}.args" ] || fail 'empty pane ran prefix'

CASE=path
ACTIVE=yes
$TMUX set-option -t prefixed pane-command-prefix "launcher '$DIR/path-launch' --"
PANE=$(PATH="$DIR:$PATH" $TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory" "$DIR/probe" "$DIR/path") || exit 1
check_probe
[ "$(cat "$DIR/path-launch.pid")" = "$PID" ] || fail 'launcher was not found on pane PATH'

LIVE=$PANE
$TMUX set-hook -g pane-died 'wait-for -S dead'
for PREFIX in "$DIR/missing" /bin/false; do
	$TMUX set-option -t prefixed pane-command-prefix "$PREFIX"
	PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed "touch '$DIR/escaped'") || exit 1
	$TMUX wait-for dead
	[ "$($TMUX display -pt "$PANE" '#{pane_dead_status}')" != 0 ] || fail 'bad prefix succeeded'
	[ ! -e "$DIR/escaped" ] || fail 'bad prefix fell back to unwrapped command'
done

$TMUX set-option -t prefixed pane-command-prefix ''
DEAD=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed 'echo retained-output; exit 17') || exit 1
$TMUX wait-for dead

prefix_state()
{
	$TMUX list-sessions -F '#{session_id}'
	$TMUX list-windows -a -F '#{session_id}:#{window_id}'
	$TMUX list-panes -a -F '#{pane_id}:#{pane_pid}:#{pane_dead}:#{pane_dead_status}:#{pane_start_command}'
}

check_prefix_error()
{
	REASON=$1
	shift
	BEFORE=$(prefix_state)
	if ERROR=$($TMUX "$@" 2>&1); then
		fail "invalid prefix accepted by $*"
	fi
	case "$ERROR" in
	*"pane-command-prefix: $REASON"*) ;;
	*) fail "missing prefix diagnostic for $*: $ERROR" ;;
	esac
	[ "$(prefix_state)" = "$BEFORE" ] || fail "$*: invalid prefix changed panes, commands or exit status"
	[ ! -e "$DIR/escaped" ] || fail 'invalid prefix ran pane command'
}

for INVALID in quote-single quote-double separator whitespace format-empty program-empty; do
	case "$INVALID" in
	quote-single) PREFIX="'"; REASON='unterminated quote' ;;
	quote-double) PREFIX='"/bin/true'; REASON='unterminated quote' ;;
	separator) PREFIX="$DIR/launcher ; /bin/true"; REASON='invalid argument list' ;;
	whitespace) PREFIX='   '; REASON='empty command' ;;
	format-empty) PREFIX='#{?pane_id,,}'; REASON='empty command' ;;
	program-empty) PREFIX="'' --"; REASON='empty command' ;;
	esac
	$TMUX set-option -t prefixed pane-command-prefix "$PREFIX"
	check_prefix_error "$REASON" new-window -d -t prefixed "touch '$DIR/escaped'"
	check_prefix_error "$REASON" split-window -d -t "$DEAD" "touch '$DIR/escaped'"
	check_prefix_error "$REASON" respawn-pane -t "$DEAD" "touch '$DIR/escaped'"
	check_prefix_error "$REASON" respawn-pane -k -t "$LIVE" "touch '$DIR/escaped'"
	check_prefix_error "$REASON" respawn-window -t "$DEAD" "touch '$DIR/escaped'"
	$TMUX set-option -g pane-command-prefix "$PREFIX"
	check_prefix_error "$REASON" new-session -d -s rejected "touch '$DIR/escaped'"
done

$TMUX set-option -t prefixed pane-command-prefix "/bin/sh -c 'exit 23' --"
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed "touch '$DIR/escaped'") || exit 1
$TMUX wait-for dead
[ "$($TMUX display -pt "$PANE" '#{pane_dead_status}')" = 23 ] || fail 'launcher exit status changed'
[ ! -e "$DIR/escaped" ] || fail 'failing launcher ran pane command'

$TMUX set-option -ut prefixed pane-command-prefix
$TMUX set-option -gu pane-command-prefix
CASE=reset
ACTIVE=
PANE=$($TMUX new-window -dP -F '#{pane_id}' -t prefixed -c "$DIR/working directory" "$DIR/probe" "$DIR/reset") || exit 1
check_probe

exit 0
