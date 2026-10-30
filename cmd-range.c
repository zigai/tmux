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
 * Create, change, kill and list pane ranges.
 */

static enum cmd_retval	cmd_new_range_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_set_range_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_kill_range_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_list_ranges_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_new_range_entry = {
	.name = "new-range",
	.alias = NULL,

	.args = { "a:b:e:PF:t:n:g:m:Sr:", 0, 0, NULL },
	.usage = "[-P] [-a anchor] [-b before] [-e after] [-F format] "
		"[-t target-pane] [-n name] [-g group] "
		"(-S | -r range | -m text)",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_new_range_exec
};

const struct cmd_entry cmd_set_range_entry = {
	.name = "set-range",
	.alias = NULL,

	.args = { "b:e:t:n:r:", 1, 1, NULL },
	.usage = "[-b before] [-e after] [-t target-pane] [-n name] "
		"[-r range] range-id",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_set_range_exec
};

const struct cmd_entry cmd_kill_range_entry = {
	.name = "kill-range",
	.alias = NULL,

	.args = { "t:g:", 0, 1, NULL },
	.usage = "[-t target-pane] (-g group | range-id)",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_kill_range_exec
};

const struct cmd_entry cmd_list_ranges_entry = {
	.name = "list-ranges",
	.alias = NULL,

	.args = { "t:g:F:f:", 0, 0, NULL },
	.usage = "[-t target-pane] [-g group] [-F format] [-f filter]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_list_ranges_exec
};

/* Find the range named by the argument. */
static struct pane_range *
cmd_range_find(struct cmdq_item *item, struct window_pane *wp,
    const char *value)
{
	struct pane_range	*r = NULL;
	const char		*errstr;
	u_int			 id;

	id = strtonum(value, 0, UINT_MAX, &errstr);
	if (errstr == NULL)
		r = pane_range_get(wp, id);
	if (r == NULL)
		cmdq_error(item, "no such range: %s", value);
	return (r);
}

/* Get the span of the copy mode selection if the pane still shows it. */
static int
cmd_range_selection(struct window_pane *wp, struct pane_range_span *span,
    char **cause)
{
	struct window_copy_selection	 selection;
	struct grid			*gd = wp->base.grid;
	char				*snapshot, *current;
	int				 same;

	if (!window_copy_get_selection_span(wp, &selection)) {
		xasprintf(cause, "no copy-mode selection");
		return (0);
	}
	*span = selection.span;
	if (selection.rectangular && span->start_line != span->end_line) {
		xasprintf(cause, "rectangular selection cannot be a range");
		return (0);
	}
	if (!selection.current ||
	    span->start_line < gd->scroll_collected ||
	    span->end_line >= gd->scroll_collected + gd->hsize + gd->sy) {
		xasprintf(cause, "selection has changed");
		return (0);
	}

	snapshot = pane_range_text(selection.grid,
	    span->start_line - selection.collected, span->start_x,
	    span->end_line - selection.collected, span->end_x);
	current = pane_range_text(gd, span->start_line - gd->scroll_collected,
	    span->start_x, span->end_line - gd->scroll_collected, span->end_x);
	same = (strcmp(snapshot, current) == 0);
	free(snapshot);
	free(current);
	if (!same) {
		xasprintf(cause, "selection has changed");
		return (0);
	}
	return (1);
}

