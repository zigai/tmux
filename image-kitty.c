/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Michael Grant <mgrant@grant.org>
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
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Read Kitty graphics commands and draw images on Kitty graphics clients.
 *
 * Each input parser keeps one unfinished command and its payload, and maps
 * application image IDs and numbers to internal image IDs. Payloads are sent
 * directly or read from a file. Once an upload is completed, it is stored as
 * an RGBA image. Each placement then uses a child image which also records the
 * placement's z-index.
 *
 * Output caches each image and placement separately for each client. Pixels
 * are scaled to the client's cell pixel size on upload and reused in redraws.
 *
 * When a redraw is started, old rectangles overlapping the redraw area are
 * marked for deletion, then new rectangles are queued during redraw. At the
 * end, old rectangles are removed, new images uploaded and new placements
 * sent. If an upload would change the drawing order of overlapping images in
 * the same layer, all the affected images are deleted and uploaded again.
 */

#define KITTY_CHUNK_SIZE 3072

/* Kitty action types. */
enum kitty_action {
	KITTY_ACTION_QUERY = 'q',
	KITTY_ACTION_TRANSMIT = 't',
	KITTY_ACTION_TRANSMIT_AND_DISPLAY = 'T',
	KITTY_ACTION_TRANSMIT_FRAME = 'f',
	KITTY_ACTION_CONTROL_ANIMATION = 'a',
	KITTY_ACTION_COMPOSE_FRAME = 'c',
	KITTY_ACTION_PLACE = 'p',
	KITTY_ACTION_DELETE = 'd'
};

/* Kitty image formats. */
enum kitty_format {
	KITTY_FORMAT_RGB = 24,
	KITTY_FORMAT_RGBA = 32,
	KITTY_FORMAT_PNG = 100
};

/* Parsed Kitty command. */
struct kitty_command {
	char			 action;	/* a */
	u_int			 image_id;	/* i */
	u_int			 image_number;	/* I */
	u_int			 placement_id;	/* p */
	u_int			 format;	/* f */
	char			 compression;	/* o */
	char			 transmission;	/* t */
	u_int			 pixel_width;	/* s */
	u_int			 pixel_height;	/* v */
	u_int			 data_size;	/* S */
	off_t			 data_offset;	/* O */
	u_int			 cols;		/* c */
	u_int			 rows;		/* r */
	u_int			 src_x;		/* x */
	u_int			 src_y;		/* y */
	u_int			 src_w;		/* w */
	u_int			 src_h;		/* h */
	u_int			 cell_xoff;	/* X */
	u_int			 cell_yoff;	/* Y */
	int32_t			 zindex;	/* z */
	int			 cursor_no_move; /* C */
	u_int			 quiet;		/* q */
	char			 delete_action;	/* d */
	int			 more;		/* m */
};

/* State attached to an input parser. */
struct kitty_state {
	struct kitty_command		 cmd;

	u_char				*data;
	size_t				 size;
	size_t				 allocated;

	TAILQ_HEAD(, kitty_image_map)	 images;
	TAILQ_ENTRY(kitty_state)	 entry;
};
TAILQ_HEAD(kitty_states, kitty_state);

/* Kitty input states holding uploaded images. */
static struct kitty_states kitty_states = TAILQ_HEAD_INITIALIZER(kitty_states);

/* Map a Kitty placement ID to an image ID. */
struct kitty_placement_map {
	u_int					 kitty_id;
	u_int					 image_id;

	TAILQ_ENTRY(kitty_placement_map)	 entry;
};

/* Map Kitty image ID and number to image ID and placement views. */
struct kitty_image_map {
	u_int					 kitty_id;
	u_int					 number;
	u_int					 image_id;
	TAILQ_HEAD(, kitty_placement_map)	 placements;

	TAILQ_ENTRY(kitty_image_map)		 entry;
};

/* An image uploaded to a terminal. */
struct kitty_image_cache {
	u_int					image_id;
	u_int					kitty_id;
	u_int					order_id;
	int32_t					zindex;
	uint64_t				index;
	int					upload;
	u_int					xpixel;
	u_int					ypixel;
	u_int					next_placement;
	TAILQ_HEAD(, kitty_placement_cache)	placements;

	TAILQ_ENTRY(kitty_image_cache)		entry;
};

/* One grid row placed on a terminal, with positions and sizes in cells. */
struct kitty_placement_cache {
	u_int					id;
	u_int					x;
	u_int					y;
	u_int					w;
	u_int					h;
	u_int					src_x;
	u_int					src_y;
	int					pending_delete;

	TAILQ_ENTRY(kitty_placement_cache)	entry;
};

/* Image uploads and placements for one terminal. */
struct kitty_output {
	TAILQ_HEAD(, kitty_image_cache)	images;
	u_int				next_id;
	int				changed;
};

/* Select Kitty graphics output. */
int
kitty_select(struct tty *tty)
{
	if (tty->term != NULL && (tty->term->flags & TERM_KITTYGRAPHICS))
		return (1);
	return (0);
}

/* Get Kitty graphics output flags. */
int
kitty_get_flags(struct tty *tty)
{
	int	flags = IMAGE_BACKEND_GRAPHICAL;

	if (tty_term_flag(tty->term, TTYC_ISCRL))
		flags |= IMAGE_BACKEND_SCROLLS;
	return (flags);
}

/* Return the Kitty output state for a terminal. */
static struct kitty_output *
kitty_get_output(struct tty *tty)
{
	struct kitty_output	*ko = tty->image_data;

	if (ko == NULL) {
		ko = xcalloc(1, sizeof *ko);
		TAILQ_INIT(&ko->images);
		tty->image_data = ko;
	}
	return (ko);
}

/* Delete a Kitty image and its placements. */
static void
kitty_delete(struct tty *tty, u_int id)
{
	char	s[256];

	xsnprintf(s, sizeof s, "\033_Ga=d,d=I,i=%u,q=2\033\\", id);
	if (tty->flags & TTY_STARTED)
		tty_puts(tty, s);
	else
		tty_raw(tty, s);
}

