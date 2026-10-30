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
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

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
	KITTY_FORMAT_INDEXED = 8,
	KITTY_FORMAT_INDEXED_ALPHA = 16,
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
	u_int			 hints;		/* N */
	char			 feature;	/* e */
};

/* Usage hint for images which are only needed for a short time. */
#define KITTY_HINT_TRANSIENT 0x1

/* Image data being collected or read. */
struct kitty_data {
	u_char			*data;
	size_t			 size;
	size_t			 allocated;
};

/* State attached to an input parser. */
struct kitty_state {
	struct kitty_command		 cmd;
	struct kitty_data		 load;
	int				 loading;
	int				 alt;

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

/*
 * Map Kitty image ID and number to image ID and placement views. The primary
 * and alternate screens have separate images.
 */
struct kitty_image_map {
	u_int					 kitty_id;
	u_int					 number;
	u_int					 image_id;
	int					 alt;
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

/* Check a single character value for a key. */
static int
kitty_check_value(char key, char value)
{
	switch (key) {
	case 'a':
		return (strchr("qtTfacpd", value) != NULL);
	case 't':
		return (strchr("dfts", value) != NULL);
	case 'o':
		return (value == 'z');
	case 'd':
		return (strchr("aAiInNcCpPqQrRxXyYzZfF", value) != NULL);
	}
	return (1);
}

/*
 * Parse Kitty graphics control data. Returns -1 if the command is malformed
 * and must be ignored. Errors for unsupported values are returned in m.
 */
static int
kitty_parse_control(struct kitty_command *cmd, const u_char *data, size_t size,
    const char **m)
{
	char		*copy, *ptr, *next, *value;
	const char	*errstr;
	long long	 ll;
	u_int		 number;
	char		 key;
	int		 error = 0;

	*m = NULL;

	copy = ptr = xstrndup(data, size);
	while (!error && (next = strsep(&ptr, ",")) != NULL) {
		if (*next == '\0') {
			if (ptr != NULL)
				error = 1;
			continue;
		}
		value = strchr(next, '=');
		if (value == NULL || value == next || value[1] == '\0') {
			error = 1;
			continue;
		}

		/* Keys longer than one character are ignored. */
		if (value != next + 1)
			continue;
		key = next[0];
		value++;

		switch (key) {
		case 'a':
		case 't':
		case 'o':
		case 'd':
		case 'e':
			if (value[1] != '\0' ||
			    !kitty_check_value(key, value[0])) {
				error = 1;
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
			case 'e':
				cmd->feature = value[0];
				break;
			}
			break;
		case 'z':
			ll = strtonum(value, INT32_MIN, INT32_MAX, &errstr);
			if (errstr != NULL)
				error = 1;
			else
				cmd->zindex = ll;
			break;
		case 'O':
			ll = strtonum(value, 0, LLONG_MAX, &errstr);
			if (errstr != NULL || (off_t)ll != ll)
				error = 1;
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
		case 'N':
			number = strtonum(value, 0, UINT_MAX, &errstr);
			if (errstr != NULL) {
				error = 1;
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
				cmd->cursor_no_move = (number == 1);
				break;
			case 'N':
				cmd->hints = number;
				break;
			}
			break;
		}
	}
	free(copy);
	if (error)
		return (-1);
	return (0);
}

/* Free collected image data. */
static void
kitty_data_free(struct kitty_data *kd)
{
	free(kd->data);
	memset(kd, 0, sizeof *kd);
}

/* Free a Kitty image mapping and its placement views. */
static void
kitty_image_map_free(struct kitty_state *ks, struct kitty_image_map *map)
{
	struct kitty_placement_map	*kpl;

	TAILQ_REMOVE(&ks->images, map, entry);
	while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
		TAILQ_REMOVE(&map->placements, kpl, entry);
		free(kpl);
	}
	image_free(image_find(map->image_id));
	free(map);
}

/* Free Kitty graphics parser state. */
void
kitty_free_state(void *state)
{
	struct kitty_state	*ks = state;
	struct kitty_image_map	*map;

	if (ks == NULL)
		return;
	TAILQ_REMOVE(&kitty_states, ks, entry);
	while ((map = TAILQ_FIRST(&ks->images)) != NULL)
		kitty_image_map_free(ks, map);
	kitty_data_free(&ks->load);
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

/* Find an image mapping by Kitty image ID on the current screen. */
static struct kitty_image_map *
kitty_image_map_find(struct kitty_state *ks, u_int id)
{
	struct kitty_image_map	*map;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->alt == ks->alt && map->kitty_id == id)
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
		if (map->alt == ks->alt && map->number == number)
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

/* Choose the lowest unused Kitty image ID for an image sent with a number. */
static u_int
kitty_image_next_id(struct kitty_state *ks)
{
	u_int	next = 1;

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
		image_free(old);
		TAILQ_REMOVE(&ks->images, map, entry);
	} else {
		map = xcalloc(1, sizeof *map);
		map->kitty_id = id;
		map->alt = ks->alt;
		TAILQ_INIT(&map->placements);
	}
	TAILQ_INSERT_HEAD(&ks->images, map, entry);
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
static void
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
	}
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
static void
kitty_handle_delete(struct kitty_state *ks, struct kitty_command *cmd,
    struct screen_write_ctx *ctx)
{
	struct screen			*s = ctx->s;
	struct grid			*gd = s->grid;
	struct kitty_image_map		*map, *map1, *newest = NULL;
	struct kitty_placement_map	*kpl;
	u_int				 removed, placement_id;
	char				 how = cmd->delete_action;
	int				 release;

