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

/*
 * Delay before changes run the hook and are forwarded to client terminals,
 * in milliseconds.
 */
#define PROGRAM_STATUS_DELAY 100

RB_HEAD(program_status_records, program_status_record);

struct program_status {
	struct window_pane		*wp;
	struct program_status_records	 records;
	u_int				 count;

	uint64_t			 generation;
	struct event			 timer;
	int				 pending;
};

/* A record as last written to a client terminal, or one to write. */
struct program_status_sent {
	char				*id;
	char				*report;
	int				 cleared;

	RB_ENTRY(program_status_sent)	 entry;
};
RB_HEAD(program_status_sent_tree, program_status_sent);

/* The generation of a pane's records when they were last forwarded. */
struct program_status_seen {
	u_int				 id;
	uint64_t			 generation;
};

struct program_status_forward {
	struct client			*c;
	struct event			 timer;
	struct program_status_sent_tree	 sent;

	struct program_status_seen	*seen;
	u_int				 nseen;
};

/* A parsed report. Strings point into the copy of the report. */
struct program_status_report {
	const char	*state;
	const char	*id;
	int		 bad_id;
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

/* Orders record updates across every pane. */
static uint64_t	program_status_clock;

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
 * Read one UTF-8 character. Returns its length or 0 if the bytes are not
 * valid UTF-8.
 */
static size_t
program_status_char(const u_char *s, size_t len, wchar_t *wc)
{
	struct utf8_data	ud;
	u_int			i;

	if (*s < 0x80) {
		*wc = *s;
		return (1);
	}
	if (utf8_open(&ud, *s) != UTF8_MORE || len < ud.size)
		return (0);
	for (i = 1; i < ud.size; i++) {
		if (utf8_append(&ud, s[i]) == UTF8_ERROR)
			return (0);
	}
	if (utf8_towc(&ud, wc) != UTF8_DONE)
		return (0);
	return (ud.size);
}

/* Is this a direction override or other invisible formatting character? */
static int
program_status_invisible(wchar_t wc)
{
	return (wc == 0xad ||
	    wc == 0x61c ||
	    wc == 0x180e ||
	    wc == 0x200b ||
	    wc == 0x200e ||
	    wc == 0x200f ||
	    (wc >= 0x202a && wc <= 0x202e) ||
	    (wc >= 0x2060 && wc <= 0x2064) ||
	    (wc >= 0x2066 && wc <= 0x206f) ||
	    wc == 0xfeff ||
	    (wc >= 0xfff9 && wc <= 0xfffb));
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
	wchar_t	 wc;

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
		step = program_status_char((u_char *)out + i, n - i, &wc);
		if (step == 0 || wc < 0x20 || (wc >= 0x7f && wc <= 0x9f))
			goto fail;
	}
	return (out);

fail:
	free(out);
	return (NULL);
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

/*
 * Check an id. Returns -1 if it breaks a limit, 1 if it does not match the
 * grammar, or 0 if it is valid.
 */
static int
program_status_check_id(const char *id)
{
	const char	*cp;
	size_t		 segment = 0;
	u_int		 depth = 1;
	int		 bad = 0;

	if (strlen(id) > PROGRAM_STATUS_MAX_ID)
		return (-1);
	for (cp = id; *cp != '\0'; cp++) {
		if (*cp == '/') {
			if (segment == 0)
				bad = 1;
			if (++depth > PROGRAM_STATUS_MAX_DEPTH)
				return (-1);
			segment = 0;
			continue;
		}
		if (++segment > PROGRAM_STATUS_MAX_SEGMENT)
			return (-1);
		if (strchr(PROGRAM_STATUS_SEGMENT_CHARS, *cp) == NULL)
			bad = 1;
	}
	if (segment == 0)
		bad = 1;
	return (bad);
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
	int		 bad;

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
			if ((bad = program_status_check_id(value)) == -1)
				return (-1);
			r->id = value;
			r->bad_id = bad;
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

/* Add a text value to a report as base64. */
static void
program_status_add_text(struct evbuffer *evb, const char *key,
    const char *text)
{
	size_t	 len = strlen(text), size = ((len + 2) / 3) * 4 + 1;
	char	*out;

	out = xmalloc(size);
	if (b64_ntop((const u_char *)text, len, out, size) == -1)
		fatalx("base64 encoding failed");
	evbuffer_add_printf(evb, ":%s=%s", key, out);
	free(out);
}

/*
 * Build the report which forwards a record to a client terminal, with the
 * pane id as the first id segment. The id is not set if it would break the
 * terminal's limits.
 */
static void
program_status_build_forward(struct window_pane *wp,
    struct program_status_record *psr)
{
	struct evbuffer	*evb;
	char		*id;

	if (*psr->id == '\0')
		xasprintf(&id, "%u", wp->id);
	else
		xasprintf(&id, "%u/%s", wp->id, psr->id);

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(evb, "\033]7501;state=%s:id=%s",
	    program_status_state_name(psr->state), id);
	if (psr->kind != NULL)
		evbuffer_add_printf(evb, ":kind=%s", psr->kind);
	if (psr->progress != -1)
		evbuffer_add_printf(evb, ":progress=%d", psr->progress);
	if (psr->app != NULL)
		evbuffer_add_printf(evb, ":app=%s", psr->app);
	if (psr->title != NULL)
		program_status_add_text(evb, "title", psr->title);
	if (psr->msg != NULL)
		program_status_add_text(evb, "msg", psr->msg);
	evbuffer_add(evb, "\033\\", 2);
	psr->forward = xstrndup(EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	evbuffer_free(evb);

	if (program_status_check_id(id) == 0)
		psr->forward_id = id;
	else
		free(id);
}

/* Find a record. */
static struct program_status_record *
program_status_find(struct program_status *ps, const char *id)
{
	struct program_status_record	find;

	find.id = (char *)id;
	return (RB_FIND(program_status_records, &ps->records, &find));
}

/* Remove a record. */
static void
program_status_remove(struct program_status *ps,
    struct program_status_record *psr)
{
	RB_REMOVE(program_status_records, &ps->records, psr);
	ps->count--;
	ps->generation = ++program_status_clock;

	free(psr->id);
	free(psr->app);
	free(psr->title);
	free(psr->msg);
	free(psr->forward_id);
	free(psr->forward);
	free(psr);
}

/* Free the record set of a pane if it is empty and has nothing to do. */
static void
program_status_check_empty(struct window_pane *wp)
{
	struct program_status	*ps = wp->program_status;

	if (ps != NULL && ps->count == 0 && !ps->pending) {
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

/* Timer to run the hook after records have changed. */
static void
program_status_changed_callback(__unused int fd, __unused short events,
    void *data)
{
	struct program_status	*ps = data;
	struct window_pane	*wp = ps->wp;

	ps->pending = 0;
	events_fire_pane("pane-program-status-changed", wp);
	program_status_check_empty(wp);
}

/*
 * Records of a pane have changed. The hook waits so a burst of reports runs
 * it once.
 */
static void
program_status_changed(struct window_pane *wp)
{
	struct program_status	*ps = wp->program_status;
	struct timeval		 tv;

	log_debug("%s: %%%u", __func__, wp->id);

	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
	if (!ps->pending) {
		ps->pending = 1;
		tv.tv_sec = 0;
		tv.tv_usec = PROGRAM_STATUS_DELAY * 1000L;
		evtimer_add(&ps->timer, &tv);
	}
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
	program_status_build_forward(wp, new);

	if (ps == NULL) {
		ps = wp->program_status = xcalloc(1, sizeof *ps);
		ps->wp = wp;
		RB_INIT(&ps->records);
		evtimer_set(&ps->timer, program_status_changed_callback, ps);
	}

	psr = program_status_find(ps, id);
	if (psr != NULL) {
		changed = (strcmp(psr->forward, new->forward) != 0);
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
	new->updated = ++program_status_clock;
	ps->generation = new->updated;
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
	if (r.bad_id) {
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

		/* An unchanged record may still change what is forwarded. */
		if (!changed)
			program_status_forward_all();
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
	evtimer_del(&ps->timer);
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
program_status_next(struct window_pane *wp, struct program_status_record *psr)
{
	if (wp->program_status == NULL)
		return (NULL);
	return (RB_NEXT(program_status_records, &wp->program_status->records,
	    psr));
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
	wchar_t		 wc;

	at = out = xmalloc(len * 2 + 1);
	while (len != 0) {
		n = program_status_char(s, len, &wc);
		if (n == 0)
			fatalx("bad UTF-8 in program status");
		if (!program_status_invisible(wc)) {
			if (escape && wc == '#')
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

/* Has an ancestor of this sent record been cleared? */
static int
program_status_ancestor_cleared(struct program_status_sent_tree *tree,
    const char *id)
{
	struct program_status_sent	 find, *found;
	char				*copy, *cp;
	int				 cleared = 0;

	copy = xstrdup(id);
	find.id = copy;
	while (!cleared && (cp = strrchr(copy, '/')) != NULL) {
		*cp = '\0';
		found = RB_FIND(program_status_sent_tree, tree, &find);
		cleared = (found != NULL && found->cleared);
	}
	free(copy);
	return (cleared);
}

static int
program_status_seen_cmp(const void *a, const void *b)
{
	const struct program_status_seen	*pss1 = a, *pss2 = b;

	if (pss1->id < pss2->id)
		return (-1);
	return (pss1->id > pss2->id);
}

/*
 * Order records for a client terminal, which holds at most 256: the root
 * record of each pane first, then the most recently updated.
 */
static int
program_status_priority_cmp(const void *a, const void *b)
{
	struct program_status_record	*psr1, *psr2;
	int				 root1, root2;

	psr1 = *(struct program_status_record *const *)a;
	psr2 = *(struct program_status_record *const *)b;
	root1 = (*psr1->id == '\0');
	root2 = (*psr2->id == '\0');

	if (root1 != root2)
		return (root2 - root1);
	if (psr1->updated > psr2->updated)
		return (-1);
	return (psr1->updated < psr2->updated);
}

/*
 * Get the panes with records in a client's session and their generations,
 * ordered by pane id. Returns 1 if they are the same as last time.
 */
static int
program_status_forward_seen(struct program_status_forward *psf)
{
	struct program_status_seen	*seen = NULL;
	struct winlink			*wl;
	struct window_pane		*wp;
	u_int				 n = 0, i, j;

	RB_FOREACH(wl, winlinks, &psf->c->session->windows) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			if (wp->program_status == NULL)
				continue;
			seen = xreallocarray(seen, n + 1, sizeof *seen);
			seen[n].id = wp->id;
			seen[n].generation = wp->program_status->generation;
			n++;
		}
	}
	if (n != 0)
		qsort(seen, n, sizeof *seen, program_status_seen_cmp);
	for (i = j = 0; i < n; i++) {
		if (j == 0 || seen[i].id != seen[j - 1].id)
			seen[j++] = seen[i];
	}
	n = j;

	if (n == psf->nseen &&
	    (n == 0 || memcmp(seen, psf->seen, n * sizeof *seen) == 0)) {
		free(seen);
		return (1);
	}
	free(psf->seen);
	psf->seen = seen;
	psf->nseen = n;
	return (0);
}

/*
 * Get the records a client terminal should hold, up to the number it can
 * keep.
 */
static void
program_status_forward_want(struct program_status_forward *psf,
    struct program_status_sent_tree *want)
{
	struct program_status_record	**list = NULL, *psr;
	struct program_status_sent	 *pss;
	struct window_pane		 *wp;
	u_int				  n = 0, i;

	for (i = 0; i < psf->nseen; i++) {
		wp = window_pane_find_by_id(psf->seen[i].id);
		RB_FOREACH(psr, program_status_records,
		    &wp->program_status->records) {
			if (psr->forward_id == NULL)
				continue;
			list = xreallocarray(list, n + 1, sizeof *list);
			list[n++] = psr;
		}
	}
	if (n > PROGRAM_STATUS_MAX_RECORDS) {
		qsort(list, n, sizeof *list, program_status_priority_cmp);
		n = PROGRAM_STATUS_MAX_RECORDS;
	}
	for (i = 0; i < n; i++) {
		pss = xcalloc(1, sizeof *pss);
		pss->id = list[i]->forward_id;
		pss->report = list[i]->forward;
		RB_INSERT(program_status_sent_tree, want, pss);
	}
	free(list);
}

/* Bring a client terminal up to date with the records of its session. */
static void
program_status_forward_flush(struct program_status_forward *psf)
{
	struct tty			*tty = &psf->c->tty;
	struct program_status_sent_tree	 want;
	struct program_status_sent	*pss, *pss1, *found;

	if (program_status_forward_seen(psf))
		return;
	RB_INIT(&want);
	program_status_forward_want(psf, &want);

	/*
	 * Clearing a record also clears every record beneath it in the client
	 * terminal, so forget those too and send them again if they are still
	 * wanted.
	 */
	RB_FOREACH(pss, program_status_sent_tree, &psf->sent) {
		if (RB_FIND(program_status_sent_tree, &want, pss) != NULL)
			continue;
		pss->cleared = 1;
		if (!program_status_ancestor_cleared(&psf->sent, pss->id)) {
			tty_puts(tty, "\033]7501;state=clear:id=");
			tty_puts(tty, pss->id);
			tty_puts(tty, "\033\\");
		}
	}
	RB_FOREACH(pss, program_status_sent_tree, &psf->sent) {
		if (program_status_ancestor_cleared(&psf->sent, pss->id))
			pss->cleared = 1;
	}
	RB_FOREACH_SAFE(pss, program_status_sent_tree, &psf->sent, pss1) {
		if (pss->cleared)
			program_status_remove_sent(&psf->sent, pss);
	}

	RB_FOREACH_SAFE(pss, program_status_sent_tree, &want, pss1) {
		RB_REMOVE(program_status_sent_tree, &want, pss);
		found = RB_FIND(program_status_sent_tree, &psf->sent, pss);
		if (found == NULL || strcmp(found->report, pss->report) != 0) {
			tty_puts(tty, pss->report);
			if (found != NULL)
				program_status_remove_sent(&psf->sent, found);
			pss->id = xstrdup(pss->id);
			pss->report = xstrdup(pss->report);
			RB_INSERT(program_status_sent_tree, &psf->sent, pss);
		} else
			free(pss);
	}
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
	struct timeval			 tv;

	if (!program_status_forward_ready(c))
		return;
	if (c->tty.flags & TTY_BLOCK) {
		tv.tv_sec = 0;
		tv.tv_usec = PROGRAM_STATUS_DELAY * 1000L;
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
	struct timeval			 tv;

	if (!program_status_forward_ready(c))
		return;
	if (psf == NULL) {
		psf = c->tty.program_status = xcalloc(1, sizeof *psf);
		psf->c = c;
		RB_INIT(&psf->sent);
		evtimer_set(&psf->timer, program_status_forward_callback, psf);
	}
	if (!evtimer_pending(&psf->timer, NULL)) {
		tv.tv_sec = 0;
		tv.tv_usec = PROGRAM_STATUS_DELAY * 1000L;
		evtimer_add(&psf->timer, &tv);
	}
}

/* Forward records to every client terminal soon. */
static void
program_status_forward_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry)
		program_status_forward_client(c);
}

/* Forget everything sent to a client terminal, clearing it if needed. */
static void
program_status_forward_clear(struct tty *tty, int raw)
{
	struct program_status_forward	*psf = tty->program_status;
	static const char		 clear[] =
	    "\033]7501;state=clear\033\\";

	if (psf == NULL)
		return;
	evtimer_del(&psf->timer);
	free(psf->seen);
	psf->seen = NULL;
	psf->nseen = 0;
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
	free(psf->seen);
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
