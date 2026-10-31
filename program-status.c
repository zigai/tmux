/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Ziga Ivansek <ziga.ivansek@gmail.com>
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

#include <resolv.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Program status protocol (OSC 7501), described at:
 *
 *     https://www.superlogical.com/rex/docs/build/program-status
 *
 * Each pane holds a set of records, one per id, created on the first report
 * and freed when the last record is removed. Records are forwarded to client
 * terminals which support the protocol, with the pane id as the first id
 * segment.
 */

#define PROGRAM_STATUS_MAX_SEQUENCE 4096
#define PROGRAM_STATUS_MAX_KEY 16
#define PROGRAM_STATUS_MAX_MSG 2732
#define PROGRAM_STATUS_MAX_MSG_DECODED 2048
#define PROGRAM_STATUS_MAX_TITLE 256
#define PROGRAM_STATUS_MAX_TITLE_DECODED 192
#define PROGRAM_STATUS_MAX_APP 32
#define PROGRAM_STATUS_MAX_ID 128
#define PROGRAM_STATUS_MAX_SEGMENT 32
#define PROGRAM_STATUS_MAX_DEPTH 8
#define PROGRAM_STATUS_MAX_RECORDS 256

/* Characters allowed in an id segment or app, and in any value. */
#define PROGRAM_STATUS_SEGMENT_CHARS \
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.+-"
#define PROGRAM_STATUS_VALUE_CHARS PROGRAM_STATUS_SEGMENT_CHARS ",/="

/* Delay before changes are forwarded to client terminals, in milliseconds. */
#define PROGRAM_STATUS_FORWARD_DELAY 100

RB_HEAD(program_status_records, program_status_record);

struct program_status {
	struct program_status_records	 records;
	u_int				 count;
	uint64_t			 updated;
};

/* A record as last written to a client terminal. */
struct program_status_sent {
	char				*id;
	char				*report;

	RB_ENTRY(program_status_sent)	 entry;
};
RB_HEAD(program_status_sent_tree, program_status_sent);

struct program_status_forward {
	struct client			*c;
	struct event			 timer;
	struct program_status_sent_tree	 sent;
};

/* A parsed report. Strings point into the copy of the report. */
struct program_status_report {
	const char	*state;
	const char	*id;
	const char	*kind;
	const char	*progress;
	const char	*app;
	char		*title;
	char		*msg;
};

static const char *program_status_state_names[] = {
	"idle",
	"working",
	"done",
	"error",
	"blocked"
};

static const char *program_status_kinds[] = {
	"permission",
	"question",
	"auth"
};

static int
program_status_record_cmp(struct program_status_record *psr1,
    struct program_status_record *psr2)
{
	return (strcmp(psr1->id, psr2->id));
}
RB_GENERATE_STATIC(program_status_records, program_status_record, entry,
    program_status_record_cmp);

static int
program_status_sent_cmp(struct program_status_sent *pss1,
    struct program_status_sent *pss2)
{
	return (strcmp(pss1->id, pss2->id));
}
RB_GENERATE_STATIC(program_status_sent_tree, program_status_sent, entry,
    program_status_sent_cmp);

static void	program_status_forward_all(void);

/* Get the name of a state. */
const char *
program_status_state_name(enum program_status_state state)
{
	return (program_status_state_names[state]);
}

/*
 * Decode one UTF-8 character. Returns its length or 0 if the bytes are not
 * valid UTF-8.
 */
static size_t
program_status_decode_char(const u_char *s, size_t len, u_int *cp)
{
	u_int	n, i, min;

	if (s[0] < 0x80) {
		*cp = s[0];
		return (1);
	}
	if (s[0] >= 0xc2 && s[0] <= 0xdf) {
		n = 2;
		min = 0x80;
		*cp = s[0] & 0x1f;
	} else if (s[0] >= 0xe0 && s[0] <= 0xef) {
		n = 3;
		min = 0x800;
		*cp = s[0] & 0x0f;
	} else if (s[0] >= 0xf0 && s[0] <= 0xf4) {
		n = 4;
		min = 0x10000;
		*cp = s[0] & 0x07;
	} else
		return (0);
	if (len < n)
		return (0);
	for (i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return (0);
		*cp = (*cp << 6) | (s[i] & 0x3f);
	}
	if (*cp < min || *cp > 0x10ffff || (*cp >= 0xd800 && *cp <= 0xdfff))
		return (0);
	return (n);
}