/* Get a cached image in drawing order. */
static struct kitty_image_cache *
kitty_get_image(struct tty *tty, struct image *im, uint64_t index)
{
	struct kitty_output		*ko = kitty_get_output(tty);
	struct kitty_image_cache	*entry, *before;

	/* Look for this image and placement in the output cache. */
	TAILQ_FOREACH(entry, &ko->images, entry) {
		if (entry->image_id == image_get_id(im) &&
		    entry->index == index)
			return (entry);
	}

	/* Create a cache entry using this terminal's cell pixel size. */
	entry = xcalloc(1, sizeof *entry);
	entry->image_id = image_get_id(im);
	entry->order_id = image_get_order_id(im);
	entry->zindex = image_get_zindex(im);
	entry->index = index;
	entry->upload = 1;
	entry->xpixel = tty->xpixel;
	if (entry->xpixel == 0)
		entry->xpixel = 8;
	entry->ypixel = tty->ypixel;
	if (entry->ypixel == 0)
		entry->ypixel = 16;
	TAILQ_INIT(&entry->placements);

	/* Insert the entry in layer, image and placement drawing order. */
	TAILQ_FOREACH(before, &ko->images, entry) {
		if (entry->zindex != before->zindex) {
			if (entry->zindex < before->zindex)
				break;
			continue;
		}
		if (entry->order_id < before->order_id ||
		    (entry->order_id == before->order_id &&
		    entry->index < before->index))
			break;
	}
	if (before != NULL)
		TAILQ_INSERT_BEFORE(before, entry, entry);
	else
		TAILQ_INSERT_TAIL(&ko->images, entry, entry);
	return (entry);
}

/* Send a rectangle of the cached output pixels. */
static void
kitty_send_placement(struct tty *tty, struct kitty_image_cache *entry,
    struct kitty_placement_cache *kpl)
{
	char	s[256], z[32] = "";
	u_int	px = kpl->src_x * entry->xpixel + 1;
	u_int	py = kpl->src_y * entry->ypixel + 1;
	u_int	w = kpl->w * entry->xpixel, h = kpl->h * entry->ypixel;

	do
		kpl->id = ++entry->next_placement;
	while (kpl->id == 0);
	if (entry->zindex != 0)
		xsnprintf(z, sizeof z, ",z=%d", entry->zindex);
	xsnprintf(s, sizeof s,
	    "\033_Ga=p,i=%u,p=%u,x=%u,y=%u,w=%u,h=%u,c=%u,r=%u%s,C=1,q=2\033\\",
	    entry->kitty_id, kpl->id, px, py, w, h, kpl->w, kpl->h, z);

	tty_cursor(tty, kpl->x, kpl->y);
	tty_puts(tty, s);
}

/* Record a placement to send after the redraw. */
static void
kitty_place(struct kitty_image_cache *entry, u_int src_x, u_int src_y,
    u_int w, u_int h, u_int dst_x, u_int dst_y)
{
	struct kitty_placement_cache	*kpl;

	kpl = xcalloc(1, sizeof *kpl);
	kpl->x = dst_x;
	kpl->y = dst_y;
	kpl->w = w;
	kpl->h = h;
	kpl->src_x = src_x;
	kpl->src_y = src_y;
	TAILQ_INSERT_HEAD(&entry->placements, kpl, entry);
}

/* Place the part of an image outside the redraw area. */
static void
kitty_redraw_keep_piece(struct kitty_image_cache *entry,
    struct kitty_placement_cache *kpl, u_int x, u_int y, u_int w, u_int h)
{
	if (w == 0 || h == 0)
		return;
	kitty_place(entry, kpl->src_x + (x - kpl->x), kpl->src_y + (y - kpl->y),
	    w, h, x, y);
}

/* Preserve placement pieces outside the redraw area. */
static void
kitty_redraw_keep(struct kitty_image_cache *entry,
    struct kitty_placement_cache *kpl, u_int x, u_int y, u_int w, u_int h)
{
	u_int	px0 = kpl->x, px1 = kpl->x + kpl->w;
	u_int	py0 = kpl->y, py1 = kpl->y + kpl->h;
	u_int	ix0, ix1, iy0, iy1;

	ix0 = (x > px0 ? x : px0);
	ix1 = (x + w < px1 ? x + w : px1);
	iy0 = (y > py0 ? y : py0);
	iy1 = (y + h < py1 ? y + h : py1);
	if (ix0 == px0 && ix1 == px1 && iy0 == py0 && iy1 == py1)
		return;

	kitty_redraw_keep_piece(entry, kpl, px0, py0, px1 - px0, iy0 - py0);
	kitty_redraw_keep_piece(entry, kpl, px0, iy1, px1 - px0, py1 - iy1);
	kitty_redraw_keep_piece(entry, kpl, px0, iy0, ix0 - px0, iy1 - iy0);
	kitty_redraw_keep_piece(entry, kpl, ix1, iy0, px1 - ix1, iy1 - iy0);
}

/* Mark placements for deletion after the redraw. */
void
kitty_redraw_start(struct tty *tty, u_int x, u_int y, u_int w, u_int h)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry;
	struct kitty_placement_cache	*kpl;

	if (ko == NULL)
		return;

	TAILQ_FOREACH(entry, &ko->images, entry) {
		TAILQ_FOREACH(kpl, &entry->placements, entry) {
			if (kpl->pending_delete)
				continue;
			if (kpl->x >= x + w ||
			    kpl->x + kpl->w <= x ||
			    kpl->y >= y + h ||
			    kpl->y + kpl->h <= y) {
				continue;
			}
			kpl->pending_delete = 1;
			ko->changed = 1;

			/* Preserve pieces outside the redraw area. */
			kitty_redraw_keep(entry, kpl, x, y, w, h);
		}
	}
}

/* Delete one terminal placement, retaining its uploaded pixels. */
static void
kitty_delete_placement(struct tty *tty, struct kitty_image_cache *entry,
    struct kitty_placement_cache *kpl)
{
	char	s[256];

	xsnprintf(s, sizeof s, "\033_Ga=d,d=i,i=%u,p=%u,q=2\033\\",
	    entry->kitty_id, kpl->id);
	if (entry->kitty_id != 0 && kpl->id != 0)
		tty_puts(tty, s);
	TAILQ_REMOVE(&entry->placements, kpl, entry);
	free(kpl);
}

