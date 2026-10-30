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
 * Each frame of an animated image is uploaded once as a gapless frame of the
 * same terminal image, and the terminal is told which frame to show when the
 * current frame changes, so playback timing stays with tmux.
 *
 * When a redraw is started, old rectangles overlapping the redraw area are
 * marked for deletion, then new rectangles are queued during redraw. At the
 * end, old rectangles are removed, new images uploaded and new placements
 * sent. If an upload would change the drawing order of overlapping images in
 * the same layer, all the affected images are deleted and uploaded again.
 *
 * When a redraw is started, old rectangles overlapping the redraw area are
 * marked for deletion, then new rectangles are queued during redraw. At the
 * end, old rectangles are removed, new images uploaded and new placements
 * sent. If an upload would change the drawing order of overlapping images in
 * the same layer, all the affected images are deleted and uploaded again.
 */

#define KITTY_CHUNK_SIZE 3072

/* Default time to show a new frame, in milliseconds. */
#define KITTY_FRAME_GAP 40

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
	int			 overwrite;	/* C for compose */
	int			 unicode;	/* U */
	u_int			 parent_image;	/* P */
	u_int			 parent_placement; /* Q */
	int32_t			 parent_xoff;	/* H */
	int32_t			 parent_yoff;	/* V */
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
	u_int				 frame_image;
	int				 alt;

	TAILQ_HEAD(, kitty_image_map)	 images;
	struct window_pane		*wp;
	TAILQ_ENTRY(kitty_state)	 entry;
};
TAILQ_HEAD(kitty_states, kitty_state);

/* Kitty input states holding uploaded images. */
static struct kitty_states kitty_states = TAILQ_HEAD_INITIALIZER(kitty_states);

/* A virtual placement fitted for one cell size. */
struct kitty_fit {
	u_int	xpixel;
	u_int	ypixel;
	u_int	image_id;
};

/* Map a Kitty placement ID to an image ID. */
struct kitty_placement_map {
	u_int					 kitty_id;
	u_int					 image_id;
	uint64_t				 serial;

	int					 flags;
#define KITTY_PLACEMENT_VIRTUAL 0x1
#define KITTY_PLACEMENT_RELATIVE 0x2
#define KITTY_PLACEMENT_ANCHORED 0x4

	/* Parent of a relative placement and offset from it in cells. */
	uint64_t				 parent;
	int32_t					 xoff;
	int32_t					 yoff;

	/* Last known origin in absolute grid coordinates. */
	int64_t					 x;
	int64_t					 y;

	/* Requested cells and fits for other cell sizes of a virtual one. */
	u_int					 cols;
	u_int					 rows;
	u_int					 xpixel;
	u_int					 ypixel;
	struct kitty_fit			*fits;
	u_int					 nfits;

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

/* A frame uploaded to a terminal, numbered from one in upload order. */
struct kitty_frame_cache {
	u_int	id;
	u_int	generation;
};

/* An image uploaded to a terminal. */
struct kitty_image_cache {
	u_int					image_id;
	u_int					kitty_id;
	u_int					order_id;
	int32_t					zindex;
	uint64_t				index;
	int					upload;
	int					 virtual;
	u_int					xpixel;
	u_int					ypixel;
	u_int					next_placement;
	TAILQ_HEAD(, kitty_placement_cache)	placements;

	struct kitty_frame_cache		*frames;
	u_int					 nframes;
	u_int					 frame;

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
	TAILQ_HEAD(, kitty_image_cache)		images;
	TAILQ_HEAD(, kitty_image_cache)		virtuals;
	u_int					next_id;
	int					changed;
};

/* Information decoded from one placeholder cell, -1 if not given. */
struct kitty_cell {
	u_int	low;
	u_int	placement;
	int	row;
	int	col;
	int	high;
};

/* Placeholder character, U+10EEEE, in UTF-8. */
#define KITTY_PLACEHOLDER "\364\216\273\256"

/* Longest chain of relative placements. */
#define KITTY_PARENT_LIMIT 8

/* Next placement serial number, used to refer to parent placements. */
static uint64_t kitty_next_serial;

/* Diacritics giving placeholder rows, columns and image ID high bytes. */
static const u_int kitty_diacritics[] = {
	0x0305, 0x030d, 0x030e, 0x0310, 0x0312, 0x033d, 0x033e, 0x033f,
	0x0346, 0x034a, 0x034b, 0x034c, 0x0350, 0x0351, 0x0352, 0x0357,
	0x035b, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
	0x036a, 0x036b, 0x036c, 0x036d, 0x036e, 0x036f, 0x0483, 0x0484,
	0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
	0x0598, 0x0599, 0x059c, 0x059d, 0x059e, 0x059f, 0x05a0, 0x05a1,
	0x05a8, 0x05a9, 0x05ab, 0x05ac, 0x05af, 0x05c4, 0x0610, 0x0611,
	0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
	0x0659, 0x065a, 0x065b, 0x065d, 0x065e, 0x06d6, 0x06d7, 0x06d8,
	0x06d9, 0x06da, 0x06db, 0x06dc, 0x06df, 0x06e0, 0x06e1, 0x06e2,
	0x06e4, 0x06e7, 0x06e8, 0x06eb, 0x06ec, 0x0730, 0x0732, 0x0733,
	0x0735, 0x0736, 0x073a, 0x073d, 0x073f, 0x0740, 0x0741, 0x0743,
	0x0745, 0x0747, 0x0749, 0x074a, 0x07eb, 0x07ec, 0x07ed, 0x07ee,
	0x07ef, 0x07f0, 0x07f1, 0x07f3, 0x0816, 0x0817, 0x0818, 0x0819,
	0x081b, 0x081c, 0x081d, 0x081e, 0x081f, 0x0820, 0x0821, 0x0822,
	0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082a, 0x082b, 0x082c,
	0x082d, 0x0951, 0x0953, 0x0954, 0x0f82, 0x0f83, 0x0f86, 0x0f87,
	0x135d, 0x135e, 0x135f, 0x17dd, 0x193a, 0x1a17, 0x1a75, 0x1a76,
	0x1a77, 0x1a78, 0x1a79, 0x1a7a, 0x1a7b, 0x1a7c, 0x1b6b, 0x1b6d,
	0x1b6e, 0x1b6f, 0x1b70, 0x1b71, 0x1b72, 0x1b73, 0x1cd0, 0x1cd1,
	0x1cd2, 0x1cda, 0x1cdb, 0x1ce0, 0x1dc0, 0x1dc1, 0x1dc3, 0x1dc4,
	0x1dc5, 0x1dc6, 0x1dc7, 0x1dc8, 0x1dc9, 0x1dcb, 0x1dcc, 0x1dd1,
	0x1dd2, 0x1dd3, 0x1dd4, 0x1dd5, 0x1dd6, 0x1dd7, 0x1dd8, 0x1dd9,
	0x1dda, 0x1ddb, 0x1ddc, 0x1ddd, 0x1dde, 0x1ddf, 0x1de0, 0x1de1,
	0x1de2, 0x1de3, 0x1de4, 0x1de5, 0x1de6, 0x1dfe, 0x20d0, 0x20d1,
	0x20d4, 0x20d5, 0x20d6, 0x20d7, 0x20db, 0x20dc, 0x20e1, 0x20e7,
	0x20e9, 0x20f0, 0x2cef, 0x2cf0, 0x2cf1, 0x2de0, 0x2de1, 0x2de2,
	0x2de3, 0x2de4, 0x2de5, 0x2de6, 0x2de7, 0x2de8, 0x2de9, 0x2dea,
	0x2deb, 0x2dec, 0x2ded, 0x2dee, 0x2def, 0x2df0, 0x2df1, 0x2df2,
	0x2df3, 0x2df4, 0x2df5, 0x2df6, 0x2df7, 0x2df8, 0x2df9, 0x2dfa,
	0x2dfb, 0x2dfc, 0x2dfd, 0x2dfe, 0x2dff, 0xa66f, 0xa67c, 0xa67d,
	0xa6f0, 0xa6f1, 0xa8e0, 0xa8e1, 0xa8e2, 0xa8e3, 0xa8e4, 0xa8e5,
	0xa8e6, 0xa8e7, 0xa8e8, 0xa8e9, 0xa8ea, 0xa8eb, 0xa8ec, 0xa8ed,
	0xa8ee, 0xa8ef, 0xa8f0, 0xa8f1, 0xaab0, 0xaab2, 0xaab3, 0xaab7,
	0xaab8, 0xaabe, 0xaabf, 0xaac1, 0xfe20, 0xfe21, 0xfe22, 0xfe23,
	0xfe24, 0xfe25, 0xfe26, 0x10a0f, 0x10a38, 0x1d185, 0x1d186, 0x1d187,
	0x1d188, 0x1d189, 0x1d1aa, 0x1d1ab, 0x1d1ac, 0x1d1ad, 0x1d242, 0x1d243,
	0x1d244,
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
		TAILQ_INIT(&ko->virtuals);
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
	free(entry->frames);
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
		TAILQ_FOREACH_SAFE(entry, &ko->virtuals, entry, next) {
			TAILQ_REMOVE(&ko->virtuals, entry, entry);
			kitty_free_entry(tty, entry, send);
		}
		free(ko);
		tty->image_data = NULL;
	}
}