/* Is this a direction override or other invisible formatting character? */
static int
program_status_invisible(u_int cp)
{
	return (cp == 0xad ||
	    cp == 0x61c ||
	    cp == 0x180e ||
	    cp == 0x200b ||
	    cp == 0x200e ||
	    cp == 0x200f ||
	    (cp >= 0x202a && cp <= 0x202e) ||
	    (cp >= 0x2060 && cp <= 0x2064) ||
	    (cp >= 0x2066 && cp <= 0x206f) ||
	    cp == 0xfeff ||
	    (cp >= 0xfff9 && cp <= 0xfffb));
}

/*
 * Decode a base64 text value, with optional padding. The result must be UTF-8
 * without control characters.
 */
static char *
program_status_decode_text(const char *value, size_t limit)
{
	char	*copy, *out;
	size_t	 len = strlen(value), step, i;
	int	 n;
	u_int	 cp;

	while (len != 0 && value[len - 1] == '=')
		len--;
	if (len % 4 == 1)
		return (NULL);

	copy = xmalloc(len + 3);
	memcpy(copy, value, len);
	while (len % 4 != 0)
		copy[len++] = '=';
	copy[len] = '\0';

	out = xmalloc((len / 4) * 3 + 1);
	n = b64_pton(copy, (u_char *)out, (len / 4) * 3);
	free(copy);
	if (n < 0 || (size_t)n > limit)
		goto fail;
	out[n] = '\0';

	for (i = 0; i < (size_t)n; i += step) {
		if ((u_char)out[i] < 0x20 || (u_char)out[i] == 0x7f)
			goto fail;
		step = program_status_decode_char((u_char *)out + i, n - i,
		    &cp);
		if (step == 0 || (cp >= 0x80 && cp <= 0x9f))
			goto fail;
	}
	return (out);

fail:
	free(out);
	return (NULL);
}

/* Encode a text value as base64. */
static char *
program_status_encode_text(const char *text)
{
	size_t	 len = strlen(text), size = ((len + 2) / 3) * 4 + 1;
	char	*out;

	out = xmalloc(size);
	if (b64_ntop((const u_char *)text, len, out, size) == -1)
		fatalx("base64 encoding failed");
	return (out);
}

/* Remove whitespace around a key or value. */
static char *
program_status_trim(char *s)
{
	char	*end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end != s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return (s);
}

/* Check an id for the grammar and limits. */
static int
program_status_check_id(const char *id, int check_grammar)
{
	const char	*cp;
	size_t		 segment = 0;
	u_int		 depth = 1;

	if (strlen(id) > PROGRAM_STATUS_MAX_ID)
		return (-1);
	for (cp = id; *cp != '\0'; cp++) {
		if (*cp == '/') {
			if (check_grammar && segment == 0)
				return (-1);
			if (++depth > PROGRAM_STATUS_MAX_DEPTH)
				return (-1);
			segment = 0;
			continue;
		}
		if (++segment > PROGRAM_STATUS_MAX_SEGMENT)
			return (-1);
		if (check_grammar && strchr(PROGRAM_STATUS_SEGMENT_CHARS,
		    *cp) == NULL)
			return (-1);
	}
	if (check_grammar && segment == 0)
		return (-1);
	return (0);
}

/*
 * Parse a report into pairs. Returns -1 if the whole report must be discarded.
 * A pair that is not well formed is skipped.
 */