/* Create a range. */
static enum cmd_retval
cmd_new_range_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct pane_range_span	 span;
	struct pane_range	*r;
	struct format_tree	*ft;
	enum pane_range_anchor	 anchor;
	const char		*template, *name, *group, *value, *text;
	char			*cause = NULL, *expanded;

	if (args_has(args, 'S') + args_has(args, 'r') +
	    args_has(args, 'm') != 1) {
		cmdq_error(item, "exactly one of -S, -r or -m is required");
		return (CMD_RETURN_ERROR);
	}
	if ((value = args_get(args, 'a')) != NULL) {
		if (strcmp(value, "grid") == 0)
			anchor = PANE_RANGE_GRID;
		else if (strcmp(value, "text") == 0)
			anchor = PANE_RANGE_TEXT;
		else {
			cmdq_error(item, "invalid anchor: %s", value);
			return (CMD_RETURN_ERROR);
		}
	} else if (args_has(args, 'm') || SCREEN_IS_ALTERNATE(&wp->base))
		anchor = PANE_RANGE_TEXT;
	else
		anchor = PANE_RANGE_GRID;
	if (anchor == PANE_RANGE_GRID && (args_has(args, 'm') ||
	    args_has(args, 'b') || args_has(args, 'e'))) {
		cmdq_error(item, "-m, -b and -e need a text anchor");
		return (CMD_RETURN_ERROR);
	}

	pane_ranges_update(wp);
	text = args_get(args, 'm');
	if (args_has(args, 'S')) {
		if (!cmd_range_selection(wp, &span, &cause))
			goto fail;
	} else if (args_has(args, 'r')) {
		if (!pane_range_parse(wp, args_get(args, 'r'), &span, &cause))
			goto fail;
	}

	name = args_get(args, 'n');
	group = args_get(args, 'g');
	r = pane_range_create(wp, text == NULL ? &span : NULL, text, anchor,
	    name == NULL ? "" : name, group == NULL ? "" : group,
	    args_get(args, 'b'), args_get(args, 'e'), &cause);
	if (r == NULL)
		goto fail;

	if (args_has(args, 'P')) {
		template = args_get(args, 'F');
		if (template == NULL)
			template = "#{range_id}";
		ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
		format_defaults(ft, cmdq_get_client(item), target->s,
		    target->wl, wp);
		pane_range_formats(ft, r);
		expanded = format_expand(ft, template);
		cmdq_print(item, "%s", expanded);
		free(expanded);
		format_free(ft);
	}
	return (CMD_RETURN_NORMAL);

fail:
	cmdq_error(item, "%s", cause);
	free(cause);
	return (CMD_RETURN_ERROR);
}

/* Change a range. */
static enum cmd_retval
cmd_set_range_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct pane_range_span	 span;
	struct pane_range	*r;
	char			*cause = NULL;

	if ((r = cmd_range_find(item, wp, args_string(args, 0))) == NULL)
		return (CMD_RETURN_ERROR);

	if ((args_has(args, 'b') || args_has(args, 'e')) &&
	    r->anchor != PANE_RANGE_TEXT) {
		cmdq_error(item, "not a text range");
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'r')) {
		if (!pane_range_parse(wp, args_get(args, 'r'), &span, &cause) ||
		    !pane_range_move(wp, r, &span, &cause))
			goto fail;
	}
	if (args_has(args, 'b') || args_has(args, 'e')) {
		pane_range_set_text_context(wp, r, args_get(args, 'b'),
		    args_get(args, 'e'));
	}
	if (args_has(args, 'n')) {
		free(r->name);
		r->name = xstrdup(args_get(args, 'n'));
	}
	return (CMD_RETURN_NORMAL);

fail:
	cmdq_error(item, "%s", cause);
	free(cause);
	return (CMD_RETURN_ERROR);
}

/* Kill a range or the ranges in a group. */
static enum cmd_retval
cmd_kill_range_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct pane_range	*r;

	if (args_has(args, 'g') == (args_count(args) != 0)) {
		cmdq_error(item, "exactly one of -g or range-id is required");
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'g')) {
		pane_ranges_kill_group(wp, args_get(args, 'g'));
		return (CMD_RETURN_NORMAL);
	}

	if ((r = cmd_range_find(item, wp, args_string(args, 0))) == NULL)
		return (CMD_RETURN_ERROR);
	pane_range_kill(wp, r);
	return (CMD_RETURN_NORMAL);
}

/* List the ranges of a pane. */
static enum cmd_retval
cmd_list_ranges_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	struct pane_range	*r;
	struct format_tree	*ft;
	const char		*group = args_get(args, 'g');
	const char		*filter = args_get(args, 'f');
	const char		*template = args_get(args, 'F');
	char			*expanded;
	int			 show;

	if (template == NULL) {
		template = "#{range_id}: #{range_name} [#{range_group}] "
		    "#{range_state} #{range_start_line}:#{range_start_x}-"
		    "#{range_end_line}:#{range_end_x}";
	}

	pane_ranges_update(wp);
	for (r = pane_range_first(wp); r != NULL; r = pane_range_next(r)) {
		if (group != NULL && strcmp(group, r->group) != 0)
			continue;

		ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
		format_defaults(ft, cmdq_get_client(item), target->s,
		    target->wl, wp);
		pane_range_formats(ft, r);

		show = 1;
		if (filter != NULL) {
			expanded = format_expand(ft, filter);
			show = format_true(expanded);
			free(expanded);
		}
		if (show) {
			expanded = format_expand(ft, template);
			cmdq_print(item, "%s", expanded);
			free(expanded);
		}
		format_free(ft);
	}
	return (CMD_RETURN_NORMAL);
}
