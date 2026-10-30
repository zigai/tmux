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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Pane ranges. A range is stored in the grid: each covered cell carries the
 * number of a range set in its extended cell data, in the same way as an OSC 8
 * hyperlink. A range set is an interned, immutable, sorted list of range IDs,
 * so cells covered by several ranges share a number. Scrolling, history
 * trimming, line insertion and deletion, scroll regions, reflow and the
 * alternate screen all move cells and so carry the tag with them; a write
 * replaces the cell and drops it, except that an identical write keeps it (see
 * grid_set_cell). The state and position of each range are derived from the
 * tagged cells. History lines are never edited, only added at the end and
 * trimmed from the start, so the tagged cells in them are kept in an index of
 * records that is updated as history changes and rebuilt when the grid is
 * rearranged or tagged. The visible lines and the saved grid are scanned each
 * time.
 *
 * A range style is applied to copies of the cells when they are drawn, never
 * to the grid. The expanded style of each range and the combined style of each
 * range set are cached until the generation changes, which happens when any
 * range or its state changes or when the pane's styles may have changed.
 */

/* Interned set of range IDs. */
struct pane_range_set {
	u_int				 number;
	u_int				 count;
	u_int				*ids;

	struct grid_cell		 style;
	int				 styled;
	u_int				 style_generation;

	RB_ENTRY(pane_range_set)	 entry;
};
RB_HEAD(pane_range_sets, pane_range_set);
RB_HEAD(pane_range_tree, pane_range);
TAILQ_HEAD(pane_range_list, pane_range);

/* What was found in the cells of one range set during a scan. */
struct pane_range_scan {
	u_int			 stamp;
	u_int			 found;
	u_int			 found_saved;
	struct pane_range_span	 seen;
	int			 visible_y;
};

/* The cells of one range set on one line. */
struct pane_range_record {
	uint64_t		 line;
	u_int			 set;
	u_int			 count;
	u_int			 first_x;
	u_int			 last_x;
};

/* A list of records. */
struct pane_range_records {
	struct pane_range_record	*list;
	u_int				 n;
	u_int				 size;
};

/* Pane and grid state that decides whether a scan is needed. */
struct pane_ranges_signature {
	uint64_t		 output;
	pid_t			 pid;
	u_int			 sx;
	u_int			 sy;
	u_int			 hsize;
	uint64_t		 collected;
	u_int			 generation;
	int			 alternate;
};

/* Ranges of a pane. */
struct pane_ranges {
	struct window_pane		 *wp;
	struct pane_range_list		  list;
	struct pane_range_tree		  tree;
	u_int				  count;

	struct pane_range_set		**sets;
	u_int				  nsets;
	struct pane_range_sets		  by_ids;

	struct pane_range_scan		 *scans;
	u_int				  nscans;
	u_int				 *touched;
	u_int				  ntouched;
	u_int				  stamp;

	struct pane_ranges_signature	  signature;

	struct pane_range_records	  index;
	struct pane_ranges_signature	  index_signature;
	int				  index_dirty;
	struct pane_range_records	  live;

	u_int				  generation;
	int				  enabled;
	u_int				  enabled_generation;

	int				  listed;
	TAILQ_ENTRY(pane_ranges)	  entry;
};
TAILQ_HEAD(pane_ranges_list, pane_ranges);

static struct pane_ranges_list pane_ranges_list =
    TAILQ_HEAD_INITIALIZER(pane_ranges_list);
static u_int pane_range_next_id = 1;

static const char *pane_range_states[] = {
	"live", "modified", "suspended", "gone"
};

/* Compare two range sets. */
static int
pane_range_set_cmp(struct pane_range_set *set1, struct pane_range_set *set2)
{
	if (set1->count != set2->count)
		return (set1->count < set2->count ? -1 : 1);
	return (memcmp(set1->ids, set2->ids, set1->count * sizeof *set1->ids));
}
RB_PROTOTYPE_STATIC(pane_range_sets, pane_range_set, entry,
    pane_range_set_cmp);
RB_GENERATE_STATIC(pane_range_sets, pane_range_set, entry,
    pane_range_set_cmp);

/* Compare two ranges by ID. */
static int
pane_range_cmp(struct pane_range *r1, struct pane_range *r2)
{
	if (r1->id != r2->id)
		return (r1->id < r2->id ? -1 : 1);
	return (0);
}
RB_PROTOTYPE_STATIC(pane_range_tree, pane_range, tree_entry, pane_range_cmp);
RB_GENERATE_STATIC(pane_range_tree, pane_range, tree_entry, pane_range_cmp);

/* Check if a position is before another. */
static int
pane_range_before(uint64_t line1, u_int x1, uint64_t line2, u_int x2)
{
	if (line1 != line2)
		return (line1 < line2);
	return (x1 < x2);
}

/* Get the number of the set holding these IDs, creating it if necessary. */
static u_int
pane_ranges_intern(struct pane_ranges *prs, u_int *ids, u_int count)
{
	struct pane_range_set	 find, *set;

	if (count == 0)
		return (0);

	find.count = count;
	find.ids = ids;
	if ((set = RB_FIND(pane_range_sets, &prs->by_ids, &find)) != NULL)
		return (set->number);

	set = xcalloc(1, sizeof *set);
	set->count = count;
	set->ids = xcalloc(count, sizeof *set->ids);
	memcpy(set->ids, ids, count * sizeof *set->ids);

	prs->sets = xreallocarray(prs->sets, prs->nsets + 1, sizeof *prs->sets);
	prs->sets[prs->nsets++] = set;
	set->number = prs->nsets;
	RB_INSERT(pane_range_sets, &prs->by_ids, set);
	return (set->number);
}

