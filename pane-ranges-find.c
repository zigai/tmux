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
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Find pane ranges at a position, for the Range mouse location, and the next
 * or previous range, for copy mode. The grid may be the pane's own or a copy
 * of its cells such as the copy mode backing screen; range set numbers are
 * always resolved through the pane that owns the cells.
 */

#define PANE_RANGES_LOCATION_MASK \
	(((1ULL << KEYC_MOUSE_BUTTON_SHIFT) - 1) << KEYC_MOUSE_LOCATION_SHIFT)

/* Check if a key is a mouse key with a location. */
static int
pane_ranges_is_mouse_key(key_code key)
{
	key_code	type = key & KEYC_MASK_TYPE;

	return (type >= KEYC_SHIFT_TYPE(KEYC_TYPE_MOUSEMOVE) &&
	    type <= KEYC_SHIFT_TYPE(KEYC_TYPE_TRIPLECLICK));
}

/* Get the location of a mouse key. */
static enum key_code_mouse_location
pane_ranges_key_location(key_code key)
{
	return ((key & PANE_RANGES_LOCATION_MASK) >>
	    KEYC_MOUSE_LOCATION_SHIFT);
}

/* Change the location of a mouse key. */
static key_code
pane_ranges_key_set_location(key_code key, enum key_code_mouse_location loc)
{
	return ((key & ~PANE_RANGES_LOCATION_MASK) |
	    ((key_code)loc << KEYC_MOUSE_LOCATION_SHIFT));
}

/* Get the newest range at a cell of a grid holding cells of a pane. */
struct pane_range *
pane_ranges_cell(struct window_pane *wp, struct grid *gd, u_int x, u_int y)
{
	struct grid_cell	  gc;
	struct pane_range	**ranges, *r = NULL, *text;
	u_int			  n;

	if (wp->base.ranges == NULL || y >= gd->hsize + gd->sy || x >= gd->sx)
		return (NULL);
	if (grid_get_line(gd, y)->extdsize != 0) {
		grid_get_cell(gd, x, y, &gc);
		if (gc.range != 0 &&
		    (ranges = pane_ranges_at(wp, gc.range, &n)) != NULL) {
			r = ranges[0];
			free(ranges);
		}
	}
	text = pane_ranges_text_cell(wp, gd, x, y);
	if (text != NULL && (r == NULL || text->id > r->id))
		return (text);
	return (r);
}

/* Get the newest range shown at a position in a pane. */
struct pane_range *
pane_ranges_visible(struct window_pane *wp, u_int x, u_int y)
{
	struct window_pane	*swp;
	struct grid		*gd;

	if (x >= wp->sx || y >= wp->sy)
		return (NULL);
	if (TAILQ_EMPTY(&wp->modes)) {
		gd = wp->base.grid;
		return (pane_ranges_cell(wp, gd, x, gd->hsize + y));
	}
	gd = window_copy_get_cell_grid(wp, &swp, &x, &y);
	if (gd == NULL)
		return (NULL);
	return (pane_ranges_cell(swp, gd, x, y));
}

/* Get the newest range under the mouse. */
struct pane_range *
pane_ranges_mouse(struct mouse_event *m)
{
	struct window_pane	*wp;
	u_int			 x, y;

	if (!m->valid)
		return (NULL);
	wp = cmd_mouse_pane(m, NULL, NULL);
	if (wp == NULL || cmd_mouse_at(wp, m, &x, &y, 0) != 0)
		return (NULL);
	return (pane_ranges_visible(wp, x, y));
}

/*
 * Change a mouse key in a pane to the Range location if the mouse is over a
 * range. The position is in the window.
 */
key_code
pane_ranges_mouse_key(struct window_pane *wp, key_code key, u_int px,
    u_int py)
{
	if (wp == NULL || !pane_ranges_is_mouse_key(key))
		return (key);
	if (pane_ranges_key_location(key) != KEYC_MOUSE_LOCATION_PANE)
		return (key);
	if (pane_ranges_visible(wp, px - wp->xoff, py - wp->yoff) == NULL)
		return (key);
	return (pane_ranges_key_set_location(key, KEYC_MOUSE_LOCATION_RANGE));
}

/* Get the Pane binding used for a Range mouse key that is not bound. */
struct key_binding *
pane_ranges_key_binding(struct key_table *table, key_code key)
{
	if (!pane_ranges_is_mouse_key(key))
		return (NULL);
	if (pane_ranges_key_location(key) != KEYC_MOUSE_LOCATION_RANGE)
		return (NULL);
	key = pane_ranges_key_set_location(key, KEYC_MOUSE_LOCATION_PANE);
	return (key_bindings_get(table, key));
}

