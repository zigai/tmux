/* $OpenBSD$ */

/*
 * Copyright (c) 2026 zigai <zainvanpotan@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHOR
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
 * THIS SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Kitty clipboard protocol (OSC 5522). A write transaction from a pane is
 * collected and sent to each client in one piece, so transactions from
 * different panes never interleave. Requests sent to a client carry an id
 * made by tmux, which routes the replies back to the pane and is replaced by
 * the pane's own id.
 */

#define KITTY_CLIPBOARD_CHUNK 4096
#define KITTY_CLIPBOARD_ID_MAX 512
#define KITTY_CLIPBOARD_WRITE_MAX (64 * 1024 * 1024)
#define KITTY_CLIPBOARD_TYPES_MAX 64
#define KITTY_CLIPBOARD_ALIASES_MAX 64

enum kitty_clipboard_type {
	KITTY_CLIPBOARD_READ,
	KITTY_CLIPBOARD_WRITE,
	KITTY_CLIPBOARD_WDATA,
	KITTY_CLIPBOARD_WALIAS
};

struct kitty_clipboard_meta {
	enum kitty_clipboard_type	 type;
	int				 primary;
	char				*id;
	char				*mime;
	char				*pw;
	char				*name;
	int				 invalid;
};

struct kitty_clipboard_data {
	char		*mime;
	u_char		*data;
	size_t		 size;
};

struct kitty_clipboard_alias {
	char		*alias;
	char		*target;
};

struct kitty_clipboard {
	int				 active;
	int				 primary;
	char				*id;
	char				*pw;
	char				*name;

	struct kitty_clipboard_data	*entries;
	u_int				 nentries;
	int				 current;
	u_char				 carry[4];
	u_int				 ncarry;
	size_t				 total;

	struct kitty_clipboard_alias	*aliases;
	u_int				 naliases;

	struct client			*paste_client;
	char				*paste_pw;
};

struct kitty_clipboard_request {
	char				 id[16];
	u_int				 pane;
	struct client			*c;
	int				 write;
	char				*pane_id;
	const char			*end;
	struct event			 timer;
	int				 late;

	int				 collect;
	char				*textmime;
	u_char				*text;
	size_t				 textsize;

	TAILQ_ENTRY(kitty_clipboard_request) entry;
};
static TAILQ_HEAD(, kitty_clipboard_request) kitty_clipboard_requests =
    TAILQ_HEAD_INITIALIZER(kitty_clipboard_requests);
static u_int kitty_clipboard_next_id;

static int
kitty_clipboard_b64(u_char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return (ch - 'A');
	if (ch >= 'a' && ch <= 'z')
		return (ch - 'a' + 26);
	if (ch >= '0' && ch <= '9')
		return (ch - '0' + 52);
	if (ch == '+')
		return (62);
	if (ch == '/')
		return (63);
	return (-1);
}

/*
 * Decode strict base64, carrying an incomplete group to the next call. Padding
 * may only end the input. Returns -1 if the input is not valid.
 */
static int
kitty_clipboard_decode(u_char *carry, u_int *ncarry, const char *in,
    u_char **out, size_t *outsize)
{
	size_t	 len = strlen(in), i;
	u_char	 ch, *p;
	int	 v[4], j, n;

	*out = xrealloc(*out, *outsize + ((*ncarry + len) / 4) * 3 + 1);
	p = *out + *outsize;
	for (i = 0; i < len; i++) {
		ch = in[i];
		if (ch == '=') {
			if (*ncarry < 2)
				return (-1);
		} else if (kitty_clipboard_b64(ch) == -1)
			return (-1);
		else if (*ncarry == 3 && carry[2] == '=')
			return (-1);
		carry[(*ncarry)++] = ch;
		if (*ncarry != 4)
			continue;
		*ncarry = 0;

		for (j = 0; j < 4; j++) {
			if (carry[j] == '=')
				v[j] = 0;
			else
				v[j] = kitty_clipboard_b64(carry[j]);
		}
		if (carry[2] == '=') {
			if (v[1] & 0xf)
				return (-1);
			n = 1;
		} else if (carry[3] == '=') {
			if (v[2] & 0x3)
				return (-1);
			n = 2;
		} else
			n = 3;
		if (n != 3 && i != len - 1)
			return (-1);

		*p++ = (v[0] << 2) | (v[1] >> 4);
		if (n > 1)
			*p++ = (v[1] << 4) | (v[2] >> 2);
		if (n > 2)
			*p++ = (v[2] << 6) | v[3];
	}
	*outsize = p - *out;
	return (0);
}