/* Get the set made by adding or removing sorted IDs from a set. */
static u_int
pane_ranges_edit(struct pane_ranges *prs, u_int number, const u_int *ids,
    u_int nids, int add)
{
	struct pane_range_set	*set = NULL;
	u_int			*old = NULL, *new, i = 0, j = 0, n = 0;
	u_int			 nold = 0, result;

	if (number != 0) {
		set = prs->sets[number - 1];
		old = set->ids;
		nold = set->count;
	}
	new = xcalloc(nold + nids, sizeof *new);
	while (i < nold || j < nids) {
		if (j == nids || (i < nold && old[i] < ids[j]))
			new[n++] = old[i++];
		else if (i == nold || ids[j] < old[i]) {
			if (add)
				new[n++] = ids[j];
			j++;
		} else {
			if (add)
				new[n++] = old[i];
			i++;
			j++;
		}
	}
	result = pane_ranges_intern(prs, new, n);
	free(new);
	return (result);
}

/* Get the range set number of a cell. */
static u_int
pane_ranges_cell_set(struct grid_line *gl, u_int x)
{
	struct grid_cell_entry	*gce = &gl->celldata[x];

	if (~gce->flags & GRID_FLAG_EXTENDED || gce->offset >= gl->extdsize)
		return (0);
	return (gl->extddata[gce->offset].range);
}

/* Get the length of the text of a line. */
static u_int
pane_range_limit(struct grid *gd, u_int y)
{
	struct grid_line	*gl = grid_get_line(gd, y);
	u_int			 limit;

	if (gl->flags & GRID_LINE_WRAPPED) {
		limit = gl->cellused;
		if (limit > gl->cellsize)
			limit = gl->cellsize;
		if (limit > gd->sx)
			limit = gd->sx;
		return (limit);
	}
	return (grid_line_length(gd, y));
}

/* Append to a buffer. */
static void
pane_range_append(char **buf, size_t *len, size_t *size, const char *s,
    size_t n)
{
	while (*len + n + 1 > *size) {
		*size *= 2;
		*buf = xrealloc(*buf, *size);
	}
	memcpy(*buf + *len, s, n);
	*len += n;
}

/*
 * Get the text of part of a grid as copy mode copies it: wrapped lines are
 * joined, padding is omitted and trailing spaces are trimmed on lines that are
 * not wrapped.
 */
char *
pane_range_text(struct grid *gd, u_int sy, u_int sx, u_int ey, u_int ex)
{
	struct grid_line	*gl;
	struct grid_cell	 gc;
	char			*buf;
	const char		*data, *acs;
	size_t			 len = 0, size = 256, n;
	u_int			 y, x, first, last, limit;

	buf = xmalloc(size);
	for (y = sy; y <= ey; y++) {
		gl = grid_get_line(gd, y);
		limit = pane_range_limit(gd, y);
		first = (y == sy) ? sx : 0;
		last = (y == ey && ex + 1 < limit) ? ex + 1 : limit;
		for (x = first; x < last; x++) {
			grid_get_cell(gd, x, y, &gc);
			if (gc.flags & GRID_FLAG_PADDING)
				continue;
			data = gc.data.data;
			n = gc.data.size;
			if (gc.flags & GRID_FLAG_TAB) {
				data = "\t";
				n = 1;
			} else if (n == 1 && (gc.attr & GRID_ATTR_CHARSET)) {
				acs = tty_acs_get(NULL, *data);
				if (acs != NULL) {
					data = acs;
					n = strlen(acs);
				}
			}
			pane_range_append(&buf, &len, &size, data, n);
		}
		if (y != ey && (~gl->flags & GRID_LINE_WRAPPED))
			pane_range_append(&buf, &len, &size, "\n", 1);
	}
	buf[len] = '\0';
	return (buf);
}

/* Get the pane ranges, creating them if necessary. */
static struct pane_ranges *
pane_ranges_get(struct window_pane *wp)
{
	struct pane_ranges	*prs = wp->base.ranges;

	if (prs != NULL)
		return (prs);

	prs = wp->base.ranges = xcalloc(1, sizeof *prs);
	prs->wp = wp;
	prs->generation = 1;
	TAILQ_INIT(&prs->list);
	RB_INIT(&prs->tree);
	RB_INIT(&prs->by_ids);
	return (prs);
}

/* Redraw a pane with new range styles. */
static void
pane_ranges_restyle(struct pane_ranges *prs)
{
	prs->generation++;
	prs->wp->flags |= (PANE_REDRAW|PANE_STYLECHANGED);
}

/* Get the signature of a pane. */
static void
pane_ranges_signature(struct window_pane *wp,
    struct pane_ranges_signature *signature)
{
	struct grid	*gd = wp->base.grid;

	memset(signature, 0, sizeof *signature);
	signature->output = wp->output_generation;
	signature->pid = wp->pid;
	signature->sx = gd->sx;
	signature->sy = gd->sy;
	signature->hsize = gd->hsize;
	signature->collected = gd->scroll_collected;
	signature->generation = gd->scroll_generation;
	signature->alternate = SCREEN_IS_ALTERNATE(&wp->base);
}

/* Make sure a pane is checked while it has ranges. */
static void
pane_ranges_list_add(struct pane_ranges *prs)
{
	if (!prs->listed) {
		TAILQ_INSERT_TAIL(&pane_ranges_list, prs, entry);
		prs->listed = 1;
	}
}