/*
 * Delete everything which may be left on the terminal after output was
 * discarded, so the following redraw sends it again. A sequence cut by the
 * discard is ended first, along with any image load it left unfinished.
 */
void
kitty_discard_output(struct tty *tty)
{
	tty_puts(tty, "\033\\\033_Gq=2,m=0;\033\\\033_Ga=d,d=A,q=2\033\\");
	kitty_free_output_state(tty, 1);
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
		TAILQ_FOREACH_SAFE(entry, &ko->virtuals, entry, next) {
			if (image_find(entry->image_id) == NULL) {
				TAILQ_REMOVE(&ko->virtuals, entry, entry);
				kitty_free_entry(tty, entry, 1);
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

/*
 * Send a frame scaled for this terminal's cell pixel size, with the given
 * action and extra control data on the first chunk. Canvases for virtual
 * placements have no border.
 */
static int
kitty_send_frame(struct tty *tty, struct kitty_image_cache *entry,
    struct image *im, u_int n, char action, u_int id, const char *extra)
{
	struct image_geom	*ig = image_get_geometry(im);
	char			 s[128], e[4097];
	u_char			*p, *row, *p0;
	size_t			 offset, size, total, limit, stride;
	u_int			 x, y, w, h, b = !entry->virtual;
	u_int			 cw = entry->xpixel, ch = entry->ypixel;
	uint64_t		 width = (uint64_t)ig->sx * cw;
	uint64_t		 height = (uint64_t)ig->sy * ch;
	int			 elen;

	/* Check the output size and allow for the border. */
	limit = options_get_number(global_options, "image-size-limit");
	if (width > UINT_MAX - 2 ||
	    height > UINT_MAX - 2 ||
	    width * height > limit / 4)
		return (0);
	w = width + 2 * b;
	h = height + 2 * b;
	if ((uint64_t)w * h > SIZE_MAX / 4)
		return (0);
	stride = (size_t)w * 4;
	total = stride * h;

	/* Allocate and scale the output pixels. */
	p = xmalloc(total);
	for (y = 0; y < height; y++) {
		row = p + (y + b) * stride;
		for (x = 0; x < width; x++) {
			p0 = image_get_frame_pixel(im, n, x, y, width, height);
			memcpy(row + (x + b) * 4, p0, 4);
		}

		/* Fill the side borders with copies of the edge pixels. */
		if (b) {
			memcpy(row, row + 4, 4);
			memcpy(row + stride - 4, row + stride - 8, 4);
		}
	}
	if (b) {
		memcpy(p, p + stride, stride);
		memcpy(p + (h - 1) * stride, p + (h - 2) * stride, stride);
	}

	/*
	 * Encode and send the pixels in base64 chunks. The terminal must get
	 * every frame recorded as sent, so the output may not be discarded.
	 */
	tty->flags |= TTY_NOBLOCK;
	for (offset = 0; offset < total; offset += size) {
		size = total - offset;
		if (size > KITTY_CHUNK_SIZE)
			size = KITTY_CHUNK_SIZE;
		elen = b64_ntop(p + offset, size, e, sizeof e);
		if (elen < 0)
			fatalx("base64 encode failed");
		if (offset == 0) {
			xsnprintf(s, sizeof s,
			    "\033_Ga=%c,f=32,s=%u,v=%u,i=%u%s,q=2,m=%d;",
			    action, w, h, id, extra, offset + size < total);
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

/* Upload the current frame as a new terminal image. */
static int
kitty_upload(struct tty *tty, struct kitty_image_cache *entry)
{
	struct kitty_output	*ko = tty->image_data;
	struct image		*im = image_find(entry->image_id);
	u_int			 n = image_get_frame(im), id;

	do
		id = ++ko->next_id;
	while (id == 0);
	if (!kitty_send_frame(tty, entry, im, n, 't', id, ""))
		return (0);
	entry->upload = 0;
	entry->kitty_id = id;

	entry->frames = xreallocarray(entry->frames, 1, sizeof *entry->frames);
	entry->frames[0].id = image_get_frame_id(im, n);
	entry->frames[0].generation = image_get_frame_generation(im, n);
	entry->nframes = 1;
	entry->frame = 1;
	return (1);
}

/* Show the current frame of an uploaded image, uploading it if needed. */
static void
kitty_show_frame(struct tty *tty, struct kitty_image_cache *entry)
{
	struct image			*im = image_find(entry->image_id);
	struct kitty_frame_cache	*kf;
	char				 control[64];
	u_int				 n, id, generation, i;

	if (im == NULL || entry->kitty_id == 0 || entry->upload)
		return;
	n = image_get_frame(im);
	id = image_get_frame_id(im, n);
	generation = image_get_frame_generation(im, n);

	for (i = 0; i < entry->nframes; i++) {
		if (entry->frames[i].id == id)
			break;
	}
	if (i == entry->nframes) {
		/* A gapless frame never advances by itself. */
		if (!kitty_send_frame(tty, entry, im, n, 'f', entry->kitty_id,
		    ",X=1,z=-1"))
			return;
		entry->frames = xreallocarray(entry->frames, i + 1,
		    sizeof *entry->frames);
		kf = &entry->frames[i];
		kf->id = id;
		kf->generation = generation;
		entry->nframes++;
	} else if (entry->frames[i].generation != generation) {
		xsnprintf(control, sizeof control, ",r=%u,X=1", i + 1);
		if (!kitty_send_frame(tty, entry, im, n, 'f', entry->kitty_id,
		    control))
			return;
		entry->frames[i].generation = generation;
	}

	if (entry->frame != i + 1) {
		xsnprintf(control, sizeof control,
		    "\033_Ga=a,i=%u,c=%u,q=2\033\\", entry->kitty_id, i + 1);
		tty_puts(tty, control);
		entry->frame = i + 1;
	}
}

/* Show the current frames of all uploaded images. */
void
kitty_update_frames(struct tty *tty)
{
	struct kitty_output		*ko = tty->image_data;
	struct kitty_image_cache	*entry;

	if (ko == NULL || (tty->flags & TTY_BLOCK))
		return;
	TAILQ_FOREACH(entry, &ko->images, entry)
		kitty_show_frame(tty, entry);
	TAILQ_FOREACH(entry, &ko->virtuals, entry)
		kitty_show_frame(tty, entry);
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
		kitty_show_frame(tty, entry);
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

/*
 * Upload an image and create a virtual placement for its placeholders, or
 * bring an earlier upload up to date with the current frame.
 */
static struct kitty_image_cache *
kitty_get_virtual(struct tty *tty, struct image *im)
{
	struct kitty_output		*ko = kitty_get_output(tty);
	struct kitty_image_cache	*entry;
	struct image_geom		*ig = image_get_geometry(im);
	char				 s[128];

	TAILQ_FOREACH(entry, &ko->virtuals, entry) {
		if (entry->image_id == image_get_id(im)) {
			kitty_show_frame(tty, entry);
			return (entry);
		}
	}
	entry = xcalloc(1, sizeof *entry);
	entry->image_id = image_get_id(im);
	entry->virtual = 1;
	entry->upload = 1;
	entry->xpixel = tty->xpixel;
	if (entry->xpixel == 0)
		entry->xpixel = 8;
	entry->ypixel = tty->ypixel;
	if (entry->ypixel == 0)
		entry->ypixel = 16;
	TAILQ_INIT(&entry->placements);
	TAILQ_INSERT_TAIL(&ko->virtuals, entry, entry);

	if (kitty_upload(tty, entry)) {
		xsnprintf(s, sizeof s, "\033_Ga=p,U=1,i=%u,c=%u,r=%u,q=2\033\\",
		    entry->kitty_id, ig->sx, ig->sy);
		tty_puts(tty, s);
	}
	return (entry);
}

/* Append a codepoint to UTF-8 data. */
static void
kitty_append_utf8(struct utf8_data *ud, u_int cp)
{
	u_char	*p = ud->data + ud->size;

	if (cp < 0x800) {
		p[0] = 0xc0 | (cp >> 6);
		p[1] = 0x80 | (cp & 0x3f);
		ud->size += 2;
	} else if (cp < 0x10000) {
		p[0] = 0xe0 | (cp >> 12);
		p[1] = 0x80 | ((cp >> 6) & 0x3f);
		p[2] = 0x80 | (cp & 0x3f);
		ud->size += 3;
	} else {
		p[0] = 0xf0 | (cp >> 18);
		p[1] = 0x80 | ((cp >> 12) & 0x3f);
		p[2] = 0x80 | ((cp >> 6) & 0x3f);
		p[3] = 0x80 | (cp & 0x3f);
		ud->size += 4;
	}
}

/*
 * Rewrite a placeholder cell to show part of an image through a virtual
 * placement on the terminal, using the terminal's image ID.
 */
const struct grid_cell *
kitty_map_placeholder(struct tty *tty, struct image *im, u_int col, u_int row,
    const struct grid_cell *gc, struct grid_cell *out)
{
	struct kitty_image_cache	*entry;
	struct utf8_data		*ud = &out->data;
	u_int				 id, low, high;
	u_int				 n = nitems(kitty_diacritics);

	if (row >= n)
		return (NULL);
	entry = kitty_get_virtual(tty, im);
	if (entry->kitty_id == 0)
		return (NULL);
	id = entry->kitty_id;
	low = id & 0xffffff;
	high = id >> 24;

	if (out != gc)
		memcpy(out, gc, sizeof *out);
	out->flags &= ~GRID_FLAG_SELECTED;
	out->flags |= GRID_FLAG_NOPALETTE;
	out->attr = 0;
	out->link = 0;
	out->us = grid_default_cell.us;
	if (low < 256)
		out->fg = low|COLOUR_FLAG_256;
	else
		out->fg = colour_join_rgb(low >> 16, (low >> 8) & 0xff,
		    low & 0xff);

	/* Leave the column to the terminal if it cannot be given. */
	memcpy(ud->data, KITTY_PLACEHOLDER, 4);
	ud->size = 4;
	ud->have = 0;
	ud->width = 1;
	kitty_append_utf8(ud, kitty_diacritics[row]);
	if (col < n) {
		kitty_append_utf8(ud, kitty_diacritics[col]);
		if (high != 0)
			kitty_append_utf8(ud, kitty_diacritics[high]);
	}
	return (out);
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
		case 'H':
		case 'V':
			ll = strtonum(value, INT32_MIN, INT32_MAX, &errstr);
			if (errstr != NULL)
				error = 1;
			else if (key == 'H')
				cmd->parent_xoff = ll;
			else
				cmd->parent_yoff = ll;
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
		case 'U':
		case 'P':
		case 'Q':
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
				cmd->overwrite = (number != 0);
				break;
			case 'N':
				cmd->hints = number;
				break;
			case 'U':
				cmd->unicode = (number != 0);
				break;
			case 'P':
				cmd->parent_image = number;
				break;
			case 'Q':
				cmd->parent_placement = number;
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

/* Free a placement mapping and any image reference it holds. */
static void
kitty_placement_free(struct kitty_placement_map *kpl)
{
	struct image	*im;
	u_int		 i;

	if (kpl->flags & (KITTY_PLACEMENT_VIRTUAL|KITTY_PLACEMENT_RELATIVE)) {
		im = image_find(kpl->image_id);
		if (im != NULL)
			image_free(im);
	}
	for (i = 0; i < kpl->nfits; i++) {
		im = image_find(kpl->fits[i].image_id);
		if (im != NULL)
			image_free(im);
	}
	free(kpl->fits);
	free(kpl);
}

/* Free a Kitty image mapping and its placement views. */
static void
kitty_image_map_free(struct kitty_state *ks, struct kitty_image_map *map)
{
	struct kitty_placement_map	*kpl;

	TAILQ_REMOVE(&ks->images, map, entry);
	while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
		TAILQ_REMOVE(&map->placements, kpl, entry);
		kitty_placement_free(kpl);
	}
	image_free(image_find(map->image_id));
	free(map);
}

/* Remove a placement mapping and its image from every grid. */
static void
kitty_placement_remove(struct kitty_image_map *map,
    struct kitty_placement_map *kpl)
{
	struct image	*im = image_find(kpl->image_id);

	TAILQ_REMOVE(&map->placements, kpl, entry);
	if (im != NULL)
		image_ref(im);
	kitty_placement_free(kpl);
	if (im != NULL) {
		image_grid_remove_all(im);
		image_free(im);
	}
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
	u_int				 id = image_get_id(im), i;

	TAILQ_FOREACH(ks, &kitty_states, entry) {
		TAILQ_FOREACH_SAFE(map, &ks->images, entry, next) {
			TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
				for (i = 0; i < kpl->nfits; i++) {
					if (kpl->fits[i].image_id != id)
						continue;
					kpl->fits[i] = kpl->fits[--kpl->nfits];
					image_free(im);
					break;
				}
				if (map->image_id != id && kpl->image_id != id)
					continue;
				TAILQ_REMOVE(&map->placements, kpl, entry);
				kitty_placement_free(kpl);
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
			kitty_placement_free(kpl);
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

/*
 * Replace a named placement, or add a new anonymous placement. A replaced
 * placement keeps its serial number so relative placements stay attached.
 */
static struct kitty_placement_map *
kitty_placement_map_set(struct screen_write_ctx *ctx,
    struct kitty_image_map *map, u_int id, struct image *im, int flags)
{
	struct kitty_placement_map	*kpl, *kpl1;
	struct image			*old;
	u_int				 i;

	if (map == NULL)
		return (NULL);
	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (image_find(kpl->image_id) == NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			kitty_placement_free(kpl);
			continue;
		}
		if (id != 0 && kpl->kitty_id == id)
			break;
	}
	if (kpl == NULL) {
		kpl = xcalloc(1, sizeof *kpl);
		kpl->kitty_id = id;
		kpl->serial = ++kitty_next_serial;
		TAILQ_INSERT_TAIL(&map->placements, kpl, entry);
	} else {
		image_clear(ctx, kpl->image_id);
		if (kpl->flags & (KITTY_PLACEMENT_VIRTUAL|
		    KITTY_PLACEMENT_RELATIVE))
			image_free(image_find(kpl->image_id));
		for (i = 0; i < kpl->nfits; i++) {
			if ((old = image_find(kpl->fits[i].image_id)) != NULL)
				image_free(old);
		}
		kpl->nfits = 0;
	}
	kpl->image_id = image_get_id(im);
	kpl->flags = flags;
	if (flags & (KITTY_PLACEMENT_VIRTUAL|KITTY_PLACEMENT_RELATIVE))
		image_ref(im);
	return (kpl);
}

/* Remove mappings for absent placements. */
static u_int
kitty_prune_placements(struct kitty_image_map *map, struct screen *s)
{
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 removed = 0;
	struct grid			*sgd = s->saved_grid;

	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (kpl->flags & (KITTY_PLACEMENT_VIRTUAL|
		    KITTY_PLACEMENT_RELATIVE))
			continue;
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

/* Find the parser state for a pane. */
static struct kitty_state *
kitty_find_state(struct window_pane *wp)
{
	struct kitty_state	*ks;

	TAILQ_FOREACH(ks, &kitty_states, entry) {
		if (ks->wp == wp)
			return (ks);
	}
	return (NULL);
}

/* Find a placement mapping by serial number. */
static struct kitty_placement_map *
kitty_find_serial(struct kitty_state *ks, uint64_t serial)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if (kpl->serial == serial)
				return (kpl);
		}
	}
	return (NULL);
}

/* Check whether a placement has relative placements attached. */
static int
kitty_has_children(struct kitty_state *ks, struct kitty_placement_map *parent)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if ((kpl->flags & KITTY_PLACEMENT_RELATIVE) &&
			    kpl->parent == parent->serial)
				return (1);
		}
	}
	return (0);
}

/*
 * Pick a placement of an image: the given placement ID, otherwise the lowest
 * placement ID, otherwise the oldest placement without an ID.
 */
static struct kitty_placement_map *
kitty_placement_select(struct kitty_image_map *map, u_int id, int virtual)
{
	struct kitty_placement_map	*kpl, *best = NULL;

	TAILQ_FOREACH(kpl, &map->placements, entry) {
		if (virtual && (~kpl->flags & KITTY_PLACEMENT_VIRTUAL))
			continue;
		if (id != 0) {
			if (kpl->kitty_id == id)
				return (kpl);
			continue;
		}
		if (best == NULL)
			best = kpl;
		else if (kpl->kitty_id != 0 &&
		    (best->kitty_id == 0 || kpl->kitty_id < best->kitty_id))
			best = kpl;
	}
	return (best);
}

/* Check whether UTF-8 data is a Kitty placeholder cell. */
int
kitty_is_placeholder(const struct utf8_data *ud)
{
	return (ud->size >= 4 && memcmp(ud->data, KITTY_PLACEHOLDER, 4) == 0);
}

/* Get the value of a diacritic, or -1 if it is not one. */
static int
kitty_diacritic_value(u_int cp)
{
	u_int	lo = 0, hi = nitems(kitty_diacritics), mid;

	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (kitty_diacritics[mid] == cp)
			return (mid);
		if (kitty_diacritics[mid] < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (-1);
}

/* Read the next codepoint from combined UTF-8 data. */
static int
kitty_next_codepoint(const struct utf8_data *ud, u_int *offset, u_int *cp)
{
	const u_char	*p = ud->data + *offset;
	u_int		 left = ud->size - *offset, n, i;

	if (left == 0)
		return (0);
	if (*p < 0x80) {
		*cp = *p;
		n = 1;
	} else if ((*p & 0xe0) == 0xc0) {
		*cp = *p & 0x1f;
		n = 2;
	} else if ((*p & 0xf0) == 0xe0) {
		*cp = *p & 0x0f;
		n = 3;
	} else {
		*cp = *p & 0x07;
		n = 4;
	}
	if (n > left)
		return (0);
	for (i = 1; i < n; i++)
		*cp = (*cp << 6) | (p[i] & 0x3f);
	*offset += n;
	return (1);
}

/* Convert a cell colour to a placeholder image or placement ID. */
static u_int
kitty_colour_id(int c)
{
	u_char	r, g, b;

	if (c & COLOUR_FLAG_RGB) {
		colour_split_rgb(c, &r, &g, &b);
		return ((r << 16)|(g << 8)|b);
	}
	if (c & COLOUR_FLAG_256)
		return (c & 0xff);
	if (c >= 0 && c <= 7)
		return (c);
	if (c >= 90 && c <= 97)
		return (c - 90 + 8);
	return (0);
}

/* Decode a placeholder cell. Invalid diacritics count as not given. */
static int
kitty_cell_read(const struct grid_cell *gc, struct kitty_cell *kc)
{
	u_int	offset = 4, cp, i;
	int	values[3] = { -1, -1, -1 };

	if (!kitty_is_placeholder(&gc->data))
		return (0);
	for (i = 0; i < nitems(values); i++) {
		if (!kitty_next_codepoint(&gc->data, &offset, &cp))
			break;
		values[i] = kitty_diacritic_value(cp);
	}
	kc->low = kitty_colour_id(gc->fg);
	kc->placement = kitty_colour_id(gc->us);
	kc->row = values[0];
	kc->col = values[1];
	kc->high = values[2];
	if (kc->high > 255)
		kc->high = -1;
	return (1);
}

/*
 * Add a cell to a run of placeholder cells, filling in values it does not
 * give from the cell to its left. Returns the image ID, placement ID, row
 * and column the cell shows.
 */
static int
kitty_run_add(struct kitty_run *run, const struct grid_cell *gc,
    struct kitty_cell *out)
{
	struct kitty_cell	kc;

	if (!kitty_cell_read(gc, &kc)) {
		run->active = 0;
		return (0);
	}
	if (run->active &&
	    kc.low == run->low &&
	    kc.placement == run->placement &&
	    (kc.row == -1 || (u_int)kc.row == run->row) &&
	    (kc.col == -1 || (u_int)kc.col == run->col + run->width) &&
	    (kc.high == -1 || (u_int)kc.high == run->high))
		run->width++;
	else {
		run->active = 1;
		run->low = kc.low;
		run->placement = kc.placement;
		run->row = (kc.row == -1 ? 0 : kc.row);
		run->col = (kc.col == -1 ? 0 : kc.col);
		run->high = (kc.high == -1 ? 0 : kc.high);
		run->width = 1;
	}
	if (out != NULL) {
		out->low = run->low|(run->high << 24);
		out->placement = run->placement;
		out->row = run->row;
		out->col = run->col + run->width - 1;
	}
	return (1);
}

/* Decode a placeholder cell, using a run ending at the cell to its left. */
static int
kitty_run_get(struct kitty_run *run, struct grid *gd, u_int x, u_int y,
    const struct grid_cell *gc, struct kitty_cell *kc)
{
	struct grid_cell	start_gc;
	u_int			start;

	if (run->gd != gd || run->y != y || !run->active || run->x + 1 != x) {
		for (start = x; start > 0; start--) {
			grid_get_cell(gd, start - 1, y, &start_gc);
			if (!kitty_is_placeholder(&start_gc.data))
				break;
		}
		run->gd = gd;
		run->y = y;
		run->active = 0;
		for (; start < x; start++) {
			grid_get_cell(gd, start, y, &start_gc);
			kitty_run_add(run, &start_gc, NULL);
		}
	}
	run->x = x;
	return (kitty_run_add(run, gc, kc));
}

/* Find the virtual placement a decoded placeholder cell shows. */
static struct kitty_placement_map *
kitty_cell_target(struct kitty_state *ks, struct kitty_cell *kc,
    struct kitty_image_map **mapp)
{
	struct kitty_image_map	*map;

	if (kc->low == 0)
		return (NULL);
	map = kitty_image_map_find(ks, kc->low);
	if (map == NULL)
		return (NULL);
	if (mapp != NULL)
		*mapp = map;
	return (kitty_placement_select(map, kc->placement, 1));
}

static struct image	*kitty_virtual_image(struct image *, u_int, u_int,
    u_int, u_int);

/*
 * Get a virtual placement fitted for a cell size, creating it the first time
 * that size is used. A zero size gives the fit made when it was placed.
 */
static struct image *
kitty_virtual_fit(struct kitty_image_map *map, struct kitty_placement_map *kpl,
    u_int xpixel, u_int ypixel)
{
	struct image	*src, *im;
	u_int		 i;

	if (xpixel == 0 || ypixel == 0)
		return (image_find(kpl->image_id));
	if (xpixel == kpl->xpixel && ypixel == kpl->ypixel)
		return (image_find(kpl->image_id));
	for (i = 0; i < kpl->nfits; i++) {
		if (kpl->fits[i].xpixel == xpixel &&
		    kpl->fits[i].ypixel == ypixel)
			return (image_find(kpl->fits[i].image_id));
	}
	if ((src = image_find(map->image_id)) == NULL)
		return (NULL);
	im = kitty_virtual_image(src, kpl->cols, kpl->rows, xpixel, ypixel);
	if (im == NULL)
		return (NULL);
	kpl->fits = xreallocarray(kpl->fits, kpl->nfits + 1,
	    sizeof *kpl->fits);
	kpl->fits[kpl->nfits].xpixel = xpixel;
	kpl->fits[kpl->nfits].ypixel = ypixel;
	kpl->fits[kpl->nfits].image_id = image_get_id(im);
	kpl->nfits++;
	return (im);
}

/*
 * Find the image and the cell within it shown by a placeholder cell in a
 * pane's grid, fitted for a cell size. The run carries inferred values
 * between calls for adjacent cells and may be NULL.
 */
int
kitty_placeholder_get(struct window_pane *wp, struct kitty_run *run,
    struct grid *gd, u_int x, u_int y, const struct grid_cell *gc,
    u_int xpixel, u_int ypixel, struct image **im, u_int *col, u_int *row)
{
	struct kitty_state		*ks;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct kitty_run		 local = { 0 };
	struct kitty_cell		 kc;
	struct image_geom		*ig;

	if (wp->screen != &wp->base &&
	    window_pane_mode(wp) != WINDOW_PANE_COPY_MODE)
		return (0);
	if ((ks = kitty_find_state(wp)) == NULL)
		return (0);
	if (run == NULL)
		run = &local;
	if (!kitty_run_get(run, gd, x, y, gc, &kc))
		return (0);
	if ((kpl = kitty_cell_target(ks, &kc, &map)) == NULL)
		return (0);
	if ((*im = kitty_virtual_fit(map, kpl, xpixel, ypixel)) == NULL)
		return (0);
	ig = image_get_geometry(*im);
	if ((u_int)kc.col >= ig->sx || (u_int)kc.row >= ig->sy)
		return (0);
	*col = kc.col;
	*row = kc.row;
	return (1);
}

/* Get the cell where a placement starts, in absolute grid coordinates. */
static int
kitty_placement_origin(struct grid *gd, struct kitty_placement_map *kpl,
    int64_t *x, int64_t *y)
{
	struct image	*im;
	int		 ox, oy;

	if (~kpl->flags & KITTY_PLACEMENT_VIRTUAL) {
		im = image_find(kpl->image_id);
		if (im != NULL && image_grid_get_origin(gd, im, &ox, &oy)) {
			*x = ox;
			*y = oy;
			return (1);
		}
	}
	if (~kpl->flags & KITTY_PLACEMENT_ANCHORED)
		return (0);
	*x = kpl->x;
	*y = kpl->y;
	return (1);
}

/* Add an image to a grid at a position which may be partly outside it. */
static void
kitty_grid_put(struct grid *gd, struct image *im, int64_t x, int64_t y)
{
	struct image_geom	*ig = image_get_geometry(im);
	u_int			 src_x = 0, src_y = 0;

	if (x < 0) {
		if (-x >= ig->sx)
			return;
		src_x = -x;
		x = 0;
	}
	if (y < 0) {
		if (-y >= ig->sy)
			return;
		src_y = -y;
		y = 0;
	}
	if (x >= gd->sx || y >= gd->hsize + gd->sy)
		return;
	image_grid_add(gd, im, x, y, src_x, src_y, ig->sx - src_x,
	    ig->sy - src_y);
}

static void	kitty_place_children(struct kitty_state *, struct grid *,
    struct kitty_placement_map *, int64_t, int64_t);

/* Move a relative placement to its parent's origin plus its offset. */
static void
kitty_place_relative(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *kpl, int64_t px, int64_t py)
{
	struct image	*im = image_find(kpl->image_id);
	int64_t		 x = px + kpl->xoff, y = py + kpl->yoff;

	if (im != NULL) {
		image_grid_clear(gd, kpl->image_id);
		kitty_grid_put(gd, im, x, y);
	}
	kpl->flags |= KITTY_PLACEMENT_ANCHORED;
	kpl->x = x;
	kpl->y = y;
	kitty_place_children(ks, gd, kpl, x, y);
}

/* Move the relative placements attached to a placement. */
static void
kitty_place_children(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *parent, int64_t x, int64_t y)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if ((kpl->flags & KITTY_PLACEMENT_RELATIVE) &&
			    kpl->parent == parent->serial)
				kitty_place_relative(ks, gd, kpl, x, y);
		}
	}
}

/* Move relative placements after their parent has been placed. */
static void
kitty_update_children(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *parent)
{
	int64_t	x, y;

	if (kitty_placement_origin(gd, parent, &x, &y))
		kitty_place_children(ks, gd, parent, x, y);
}

/*
 * Remove relative placements whose parent has gone, and their images if they
 * are released and no longer placed.
 */
static int
kitty_remove_orphans(struct kitty_state *ks, int release)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	int				 removed = 0;

again:
	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if (~kpl->flags & KITTY_PLACEMENT_RELATIVE)
				continue;
			if (kitty_find_serial(ks, kpl->parent) != NULL)
				continue;
			kitty_placement_remove(map, kpl);
			removed = 1;
			if (release && TAILQ_EMPTY(&map->placements))
				kitty_image_map_free(ks, map);
			goto again;
		}
	}
	return (removed);
}