static int
program_status_parse(char *body, struct program_status_report *r)
{
	char		*next, *pair, *key, *value, *text;
	const char	*cp;

	while ((next = strsep(&body, ":")) != NULL) {
		pair = next;
		if ((value = strchr(pair, '=')) == NULL)
			continue;
		*value++ = '\0';
		key = program_status_trim(pair);
		value = program_status_trim(value);

		if (*key == '\0')
			continue;
		for (cp = key; *cp != '\0'; cp++) {
			if (*cp < 'a' || *cp > 'z')
				break;
		}
		if (*cp != '\0')
			continue;
		if (strlen(key) > PROGRAM_STATUS_MAX_KEY)
			return (-1);
		if (strspn(value, PROGRAM_STATUS_VALUE_CHARS) != strlen(value))
			continue;

		if (strcmp(key, "state") == 0)
			r->state = value;
		else if (strcmp(key, "id") == 0) {
			if (program_status_check_id(value, 0) != 0)
				return (-1);
			r->id = value;
		} else if (strcmp(key, "kind") == 0)
			r->kind = value;
		else if (strcmp(key, "progress") == 0)
			r->progress = value;
		else if (strcmp(key, "app") == 0) {
			if (strlen(value) > PROGRAM_STATUS_MAX_APP)
				return (-1);
			r->app = value;
		} else if (strcmp(key, "title") == 0) {
			if (strlen(value) > PROGRAM_STATUS_MAX_TITLE)
				return (-1);
			text = program_status_decode_text(value,
			    PROGRAM_STATUS_MAX_TITLE_DECODED);
			if (text == NULL)
				return (-1);
			free(r->title);
			r->title = text;
		} else if (strcmp(key, "msg") == 0) {
			if (strlen(value) > PROGRAM_STATUS_MAX_MSG)
				return (-1);
			text = program_status_decode_text(value,
			    PROGRAM_STATUS_MAX_MSG_DECODED);
			if (text == NULL)
				return (-1);
			free(r->msg);
			r->msg = text;
		}
	}
	return (0);
}

/* Find a record. */
static struct program_status_record *
program_status_find(struct program_status *ps, const char *id)
{
	struct program_status_record	find;

	find.id = (char *)id;
	return (RB_FIND(program_status_records, &ps->records, &find));
}

/* Free a record. */
static void
program_status_free_record(struct program_status_record *psr)
{
	free(psr->id);
	free(psr->app);
	free(psr->title);
	free(psr->msg);
	free(psr);
}

/* Remove a record. */
static void
program_status_remove(struct program_status *ps,
    struct program_status_record *psr)
{
	RB_REMOVE(program_status_records, &ps->records, psr);
	ps->count--;
	program_status_free_record(psr);
}

/* Free the record set if it is empty. */
static void
program_status_check_empty(struct window_pane *wp)
{
	struct program_status	*ps = wp->program_status;

	if (ps != NULL && ps->count == 0) {
		free(ps);
		wp->program_status = NULL;
	}
}

/* Is this record the one with this id or beneath it? */
static int
program_status_beneath(const char *id, const char *parent)
{
	size_t	len = strlen(parent);

	if (len == 0)
		return (1);
	return (strncmp(id, parent, len) == 0 &&
	    (id[len] == '\0' || id[len] == '/'));
}

/* Records of a pane have changed. */
static void
program_status_changed(struct window_pane *wp)
{
	log_debug("%s: %%%u", __func__, wp->id);

	program_status_check_empty(wp);
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
	events_fire_pane("pane-program-status-changed", wp);
	program_status_forward_all();
}

/*
 * Remove records which match a set of states. Returns 1 if any were
 * removed.
 */
static int
program_status_remove_states(struct window_pane *wp, int states)
{
	struct program_status		*ps = wp->program_status;
	struct program_status_record	*psr, *psr1;
	int				 changed = 0;

	if (ps == NULL)
		return (0);
	RB_FOREACH_SAFE(psr, program_status_records, &ps->records, psr1) {
		if (states & (1 << psr->state)) {
			program_status_remove(ps, psr);
			changed = 1;
		}
	}
	return (changed);
}

/* Do two records hold the same keys? */
static int
program_status_same(struct program_status_record *psr1,
    struct program_status_record *psr2)
{
	if (psr1->state != psr2->state ||
	    psr1->kind != psr2->kind ||
	    psr1->progress != psr2->progress)
		return (0);
	if ((psr1->app == NULL) != (psr2->app == NULL) ||
	    (psr1->app != NULL && strcmp(psr1->app, psr2->app) != 0))
		return (0);
	if ((psr1->title == NULL) != (psr2->title == NULL) ||
	    (psr1->title != NULL && strcmp(psr1->title, psr2->title) != 0))
		return (0);
	if ((psr1->msg == NULL) != (psr2->msg == NULL) ||
	    (psr1->msg != NULL && strcmp(psr1->msg, psr2->msg) != 0))
		return (0);
	return (1);
}

/* Apply a valid report to the record set. Returns 1 if it changed. */
static int
program_status_apply(struct window_pane *wp, struct program_status_report *r,
    enum program_status_state state)
{
	struct program_status		*ps = wp->program_status;
	struct program_status_record	*psr, *new, *oldest;
	const char			*id = r->id;
	const char			*errstr;
	u_int				 i;
	int				 changed = 1;