/* Free the ranges of a screen. */
void
pane_ranges_free(struct screen *s)
{
	struct pane_ranges	*prs = s->ranges;
	struct pane_range	*r;
	u_int			 i;

	if (prs == NULL)
		return;
	if (prs->listed)
		TAILQ_REMOVE(&pane_ranges_list, prs, entry);

	while ((r = TAILQ_FIRST(&prs->list)) != NULL) {
		TAILQ_REMOVE(&prs->list, r, entry);
		free(r->name);
		free(r->group);
		free(r->text);
		free(r->style);
		free(r);
	}
	for (i = 0; i < prs->nsets; i++) {
		free(prs->sets[i]->ids);
		free(prs->sets[i]);
	}
	free(prs->sets);
	free(prs->scans);
	free(prs->touched);
	free(prs->index.list);
	free(prs->live.list);
	free(prs);
	s->ranges = NULL;
}

/* Get the first range of a pane. */
struct pane_range *
pane_range_first(struct window_pane *wp)
{
	if (wp->base.ranges == NULL)
		return (NULL);
	return (TAILQ_FIRST(&wp->base.ranges->list));
}

/* Get the next range of a pane. */
struct pane_range *
pane_range_next(struct pane_range *r)
{
	return (TAILQ_NEXT(r, entry));
}

/* Get a range by ID. */
struct pane_range *
pane_range_get(struct window_pane *wp, u_int id)
{
	struct pane_range	find;

	if (wp->base.ranges == NULL)
		return (NULL);
	find.id = id;
	return (RB_FIND(pane_range_tree, &wp->base.ranges->tree, &find));
}

/* Get the ranges in a range set, newest first. */
struct pane_range **
pane_ranges_at(struct window_pane *wp, u_int number, u_int *n)
{
	struct pane_ranges	 *prs = wp->base.ranges;
	struct pane_range_set	 *set;
	struct pane_range	**ranges, *r;
	u_int			  i;

	*n = 0;
	if (prs == NULL || number == 0 || number > prs->nsets)
		return (NULL);
	set = prs->sets[number - 1];

	ranges = xcalloc(set->count, sizeof *ranges);
	for (i = set->count; i > 0; i--) {
		r = pane_range_get(wp, set->ids[i - 1]);
		if (r != NULL)
			ranges[(*n)++] = r;
	}
	if (*n == 0) {
		free(ranges);
		return (NULL);
	}
	return (ranges);
}

/* Count the ranges of a pane that are not gone. */
u_int
pane_ranges_count(struct window_pane *wp)
{
	struct pane_range	*r;
	u_int			 n = 0;

	for (r = pane_range_first(wp); r != NULL; r = pane_range_next(r)) {
		if (r->state != PANE_RANGE_GONE)
			n++;
	}
	return (n);
}

/* Add the records of the tagged cells in a line to a list. */
static void
pane_ranges_line_records(struct pane_ranges *prs, struct grid *gd, u_int y,
    uint64_t line, struct pane_range_records *records)
{
	struct grid_line		*gl = grid_get_line(gd, y);
	struct pane_range_record	*rec;
	u_int				 x, i, first, limit, set;

	if (gl->extdsize == 0)
		return;
	limit = gl->cellsize;
	if (limit > gd->sx)
		limit = gd->sx;

	first = records->n;
	for (x = 0; x < limit; x++) {
		set = pane_ranges_cell_set(gl, x);
		if (set == 0 || set > prs->nsets)
			continue;

		for (i = records->n; i > first; i--) {
			if (records->list[i - 1].set == set)
				break;
		}
		if (i != first) {
			rec = &records->list[i - 1];
			rec->count++;
			rec->last_x = x;
			continue;
		}

		if (records->n == records->size) {
			records->size = records->size == 0 ? 64 :
			    records->size * 2;
			records->list = xreallocarray(records->list,
			    records->size, sizeof *records->list);
		}
		rec = &records->list[records->n++];
		rec->line = line;
		rec->set = set;
		rec->count = 1;
		rec->first_x = rec->last_x = x;
	}
}

/* Bring the index of the history up to date. */
static void
pane_ranges_index_update(struct pane_ranges *prs)
{
	struct window_pane		*wp = prs->wp;
	struct grid			*gd = wp->base.grid;
	struct pane_ranges_signature	*old = &prs->index_signature;
	struct pane_ranges_signature	 signature;
	struct pane_range_records	*index = &prs->index;
	uint64_t			 end = old->collected + old->hsize;
	u_int				 y, lo, hi, mid;

	pane_ranges_signature(wp, &signature);
	if (prs->index_dirty ||
	    signature.generation != old->generation ||
	    signature.sx != old->sx ||
	    signature.sy != old->sy ||
	    signature.pid != old->pid ||
	    signature.alternate != old->alternate ||
	    signature.collected + signature.hsize < end) {
		index->n = 0;
		y = 0;
	} else {
		lo = 0;
		hi = index->n;
		while (lo < hi) {
			mid = lo + (hi - lo) / 2;
			if (index->list[mid].line < gd->scroll_collected)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo != 0) {
			memmove(index->list, &index->list[lo],
			    (index->n - lo) * sizeof *index->list);
			index->n -= lo;
		}

		y = 0;
		if (end > gd->scroll_collected)
			y = end - gd->scroll_collected;
	}
	for (; y < gd->hsize; y++) {
		pane_ranges_line_records(prs, gd, y, gd->scroll_collected + y,
		    index);
	}

	prs->index_signature = signature;
	prs->index_dirty = 0;
}

