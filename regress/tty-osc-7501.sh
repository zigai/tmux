#!/bin/sh

# Program status records (OSC 7501) forwarded to a client terminal that
# supports them: detection when the client attaches, ids under the pane id,
# coalescing, clearing on detach, session switch and pane exit, and
# dismissing done and error records with a key. An outer tmux is the
# terminal and pipe-pane -O records what the inner client writes to it.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

. ./regress-common.inc

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX="$TEST_TMUX -LtestI$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestO$$ -f/dev/null"
trap 'stop_servers; rm -rf "$DIR"' 0
trap 'exit 1' 1 2 15

fail()
{
	printf '%s\n' "$*" >&2
	exit 1
}

b64()
{
	printf '%s' "$1" | base64 | tr -d '\n'
}

repeat()
{
	printf "%$2s" '' | tr ' ' "$1"
}

emit()
{
	printf '\033]7501;%s\033\\' "$2" |
	    $TMUX display-message -I -t "$1" || exit 1
}

pane_number()
{
	$TMUX display-message -p -t "$1" '#{s/%//:pane_id}'
}

# Program status sequences written to the outer terminal, one per line, with
# E for Escape. The detection query is left out.
sent()
{
	tr '\033\007' 'EB' <"$DIR/out" |
	    grep -o 'E\]7501;[^E]*E\\' | grep -v '^E\]7501;?'
}

# Wait for exactly these sequences to be written since the last mark.
mark()
{
	marked=$(sent | wc -l)
}
since()
{
	sent | tail -n +$((marked + 1))
}
expect()
{
	expected=$(printf 'E]7501;%sE\\\n' "$@")
	wait_for_output "$expected" since ||
	    fail "expected: $expected" "got: $(since)"
	mark
}

# The same in any order, for reports that may be sent in several batches.
since_sorted()
{
	since | sort
}
expect_sorted()
{
	expected=$(printf 'E]7501;%sE\\\n' "$@" | sort)
	wait_for_output "$expected" since_sorted ||
	    fail "expected: $expected" "got: $(since)"
	mark
}

$TMUX new-session -d -s s1 -x80 -y24 'exec sleep 1000' || exit 1
$TMUX set-option -g status off || exit 1
p1=$($TMUX new-window -P -F '#{pane_id}' -t s1 '') || exit 1
n1=$(pane_number "$p1")
$TMUX new-session -d -s s2 -x80 -y24 'exec sleep 1000' || exit 1
p2=$($TMUX new-window -P -F '#{pane_id}' -t s2 '') || exit 1
n2=$(pane_number "$p2")

# Records that exist before the client attaches are forwarded once the
# terminal has answered the detection query.
emit "$p1" 'state=working:app=build'

$TMUX2 new-session -d -s outer -x80 -y24 \
    "while [ ! -e '$DIR/go' ]; do sleep 0.01; done; exec $TMUX attach -t s1" ||
    exit 1
$TMUX2 set-option -g status off || exit 1
$TMUX2 pipe-pane -O -t outer "cat >>'$DIR/out'" || exit 1
touch "$DIR/go"
wait_for_text programstatus $TMUX list-clients -F '#{client_termfeatures}' ||
    exit 1