/* Check whether any relative placement of a parent has left the grid. */
static int
kitty_children_missing(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *parent)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if ((kpl->flags & KITTY_PLACEMENT_RELATIVE) &&
			    kpl->parent == parent->serial &&
			    !image_grid_has_image(gd, kpl->image_id))
				return (1);
		}
	}
	return (0);
}

/* Remove the relative placements below a parent from the grid. */
static int
kitty_hide_children(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *parent)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	int				 hidden = 0;

	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if ((~kpl->flags & KITTY_PLACEMENT_RELATIVE) ||
			    kpl->parent != parent->serial)
				continue;
			if (image_grid_clear(gd, kpl->image_id) != 0)
				hidden = 1;
			kpl->flags &= ~KITTY_PLACEMENT_ANCHORED;
			if (kitty_hide_children(ks, gd, kpl))
				hidden = 1;
		}
	}
	return (hidden);
}

/*
 * Anchor relative placements of a virtual placement at the top-left of the
 * visible placeholder cells showing it. If there are none, the relative
 * placements are hidden until there are.
 */
static int
kitty_anchor_virtual(struct kitty_state *ks, struct grid *gd,
    struct kitty_placement_map *kpl)
{
	struct grid_line	*gl;
	struct grid_cell	 gc;
	struct kitty_run	 run;
	struct kitty_cell	 kc;
	u_int			 x, y, min_x = 0, min_y = 0;
	int			 found = 0;

