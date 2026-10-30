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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Kitty drag and drop protocol (OSC 72).
 *
 * Each client terminal sees tmux as one program. A client is registered to
 * accept drops while any pane in its current window accepts them, and to
 * offer drags while any pane offers them. Drop movement and drag start events
 * from the terminal go to the pane under the reported cell, with coordinates
 * made relative to the pane. Once a pane has the drop or the drag, replies in
 * both directions are passed between that pane and that client. The i key
 * from the pane is put back on everything sent to it.
 */

#define DND_HAS_X 0x1
#define DND_HAS_Y 0x2
#define DND_HAS_PX 0x4
#define DND_HAS_PY 0x8
#define DND_HAS_O 0x10
#define DND_HAS_M 0x20

/* Longest payload in one sequence, and longest stored list, as kitty. */
#define DND_CHUNK_SIZE 4096
#define DND_LIST_SIZE (1024 * 1024)

enum dnd_role {
	DND_HOVER,
	DND_DROP,
	DND_DRAG
};

struct dnd_msg {
	char		 type;
	int		 flags;
	int		 x;
	int		 y;
	int		 px;
	int		 py;
	u_int		 o;
	u_int		 m;
	u_int		 id;
	const char	*payload;
};

struct dnd_pane {
	int		 accept;
	u_int		 accept_id;
	char		*accept_mimes;
	char		*accept_pending;
	char		*accept_machine;

	int		 offer;
	u_int		 offer_id;
	char		*offer_machine;

	int		 chunked;
	struct dnd_msg	 first;
};

struct dnd_tty {
	u_int		 generation;
	u_int		 window;
	char		*accept;
	int		 offer;
	char		*accept_machine;
	char		*offer_machine;

	int		 hover_pane;
	char		*hover_mimes;
	int		 drop_pane;
	int		 drag_pane;

	int		 chunked;
	int		 chunk_pane;
	struct dnd_msg	 first;
};

static u_int	dnd_generation = 1;
static u_int	dnd_registered;

static int
dnd_parse(const char *s, struct dnd_msg *msg)
{
	char		key;
	int		negative;
	long long	n;

	memset(msg, 0, sizeof *msg);
	if (*s == ';') {
		msg->payload = s + 1;
		return (0);
	}
	while (*s != '\0') {
		key = *s++;
		if (*s++ != '=')
			return (-1);
		if (key == 't') {
			if (*s == '\0' || strchr("AEMPRaekmopqr", *s) == NULL)
				return (-1);
			msg->type = *s++;
		} else {
			negative = (*s == '-');
			if (negative)
				s++;
			if (*s < '0' || *s > '9')
				return (-1);
			for (n = 0; *s >= '0' && *s <= '9'; s++) {
				n = n * 10 + (*s - '0');
				if (n > UINT32_MAX)
					return (-1);
			}
			if (negative)
				n = -n;
			switch (key) {
			case 'x':
				msg->x = n;
				msg->flags |= DND_HAS_X;
				break;
			case 'y':
				msg->y = n;
				msg->flags |= DND_HAS_Y;
				break;
			case 'X':
				msg->px = n;
				msg->flags |= DND_HAS_PX;
				break;
			case 'Y':
				msg->py = n;
				msg->flags |= DND_HAS_PY;
				break;
			case 'o':
			case 'm':
			case 'i':
				if (negative)
					return (-1);
				if (key == 'o') {
					msg->o = n;
					msg->flags |= DND_HAS_O;
				} else if (key == 'm') {
					msg->m = n;
					msg->flags |= DND_HAS_M;
				} else
					msg->id = n;
				break;
			default:
				return (-1);
			}
		}
		if (*s == ';') {
			msg->payload = s + 1;
			break;
		}
		if (*s == ':')
			s++;
		else if (*s != '\0')
			return (-1);
	}
	return (0);
}

static void
dnd_add_key(char *buf, size_t size, char key, long long value)
{
	char	tmp[32];

	snprintf(tmp, sizeof tmp, "%s%c=%lld", *buf == '\0' ? "" : ":", key,
	    value);
	strlcat(buf, tmp, size);
}