/* Decode a complete base64 value as a string. */
static char *
kitty_clipboard_decode_string(const char *in)
{
	u_char	 carry[4], *out = NULL;
	u_int	 ncarry = 0;
	size_t	 size = 0;

	if (kitty_clipboard_decode(carry, &ncarry, in, &out, &size) != 0 ||
	    ncarry != 0 ||
	    memchr(out, '\0', size) != NULL) {
		free(out);
		return (NULL);
	}
	out[size] = '\0';
	if (!utf8_isvalid(out)) {
		free(out);
		return (NULL);
	}
	return (out);
}

static char *
kitty_clipboard_encode(const u_char *data, size_t size)
{
	char	*out;
	size_t	 outsize = 4 * ((size + 2) / 3) + 1;

	out = xmalloc(outsize);
	if (b64_ntop(data, size, out, outsize) == -1)
		fatalx("base64 encode failed");
	return (out);
}

static int
kitty_clipboard_is_text(const char *mime)
{
	return (strcmp(mime, "text/plain") == 0 ||
	    strcmp(mime, "text/plain;charset=utf-8") == 0 ||
	    strcmp(mime, "UTF8_STRING") == 0 ||
	    strcmp(mime, "TEXT") == 0 ||
	    strcmp(mime, "STRING") == 0);
}

static void
kitty_clipboard_free_meta(struct kitty_clipboard_meta *meta)
{
	free(meta->id);
	free(meta->mime);
	free(meta->pw);
	free(meta->name);
}

/*
 * Parse metadata. Returns -1 if the packet should be ignored. A base64 value
 * which is not valid sets the invalid flag.
 */
static int
kitty_clipboard_parse(char *metadata, struct kitty_clipboard_meta *meta)
{
	char		*record, *value, *next = metadata, *s;
	const char	*type = NULL, *id = "", *mime = "", *pw = "";
	const char	*name = "";
	size_t		 n = 0;

	memset(meta, 0, sizeof *meta);
	while ((record = strsep(&next, ":")) != NULL) {
		if ((value = strchr(record, '=')) == NULL)
			return (-1);
		*value++ = '\0';
		if (strcmp(record, "type") == 0)
			type = value;
		else if (strcmp(record, "loc") == 0)
			meta->primary = (strcmp(value, "primary") == 0);
		else if (strcmp(record, "id") == 0)
			id = value;
		else if (strcmp(record, "mime") == 0)
			mime = value;
		else if (strcmp(record, "pw") == 0)
			pw = value;
		else if (strcmp(record, "name") == 0)
			name = value;
	}
	if (type == NULL)
		return (-1);
	if (strcmp(type, "read") == 0)
		meta->type = KITTY_CLIPBOARD_READ;
	else if (strcmp(type, "write") == 0)
		meta->type = KITTY_CLIPBOARD_WRITE;
	else if (strcmp(type, "wdata") == 0)
		meta->type = KITTY_CLIPBOARD_WDATA;
	else if (strcmp(type, "walias") == 0)
		meta->type = KITTY_CLIPBOARD_WALIAS;
	else
		return (-1);

	meta->id = s = xmalloc(strlen(id) + 1);
	for (; *id != '\0' && n < KITTY_CLIPBOARD_ID_MAX; id++) {
		if (isalnum((u_char)*id) || strchr("-_+.", *id) != NULL)
			s[n++] = *id;
	}
	s[n] = '\0';

	if (*mime != '\0') {
		meta->mime = kitty_clipboard_decode_string(mime);
		if (meta->mime == NULL)
			meta->invalid = 1;
		else if (*meta->mime == '\0') {
			free(meta->mime);
			meta->mime = NULL;
		}
	}
	if (*pw != '\0') {
		if ((s = kitty_clipboard_decode_string(pw)) == NULL)
			meta->invalid = 1;
		free(s);
		meta->pw = xstrdup(pw);
	}
	if (*name != '\0') {
		if ((s = kitty_clipboard_decode_string(name)) == NULL)
			meta->invalid = 1;
		free(s);
		meta->name = xstrdup(name);
	}
	return (0);
}