/* Add up what is found in a list of records. */
static void
pane_ranges_found(struct pane_ranges *prs,
    const struct pane_range_records *records, int saved, int visible)
{
	struct grid			*gd = prs->wp->base.grid;
	struct pane_range_record	*rec;
	struct pane_range_scan		*scan;
	u_int				 i;

	for (i = 0; i < records->n; i++) {
		rec = &records->list[i];
		scan = &prs->scans[rec->set];
		if (scan->stamp != prs->stamp) {
			memset(scan, 0, sizeof *scan);
			scan->stamp = prs->stamp;
			scan->visible_y = -1;
			prs->touched[prs->ntouched++] = rec->set;
		}
		if (saved) {
			scan->found_saved += rec->count;
			continue;
		}
		if (scan->found == 0) {
			scan->seen.start_line = rec->line;
			scan->seen.start_x = rec->first_x;
		}
		scan->found += rec->count;
		scan->seen.end_line = rec->line;
		scan->seen.end_x = rec->last_x;
		if (visible && scan->visible_y == -1) {
			scan->visible_y = rec->line - gd->scroll_collected -
			    gd->hsize;
		}
	}
}

/* Scan the cells that may be tagged and add up what was found per range. */
static void
pane_ranges_scan(struct pane_ranges *prs)
{
	struct window_pane	*wp = prs->wp;
	struct grid		*gd = wp->base.grid;
	struct grid		*saved = wp->base.saved_grid;
	struct pane_range_scan	*scan;
	struct pane_range_set	*set;
	struct pane_range	*r, find;
	u_int			 y, i, j;

	if (prs->nscans < prs->nsets + 1) {
		prs->scans = xrecallocarray(prs->scans, prs->nscans,
		    prs->nsets + 1, sizeof *prs->scans);
		prs->touched = xreallocarray(prs->touched, prs->nsets + 1,
		    sizeof *prs->touched);
		prs->nscans = prs->nsets + 1;
	}
	prs->stamp++;
	prs->ntouched = 0;

	pane_ranges_index_update(prs);
	pane_ranges_found(prs, &prs->index, 0, 0);

	prs->live.n = 0;
	for (y = gd->hsize; y < gd->hsize + gd->sy; y++) {
		pane_ranges_line_records(prs, gd, y, gd->scroll_collected + y,
		    &prs->live);
	}
	pane_ranges_found(prs, &prs->live, 0, 1);

	prs->live.n = 0;
	if (saved != NULL) {
		for (y = 0; y < saved->sy; y++) {
			pane_ranges_line_records(prs, saved, saved->hsize + y,
			    0, &prs->live);
		}
	}
	pane_ranges_found(prs, &prs->live, 1, 0);

	TAILQ_FOREACH(r, &prs->list, entry) {
		r->found = r->found_saved = 0;
		r->visible_y = -1;
	}
	for (i = 0; i < prs->ntouched; i++) {
		scan = &prs->scans[prs->touched[i]];
		set = prs->sets[prs->touched[i] - 1];

		for (j = 0; j < set->count; j++) {
			find.id = set->ids[j];
			r = RB_FIND(pane_range_tree, &prs->tree, &find);
			if (r == NULL)
				continue;
			r->found_saved += scan->found_saved;
			if (scan->found == 0)
				continue;
			if (r->found == 0 || pane_range_before(
			    scan->seen.start_line, scan->seen.start_x,
			    r->seen.start_line, r->seen.start_x)) {
				r->seen.start_line = scan->seen.start_line;
				r->seen.start_x = scan->seen.start_x;
			}
			if (r->found == 0 || pane_range_before(
			    r->seen.end_line, r->seen.end_x,
			    scan->seen.end_line, scan->seen.end_x)) {
				r->seen.end_line = scan->seen.end_line;
				r->seen.end_x = scan->seen.end_x;
			}
			r->found += scan->found;
			if (scan->visible_y != -1 && (r->visible_y == -1 ||
			    scan->visible_y < r->visible_y))
				r->visible_y = scan->visible_y;
		}
	}
}

/* Work out the state of a range from what the scan found. */
static void
pane_ranges_derive(struct pane_ranges *prs, struct pane_range *r,
    uint64_t stable)
{
	struct window_pane	*wp = prs->wp;
	struct grid		*gd = wp->base.grid;
	char			*text;
	int			 same;

	if (SCREEN_IS_ALTERNATE(&wp->base) && r->found_saved != 0) {
		r->state = PANE_RANGE_SUSPENDED;
		r->visible_y = -1;
		return;
	}
	if (r->found == 0) {
		r->state = PANE_RANGE_GONE;
		r->positioned = 0;
		r->visible_y = -1;
		return;
	}

	same = (r->positioned &&
	    r->span.start_line == r->seen.start_line &&
	    r->span.start_x == r->seen.start_x &&
	    r->span.end_line == r->seen.end_line &&
	    r->span.end_x == r->seen.end_x);
	if (r->found == r->cells && same && r->state == PANE_RANGE_LIVE &&
	    r->span.end_line < stable)
		return;

	r->span = r->seen;
	r->positioned = 1;
	r->state = PANE_RANGE_MODIFIED;
	if (r->found != r->cells)
		return;
	text = pane_range_text(gd, r->span.start_line - gd->scroll_collected,
	    r->span.start_x, r->span.end_line - gd->scroll_collected,
	    r->span.end_x);
	if (strcmp(text, r->text) == 0)
		r->state = PANE_RANGE_LIVE;
	free(text);
}