static char *
dnd_format(const struct dnd_msg *msg, u_int id)
{
	char	 buf[256], *out;

	*buf = '\0';
	if (msg->type != '\0')
		snprintf(buf, sizeof buf, "t=%c", msg->type);
	if (msg->flags & DND_HAS_X)
		dnd_add_key(buf, sizeof buf, 'x', msg->x);
	if (msg->flags & DND_HAS_Y)
		dnd_add_key(buf, sizeof buf, 'y', msg->y);
	if (msg->flags & DND_HAS_PX)
		dnd_add_key(buf, sizeof buf, 'X', msg->px);
	if (msg->flags & DND_HAS_PY)
		dnd_add_key(buf, sizeof buf, 'Y', msg->py);
	if (msg->flags & DND_HAS_O)
		dnd_add_key(buf, sizeof buf, 'o', msg->o);
	if (id != 0)
		dnd_add_key(buf, sizeof buf, 'i', id);
	if (msg->flags & DND_HAS_M)
		dnd_add_key(buf, sizeof buf, 'm', msg->m);
	if (msg->payload == NULL)
		return (xstrdup(buf));
	xasprintf(&out, "%s;%s", buf, msg->payload);
	return (out);
}

/*
 * Track chunked transfers: later chunks may leave out everything except m and
 * i, so they use the keys from the first chunk.
 */
static void
dnd_chunk(int *chunked, struct dnd_msg *first, const struct dnd_msg *msg,
    struct dnd_msg *effective)
{
	if (*chunked) {
		*effective = *first;
		effective->m = msg->m;
		effective->payload = msg->payload;
		*chunked = (msg->m != 0);
		return;
	}
	*effective = *msg;
	if (msg->m != 0) {
		*chunked = 1;
		*first = *msg;
		first->payload = NULL;
	}
}

/* Set or append to a list. A chunk making it too long is dropped. */
static int
dnd_set_string(char **s, const char *payload, int append)
{
	char	*new;
	size_t	 size = 0;

	if (payload == NULL)
		payload = "";
	if (append && *s != NULL)
		size = strlen(*s);
	if (size + strlen(payload) > DND_LIST_SIZE)
		return (-1);
	if (append && *s != NULL) {
		xasprintf(&new, "%s%s", *s, payload);
		free(*s);
		*s = new;
		return (0);
	}
	free(*s);
	*s = xstrdup(payload);
	return (0);
}

/* Check a payload from a pane is safe to give to the terminal. */
static int
dnd_payload_safe(char type, const char *s)
{
	const u_char	*p;

	if (s == NULL)
		return (1);
	for (p = s; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			return (0);
		if (*p > 0x7e && type != 'E')
			return (0);
		if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f)
			return (0);
	}
	return (type != 'E' || utf8_isvalid(s));
}

/*
 * Format a message, splitting a long payload into chunks. Later chunks have
 * only the m and i keys.
 */
static void
dnd_output(const struct dnd_msg *msg, u_int id,
    void (*cb)(const char *, void *), void *arg)
{
	struct dnd_msg	 part;
	const char	*payload = msg->payload;
	char		*s;
	size_t		 left, size;

	if (payload == NULL || strlen(payload) <= DND_CHUNK_SIZE) {
		s = dnd_format(msg, id);
		cb(s, arg);
		free(s);
		return;
	}
	part = *msg;
	left = strlen(payload);
	while (left != 0) {
		size = left > DND_CHUNK_SIZE ? DND_CHUNK_SIZE : left;
		part.flags |= DND_HAS_M;
		if (size != left)
			part.m = 1;
		else
			part.m = (msg->flags & DND_HAS_M) ? msg->m : 0;
		part.payload = xstrndup(payload, size);
		s = dnd_format(&part, id);
		cb(s, arg);
		free(s);
		free((void *)part.payload);

		payload += size;
		left -= size;
		memset(&part, 0, sizeof part);
	}
}