/* Move cached grid rows with a terminal scroll region. */
void
kitty_scroll(struct tty *tty, u_int x, u_int y, u_int w, u_int h, int n)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry;
	struct kitty_placement_cache	*kpl, *next;
	int				 py;

	if (ko == NULL || !tty_term_flag(tty->term, TTYC_ISCRL))
		return;
	TAILQ_FOREACH(entry, &ko->images, entry) {
		TAILQ_FOREACH_SAFE(kpl, &entry->placements, entry, next) {
			if (kpl->x < x ||
			    kpl->x + kpl->w > x + w ||
			    kpl->y < y ||
			    kpl->y >= y + h)
				continue;
			py = (int)kpl->y - n;
			if (py < (int)y || py >= (int)(y + h)) {
				kitty_delete_placement(tty, entry, kpl);
				ko->changed = 1;
			} else
				kpl->y = py;
		}
	}
}

/* Free a cached Kitty image. */
static void
kitty_free_entry(struct tty *tty, struct kitty_image_cache *entry, int send)
{
	struct kitty_placement_cache	*kpl, *next;

	if (send && entry->kitty_id != 0)
		kitty_delete(tty, entry->kitty_id);
	TAILQ_FOREACH_SAFE(kpl, &entry->placements, entry, next) {
		TAILQ_REMOVE(&entry->placements, kpl, entry);
		free(kpl);
	}
	free(entry);
}

/* Free Kitty output state for a terminal. */
void
kitty_free_output_state(struct tty *tty, int send)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry, *next;

	if (ko != NULL) {
		TAILQ_FOREACH_SAFE(entry, &ko->images, entry, next) {
			TAILQ_REMOVE(&ko->images, entry, entry);
			kitty_free_entry(tty, entry, send);
		}
		free(ko);
		tty->image_data = NULL;
	}
}

/* Remove cached Kitty images no longer held by the server. */
static void
kitty_free_stale_images(struct tty *tty)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry, *next;

	if (ko != NULL) {
		TAILQ_FOREACH_SAFE(entry, &ko->images, entry, next) {
			if (image_find(entry->image_id) == NULL) {
				TAILQ_REMOVE(&ko->images, entry, entry);
				kitty_free_entry(tty, entry, 1);
				ko->changed = 1;
			}
		}
	}
}

/* Check whether any placements of two images overlap. */
static int
kitty_images_overlap(struct kitty_image_cache *a, struct kitty_image_cache *b)
{
	struct kitty_placement_cache	*akpl, *bkpl;

	TAILQ_FOREACH(akpl, &a->placements, entry) {
		TAILQ_FOREACH(bkpl, &b->placements, entry) {
			if (akpl->x < bkpl->x + bkpl->w &&
			    bkpl->x < akpl->x + akpl->w &&
			    akpl->y < bkpl->y + bkpl->h &&
			    bkpl->y < akpl->y + akpl->h)
				return (1);
		}
	}
	return (0);
}

/* Upload a canvas prepared for this terminal's cell pixel size. */
static int
kitty_upload(struct tty *tty, struct kitty_image_cache *entry)
{
	struct kitty_output	*ko = tty->image_data;
	struct image		*im = image_find(entry->image_id);
	struct image_geom	*ig = image_get_geometry(im);
	char			 s[128], e[4097];
	u_char			*p, *row, *p0;
	size_t			 offset, size, total, limit, stride;
	u_int			 x, y, w, h;
	u_int			 cw = entry->xpixel, ch = entry->ypixel;
	uint64_t		 width = (uint64_t)ig->sx * cw;
	uint64_t		 height = (uint64_t)ig->sy * ch;
	int			 elen;

	/* Check the output size and allow for a one-pixel border. */
	limit = options_get_number(global_options, "image-size-limit");
	if (width > UINT_MAX - 2 ||
	    height > UINT_MAX - 2 ||
	    width * height > limit / 4)
		return (0);
	w = width + 2;
	h = height + 2;
	if ((uint64_t)w * h > SIZE_MAX / 4)
		return (0);
	stride = (size_t)w * 4;
	total = stride * h;

	/* Allocate and scale the output pixels. */
	p = xmalloc(total);
	for (y = 0; y < height; y++) {
		row = p + (y + 1) * stride;
		for (x = 0; x < width; x++) {
			p0 = image_get_pixel(im, x, y, width, height);
			memcpy(row + (x + 1) * 4, p0, 4);
		}

		/* Fill the side borders with copies of the edge pixels. */
		memcpy(row, row + 4, 4);
		memcpy(row + stride - 4, row + stride - 8, 4);
	}
	memcpy(p, p + stride, stride);
	memcpy(p + (h - 1) * stride, p + (h - 2) * stride, stride);

	entry->upload = 0;
	do
		entry->kitty_id = ++ko->next_id;
	while (entry->kitty_id == 0);

	/* Encode and send the pixels in base64 chunks. */
	for (offset = 0; offset < total; offset += size) {
		size = total - offset;
		if (size > KITTY_CHUNK_SIZE)
			size = KITTY_CHUNK_SIZE;
		elen = b64_ntop(p + offset, size, e, sizeof e);
		if (elen < 0)
			fatalx("base64 encode failed");
		if (offset == 0) {
			xsnprintf(s, sizeof s,
			    "\033_Ga=t,f=32,s=%u,v=%u,i=%u,q=2,m=%d;",
			    w, h, entry->kitty_id, offset + size < total);
		} else {
			if (offset + size < total)
				xsnprintf(s, sizeof s, "\033_Gm=1;");
			else
				xsnprintf(s, sizeof s, "\033_Gm=0;");
		}
		tty_puts(tty, s);
		tty_putn(tty, e, elen, 0);
		tty_puts(tty, "\033\\");
	}
	free(p);
	return (1);
}