/* Add a packet to a buffer. */
static void
kitty_clipboard_packet(struct evbuffer *evb, const char *meta, const char *id,
    const char *mime, const u_char *data, size_t size, const char *end)
{
	char	*encoded;

	evbuffer_add_printf(evb, "\033]5522;%s", meta);
	if (id != NULL && *id != '\0')
		evbuffer_add_printf(evb, ":id=%s", id);
	if (mime != NULL) {
		encoded = kitty_clipboard_encode(mime, strlen(mime));
		evbuffer_add_printf(evb, ":mime=%s", encoded);
		free(encoded);
	}
	if (size != 0) {
		encoded = kitty_clipboard_encode(data, size);
		evbuffer_add_printf(evb, ";%s", encoded);
		free(encoded);
	}
	evbuffer_add(evb, end, strlen(end));
}

/* Add a data packet for each chunk. */
static void
kitty_clipboard_chunks(struct evbuffer *evb, const char *meta, const char *id,
    const char *mime, const u_char *data, size_t size, const char *end)
{
	size_t	 n;

	if (size == 0) {
		kitty_clipboard_packet(evb, meta, id, mime, NULL, 0, end);
		return;
	}
	while (size != 0) {
		n = size;
		if (n > KITTY_CLIPBOARD_CHUNK)
			n = KITTY_CLIPBOARD_CHUNK;
		kitty_clipboard_packet(evb, meta, id, mime, data, n, end);
		data += n;
		size -= n;
	}
}

static void
kitty_clipboard_send_pane(struct window_pane *wp, struct evbuffer *evb)
{
	if (wp->event != NULL)
		bufferevent_write_buffer(wp->event, evb);
}

static void
kitty_clipboard_send_tty(struct tty *tty, const char *s)
{
	tty->flags |= TTY_NOBLOCK;
	tty_puts(tty, s);
}

static void
kitty_clipboard_status(struct window_pane *wp, const char *type,
    const char *id, const char *status, const char *end)
{
	struct evbuffer	*evb;
	char		*meta;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	xasprintf(&meta, "type=%s:status=%s", type, status);
	kitty_clipboard_packet(evb, meta, id, NULL, NULL, 0, end);
	kitty_clipboard_send_pane(wp, evb);
	free(meta);
	evbuffer_free(evb);
}

/* Can this client be used for a pane in this window? */
static int
kitty_clipboard_client_ready(struct client *c, struct window *w)
{
	if (c->flags & CLIENT_UNATTACHEDFLAGS)
		return (0);
	if (c->session == NULL || !session_has(c->session, w))
		return (0);
	if (~c->tty.flags & TTY_STARTED)
		return (0);
	if (c->tty.flags & TTY_BLOCK)
		return (0);
	return (1);
}

static void
kitty_clipboard_free_request(struct kitty_clipboard_request *r)
{
	evtimer_del(&r->timer);
	TAILQ_REMOVE(&kitty_clipboard_requests, r, entry);
	free(r->pane_id);
	free(r->textmime);
	free(r->text);
	free(r);
}

/*
 * The terminal may be asking the user, so a request it has not answered keeps
 * waiting, but no longer makes keys wait for a reply.
 */
static void
kitty_clipboard_timer(__unused int fd, __unused short events, void *data)
{
	struct kitty_clipboard_request	*r = data;

	r->late = 1;
}

static struct kitty_clipboard_request *
kitty_clipboard_add_request(struct window_pane *wp, struct client *c,
    int write, const char *id, const char *end)
{
	struct kitty_clipboard_request	*r;
	struct timeval			 tv = { .tv_sec = TTY_QUERY_TIMEOUT };

	r = xcalloc(1, sizeof *r);
	xsnprintf(r->id, sizeof r->id, "tmux-%u", ++kitty_clipboard_next_id);
	r->pane = wp->id;
	r->c = c;
	r->write = write;
	r->pane_id = xstrdup(id);
	r->end = end;
	TAILQ_INSERT_TAIL(&kitty_clipboard_requests, r, entry);
	evtimer_set(&r->timer, kitty_clipboard_timer, r);
	evtimer_add(&r->timer, &tv);
	return (r);
}

/* Forget any write transaction. */
static void
kitty_clipboard_reset(struct kitty_clipboard *kc)
{
	u_int	i;

	for (i = 0; i < kc->nentries; i++) {
		free(kc->entries[i].mime);
		free(kc->entries[i].data);
	}
	free(kc->entries);
	for (i = 0; i < kc->naliases; i++) {
		free(kc->aliases[i].alias);
		free(kc->aliases[i].target);
	}
	free(kc->aliases);
	free(kc->id);
	free(kc->pw);
	free(kc->name);

	kc->active = 0;
	kc->primary = 0;
	kc->id = kc->pw = kc->name = NULL;
	kc->entries = NULL;
	kc->nentries = 0;
	kc->current = -1;
	kc->ncarry = 0;
	kc->total = 0;
	kc->aliases = NULL;
	kc->naliases = 0;
}