	/* Animation frames are not stored, so there is nothing to delete. */
	if (how == 'f' || how == 'F')
		return;

	release = (how >= 'A' && how <= 'Z');
	if (release)
		how += 'a' - 'A';
	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->alt == ks->alt)
			kitty_prune_placements(map, s);
	}
	kitty_delete_area(cmd, s, how);
	if (cmd->image_number != 0)
		newest = kitty_image_map_find_number(ks, cmd->image_number);
	placement_id = (how == 'r' ? 0 : cmd->placement_id);

	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		if (map->alt != ks->alt)
			continue;
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
		kitty_image_map_free(ks, map);
	}
	if (ctx->wp != NULL)
		ctx->wp->flags |= PANE_REDRAW;
}

/*
 * Decode and append one base64-encoded Kitty payload chunk. Returns -1 for
 * invalid base64 and -2 if the data is too large.
 */
static int
kitty_append_data(struct kitty_data *kd, const u_char *data, size_t size)
{
	u_char	*d;
	size_t	 dsize, allocated, limit;

	limit = options_get_number(global_options, "image-size-limit");
	if (size / 4 * 3 > limit || kd->size > limit)
		return (-2);
	if (size == 0)
		return (0);

	d = image_base64_decode(data, size, limit, &dsize);
	if (d == NULL)
		return (-1);
	if (dsize == 0) {
		free(d);
		return (0);
	}
	if (dsize > limit - kd->size) {
		free(d);
		return (-2);
	}

	if (kd->size + dsize > kd->allocated) {
		allocated = kd->allocated;
		if (allocated == 0)
			allocated = 4096;
		while (allocated < kd->size + dsize) {
			if (allocated > limit / 2)
				allocated = limit;
			else
				allocated *= 2;
		}
		kd->data = xrealloc(kd->data, allocated);
		kd->allocated = allocated;
	}
	memcpy(kd->data + kd->size, d, dsize);
	kd->size += dsize;

	free(d);
	return (0);
}

/* Discard an unfinished transfer. */
static void
kitty_reset_transfer(struct kitty_state *ks)
{
	kitty_data_free(&ks->load);
	ks->loading = 0;
}

/* Check if a path is inside a directory. */
static int
kitty_path_in_directory(const char *path, const char *directory)
{
	size_t	len;

	if (directory == NULL || *directory == '\0')
		return (0);
	len = strlen(directory);
	while (len > 1 && directory[len - 1] == '/')
		len--;
	if (strncmp(path, directory, len) != 0)
		return (0);
	return (path[len] == '/' || (len == 1 && *directory == '/'));
}

/* Check if a resolved path is in a temporary directory. */
static int
kitty_path_is_temporary(const char *path)
{
	const char	*directory;
	char		 resolved[PATH_MAX];

	if (kitty_path_in_directory(path, "/tmp") ||
	    kitty_path_in_directory(path, "/dev/shm"))
		return (1);
	directory = getenv("TMPDIR");
	if (directory == NULL || *directory == '\0')
		directory = getenv("TMP");
	if (directory == NULL || *directory == '\0')
		return (0);
	if (kitty_path_in_directory(path, directory))
		return (1);
	if (realpath(directory, resolved) != NULL &&
	    kitty_path_in_directory(path, resolved))
		return (1);
	return (0);
}