static void
dnd_send_cb(const char *s, void *arg)
{
	struct tty	*tty = arg;
	int		 flags;

	log_debug("%s: %s: %s", __func__, tty->client->name, s);

	/* Dropping any part of a transfer would break it, so never block. */
	flags = (tty->flags & TTY_BLOCK);
	tty->flags &= ~TTY_BLOCK;
	tty_putcode_s(tty, TTYC_DND, s);
	tty->flags |= flags;
}

static void
dnd_send(struct tty *tty, const struct dnd_msg *msg)
{
	if (tty_term_has(tty->term, TTYC_DND))
		dnd_output(msg, 0, dnd_send_cb, tty);
}

static void
dnd_send_type(struct tty *tty, char type, const char *payload)
{
	struct dnd_msg	msg;

	memset(&msg, 0, sizeof msg);
	msg.type = type;
	msg.payload = payload;
	dnd_send(tty, &msg);
}

/* Refuse a drop being moved (t=m) or end one that was dropped (t=r). */
static void
dnd_send_refuse(struct tty *tty, char type)
{
	struct dnd_msg	msg;

	memset(&msg, 0, sizeof msg);
	msg.type = type;
	msg.flags = DND_HAS_O;
	dnd_send(tty, &msg);
}

static void
dnd_write_cb(const char *s, void *arg)
{
	struct window_pane	*wp = arg;

	log_debug("%s: %%%u: %s", __func__, wp->id, s);
	bufferevent_write(wp->event, "\033]72;", 5);
	bufferevent_write(wp->event, s, strlen(s));
	bufferevent_write(wp->event, "\033\\", 2);
}

static void
dnd_write(struct window_pane *wp, const struct dnd_msg *msg, u_int id)
{
	if (wp->event != NULL)
		dnd_output(msg, id, dnd_write_cb, wp);
}

static void
dnd_reply(struct input_ctx *ictx, const struct dnd_msg *msg, u_int id)
{
	char	*s;

	s = dnd_format(msg, id);
	input_reply(ictx, 1, "\033]72;%s\033\\", s);
	free(s);
}

static int
dnd_supported(struct window_pane *wp)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || (c->flags & CLIENT_CONTROL))
			continue;
		if (!session_has(c->session, wp->window))
			continue;
		if (tty_term_has(c->tty.term, TTYC_DND))
			return (1);
	}
	return (0);
}

/* Refuse a request, unless no terminal could have answered it. */
static void
dnd_reply_error(struct window_pane *wp, struct input_ctx *ictx,
    const struct dnd_msg *msg, char type, const char *error, u_int id)
{
	struct dnd_msg	reply;

	if (!dnd_supported(wp))
		return;
	memset(&reply, 0, sizeof reply);
	reply.type = type;
	reply.flags = msg->flags & (DND_HAS_X|DND_HAS_Y|DND_HAS_PY);
	reply.x = msg->x;
	reply.y = msg->y;
	reply.py = msg->py;
	reply.payload = error;
	dnd_reply(ictx, &reply, id);
}

static struct dnd_tty *
dnd_get_tty(struct tty *tty)
{
	struct dnd_tty	*dt = tty->dnd;

	if (dt == NULL) {
		dt = tty->dnd = xcalloc(1, sizeof *dt);
		dt->hover_pane = dt->drop_pane = dt->drag_pane = -1;
		dt->chunk_pane = -1;
	}
	return (dt);
}

static struct dnd_pane *
dnd_get_pane(struct window_pane *wp)
{
	if (wp->dnd == NULL)
		wp->dnd = xcalloc(1, sizeof *wp->dnd);
	return (wp->dnd);
}

static void
dnd_set_registered(struct dnd_pane *dp, int accept, int offer)
{
	if (dp->accept || dp->offer)
		dnd_registered--;
	dp->accept = accept;
	dp->offer = offer;
	if (dp->accept || dp->offer)
		dnd_registered++;
	dnd_generation++;
}