/* Finish drawing, replacing overlapping images if needed. */
void
kitty_redraw_finish(struct tty *tty)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry, *other, *next_entry;
	struct kitty_placement_cache	*kpl, *next;
	int				 changed;

	if (ko == NULL)
		return;
	kitty_free_stale_images(tty);

	if (!ko->changed)
		return;
	ko->changed = 0;

	/* Delete placements marked during the redraw. */
	TAILQ_FOREACH_SAFE(entry, &ko->images, entry, next_entry) {
		TAILQ_FOREACH_SAFE(kpl, &entry->placements, entry, next) {
			if (kpl->pending_delete)
				kitty_delete_placement(tty, entry, kpl);
		}
		if (TAILQ_EMPTY(&entry->placements)) {
			TAILQ_REMOVE(&ko->images, entry, entry);
			kitty_free_entry(tty, entry, 1);
			continue;
		}
		if (entry->kitty_id == 0)
			entry->upload = 1;
	}

	/* Include the overlap group if an upload needs to be replaced. */
	do {
		changed = 0;
		TAILQ_FOREACH(entry, &ko->images, entry) {
			other = entry;
			while ((other = TAILQ_NEXT(other, entry)) != NULL) {
				if (entry->upload && other->upload)
					continue;
				if (entry->zindex != other->zindex)
					continue;
				if (!entry->upload &&
				    !other->upload &&
				    entry->kitty_id < other->kitty_id)
					continue;
				if (!kitty_images_overlap(entry, other))
					continue;
				entry->upload = other->upload = 1;
				changed = 1;
			}
		}
	} while (changed);

	/* Delete affected uploads and reset their placement IDs. */
	TAILQ_FOREACH(entry, &ko->images, entry) {
		if (!entry->upload || entry->kitty_id == 0)
			continue;
		kitty_delete(tty, entry->kitty_id);
		entry->kitty_id = 0;
		TAILQ_FOREACH(kpl, &entry->placements, entry)
			kpl->id = 0;
	}

	/* Upload images in drawing order and send new placements. */
	TAILQ_FOREACH(entry, &ko->images, entry) {
		if (entry->upload && !kitty_upload(tty, entry))
			continue;
		TAILQ_FOREACH(kpl, &entry->placements, entry) {
			if (kpl->id == 0)
				kitty_send_placement(tty, entry, kpl);
		}
	}
}

/* Draw an image rectangle using the Kitty graphics protocol. */
void
kitty_draw_rect(struct tty *tty, struct image_rect *rect)
{
	struct kitty_image_cache	*entry;

	entry = kitty_get_image(tty, rect->image, rect->index);
	kitty_get_output(tty)->changed = 1;
	kitty_place(entry, rect->src_x, rect->src_y,
	    rect->sx, rect->sy, rect->dst_x, rect->dst_y);
}

/* Record the first error found in a command. */
static void
kitty_set_error(const char **m, const char *error)
{
	if (*m == NULL)
		*m = error;
}

/* Reset a command to the default values. */
static void
kitty_command_reset(struct kitty_command *cmd)
{
	memset(cmd, 0, sizeof *cmd);
	cmd->action = KITTY_ACTION_TRANSMIT;
	cmd->format = KITTY_FORMAT_RGBA;
	cmd->transmission = 'd';
	cmd->delete_action = 'a';
}

/* Parse Kitty graphics control data, continuing after the first error. */
static int
kitty_parse_control(struct kitty_command *cmd, const u_char *data, size_t size,
    const char **m)
{
	char		*copy, *ptr, *next, *value;
	const char	*errstr;
	long long	 ll;
	u_int		 number;
	char		 key;

	*m = NULL;

	copy = ptr = xstrndup(data, size);
	while ((next = strsep(&ptr, ",")) != NULL) {
		if (*next == '\0') {
			if (ptr == NULL)
				break;
			kitty_set_error(m, "EINVAL");
			continue;
		}
		key = next[0];
		if (next[1] != '=' || next[2] == '\0') {
			kitty_set_error(m, "EINVAL");
			continue;
		}
		value = next + 2;

		switch (key) {
		case 'a':
		case 't':
		case 'o':
		case 'd':
			if (value[1] != '\0') {
				kitty_set_error(m, "EINVAL");
				break;
			}
			switch (key) {
			case 'a':
				cmd->action = value[0];
				break;
			case 't':
				cmd->transmission = value[0];
				break;
			case 'o':
				cmd->compression = value[0];
				break;
			case 'd':
				cmd->delete_action = value[0];
				break;
			}
			break;
		case 'z':
			ll = strtonum(value, INT32_MIN, INT32_MAX, &errstr);
			if (errstr != NULL)
				kitty_set_error(m, "EINVAL");
			else
				cmd->zindex = ll;
			break;
		case 'O':
			ll = strtonum(value, 0, LLONG_MAX, &errstr);
			if (errstr != NULL || (off_t)ll != ll)
				kitty_set_error(m, "EINVAL");
			else
				cmd->data_offset = ll;
			break;
		case 'U':
		case 'P':
		case 'Q':
		case 'H':
		case 'V':
			if (strtonum(value, INT_MIN, UINT_MAX, &errstr) != 0 ||
			    errstr != NULL)
				kitty_set_error(m, "ENOTSUP");
			break;
		case 'f':
		case 's':
		case 'v':
		case 'i':
		case 'I':
		case 'p':
		case 'q':
		case 'm':
		case 'S':
		case 'x':
		case 'y':
		case 'w':
		case 'h':
		case 'X':
		case 'Y':
		case 'c':
		case 'r':
		case 'C':
			number = strtonum(value, 0, UINT_MAX, &errstr);
			if (errstr != NULL) {
				kitty_set_error(m, "EINVAL");
				break;
			}
			switch (key) {
			case 'f':
				cmd->format = number;
				break;
			case 's':
				cmd->pixel_width = number;
				break;
			case 'v':
				cmd->pixel_height = number;
				break;
			case 'i':
				cmd->image_id = number;
				break;
			case 'I':
				cmd->image_number = number;
				break;
			case 'p':
				cmd->placement_id = number;
				break;
			case 'q':
				cmd->quiet = number;
				break;
			case 'm':
				cmd->more = (number != 0);
				break;
			case 'S':
				cmd->data_size = number;
				break;
			case 'x':
				cmd->src_x = number;
				break;
			case 'y':
				cmd->src_y = number;
				break;
			case 'w':
				cmd->src_w = number;
				break;
			case 'h':
				cmd->src_h = number;
				break;
			case 'X':
				cmd->cell_xoff = number;
				break;
			case 'Y':
				cmd->cell_yoff = number;
				break;
			case 'c':
				cmd->cols = number;
				break;
			case 'r':
				cmd->rows = number;
				break;
			case 'C':
				cmd->cursor_no_move = (number != 0);
				break;
			}
			break;
		}
	}
	free(copy);
	if (*m != NULL)
		return (-1);
	return (0);
}