static struct kitty_clipboard *
kitty_clipboard_get(struct window_pane *wp)
{
	if (wp->kitty_clipboard == NULL) {
		wp->kitty_clipboard = xcalloc(1, sizeof *wp->kitty_clipboard);
		wp->kitty_clipboard->current = -1;
	}
	return (wp->kitty_clipboard);
}

/*
 * A pane has one request at a time waiting for the terminal, which may be
 * asking the user about it, so a new request is refused until it is answered.
 */
static int
kitty_clipboard_busy(struct window_pane *wp)
{
	struct kitty_clipboard_request	*r;

	TAILQ_FOREACH(r, &kitty_clipboard_requests, entry) {
		if (r->pane == wp->id)
			return (1);
	}
	return (0);
}

/* Fail a write transaction. */
static void
kitty_clipboard_write_fail(struct window_pane *wp, const char *status,
    const char *end)
{
	struct kitty_clipboard	*kc = wp->kitty_clipboard;

	kitty_clipboard_status(wp, "write", kc->id, status, end);
	kitty_clipboard_reset(kc);
}

static void
kitty_clipboard_write_begin(struct window_pane *wp,
    struct kitty_clipboard_meta *meta, const char *end)
{
	struct kitty_clipboard	*kc = kitty_clipboard_get(wp);

	kitty_clipboard_reset(kc);

	if (options_get_number(global_options, "set-clipboard") != 2) {
		kitty_clipboard_status(wp, "write", meta->id, "EPERM", end);
		return;
	}

	kc->active = 1;
	kc->primary = meta->primary;
	kc->id = xstrdup(meta->id);
	if (meta->pw != NULL)
		kc->pw = xstrdup(meta->pw);
	if (meta->name != NULL)
		kc->name = xstrdup(meta->name);
}

static const char *
kitty_clipboard_write_data(struct kitty_clipboard *kc, const char *mime,
    const char *payload)
{
	struct kitty_clipboard_data	*kd;
	u_int				 i;
	size_t				 size;

	if (kc->current == -1 || strcmp(kc->entries[kc->current].mime,
	    mime) != 0) {
		if (kc->current != -1 && kc->ncarry != 0)
			return ("EINVAL");
		kc->current = -1;
		for (i = 0; i < kc->nentries; i++) {
			if (strcmp(kc->entries[i].mime, mime) == 0)
				break;
		}
		if (i == kc->nentries &&
		    kc->nentries == KITTY_CLIPBOARD_TYPES_MAX)
			return (NULL);
		if (i == kc->nentries) {
			kc->entries = xreallocarray(kc->entries,
			    kc->nentries + 1, sizeof *kc->entries);
			kd = &kc->entries[kc->nentries++];
			kd->mime = xstrdup(mime);
			kd->data = NULL;
		} else
			kd = &kc->entries[i];
		kd->size = 0;
		kc->current = i;
	}

	kd = &kc->entries[kc->current];
	size = kd->size;
	if (kitty_clipboard_decode(kc->carry, &kc->ncarry, payload, &kd->data,
	    &kd->size) != 0)
		return ("EINVAL");
	kc->total += kd->size - size;
	if (kc->total > KITTY_CLIPBOARD_WRITE_MAX)
		return ("EFBIG");
	return (NULL);
}

static int
kitty_clipboard_write_alias(struct kitty_clipboard *kc, const char *target,
    const char *payload)
{
	struct kitty_clipboard_alias	*ka;
	char				*decoded, *next, *name;
	u_int				 i;

	if ((decoded = kitty_clipboard_decode_string(payload)) == NULL)
		return (-1);
	next = decoded;
	while ((name = strsep(&next, " \t\n\r\f\v")) != NULL) {
		if (*name == '\0')
			continue;
		for (i = 0; i < kc->naliases; i++) {
			if (strcmp(kc->aliases[i].alias, name) == 0)
				break;
		}
		if (i == kc->naliases &&
		    kc->naliases == KITTY_CLIPBOARD_ALIASES_MAX)
			break;
		if (i == kc->naliases) {
			kc->aliases = xreallocarray(kc->aliases,
			    kc->naliases + 1, sizeof *kc->aliases);
			ka = &kc->aliases[kc->naliases++];
			ka->alias = xstrdup(name);
		} else {
			ka = &kc->aliases[i];
			free(ka->target);
		}
		ka->target = xstrdup(target);
	}
	free(decoded);
	return (0);
}

