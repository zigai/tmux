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
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <ctype.h>
#include <limits.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Desktop notifications from panes. OSC 9 and OSC 777 are passed to clients
 * as they are. Kitty OSC 99 notifications are assembled from their chunks for
 * each pane and written to each client in one piece, with an identifier that
 * names the pane so that replies from the terminal can be returned to it.
 */

/* Notifications kept for replies, as kitty does. */
#define NOTIFICATION_ENTRIES 128

/* Icons kept for later notifications with the same key. */
#define NOTIFICATION_ICONS 16

/* Largest chunk of payload before encoding allowed by the protocol. */
#define NOTIFICATION_CHUNK 2048

enum notification_type {
	NOTIFICATION_TITLE,
	NOTIFICATION_BODY,
	NOTIFICATION_BUTTONS,
	NOTIFICATION_ICON,
	NOTIFICATION_UNKNOWN
};
static const char *notification_type_names[] = {
	"title",
	"body",
	"buttons",
	"icon"
};

struct notification_payload {
	u_char				*data;
	size_t				 size;

	char				*encoded;
	size_t				 encodedlen;
};

struct notification_key {
	char				 key;
	char				*value;

	TAILQ_ENTRY(notification_key)	 entry;
};
TAILQ_HEAD(notification_keys, notification_key);

struct notification {
	char				*id;
	struct notification_keys	 keys;
	struct notification_payload	 payloads[NOTIFICATION_UNKNOWN];
};

struct notification_entry {
	char				*id;
	u_int				 number;

	int				 flags;
#define NOTIFICATION_FOCUS 0x1
#define NOTIFICATION_REPORT 0x2
#define NOTIFICATION_CLOSE 0x4
#define NOTIFICATION_UNTRACKED 0x8

	TAILQ_ENTRY(notification_entry)	 entry;
};
TAILQ_HEAD(notification_entries, notification_entry);

struct notification_icon {
	char				*key;
	u_char				*data;
	size_t				 size;

	TAILQ_ENTRY(notification_icon)	 entry;
};
TAILQ_HEAD(notification_icons, notification_icon);

struct notification_query {
	char				*id;

	TAILQ_ENTRY(notification_query)	 entry;
};
TAILQ_HEAD(notification_queries, notification_query);

struct notifications {
	struct notification		*pending;

	struct notification_entries	 entries;
	u_int				 nentries;

	struct notification_icons	 icons;
	u_int				 nicons;

	struct notification_queries	 queries;
};

/* Numbers for notifications without an identifier, never reused. */
static u_int	notification_next;

static struct notifications *
notification_get(struct window_pane *wp)
{
	struct notifications	*ns = wp->notifications;

	if (ns == NULL) {
		ns = wp->notifications = xcalloc(1, sizeof *ns);
		TAILQ_INIT(&ns->entries);
		TAILQ_INIT(&ns->icons);
		TAILQ_INIT(&ns->queries);
	}
	return (ns);
}

static int
notification_client_ok(struct client *c, struct window_pane *wp,
    enum tty_code_code code)
{
	if (c->session == NULL || (c->flags & CLIENT_CONTROL))
		return (0);
	if (c->flags & CLIENT_UNATTACHEDFLAGS)
		return (0);
	if (~c->tty.flags & TTY_STARTED)
		return (0);
	if (wp != NULL && !session_has(c->session, wp->window))
		return (0);
	return (tty_term_has(c->tty.term, code));
}

/* Forward an OSC 9 notification. */
void
notification_osc9(struct window_pane *wp, const char *message)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!notification_client_ok(c, wp, TTYC_SNOT9))
			continue;
		c->tty.flags |= TTY_NOBLOCK;
		tty_putcode_s(&c->tty, TTYC_SNOT9, message);
	}
}

/* Forward an OSC 777 notification. */
void
notification_osc777(struct window_pane *wp, const char *title,
    const char *body)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!notification_client_ok(c, wp, TTYC_SNOT777))
			continue;
		c->tty.flags |= TTY_NOBLOCK;
		tty_putcode_ss(&c->tty, TTYC_SNOT777, title, body);
	}
}

static void
notification_write(struct client *c, const char *metadata,
    const char *payload)
{
	log_debug("%s: %s: %s;%s", __func__, c->name, metadata, payload);
	c->tty.flags |= TTY_NOBLOCK;
	tty_putcode_ss(&c->tty, TTYC_SNOT99, metadata, payload);
}