	if (id == NULL)
		id = "";

	new = xcalloc(1, sizeof *new);
	new->id = xstrdup(id);
	new->state = state;
	new->progress = -1;
	if (state == PROGRAM_STATUS_BLOCKED && r->kind != NULL) {
		for (i = 0; i < nitems(program_status_kinds); i++) {
			if (strcmp(r->kind, program_status_kinds[i]) == 0)
				new->kind = program_status_kinds[i];
		}
	}
	if ((state == PROGRAM_STATUS_WORKING ||
	    state == PROGRAM_STATUS_BLOCKED) &&
	    r->progress != NULL &&
	    strspn(r->progress, "0123456789") == strlen(r->progress)) {
		new->progress = strtonum(r->progress, 0, 100, &errstr);
		if (errstr != NULL)
			new->progress = -1;
	}
	if (r->app != NULL &&
	    *r->app != '\0' &&
	    strspn(r->app, PROGRAM_STATUS_SEGMENT_CHARS) == strlen(r->app))
		new->app = xstrdup(r->app);
	new->title = r->title;
	r->title = NULL;
	new->msg = r->msg;
	r->msg = NULL;

	if (ps == NULL) {
		ps = wp->program_status = xcalloc(1, sizeof *ps);
		RB_INIT(&ps->records);
	}
	new->updated = ++ps->updated;

	psr = program_status_find(ps, id);
	if (psr != NULL) {
		changed = !program_status_same(psr, new);
		program_status_remove(ps, psr);
	} else if (ps->count == PROGRAM_STATUS_MAX_RECORDS) {
		oldest = NULL;
		RB_FOREACH(psr, program_status_records, &ps->records) {
			if (oldest == NULL || psr->updated < oldest->updated)
				oldest = psr;
		}
		log_debug("%s: %%%u: evict %s", __func__, wp->id, oldest->id);
		program_status_remove(ps, oldest);
	}
	RB_INSERT(program_status_records, &ps->records, new);
	ps->count++;
	return (changed);
}

/* Clear a record and the records beneath it. Returns 1 if any were removed. */
static int
program_status_clear(struct window_pane *wp, const char *id)
{
	struct program_status		*ps = wp->program_status;
	struct program_status_record	*psr, *psr1;
	int				 changed = 0;

	if (ps == NULL)
		return (0);
	if (id == NULL)
		id = "";
	RB_FOREACH_SAFE(psr, program_status_records, &ps->records, psr1) {
		if (program_status_beneath(psr->id, id)) {
			program_status_remove(ps, psr);
			changed = 1;
		}
	}
	return (changed);
}

/*
 * Handle a report from a pane. The size is the whole sequence from OSC
 * through ST.
 */
void
program_status_report(struct window_pane *wp, const char *body, size_t size)
{
	struct program_status_report	 r;
	char				*copy;
	const char			*name;
	u_int				 state;
	int				 changed = 0;

	if (size > PROGRAM_STATUS_MAX_SEQUENCE) {
		log_debug("%s: %%%u: too long (%zu)", __func__, wp->id, size);
		return;
	}

	memset(&r, 0, sizeof r);
	copy = xstrdup(body);
	if (program_status_parse(copy, &r) != 0) {
		log_debug("%s: %%%u: discarded: %s", __func__, wp->id, body);
		goto out;
	}
	if (r.state == NULL)
		goto out;
	if (r.id != NULL && program_status_check_id(r.id, 1) != 0) {
		log_debug("%s: %%%u: bad id: %s", __func__, wp->id, r.id);
		goto out;
	}

	if (strcmp(r.state, "clear") == 0)
		changed = program_status_clear(wp, r.id);
	else {
		for (state = 0; state < nitems(program_status_state_names);
		    state++) {
			name = program_status_state_names[state];
			if (strcmp(r.state, name) == 0)
				break;
		}
		if (state == nitems(program_status_state_names))
			goto out;
		changed = program_status_apply(wp, &r, state);
	}
	if (changed)
		program_status_changed(wp);

out:
	free(r.title);
	free(r.msg);
	free(copy);
}