/* Check if a resolved path is in a part of the filesystem never read. */
static int
kitty_path_is_sensitive(const char *path)
{
	if (strncmp(path, "/proc/", 6) == 0 || strncmp(path, "/sys/", 5) == 0)
		return (1);
	if (strncmp(path, "/dev/", 5) == 0 &&
	    strncmp(path, "/dev/shm/", 9) != 0)
		return (1);
	return (0);
}

/*
 * Read part of an open file or shared memory object. A raw size is used if
 * no size is given.
 */
static int
kitty_read_fd(struct kitty_command *cmd, int fd, size_t raw_size,
    struct kitty_data *kd)
{
	struct stat	 sb;
	u_char		*buf;
	size_t		 size, done = 0, limit;
	ssize_t		 n;

	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0)
		return (-1);
	if (cmd->data_offset > sb.st_size)
		return (-1);
	size = sb.st_size - cmd->data_offset;
	if (cmd->data_size != 0) {
		if (cmd->data_size > size)
			return (-1);
		size = cmd->data_size;
	} else if (raw_size != 0) {
		if (raw_size > size)
			return (-1);
		size = raw_size;
	}
	limit = options_get_number(global_options, "image-size-limit");
	if (size == 0 || size > limit)
		return (-1);

	buf = xmalloc(size);
	while (done < size) {
		n = pread(fd, buf + done, size - done,
		    cmd->data_offset + done);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0) {
			free(buf);
			return (-1);
		}
		done += n;
	}
	kd->data = buf;
	kd->size = kd->allocated = size;
	return (0);
}

/* Read image data from a file named by the payload. */
static const char *
kitty_read_file(struct kitty_command *cmd, struct kitty_data *name,
    struct kitty_data *kd)
{
	char		*path, resolved[PATH_MAX];
	const char	*m = "EINVAL: invalid data";
	int		 fd = -1;

	if (name->size == 0 || memchr(name->data, '\0', name->size) != NULL)
		return (m);
	path = xstrndup(name->data, name->size);

	/* Check the real path before opening, which may have side effects. */
	if (realpath(path, resolved) == NULL ||
	    kitty_path_is_sensitive(resolved))
		goto out;
	if (cmd->transmission == 't') {
		if (!kitty_path_is_temporary(resolved)) {
			m = "EINVAL: temporary file not in temp dir";
			goto out;
		}
		if (strstr(resolved, "tty-graphics-protocol") == NULL) {
			m = "EINVAL: temporary file not named correctly";
			goto out;
		}
	}

	fd = open(resolved, O_RDONLY|O_NONBLOCK|O_NOCTTY|O_NOFOLLOW);
	if (fd == -1)
		goto out;
	if (cmd->transmission == 't' && unlink(resolved) != 0)
		log_debug("%s: failed to remove %s", __func__, resolved);
	if (kitty_read_fd(cmd, fd, 0, kd) == 0)
		m = NULL;

out:
	if (m != NULL)
		log_debug("%s: failed to read %s", __func__, path);
	if (fd != -1)
		close(fd);
	free(path);
	return (m);
}

/* Get the number of bytes for each pixel of a raw format. */
static u_int
kitty_format_bytes(u_int format)
{
	switch (format) {
	case KITTY_FORMAT_INDEXED:
		return (1);
	case KITTY_FORMAT_INDEXED_ALPHA:
		return (2);
	case KITTY_FORMAT_RGB:
		return (3);
	case KITTY_FORMAT_RGBA:
		return (4);
	}
	return (0);
}

/* Read image data from a shared memory object named by the payload. */
static const char *
kitty_read_shm(struct kitty_command *cmd, struct kitty_data *name,
    struct kitty_data *kd)
{
	char		*path;
	const char	*m = "EINVAL: invalid data";
	size_t		 raw_size = 0, limit;
	u_int		 bytes;
	int		 fd;

	if (name->size == 0 ||
	    name->size > NAME_MAX ||
	    memchr(name->data, '\0', name->size) != NULL)
		return (m);
	path = xstrndup(name->data, name->size);
	if (strcmp(path, "/") == 0)
		goto out;