/* Derive the ranges of a pane and say if any state or position changed. */
static int
pane_ranges_derive_all(struct pane_ranges *prs, uint64_t stable)
{
	struct pane_range	*r;
	struct pane_range_span	 span;
	enum pane_range_state	 old;
	int			 changed = 0;

	TAILQ_FOREACH(r, &prs->list, entry) {
		old = r->state;
		span = r->span;
		pane_ranges_derive(prs, r, stable);
		if (r->state != old ||
		    span.start_line != r->span.start_line ||
		    span.start_x != r->span.start_x ||
		    span.end_line != r->span.end_line ||
		    span.end_x != r->span.end_x)
			changed = 1;
	}
	return (changed);
}

/* Update the states of the ranges of a pane if it has changed. */
void
pane_ranges_update(struct window_pane *wp)
{
	struct pane_ranges		*prs = wp->base.ranges;
	struct pane_ranges_signature	 signature, *old;
	uint64_t			 stable = 0;

	if (prs == NULL || prs->count == 0)
		return;

	pane_ranges_signature(wp, &signature);
	old = &prs->signature;
	if (memcmp(&signature, old, sizeof signature) == 0)
		return;
	if (signature.generation == old->generation &&
	    signature.sx == old->sx &&
	    signature.sy == old->sy &&
	    signature.alternate == old->alternate) {
		stable = old->collected + old->hsize;
		if (stable > signature.collected + signature.hsize)
			stable = signature.collected + signature.hsize;
	}
	prs->signature = signature;

	pane_ranges_scan(prs);
	if (pane_ranges_derive_all(prs, stable)) {
		pane_ranges_restyle(prs);
		events_fire_pane("pane-ranges-changed", wp);
	}
}

/* Update the ranges of every pane that has them. */
void
pane_ranges_check(void)
{
	struct pane_ranges	*prs, *prs1;

	TAILQ_FOREACH_SAFE(prs, &pane_ranges_list, entry, prs1)
		pane_ranges_update(prs->wp);
}

/*
 * Tag the cells of a span with a range, or just count them if dry is set. The
 * span is widened to take in padding cells. Return the number of cells and
 * the first and last of them.
 */
static u_int
pane_ranges_tag(struct pane_ranges *prs, const struct pane_range_span *span,
    u_int id, int dry, struct pane_range_span *found, int *visible_y)
{
	struct grid		*gd = prs->wp->base.grid;
	struct grid_line	*gl;
	struct grid_cell	 gc;
	u_int			 y, y0, y1, x, x0, x1, limit, n = 0;
	u_int			 old, new = 0, last = 0;
	int			 cached = 0;

	y0 = span->start_line - gd->scroll_collected;
	y1 = span->end_line - gd->scroll_collected;
	x0 = span->start_x;
	x1 = span->end_x;

	grid_get_cell(gd, x0, y0, &gc);
	if (x0 != 0 && (gc.flags & GRID_FLAG_PADDING))
		x0--;
	while (x1 + 1 < gd->sx) {
		grid_get_cell(gd, x1 + 1, y1, &gc);
		if (~gc.flags & GRID_FLAG_PADDING)
			break;
		x1++;
	}

	if (!dry)
		prs->index_dirty = 1;

	*visible_y = -1;
	for (y = y0; y <= y1; y++) {
		gl = grid_get_line(gd, y);
		limit = pane_range_limit(gd, y);
		for (x = (y == y0 ? x0 : 0); x < limit; x++) {
			if (y == y1 && x > x1)
				break;
			if (n++ == 0) {
				found->start_line = gd->scroll_collected + y;
				found->start_x = x;
			}
			found->end_line = gd->scroll_collected + y;
			found->end_x = x;
			if (*visible_y == -1 && y >= gd->hsize)
				*visible_y = y - gd->hsize;
			if (dry)
				continue;

			old = pane_ranges_cell_set(gl, x);
			if (!cached || old != last) {
				new = pane_ranges_edit(prs, old, &id, 1, 1);
				last = old;
				cached = 1;
			}
			if (new != old)
				grid_set_range(gd, x, y, new);
		}
	}
	return (n);
}

/* Tag the cells of a span in another grid holding copies of the pane cells. */
static void
pane_ranges_tag_copy(struct pane_ranges *prs, struct grid *gd,
    uint64_t collected, const struct pane_range_span *span, u_int id)
{
	struct grid_line	*gl;
	u_int			 y, y0, y1, x, limit, old, new = 0, last = 0;
	int			 cached = 0;

	if (span->start_line < collected ||
	    span->end_line - collected >= gd->hsize + gd->sy)
		return;
	y0 = span->start_line - collected;
	y1 = span->end_line - collected;

	for (y = y0; y <= y1; y++) {
		gl = grid_get_line(gd, y);
		limit = pane_range_limit(gd, y);
		for (x = (y == y0 ? span->start_x : 0); x < limit; x++) {
			if (y == y1 && x > span->end_x)
				break;
			old = pane_ranges_cell_set(gl, x);
			if (!cached || old != last) {
				new = pane_ranges_edit(prs, old, &id, 1, 1);
				last = old;
				cached = 1;
			}
			if (new != old)
				grid_set_range(gd, x, y, new);
		}
	}
}