/* Find the text in a transaction, after applying aliases. */
static struct kitty_clipboard_data *
kitty_clipboard_find_text(struct kitty_clipboard *kc)
{
	struct kitty_clipboard_data	*all, *kd, *found = NULL;
	u_int				 n, i, j;

	all = xreallocarray(NULL, kc->nentries + kc->naliases + 1,
	    sizeof *all);
	memcpy(all, kc->entries, kc->nentries * sizeof *all);
	n = kc->nentries;
	for (i = 0; i < kc->naliases; i++) {
		for (j = 0; j < n; j++) {
			if (strcmp(all[j].mime, kc->aliases[i].target) == 0)
				break;
		}
		if (j == n)
			continue;
		kd = &all[j];
		for (j = 0; j < n; j++) {
			if (strcmp(all[j].mime, kc->aliases[i].alias) == 0)
				break;
		}
		if (j == n)
			n++;
		all[j].mime = kc->aliases[i].alias;
		all[j].data = kd->data;
		all[j].size = kd->size;
	}

	for (i = 0; i < n; i++) {
		if (strcmp(all[i].mime, "text/plain") == 0)
			break;
	}
	if (i == n) {
		for (i = 0; i < n; i++) {
			if (kitty_clipboard_is_text(all[i].mime))
				break;
		}
	}
	if (i != n && all[i].size != 0) {
		found = xmalloc(sizeof *found);
		memcpy(found, &all[i], sizeof *found);
	}
	free(all);
	return (found);
}

/* Send a complete write transaction to clients and keep text as a buffer. */
static void
kitty_clipboard_write_commit(struct window_pane *wp, const char *end)
{
	struct kitty_clipboard		*kc = wp->kitty_clipboard;
	struct kitty_clipboard_data	*kd, *text;
	struct kitty_clipboard_alias	*ka;
	struct kitty_clipboard_request	*r = NULL;
	struct client			*c, *best = NULL;
	struct evbuffer			*evb;
	char				*meta;
	u_int				 i;

	if (kc->current != -1 && kc->ncarry != 0) {
		kitty_clipboard_write_fail(wp, "EINVAL", end);
		return;
	}
	if (kitty_clipboard_busy(wp)) {
		kitty_clipboard_write_fail(wp, "EBUSY", end);
		return;
	}
	text = kitty_clipboard_find_text(kc);

	TAILQ_FOREACH(c, &clients, entry) {
		if (!kitty_clipboard_client_ready(c, wp->window))
			continue;
		if (~c->tty.term->flags & TERM_KITTYCLIPBOARD)
			continue;
		if (best == NULL ||
		    timercmp(&c->activity_time, &best->activity_time, >))
			best = c;
	}
	if (best != NULL)
		r = kitty_clipboard_add_request(wp, best, 1, kc->id, end);

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	if (r != NULL) {
		xasprintf(&meta, "type=write%s:id=%s%s%s%s%s",
		    kc->primary ? ":loc=primary" : "", r->id,
		    kc->pw != NULL ? ":pw=" : "", kc->pw != NULL ? kc->pw : "",
		    kc->name != NULL ? ":name=" : "",
		    kc->name != NULL ? kc->name : "");
		kitty_clipboard_packet(evb, meta, NULL, NULL, NULL, 0,
		    "\033\\");
		free(meta);
		for (i = 0; i < kc->nentries; i++) {
			kd = &kc->entries[i];
			kitty_clipboard_chunks(evb, "type=wdata", NULL,
			    kd->mime, kd->data, kd->size, "\033\\");
		}
		for (i = 0; i < kc->naliases; i++) {
			ka = &kc->aliases[i];
			kitty_clipboard_packet(evb, "type=walias", NULL,
			    ka->target, ka->alias, strlen(ka->alias),
			    "\033\\");
		}
		kitty_clipboard_packet(evb, "type=wdata", NULL, NULL, NULL, 0,
		    "\033\\");
	}
	evbuffer_add(evb, "", 1);

	TAILQ_FOREACH(c, &clients, entry) {
		if (!kitty_clipboard_client_ready(c, wp->window))
			continue;
		if (c->tty.term->flags & TERM_KITTYCLIPBOARD)
			kitty_clipboard_send_tty(&c->tty, EVBUFFER_DATA(evb));
		else if (text != NULL) {
			tty_set_selection(&c->tty, kc->primary ? "p" : "",
			    text->data, text->size);
		}
	}
	evbuffer_free(evb);

	if (text != NULL) {
		paste_add(NULL, xmemdup(text->data, text->size), text->size);
		events_fire_pane("pane-set-clipboard", wp);
	}
	if (r == NULL) {
		kitty_clipboard_status(wp, "write", kc->id,
		    text != NULL ? "DONE" : "ENOSYS", end);
	}
	free(text);
	kitty_clipboard_reset(kc);
}