	/* Uncompressed raw data is sized from the image dimensions. */
	bytes = kitty_format_bytes(cmd->format);
	if (cmd->compression == '\0' && bytes != 0) {
		limit = options_get_number(global_options, "image-size-limit");
		if (cmd->pixel_width != 0 &&
		    cmd->pixel_height != 0 &&
		    (uint64_t)cmd->pixel_width * cmd->pixel_height >
		    limit / bytes) {
			m = "EINVAL: dimensions too large";
			goto out;
		}
		raw_size = (size_t)cmd->pixel_width * cmd->pixel_height * bytes;
	}

	fd = shm_open(path, O_RDONLY, 0);
	if (fd == -1)
		goto out;
	shm_unlink(path);
	if (kitty_read_fd(cmd, fd, raw_size, kd) == 0)
		m = NULL;
	close(fd);

out:
	if (m != NULL)
		log_debug("%s: failed to read %s", __func__, path);
	free(path);
	return (m);
}

/* Decompress image data. */
#ifdef HAVE_LIBZ
static int
kitty_inflate(struct kitty_data *kd)
{
	z_stream	 z;
	u_char		*out;
	size_t		 allocated, limit;
	int		 error;

	limit = options_get_number(global_options, "image-size-limit");
	if (kd->size == 0 || kd->size > UINT_MAX)
		return (-1);

	allocated = kd->size * 4;
	if (allocated < 4096)
		allocated = 4096;
	if (allocated > limit)
		allocated = limit;
	out = xmalloc(allocated);

	memset(&z, 0, sizeof z);
	if (inflateInit(&z) != Z_OK) {
		free(out);
		return (-1);
	}
	z.next_in = kd->data;
	z.avail_in = kd->size;
	z.next_out = out;
	z.avail_out = allocated;
	for (;;) {
		error = inflate(&z, Z_NO_FLUSH);
		if (error == Z_STREAM_END)
			break;
		if (error != Z_OK && error != Z_BUF_ERROR)
			goto fail;
		if (z.avail_out != 0 || allocated == limit)
			goto fail;
		if (allocated > limit / 2)
			allocated = limit;
		else
			allocated *= 2;
		out = xrealloc(out, allocated);
		z.next_out = out + z.total_out;
		z.avail_out = allocated - z.total_out;
	}

	free(kd->data);
	kd->data = out;
	kd->size = z.total_out;
	kd->allocated = allocated;
	inflateEnd(&z);
	return (0);

fail:
	inflateEnd(&z);
	free(out);
	return (-1);
}
#else
static int
kitty_inflate(__unused struct kitty_data *kd)
{
	return (-1);
}
#endif

/* Get the RGB colour used for a palette index in indexed image data. */
static int
kitty_indexed_colour(struct window_pane *wp, u_int idx)
{
	struct grid_cell	defaults;
	int			c;

	switch (idx) {
	case 248:
	case 251:
	case 253:
		c = window_pane_get_bg(wp);
		if (c == -1 || COLOUR_DEFAULT(c))
			c = 0;
		break;
	case 249:
	case 250:
	case 252:
	case 254:
	case 255:
		c = window_pane_get_fg_control_client(wp);
		if (c == -1) {
			tty_default_colours(&defaults, wp, NULL);
			if (COLOUR_DEFAULT(defaults.fg))
				c = window_pane_get_fg(wp);
			else
				c = defaults.fg;
		}
		if (c == -1 || COLOUR_DEFAULT(c))
			c = 7;
		break;
	default:
		c = colour_palette_get(&wp->palette, idx|COLOUR_FLAG_256);
		if (c == -1)
			c = idx|COLOUR_FLAG_256;
		break;
	}
	c = colour_force_rgb(c);
	if (c == -1)
		return (0);
	return (c);
}

/* Convert indexed image data to RGBA using the pane's colours. */
static u_char *
kitty_indexed_decode(struct window_pane *wp, struct kitty_data *kd,
    u_int format, u_int width, u_int height)
{
	int	 colours[256];
	u_char	*out, *p, *src = kd->data;
	size_t	 i, n = (size_t)width * height;
	u_int	 bytes = kitty_format_bytes(format);

	if (kd->size != n * bytes)
		return (NULL);
	for (i = 0; i < nitems(colours); i++)
		colours[i] = kitty_indexed_colour(wp, i);

	out = p = xmalloc(n * 4);
	for (i = 0; i < n; i++, src += bytes, p += 4) {
		colour_split_rgb(colours[src[0]], &p[0], &p[1], &p[2]);
		p[3] = (bytes == 2 ? src[1] : 255);
	}
	return (out);
}