static void
notification_reply(struct window_pane *wp, const char *metadata,
    const char *payload)
{
	log_debug("%s: %%%u: %s;%s", __func__, wp->id, metadata, payload);
	if (wp->event == NULL)
		return;
	bufferevent_write(wp->event, "\033]99;", 5);
	bufferevent_write(wp->event, metadata, strlen(metadata));
	bufferevent_write(wp->event, ";", 1);
	bufferevent_write(wp->event, payload, strlen(payload));
	bufferevent_write(wp->event, "\033\\", 2);
}

static char *
notification_outer_id(struct window_pane *wp, const char *id, u_int number)
{
	char	*oid;

	if (id != NULL)
		xasprintf(&oid, "tmux-%ld-%u-i%s", (long)getpid(), wp->id, id);
	else {
		xasprintf(&oid, "tmux-%ld-%u-n%u", (long)getpid(), wp->id,
		    number);
	}
	return (oid);
}

/*
 * Find the pane named by an identifier from notification_outer_id and return
 * the rest of the identifier after the pane.
 */
static struct window_pane *
notification_find(const char *oid, const char **rest)
{
	char		 prefix[64], *copy, *cp;
	const char	*errstr;
	size_t		 len;
	u_int		 id;

	len = xsnprintf(prefix, sizeof prefix, "tmux-%ld-", (long)getpid());
	if (strncmp(oid, prefix, len) != 0)
		return (NULL);
	copy = xstrdup(oid + len);
	cp = strchr(copy, '-');
	if (cp == NULL || (cp[1] != 'i' && cp[1] != 'n')) {
		free(copy);
		return (NULL);
	}
	*cp = '\0';
	id = strtonum(copy, 0, UINT_MAX, &errstr);
	if (errstr != NULL) {
		free(copy);
		return (NULL);
	}
	*rest = oid + len + (cp - copy) + 1;
	free(copy);
	return (window_pane_find_by_id(id));
}

static struct notification_entry *
notification_find_entry(struct notifications *ns, const char *rest)
{
	struct notification_entry	*ne;
	const char			*errstr;
	u_int				 number;

	if (*rest == 'i') {
		TAILQ_FOREACH(ne, &ns->entries, entry) {
			if (ne->id != NULL && strcmp(ne->id, rest + 1) == 0)
				return (ne);
		}
		return (NULL);
	}
	number = strtonum(rest + 1, 0, UINT_MAX, &errstr);
	if (errstr != NULL)
		return (NULL);
	TAILQ_FOREACH(ne, &ns->entries, entry) {
		if (ne->id == NULL && ne->number == number)
			return (ne);
	}
	return (NULL);
}

static void
notification_remove_entry(struct notifications *ns,
    struct notification_entry *ne)
{
	TAILQ_REMOVE(&ns->entries, ne, entry);
	ns->nentries--;
	free(ne->id);
	free(ne);
}

/* Close a notification in every client except one. */
static void
notification_close_others(struct client *except, const char *oid)
{
	struct client	*c;
	char		*metadata;

	xasprintf(&metadata, "i=%s:p=close", oid);
	TAILQ_FOREACH(c, &clients, entry) {
		if (c != except && notification_client_ok(c, NULL, TTYC_SNOT99))
			notification_write(c, metadata, "");
	}
	free(metadata);
}

static void
notification_free_one(struct notification *n)
{
	struct notification_key	*nk, *nk1;
	u_int			 i;

	if (n == NULL)
		return;
	TAILQ_FOREACH_SAFE(nk, &n->keys, entry, nk1) {
		TAILQ_REMOVE(&n->keys, nk, entry);
		free(nk->value);
		free(nk);
	}
	for (i = 0; i < nitems(n->payloads); i++) {
		free(n->payloads[i].data);
		free(n->payloads[i].encoded);
	}
	free(n->id);
	free(n);
}