/* Answer a read request from the newest paste buffer. */
static void
kitty_clipboard_read_buffer(struct window_pane *wp,
    struct kitty_clipboard_meta *meta, char *mimes, const char *end)
{
	struct paste_buffer	*pb = paste_get_top(NULL);
	struct evbuffer		*evb;
	const char		*data = NULL;
	char			*mime, *next = mimes, **texts = NULL;
	size_t			 size = 0;
	u_int			 ntexts = 0, i;
	int			 list = 0;

	if (pb != NULL)
		data = paste_buffer_data(pb, &size);
	while ((mime = strsep(&next, " \t\n\r\f\v")) != NULL) {
		if (strcmp(mime, ".") == 0)
			list = 1;
		else if (kitty_clipboard_is_text(mime)) {
			texts = xreallocarray(texts, ntexts + 1,
			    sizeof *texts);
			texts[ntexts++] = mime;
		}
	}

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	kitty_clipboard_packet(evb, meta->primary ?
	    "type=read:status=OK:loc=primary" : "type=read:status=OK",
	    meta->id, NULL, NULL, 0, end);
	if (list) {
		kitty_clipboard_packet(evb, "type=read:status=DATA", meta->id,
		    ".", "text/plain\n", size != 0 ? 11 : 0, end);
	}
	for (i = 0; size != 0 && i < ntexts; i++) {
		kitty_clipboard_chunks(evb, "type=read:status=DATA", meta->id,
		    texts[i], data, size, end);
	}
	kitty_clipboard_packet(evb, "type=read:status=DONE", meta->id, NULL,
	    NULL, 0, end);
	kitty_clipboard_send_pane(wp, evb);
	evbuffer_free(evb);
	free(texts);
}

static void
kitty_clipboard_read(struct window_pane *wp,
    struct kitty_clipboard_meta *meta, const char *payload, const char *end)
{
	struct kitty_clipboard		*kc;
	struct kitty_clipboard_request	*r;
	struct client			*c, *best = NULL;
	char				*mimes, *out;
	int				 state;

	if ((mimes = kitty_clipboard_decode_string(payload)) == NULL)
		return;

	state = options_get_number(global_options, "get-clipboard");
	if (state == 0 ||
	    options_get_number(global_options, "set-clipboard") != 2) {
		kitty_clipboard_status(wp, "read", meta->id, "EPERM", end);
		free(mimes);
		return;
	}
	if (kitty_clipboard_busy(wp)) {
		kitty_clipboard_status(wp, "read", meta->id, "EBUSY", end);
		free(mimes);
		return;
	}

	/* A read after a paste event goes to the client which sent it. */
	kc = wp->kitty_clipboard;
	if (state != 1 && kc != NULL && kc->paste_client != NULL &&
	    meta->pw != NULL && strcmp(meta->pw, kc->paste_pw) == 0) {
		c = kc->paste_client;
		if (kitty_clipboard_client_ready(c, wp->window) &&
		    (c->tty.term->flags & TERM_KITTYCLIPBOARD))
			best = c;
	}
	if (state != 1 && best == NULL) {
		TAILQ_FOREACH(c, &clients, entry) {
			if (!kitty_clipboard_client_ready(c, wp->window))
				continue;
			if (~c->tty.term->flags & TERM_KITTYCLIPBOARD)
				continue;
			if (best == NULL ||
			    timercmp(&c->activity_time, &best->activity_time,
			    >))
				best = c;
		}
	}
	if (best == NULL) {
		kitty_clipboard_read_buffer(wp, meta, mimes, end);
		free(mimes);
		return;
	}
	free(mimes);

	r = kitty_clipboard_add_request(wp, best, 0, meta->id, end);
	r->collect = (state == 3);

	xasprintf(&out, "\033]5522;type=read%s:id=%s%s%s%s%s%s%s\033\\",
	    meta->primary ? ":loc=primary" : "", r->id,
	    meta->pw != NULL ? ":pw=" : "", meta->pw != NULL ? meta->pw : "",
	    meta->name != NULL ? ":name=" : "",
	    meta->name != NULL ? meta->name : "",
	    *payload != '\0' ? ";" : "", payload);
	kitty_clipboard_send_tty(&best->tty, out);
	free(out);
}

