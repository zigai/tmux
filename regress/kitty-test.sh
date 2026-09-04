#!/bin/sh

PATH=/bin:/usr/bin
export PATH

cd "$(dirname "$0")" || exit 1

CC=${CC:-cc}
PROG=$(mktemp)
ERR=$(mktemp)
trap "rm -f $PROG $ERR" 0 1 15

if grep -q -- "-DHAVE_B64_NTOP=1" ../Makefile 2>/dev/null; then
	CPPFLAGS="$CPPFLAGS -DHAVE_B64_NTOP=1"
fi
if grep -q -- "-lresolv" ../Makefile 2>/dev/null; then
	LIBS="$LIBS -lresolv"
fi

try_compile() {
	$CC $CPPFLAGS $CFLAGS -DKITTY_TEST -DENABLE_KITTY_IMAGES \
	    -I.. -I../compat kitty-test.c $LDFLAGS "$@" -o "$PROG" \
	    2>"$ERR"
}

if ! try_compile $LIBS; then
	if ! try_compile $LIBS -lresolv; then
		cat "$ERR"
		exit 1
	fi
fi

"$PROG"