/* Free notification state for a pane. */
void
notification_free(struct window_pane *wp)
{
	struct notifications		*ns = wp->notifications;
	struct notification_entry	*ne, *ne1;
	struct notification_icon	*ni, *ni1;
	struct notification_query	*nq, *nq1;

	if (ns == NULL)
		return;
	notification_free_one(ns->pending);
	TAILQ_FOREACH_SAFE(ne, &ns->entries, entry, ne1)
		notification_remove_entry(ns, ne);
	TAILQ_FOREACH_SAFE(ni, &ns->icons, entry, ni1) {
		TAILQ_REMOVE(&ns->icons, ni, entry);
		free(ni->key);
		free(ni->data);
		free(ni);
	}
	TAILQ_FOREACH_SAFE(nq, &ns->queries, entry, nq1) {
		TAILQ_REMOVE(&ns->queries, nq, entry);
		free(nq->id);
		free(nq);
	}
	free(ns);
	wp->notifications = NULL;
}

static const char *
notification_get_key(struct notification *n, char key)
{
	struct notification_key	*nk;

	TAILQ_FOREACH(nk, &n->keys, entry) {
		if (nk->key == key)
			return (nk->value);
	}
	return (NULL);
}

/*
 * Add a key to a notification. Icon names and types may be given more than
 * once; other keys replace any earlier value, as kitty does across chunks.
 */
static void
notification_set_key(struct notification *n, char key, const char *value)
{
	struct notification_key	*nk, *nk1;

	if (key != 'n' && key != 't') {
		TAILQ_FOREACH_SAFE(nk, &n->keys, entry, nk1) {
			if (nk->key == key) {
				TAILQ_REMOVE(&n->keys, nk, entry);
				free(nk->value);
				free(nk);
			}
		}
	}
	nk = xmalloc(sizeof *nk);
	nk->key = key;
	nk->value = xstrdup(value);
	TAILQ_INSERT_TAIL(&n->keys, nk, entry);
}

static void
notification_append(struct notification_payload *np, const void *buf,
    size_t len)
{
	np->data = xrealloc(np->data, np->size + len);
	memcpy(np->data + np->size, buf, len);
	np->size += len;
}

/*
 * Decode base64 data. Chunks may be split anywhere in the encoded data, or be
 * encoded separately with their own padding, so decode only whole groups
 * unless padding has been seen or this is the end of the payload.
 */
static void
notification_decode(struct notification_payload *np, int final)
{
	char	*copy;
	u_char	*out;
	size_t	 len = np->encodedlen, used;
	int	 outlen;

	if (len == 0)
		return;
	if (memchr(np->encoded, '=', len) != NULL)
		final = 1;
	if (final) {
		used = len;
		if (len % 4 == 1)
			len--;
	} else
		used = len = len - len % 4;

	if (len != 0) {
		copy = xmalloc(len + 3);
		memcpy(copy, np->encoded, len);
		while (len % 4 != 0)
			copy[len++] = '=';
		copy[len] = '\0';
		out = xmalloc(len / 4 * 3);
		outlen = b64_pton(copy, out, len / 4 * 3);
		if (outlen == -1)
			log_debug("%s: bad base64", __func__);
		else
			notification_append(np, out, outlen);
		free(out);
		free(copy);
	}

	memmove(np->encoded, np->encoded + used, np->encodedlen - used);
	np->encodedlen -= used;
}

static void
notification_add_payload(struct notification_payload *np, const char *payload,
    int encoded)
{
	size_t	len = strlen(payload);

	if (!encoded) {
		notification_decode(np, 1);
		notification_append(np, payload, len);
		return;
	}
	np->encoded = xrealloc(np->encoded, np->encodedlen + len);
	memcpy(np->encoded + np->encodedlen, payload, len);
	np->encodedlen += len;
	notification_decode(np, 0);
}

/* Keep an icon for later notifications, or fetch a kept one. */
static void
notification_icon(struct notifications *ns, struct notification *n)
{
	struct notification_payload	*np = &n->payloads[NOTIFICATION_ICON];
	struct notification_icon	*ni;
	const char			*key;

	key = notification_get_key(n, 'g');
	if (key == NULL)
		return;
	TAILQ_FOREACH(ni, &ns->icons, entry) {
		if (strcmp(ni->key, key) == 0)
			break;
	}
	if (np->size == 0) {
		if (ni != NULL)
			notification_append(np, ni->data, ni->size);
		return;
	}

	if (ni != NULL) {
		TAILQ_REMOVE(&ns->icons, ni, entry);
		free(ni->data);
	} else {
		if (ns->nicons == NOTIFICATION_ICONS) {
			ni = TAILQ_FIRST(&ns->icons);
			TAILQ_REMOVE(&ns->icons, ni, entry);
			free(ni->key);
			free(ni->data);
			free(ni);
		} else
			ns->nicons++;
		ni = xmalloc(sizeof *ni);
		ni->key = xstrdup(key);
	}
	ni->data = xmalloc(np->size);
	memcpy(ni->data, np->data, np->size);
	ni->size = np->size;
	TAILQ_INSERT_TAIL(&ns->icons, ni, entry);
}