	for (y = gd->hsize; y < gd->hsize + gd->sy; y++) {
		gl = grid_get_line(gd, y);
		if (~gl->flags & GRID_LINE_EXTENDED)
			continue;
		memset(&run, 0, sizeof run);
		for (x = 0; x < gl->cellsize; x++) {
			grid_get_cell(gd, x, y, &gc);
			if (!kitty_run_add(&run, &gc, &kc))
				continue;
			if (kitty_cell_target(ks, &kc, NULL) != kpl)
				continue;
			if (!found || x < min_x)
				min_x = x;
			if (!found)
				min_y = y;
			found = 1;
		}
	}
	if (!found) {
		kpl->flags &= ~KITTY_PLACEMENT_ANCHORED;
		return (kitty_hide_children(ks, gd, kpl));
	}
	if ((kpl->flags & KITTY_PLACEMENT_ANCHORED) &&
	    kpl->x == min_x &&
	    kpl->y == min_y &&
	    !kitty_children_missing(ks, gd, kpl))
		return (0);
	kpl->flags |= KITTY_PLACEMENT_ANCHORED;
	kpl->x = min_x;
	kpl->y = min_y;
	kitty_place_children(ks, gd, kpl, min_x, min_y);
	return (1);
}

/*
 * Update relative placements after pane output: remove those whose parent
 * has gone and move those of virtual placements to follow the placeholder
 * cells.
 */
