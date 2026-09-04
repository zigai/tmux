#!/bin/sh

PATH=/bin:/usr/bin
export PATH

cd "$(dirname "$0")" || exit 1

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
export TEST_TMUX

CC=${CC:-cc}
PROG=$(mktemp)
ERR=$(mktemp)
trap "rm -f $PROG $ERR" 0 1 15

try_compile() {
	$CC $CPPFLAGS $CFLAGS -I.. -I../compat kitty-draw-test.c $LDFLAGS "$@" -o "$PROG" 2>"$ERR"
}

if ! try_compile; then
	if ! try_compile -lutil; then
		cat "$ERR"
		exit 1
	fi
fi

"$PROG"