/* Check a complete command before acting on it. */
static const char *
kitty_check_command(struct kitty_command *cmd)
{
	if (cmd->image_id == 0 && cmd->image_number == 0)
		cmd->placement_id = 0;
	if (cmd->image_id != 0 && cmd->image_number != 0)
		return ("EINVAL");

	switch (cmd->transmission) {
	case 'd':
		break;
	case 'f':
	case 't':
		cmd->more = 0;
		break;
	case 's':
		return ("ENOTSUP");
	default:
		return ("EINVAL");
	}

	switch (cmd->action) {
	case KITTY_ACTION_QUERY:
	case KITTY_ACTION_TRANSMIT:
	case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
	case KITTY_ACTION_PLACE:
	case KITTY_ACTION_DELETE:
		return (NULL);
	case KITTY_ACTION_TRANSMIT_FRAME:
	case KITTY_ACTION_CONTROL_ANIMATION:
	case KITTY_ACTION_COMPOSE_FRAME:
		return ("ENOTSUP");
	}
	return ("EINVAL");
}

/* Free Kitty graphics parser state. */
void
kitty_free_state(void *state)
{
	struct kitty_state		*ks = state;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	if (ks == NULL)
		return;
	TAILQ_REMOVE(&kitty_states, ks, entry);
	while ((map = TAILQ_FIRST(&ks->images)) != NULL) {
		TAILQ_REMOVE(&ks->images, map, entry);
		while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			free(kpl);
		}
		image_free(image_find(map->image_id));
		free(map);
	}
	free(ks->data);
	free(ks);
}

/* Remove a Kitty image. */
void
kitty_remove_image(struct image *im)
{
	struct kitty_state		*ks;
	struct kitty_image_map		*map, *next;
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 id = image_get_id(im);

	TAILQ_FOREACH(ks, &kitty_states, entry) {
		TAILQ_FOREACH_SAFE(map, &ks->images, entry, next) {
			TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
				if (map->image_id != id && kpl->image_id != id)
					continue;
				TAILQ_REMOVE(&map->placements, kpl, entry);
				free(kpl);
			}
			if (map->image_id != id)
				continue;
			TAILQ_REMOVE(&ks->images, map, entry);
			image_free(im);
			free(map);
		}
	}
}

/* Find an image mapping by Kitty image ID. */
static struct kitty_image_map *
kitty_image_map_find(struct kitty_state *ks, u_int id)
{
	struct kitty_image_map	*map;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->kitty_id == id)
			return (map);
	}
	return (NULL);
}

/* Find the newest image mapping with a Kitty image number. */
static struct kitty_image_map *
kitty_image_map_find_number(struct kitty_state *ks, u_int number)
{
	struct kitty_image_map	*map;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->number == number)
			return (map);
	}
	return (NULL);
}

/* Find the image mapping selected by a command's image ID or number. */
static struct kitty_image_map *
kitty_image_map_select(struct kitty_state *ks, struct kitty_command *cmd)
{
	if (cmd->image_id != 0)
		return (kitty_image_map_find(ks, cmd->image_id));
	if (cmd->image_number != 0)
		return (kitty_image_map_find_number(ks, cmd->image_number));
	return (NULL);
}

/* Choose an unused Kitty image ID for an image sent with only a number. */
static u_int
kitty_image_next_id(struct kitty_state *ks)
{
	struct kitty_image_map	*map;
	u_int			 next = 1;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->kitty_id >= next)
			next = map->kitty_id + 1;
		if (next == 0)
			next = 1;
	}
	while (kitty_image_map_find(ks, next) != NULL) {
		if (++next == 0)
			next = 1;
	}
	return (next);
}

/* Replace the image associated with a Kitty image ID. */
static struct kitty_image_map *
kitty_image_map_set(struct kitty_state *ks, struct screen_write_ctx *ctx,
    u_int id, u_int number, struct image *im)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct image			*old;

	if (id == 0)
		return (NULL);
	map = kitty_image_map_find(ks, id);
	if (map != NULL) {
		old = image_find(map->image_id);
		image_set_order_id(im, image_get_order_id(old));
		while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			image_clear(ctx, kpl->image_id);
			free(kpl);
		}
		image_free(image_find(map->image_id));
	} else {
		map = xcalloc(1, sizeof *map);
		map->kitty_id = id;
		TAILQ_INIT(&map->placements);
		TAILQ_INSERT_HEAD(&ks->images, map, entry);
	}
	image_ref(im);
	map->image_id = image_get_id(im);
	map->number = number;
	return (map);
}

/* Replace a named placement, or add a new anonymous placement. */
static void
kitty_placement_map_set(struct screen_write_ctx *ctx,
    struct kitty_image_map *map, u_int id, struct image *im)
{
	struct kitty_placement_map	*kpl, *kpl1;

	if (map == NULL)
		return;
	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (image_find(kpl->image_id) == NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			free(kpl);
			continue;
		}
		if (id != 0 && kpl->kitty_id == id)
			break;
	}
	if (kpl == NULL) {
		kpl = xcalloc(1, sizeof *kpl);
		kpl->kitty_id = id;
		TAILQ_INSERT_TAIL(&map->placements, kpl, entry);
	} else
		image_clear(ctx, kpl->image_id);
	kpl->image_id = image_get_id(im);
}

/* Remove mappings for absent placements. */
static u_int
kitty_prune_placements(struct kitty_image_map *map, struct screen *s)
{
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 removed = 0;
	struct grid			*sgd = s->saved_grid;

	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (image_grid_has_image(s->grid, kpl->image_id))
			continue;
		if (sgd != NULL && image_grid_has_image(sgd, kpl->image_id))
			continue;
		TAILQ_REMOVE(&map->placements, kpl, entry);
		free(kpl);
		removed++;
	}
	return (removed);
}

/* Check whether a placement image is in a z-index layer. */
static int
kitty_match_zindex(struct image *im, void *arg)
{
	return (image_get_zindex(im) == *(int32_t *)arg);
}