/* Work out the actions the application wants from a list like kitty. */
static int
notification_actions(const char *value)
{
	char	*copy, *cp, *next;
	int	 flags = NOTIFICATION_FOCUS, flag, negate;

	if (value == NULL)
		return (flags);
	copy = next = xstrdup(value);
	while ((cp = strsep(&next, ",")) != NULL) {
		negate = (*cp == '-');
		if (*cp == '-' || *cp == '+')
			cp++;
		if (strcmp(cp, "focus") == 0)
			flag = NOTIFICATION_FOCUS;
		else if (strcmp(cp, "report") == 0)
			flag = NOTIFICATION_REPORT;
		else
			continue;
		if (negate)
			flags &= ~flag;
		else
			flags |= flag;
	}
	free(copy);
	return (flags);
}

/*
 * The occasion is about the window the notification came from, which is the
 * pane: it is always unfocused and invisible if it is not shown on the
 * client, otherwise let the terminal decide.
 */
static const char *
notification_occasion(struct client *c, struct window_pane *wp,
    const char *occasion)
{
	struct window	*w = wp->window;
	struct winlink	*wl = c->session->curw;

	if (strcmp(occasion, "unfocused") != 0 &&
	    strcmp(occasion, "invisible") != 0)
		return (occasion);
	if (wl == NULL || wl->window != w)
		return ("always");
	if (strcmp(occasion, "unfocused") == 0)
		return (wp == w->active ? occasion : "always");
	return (window_pane_is_visible(wp) ? occasion : "always");
}

/* Write a complete notification to one client. */
static void
notification_send_client(struct client *c, struct window_pane *wp,
    struct notification *n, const char *oid, const char *keys)
{
	struct notification_payload	*np;
	const char			*occasion;
	char				*metadata, *first, *base, *chunk;
	size_t				 off, len, size;
	u_int				 i, last = NOTIFICATION_UNKNOWN;
	int				 done;

	occasion = notification_get_key(n, 'o');
	if (occasion != NULL) {
		xasprintf(&first, "%s:o=%s", keys,
		    notification_occasion(c, wp, occasion));
	} else
		first = xstrdup(keys);

	for (i = 0; i < nitems(n->payloads); i++) {
		if (n->payloads[i].size != 0)
			last = i;
	}
	if (last == NOTIFICATION_UNKNOWN) {
		notification_write(c, first, "");
		free(first);
		return;
	}

	/* Only the first chunk needs the keys, later ones just the id. */
	xasprintf(&base, "i=%s", oid);
	size = 4 * ((NOTIFICATION_CHUNK + 2) / 3) + 1;
	chunk = xmalloc(size);
	for (i = 0; i <= last; i++) {
		np = &n->payloads[i];
		for (off = 0; off < np->size; off += len) {
			len = np->size - off;
			if (len > NOTIFICATION_CHUNK)
				len = NOTIFICATION_CHUNK;
			done = (i == last && off + len == np->size);
			xasprintf(&metadata, "%s:p=%s:e=1%s",
			    first != NULL ? first : base,
			    notification_type_names[i], done ? "" : ":d=0");
			free(first);
			first = NULL;
			b64_ntop(np->data + off, len, chunk, size);
			notification_write(c, metadata, chunk);
			free(metadata);
		}
	}
	free(chunk);
	free(base);
}