/* Drop working and blocked records when the program exits or at a prompt. */
void
program_status_end_running(struct window_pane *wp)
{
	int	states;

	states = (1 << PROGRAM_STATUS_WORKING)|(1 << PROGRAM_STATUS_BLOCKED);
	if (program_status_remove_states(wp, states))
		program_status_changed(wp);
}

/* Remove done and error records once the user has seen them. */
void
program_status_dismiss(struct window_pane *wp)
{
	int	states;

	states = (1 << PROGRAM_STATUS_DONE)|(1 << PROGRAM_STATUS_ERROR);
	if (program_status_remove_states(wp, states))
		program_status_changed(wp);
}

/* Remove every record on a full reset. */
void
program_status_reset(struct window_pane *wp)
{
	if (program_status_clear(wp, NULL))
		program_status_changed(wp);
}

/* Free the records of a pane which is being destroyed. */
void
program_status_free(struct window_pane *wp)
{
	struct program_status		*ps = wp->program_status;
	struct program_status_record	*psr, *psr1;

	if (ps == NULL)
		return;
	RB_FOREACH_SAFE(psr, program_status_records, &ps->records, psr1)
		program_status_remove(ps, psr);
	free(ps);
	wp->program_status = NULL;
	program_status_forward_all();
}

/* Get the root record of a pane. */
struct program_status_record *
program_status_root(struct window_pane *wp)
{
	if (wp->program_status == NULL)
		return (NULL);
	return (program_status_find(wp->program_status, ""));
}

/* Get the first record of a pane, in id order. */
struct program_status_record *
program_status_first(struct window_pane *wp)
{
	if (wp->program_status == NULL)
		return (NULL);
	return (RB_MIN(program_status_records, &wp->program_status->records));
}

/* Get the next record. */
struct program_status_record *
program_status_next(struct program_status_record *psr)
{
	return (RB_NEXT(program_status_records, NULL, psr));
}

/* Get the number of records in a pane. */
u_int
program_status_count(struct window_pane *wp)
{
	if (wp->program_status == NULL)
		return (0);
	return (wp->program_status->count);
}

/* Get the app of a record, from its nearest ancestor if it has none. */
const char *
program_status_app(struct window_pane *wp, struct program_status_record *psr)
{
	struct program_status_record	*found;
	char				*id, *cp;
	const char			*app = NULL;

	if (psr->app != NULL || *psr->id == '\0')
		return (psr->app);

	id = xstrdup(psr->id);
	do {
		if ((cp = strrchr(id, '/')) != NULL)
			*cp = '\0';
		else
			*id = '\0';
		found = program_status_find(wp->program_status, id);
		if (found != NULL && found->app != NULL) {
			app = found->app;
			break;
		}
	} while (*id != '\0');
	free(id);
	return (app);
}

/* Get the most urgent state of a pane, or -1 if it has no records. */
int
program_status_pane_state(struct window_pane *wp)
{
	struct program_status_record	*psr;
	int				 state = -1;

	if (wp->program_status == NULL)
		return (-1);
	RB_FOREACH(psr, program_status_records, &wp->program_status->records) {
		if ((int)psr->state > state)
			state = psr->state;
	}
	return (state);
}

/* Get the most urgent state of a window, or -1 if it has no records. */
int
program_status_window_state(struct window *w)
{
	struct window_pane	*wp;
	int			 state = -1, pane_state;

	TAILQ_FOREACH(wp, &w->panes, entry) {
		pane_state = program_status_pane_state(wp);
		if (pane_state > state)
			state = pane_state;
	}
	return (state);
}

/* Get the most urgent state of a session, or -1 if it has no records. */
int
program_status_session_state(struct session *s)
{
	struct winlink	*wl;
	int		 state = -1, window_state;

	RB_FOREACH(wl, winlinks, &s->windows) {
		window_state = program_status_window_state(wl->window);
		if (window_state > state)
			state = window_state;
	}
	return (state);
}

/*
 * Copy record text for a format, without invisible formatting characters.
 * If the text will be drawn, escape # so it cannot be used as markup.
 */
char *
program_status_text(const char *text, int escape)
{
	const u_char	*s = (const u_char *)text;
	size_t		 len = strlen(text), n;
	char		*out, *at;
	u_int		 cp;

	at = out = xmalloc(len * 2 + 1);
	while (len != 0) {
		n = program_status_decode_char(s, len, &cp);
		if (n == 0)
			fatalx("bad UTF-8 in program status");
		if (!program_status_invisible(cp)) {
			if (escape && cp == '#')
				*at++ = '#';
			memcpy(at, s, n);
			at += n;
		}
		s += n;
		len -= n;
	}
	*at = '\0';
	return (out);
}