static struct client *
dnd_find_client(struct window_pane *wp, enum dnd_role role)
{
	struct client	*c;
	struct dnd_tty	*dt;
	int		 id;

	TAILQ_FOREACH(c, &clients, entry) {
		if ((dt = c->tty.dnd) == NULL)
			continue;
		switch (role) {
		case DND_HOVER:
			id = dt->hover_pane;
			break;
		case DND_DROP:
			id = dt->drop_pane;
			break;
		case DND_DRAG:
			id = dt->drag_pane;
			break;
		}
		if (id == (int)wp->id)
			return (c);
	}
	return (NULL);
}

/* Cancel the drop or drag a pane has with every client but one. */
static void
dnd_cancel(struct window_pane *wp, struct client *except, int drop, int drag)
{
	struct client	*c;
	struct dnd_tty	*dt;
	struct dnd_msg	 msg;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c == except || (dt = c->tty.dnd) == NULL)
			continue;
		if (drop && dt->hover_pane == (int)wp->id) {
			dt->hover_pane = -1;
			dnd_send_refuse(&c->tty, 'm');
		}
		if (drop && dt->drop_pane == (int)wp->id) {
			dt->drop_pane = -1;
			dnd_send_refuse(&c->tty, 'r');
		}
		if (drag && dt->drag_pane == (int)wp->id) {
			dt->drag_pane = -1;
			memset(&msg, 0, sizeof msg);
			msg.type = 'E';
			msg.flags = DND_HAS_Y;
			msg.y = -1;
			dnd_send(&c->tty, &msg);
		}
		if (dt->chunk_pane == (int)wp->id)
			dt->chunk_pane = -1;
	}
}

/* Find the pane at a client cell and the position inside it. */
static struct window_pane *
dnd_pane_at(struct client *c, int x, int y, int *px, int *py)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;
	int			 statusat, sideat;
	u_int			 lines, width, ox, oy, sx, sy;

	if (x < 0 || y < 0)
		return (NULL);

	statusat = status_at_line(c);
	lines = status_line_size(c);
	if (statusat != -1 && y >= statusat && y < statusat + (int)lines)
		return (NULL);
	if (statusat == 0)
		y -= lines;

	sideat = status_side_at_column(c);
	width = status_side_size(c);
	if (sideat != -1 && x >= sideat && x < sideat + (int)width)
		return (NULL);
	if (sideat == 0)
		x -= width;

	tty_window_offset(&c->tty, &ox, &oy, &sx, &sy);
	if (y < 0 || x >= (int)sx || y >= (int)sy)
		return (NULL);
	x += ox;
	y += oy;

	wp = window_get_active_at(w, x, y);
	if (wp == NULL)
		return (NULL);
	if (x < wp->xoff || x >= wp->xoff + (int)wp->sx ||
	    y < wp->yoff || y >= wp->yoff + (int)wp->sy)
		return (NULL);
	*px = x - wp->xoff;
	*py = y - wp->yoff;
	return (wp);
}

/* Make a terminal event relative to a pane. */
static void
dnd_translate(struct tty *tty, struct dnd_msg *msg, int px, int py)
{
	if (msg->flags & DND_HAS_PX) {
		msg->px -= (msg->x - px) * (int)tty->xpixel;
		if (msg->px < 0)
			msg->px = 0;
	}
	if (msg->flags & DND_HAS_PY) {
		msg->py -= (msg->y - py) * (int)tty->ypixel;
		if (msg->py < 0)
			msg->py = 0;
	}
	msg->x = px;
	msg->y = py;
}

static void
dnd_leave(struct tty *tty)
{
	struct dnd_tty		*dt = tty->dnd;
	struct window_pane	*wp;
	struct dnd_msg		 msg;

	if (dt->hover_pane == -1)
		return;
	wp = window_pane_find_by_id(dt->hover_pane);
	dt->hover_pane = -1;
	if (wp == NULL || wp->dnd == NULL || !wp->dnd->accept)
		return;
	memset(&msg, 0, sizeof msg);
	msg.type = 'm';
	msg.flags = DND_HAS_X|DND_HAS_Y;
	msg.x = msg.y = -1;
	dnd_write(wp, &msg, wp->dnd->accept_id);
}

