#!/bin/sh

# Kitty OSC 99 notifications are assembled from their chunks for each pane and
# sent to clients with the osc99 feature in one piece, with an identifier
# naming the pane.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
OUTER="$TEST_TMUX -LtestB$$ -f/dev/null"
trap 'stop_server $OUTER; stop_server $TMUX; rm -rf "$DIR"' 0
trap 'exit 1' 1 2 15

fail()
{
	echo "FAIL: $*"
	exit 1
}

# Attach a client with a terminal type and capture what it writes.
attach()
{
	$OUTER new-session -d -s "$1" -x40 -y10 \
	    "while [ ! -e '$DIR/go-$1' ]; do sleep 0.01; done;
	    exec env TERM=$2 $TMUX attach-session -t $3" || exit 1
	$OUTER pipe-pane -t "$1" -O "cat >'$DIR/$1'" || exit 1
	touch "$DIR/go-$1"
}

emit()
{
	$TMUX send-keys -t "$1" -l "$(printf "$2")" || exit 1
}

has()
{
	grep -Fq "$(printf "$2")" "$DIR/$1"
}

# The identifier the terminal sees for a notification from a pane.
oid()
{
	printf 'tmux-%s-%s-%s' "$PID" "${1#%}" "$2"
}

check()
{
	wait_until has full "$1" || fail "$2"
}

CAT="stty raw -echo; exec cat"
A=$($TMUX new-session -d -P -F '#{pane_id}' -s one -x40 -y10 "$CAT") ||
    exit 1
A2=$($TMUX split-window -d -P -F '#{pane_id}' -t "$A" "$CAT") || exit 1
B=$($TMUX new-window -d -P -F '#{pane_id}' -t one: "$CAT") || exit 1
$TMUX set -g status off || exit 1
$TMUX set -as terminal-features 'xterm-256color:osc99' || exit 1
PID=$($TMUX display -p '#{pid}')

attach full xterm-256color one
attach plain screen one
wait_for_clients 2 $TMUX || exit 1

# The terminal is asked what it supports.
check '\033]99;i=tmux:p=?;\033\\' "no query sent"

emit "$A" '\033]99;;Hello\033\\'
check "\033]99;i=$(oid $A n0):a=focus,report:p=title:e=1;SGVsbG8=\033\\\\" \
    "anonymous notification"
emit "$A" '\033]99;i=x:a=-focus:u=2:f=YXBw;Hi\007'
check "\033]99;i=$(oid $A ix):a=-focus,report:u=2:f=YXBw:p=title:e=1;SGk=\033\\\\" \
    "notification with keys"

# Identifiers keep only the characters kitty allows and are short enough that
# kitty does not truncate them once the pane is added.
emit "$A" '\033]99;i=a/b!c;Id\033\\'
check "\033]99;i=$(oid $A iabc):a=focus,report:p=title:e=1;SWQ=\033\\\\" \
    "identifier with other characters"
emit "$A" "\033]99;i=$(printf '%0600d' 0);Long\033\\\\"
check "\033]99;i=$(oid $A i$(printf '%0472d' 0)):a=focus,report:p=title:e=1;TG9uZw==\033\\\\" \
    "long identifier"

# Chunks from two panes do not mix and are sent together.
emit "$A" '\033]99;i=a:d=0:u=2;Ti\033\\'
emit "$A2" '\033]99;i=b;Other\033\\'
check "\033]99;i=$(oid $A2 ib):a=focus,report:p=title:e=1;T3RoZXI=\033\\\\" \
    "notification from second pane"
has full 'VGk=' && fail "unfinished notification sent"
emit "$A" '\033]99;i=a:p=body:e=1:d=0;Qm9\033\\\033]99;i=a:p=body:e=1;keQ==\033\\'
check "\033]99;i=$(oid $A ia):a=focus,report:u=2:p=title:e=1:d=0;VGk=\033\\\\\033]99;i=$(oid $A ia):p=body:e=1;Qm9keQ==\033\\\\" \
    "chunked notification"