/* Send a complete notification to every client that can show it. */
static void
notification_send(struct window_pane *wp, struct notification *n)
{
	struct notifications		*ns = notification_get(wp);
	struct notification_entry	*ne;
	struct notification_key		*nk;
	struct client			*c;
	struct evbuffer			*evb;
	char				*oid, *keys;
	const char			*value;
	u_int				 i;

	for (i = 0; i < nitems(n->payloads); i++)
		notification_decode(&n->payloads[i], 1);
	notification_icon(ns, n);

	if (n->id != NULL) {
		TAILQ_FOREACH(ne, &ns->entries, entry) {
			if (ne->id != NULL && strcmp(ne->id, n->id) == 0)
				break;
		}
	} else
		ne = NULL;
	if (ne == NULL) {
		if (ns->nentries == NOTIFICATION_ENTRIES) {
			notification_remove_entry(ns,
			    TAILQ_FIRST(&ns->entries));
		}
		ne = xcalloc(1, sizeof *ne);
		if (n->id != NULL)
			ne->id = xstrdup(n->id);
		else
			ne->number = notification_next++;
		ns->nentries++;
	} else
		TAILQ_REMOVE(&ns->entries, ne, entry);
	TAILQ_INSERT_TAIL(&ns->entries, ne, entry);

	ne->flags = notification_actions(notification_get_key(n, 'a'));
	value = notification_get_key(n, 'c');
	if (value != NULL && strcmp(value, "0") != 0)
		ne->flags |= NOTIFICATION_CLOSE;

	/*
	 * Always ask for activation to be reported so the pane can be focused
	 * and decide whether to tell the application.
	 */
	oid = notification_outer_id(wp, ne->id, ne->number);
	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(evb, "i=%s:a=%sfocus,report", oid,
	    (ne->flags & NOTIFICATION_FOCUS) ? "" : "-");
	TAILQ_FOREACH(nk, &n->keys, entry) {
		if (nk->key != 'a' && nk->key != 'o')
			evbuffer_add_printf(evb, ":%c=%s", nk->key, nk->value);
	}
	keys = xstrndup((char *)EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	evbuffer_free(evb);

	TAILQ_FOREACH(c, &clients, entry) {
		if (notification_client_ok(c, wp, TTYC_SNOT99))
			notification_send_client(c, wp, n, oid, keys);
	}
	free(keys);
	free(oid);
}

static void
notification_close(struct window_pane *wp, const char *id)
{
	struct notifications		*ns = notification_get(wp);
	struct notification_entry	*ne;
	char				*oid;

	TAILQ_FOREACH(ne, &ns->entries, entry) {
		if (ne->id != NULL && strcmp(ne->id, id) == 0)
			break;
	}
	if (ne != NULL && (~ne->flags & NOTIFICATION_CLOSE))
		notification_remove_entry(ns, ne);

	oid = notification_outer_id(wp, id, 0);
	notification_close_others(NULL, oid);
	free(oid);
}

static void
notification_alive(struct window_pane *wp, const char *id)
{
	struct notifications		*ns = notification_get(wp);
	struct notification_query	*nq;
	struct client			*c;
	char				*oid, *metadata;
	int				 sent = 0;

	oid = notification_outer_id(wp, id, 0);
	xasprintf(&metadata, "i=%s:p=alive", oid);
	TAILQ_FOREACH(c, &clients, entry) {
		if (notification_client_ok(c, wp, TTYC_SNOT99)) {
			notification_write(c, metadata, "");
			sent = 1;
		}
	}
	free(metadata);
	free(oid);
	if (!sent)
		return;

	TAILQ_FOREACH(nq, &ns->queries, entry) {
		if (strcmp(nq->id, id) == 0)
			return;
	}
	nq = xmalloc(sizeof *nq);
	nq->id = xstrdup(id);
	TAILQ_INSERT_TAIL(&ns->queries, nq, entry);
}

/* Answer a query with what the most recently active client supports. */
static char *
notification_query(struct window_pane *wp, const char *id, const char *end)
{
	struct client	*c, *best = NULL;
	char		*reply;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!notification_client_ok(c, wp, TTYC_SNOT99))
			continue;
		if (c->tty.notification_caps == NULL)
			continue;
		if (best == NULL ||
		    timercmp(&c->activity_time, &best->activity_time, >))
			best = c;
	}
	if (best == NULL)
		return (NULL);
	xasprintf(&reply, "\033]99;i=%s:p=?;%s%s", id != NULL ? id : "0",
	    best->tty.notification_caps, end);
	return (reply);
}

static int
notification_valid_value(const char *value)
{
	const char	*cp;

	for (cp = value; *cp != '\0'; cp++) {
		if (*cp <= ' ' || *cp > '~' || *cp == ':' || *cp == ';')
			return (0);
	}
	return (1);
}