void
kitty_update_placements(void *state, struct window_pane *wp)
{
	struct kitty_state		*ks = state;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	int				 relative = 0, changed = 0;

	if (ks == NULL)
		return;
	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if (kpl->flags & KITTY_PLACEMENT_RELATIVE)
				relative = 1;
		}
	}
	if (!relative)
		return;

	TAILQ_FOREACH(map, &ks->images, entry)
		kitty_prune_placements(map, &wp->base);
	changed = kitty_remove_orphans(ks, 0);
	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if ((~kpl->flags & KITTY_PLACEMENT_VIRTUAL) ||
			    !kitty_has_children(ks, kpl))
				continue;
			if (kitty_anchor_virtual(ks, wp->base.grid, kpl))
				changed = 1;
		}
	}
	if (changed)
		wp->flags |= PANE_REDRAW;
}

/* Placements a delete by area may remove. */
struct kitty_delete_match {
	struct kitty_state	*ks;
	int32_t			 zindex;
	int			 use_zindex;
};

/* Check whether a placement image may be removed by a delete by area. */
static int
kitty_match_area(struct image *im, void *arg)
{
	struct kitty_delete_match	*dm = arg;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	u_int				 id = image_get_id(im);

	if (dm->use_zindex && image_get_zindex(im) != dm->zindex)
		return (0);
	TAILQ_FOREACH(map, &dm->ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if (kpl->image_id == id &&
			    (kpl->flags & KITTY_PLACEMENT_RELATIVE))
				return (0);
		}
	}
	return (1);
}

/* Remove relative placements in a z-index layer. */
static void
kitty_delete_relative_zindex(struct kitty_state *ks, int32_t zindex)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct image			*im;

again:
	TAILQ_FOREACH(map, &ks->images, entry) {
		TAILQ_FOREACH(kpl, &map->placements, entry) {
			if (~kpl->flags & KITTY_PLACEMENT_RELATIVE)
				continue;
			im = image_find(kpl->image_id);
			if (im == NULL || image_get_zindex(im) != zindex)
				continue;
			kitty_placement_remove(map, kpl);
			goto again;
		}
	}
}

/*
 * Remove placements intersecting cells selected by a delete command. Virtual
 * placements have no cells, and relative placements are only removed with
 * their parent or by z-index.
 */
static void
kitty_delete_area(struct kitty_state *ks, struct kitty_command *cmd,
    struct screen *s, char how)
{
	struct grid			*gd = s->grid;
	u_int				 hsize = gd->hsize;
	struct kitty_delete_match	 dm = { ks, cmd->zindex, 0 };