/* Tell the terminal the remote machine of the pane being used, if changed. */
static void
dnd_send_machine(struct tty *tty, char type, char **last, const char *machine)
{
	struct dnd_msg	msg;

	if (machine == NULL)
		machine = "";
	if (*last != NULL && strcmp(*last, machine) == 0)
		return;
	if (*last == NULL && *machine == '\0')
		return;
	free(*last);
	*last = xstrdup(machine);

	memset(&msg, 0, sizeof msg);
	msg.type = type;
	msg.flags = DND_HAS_X;
	msg.x = 1;
	msg.payload = machine;
	dnd_send(tty, &msg);
}

static void
dnd_tty_move(struct client *c, struct dnd_msg *msg, struct dnd_msg *raw)
{
	struct tty		*tty = &c->tty;
	struct dnd_tty		*dt = tty->dnd;
	struct window_pane	*wp;
	struct dnd_pane		*dp;
	int			 px, py, changed;

	if (msg->payload != NULL && *msg->payload != '\0')
		dnd_set_string(&dt->hover_mimes, msg->payload, 0);

	if (msg->x == -1 && msg->y == -1) {
		dnd_leave(tty);
		free(dt->hover_mimes);
		dt->hover_mimes = NULL;
		return;
	}

	wp = dnd_pane_at(c, msg->x, msg->y, &px, &py);
	if (wp != NULL && (wp->dnd == NULL || !wp->dnd->accept))
		wp = NULL;
	changed = (wp == NULL || dt->hover_pane != (int)wp->id);
	if (changed) {
		dnd_leave(tty);
		/* Until the new pane answers, the drop is not accepted. */
		if (msg->type == 'm')
			dnd_send_refuse(tty, 'm');
	}
	if (wp == NULL) {
		if (msg->type == 'M')
			dnd_send_refuse(tty, 'r');
		if (raw->m != 0)
			dt->chunk_pane = -1;
		return;
	}
	dp = wp->dnd;
	if (raw->m != 0)
		dt->chunk_pane = wp->id;

	if (msg->type == 'M') {
		dnd_cancel(wp, c, 1, 0);
		dt->hover_pane = -1;
		dt->drop_pane = wp->id;
		dnd_send_machine(tty, 'a', &dt->accept_machine,
		    dp->accept_machine);
	} else
		dt->hover_pane = wp->id;

	dnd_translate(tty, raw, px, py);
	if (changed && raw->payload == NULL && dt->hover_mimes != NULL)
		raw->payload = dt->hover_mimes;
	dnd_write(wp, raw, dp->accept_id);
	if (msg->type == 'M') {
		free(dt->hover_mimes);
		dt->hover_mimes = NULL;
	}
}

static void
dnd_tty_start_drag(struct client *c, struct dnd_msg *msg)
{
	struct tty		*tty = &c->tty;
	struct dnd_tty		*dt = tty->dnd;
	struct window_pane	*wp;
	int			 px, py;

	wp = dnd_pane_at(c, msg->x, msg->y, &px, &py);
	if (wp == NULL || wp->dnd == NULL || !wp->dnd->offer)
		return;
	dnd_cancel(wp, c, 0, 1);
	dt->drag_pane = wp->id;
	dnd_send_machine(tty, 'o', &dt->offer_machine, wp->dnd->offer_machine);
	dnd_translate(tty, msg, px, py);
	dnd_write(wp, msg, wp->dnd->offer_id);
}