static char *
notification_sanitize_id(const char *value)
{
	static const char	 valid[] = "abcdefghijklmnopqrstuvwxyz"
				     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+.";
	char			*id, *out;

	id = out = xmalloc(strlen(value) + 1);
	for (; *value != '\0'; value++) {
		if (strchr(valid, *value) != NULL)
			*out++ = *value;
	}
	*out = '\0';
	if (*id == '\0') {
		free(id);
		return (NULL);
	}
	return (id);
}

/*
 * Handle an OSC 99 notification sequence from a pane. Returns a reply to a
 * query or NULL.
 */
char *
notification_osc99(struct window_pane *wp, const char *p, const char *end)
{
	struct notifications	*ns;
	struct notification	*n = NULL, *pending;
	struct notification_key	*nk;
	char			*copy, *metadata, *payload, *cp, *next;
	char			*id = NULL, *reply = NULL;
	const char		*type = "title";
	enum notification_type	 t;
	int			 encoded = 0, done = 1;

	copy = xstrdup(p);
	payload = strchr(copy, ';');
	if (payload != NULL)
		*payload++ = '\0';
	else
		payload = copy + strlen(copy);
	metadata = copy;

	n = xcalloc(1, sizeof *n);
	TAILQ_INIT(&n->keys);
	next = metadata;
	while ((cp = strsep(&next, ":")) != NULL) {
		if (*cp == '\0')
			continue;
		if (cp[1] != '=' || !isascii((u_char)*cp) ||
		    !isalpha((u_char)*cp)) {
			log_debug("%s: bad metadata: %s", __func__, metadata);
			goto out;
		}
		if (!notification_valid_value(cp + 2))
			continue;
		switch (*cp) {
		case 'i':
			free(id);
			id = notification_sanitize_id(cp + 2);
			break;
		case 'p':
			type = cp + 2;
			break;
		case 'e':
			encoded = (strcmp(cp + 2, "1") == 0);
			break;
		case 'd':
			done = (strcmp(cp + 2, "0") != 0);
			break;
		default:
			notification_set_key(n, *cp, cp + 2);
			break;
		}
	}

	if (strcmp(type, "?") == 0) {
		reply = notification_query(wp, id, end);
		goto out;
	}
	if (strcmp(type, "alive") == 0) {
		if (id != NULL)
			notification_alive(wp, id);
		goto out;
	}
	if (strcmp(type, "close") == 0) {
		if (id != NULL)
			notification_close(wp, id);
		goto out;
	}

	ns = notification_get(wp);
	pending = ns->pending;
	if (pending != NULL &&
	    (pending->id == NULL ? id == NULL :
	    (id != NULL && strcmp(pending->id, id) == 0))) {
		TAILQ_FOREACH(nk, &n->keys, entry)
			notification_set_key(pending, nk->key, nk->value);
		notification_free_one(n);
		n = pending;
	} else {
		notification_free_one(pending);
		n->id = id;
		id = NULL;
	}
	ns->pending = NULL;

	for (t = 0; t < NOTIFICATION_UNKNOWN; t++) {
		if (strcmp(type, notification_type_names[t]) == 0)
			break;
	}
	if (t != NOTIFICATION_UNKNOWN)
		notification_add_payload(&n->payloads[t], payload, encoded);
	else
		log_debug("%s: unknown payload type %s", __func__, type);

	if (done)
		notification_send(wp, n);
	else {
		ns->pending = n;
		n = NULL;
	}

out:
	notification_free_one(n);
	free(id);
	free(copy);
	return (reply);
}

static void
notification_focus(struct client *c, struct window_pane *wp)
{
	struct window		*w = wp->window;
	struct session		*s = c->session;
	struct winlink		*wl;
	struct cmd_find_state	 fs;
	int			 visible;

	if (s == NULL || (c->flags & CLIENT_READONLY))
		return;
	if ((wl = winlink_find_by_window(&s->windows, w)) == NULL) {
		if (cmd_find_from_pane(&fs, wp, 0) != 0)
			return;
		s = fs.s;
		wl = fs.wl;
	}

	if (wp != w->active) {
		if (w->modal != NULL && wp != w->modal)
			visible = 1;
		else
			visible = window_pane_is_visible(wp);
		if (!visible && window_push_zoom(w, 0, 0))
			server_redraw_window(w);
		window_redraw_active_switch(w, wp);
		window_set_active_pane(w, wp, 1);
		if (!visible && window_pop_zoom(w))
			server_redraw_window(w);
	}
	session_set_current(s, wl);
	server_client_set_session(c, s);
}