# The query is sent right before the primary DA request.
bytes=$(tr '\033\007' 'EB' <"$DIR/out")
before_query=${bytes%%E]7501;?E\\*}
before_da=${bytes%%E\[c*}
[ "${#before_query}" -lt "${#before_da}" ] ||
    fail 'no program status query before the DA request'
[ "$((${#before_query} + 10))" -eq "${#before_da}" ] ||
    fail 'the program status query is not right before the DA request'

marked=0
expect "state=working:id=$n1:app=build"

# Child records are under the pane id. Every key is forwarded.
emit "$p1" 'state=blocked:id=deploy/eu:kind=permission:progress=40:app=dep:title=RVU:msg=QXBwcm92ZT8'
expect "state=blocked:id=$n1/deploy/eu:kind=permission:progress=40:app=dep:title=RVU=:msg=QXBwcm92ZT8="

# Reports are coalesced, so only the last of a burst is sent.
i=0
while [ "$i" -le 50 ]; do
	printf '\033]7501;state=working:id=job:progress=%d\033\\' "$i"
	i=$((i + 1))
done | $TMUX display-message -I -t "$p1" || exit 1
expect "state=working:id=$n1/job:progress=50"

# Only changed records are sent again, and removed ones are cleared.
emit "$p1" 'state=working:id=job:progress=50'
emit "$p1" 'state=done:id=job'
expect "state=done:id=$n1/job"
emit "$p1" 'state=clear:id=deploy'
expect "state=clear:id=$n1/deploy/eu"

# Clearing a record in the terminal also clears the records beneath it, so
# those are sent again.
printf '\033]133;A\033\\' | $TMUX display-message -I -t "$p1" || exit 1
expect "state=clear:id=$n1" "state=done:id=$n1/job"

# A record that would break the terminal's id limits under the pane id is
# not forwarded.
emit "$p1" 'state=idle:id=1/2/3/4/5/6/7'
expect "state=idle:id=$n1/1/2/3/4/5/6/7"
emit "$p1" 'state=idle:id=1/2/3/4/5/6/7/8'
fits=$(repeat a 32)/$(repeat b 32)/$(repeat c 32)/$(repeat d $((128 - ${#n1} - 1 - 99)))
emit "$p1" "state=idle:id=${fits}x"
emit "$p1" "state=idle:id=$fits"
emit "$p1" 'state=idle:id=marker'
expect "state=idle:id=$n1/$fits" "state=idle:id=$n1/marker"

# Panes in other sessions are not forwarded.
emit "$p2" 'state=blocked:kind=question:app=other'
emit "$p1" 'state=idle:id=marker2'
expect "state=idle:id=$n1/marker2"

# A pane that goes away is cleared.
p3=$($TMUX split-window -d -P -F '#{pane_id}' -t s1 '') || exit 1
n3=$(pane_number "$p3")
emit "$p3" 'state=error:app=gone'
expect "state=error:id=$n3:app=gone"
$TMUX kill-pane -t "$p3" || exit 1
expect "state=clear:id=$n3"

# Changing session clears everything and sends the new session's records.
$TMUX switch-client -t s2 || exit 1
expect 'state=clear' "state=blocked:id=$n2:kind=question:app=other"

# Text that is drawn, in the status line, a message or a mode, is not
# treated as markup.
msg=$(b64 '#[fg=red]markup')
title=$(b64 '#[fg=blue]title')
emit "$p2" "state=blocked:kind=question:app=other:title=$title:msg=$msg"
expect "state=blocked:id=$n2:kind=question:app=other:title=$title:msg=$msg"
$TMUX set-option -g status on || exit 1
$TMUX set-option -g status-left-length 60 || exit 1
$TMUX set-option -g status-left '<#{program_message}|#{program_title}>' ||
    exit 1
wait_for_text '<#[fg=red]markup|#[fg=blue]title>' \
    $TMUX2 capture-pane -p -t outer || exit 1
$TMUX set-option -g status-left '' || exit 1
$TMUX display-message -d 0 '[#{program_message}|#{program_title}]' || exit 1
wait_for_text '[#[fg=red]markup|#[fg=blue]title]' \
    $TMUX2 capture-pane -p -t outer || exit 1
$TMUX display-message -d 1 '' || exit 1
$TMUX set-option -g status off || exit 1
$TMUX choose-tree -t "$p2" -F '{#{program_message}|#{program_title}}' ||
    exit 1
wait_for_text '{#[fg=red]markup|#[fg=blue]title}' \
    $TMUX2 capture-pane -p -t outer || exit 1
$TMUX copy-mode -q -t "$p2" || exit 1

# A key typed in a pane removes its done and error records only. Records in
# other panes stay.
p4=$($TMUX split-window -d -P -F '#{pane_id}' -t s2 '') || exit 1
n4=$(pane_number "$p4")
emit "$p4" 'state=done:app=bg'
emit "$p2" 'state=done:id=d'
emit "$p2" 'state=error:id=e'
emit "$p2" 'state=idle:id=i'
expect_sorted "state=done:id=$n2/d" "state=error:id=$n2/e" \
    "state=idle:id=$n2/i" "state=done:id=$n4:app=bg"
$TMUX2 send-keys -t outer x || exit 1
expect "state=clear:id=$n2/d" "state=clear:id=$n2/e"
counts()
{
	$TMUX display-message -p -t "$p2" '#{G:#{program_id}=#{program_state} }'
	$TMUX display-message -p -t "$p4" '#{G:#{program_id}=#{program_state} }'
}
wait_for_output "$(printf '%s\n%s' '=blocked i=idle ' '=done ')" counts ||
    fail "dismissed the wrong records: $(counts)"

# Detaching clears everything.
$TMUX detach-client || exit 1
wait_for_output '' $TMUX list-clients || exit 1
expect 'state=clear'

# A terminal that answers the primary DA request but not the query does not
# support the protocol, and once the DA reply is in, an Escape is not held
# back waiting for an answer.
$TMUX new-session -d -s s3 -x80 -y24 'exec cat >/dev/null' || exit 1
$TMUX set-option -s escape-time 50 || exit 1
$TMUX bind-key -n Escape set-option -g @key E || exit 1
$TMUX bind-key -n M-x set-option -g @key M || exit 1
python3 - "$TEST_TMUX" "-LtestI$$" <<'EOF' || fail 'fake terminal failed'
import fcntl, os, pty, struct, subprocess, sys, termios, time

tmux = [sys.argv[1], sys.argv[2], "-f/dev/null"]
replies = [
    (b"\033[c", b"\033[?62;22c"),
    (b"\033[>c", b"\033[>1;0;0c"),
    (b"\033[>q", b"\033P>|fake 1\033\\"),
    (b"\033[?2026$p", b"\033[?2026;0$y"),
    (b"\033]10;?\033\\", b"\033]10;rgb:ffff/ffff/ffff\033\\"),
    (b"\033]11;?\033\\", b"\033]11;rgb:0000/0000/0000\033\\"),
]
query = b"\033]7501;?\033\\"

def show(fmt):
    return subprocess.run(tmux + ["display-message", "-p", fmt],
        stdout=subprocess.PIPE).stdout.decode().strip()

def clients(fmt):
    return subprocess.run(tmux + ["list-clients", "-F", fmt],
        stdout=subprocess.PIPE).stdout.decode().strip()

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 640, 384))
pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0, 1, 2):
        os.dup2(slave, fd)
    os.environ["TERM"] = "screen"
    os.execvp(tmux[0], tmux + ["attach", "-t", "s3"])
os.set_blocking(master, False)

def read(out):
    try:
        return out + os.read(master, 65536)
    except OSError:
        return out

out = b""
for i in range(200):
    out = read(out)
    if all(q in out for q, r in replies):
        break
    time.sleep(0.05)
else:
    sys.exit("not every request was sent: %r" % out)
if out.find(query) == -1 or out.find(query) + len(query) != out.find(b"\033[c"):
    sys.exit("query is not right before the DA request: %r" % out)
for q, r in sorted(replies, key=lambda x: out.find(x[0])):
    os.write(master, r)
for i in range(200):
    out = read(out)
    if clients("#{client_termtype}") == "fake 1":
        break
    time.sleep(0.05)
else:
    sys.exit("the replies were not read")

os.write(master, b"\033")
time.sleep(0.3)
os.write(master, b"x")
for i in range(200):
    out = read(out)
    if show("#{@key}") != "":
        break
    time.sleep(0.05)
if show("#{@key}") != "E":
    sys.exit("Escape was held back: %s" % show("#{@key}"))

# A late reply is not taken as support.
os.write(master, query.replace(b"?", b"?:states=idle"))
time.sleep(0.3)
features = clients("#{client_termfeatures}")
if "programstatus" in features:
    sys.exit("late reply was used: " + features)
os.kill(pid, 15)
os.waitpid(pid, 0)
EOF

exit 0