	switch (how) {
	case 'a':
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0, hsize, gd->sx,
		    gd->sy, kitty_match_area, &dm);
		break;
	case 'c':
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, s->cx,
		    hsize + s->cy, 1, 1, kitty_match_area, &dm);
		break;
	case 'p':
	case 'q':
		if (cmd->src_x == 0 || cmd->src_y == 0 || cmd->src_y > gd->sy)
			break;
		dm.use_zindex = (how == 'q');
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, cmd->src_x - 1,
		    hsize + cmd->src_y - 1, 1, 1, kitty_match_area, &dm);
		break;
	case 'x':
		if (cmd->src_x == 0)
			break;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, cmd->src_x - 1,
		    hsize, 1, gd->sy, kitty_match_area, &dm);
		break;
	case 'y':
		if (cmd->src_y == 0 || cmd->src_y > gd->sy)
			break;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0,
		    hsize + cmd->src_y - 1, gd->sx, 1, kitty_match_area, &dm);
		break;
	case 'z':
		dm.use_zindex = 1;
		image_grid_clear_area(gd, IMAGE_TYPE_KITTY, 0, 0, gd->sx,
		    hsize + gd->sy, kitty_match_area, &dm);
		kitty_delete_relative_zindex(ks, cmd->zindex);
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

/* Delete a frame, or the whole image if it has only one and release is set. */
static void
kitty_delete_frame(struct kitty_state *ks, struct kitty_command *cmd,
    struct screen_write_ctx *ctx, int release)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct image			*im;
	u_int				 count, n;

	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return;
	im = image_find(map->image_id);
	count = image_get_frame_count(im);

	if (count == 1) {
		if (!release)
			return;
		TAILQ_FOREACH(kpl, &map->placements, entry)
			image_clear(ctx, kpl->image_id);
		kitty_image_map_free(ks, map);
		return;
	}

	/* Zero is the first frame and numbers past the end are the last. */
	n = cmd->rows;
	if (n > count)
		n = count;
	if (n == 0)
		n = 1;
	image_delete_frame(im, n - 1);
}

/* Delete selected Kitty placements. */
static void
kitty_handle_delete(struct kitty_state *ks, struct kitty_command *cmd,
    struct screen_write_ctx *ctx)
{
	struct screen			*s = ctx->s;
	struct grid			*gd = s->grid;
	struct kitty_image_map		*map, *map1, *newest = NULL;
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 removed, placement_id;
	char				 how = cmd->delete_action;
	int				 release;

	release = (how >= 'A' && how <= 'Z');
	if (release)
		how += 'a' - 'A';
	if (how == 'f') {
		kitty_delete_frame(ks, cmd, ctx, release);
		kitty_remove_orphans(ks, release);
		return;
	}
	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->alt == ks->alt)
			kitty_prune_placements(map, s);
	}
	kitty_delete_area(ks, cmd, s, how);
	if (cmd->image_number != 0)
		newest = kitty_image_map_find_number(ks, cmd->image_number);
	placement_id = (how == 'r' ? 0 : cmd->placement_id);

	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		if (map->alt != ks->alt)
			continue;
		removed = 0;
		if (kitty_delete_selects(cmd, map, newest, how)) {
			TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
				if (placement_id != 0 &&
				    kpl->kitty_id != placement_id)
					continue;
				if (kpl->flags & (KITTY_PLACEMENT_VIRTUAL|
				    KITTY_PLACEMENT_RELATIVE)) {
					kitty_placement_remove(map, kpl);
					removed++;
				} else
					removed += image_grid_clear(gd,
					    kpl->image_id);
			}
			if (placement_id == 0)
				removed++;
		}
		removed += kitty_prune_placements(map, s);
		if (!release || removed == 0 || !TAILQ_EMPTY(&map->placements))
			continue;
		kitty_image_map_free(ks, map);
	}
	kitty_remove_orphans(ks, release);
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
	size_t			 limit;
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
	im = image_create_with_parent(type, src, &ig);
	if (im != NULL) {
		image_set_order_id(im, image_get_order_id(src));
		image_set_zindex(im, cmd->zindex);
	}
	return (im);
}

/*
 * Create a virtual placement for placeholder cells. The whole image is fitted
 * to the cells, keeping its aspect ratio and centred on the shorter axis.
 */
static struct image *
kitty_virtual_image(struct image *src, u_int cols, u_int rows, u_int xpixel,
    u_int ypixel)
{
	struct image_geom	*sg = image_get_geometry(src);
	struct image_geom	 ig = { 0 };
	struct image		*im;
	uint64_t		 cw, ch;
	double			 scale;

	if (xpixel == 0)
		xpixel = 8;
	if (ypixel == 0)
		ypixel = 16;
	ig.src_w = sg->src_w;
	ig.src_h = sg->src_h;

	/* Use the requested cells or enough for the image's own size. */
	ig.sx = cols;
	if (ig.sx == 0)
		ig.sx = ig.src_w / xpixel + (ig.src_w % xpixel != 0);
	ig.sy = rows;
	if (ig.sy == 0)
		ig.sy = ig.src_h / ypixel + (ig.src_h % ypixel != 0);
	if (ig.sx > USHRT_MAX || ig.sy > USHRT_MAX)
		return (NULL);
	cw = (uint64_t)ig.sx * xpixel;
	ch = (uint64_t)ig.sy * ypixel;
	if (cw > UINT_MAX || ch > UINT_MAX)
		return (NULL);
	ig.canvas_width = cw;
	ig.canvas_height = ch;

	if ((double)ig.src_w * ch > (double)ig.src_h * cw)
		scale = (double)cw / ig.src_w;
	else
		scale = (double)ch / ig.src_h;
	ig.scaled_width = ig.src_w * scale;
	ig.scaled_height = ig.src_h * scale;
	ig.canvas_xoff = (cw - ig.scaled_width) / 2 + 0.5;
	ig.canvas_yoff = (ch - ig.scaled_height) / 2 + 0.5;

	im = image_create_with_parent(image_get_type(src), src, &ig);
	if (im != NULL)
		image_set_order_id(im, image_get_order_id(src));
	return (im);
}

/* Place a relative placement after checking its parent. */
static const char *
kitty_display_relative(struct kitty_state *ks, struct window_pane *wp,
    struct screen_write_ctx *ctx, struct kitty_command *cmd,
    struct kitty_image_map *map, struct image *im)
{
	struct kitty_image_map		*pmap;
	struct kitty_placement_map	*parent, *kpl;
	struct screen			*s = ctx->s;
	u_int				 depth = 1;
	int64_t				 x, y;

	pmap = kitty_image_map_find(ks, cmd->parent_image);
	if (pmap == NULL)
		return ("ENOPARENT: parent image not found");
	kitty_prune_placements(pmap, s);
	parent = kitty_placement_select(pmap, cmd->parent_placement, 0);
	if (parent == NULL)
		return ("ENOPARENT: parent placement not found");
	if (map != NULL && pmap == map && cmd->placement_id != 0 &&
	    parent->kitty_id == cmd->placement_id)
		return ("EINVAL: placement cannot be its own parent");

	/* Walk up the chain, which may not include the new placement. */
	for (kpl = parent;; depth++) {
		if (map != NULL &&
		    cmd->placement_id != 0 &&
		    kpl->kitty_id == cmd->placement_id &&
		    kitty_placement_select(map, cmd->placement_id, 0) == kpl)
			return ("ECYCLE: parent chain creates a cycle");
		if (~kpl->flags & KITTY_PLACEMENT_RELATIVE)
			break;
		if (depth >= KITTY_PARENT_LIMIT)
			return ("ETOODEEP: parent chain too deep");
		if ((kpl = kitty_find_serial(ks, kpl->parent)) == NULL)
			return ("ENOENT: parent chain ancestor not found");
	}

	kpl = kitty_placement_map_set(ctx, map, cmd->placement_id, im,
	    KITTY_PLACEMENT_RELATIVE);
	if (kpl == NULL) {
		if (kitty_placement_origin(s->grid, parent, &x, &y)) {
			kitty_grid_put(s->grid, im, x + cmd->parent_xoff,
			    y + cmd->parent_yoff);
		}
	} else {
		kpl->parent = parent->serial;
		kpl->xoff = cmd->parent_xoff;
		kpl->yoff = cmd->parent_yoff;
		if (kitty_placement_origin(s->grid, parent, &x, &y))
			kitty_place_relative(ks, s->grid, kpl, x, y);
	}
	wp->flags |= PANE_REDRAW;
	return ("OK");
}

/* Display a placement and record its placement ID. */
static const char *
kitty_display(struct kitty_state *ks, struct window_pane *wp,
    struct screen_write_ctx *ctx, struct kitty_command *cmd,
    struct kitty_image_map *map, struct image *im, u_int bg)
{
	struct kitty_placement_map	*kpl;