/* Build the report to forward a record to a client terminal. */
static char *
program_status_forward_report(struct program_status_record *psr,
    const char *id)
{
	char	*report, *title = NULL, *msg = NULL, progress[16] = "";

	if (psr->progress != -1)
		xsnprintf(progress, sizeof progress, ":progress=%d",
		    psr->progress);
	if (psr->title != NULL)
		title = program_status_encode_text(psr->title);
	if (psr->msg != NULL)
		msg = program_status_encode_text(psr->msg);
	xasprintf(&report, "\033]7501;state=%s:id=%s%s%s%s%s%s%s%s%s%s\033\\",
	    program_status_state_name(psr->state), id,
	    psr->kind == NULL ? "" : ":kind=",
	    psr->kind == NULL ? "" : psr->kind,
	    progress,
	    psr->app == NULL ? "" : ":app=",
	    psr->app == NULL ? "" : psr->app,
	    title == NULL ? "" : ":title=",
	    title == NULL ? "" : title,
	    msg == NULL ? "" : ":msg=",
	    msg == NULL ? "" : msg);
	free(title);
	free(msg);
	return (report);
}

/* Remove a sent record from a tree. */
static void
program_status_remove_sent(struct program_status_sent_tree *tree,
    struct program_status_sent *pss)
{
	RB_REMOVE(program_status_sent_tree, tree, pss);
	free(pss->id);
	free(pss->report);
	free(pss);
}

/* Free a tree of sent records. */
static void
program_status_free_sent(struct program_status_sent_tree *tree)
{
	struct program_status_sent	*pss, *pss1;

	RB_FOREACH_SAFE(pss, program_status_sent_tree, tree, pss1)
		program_status_remove_sent(tree, pss);
}

/* Add the records of a pane to the set a client terminal should hold. */
static void
program_status_forward_pane(struct window_pane *wp,
    struct program_status_sent_tree *want)
{
	struct program_status_record	*psr;
	struct program_status_sent	*pss;
	char				*id;

	if (wp->program_status == NULL)
		return;
	RB_FOREACH(psr, program_status_records, &wp->program_status->records) {
		if (*psr->id == '\0')
			xasprintf(&id, "%u", wp->id);
		else
			xasprintf(&id, "%u/%s", wp->id, psr->id);
		if (program_status_check_id(id, 1) != 0) {
			free(id);
			continue;
		}
		pss = xcalloc(1, sizeof *pss);
		pss->id = id;
		pss->report = program_status_forward_report(psr, id);
		if (RB_INSERT(program_status_sent_tree, want, pss) != NULL) {
			free(pss->id);
			free(pss->report);
			free(pss);
		}
	}
}

/* Bring a client terminal up to date with the records of its session. */
static void
program_status_forward_flush(struct program_status_forward *psf)
{
	struct client			*c = psf->c;
	struct tty			*tty = &c->tty;
	struct program_status_sent_tree	 want;
	struct program_status_sent	*pss, *pss1, *found;
	struct winlink			*wl;
	struct window_pane		*wp;
	char				*clear, *id;

	RB_INIT(&want);
	RB_FOREACH(wl, winlinks, &c->session->windows) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry)
			program_status_forward_pane(wp, &want);
	}

	/*
	 * Clearing a record also clears every record beneath it in the client
	 * terminal, so forget those too and send them again if they are still
	 * wanted.
	 */
	for (;;) {
		RB_FOREACH(pss, program_status_sent_tree, &psf->sent) {
			found = RB_FIND(program_status_sent_tree, &want, pss);
			if (found == NULL)
				break;
		}
		if (pss == NULL)
			break;
		xasprintf(&clear, "\033]7501;state=clear:id=%s\033\\", pss->id);
		tty_puts(tty, clear);
		free(clear);

		id = xstrdup(pss->id);
		RB_FOREACH_SAFE(found, program_status_sent_tree, &psf->sent,
		    pss1) {
			if (program_status_beneath(found->id, id))
				program_status_remove_sent(&psf->sent, found);
		}
		free(id);
	}

	RB_FOREACH_SAFE(pss, program_status_sent_tree, &want, pss1) {
		found = RB_FIND(program_status_sent_tree, &psf->sent, pss);
		if (found != NULL && strcmp(found->report, pss->report) == 0)
			continue;
		tty_puts(tty, pss->report);
		if (found != NULL)
			program_status_remove_sent(&psf->sent, found);
		RB_REMOVE(program_status_sent_tree, &want, pss);
		RB_INSERT(program_status_sent_tree, &psf->sent, pss);
	}
	program_status_free_sent(&want);
}

