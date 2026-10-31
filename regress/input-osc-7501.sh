#!/bin/sh

# Program status reports (OSC 7501) from a pane: syntax, limits and
# validation, the record set, lifetime events, formats, the hook and the
# feature detection reply.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
DIR=$(mktemp -d) || exit 1
trap 'rm -rf "$DIR"; stop_server $TMUX || exit 1' 0
trap 'exit 1' 1 2 15

fail()
{
	printf '%s\n' "$*" >&2
	exit 1
}

b64()
{
	printf "$1" | base64 | tr -d '\n'
}

repeat()
{
	printf "%$2s" '' | tr ' ' "$1"
}

# Write bytes as output from the pane.
raw()
{
	printf '%s' "$1" | $TMUX display-message -I -t "$pane" || exit 1
}

emit()
{
	raw "$(printf '\033]7501;%s\033\\' "$1")"
}

check()
{
	actual=$($TMUX display-message -p -t "$pane" "$1") || exit 1
	[ "$actual" = "$2" ] ||
	    fail "$1: expected '$2', got '$actual'"
}

# Every record as id=state, with . for the root record.
records()
{
	check '#{G:#{?program_id,#{program_id},.}=#{program_state} }' "$1"
}

root()
{
	check '#{program_state}|#{program_name}|#{program_kind}|#{program_progress}|#{program_title}|#{program_message}' "$1"
}

clear_all()
{
	emit 'state=clear'
	records ''
}

$TMUX new-session -d -s test -x80 -y24 'sleep 1000' || exit 1
pane=$($TMUX new-window -d -P -F '#{pane_id}' '') || exit 1

# A pane that never reported has no records.
records ''
check '#{pane_program_count}|#{pane_program_state}' '0|'

# A report sets the root record with its keys.
emit "state=working:app=brew:msg=$(b64 'Installing updates')"
root 'working|brew||||Installing updates'
check '#{pane_program_count}|#{pane_program_state}' '1|working'

# Each report replaces the record completely.
emit 'state=done'
root 'done|||||'

# Unknown keys are ignored; the last of a repeated key wins.
emit 'state=idle:future=1:other=x'
root 'idle|||||'
emit 'state=working:state=error:app=one:app=two'
root 'error|two||||'

# Whitespace around keys and values is removed.
emit ' state = blocked : kind = question '
root 'blocked||question|||'

# Malformed pairs are skipped: no =, an empty key, a key outside [a-z] and a
# value byte outside the value set.
emit "garbage:=done:State=done:state=working:msg=a;b:app=x\$y:title=$(b64 T)"
root 'working||||T|'

# A report with no state or an unknown state is ignored.
emit 'app=nostate'
root 'working||||T|'
emit 'state=paused:app=paused'
root 'working||||T|'
emit 'state=Working:app=upper'
root 'working||||T|'

# kind applies to blocked only and must be known.
emit 'state=working:kind=auth'
root 'working|||||'
emit 'state=blocked:kind=permission'
root 'blocked||permission|||'
emit 'state=blocked:kind=auth'
root 'blocked||auth|||'
emit 'state=blocked:kind=other'
root 'blocked|||||'

# progress is an integer from 0 to 100 with working or blocked only.
emit 'state=working:progress=0'
root 'working|||0||'
emit 'state=working:progress=100'
root 'working|||100||'
emit 'state=blocked:progress=7'
root 'blocked|||7||'
for value in 101 -1 5x 1.5 ''; do
	emit "state=working:progress=$value"
	root 'working|||||'
done
emit 'state=done:progress=50'
root 'done|||||'
emit 'state=idle:progress=50'
root 'idle|||||'

# app outside its character set is treated as absent.
for value in 'a,b' 'a/b' 'a=b' ''; do
	emit "state=idle:app=$value"
	root 'idle|||||'
done
emit 'state=idle:app=A-z_0.9+'
root 'idle|A-z_0.9+||||'

# base64 padding is optional.
emit 'state=idle:msg=SGk'
root 'idle|||||Hi'
emit 'state=idle:msg=SGk='
root 'idle|||||Hi'
emit 'state=idle:title=SGk'
root 'idle||||Hi|'
emit 'state=idle:msg='
root 'idle|||||'
emit "state=idle:msg=$(b64 'caf\303\251 \344\270\255')"
root "idle|||||caf$(printf '\303\251') $(printf '\344\270\255')"

# A report whose base64 does not decode, or whose text has a control
# character or is not UTF-8, is discarded whole.
emit 'state=done'
root 'done|||||'
for value in S SGk=SGk a.b- SG-k "$(b64 'a\nb')" "$(b64 'a\tb')" \
    "$(b64 'a\033b')" "$(b64 'a\000b')" "$(b64 'a\177b')" \
    "$(b64 'a\302\205b')" "$(b64 'a\302\237b')" "$(b64 'a\377b')" \
    "$(b64 'a\300\200b')" "$(b64 'a\355\240\200b')"; do
	emit "state=error:app=bad:msg=$value"
	root 'done|||||'
	emit "state=error:app=bad:title=$value"
	root 'done|||||'