	if (cmd->unicode) {
		kpl = kitty_placement_map_set(ctx, map, cmd->placement_id, im,
		    KITTY_PLACEMENT_VIRTUAL);
		if (kpl != NULL) {
			kpl->cols = cmd->cols;
			kpl->rows = cmd->rows;
			kpl->xpixel = wp->window->xpixel;
			if (kpl->xpixel == 0)
				kpl->xpixel = 8;
			kpl->ypixel = wp->window->ypixel;
			if (kpl->ypixel == 0)
				kpl->ypixel = 16;
		}
		wp->flags |= PANE_REDRAW;
		return ("OK");
	}
	if (cmd->parent_image != 0)
		return (kitty_display_relative(ks, wp, ctx, cmd, map, im));
	kpl = kitty_placement_map_set(ctx, map, cmd->placement_id, im, 0);
	image_write(ctx, im, bg, cmd->cursor_no_move);
	if (kpl != NULL)
		kitty_update_children(ks, ctx->s->grid, kpl);
	return ("OK");
}

/* Place an image which was uploaded earlier. */
static const char *
kitty_handle_place(struct kitty_state *ks, struct kitty_command *cmd,
    struct window_pane *wp, struct screen_write_ctx *ctx, u_int bg)
{
	struct window		*w = wp->window;
	struct kitty_image_map	*map;
	struct image		*src, *im;

	const char		*m;

	if (cmd->unicode && cmd->parent_image != 0)
		return ("EINVAL: virtual placement cannot refer to a parent");
	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return ("ENOENT: image not found");
	cmd->image_id = map->kitty_id;
	src = image_find(map->image_id);
	image_ref(src);

	if (cmd->unicode)
		im = kitty_virtual_image(src, cmd->cols, cmd->rows, w->xpixel,
		    w->ypixel);
	else
		im = kitty_place_image(src, cmd, w->xpixel, w->ypixel);
	if (im == NULL) {
		image_free(src);
		return ("EINVAL: invalid placement");
	}
	m = kitty_display(ks, wp, ctx, cmd, map, im, bg);
	image_free(im);
	image_free(src);
	return (m);
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

	/* The image is kept even if a virtual placement has a parent. */
	m = "OK";
	if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
		if (cmd->unicode && cmd->parent_image != 0)
			m = "EINVAL: virtual placement cannot refer to a parent";
		else {
			if (cmd->unicode) {
				im = kitty_virtual_image(src, cmd->cols,
				    cmd->rows, w->xpixel, w->ypixel);
			} else {
				im = kitty_place_image(src, cmd, w->xpixel,
				    w->ypixel);
			}
			if (im == NULL) {
				image_free(src);
				return ("EINVAL: invalid placement");
			}
		}
	}
	if (cmd->image_id == 0 && cmd->image_number != 0)
		cmd->image_id = kitty_image_next_id(ks);
	map = kitty_image_map_set(ks, ctx, cmd->image_id, cmd->image_number,
	    src);
	if (im != NULL) {
		image_set_order_id(im, image_get_order_id(src));
		m = kitty_display(ks, wp, ctx, cmd, map, im, bg);
		image_free(im);
	}
	image_free(src);
	return (m);
}

/* Fill a frame with a colour given as 0xRRGGBBAA. */
static void
kitty_fill_frame(u_char *pixels, size_t size, u_int colour)
{
	u_char	rgba[4];
	size_t	i;

	rgba[0] = colour >> 24;
	rgba[1] = (colour >> 16) & 0xff;
	rgba[2] = (colour >> 8) & 0xff;
	rgba[3] = colour & 0xff;
	for (i = 0; i + 4 <= size; i += 4)
		memcpy(pixels + i, rgba, 4);
}

/* Copy a row of RGBA pixels, or blend it over the existing pixels. */
static void
kitty_compose_row(u_char *dst, const u_char *src, u_int w, int replace)
{
	u_int	x, i, sa, da, oa;

	if (replace) {
		memcpy(dst, src, (size_t)w * 4);
		return;
	}
	for (x = 0; x < w; x++, dst += 4, src += 4) {
		sa = src[3];
		if (sa == 255) {
			memcpy(dst, src, 4);
			continue;
		}
		if (sa == 0)
			continue;

		/* Alpha values here are scaled by 255. */
		sa *= 255;
		da = dst[3] * (255 - src[3]);
		oa = sa + da;
		for (i = 0; i < 3; i++)
			dst[i] = (src[i] * sa + dst[i] * da + oa / 2) / oa;
		dst[3] = (oa + 127) / 255;
	}
}

/* Add a frame to an image or change an existing frame. */
static const char *
kitty_handle_frame(struct kitty_state *ks, struct kitty_command *cmd,
    struct kitty_data *payload, struct window_pane *wp)
{
	struct kitty_image_map	*map;
	struct image		*im;
	struct image_geom	*ig;
	u_char			*p, *frame;
	const char		*m;
	size_t			 size, stride;
	u_int			 count, n, w, h, y, gap;
	int			 replace = (cmd->cell_xoff == 1);

	/* The image must not have been replaced since the first chunk. */
	map = kitty_image_map_select(ks, cmd);
	if (map == NULL || map->image_id != ks->frame_image)
		return ("ENOENT: image not found");
	cmd->image_id = map->kitty_id;
	im = image_find(map->image_id);
	ig = image_get_geometry(im);

	p = kitty_load(cmd, wp, payload, &m);
	if (p == NULL)
		return (m);
	if (cmd->pixel_width > ig->src_w || cmd->pixel_height > ig->src_h) {
		free(p);
		return ("EINVAL: frame dimensions exceed image");
	}

	/* Frame numbers past the end add a new frame. */
	count = image_get_frame_count(im);
	n = cmd->rows;
	if (n == 0 || n > count)
		n = count + 1;
	cmd->rows = n;

	size = (size_t)ig->src_w * ig->src_h * 4;
	if (n > count) {
		if (cmd->cols > count) {
			free(p);
			return ("EINVAL: base frame not found");
		}
		frame = xmalloc(size);
		if (cmd->cols != 0) {
			memcpy(frame, image_get_frame_pixels(im, cmd->cols - 1),
			    size);
		} else
			kitty_fill_frame(frame, size, cmd->cell_yoff);
	} else
		frame = image_get_frame_pixels(im, n - 1);

	/* Data outside the image is clipped. */
	if (cmd->src_x < ig->src_w && cmd->src_y < ig->src_h) {
		w = cmd->pixel_width;
		if (w > ig->src_w - cmd->src_x)
			w = ig->src_w - cmd->src_x;
		h = cmd->pixel_height;
		if (h > ig->src_h - cmd->src_y)
			h = ig->src_h - cmd->src_y;
		stride = (size_t)ig->src_w * 4;
		for (y = 0; y < h; y++) {
			kitty_compose_row(frame + (cmd->src_y + y) * stride +
			    (size_t)cmd->src_x * 4,
			    p + (size_t)y * cmd->pixel_width * 4, w, replace);
		}
	}
	free(p);

	/* A zero gap means the default for a new frame and no change. */
	if (n > count) {
		if (cmd->zindex > 0)
			gap = cmd->zindex;
		else if (cmd->zindex < 0)
			gap = 0;
		else
			gap = KITTY_FRAME_GAP;
		if (image_add_frame(im, frame, gap) != 0) {
			free(frame);
			return ("ENOSPC: animation frame storage full");
		}
		return ("OK");
	}
	if (cmd->zindex != 0) {
		image_set_frame_gap(im, n - 1,
		    cmd->zindex > 0 ? cmd->zindex : 0);
	}
	image_frame_changed(im, n - 1);
	return ("OK");
}

/* Control playback of an image's frames. Invalid values are ignored. */
static const char *
kitty_handle_animation(struct kitty_state *ks, struct kitty_command *cmd)
{
	struct kitty_image_map	*map;
	struct image		*im;
	u_int			 count;

	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return ("ENOENT: image not found");
	im = image_find(map->image_id);
	count = image_get_frame_count(im);

	if (cmd->rows != 0 && cmd->rows <= count && cmd->zindex != 0) {
		image_set_frame_gap(im, cmd->rows - 1,
		    cmd->zindex > 0 ? cmd->zindex : 0);
	}
	if (cmd->cols != 0 && cmd->cols <= count)
		image_set_frame(im, cmd->cols - 1);
	switch (cmd->pixel_width) {
	case 1:
		image_set_animation(im, IMAGE_ANIMATION_STOPPED);
		break;
	case 2:
		image_set_animation(im, IMAGE_ANIMATION_LOADING);
		break;
	case 3:
		image_set_animation(im, IMAGE_ANIMATION_LOOPING);
		break;
	}
	if (cmd->pixel_height != 0)
		image_set_animation_loops(im, cmd->pixel_height - 1);
	return ("OK");
}

