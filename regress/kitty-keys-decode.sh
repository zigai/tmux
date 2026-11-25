#!/bin/sh

# Keys sent by a terminal using the Kitty keyboard protocol are decoded into
# tmux key names. An outer tmux stands in for the terminal and answers the
# inner tmux's query, so the inner tmux turns the protocol on.

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -LtestA$$ -f/dev/null"
TMUX2="$TEST_TMUX -LtestB$$ -f/dev/null"
TMP=$(mktemp)
prompt_pid=

cleanup()
{
	if [ -n "$prompt_pid" ]; then
		kill "$prompt_pid" 2>/dev/null
		wait "$prompt_pid" 2>/dev/null
	fi
	rm -f "$TMP"
	stop_servers
}
trap cleanup 0
trap 'exit 1' 1 2 15

fail()
{
	echo "[FAIL] $*" >&2
	exit 1
}

$TMUX2 set -g extended-keys on \; new -d || exit 1
$TMUX set -g extended-keys on \; new -d "$TMUX2 attach" || exit 1
wait_for_output 'Kitty 5' $TMUX display -p '#{pane_key_mode}' ||
    fail "inner tmux did not turn on the protocol"

assertion=0
assert_key()
{
	keys=$1
	expected=$2

	assertion=$((assertion + 1))
	prompt="kitty-keys-$assertion:"
	$TMUX2 command-prompt -k -p "$prompt" 'display-message -pl "%%"' \
	    >"$TMP" &
	prompt_pid=$!
	wait_for_text "$prompt" $TMUX capture-pane -p ||
	    fail "$keys: timed out waiting for prompt"
	$TMUX send-keys -H $(printf "$keys" | od -An -v -t x1) ||
	    fail "could not send $keys"
	wait_for_exit "$prompt_pid" || fail "$keys: timed out waiting for key"
	wait "$prompt_pid" || fail "$keys: prompt failed"
	prompt_pid=

	IFS= read -r actual <"$TMP"
	[ "$actual" = "$expected" ] ||
	    fail "$keys -> $expected (got '$actual')"
}

# Modifiers, including Super and Hyper which only this protocol reports, and
# Meta which is the same as Alt to tmux.
assert_key '\033[97;3u' 'M-a'
assert_key '\033[97;5u' 'C-a'
assert_key '\033[97;6u' 'C-S-a'
assert_key '\033[97;7u' 'C-M-a'
assert_key '\033[97;9u' 'Super-a'
assert_key '\033[97;17u' 'Hyper-a'
assert_key '\033[97;33u' 'M-a'

# A key with only Shift is the character it types, from the alternate key or
# the associated text when they are sent. Lock modifiers are ignored.
assert_key '\033[97;2u' 'A'
assert_key '\033[97:65;2u' 'A'
assert_key '\033[49:33;2u' '!'
assert_key '\033[49;2;33u' '!'
assert_key '\033[97;65u' 'a'
assert_key '\033[97;65;65u' 'A'
assert_key '\033[97;129u' 'a'
assert_key '\033[97:65:97;5u' 'C-a'

# Keys with their own tmux key name or another final byte.
assert_key '\033[9;5u' 'C-Tab'
assert_key '\033[9;2u' 'BTab'
assert_key '\033[13;5u' 'C-Enter'
assert_key '\033[27u' 'Escape'
assert_key '\033[32;5u' 'C-Space'
assert_key '\033[127;5u' 'C-BSpace'
assert_key '\033[1;5A' 'C-Up'
assert_key '\033[1;3D' 'M-Left'
assert_key '\033[1;5H' 'C-Home'
assert_key '\033[1;5P' 'C-F1'
assert_key '\033[15;5~' 'C-F5'
assert_key '\033[57376u' 'F13'
assert_key '\033[57399;5u' 'C-KP0'
assert_key '\033[233;5u' 'C-é'

# A repeat is a press. A release and a modifier key are never offered to the
# prompt, so the key after them is the one read.
assert_key '\033[97;5:2u' 'C-a'
assert_key '\033[97;5:3u\033[98;5u' 'C-b'
assert_key '\033[57441;2u\033[98;5u' 'C-b'

exit 0