/* Remove sorted IDs from the cells of a line. */
static void
pane_ranges_untag_line(struct pane_ranges *prs, struct grid *gd, u_int y,
    const u_int *ids, u_int nids, u_int *map, u_int nmap)
{
	struct grid_line	*gl = grid_get_line(gd, y);
	u_int			 x, limit, set;

	if (gl->extdsize == 0)
		return;
	limit = gl->cellsize;
	if (limit > gd->sx)
		limit = gd->sx;

	for (x = 0; x < limit; x++) {
		set = pane_ranges_cell_set(gl, x);
		if (set == 0 || set >= nmap)
			continue;
		if (map[set] == 0)
			map[set] = pane_ranges_edit(prs, set, ids, nids, 0) + 1;
		if (map[set] - 1 != set)
			grid_set_range(gd, x, y, map[set] - 1);
	}
}

/* Remove sorted IDs from every cell that carries them. */
static void
pane_ranges_untag(struct pane_ranges *prs, const u_int *ids, u_int nids)
{
	struct window_pane	*wp = prs->wp;
	struct grid		*gd = wp->base.grid;
	struct grid		*saved = wp->base.saved_grid;
	struct grid		*copy;
	u_int			*map, nmap = prs->nsets + 1;
	u_int			 i, y, set;
	uint64_t		 last = 0, collected;
	int			 done = 0, current;

	map = xcalloc(nmap, sizeof *map);
	pane_ranges_index_update(prs);
	for (i = 0; i < prs->index.n; i++) {
		set = prs->index.list[i].set;
		if (map[set] == 0)
			map[set] = pane_ranges_edit(prs, set, ids, nids, 0) + 1;
		if (map[set] - 1 == set)
			continue;
		if (done && prs->index.list[i].line == last)
			continue;
		last = prs->index.list[i].line;
		done = 1;
		pane_ranges_untag_line(prs, gd, last - gd->scroll_collected,
		    ids, nids, map, nmap);
	}
	for (y = gd->hsize; y < gd->hsize + gd->sy; y++)
		pane_ranges_untag_line(prs, gd, y, ids, nids, map, nmap);
	if (saved != NULL) {
		for (y = 0; y < saved->sy; y++) {
			pane_ranges_untag_line(prs, saved, saved->hsize + y,
			    ids, nids, map, nmap);
		}
	}
	copy = window_copy_get_backing(wp, &collected, &current);
	if (copy != NULL) {
		for (y = 0; y < copy->hsize + copy->sy; y++) {
			pane_ranges_untag_line(prs, copy, y, ids, nids, map,
			    nmap);
		}
	}
	prs->index_dirty = 1;
	free(map);
}

/* Check that a span is inside the grid. */
static int
pane_range_check(struct window_pane *wp, const struct pane_range_span *span,
    char **cause)
{
	struct grid	*gd = wp->base.grid;

	if (span->start_line > span->end_line ||
	    (span->start_line == span->end_line &&
	     span->start_x > span->end_x) ||
	    span->start_line < gd->scroll_collected ||
	    span->end_line >= gd->scroll_collected + gd->hsize + gd->sy ||
	    span->start_x >= gd->sx ||
	    span->end_x >= gd->sx) {
		xasprintf(cause, "range outside grid");
		return (0);
	}
	return (1);
}

/* Parse a range as SL:SX-EL:EX. */
int
pane_range_parse(struct window_pane *wp, const char *value,
    struct pane_range_span *span, char **cause)
{
	char		*copy, *p, *token;
	const char	*error;
	long long	 n[4];
	u_int		 i;

	p = copy = xstrdup(value);
	for (i = 0; i < 4; i++) {
		token = strsep(&p, i == 1 ? "-" : ":");
		if (token == NULL || *token == '\0')
			break;
		n[i] = strtonum(token, 0, i % 2 ? UINT_MAX : LLONG_MAX,
		    &error);
		if (error != NULL)
			break;
	}
	free(copy);
	if (i != 4 || p != NULL) {
		xasprintf(cause, "invalid range: %s", value);
		return (0);
	}
	span->start_line = n[0];
	span->start_x = n[1];
	span->end_line = n[2];
	span->end_x = n[3];
	return (pane_range_check(wp, span, cause));
}

/* Set the cells, text and position of a range from a span. */
static int
pane_range_apply(struct pane_ranges *prs, struct pane_range *r,
    const struct pane_range_span *span, char **cause)
{
	struct grid		*gd = prs->wp->base.grid;
	struct grid		*copy;
	struct pane_range_span	 found;
	uint64_t		 collected;
	int			 visible_y, current;
	u_int			 n;

	if (pane_ranges_tag(prs, span, r->id, 1, &found, &visible_y) == 0) {
		xasprintf(cause, "range has no text");
		return (0);
	}
	n = pane_ranges_tag(prs, span, r->id, 0, &found, &visible_y);
	copy = window_copy_get_backing(prs->wp, &collected, &current);
	if (copy != NULL && current)
		pane_ranges_tag_copy(prs, copy, collected, &found, r->id);

	free(r->text);
	r->text = pane_range_text(gd, found.start_line - gd->scroll_collected,
	    found.start_x, found.end_line - gd->scroll_collected, found.end_x);
	r->cells = n;
	r->state = PANE_RANGE_LIVE;
	r->positioned = 1;
	r->span = found;
	r->visible_y = visible_y;
	pane_ranges_restyle(prs);
	return (1);
}