/* Handle an OSC 72 sequence from the terminal (without OSC 72 and ST). */
void
dnd_tty_input(struct tty *tty, const char *s)
{
	struct client		*c = tty->client;
	struct dnd_tty		*dt;
	struct window_pane	*wp;
	struct dnd_msg		 raw, msg;
	int			 continued;

	if (dnd_parse(s, &raw) != 0) {
		log_debug("%s: %s: invalid: %s", __func__, c->name, s);
		return;
	}
	log_debug("%s: %s: %s", __func__, c->name, s);

	/* A query reply means the terminal supports the protocol. */
	if (raw.type == 'q') {
		if (~tty->flags & TTY_HAVEDND) {
			tty_parse_client_features(c, "dnd", ",");
			tty_update_features(tty);
			tty->flags |= TTY_HAVEDND;
		}
		return;
	}
	if (c->session == NULL || c->session->curw == NULL)
		return;
	dt = dnd_get_tty(tty);
	continued = dt->chunked;
	dnd_chunk(&dt->chunked, &dt->first, &raw, &msg);

	if (continued) {
		if (msg.type == 'm' || msg.type == 'M') {
			if (msg.payload != NULL)
				dnd_set_string(&dt->hover_mimes, msg.payload,
				    1);
		}
		if (dt->chunk_pane == -1)
			return;
		wp = window_pane_find_by_id(dt->chunk_pane);
		if (!dt->chunked)
			dt->chunk_pane = -1;
		if (wp == NULL || wp->dnd == NULL)
			return;
		if (strchr("mMrR", msg.type) != NULL)
			dnd_write(wp, &raw, wp->dnd->accept_id);
		else
			dnd_write(wp, &raw, wp->dnd->offer_id);
		return;
	}

	switch (msg.type) {
	case 'm':
	case 'M':
		dnd_tty_move(c, &msg, &raw);
		return;
	case 'o':
		dnd_tty_start_drag(c, &raw);
		return;
	case 'r':
	case 'R':
		wp = window_pane_find_by_id(dt->drop_pane);
		break;
	case 'e':
	case 'E':
	case 'k':
		wp = window_pane_find_by_id(dt->drag_pane);
		break;
	default:
		return;
	}
	if (wp == NULL || wp->dnd == NULL) {
		dt->chunk_pane = -1;
		return;
	}
	if (raw.m != 0)
		dt->chunk_pane = wp->id;
	if (msg.type == 'r' || msg.type == 'R') {
		dnd_write(wp, &raw, wp->dnd->accept_id);
		return;
	}
	dnd_write(wp, &raw, wp->dnd->offer_id);

	/* The drag is over when it finishes or the terminal gives an error. */
	if ((msg.type == 'e' && msg.x == 4) ||
	    (msg.type == 'E' && (msg.payload == NULL ||
	    strcmp(msg.payload, "OK") != 0)))
		dt->drag_pane = -1;
}

static void
dnd_pane_drop(struct window_pane *wp, struct input_ctx *ictx,
    struct dnd_msg *msg, struct dnd_msg *raw, int continued)
{
	struct dnd_pane	*dp = wp->dnd;
	struct client	*c;

	switch (msg->type) {
	case 'a':
		if (msg->x == 1) {
			dnd_set_string(&dp->accept_machine, msg->payload,
			    continued);
			return;
		}
		dp->accept_id = msg->id;
		if (dnd_set_string(&dp->accept_pending, msg->payload,
		    continued) != 0 || msg->m != 0)
			return;
		free(dp->accept_mimes);
		dp->accept_mimes = dp->accept_pending;
		dp->accept_pending = NULL;
		dnd_set_registered(dp, 1, dp->offer);
		return;
	case 'A':
		dnd_cancel(wp, NULL, 1, 0);
		free(dp->accept_mimes);
		dp->accept_mimes = NULL;
		free(dp->accept_machine);
		dp->accept_machine = NULL;
		dnd_set_registered(dp, 0, dp->offer);
		return;
	case 'm':
		c = dnd_find_client(wp, DND_HOVER);
		if (c != NULL)
			dnd_send(&c->tty, raw);
		return;
	case 'r':
		c = dnd_find_client(wp, DND_DROP);
		if (c == NULL) {
			if (!continued && (msg->flags & (DND_HAS_X|DND_HAS_PY)))
				dnd_reply_error(wp, ictx, msg, 'R', "EPERM",
				    dp->accept_id);
			return;
		}
		dnd_send(&c->tty, raw);

		/* A request with no item or directory finishes the drop. */
		if ((msg->flags & (DND_HAS_X|DND_HAS_PY)) == 0)
			c->tty.dnd->drop_pane = -1;
		return;
	}
}

