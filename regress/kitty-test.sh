#!/bin/sh

# Build and run the Kitty graphics unit tests against the built tmux objects.

PATH=/bin:/usr/bin
export PATH

cd "$(dirname "$0")" || exit 1

makevar() {
	${MAKE:-make} -s -C .. --no-print-directory \
	    --eval 'print-%: ; $(info $($*))@:' "print-$1" || exit 1
}

# Old image objects can be left over from an earlier build.
case " $(makevar DEFS) " in
*-DENABLE_IMAGES*)
	;;
*)
	exit 101
	;;
esac

CC=${CC:-cc}
PROG=$(mktemp)
ERR=$(mktemp)
MAIN=$(mktemp).o
trap 'rm -f "$PROG" "$ERR" "$MAIN"' 0 1 15

OBJS=$MAIN
for o in ../*.o ../compat/*.o; do
	case $o in
	../tmux.o|../image-kitty.o)
		;;
	*)
		OBJS="$OBJS $o"
		;;
	esac
done

eval "set -- $(makevar DEFS) $(makevar AM_CPPFLAGS) $(makevar CPPFLAGS) \
    $(makevar AM_CFLAGS) $(makevar CFLAGS)"
# Keep the helpers and globals from tmux.c but not its main.
if ! $CC "$@" -Dmain=tmux_main -I.. -I../compat -c -o "$MAIN" ../tmux.c \
    2>"$ERR" || ! $CC "$@" -I.. -I../compat -o "$PROG" kitty-test.c $OBJS \
    $(makevar LDFLAGS) $(makevar LIBS) 2>"$ERR"; then
	cat "$ERR"
	exit 1
fi

"$PROG"