/* Create a range. */
struct pane_range *
pane_range_create(struct window_pane *wp, const struct pane_range_span *span,
    const char *name, const char *group, const char *style, char **cause)
{
	struct pane_ranges	*prs = pane_ranges_get(wp);
	struct pane_range	*r;

	pane_ranges_update(wp);
	if (pane_range_next_id == UINT_MAX) {
		xasprintf(cause, "range ids exhausted");
		return (NULL);
	}

	r = xcalloc(1, sizeof *r);
	r->id = pane_range_next_id;
	r->name = xstrdup(name);
	r->group = xstrdup(group);
	if (style != NULL && *style != '\0')
		r->style = xstrdup(style);
	if (!pane_range_apply(prs, r, span, cause)) {
		free(r->name);
		free(r->group);
		free(r->style);
		free(r);
		if (prs->count == 0 && prs->nsets == 0)
			pane_ranges_free(&wp->base);
		return (NULL);
	}
	pane_range_next_id++;

	TAILQ_INSERT_TAIL(&prs->list, r, entry);
	RB_INSERT(pane_range_tree, &prs->tree, r);
	prs->count++;
	if (prs->count == 1)
		pane_ranges_signature(wp, &prs->signature);
	pane_ranges_list_add(prs);
	return (r);
}

/* Move a range to different cells. */
int
pane_range_move(struct window_pane *wp, struct pane_range *r,
    const struct pane_range_span *span, char **cause)
{
	struct pane_ranges	*prs = wp->base.ranges;
	struct pane_range_span	 found;
	int			 visible_y;

	pane_ranges_update(wp);
	if (pane_ranges_tag(prs, span, r->id, 1, &found, &visible_y) == 0) {
		xasprintf(cause, "range has no text");
		return (0);
	}
	pane_ranges_untag(prs, &r->id, 1);
	return (pane_range_apply(prs, r, span, cause));
}

/* Remove a range from the list and free it. */
static void
pane_range_remove(struct pane_ranges *prs, struct pane_range *r)
{
	TAILQ_REMOVE(&prs->list, r, entry);
	RB_REMOVE(pane_range_tree, &prs->tree, r);
	free(r->name);
	free(r->group);
	free(r->text);
	free(r->style);
	free(r);

	if (--prs->count == 0 && prs->listed) {
		TAILQ_REMOVE(&pane_ranges_list, prs, entry);
		prs->listed = 0;
		free(prs->index.list);
		memset(&prs->index, 0, sizeof prs->index);
		prs->index_dirty = 1;
	}
}

/* Kill a range. */
void
pane_range_kill(struct window_pane *wp, struct pane_range *r)
{
	struct pane_ranges	*prs = wp->base.ranges;
	u_int			 id = r->id;

	pane_ranges_update(wp);
	pane_ranges_untag(prs, &id, 1);
	pane_range_remove(prs, r);
	pane_ranges_restyle(prs);
}

/* Kill all the ranges in a group. */
void
pane_ranges_kill_group(struct window_pane *wp, const char *group)
{
	struct pane_ranges	*prs = wp->base.ranges;
	struct pane_range	*r, *r1;
	u_int			*ids, n = 0;

	if (prs == NULL)
		return;
	pane_ranges_update(wp);

	ids = xcalloc(prs->count + 1, sizeof *ids);
	TAILQ_FOREACH(r, &prs->list, entry) {
		if (strcmp(r->group, group) == 0)
			ids[n++] = r->id;
	}
	if (n != 0) {
		pane_ranges_untag(prs, ids, n);
		pane_ranges_restyle(prs);
	}
	TAILQ_FOREACH_SAFE(r, &prs->list, entry, r1) {
		if (strcmp(r->group, group) == 0)
			pane_range_remove(prs, r);
	}
	free(ids);
}

/* Add the formats of a range. */
void
pane_range_formats(struct format_tree *ft, struct pane_range *r)
{
	format_add(ft, "range_id", "%u", r->id);
	format_add(ft, "range_name", "%s", r->name);
	format_add(ft, "range_group", "%s", r->group);
	format_add(ft, "range_state", "%s", pane_range_states[r->state]);
	format_add(ft, "range_text", "%s", r->text);
	format_add(ft, "range_style", "%s", r->style == NULL ? "" : r->style);
	if (r->positioned) {
		format_add(ft, "range_start_line", "%llu",
		    (unsigned long long)r->span.start_line);
		format_add(ft, "range_start_x", "%u", r->span.start_x);
		format_add(ft, "range_end_line", "%llu",
		    (unsigned long long)r->span.end_line);
		format_add(ft, "range_end_x", "%u", r->span.end_x);
	} else {
		format_add(ft, "range_start_line", "%s", "");
		format_add(ft, "range_start_x", "%s", "");
		format_add(ft, "range_end_line", "%s", "");
		format_add(ft, "range_end_x", "%s", "");
	}
	if (r->visible_y != -1)
		format_add(ft, "range_visible_y", "%d", r->visible_y);
	else
		format_add(ft, "range_visible_y", "%s", "");
}

/* Change the name of a range. */
void
pane_range_set_name(struct window_pane *wp, struct pane_range *r,
    const char *name)
{
	free(r->name);
	r->name = xstrdup(name);
	pane_ranges_restyle(wp->base.ranges);
}

/* Change the style of a range, or use the default style if it is empty. */
void
pane_range_set_style(struct window_pane *wp, struct pane_range *r,
    const char *style)
{
	free(r->style);
	r->style = NULL;
	if (*style != '\0')
		r->style = xstrdup(style);
	pane_ranges_restyle(wp->base.ranges);
}