/* Remove placements intersecting cells selected by a delete command. */
static int
kitty_delete_area(struct kitty_command *cmd, struct screen *s, char how)
{
	struct grid	*gd = s->grid;
	u_int		 hsize = gd->hsize;
	int32_t		*z = &cmd->zindex;

	switch (how) {
	case 'a':
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0, hsize, gd->sx,
		    gd->sy, NULL, NULL);
		break;
	case 'c':
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, s->cx,
		    hsize + s->cy, 1, 1, NULL, NULL);
		break;
	case 'p':
	case 'q':
		if (cmd->src_x == 0 || cmd->src_y == 0 || cmd->src_y > gd->sy)
			break;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, cmd->src_x - 1,
		    hsize + cmd->src_y - 1, 1, 1,
		    how == 'q' ? kitty_match_zindex : NULL, z);
		break;
	case 'x':
		if (cmd->src_x == 0)
			break;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, cmd->src_x - 1,
		    hsize, 1, gd->sy, NULL, NULL);
		break;
	case 'y':
		if (cmd->src_y == 0 || cmd->src_y > gd->sy)
			break;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0,
		    hsize + cmd->src_y - 1, gd->sx, 1, NULL, NULL);
		break;
	case 'z':
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0, 0, gd->sx,
		    hsize + gd->sy, kitty_match_zindex, z);
		break;
	case 'i':
	case 'n':
	case 'r':
		break;
	default:
		return (-1);
	}
	return (0);
}

/* Check whether a delete command selects an image mapping by ID. */
static int
kitty_delete_selects(struct kitty_command *cmd, struct kitty_image_map *map,
    struct kitty_image_map *newest, char how)
{
	switch (how) {
	case 'i':
		if (cmd->image_id == 0)
			return (map == newest);
		return (map->kitty_id == cmd->image_id);
	case 'n':
		return (map == newest);
	case 'r':
		return (map->kitty_id >= cmd->src_x &&
		    map->kitty_id <= cmd->src_y);
	}
	return (0);
}

/* Delete selected Kitty placements. */
static const char *
kitty_handle_delete(struct kitty_state *ks, struct screen_write_ctx *ctx)
{
	struct kitty_command		*cmd = &ks->cmd;
	struct screen			*s = ctx->s;
	struct grid			*gd = s->grid;
	struct kitty_image_map		*map, *map1, *newest = NULL;
	struct kitty_placement_map	*kpl;
	u_int				 removed, placement_id;
	char				 how = cmd->delete_action;
	int				 release;

	release = (how >= 'A' && how <= 'Z');
	if (release)
		how += 'a' - 'A';
	TAILQ_FOREACH(map, &ks->images, entry)
		kitty_prune_placements(map, s);
	if (kitty_delete_area(cmd, s, how) != 0)
		return ("ENOTSUP");
	if (cmd->image_number != 0)
		newest = kitty_image_map_find_number(ks, cmd->image_number);
	placement_id = (how == 'r' ? 0 : cmd->placement_id);

	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		removed = 0;
		if (kitty_delete_selects(cmd, map, newest, how)) {
			TAILQ_FOREACH(kpl, &map->placements, entry) {
				if (placement_id != 0 &&
				    kpl->kitty_id != placement_id)
					continue;
				removed += image_grid_clear(gd, kpl->image_id);
			}
			if (placement_id == 0)
				removed++;
		}
		removed += kitty_prune_placements(map, s);
		if (!release || removed == 0 || !TAILQ_EMPTY(&map->placements))
			continue;
		TAILQ_REMOVE(&ks->images, map, entry);
		image_free(image_find(map->image_id));
		free(map);
	}
	if (ctx->wp != NULL)
		ctx->wp->flags |= PANE_REDRAW;
	return ("OK");
}

/* Decode and append one base64-encoded Kitty payload chunk. */
static int
kitty_append_data(struct kitty_state *ks, const u_char *data, size_t size)
{
	u_char	*d;
	size_t	 dsize, allocated, limit;

	limit = options_get_number(global_options, "image-size-limit");
	if (size > limit || ks->size > limit)
		return (-1);

	d = image_base64_decode(data, size, limit, &dsize);
	if (d == NULL)
		return (-1);
	if (dsize == 0) {
		free(d);
		return (0);
	}
	if (dsize > limit - ks->size) {
		free(d);
		return (-1);
	}

	if (ks->size + dsize > ks->allocated) {
		allocated = ks->allocated;
		if (allocated == 0)
			allocated = 4096;
		while (allocated < ks->size + dsize) {
			if (allocated > limit / 2)
				allocated = limit;
			else
				allocated *= 2;
		}
		ks->data = xrealloc(ks->data, allocated);
		ks->allocated = allocated;
	}
	memcpy(ks->data + ks->size, d, dsize);
	ks->size += dsize;

	free(d);
	return (0);
}

/* Discard an unfinished transfer. */
static void
kitty_reset_transfer(struct kitty_state *ks)
{
	free(ks->data);
	ks->data = NULL;
	ks->size = 0;
	ks->allocated = 0;
	ks->cmd.more = 0;
}

/* Return if a temporary file may be removed after reading. */
static int
kitty_temp_path_allowed(const char *path)
{
	const char	*base, *tmpdir;
	size_t		 len;

	base = strrchr(path, '/');
	if (base == NULL)
		return (0);
	if (strstr(base + 1, "tty-graphics-protocol") == NULL)
		return (0);

	if (strncmp(path, "/tmp/", 5) == 0 ||
	    strncmp(path, "/var/tmp/", 9) == 0 ||
	    strncmp(path, "/dev/shm/", 9) == 0)
		return (1);

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL || *tmpdir == '\0')
		return (0);
	len = strlen(tmpdir);
	while (len > 1 && tmpdir[len - 1] == '/')
		len--;
	if (strncmp(path, tmpdir, len) == 0 && path[len] == '/')
		return (1);
	return (0);
}