emit "$A" '\033]99;i=c:d=0:e=1;QQ==\033\\\033]99;i=c:e=1;Qg==\033\\'
check "\033]99;i=$(oid $A ic):a=focus,report:p=title:e=1;QUI=\033\\\\" \
    "separately encoded chunks"

# A different identifier drops an unfinished notification.
emit "$A" '\033]99;i=old:d=0;Lost\033\\\033]99;i=new;New\033\\'
check "\033]99;i=$(oid $A inew):a=focus,report:p=title:e=1;TmV3\033\\\\" \
    "notification after dropped one"
has full 'TG9zdA' && fail "dropped notification sent"

# A respawned pane starts without the old process's unfinished notification.
$TMUX respawn-pane -k -t "$A" "stty raw -echo;
    printf '\033]99;i=p:d=0;Old\033\\\\\033]99;i=s:p=close;\033\\\\';
    exec cat" || exit 1
check "\033]99;i=$(oid $A is):p=close;\033\\\\" "close after unfinished chunk"
$TMUX respawn-pane -k -t "$A" \
    "stty raw -echo; printf '\033]99;i=p;New\033\\\\'; exec cat" || exit 1
check "\033]99;i=$(oid $A ip):a=focus,report:p=title:e=1;TmV3\033\\\\" \
    "notification after respawn"

# The occasion is about the pane rather than the terminal window.
emit "$A" '\033]99;i=o1:o=unfocused;x\033\\'
check "\033]99;i=$(oid $A io1):a=focus,report:o=unfocused:p=title:e=1;eA==" \
    "unfocused in active pane"
emit "$A2" '\033]99;i=o2:o=unfocused;x\033\\'
check "\033]99;i=$(oid $A2 io2):a=focus,report:o=always:p=title:e=1;eA==" \
    "unfocused in inactive pane"
emit "$A2" '\033]99;i=o3:o=invisible;x\033\\'
check "\033]99;i=$(oid $A2 io3):a=focus,report:o=invisible:p=title:e=1;eA==" \
    "invisible in visible pane"
emit "$B" '\033]99;i=o4:o=invisible;x\033\\'
check "\033]99;i=$(oid $B io4):a=focus,report:o=always:p=title:e=1;eA==" \
    "invisible in hidden window"
$TMUX select-pane -t "$A2" || exit 1
emit "$A2" '\033]99;i=o5:o=unfocused;x\033\\'
check "\033]99;i=$(oid $A2 io5):a=focus,report:o=unfocused:p=title:e=1;eA==" \
    "unfocused after switching pane"

# Icons are kept for later notifications with the same key.
emit "$A" '\033]99;i=i1:g=key1:d=0;Icon\033\\\033]99;i=i1:p=icon:e=1;iVBORw==\033\\'
check "\033]99;i=$(oid $A ii1):a=focus,report:g=key1:p=title:e=1:d=0;SWNvbg==\033\\\\\033]99;i=$(oid $A ii1):p=icon:e=1;iVBORw==\033\\\\" \
    "notification with icon"
emit "$A" '\033]99;i=i2:g=key1;Again\033\\'
check "\033]99;i=$(oid $A ii2):a=focus,report:g=key1:p=title:e=1:d=0;QWdhaW4=\033\\\\\033]99;i=$(oid $A ii2):p=icon:e=1;iVBORw==\033\\\\" \
    "notification with kept icon"

# Icons are kept for all panes together within a limit, so other panes' icons
# replace them.
cat >"$DIR/icons.sh" <<'EOF2'
stty raw -echo
a=$(head -c 900000 /dev/zero | tr '\0' A)
for k in 1 2 3; do
	printf '\033]99;i=b%s:g=big%s:d=0;Big\033\\' $k $k
	for i in 1 2 3; do printf '\033]99;i=b%s:p=icon:d=0;%s\033\\' $k "$a"; done
	printf '\033]99;i=b%s:p=icon;%s\033\\' $k "$a"