/* Handle an OSC 5522 sequence from a pane. */
void
kitty_clipboard_input(struct window_pane *wp, const char *p, const char *end)
{
	struct kitty_clipboard		*kc = wp->kitty_clipboard;
	struct kitty_clipboard_meta	 meta;
	const char			*payload, *status;
	char				*metadata;

	if ((payload = strchr(p, ';')) == NULL) {
		metadata = xstrdup(p);
		payload = "";
	} else {
		metadata = xstrndup(p, payload - p);
		payload++;
	}
	if (kitty_clipboard_parse(metadata, &meta) != 0) {
		kitty_clipboard_free_meta(&meta);
		free(metadata);
		return;
	}
	free(metadata);

	if (meta.invalid) {
		if (kc != NULL && kc->active &&
		    (meta.type == KITTY_CLIPBOARD_WDATA ||
		    meta.type == KITTY_CLIPBOARD_WALIAS))
			kitty_clipboard_write_fail(wp, "EINVAL", end);
		kitty_clipboard_free_meta(&meta);
		return;
	}

	switch (meta.type) {
	case KITTY_CLIPBOARD_READ:
		kitty_clipboard_read(wp, &meta, payload, end);
		break;
	case KITTY_CLIPBOARD_WRITE:
		kitty_clipboard_write_begin(wp, &meta, end);
		break;
	case KITTY_CLIPBOARD_WDATA:
		if (kc == NULL || !kc->active)
			break;
		if (meta.mime == NULL)
			kitty_clipboard_write_commit(wp, end);
		else if ((status = kitty_clipboard_write_data(kc, meta.mime,
		    payload)) != NULL)
			kitty_clipboard_write_fail(wp, status, end);
		break;
	case KITTY_CLIPBOARD_WALIAS:
		if (kc == NULL || !kc->active)
			break;
		if (meta.mime == NULL ||
		    kitty_clipboard_write_alias(kc, meta.mime, payload) != 0)
			kitty_clipboard_write_fail(wp, "EINVAL", end);
		break;
	}
	kitty_clipboard_free_meta(&meta);
}

/* Keep text from a read reply to make a buffer when it is done. */
static void
kitty_clipboard_collect(struct kitty_clipboard_request *r, const char *mime,
    const char *payload)
{
	char	*decoded;
	u_char	 carry[4];
	u_int	 ncarry = 0;

	if (mime == NULL)
		return;
	decoded = kitty_clipboard_decode_string(mime);
	if (decoded == NULL || !kitty_clipboard_is_text(decoded)) {
		free(decoded);
		return;
	}
	if (r->textmime == NULL)
		r->textmime = decoded;
	else {
		if (strcmp(r->textmime, decoded) != 0) {
			free(decoded);
			return;
		}
		free(decoded);
	}
	if (kitty_clipboard_decode(carry, &ncarry, payload, &r->text,
	    &r->textsize) != 0 || ncarry != 0) {
		free(r->text);
		r->text = NULL;
		r->textsize = 0;
		r->collect = 0;
	}
}

/*
 * Handle an OSC 5522 reply from a client (without the leading \033]5522; or
 * the terminator).
 */