/* Compare two ranges by ID. */
static int
pane_ranges_find_cmp(const void *a, const void *b)
{
	const struct pane_range	*r1 = *(struct pane_range *const *)a;
	const struct pane_range	*r2 = *(struct pane_range *const *)b;

	if (r1->id != r2->id)
		return (r1->id < r2->id ? -1 : 1);
	return (0);
}

/* State of a search for the next or previous range. */
struct pane_ranges_search {
	struct pane_range	**list;
	u_int			  nlist;
	u_char			 *done;
	const char		 *group;
	int			  next;
	int			  result;
	u_int			  rx;
	u_int			  ry;
};

/*
 * Consider a range found at a cell for the first time. Return 1 if the search
 * should stop.
 */
static int
pane_ranges_find_one(struct pane_ranges_search *search, struct pane_range *r,
    u_int x, u_int y, int after, int at)
{
	struct pane_range	**found;

	found = bsearch(&r, search->list, search->nlist, sizeof *search->list,
	    pane_ranges_find_cmp);
	if (found == NULL || search->done[found - search->list])
		return (0);
	search->done[found - search->list] = 1;
	if (search->group != NULL && strcmp(r->group, search->group) != 0)
		return (0);
	if (search->next && after) {
		search->rx = x;
		search->ry = y;
		search->result = 1;
		return (1);
	}
	if (!search->next && !after && !at) {
		search->rx = x;
		search->ry = y;
		search->result = 1;
	}
	return (0);
}

/*
 * Find the first cell of the next or previous range after or before a position
 * in a grid holding cells of a pane, optionally only ranges in a group. A
 * range starts at the first cell in the grid that carries it, or for a text
 * range the first cell of its text. Return 1 and change the position if one
 * is found.
 */
int
pane_ranges_find(struct window_pane *wp, struct grid *gd, u_int *px,
    u_int *py, int next, const char *group)
{
	struct pane_ranges_search  search;
	struct pane_range	 **list = NULL, **ranges, **text, *r;
	struct grid_line	  *gl;
	struct grid_cell	   gc;
	u_char			  *seen = NULL;
	uint64_t		   line;
	u_int			   nlist = 0, nseen = 0, ntext, n, i, x, y;
	u_int			   limit;
	int			   alternate, after, at, stop = 0;

	if (wp->base.ranges == NULL)
		return (0);
	for (r = pane_range_first(wp); r != NULL; r = pane_range_next(r)) {
		list = xreallocarray(list, nlist + 1, sizeof *list);
		list[nlist++] = r;
	}
	if (nlist == 0)
		return (0);
	qsort(list, nlist, sizeof *list, pane_ranges_find_cmp);

	memset(&search, 0, sizeof search);
	search.list = list;
	search.nlist = nlist;
	search.done = xcalloc(nlist, 1);
	search.group = group;
	search.next = next;
	alternate = SCREEN_IS_ALTERNATE(&wp->base);

	for (y = 0; !stop && y < gd->hsize + gd->sy; y++) {
		gl = grid_get_line(gd, y);
		text = pane_ranges_text_line(wp, gd, y, &line, &ntext);
		if (gl->extdsize == 0 && ntext == 0)
			continue;
		limit = gl->cellsize;
		if (ntext != 0 || limit > gd->sx)
			limit = gd->sx;
		for (x = 0; !stop && x < limit; x++) {
			after = (y > *py || (y == *py && x > *px));
			at = (y == *py && x == *px);
			for (i = 0; !stop && i < ntext; i++) {
				r = text[i];
				if (r->span.start_line != line ||
				    r->span.start_x != x ||
				    r->alternate != alternate)
					continue;
				stop = pane_ranges_find_one(&search, r, x, y,
				    after, at);
			}
			if (gl->extdsize == 0 || x >= gl->cellsize)
				continue;
			grid_get_cell(gd, x, y, &gc);
			if (gc.range == 0)
				continue;
			if (gc.range >= nseen) {
				seen = xrecallocarray(seen, nseen,
				    gc.range + 1, 1);
				nseen = gc.range + 1;
			} else if (seen[gc.range])
				continue;
			seen[gc.range] = 1;

			ranges = pane_ranges_at(wp, gc.range, &n);
			for (i = 0; !stop && i < n; i++) {
				stop = pane_ranges_find_one(&search, ranges[i],
				    x, y, after, at);
			}
			free(ranges);
			if (!next && (after || at))
				stop = 1;
		}
	}

	free(seen);
	free(search.done);
	free(list);
	if (search.result) {
		*px = search.rx;
		*py = search.ry;
	}
	return (search.result);
}