static void
dnd_pane_drag(struct window_pane *wp, struct input_ctx *ictx,
    struct dnd_msg *msg, struct dnd_msg *raw, int continued)
{
	struct dnd_pane	*dp = wp->dnd;
	struct client	*c;
	struct dnd_msg	 none;

	if (msg->type == 'o' && msg->x == 1) {
		if (msg->id != 0)
			dp->offer_id = msg->id;
		if (dnd_set_string(&dp->offer_machine, msg->payload,
		    continued) == 0 && msg->m == 0)
			dnd_set_registered(dp, dp->accept, 1);
		return;
	}
	if (msg->type == 'o' && msg->x == 2) {
		dnd_cancel(wp, NULL, 0, 1);
		free(dp->offer_machine);
		dp->offer_machine = NULL;
		dnd_set_registered(dp, dp->accept, 0);
		return;
	}
	if (msg->type == 'o' && msg->id != 0)
		dp->offer_id = msg->id;

	c = dnd_find_client(wp, DND_DRAG);
	if (c == NULL) {
		if (continued || msg->type == 'E')
			return;
		memset(&none, 0, sizeof none);
		dnd_reply_error(wp, ictx, &none, 'E',
		    msg->type == 'o' ? "EPERM" : "EINVAL", dp->offer_id);
		return;
	}
	dnd_send(&c->tty, raw);
	if (msg->type == 'E' && (msg->flags & DND_HAS_Y) && msg->y == -1)
		c->tty.dnd->drag_pane = -1;
}

/* Handle an OSC 72 sequence from a pane (without OSC 72 and ST). */
void
dnd_pane_input(struct window_pane *wp, struct input_ctx *ictx, const char *s)
{
	struct dnd_pane	*dp;
	struct dnd_msg	 raw, msg;
	int		 continued;

	if (dnd_parse(s, &raw) != 0) {
		log_debug("%s: %%%u: invalid: %s", __func__, wp->id, s);
		return;
	}
	log_debug("%s: %%%u: %s", __func__, wp->id, s);

	/* Queries may come between chunks and are not part of them. */
	if (raw.type == 'q') {
		if (dnd_supported(wp)) {
			memset(&msg, 0, sizeof msg);
			msg.type = 'q';
			dnd_reply(ictx, &msg, raw.id);
		}
		return;
	}

	dp = dnd_get_pane(wp);
	continued = dp->chunked;
	dnd_chunk(&dp->chunked, &dp->first, &raw, &msg);
	if (!dnd_payload_safe(msg.type, msg.payload)) {
		log_debug("%s: %%%u: unsafe payload", __func__, wp->id);
		return;
	}

	switch (msg.type) {
	case 'a':
	case 'A':
	case 'm':
	case 'r':
		dnd_pane_drop(wp, ictx, &msg, &raw, continued);
		break;
	case 'o':
	case 'p':
	case 'P':
	case 'e':
	case 'E':
	case 'k':
		dnd_pane_drag(wp, ictx, &msg, &raw, continued);
		break;
	}
}

/* Stop a chunked transfer on reset, but keep the registration. */
void
dnd_pane_reset(struct window_pane *wp)
{
	struct dnd_pane	*dp = wp->dnd;

	if (dp != NULL) {
		dp->chunked = 0;
		free(dp->accept_pending);
		dp->accept_pending = NULL;
	}
}

/* A pane changed window, so the clients' registrations may change. */
void
dnd_pane_moved(struct window_pane *wp)
{
	if (wp->dnd != NULL)
		dnd_generation++;
}

/* Forget a pane's state, when it exits or is destroyed. */
void
dnd_pane_free(struct window_pane *wp)
{
	struct dnd_pane	*dp = wp->dnd;

	if (dp == NULL)
		return;
	dnd_cancel(wp, NULL, 1, 1);
	dnd_set_registered(dp, 0, 0);
	free(dp->accept_mimes);
	free(dp->accept_pending);
	free(dp->accept_machine);
	free(dp->offer_machine);
	free(dp);
	wp->dnd = NULL;
}