/*
 * Read a completed upload from its medium and decode it to RGBA pixels. The
 * payload is the data itself or the name of a file or shared memory object.
 */
static u_char *
kitty_load(struct kitty_command *cmd, struct window_pane *wp,
    struct kitty_data *payload, const char **m)
{
	struct kitty_data	 kd = { 0 };
	u_char			*p = NULL;
	size_t			 limit;
	u_int			 bytes = kitty_format_bytes(cmd->format);

	if (bytes == 0 && cmd->format != KITTY_FORMAT_PNG) {
		*m = "EINVAL: unsupported format";
		return (NULL);
	}

	*m = NULL;
	switch (cmd->transmission) {
	case 'd':
		kd = *payload;
		memset(payload, 0, sizeof *payload);
		break;
	case 'f':
	case 't':
		*m = kitty_read_file(cmd, payload, &kd);
		break;
	case 's':
		*m = kitty_read_shm(cmd, payload, &kd);
		break;
	}
	if (*m != NULL)
		goto out;

	if (cmd->compression == 'z' && kitty_inflate(&kd) != 0) {
		*m = "EINVAL: decompression failed";
		goto out;
	}

	limit = options_get_number(global_options, "image-size-limit");
	if (cmd->format == KITTY_FORMAT_PNG) {
		p = image_png_decode(kd.data, kd.size, limit, 0, 0,
		    &cmd->pixel_width, &cmd->pixel_height);
		if (p == NULL)
			*m = "EINVAL: invalid data";
		goto out;
	}

	if (cmd->pixel_width == 0 || cmd->pixel_height == 0) {
		*m = "EINVAL: dimensions required";
		goto out;
	}
	if ((uint64_t)cmd->pixel_width * cmd->pixel_height > limit / 4) {
		*m = "EINVAL: dimensions too large";
		goto out;
	}
	if (bytes >= 3) {
		p = image_raw_decode(kd.data, kd.size, limit, cmd->format,
		    cmd->pixel_width, cmd->pixel_height, 0);
	} else {
		p = kitty_indexed_decode(wp, &kd, cmd->format,
		    cmd->pixel_width, cmd->pixel_height);
	}
	if (p == NULL)
		*m = "EINVAL: invalid data";

out:
	kitty_data_free(&kd);
	return (p);
}