/* Copy or blend a rectangle from one frame to another. */
static const char *
kitty_handle_compose(struct kitty_state *ks, struct kitty_command *cmd)
{
	struct kitty_image_map	*map;
	struct image		*im;
	struct image_geom	*ig;
	u_char			*src, *dst;
	u_int			 count, w, h, y;
	u_int			 sx = cmd->cell_xoff, sy = cmd->cell_yoff;
	u_int			 dx = cmd->src_x, dy = cmd->src_y;
	size_t			 stride;

	map = kitty_image_map_select(ks, cmd);
	if (map == NULL)
		return ("ENOENT: image not found");
	cmd->image_id = map->kitty_id;
	im = image_find(map->image_id);
	ig = image_get_geometry(im);

	/* The source is frame r and the destination frame c. */
	count = image_get_frame_count(im);
	if (cmd->rows == 0 || cmd->rows > count)
		return ("ENOENT: source frame not found");
	if (cmd->cols == 0 || cmd->cols > count)
		return ("ENOENT: destination frame not found");

	/* Rectangles are not clipped. */
	w = (cmd->src_w != 0 ? cmd->src_w : ig->src_w);
	h = (cmd->src_h != 0 ? cmd->src_h : ig->src_h);
	if ((uint64_t)dx + w > ig->src_w || (uint64_t)dy + h > ig->src_h)
		return ("EINVAL: destination rectangle out of bounds");
	if ((uint64_t)sx + w > ig->src_w || (uint64_t)sy + h > ig->src_h)
		return ("EINVAL: source rectangle out of bounds");
	if (cmd->rows == cmd->cols &&
	    (sx > dx ? sx : dx) < (sx < dx ? sx : dx) + w &&
	    (sy > dy ? sy : dy) < (sy < dy ? sy : dy) + h)
		return ("EINVAL: source and destination rectangles overlap");

	src = image_get_frame_pixels(im, cmd->rows - 1);
	dst = image_get_frame_pixels(im, cmd->cols - 1);
	stride = (size_t)ig->src_w * 4;
	for (y = 0; y < h; y++) {
		kitty_compose_row(dst + (dy + y) * stride + (size_t)dx * 4,
		    src + (sy + y) * stride + (size_t)sx * 4, w,
		    cmd->overwrite);
	}
	image_frame_changed(im, cmd->cols - 1);
	return ("OK");
}

/* Reply to a frame command, with the frame number if there is one. */
static void
kitty_send_frame_reply(struct input_ctx *ictx, struct kitty_command *cmd,
    const char *m)
{
	char	extra[32] = "";

	if (cmd->rows != 0)
		xsnprintf(extra, sizeof extra, ",r=%u", cmd->rows);
	kitty_send_reply(ictx, cmd, extra, m);
}

/* Store a completed upload as an image or a frame and reply to it. */
static void
kitty_finish_transfer(struct input_ctx *ictx, struct kitty_state *ks,
    struct kitty_command *cmd, struct kitty_data *payload,
    struct window_pane *wp, struct screen_write_ctx *ctx, u_int bg)
{
	const char	*m;

	if (cmd->action == KITTY_ACTION_TRANSMIT_FRAME) {
		m = kitty_handle_frame(ks, cmd, payload, wp);
		kitty_send_frame_reply(ictx, cmd, m);
		return;
	}
	m = kitty_handle_transmit(ks, cmd, payload, wp, ctx, bg);
	kitty_send_reply(ictx, cmd, NULL, m);
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
		keys = "ftosvSOpNxywhXYcrCzUPQHV";
		break;
	case KITTY_ACTION_PLACE:
		keys = "pxywhXYcrCzUPQHV";
		break;
	case KITTY_ACTION_DELETE:
		keys = "dpxyzr";
		break;
	case KITTY_ACTION_TRANSMIT_FRAME:
		keys = "ftosvSOxycrzXY";
		break;
	case KITTY_ACTION_CONTROL_ANIMATION:
		keys = "srzcv";
		break;
	case KITTY_ACTION_COMPOSE_FRAME:
		keys = "rcxyXYwhC";
		break;
	default:
		return (0);
	}
	if (strchr(keys, key) == NULL)
		return (0);

	/* Animation keys with a fixed set of values. */
	switch (action) {
	case KITTY_ACTION_TRANSMIT_FRAME:
		if (key == 'f') {
			return (strcmp(value, "24") == 0 ||
			    strcmp(value, "100") == 0);
		}
		if (key == 'X')
			return (strcmp(value, "1") == 0);
		break;
	case KITTY_ACTION_CONTROL_ANIMATION:
		if (key == 's') {
			return (strchr("123", value[0]) != NULL &&
			    value[1] == '\0');
		}
		break;
	}

	switch (key) {
	case 'f':
		return (strcmp(value, "8") == 0 ||
		    strcmp(value, "16") == 0 ||
		    strcmp(value, "24") == 0 ||
		    strcmp(value, "100") == 0);
	case 't':
		return (strchr("fts", value[0]) != NULL && value[1] == '\0');
	case 'd':
		return (strchr("AiInNcCpPqQrRxXyYzZfF", value[0]) != NULL &&
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
	    cmd->feature == KITTY_ACTION_DELETE ||
	    cmd->feature == KITTY_ACTION_TRANSMIT_FRAME ||
	    cmd->feature == KITTY_ACTION_CONTROL_ANIMATION ||
	    cmd->feature == KITTY_ACTION_COMPOSE_FRAME)
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

	/* Continuation chunks only change the quiet level and m. */
	switch (kitty_append_data(&ks->load, data, size)) {
	case -1:
		return;
	case -2:
		if (next->quiet != 0)
			ks->cmd.quiet = next->quiet;
		if (ks->cmd.action == KITTY_ACTION_TRANSMIT_FRAME) {
			kitty_send_frame_reply(ictx, &ks->cmd,
			    "EINVAL: invalid data");
		} else {
			kitty_send_reply(ictx, &ks->cmd, NULL,
			    "EINVAL: invalid data");
		}
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

	kitty_finish_transfer(ictx, ks, &cmd, &payload, wp, ctx, bg);
	kitty_data_free(&payload);
}

/* Start an upload, storing it if it is not sent in chunks. */
static void
kitty_start_transfer(struct input_ctx *ictx, struct kitty_state *ks,
    struct kitty_command *cmd, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, const u_char *data, size_t size)
{
	struct kitty_data	 payload = { 0 };
	struct kitty_image_map	*map;
	const char		*m = NULL;

	if (kitty_format_bytes(cmd->format) == 0 &&
	    cmd->format != KITTY_FORMAT_PNG)
		m = "EINVAL: unsupported format";
	else if (cmd->action == KITTY_ACTION_TRANSMIT_FRAME) {
		/* Frames are full colour and the image must exist now. */
		map = kitty_image_map_select(ks, cmd);
		if (cmd->format == KITTY_FORMAT_INDEXED ||
		    cmd->format == KITTY_FORMAT_INDEXED_ALPHA)
			m = "EINVAL: unsupported format";
		else if (map == NULL)
			m = "ENOENT: image not found";
		else
			ks->frame_image = map->image_id;
	}
	if (m == NULL) {
		switch (kitty_append_data(&payload, data, size)) {
		case -1:
			kitty_data_free(&payload);
			return;
		case -2:
			m = "EINVAL: invalid data";
			break;
		}
	}
	if (m != NULL) {
		kitty_data_free(&payload);
		if (cmd->action == KITTY_ACTION_TRANSMIT_FRAME)
			kitty_send_frame_reply(ictx, cmd, m);
		else
			kitty_send_reply(ictx, cmd, NULL, m);
		return;
	}

	/* Only direct uploads may be sent in chunks. */
	if (cmd->transmission == 'd' && cmd->more) {
		ks->cmd = *cmd;
		ks->load = payload;
		ks->loading = 1;
		return;
	}

	kitty_finish_transfer(ictx, ks, cmd, &payload, wp, ctx, bg);
	kitty_data_free(&payload);
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
		ks->wp = wp;
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
	case KITTY_ACTION_TRANSMIT_FRAME:
		kitty_start_transfer(ictx, ks, &cmd, wp, ctx, bg, data, size);
		break;
	case KITTY_ACTION_CONTROL_ANIMATION:
		m = kitty_handle_animation(ks, &cmd);
		if (strcmp(m, "OK") != 0)
			kitty_send_reply(ictx, &cmd, NULL, m);
		break;
	case KITTY_ACTION_COMPOSE_FRAME:
		m = kitty_handle_compose(ks, &cmd);
		kitty_send_reply(ictx, &cmd, NULL, m);
		break;
	case KITTY_ACTION_PLACE:
		m = kitty_handle_place(ks, &cmd, wp, ctx, bg);
		kitty_send_reply(ictx, &cmd, NULL, m);
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
	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->alt == ks->alt)
			kitty_prune_placements(map, ctx->s);
	}
	kitty_remove_orphans(ks, 0);
	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		if (map->alt == ks->alt && TAILQ_EMPTY(&map->placements))
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