static char *
dnd_window_mimes(struct window *w, int *accept, int *offer)
{
	struct window_pane	*wp;
	struct dnd_pane		*dp;
	char			*list = NULL, *copy, *next, *loop, *tmp;
	size_t			 len;

	*accept = *offer = 0;
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if ((dp = wp->dnd) == NULL)
			continue;
		if (dp->offer)
			*offer = 1;
		if (!dp->accept)
			continue;
		*accept = 1;
		if (dp->accept_mimes == NULL)
			continue;
		loop = copy = xstrdup(dp->accept_mimes);
		while ((next = strsep(&loop, " ")) != NULL) {
			if (*next == '\0')
				continue;
			if (list != NULL) {
				len = strlen(next);
				tmp = list;
				while ((tmp = strstr(tmp, next)) != NULL) {
					if ((tmp == list || tmp[-1] == ' ') &&
					    (tmp[len] == ' ' || tmp[len] == '\0'))
						break;
					tmp += len;
				}
				if (tmp != NULL)
					continue;
				xasprintf(&tmp, "%s %s", list, next);
				free(list);
				list = tmp;
			} else
				list = xstrdup(next);
		}
		free(copy);
	}
	if (*accept && list == NULL)
		list = xstrdup("");
	return (list);
}

/* Register the client as a drop target or drag source to match its window. */
void
dnd_tty_update(struct client *c)
{
	struct tty	*tty = &c->tty;
	struct window	*w = c->session->curw->window;
	struct dnd_tty	*dt = tty->dnd;
	struct dnd_msg	 msg;
	char		*accept;
	int		 want_accept, want_offer;

	if (!tty_term_has(tty->term, TTYC_DND))
		return;
	if (dt == NULL) {
		if (dnd_registered == 0)
			return;
		dt = dnd_get_tty(tty);
	}
	if (dt->generation == dnd_generation && dt->window == w->id)
		return;
	dt->generation = dnd_generation;
	dt->window = w->id;

	accept = dnd_window_mimes(w, &want_accept, &want_offer);
	if (accept == NULL && dt->accept != NULL) {
		dnd_send_type(tty, 'A', NULL);
		free(dt->accept);
		dt->accept = NULL;
		free(dt->accept_machine);
		dt->accept_machine = NULL;
	} else if (accept != NULL &&
	    (dt->accept == NULL || strcmp(accept, dt->accept) != 0)) {
		dnd_send_type(tty, 'a', *accept == '\0' ? NULL : accept);
		free(dt->accept);
		dt->accept = accept;
		accept = NULL;
		free(dt->accept_machine);
		dt->accept_machine = NULL;
	}
	free(accept);

	if (want_offer != dt->offer) {
		memset(&msg, 0, sizeof msg);
		msg.type = 'o';
		msg.flags = DND_HAS_X;
		msg.x = want_offer ? 1 : 2;
		dnd_send(tty, &msg);
		dt->offer = want_offer;
		free(dt->offer_machine);
		dt->offer_machine = NULL;
	}
}

/* Unregister when the terminal is given back. */
void
dnd_tty_stop(struct tty *tty)
{
	struct dnd_tty	*dt = tty->dnd;

	if (dt == NULL || !tty_term_has(tty->term, TTYC_DND))
		return;
	if (dt->accept != NULL)
		tty_raw(tty, tty_term_string_s(tty->term, TTYC_DND, "t=A"));
	if (dt->offer)
		tty_raw(tty, tty_term_string_s(tty->term, TTYC_DND, "t=o:x=2"));
	dnd_tty_free(tty);
}

void
dnd_tty_free(struct tty *tty)
{
	struct dnd_tty	*dt = tty->dnd;

	if (dt == NULL)
		return;
	free(dt->accept);
	free(dt->accept_machine);
	free(dt->offer_machine);
	free(dt->hover_mimes);
	free(dt);
	tty->dnd = NULL;
}
