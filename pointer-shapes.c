/* $OpenBSD$ */

/*
 * Copyright (c) 2026 zigai <ziga.ivansek@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <string.h>

#include "tmux.h"

/*
 * OSC 22 mouse pointer shapes, described at:
 *
 *     https://sw.kovidgoyal.net/kitty/pointer-shapes/
 *
 * Shapes are stored as an index into the table of CSS names; zero means no
 * shape is set.
 */

static const char *pointer_shape_names[] = {
	NULL,
	"alias",
	"cell",
	"copy",
	"crosshair",
	"default",
	"e-resize",
	"ew-resize",
	"grab",
	"grabbing",
	"help",
	"move",
	"n-resize",
	"ne-resize",
	"nesw-resize",
	"no-drop",
	"not-allowed",
	"ns-resize",
	"nw-resize",
	"nwse-resize",
	"pointer",
	"progress",
	"s-resize",
	"se-resize",
	"sw-resize",
	"text",
	"vertical-text",
	"w-resize",
	"wait",
	"zoom-in",
	"zoom-out"
};

/* X11 cursor font names accepted as aliases for the CSS names. */
static const struct {
	const char	*alias;
	const char	*name;
} pointer_shape_aliases[] = {
	{ "arrow", "default" },
	{ "beam", "text" },
	{ "bottom_left_corner", "sw-resize" },
	{ "bottom_right_corner", "se-resize" },
	{ "bottom_side", "s-resize" },
	{ "clock", "wait" },
	{ "closedhand", "grabbing" },
	{ "cross", "cell" },
	{ "crossed_circle", "not-allowed" },
	{ "dnd-copy", "copy" },
	{ "dnd-link", "alias" },
	{ "dnd-move", "move" },
	{ "dnd-no-drop", "no-drop" },
	{ "dnd-none", "grabbing" },
	{ "fleur", "move" },
	{ "forbidden", "not-allowed" },
	{ "half-busy", "progress" },
	{ "hand", "pointer" },
	{ "hand1", "grab" },
	{ "hand2", "pointer" },
	{ "ibeam", "text" },
	{ "left_ptr", "default" },
	{ "left_ptr_watch", "progress" },
	{ "left_side", "w-resize" },
	{ "openhand", "grab" },
	{ "plus", "cell" },
	{ "pointer-move", "move" },
	{ "pointing_hand", "pointer" },
	{ "question_arrow", "help" },
	{ "right_side", "e-resize" },
	{ "sb_h_double_arrow", "ew-resize" },
	{ "sb_v_double_arrow", "ns-resize" },
	{ "size-bdiag", "nesw-resize" },
	{ "size-fdiag", "nwse-resize" },
	{ "size_bdiag", "nesw-resize" },
	{ "size_fdiag", "nwse-resize" },
	{ "split_h", "ew-resize" },
	{ "split_v", "ns-resize" },
	{ "tcross", "crosshair" },
	{ "top_left_corner", "nw-resize" },
	{ "top_right_corner", "ne-resize" },
	{ "top_side", "n-resize" },
	{ "watch", "wait" },
	{ "whats_this", "help" },
	{ "xterm", "text" },
	{ "zoom_in", "zoom-in" },
	{ "zoom_out", "zoom-out" }
};

/* Find a shape by CSS name. Returns zero if not found. */
u_int
pointer_shape_find(const char *name)
{
	u_int	i;

	for (i = 1; i < nitems(pointer_shape_names); i++) {
		if (strcmp(pointer_shape_names[i], name) == 0)
			return (i);
	}
	return (0);
}

/* Find a shape by CSS name or X11 alias. Returns zero if not found. */
u_int
pointer_shape_find_alias(const char *name)
{
	u_int	i;

	for (i = 0; i < nitems(pointer_shape_aliases); i++) {
		if (strcmp(pointer_shape_aliases[i].alias, name) == 0) {
			name = pointer_shape_aliases[i].name;
			break;
		}
	}
	return (pointer_shape_find(name));
}

/* Get the CSS name of a shape. */
const char *
pointer_shape_name(u_int shape)
{
	if (shape == 0 || shape >= nitems(pointer_shape_names))
		return (NULL);
	return (pointer_shape_names[shape]);
}