static void
notification_activated(struct client *c, struct window_pane *wp,
    const char *oid, const char *rest, const char *payload)
{
	struct notifications		*ns = wp->notifications;
	struct notification_entry	*ne;
	char				*metadata;

	if (ns == NULL || (ne = notification_find_entry(ns, rest)) == NULL)
		return;
	if (ne->flags & NOTIFICATION_REPORT) {
		xasprintf(&metadata, "i=%s", ne->id != NULL ? ne->id : "0");
		notification_reply(wp, metadata, payload);
		free(metadata);
	}
	if (ne->flags & NOTIFICATION_FOCUS)
		notification_focus(c, wp);
	if (~ne->flags & NOTIFICATION_CLOSE)
		notification_remove_entry(ns, ne);
	notification_close_others(c, oid);
}

static void
notification_closed(struct client *c, struct window_pane *wp,
    const char *oid, const char *rest, const char *payload)
{
	struct notifications		*ns = wp->notifications;
	struct notification_entry	*ne;
	char				*metadata;
	int				 untracked;

	if (ns == NULL || (ne = notification_find_entry(ns, rest)) == NULL)
		return;
	if (~ne->flags & NOTIFICATION_CLOSE)
		return;
	untracked = (strcmp(payload, "untracked") == 0);
	if (untracked && (ne->flags & NOTIFICATION_UNTRACKED))
		return;

	xasprintf(&metadata, "i=%s:p=close", ne->id != NULL ? ne->id : "0");
	notification_reply(wp, metadata, payload);
	free(metadata);

	if (untracked)
		ne->flags |= NOTIFICATION_UNTRACKED;
	else {
		notification_remove_entry(ns, ne);
		notification_close_others(c, oid);
	}
}

static void
notification_alive_reply(struct window_pane *wp, const char *rest,
    const char *payload)
{
	struct notifications		*ns = wp->notifications;
	struct notification_query	*nq;
	struct evbuffer			*evb;
	struct window_pane		*found;
	char				*copy, *cp, *next, *ids, *metadata;
	const char			*id;

	if (ns == NULL || *rest != 'i')
		return;
	TAILQ_FOREACH(nq, &ns->queries, entry) {
		if (strcmp(nq->id, rest + 1) == 0)
			break;
	}
	if (nq == NULL)
		return;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	copy = next = xstrdup(payload);
	while ((cp = strsep(&next, ",")) != NULL) {
		found = notification_find(cp, &id);
		if (found != wp || *id != 'i')
			continue;
		if (EVBUFFER_LENGTH(evb) != 0)
			evbuffer_add(evb, ",", 1);
		evbuffer_add_printf(evb, "%s", id + 1);
	}
	free(copy);
	ids = xstrndup((char *)EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	evbuffer_free(evb);

	xasprintf(&metadata, "i=%s:p=alive", nq->id);
	notification_reply(wp, metadata, ids);
	free(metadata);
	free(ids);

	TAILQ_REMOVE(&ns->queries, nq, entry);
	free(nq->id);
	free(nq);
}

/* Handle an OSC 99 reply from a client's terminal. */
void
notification_terminal_reply(struct client *c, const char *buf, size_t len)
{
	struct window_pane	*wp;
	char			*copy, *payload, *cp, *next;
	const char		*oid = NULL, *type = "title", *rest;

	copy = xstrndup(buf, len);
	log_debug("%s: %s: %s", __func__, c->name, copy);
	payload = strchr(copy, ';');
	if (payload == NULL)
		goto out;
	*payload++ = '\0';

	next = copy;
	while ((cp = strsep(&next, ":")) != NULL) {
		if (strncmp(cp, "i=", 2) == 0)
			oid = cp + 2;
		else if (strncmp(cp, "p=", 2) == 0)
			type = cp + 2;
	}

	if (strcmp(type, "?") == 0) {
		free(c->tty.notification_caps);
		c->tty.notification_caps = xstrdup(payload);
		goto out;
	}
	if (oid == NULL || (wp = notification_find(oid, &rest)) == NULL)
		goto out;
	if (strcmp(type, "title") == 0)
		notification_activated(c, wp, oid, rest, payload);
	else if (strcmp(type, "close") == 0)
		notification_closed(c, wp, oid, rest, payload);
	else if (strcmp(type, "alive") == 0)
		notification_alive_reply(wp, rest, payload);

out:
	free(copy);
}