/* Replace a file name payload with data read from the file. */
static int
kitty_read_file(struct kitty_state *ks)
{
	struct kitty_command	*cmd = &ks->cmd;
	struct stat		 sb;
	char			*path;
	u_char			*buf = NULL;
	size_t			 size, done = 0, limit;
	ssize_t			 n;
	int			 fd;

	if (ks->size == 0 || memchr(ks->data, '\0', ks->size) != NULL)
		return (-1);
	path = xstrndup(ks->data, ks->size);

	fd = open(path, O_RDONLY|O_NONBLOCK);
	if (fd == -1)
		goto fail;
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode))
		goto fail;
	if (cmd->data_offset > sb.st_size)
		goto fail;
	size = sb.st_size - cmd->data_offset;
	if (cmd->data_size != 0) {
		if (cmd->data_size > size)
			goto fail;
		size = cmd->data_size;
	}
	limit = options_get_number(global_options, "image-size-limit");
	if (size == 0 || size > limit)
		goto fail;

	buf = xmalloc(size);
	while (done < size) {
		n = pread(fd, buf + done, size - done,
		    cmd->data_offset + done);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			goto fail;
		done += n;
	}
	close(fd);

	free(ks->data);
	ks->data = buf;
	ks->size = ks->allocated = size;

	if (cmd->transmission == 't' && kitty_temp_path_allowed(path))
		unlink(path);
	free(path);
	return (0);

fail:
	log_debug("%s: failed to read %s", __func__, path);
	if (fd != -1)
		close(fd);
	free(buf);
	free(path);
	return (-1);
}

/* Reply to a Kitty graphics sequence. */
static void
kitty_send_reply(struct input_ctx *ictx, struct kitty_command *cmd,
    const char *m)
{
	char	i[96] = "";
	char	I[96] = "";
	char	p[96] = "";

	if (cmd->quiet >= 2)
		return;
	if (cmd->action == KITTY_ACTION_DELETE && strcmp(m, "OK") == 0)
		return;
	if (cmd->quiet == 1 && strcmp(m, "OK") == 0)
		return;
	if (cmd->image_id == 0 && cmd->image_number == 0 &&
	    cmd->action != KITTY_ACTION_QUERY)
		return;

	if (cmd->image_id != 0 || cmd->image_number == 0)
		xsnprintf(i, sizeof i, "i=%u", cmd->image_id);
	if (cmd->image_number != 0) {
		if (*i != '\0')
			strlcat(i, ",", sizeof i);
		xsnprintf(I, sizeof I, "I=%u", cmd->image_number);
	}
	if (cmd->placement_id != 0)
		xsnprintf(p, sizeof p, ",p=%u", cmd->placement_id);
	input_reply(ictx, 0, "\033_G%s%s%s;%s\033\\", i, I, p, m);
}

/* Create a Kitty placement with shared source pixels and stored geometry. */
static struct image *
kitty_place_image(struct image *src, struct kitty_command *cmd, u_int xpixel,
    u_int ypixel)
{
	struct image_geom	*sg = image_get_geometry(src);
	enum image_type		 type = image_get_type(src);
	struct image_geom	 ig = { 0 };
	struct image		*im;
	u_char			*pixels;
	size_t			 stride, limit;
	uint64_t		 cw, ch;
	double			 size;

	ig.src_x = cmd->src_x;
	ig.src_y = cmd->src_y;

	/* Clip the source rectangle. */
	if (ig.src_x >= sg->src_w || ig.src_y >= sg->src_h)
		return (NULL);
	ig.src_w = sg->src_w - ig.src_x;
	if (cmd->src_w != 0 && ig.src_w > cmd->src_w)
		ig.src_w = cmd->src_w;
	ig.src_h = sg->src_h - ig.src_y;
	if (cmd->src_h != 0 && ig.src_h > cmd->src_h)
		ig.src_h = cmd->src_h;

	/* Clamp offsets to the starting cell. */
	if (xpixel == 0)
		xpixel = 8;
	if (ypixel == 0)
		ypixel = 16;
	ig.canvas_xoff = cmd->cell_xoff;
	if (ig.canvas_xoff >= xpixel)
		ig.canvas_xoff = xpixel - 1;
	ig.canvas_yoff = cmd->cell_yoff;
	if (ig.canvas_yoff >= ypixel)
		ig.canvas_yoff = ypixel - 1;

	/* Work out the size. */
	ig.sx = cmd->cols;
	ig.sy = cmd->rows;
	ig.scaled_width = ig.src_w;
	ig.scaled_height = ig.src_h;
	if (ig.sx == 0 && ig.sy == 0) {
		if (ig.canvas_xoff > UINT_MAX - ig.src_w ||
		    ig.canvas_yoff > UINT_MAX - ig.src_h)
			return (NULL);
		image_size_in_cells(ig.src_w + ig.canvas_xoff,
		    ig.src_h + ig.canvas_yoff, xpixel, ypixel, &ig.sx, &ig.sy);
	} else if (ig.sy == 0) {
		ig.scaled_width = (double)ig.sx * xpixel - ig.canvas_xoff;
		ig.scaled_height = ig.src_h * ig.scaled_width / ig.src_w;

		size = ceil((ig.scaled_height + ig.canvas_yoff) / ypixel);
		if (size > USHRT_MAX)
			return (NULL);
		ig.sy = size;
	} else if (ig.sx == 0) {
		ig.scaled_height = (double)ig.sy * ypixel - ig.canvas_yoff;
		ig.scaled_width = ig.src_w * ig.scaled_height / ig.src_h;

		size = ceil((ig.scaled_width + ig.canvas_xoff) / xpixel);
		if (size > USHRT_MAX)
			return (NULL);
		ig.sx = size;
	} else {
		ig.scaled_width = (double)ig.sx * xpixel - ig.canvas_xoff;
		ig.scaled_height = (double)ig.sy * ypixel - ig.canvas_yoff;
	}
	limit = options_get_number(global_options, "image-size-limit");
	if (ig.sx > USHRT_MAX ||
	    ig.sy > USHRT_MAX ||
	    ig.sx * ig.sy > limit)
		return (NULL);

	/* Record geometry in the input terminal's pixel coordinates. */
	cw = (uint64_t)ig.sx * xpixel;
	ch = (uint64_t)ig.sy * ypixel;
	if (cw > UINT_MAX || ch > UINT_MAX)
		return (NULL);
	ig.canvas_width = cw;
	ig.canvas_height = ch;
	pixels = image_get_pixels(src, &stride, NULL);
	im = image_create_with_parent(type, src, &ig, stride, pixels);
	if (im != NULL) {
		image_set_order_id(im, image_get_order_id(src));
		image_set_zindex(im, cmd->zindex);
	}
	return (im);
}