/* Is this client terminal ready for records? */
static int
program_status_forward_ready(struct client *c)
{
	if (c->session == NULL ||
	    (~c->flags & CLIENT_TERMINAL) ||
	    (c->flags & (CLIENT_CONTROL|CLIENT_UNATTACHEDFLAGS)) ||
	    c->tty.term == NULL ||
	    (~c->tty.term->flags & TERM_PROGRAMSTATUS) ||
	    (~c->tty.flags & TTY_STARTED))
		return (0);
	return (1);
}

/* Timer to forward records to a client terminal. */
static void
program_status_forward_callback(__unused int fd, __unused short events,
    void *data)
{
	struct program_status_forward	*psf = data;
	struct client			*c = psf->c;
	struct timeval			 tv = { 0 };

	if (!program_status_forward_ready(c))
		return;
	if (c->tty.flags & TTY_BLOCK) {
		tv.tv_usec = PROGRAM_STATUS_FORWARD_DELAY * 1000L;
		evtimer_add(&psf->timer, &tv);
		return;
	}
	program_status_forward_flush(psf);
}

/* Forward records to a client terminal soon. */
void
program_status_forward_client(struct client *c)
{
	struct program_status_forward	*psf = c->tty.program_status;
	struct timeval			 tv = { 0 };

	if (!program_status_forward_ready(c))
		return;
	if (psf == NULL) {
		psf = c->tty.program_status = xcalloc(1, sizeof *psf);
		psf->c = c;
		RB_INIT(&psf->sent);
		evtimer_set(&psf->timer, program_status_forward_callback, psf);
	}
	if (evtimer_pending(&psf->timer, NULL))
		return;
	tv.tv_usec = PROGRAM_STATUS_FORWARD_DELAY * 1000L;
	evtimer_add(&psf->timer, &tv);
}

/* Forward records to every client terminal soon. */
static void
program_status_forward_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry)
		program_status_forward_client(c);
}

/* Clear everything sent to a client terminal. */
static void
program_status_forward_clear(struct tty *tty, int raw)
{
	struct program_status_forward	*psf = tty->program_status;
	static const char		 clear[] =
	    "\033]7501;state=clear\033\\";

	if (psf == NULL)
		return;
	evtimer_del(&psf->timer);
	if (RB_EMPTY(&psf->sent))
		return;
	if (raw)
		tty_raw(tty, clear);
	else
		tty_puts(tty, clear);
	program_status_free_sent(&psf->sent);
}

/* A client has changed session, so its terminal needs other records. */
void
program_status_session_changed(struct client *c)
{
	program_status_forward_clear(&c->tty, 0);
	program_status_forward_client(c);
}

/*
 * A client terminal is being stopped, so clear what it holds. The output
 * buffer has been written or discarded by now.
 */
void
program_status_stop(struct tty *tty)
{
	program_status_forward_clear(tty, 1);
}

/* Free forwarding state for a client terminal. */
void
program_status_free_tty(struct tty *tty)
{
	struct program_status_forward	*psf = tty->program_status;

	if (psf == NULL)
		return;
	evtimer_del(&psf->timer);
	program_status_free_sent(&psf->sent);
	free(psf);
	tty->program_status = NULL;
}

/* Panes have moved between windows or sessions. */
static void
program_status_moved_cb(__unused const char *name,
    __unused struct event_payload *ep, __unused void *data)
{
	program_status_forward_all();
}

/* Build event sinks for forwarding. */
void
program_status_build_events(void)
{
	static const char	*names[] = {
		"pane-moved",
		"window-layout-changed",
		"window-linked",
		"window-unlinked"
	};
	u_int			 i;

	for (i = 0; i < nitems(names); i++)
		events_add_sink(names[i], program_status_moved_cb, NULL);
}