done
nbsp="a$(printf '\302\240')b"
emit "state=error:msg=$(b64 'a\302\240b')"
root "error|||||$nbsp"

# Nothing from a discarded report is applied, even a clear.
emit 'state=clear:msg=S'
root "error|||||$nbsp"

# A key of 16 bytes is allowed; a longer one discards the report.
emit "state=idle:$(repeat k 16)=1"
root 'idle|||||'
emit "state=working:$(repeat k 17)=1"
root 'idle|||||'
emit "state=working:$(repeat K 17)=1"
root 'working|||||'

# The whole sequence may be 4096 bytes, OSC through ST.
sized()
{
	prefix="state=$1:zz="
	fill=$(repeat a $(($2 - 7 - ${#prefix} - ${#3})))
	raw "$(printf '\033]7501;%s%s%s' "$prefix" "$fill" "$3")"
}
ST=$(printf '\033\\')
BEL=$(printf '\007')
sized done 4096 "$ST"
root 'done|||||'
sized working 4097 "$ST"
root 'done|||||'
sized working 4096 "$BEL"
root 'working|||||'
sized done 4097 "$BEL"
root 'working|||||'

# msg is limited to 2732 bytes encoded and 2048 decoded, and the encoded
# size is checked before decoding.
long=$(b64 "$(repeat A 2048)")
[ "${#long}" -eq 2732 ] || fail "bad msg length ${#long}"
emit "state=done:msg=$long"
check '#{n:program_message}' 2048
emit "state=working:msg=${long}="
check '#{program_state}' done
emit "state=working:msg=$(b64 "$(repeat A 2049)")"
check '#{program_state}' done

# title is limited to 256 bytes encoded and 192 decoded.
emit "state=idle:title=$(b64 "$(repeat B 192)")"
check '#{program_state}:#{n:program_title}' idle:192
emit "state=done:title=$(b64 "$(repeat B 193)")"
check '#{program_state}:#{n:program_title}' idle:192
emit "state=done:title=$(b64 "$(repeat B 192)")="
check '#{program_state}:#{n:program_title}' idle:192

# app is limited to 32 bytes.
emit "state=working:app=$(repeat c 32)"
check '#{program_state}:#{n:program_name}' working:32
emit "state=done:app=$(repeat c 33)"
check '#{program_state}:#{n:program_name}' working:32
clear_all

# id is limited to 128 bytes, 32 per segment and 8 levels.
id32=$(repeat i 32)
emit "state=idle:id=$id32"
records "$id32=idle "
emit "state=idle:id=${id32}x"
records "$id32=idle "
clear_all
id128=$id32/$id32/$id32/$(repeat j 29)
emit "state=idle:id=$id128"
records "$id128=idle "
emit "state=idle:id=${id128}j"
records "$id128=idle "
clear_all
emit 'state=idle:id=1/2/3/4/5/6/7/8'
records '1/2/3/4/5/6/7/8=idle '
emit 'state=idle:id=1/2/3/4/5/6/7/8/9'
records '1/2/3/4/5/6/7/8=idle '
clear_all

# A report whose id does not match the grammar is ignored rather than
# applied to the root record.
emit 'state=working'
for id in '' a//b /a a/ 'a,b' 'a=b'; do
	emit "state=error:id=$id"
	records '.=working '
done
emit 'state=error:id=bad/:id=good'
records '.=working good=error '
emit 'state=error:id=a-Z_0.9+/x'
records '.=working a-Z_0.9+/x=error good=error '
clear_all

# Clearing an id removes the record and every record beneath it; clearing
# with no id removes every record.
for id in build build/test build/test/unit buildx build-x other; do
	emit "state=working:id=$id"
done
emit 'state=idle'
emit 'state=clear:id=build'
records '.=idle build-x=working buildx=working other=working '
emit 'state=clear:id=missing'
records '.=idle build-x=working buildx=working other=working '
emit 'state=clear:id=build-x:msg=SGk'
records '.=idle buildx=working other=working '
emit 'state=clear'
records ''
check '#{pane_program_count}|#{program_state}' '0|'

# A record without app takes it from its nearest ancestor that has one.
emit 'state=idle:app=deploy'
emit 'state=working:id=a/b/c'
emit 'state=working:id=x'
check '#{G:#{program_id}=#{program_name} }' '=deploy a/b/c=deploy x=deploy '
emit 'state=working:id=a:app=inner'
check '#{G:#{program_id}=#{program_name} }' \
    '=deploy a=inner a/b/c=inner x=deploy '
emit 'state=working:id=a/b/c:app=own'
check '#{G:#{program_id}=#{program_name} }' \
    '=deploy a=inner a/b/c=own x=deploy '
emit 'state=idle'
check '#{G:#{program_id}=#{program_name} }' '= a=inner a/b/c=own x= '
clear_all

# The most urgent state across the records of a pane, its window and its
# session: blocked, error, done, working, idle.
other=$($TMUX new-window -d -P -F '#{pane_id}' '') || exit 1
check '#{pane_program_state}|#{window_program_state}|#{session_program_state}' '||'
urgent()
{
	emit "state=$1:id=$1"
	check '#{pane_program_state}|#{window_program_state}|#{session_program_state}' \
	    "$1|$1|$1"
}
urgent idle
urgent working
urgent done
urgent error
urgent blocked
check '#{pane_program_count}' 5
emit 'state=clear:id=blocked'
check '#{pane_program_state}' error
emit 'state=clear:id=error'
emit 'state=clear:id=done'
check '#{pane_program_state}' working
saved=$pane
pane=$other
emit 'state=blocked'
check '#{pane_program_state}|#{window_program_state}|#{session_program_state}' \
    'blocked|blocked|blocked'
pane=$saved
check '#{pane_program_state}|#{window_program_state}|#{session_program_state}' \
    'working|working|blocked'
$TMUX kill-pane -t "$other" || exit 1
check '#{session_program_state}' working
clear_all

# A pane holds up to 256 records. A new record past that removes the one
# updated least recently, and an update to an existing record never does.
i=0
while [ "$i" -lt 256 ]; do
	printf '\033]7501;state=idle:id=r%d\033\\' "$i"
	i=$((i + 1))
done | $TMUX display-message -I -t "$pane" || exit 1
check '#{pane_program_count}' 256
emit 'state=working:id=r0'
emit 'state=idle:id=r2'
emit 'state=working:id=r256'
check '#{pane_program_count}' 256
check '#{G:#{?#{m:r[0-2],#{program_id}},#{program_id} ,}}' 'r0 r2 '
check '#{G:#{?#{==:#{program_id},r256},#{program_state},}}' working
emit 'state=done:id=r3'
check '#{pane_program_count}' 256
emit 'state=done:id=r257'
check '#{G:#{?#{m:r[0-3],#{program_id}},#{program_id} ,}}' 'r0 r2 r3 '
clear_all

# An OSC 133 prompt drops working and blocked records but not idle, done and
# error. So do the program exiting and respawning the pane.
lifetime()
{
	for state in idle working done blocked error; do
		emit "state=$state:id=$state"
	done
	emit 'state=working'
}
lifetime
raw "$(printf '\033]133;B\033\\')"
records '.=working blocked=blocked done=done error=error idle=idle working=working '
raw "$(printf '\033]133;A\033\\')"
records 'done=done error=error idle=idle '
clear_all

# Switching screens and a soft reset (DECSTR) do not touch records; a full
# reset (RIS) removes them all.
lifetime
raw "$(printf '\033[?1049h\033[?1049l\033[?47h\033[?47l\033[!p')"
records '.=working blocked=blocked done=done error=error idle=idle working=working '
raw "$(printf '\033c')"
records ''

# Reports from a program that exits leave only idle, done and error.
cat >"$DIR/report" <<'EOF2'
for state in idle working done blocked error; do
	printf '\033]7501;state=%s:id=%s\033\\' $state $state
done
printf '\033]7501;state=working\007'
exec sleep 1000
EOF2
$TMUX set-option -g remain-on-exit on || exit 1
dead=$($TMUX new-window -d -P -F '#{pane_id}' "sh $DIR/report") || exit 1
wait_for_output 6 $TMUX display-message -p -t "$dead" \
    '#{pane_program_count}' || exit 1
$TMUX set-hook -g pane-died 'set -gF @at_death "#{pane_program_count}"' ||
    exit 1
kill "$($TMUX display-message -p -t "$dead" '#{pane_pid}')" || exit 1
wait_for_output 1 $TMUX display-message -p -t "$dead" '#{pane_dead}' ||
    exit 1
pane=$dead
records 'done=done error=error idle=idle '
wait_for_output 3 $TMUX show-options -gv @at_death || exit 1
$TMUX set-hook -gu pane-died || exit 1

# Respawning a live pane kills its program, so the same happens.
$TMUX respawn-pane -k -t "$dead" "sh $DIR/report" || exit 1
wait_for_output 6 $TMUX display-message -p -t "$dead" \
    '#{pane_program_count}' || exit 1
$TMUX respawn-pane -k -t "$dead" 'exec sleep 1000' || exit 1
records 'done=done error=error idle=idle '
$TMUX set-option -g remain-on-exit off || exit 1
$TMUX kill-pane -t "$dead" || exit 1
pane=$saved

# Text is shown without direction overrides and invisible formatting
# characters. Joiners used by emoji are kept.
emit "state=idle:msg=$(b64 'a\342\200\256b\342\200\213c\342\201\246d\357\273\277e\342\200\215f\302\255g')"
root "idle|||||abcde$(printf '\342\200\215')fg"
emit "state=idle:title=$(b64 '\330\234x\342\200\217')"
root 'idle||||x|'

# Markup in text is not expanded outside a status line.
emit "state=idle:msg=$(b64 '#[fg=red]#(echo x)')"
root 'idle|||||#[fg=red]#(echo x)'
clear_all

# The hook runs when the records of a pane change, but not for a report
# that leaves its record as it was or one that is discarded.
$TMUX set-option -g @n 0 || exit 1
$TMUX set-hook -g pane-program-status-changed \
    'set -gF @n "#{e|+:#{@n},1}" ; set -gF @last "#{hook_pane}:#{pane_program_count}"' ||
    exit 1
hooked()
{
	wait_for_output "$1|$2" $TMUX display-message -p '#{@n}|#{@last}' ||
	    fail "hook is $($TMUX display-message -p '#{@n}|#{@last}'), not $1|$2"
}
emit 'state=working:app=x'
hooked 1 "$pane:1"
emit 'state=working:app=x'
emit 'state=working:app=x:msg=S'
emit 'state=clear:id=none'
emit 'state=paused'
sleep 0.3
emit 'state=blocked:id=job'
hooked 2 "$pane:2"
raw "$(printf '\033]133;A\033\\')"
hooked 3 "$pane:0"
raw "$(printf '\033]133;A\033\\')"
emit 'state=done'
hooked 4 "$pane:1"
raw "$(printf '\033c')"
hooked 5 "$pane:0"
emit 'state=clear'
raw "$(printf '\033c')"
emit 'state=idle'
hooked 6 "$pane:1"

# A change to any one field runs the hook.
t1=$(b64 one)
t2=$(b64 two)
n=7
for report in \
    "state=working:progress=1:app=a:title=$t1:msg=$t1" \
    "state=working:progress=2:app=a:title=$t1:msg=$t1" \
    "state=working:progress=2:app=b:title=$t1:msg=$t1" \
    "state=working:progress=2:app=b:title=$t2:msg=$t1" \
    "state=working:progress=2:app=b:title=$t2:msg=$t2" \
    "state=working:progress=2:app=b:title=$t2" \
    "state=blocked:progress=2:app=b:title=$t2" \
    "state=blocked:kind=auth:progress=2:app=b:title=$t2"; do
	emit "$report"
	hooked "$n" "$pane:1"
	n=$((n + 1))
done

# A burst of reports runs the hook once, with the last state.
$TMUX set-hook -ga pane-program-status-changed \
    'set -gF @p "#{program_progress}"' || exit 1
burst()
{
	wait_for_output "$1" $TMUX display-message -p '#{@n}|#{@last}|#{@p}' ||
	    fail "hook is $($TMUX display-message -p '#{@n}|#{@last}|#{@p}'), not $1"
}
i=0
while [ "$i" -le 500 ]; do
	printf '\033]7501;state=working:progress=%d\033\\' $((i % 101))
	i=$((i + 1))
done | $TMUX display-message -I -t "$pane" || exit 1
burst "15|$pane:1|96"
emit 'state=idle'
burst "16|$pane:1|"
$TMUX set-hook -gu pane-program-status-changed || exit 1
clear_all

# The feature detection reply uses the query's terminator and stays in
# order with other replies. Nothing else is ever written back to the pane.
hex()
{
	printf "$1" | od -An -v -tx1 | tr -d ' \n'
}
reply()
{
	expected=$(hex "$2")
	$TMUX respawn-window -k -t test:0 \
	    "stty raw -echo min 1 time 20; printf '$1'; dd bs=1 count=$((${#expected} / 2)) 2>/dev/null | od -An -v -tx1 | tr -d ' \\n' >$DIR/reply" ||
	    exit 1
	wait_for_output 1 $TMUX display-message -p -t test:0 '#{pane_dead}' ||
	    fail "reply reader for '$1' did not exit"
	[ "$(cat "$DIR/reply")" = "$expected" ] ||
	    fail "reply to '$1' is $(cat "$DIR/reply"), not $expected"
}
$TMUX set-option -g remain-on-exit on || exit 1
answer='\033]7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth'
reply '\033]7501;?\033\\' "$answer\\033\\\\"
reply '\033]7501;?\007' "$answer\\007"
reply '\033]7501;state=done:id=x\033\\\033]7501;?\007\033[5n' \
    "$answer\\007\\033[0n"
reply '\033]7501; ?\033\\\033]7501;?x\033\\\033]7501;state=?\033\\\033[5n' \
    '\033[0n'

exit 0