/* Note that the styles of a pane may have changed. */
void
pane_ranges_style_changed(struct window_pane *wp)
{
	if (wp->base.ranges != NULL)
		wp->base.ranges->generation++;
}

/* Get the style of a range in its current state. */
static const struct grid_cell *
pane_range_style(struct pane_ranges *prs, struct pane_range *r)
{
	struct window_pane	*wp = prs->wp;
	struct format_tree	*ft;
	struct style		 sy, *osy;
	char			*expanded;

	if (r->style_generation == prs->generation)
		return (&r->style_cell);

	ft = format_create(NULL, NULL, FORMAT_PANE|wp->id, FORMAT_NOJOBS);
	format_defaults(ft, NULL, NULL, NULL, wp);
	pane_range_formats(ft, r);
	style_set(&sy, &grid_default_cell);
	if (r->style != NULL) {
		expanded = format_expand(ft, r->style);
		if (style_parse(&sy, &grid_default_cell, expanded) != 0)
			style_set(&sy, &grid_default_cell);
		free(expanded);
	} else {
		osy = options_string_to_style(wp->options, "pane-range-style",
		    ft);
		if (osy != NULL)
			memcpy(&sy, osy, sizeof sy);
	}
	format_free(ft);

	memcpy(&r->style_cell, &sy.gc, sizeof r->style_cell);
	r->style_generation = prs->generation;
	return (&r->style_cell);
}

/* Combine the styles of the ranges in a set, newest last. */
static void
pane_ranges_set_style(struct pane_ranges *prs, struct pane_range_set *set)
{
	struct grid_cell	*sc = &set->style;
	const struct grid_cell	*gc;
	struct pane_range	*r, find;
	u_int			 i;

	memcpy(sc, &grid_default_cell, sizeof *sc);
	for (i = 0; i < set->count; i++) {
		find.id = set->ids[i];
		r = RB_FIND(pane_range_tree, &prs->tree, &find);
		if (r == NULL)
			continue;
		gc = pane_range_style(prs, r);
		if (gc->fg != 8)
			sc->fg = gc->fg;
		if (gc->bg != 8)
			sc->bg = gc->bg;
		if (gc->us != 8)
			sc->us = gc->us;
		if (gc->attr & GRID_ATTR_NOATTR)
			sc->attr = gc->attr;
		else
			sc->attr |= gc->attr;
	}
	set->styled = (sc->fg != 8 || sc->bg != 8 || sc->us != 8 ||
	    sc->attr != 0);
	set->style_generation = prs->generation;
}

/*
 * Get the ranges of a pane if their styles should be drawn on a screen, which
 * must be the pane's own screen.
 */
struct pane_ranges *
pane_ranges_drawing(struct window_pane *wp, struct screen *s)
{
	struct pane_ranges	*prs = wp->base.ranges;

	if (prs == NULL || prs->count == 0 || s != &wp->base)
		return (NULL);
	if (prs->enabled_generation != prs->generation) {
		prs->enabled = options_get_number(wp->options, "pane-ranges");
		prs->enabled_generation = prs->generation;
	}
	if (!prs->enabled)
		return (NULL);
	return (prs);
}

/* Apply the style of the ranges of a cell to a copy of it. */
void
pane_ranges_draw_cell(struct pane_ranges *prs, struct grid_cell *gc)
{
	struct pane_range_set	*set;
	struct grid_cell	*sc;

	if (gc->range == 0 || gc->range > prs->nsets)
		return;
	set = prs->sets[gc->range - 1];
	if (set->style_generation != prs->generation)
		pane_ranges_set_style(prs, set);
	if (!set->styled)
		return;

	sc = &set->style;
	if (sc->fg != 8)
		gc->fg = sc->fg;
	if (sc->bg != 8)
		gc->bg = sc->bg;
	if (sc->us != 8)
		gc->us = sc->us;
	if (sc->attr & GRID_ATTR_NOATTR) {
		gc->attr = (sc->attr & ~GRID_ATTR_NOATTR) |
		    (gc->attr & GRID_ATTR_CHARSET);
	} else
		gc->attr |= sc->attr;
}

/*
 * Apply the style of the ranges of a cell copied from a pane into a mode
 * screen and remove the ranges from it.
 */
void
pane_ranges_copy_cell(struct window_pane *wp, struct grid_cell *gc)
{
	struct pane_ranges	*prs;

	prs = pane_ranges_drawing(wp, &wp->base);
	if (prs != NULL)
		pane_ranges_draw_cell(prs, gc);
	gc->range = 0;
}

/*
 * Redraw cells just written to the terminal if any of them is in a range, so
 * a cell rewritten without change keeps its range style.
 */
int
pane_ranges_redraw_cells(struct tty *tty, const struct tty_ctx *ctx, u_int n)
{
	struct grid		*gd = ctx->s->grid;
	struct grid_line	*gl;
	struct tty_ctx		 rctx;
	u_int			 x, end;

	gl = grid_get_line(gd, gd->hsize + ctx->ocy);
	if (gl->extdsize == 0)
		return (0);
	end = ctx->ocx + n;
	if (end > gl->cellsize)
		end = gl->cellsize;
	for (x = ctx->ocx; x < end; x++) {
		if (pane_ranges_cell_set(gl, x) != 0)
			break;
	}
	if (x >= end)
		return (0);

	memcpy(&rctx, ctx, sizeof rctx);
	rctx.n = n;
	tty_cmd_redrawline(tty, &rctx);
	return (1);
}