void
kitty_clipboard_reply(struct client *c, const char *buf, size_t len)
{
	struct kitty_clipboard		*kc;
	struct kitty_clipboard_request	*r;
	struct window_pane		*wp;
	struct evbuffer			*evb;
	struct timeval			 tv = { .tv_sec = TTY_QUERY_TIMEOUT };
	char				*copy, *metadata, *record, *value;
	char				*payload, *next;
	const char			*type = "", *status = "", *id = "";
	const char			*mime = NULL, *pw = NULL;
	int				 first = 1, done;

	copy = xstrndup(buf, len);
	if ((payload = strchr(copy, ';')) != NULL)
		*payload++ = '\0';
	metadata = xstrdup(copy);

	next = copy;
	while ((record = strsep(&next, ":")) != NULL) {
		if ((value = strchr(record, '=')) == NULL)
			continue;
		*value++ = '\0';
		if (strcmp(record, "type") == 0)
			type = value;
		else if (strcmp(record, "status") == 0)
			status = value;
		else if (strcmp(record, "id") == 0)
			id = value;
		else if (strcmp(record, "mime") == 0)
			mime = value;
		else if (strcmp(record, "pw") == 0)
			pw = value;
	}
	log_debug("%s: %s type=%s status=%s id=%s", __func__, c->name, type,
	    status, id);

	if (*id == '\0') {
		/* A paste event goes to the active pane. */
		r = NULL;
		wp = NULL;
		if (strcmp(type, "read") == 0 && c->session != NULL)
			wp = c->session->curw->window->active;
		if (wp == NULL || (~wp->screen->mode & MODE_PASTE_EVENTS))
			goto out;
		if (pw != NULL && strcmp(status, "OK") == 0) {
			kc = kitty_clipboard_get(wp);
			free(kc->paste_pw);
			kc->paste_pw = xstrdup(pw);
			kc->paste_client = c;
		}
	} else {
		TAILQ_FOREACH(r, &kitty_clipboard_requests, entry) {
			if (r->c == c && strcmp(r->id, id) == 0)
				break;
		}
		if (r == NULL)
			goto out;
		wp = window_pane_find_by_id(r->pane);
		if (wp == NULL) {
			kitty_clipboard_free_request(r);
			goto out;
		}
		r->late = 0;
		evtimer_add(&r->timer, &tv);
	}

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add(evb, "\033]5522;", 7);
	next = metadata;
	while ((record = strsep(&next, ":")) != NULL) {
		if (r != NULL && strncmp(record, "id=", 3) == 0) {
			if (*r->pane_id == '\0')
				continue;
			evbuffer_add_printf(evb, "%sid=%s", first ? "" : ":",
			    r->pane_id);
		} else {
			evbuffer_add_printf(evb, "%s%s", first ? "" : ":",
			    record);
		}
		first = 0;
	}
	if (payload != NULL)
		evbuffer_add_printf(evb, ";%s", payload);
	evbuffer_add_printf(evb, "%s", r != NULL ? r->end : "\033\\");
	kitty_clipboard_send_pane(wp, evb);
	evbuffer_free(evb);

	if (r == NULL)
		goto out;
	if (r->write)
		done = 1;
	else {
		done = (strcmp(status, "OK") != 0 &&
		    strcmp(status, "DATA") != 0);
		if (r->collect && payload != NULL &&
		    strcmp(status, "DATA") == 0)
			kitty_clipboard_collect(r, mime, payload);
	}
	if (done) {
		if (r->collect && r->text != NULL &&
		    strcmp(status, "DONE") == 0) {
			paste_add(NULL, (char *)r->text, r->textsize);
			r->text = NULL;
		}
		kitty_clipboard_free_request(r);
	}

out:
	free(metadata);
	free(copy);
}

/* Free clipboard state for a pane. */
void
kitty_clipboard_free(struct window_pane *wp)
{
	struct kitty_clipboard_request	*r, *r1;

	TAILQ_FOREACH_SAFE(r, &kitty_clipboard_requests, entry, r1) {
		if (r->pane == wp->id)
			kitty_clipboard_free_request(r);
	}
	if (wp->kitty_clipboard != NULL) {
		kitty_clipboard_reset(wp->kitty_clipboard);
		free(wp->kitty_clipboard->paste_pw);
		free(wp->kitty_clipboard);
		wp->kitty_clipboard = NULL;
	}
}

/* Is a request waiting for this client to answer, and not yet late? */
int
kitty_clipboard_waiting(struct client *c)
{
	struct kitty_clipboard_request	*r;

	TAILQ_FOREACH(r, &kitty_clipboard_requests, entry) {
		if (r->c == c && !r->late)
			return (1);
	}
	return (0);
}

/* Fail requests waiting for a client which has gone. */
void
kitty_clipboard_client_lost(struct client *c)
{
	struct kitty_clipboard_request	*r, *r1;
	struct window_pane		*wp;

	TAILQ_FOREACH_SAFE(r, &kitty_clipboard_requests, entry, r1) {
		if (r->c != c)
			continue;
		if ((wp = window_pane_find_by_id(r->pane)) != NULL) {
			kitty_clipboard_status(wp, r->write ? "write" : "read",
			    r->pane_id, "EBUSY", r->end);
		}
		kitty_clipboard_free_request(r);
	}
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->kitty_clipboard != NULL &&
		    wp->kitty_clipboard->paste_client == c)
			wp->kitty_clipboard->paste_client = NULL;
	}
}

/* Forget a write transaction when the terminal is reset. */
void
kitty_clipboard_reset_pane(struct window_pane *wp)
{
	if (wp->kitty_clipboard != NULL)
		kitty_clipboard_reset(wp->kitty_clipboard);
}