/* Reply to a Kitty graphics sequence, adding any extra keys after the IDs. */
static void
kitty_send_reply(struct input_ctx *ictx, struct kitty_command *cmd,
    const char *extra, const char *m)
{
	char	ids[128] = "", key[32];

	if (cmd->quiet >= 2)
		return;
	if (cmd->quiet == 1 && strcmp(m, "OK") == 0)
		return;
	if (cmd->image_id == 0 && cmd->image_number == 0)
		return;

	if (cmd->image_id != 0)
		xsnprintf(ids, sizeof ids, "i=%u", cmd->image_id);
	if (cmd->image_number != 0) {
		xsnprintf(key, sizeof key, "%sI=%u", *ids == '\0' ? "" : ",",
		    cmd->image_number);
		strlcat(ids, key, sizeof ids);
	}
	if (cmd->placement_id != 0) {
		xsnprintf(key, sizeof key, ",p=%u", cmd->placement_id);
		strlcat(ids, key, sizeof ids);
	}
	if (extra == NULL)
		extra = "";
	input_reply(ictx, 0, "\033_G%s%s;%s\033\\", ids, extra, m);
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
		ig.scaled_height = round(ig.src_h * ig.scaled_width / ig.src_w);

		size = ceil((ig.scaled_height + ig.canvas_yoff) / ypixel);
		if (size > USHRT_MAX)
			return (NULL);
		ig.sy = size;
	} else if (ig.sx == 0) {
		ig.scaled_height = (double)ig.sy * ypixel - ig.canvas_yoff;
		ig.scaled_width = round(ig.src_w * ig.scaled_height / ig.src_h);

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
kitty_handle_place(struct kitty_state *ks, struct kitty_command *cmd,
    struct window_pane *wp, struct screen_write_ctx *ctx, u_int bg)
{
	struct window		*w = wp->window;
	struct kitty_image_map	*map;
	struct image		*src, *im;

	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return ("ENOENT: image not found");
	cmd->image_id = map->kitty_id;
	src = image_find(map->image_id);
	image_ref(src);

	im = kitty_place_image(src, cmd, w->xpixel, w->ypixel);
	if (im == NULL) {
		image_free(src);
		return ("EINVAL: invalid placement");
	}
	kitty_display(ctx, cmd, map, im, bg);
	image_free(im);
	image_free(src);
	return ("OK");
}

/* Store a completed upload, and place it for a transmit and display. */
static const char *
kitty_handle_transmit(struct kitty_state *ks, struct kitty_command *cmd,
    struct kitty_data *payload, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg)
{
	struct window		*w = wp->window;
	struct image_geom	 ig = { 0 };
	struct kitty_image_map	*map = NULL;
	struct image		*src, *im = NULL;
	const char		*m;
	u_char			*p;
	u_int			 sx, sy;
	uint64_t		 cw, ch;

	p = kitty_load(cmd, wp, payload, &m);
	if (p == NULL)
		return (m);

	cw = (w->xpixel == 0 ? 8 : w->xpixel);
	ch = (w->ypixel == 0 ? 16 : w->ypixel);
	image_size_in_cells(cmd->pixel_width, cmd->pixel_height, cw, ch, &sx,
	    &sy);
	cw *= sx;
	ch *= sy;
	if (cw > UINT_MAX || ch > UINT_MAX || sx > USHRT_MAX ||
	    sy > USHRT_MAX) {
		free(p);
		return ("EINVAL: dimensions too large");
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
		return ("ENOMEM: out of memory");
	}
	image_set_transient(src, cmd->hints & KITTY_HINT_TRANSIENT);

	if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
		im = kitty_place_image(src, cmd, w->xpixel, w->ypixel);
		if (im == NULL) {
			image_free(src);
			return ("EINVAL: invalid placement");
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

/* Check if a key and value in a feature query are supported. */
static int
kitty_feature_supported(char action, char key, const char *value)
{
	const char	*keys;

	switch (action) {
	case KITTY_ACTION_QUERY:
	case KITTY_ACTION_TRANSMIT:
		keys = "ftosvSOpN";
		break;
	case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
		keys = "ftosvSOpNxywhXYcrCz";
		break;
	case KITTY_ACTION_PLACE:
		keys = "pxywhXYcrCz";
		break;
	case KITTY_ACTION_DELETE:
		keys = "dpxyz";
		break;
	default:
		return (0);
	}
	if (strchr(keys, key) == NULL)
		return (0);

	switch (key) {
	case 'f':
		return (strcmp(value, "8") == 0 ||
		    strcmp(value, "16") == 0 ||
		    strcmp(value, "24") == 0 ||
		    strcmp(value, "100") == 0);
	case 't':
		return (strchr("fts", value[0]) != NULL && value[1] == '\0');
	case 'd':
		return (strchr("AiInNcCpPqQrRxXyYzZ", value[0]) != NULL &&
		    value[1] == '\0');
	case 'C':
		return (strcmp(value, "1") == 0);
	case 'N':
		return (strcmp(value, "1") == 0);
	}
	return (1);
}

/* Report the keys supported for an action in a feature query. */
static void
kitty_handle_feature_query(struct input_ctx *ictx, struct kitty_command *cmd,
    const u_char *data, size_t size)
{
	char	*copy, *ptr, *next, *value, *extra = NULL, *tmp;
	char	 key;

	if (cmd->feature == KITTY_ACTION_QUERY ||
	    cmd->feature == KITTY_ACTION_TRANSMIT ||
	    cmd->feature == KITTY_ACTION_TRANSMIT_AND_DISPLAY ||
	    cmd->feature == KITTY_ACTION_PLACE ||
	    cmd->feature == KITTY_ACTION_DELETE)
		xasprintf(&extra, ",e=%c", cmd->feature);

	copy = ptr = xstrndup(data, size);
	while (extra != NULL && (next = strsep(&ptr, ",")) != NULL) {
		if (next[0] == '\0' || next[1] != '=')
			continue;
		key = next[0];
		value = next + 2;
		if (strchr("aiImqe", key) != NULL)
			continue;

		/* Default values are not reported. */
		if (strspn(value, "0") == strlen(value))
			continue;
		if ((key == 'f' && strcmp(value, "32") == 0) ||
		    (key == 't' && strcmp(value, "d") == 0) ||
		    (key == 'd' && strcmp(value, "a") == 0))
			continue;

		if (!kitty_feature_supported(cmd->feature, key, value))
			continue;
		xasprintf(&tmp, "%s,%c=%s", extra, key, value);
		free(extra);
		extra = tmp;
	}
	free(copy);

	kitty_send_reply(ictx, cmd, extra, "OK");
	free(extra);
}

/* Check an upload without storing it, or report supported features. */
static void
kitty_handle_query(struct input_ctx *ictx, struct kitty_command *cmd,
    struct window_pane *wp, const u_char *control, size_t csize,
    const u_char *data, size_t size)
{
	struct kitty_data	 payload = { 0 };
	const char		*m;
	u_char			*p;

	if (cmd->image_id == 0) {
		kitty_send_reply(ictx, cmd, NULL, "EINVAL: image ID required");
		return;
	}
	if (cmd->feature != '\0') {
		kitty_handle_feature_query(ictx, cmd, control, csize);
		return;
	}

	switch (kitty_append_data(&payload, data, size)) {
	case -1:
		return;
	case -2:
		m = "EINVAL: invalid data";
		break;
	default:
		p = kitty_load(cmd, wp, &payload, &m);
		if (p != NULL) {
			free(p);
			m = "OK";
		}
		break;
	}
	kitty_data_free(&payload);
	kitty_send_reply(ictx, cmd, NULL, m);
}

/* Add a chunk to an unfinished upload, and store it when it is complete. */
static void
kitty_continue_transfer(struct input_ctx *ictx, struct kitty_state *ks,
    struct kitty_command *next, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, const u_char *data, size_t size)
{
	struct kitty_command	 cmd;
	struct kitty_data	 payload;
	const char		*m;

	/* Continuation chunks only change the quiet level and m. */
	switch (kitty_append_data(&ks->load, data, size)) {
	case -1:
		return;
	case -2:
		if (next->quiet != 0)
			ks->cmd.quiet = next->quiet;
		kitty_send_reply(ictx, &ks->cmd, NULL, "EINVAL: invalid data");
		kitty_reset_transfer(ks);
		return;
	}
	if (next->quiet != 0)
		ks->cmd.quiet = next->quiet;
	if (next->more)
		return;

	cmd = ks->cmd;
	payload = ks->load;
	memset(&ks->load, 0, sizeof ks->load);
	ks->loading = 0;

	m = kitty_handle_transmit(ks, &cmd, &payload, wp, ctx, bg);
	kitty_data_free(&payload);
	kitty_send_reply(ictx, &cmd, NULL, m);
}

/* Start an upload, storing it if it is not sent in chunks. */
static void
kitty_start_transfer(struct input_ctx *ictx, struct kitty_state *ks,
    struct kitty_command *cmd, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, const u_char *data, size_t size)
{
	struct kitty_data	 payload = { 0 };
	const char		*m;

	if (kitty_format_bytes(cmd->format) == 0 &&
	    cmd->format != KITTY_FORMAT_PNG) {
		kitty_send_reply(ictx, cmd, NULL, "EINVAL: unsupported format");
		return;
	}

	switch (kitty_append_data(&payload, data, size)) {
	case -1:
		kitty_data_free(&payload);
		return;
	case -2:
		kitty_data_free(&payload);
		kitty_send_reply(ictx, cmd, NULL, "EINVAL: invalid data");
		return;
	}

	/* Only direct uploads may be sent in chunks. */
	if (cmd->transmission == 'd' && cmd->more) {
		ks->cmd = *cmd;
		ks->load = payload;
		ks->loading = 1;
		return;
	}

	m = kitty_handle_transmit(ks, cmd, &payload, wp, ctx, bg);
	kitty_data_free(&payload);
	kitty_send_reply(ictx, cmd, NULL, m);
}

/* Parse and reply to a Kitty graphics sequence. */
void
kitty_parse_input(struct input_ctx *ictx, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, void **state, const u_char *buf,
    size_t len)
{
	struct kitty_state	*ks = *state;
	struct kitty_command	 cmd;
	const u_char		*found, *data = NULL;
	const char		*m;
	size_t			 csize, size = 0;

	buf++;
	len--;

	if (ks == NULL) {
		ks = xcalloc(1, sizeof *ks);
		TAILQ_INIT(&ks->images);
		TAILQ_INSERT_TAIL(&kitty_states, ks, entry);
		*state = ks;
	}
	ks->alt = SCREEN_IS_ALTERNATE(ctx->s);

	found = memchr(buf, ';', len);
	if (found == NULL)
		csize = len;
	else {
		csize = found - buf;
		data = found + 1;
		size = len - csize - 1;
	}

	/* Malformed commands are ignored without a reply. */
	kitty_command_reset(&cmd);
	if (kitty_parse_control(&cmd, buf, csize, &m) != 0)
		return;

	/*
	 * While an upload is unfinished, direct transmissions continue it,
	 * other transmissions and deletes abandon it, and other actions leave
	 * it alone.
	 */
	if (ks->loading) {
		switch (cmd.action) {
		case KITTY_ACTION_TRANSMIT:
		case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
		case KITTY_ACTION_TRANSMIT_FRAME:
			if (cmd.transmission == 'd') {
				kitty_continue_transfer(ictx, ks, &cmd, wp, ctx,
				    bg, data, size);
				return;
			}
			/* FALLTHROUGH */
		case KITTY_ACTION_DELETE:
			kitty_reset_transfer(ks);
			break;
		}
	}

	if (cmd.image_id != 0 && cmd.image_number != 0) {
		kitty_send_reply(ictx, &cmd, NULL,
		    "EINVAL: image ID and number are mutually exclusive");
		return;
	}
	if (cmd.action == KITTY_ACTION_DELETE) {
		kitty_handle_delete(ks, &cmd, ctx);
		return;
	}
	if (m != NULL) {
		kitty_send_reply(ictx, &cmd, NULL, m);
		return;
	}

	switch (cmd.action) {
	case KITTY_ACTION_QUERY:
		kitty_handle_query(ictx, &cmd, wp, buf, csize, data, size);
		break;
	case KITTY_ACTION_TRANSMIT:
	case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
		kitty_start_transfer(ictx, ks, &cmd, wp, ctx, bg, data, size);
		break;
	case KITTY_ACTION_PLACE:
		m = kitty_handle_place(ks, &cmd, wp, ctx, bg);
		kitty_send_reply(ictx, &cmd, NULL, m);
		break;
	default:
		kitty_send_reply(ictx, &cmd, NULL, "ENOTSUP");
		break;
	}
}

/* Remove images and placements on the current screen, or on both screens. */
static void
kitty_free_screen(struct kitty_state *ks, struct screen_write_ctx *ctx,
    int both)
{
	struct kitty_image_map		*map, *map1;
	struct kitty_placement_map	*kpl;

	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		if (!both && map->alt != ks->alt)
			continue;
		TAILQ_FOREACH(kpl, &map->placements, entry)
			image_clear(ctx, kpl->image_id);
		kitty_image_map_free(ks, map);
	}
}

/* Free images without placements after the screen is cleared. */
void
kitty_clear_screen(void *state, struct screen_write_ctx *ctx)
{
	struct kitty_state	*ks = state;
	struct kitty_image_map	*map, *map1;

	if (ks == NULL)
		return;
	ks->alt = SCREEN_IS_ALTERNATE(ctx->s);
	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		if (map->alt != ks->alt)
			continue;
		kitty_prune_placements(map, ctx->s);
		if (TAILQ_EMPTY(&map->placements))
			kitty_image_map_free(ks, map);
	}
}

/* Free the alternate screen's images when it is entered or left. */
void
kitty_alternate_screen(void *state, struct screen_write_ctx *ctx)
{
	struct kitty_state	*ks = state;

	if (ks == NULL)
		return;
	ks->alt = 1;
	kitty_free_screen(ks, ctx, 0);
	ks->alt = SCREEN_IS_ALTERNATE(ctx->s);
}

/* Free all images and placements after a terminal reset. */
void
kitty_reset(void *state, struct screen_write_ctx *ctx)
{
	struct kitty_state	*ks = state;

	if (ks == NULL)
		return;
	kitty_reset_transfer(ks);
	kitty_free_screen(ks, ctx, 1);
}