done
exec cat
EOF2
$TMUX respawn-pane -k -t "$A2" "sh '$DIR/icons.sh'" || exit 1
check "\033]99;i=$(oid $A2 ib3):p=icon:e=1;QUFB" "big icons"
emit "$A" '\033]99;i=i3:g=key1;Gone\033\\'
check "\033]99;i=$(oid $A ii3):a=focus,report:g=key1:p=title:e=1;R29uZQ==\033\\\\" \
    "notification with replaced icon"

# Nothing is kept for a pane with no client that can show notifications.
C=$($TMUX new-session -d -P -F '#{pane_id}' -s two -x40 -y10 "$CAT") ||
    exit 1
emit "$C" '\033]99;i=i4:g=key4:d=0;Icon\033\\\033]99;i=i4:p=icon:e=1;iVBORw==\033\\seen'
wait_for_text seen $TMUX capture-pane -p -t "$C" || exit 1
$TMUX switch-client -c "$($TMUX list-clients -F '#{client_name}' \
    -f '#{==:#{client_termname},xterm-256color}')" -t two || exit 1
wait_for_output two $TMUX list-clients -F '#{session_name}' \
    -f '#{==:#{client_termname},xterm-256color}' || exit 1
emit "$C" '\033]99;i=i5:g=key4;Later\033\\'
check "\033]99;i=$(oid $C ii5):a=focus,report:g=key4:p=title:e=1;TGF0ZXI=\033\\\\" \
    "notification after no client"
$TMUX switch-client -c "$($TMUX list-clients -F '#{client_name}' \
    -f '#{==:#{client_termname},xterm-256color}')" -t one || exit 1

# A payload longer than kitty's limit is not sent with a gap: text is replaced
# by a message and an icon is dropped.
cat >"$DIR/long.sh" <<'EOF2'
stty raw -echo
a=$(head -c 900000 /dev/zero | tr '\0' A)
b=$(head -c 900000 /dev/zero | tr '\0' B)
for i in 1 2 3 4; do printf '\033]99;i=t:d=0;%s\033\\' "$a"; done
printf '\033]99;i=t:d=0;%s\033\\\033]99;i=t;Tail\033\\' "$b"
printf '\033]99;i=g:d=0;Big icon\033\\'
for i in 1 2 3 4; do printf '\033]99;i=g:p=icon:d=0;%s\033\\' "$a"; done
printf '\033]99;i=g:p=icon:d=0;%s\033\\\033]99;i=g:p=icon;Tail\033\\' "$b"
exec cat
EOF2
$TMUX respawn-pane -k -t "$A" "sh '$DIR/long.sh'" || exit 1
check "\033]99;i=$(oid $A it):a=focus,report:p=title:e=1;IHRvbyBsb25nLCB0cnVuY2F0ZWQ=\033\\\\" \
    "long title"
check "\033]99;i=$(oid $A ig):a=focus,report:p=title:e=1;QmlnIGljb24=\033\\\\" \
    "long icon"

# Nothing is sent from a pane where notifications are not allowed.
$TMUX set -p -t "$A" allow-notifications off || exit 1
emit "$A" '\033]99;i=na;Not allowed\033\\seen'
wait_for_text seen $TMUX capture-pane -p -t "$A" || exit 1
$TMUX set -p -t "$A" allow-notifications on || exit 1
emit "$A" '\033]99;i=ya;Allowed\033\\'
check "\033]99;i=$(oid $A iya):a=focus,report:p=title:e=1;QWxsb3dlZA==\033\\\\" \
    "notification allowed again"
has full "$(oid $A ina)" && fail "sent when not allowed"

# The application can close its notification.
emit "$A" '\033]99;i=x:p=close;\033\\'
check "\033]99;i=$(oid $A ix):p=close;\033\\\\" "close not sent"

sleep 0.5
has plain '\033]99;' && fail "sent to client without osc99"

exit 0