/* Display a placement at the cursor and record its placement ID. */
static void
kitty_display(struct screen_write_ctx *ctx, struct kitty_command *cmd,
    struct kitty_image_map *map, struct image *im, u_int bg)
{
	kitty_placement_map_set(ctx, map, cmd->placement_id, im);
	image_write(ctx, im, bg, cmd->cursor_no_move);
}

/* Place an image which was uploaded earlier. */
static const char *
kitty_handle_place(struct kitty_state *ks, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg)
{
	struct kitty_command	*cmd = &ks->cmd;
	struct window		*w = wp->window;
	struct kitty_image_map	*map;
	struct image		*src, *im;

	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return ("ENOENT");
	cmd->image_id = map->kitty_id;
	src = image_find(map->image_id);
	image_ref(src);

	im = kitty_place_image(src, cmd, w->xpixel, w->ypixel);
	if (im == NULL) {
		image_free(src);
		return ("EINVAL");
	}
	kitty_display(ctx, cmd, map, im, bg);
	image_free(im);
	image_free(src);
	return ("OK");
}

/* Decode an upload and store it, place it, or only check it for a query. */
static const char *
kitty_handle_transmit(struct kitty_state *ks, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg)
{
	struct kitty_command	*cmd = &ks->cmd;
	struct window		*w = wp->window;
	struct image_geom	 ig = { 0 };
	struct kitty_image_map	*map = NULL;
	struct image		*src, *im = NULL;
	u_char			*p = NULL;
	size_t			 limit;
	u_int			 sx, sy;
	uint64_t		 cw, ch;
	int			 z;

	if (cmd->transmission != 'd' && kitty_read_file(ks) != 0)
		return ("EBADF");

	/* Decode the completed transfer into RGBA pixels. */
	if (cmd->compression != '\0' && cmd->compression != 'z')
		return ("EINVAL");
	z = (cmd->compression == 'z');
	limit = options_get_number(global_options, "image-size-limit");
	switch (cmd->format) {
	case KITTY_FORMAT_RGB:
	case KITTY_FORMAT_RGBA:
		p = image_raw_decode(ks->data, ks->size, limit, cmd->format,
		    cmd->pixel_width, cmd->pixel_height, z);
		break;
	case KITTY_FORMAT_PNG:
		p = image_png_decode(ks->data, ks->size, limit, z,
		    cmd->data_size, &cmd->pixel_width, &cmd->pixel_height);
		break;
	}
	if (p == NULL)
		return ("EINVAL");

	cw = (w->xpixel == 0 ? 8 : w->xpixel);
	ch = (w->ypixel == 0 ? 16 : w->ypixel);
	image_size_in_cells(cmd->pixel_width, cmd->pixel_height, cw, ch, &sx,
	    &sy);
	cw *= sx;
	ch *= sy;
	if (cw > UINT_MAX || ch > UINT_MAX) {
		free(p);
		return ("EINVAL");
	}
	ig.src_w = ig.scaled_width = cmd->pixel_width;
	ig.src_h = ig.scaled_height = cmd->pixel_height;
	ig.canvas_width = cw;
	ig.canvas_height = ch;
	ig.sx = sx;
	ig.sy = sy;
	src = image_create(IMAGE_TYPE_KITTY, &ig, p);
	if (src == NULL) {
		free(p);
		return ("EINVAL");
	}
	if (cmd->action == KITTY_ACTION_QUERY) {
		image_free(src);
		return ("OK");
	}

	if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
		im = kitty_place_image(src, cmd, w->xpixel, w->ypixel);
		if (im == NULL) {
			image_free(src);
			return ("EINVAL");
		}
	}
	if (cmd->image_id == 0 && cmd->image_number != 0)
		cmd->image_id = kitty_image_next_id(ks);
	map = kitty_image_map_set(ks, ctx, cmd->image_id, cmd->image_number,
	    src);
	if (im != NULL) {
		image_set_order_id(im, image_get_order_id(src));
		kitty_display(ctx, cmd, map, im, bg);
		image_free(im);
	}
	image_free(src);
	return ("OK");
}

/* Parse and reply to a Kitty graphics sequence. */
void
kitty_parse_input(struct input_ctx *ictx, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, void **state, const u_char *buf,
    size_t len)
{
	struct kitty_state	*ks = *state;
	struct kitty_command	*cmd, next;
	const u_char		*found;
	const char		*m;
	size_t			 csize, psize;

	buf++;
	len--;

	if (ks == NULL) {
		ks = xcalloc(1, sizeof *ks);
		TAILQ_INIT(&ks->images);
		TAILQ_INSERT_TAIL(&kitty_states, ks, entry);
		*state = ks;
	}
	cmd = &ks->cmd;

	found = memchr(buf, ';', len);
	if (found == NULL) {
		csize = len;
		psize = 0;
	} else {
		csize = found - buf;
		psize = len - csize - 1;
	}

	/* A delete aborts an unfinished transfer instead of continuing it. */
	if (cmd->more) {
		kitty_command_reset(&next);
		kitty_parse_control(&next, buf, csize, &m);
		if (next.action == KITTY_ACTION_DELETE)
			kitty_reset_transfer(ks);
	}

	/* Continuation chunks keep the controls from the first chunk. */
	if (!cmd->more)
		kitty_command_reset(cmd);
	cmd->more = 0;

	if (kitty_parse_control(cmd, buf, csize, &m) != 0)
		goto out;
	if ((m = kitty_check_command(cmd)) != NULL)
		goto out;
	if (cmd->action == KITTY_ACTION_DELETE) {
		m = kitty_handle_delete(ks, ctx);
		goto out;
	}

	/* Collect the payload until the last chunk arrives. */
	m = "EINVAL";
	if (psize != 0 && kitty_append_data(ks, found + 1, psize) != 0)
		goto out;
	if (cmd->more)
		return;

	if (cmd->action == KITTY_ACTION_PLACE)
		m = kitty_handle_place(ks, wp, ctx, bg);
	else
		m = kitty_handle_transmit(ks, wp, ctx, bg);

out:
	kitty_send_reply(ictx, cmd, m);
	kitty_reset_transfer(ks);
}
