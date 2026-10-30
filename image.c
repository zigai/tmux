/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 * Copyright (c) 2026 Michael Grant <mgrant@grant.org>
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

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Store decoded images and map their pixels for drawing.
 *
 * An image contains RGBA pixels and geometry describing which part of the
 * source image to use, its size in cells, and any scaling or padding. Child
 * images can share a parent's pixels with different geometry. Each image has a
 * unique ID.
 *
 * Scaling and offsets are recorded in the geometry. The drawing backends use
 * the helpers here to map output cells or pixels back to the source image.
 * This lets clients with different cell pixel sizes draw the same image
 * without changing its stored pixels.
 *
 * An image without a parent has one or more frames of pixels, of which one is
 * current. Child images always show the current frame of their parent. When
 * playback is running, a timer moves to the next frame while the image is
 * visible on a client.
 */

/* Minimum interval between redraws for a changed frame, in milliseconds. */
#define IMAGE_ANIMATION_REDRAW 30

/* A frame of pixels. Generations are unique and change when pixels change. */
struct image_frame {
	u_char			*pixels;
	u_int			 gap;
	u_int			 id;
	u_int			 generation;
};

/* An image. */
TAILQ_HEAD(image_list, image);
struct image {
	u_int			 id;
	enum image_type		 type;
	u_int			 references;
	struct image_geom	 geom;

	struct image		*parent;
	struct image_list	 children;
	u_int			 order_id;
	int32_t			 zindex;

	/*
	 * RGBA pixels use four bytes each. Stride is the number of bytes
	 * between the start of each row. Size is the number of bytes
	 * containing all of this image's pixels in one frame - it may include
	 * pixels that are part of the parent image but not part of this image.
	 */
	size_t			 stride;
	size_t			 size;

	/* Frames and playback, only for images without a parent. */
	struct image_frame	*frames;
	u_int			 nframes;
	u_int			 frame;
	enum image_animation	 animation;
	u_int			 loops;
	u_int			 loop;
	uint64_t		 shown;
	uint64_t		 redrawn;
	int			 redraw;
	struct event		 timer;

	RB_ENTRY(image)		 entry;
	TAILQ_ENTRY(image)	 all_entry;
	TAILQ_ENTRY(image)	 child_entry;
};
RB_HEAD(images, image);
static struct images images = RB_INITIALIZER(&images);

/* All images, most recently used first. */
static struct image_list all_images = TAILQ_HEAD_INITIALIZER(all_images);

/* Number of images and size. */
static u_int image_count;
static size_t image_size;

/* Next image ID. */
static u_int image_next_id;

/* Next frame ID and generation. */
static u_int image_next_frame;

/* Compare images by IDs. */
static int
image_cmp(struct image *a, struct image *b)
{
	if (a->id < b->id)
		return (-1);
	if (a->id > b->id)
		return (1);
	return (0);
}
RB_GENERATE_STATIC(images, image, entry, image_cmp);

/* Record use of an image and its parents. */
void
image_touch(struct image *im)
{
	for (; im != NULL; im = im->parent) {
		if (TAILQ_FIRST(&all_images) != im) {
			TAILQ_REMOVE(&all_images, im, all_entry);
			TAILQ_INSERT_HEAD(&all_images, im, all_entry);
		}
	}
}

/* Find an image by its ID. */
struct image *
image_find(u_int id)
{
	struct image	find;

	find.id = id;
	return (RB_FIND(images, &images, &find));
}

/* Get the ID of an image. */
u_int
image_get_id(struct image *im)
{
	return (im->id);
}

/* Get order ID of an image. */
u_int
image_get_order_id(struct image *im)
{
	return (im->order_id);
}

/* Set the order ID on an image. */
void
image_set_order_id(struct image *im, u_int id)
{
	im->order_id = id;
}

/* Get the z-index of an image. */
int32_t
image_get_zindex(struct image *im)
{
	return (im->zindex);
}

/* Set the z-index of an image. */
void
image_set_zindex(struct image *im, int32_t zindex)
{
	im->zindex = zindex;
}

/* Get the type of an image. */
enum image_type
image_get_type(struct image *im)
{
	return (im->type);
}

/* Remove an image and children. */
static void
image_remove_all(struct image *im)
{
	struct image *child;

	image_ref(im);
	while ((child = TAILQ_FIRST(&im->children)) != NULL)
		image_remove_all(child);
	image_grid_remove_all(im);
	image_free(im);
}

/* Find the oldest image. */
static struct image *
image_find_oldest(struct image *keep, size_t size)
{
	struct image	*im, *parent;
	size_t		 limit;

	limit = options_get_number(global_options, "image-total-size-limit");
	TAILQ_FOREACH_REVERSE(im, &all_images, image_list, all_entry) {
		if (im->parent != NULL &&
		    (image_size > limit || size > limit - image_size))
			continue;
		for (parent = keep; parent != NULL; parent = parent->parent) {
			if (parent == im)
				break;
		}
		if (parent == NULL)
			return (im);
	}
	return (NULL);
}

/* Is there enough space for this image or frame? */
static int
image_can_add(size_t size, int new_image)
{
	u_int	count;
	size_t	limit;

	count = options_get_number(global_options, "image-total-count-limit");
	if (new_image && image_count >= count)
		return (0);
	limit = options_get_number(global_options, "image-total-size-limit");
	if (image_size > limit || size > limit - image_size)
		return (0);
	return (1);
}

/* Check limits and remove images if needed. */
static int
image_check_limits(struct image *keep, size_t size, int new_image)
{
	struct image	*oldest;
	struct client	*c;
	size_t		 limit;

	limit = options_get_number(global_options, "image-total-size-limit");
	if (size > limit)
		return (-1);
	if (image_can_add(size, new_image))
		return (0);

	TAILQ_FOREACH(c, &clients, entry)
		image_tty_free(&c->tty, 1);
	while (!image_can_add(size, new_image)) {
		oldest = image_find_oldest(keep, size);
		if (oldest == NULL)
			break;
		image_remove_all(oldest);
	}
	TAILQ_FOREACH(c, &clients, entry)
		server_redraw_client(c);
	if (!image_can_add(size, new_image))
		return (-1);
	return (0);
}

/* Allocate an image and assign a unique ID. */
static struct image *
image_alloc(enum image_type type, struct image_geom *ig, size_t stride)
{
	struct image	*im;
	u_int		 id;

	do {
		if (++image_next_id == 0)
			image_next_id++;
		id = image_next_id;
	} while (image_find(id) != NULL);

	im = xcalloc(1, sizeof *im);
	im->id = id;
	im->type = type;
	im->references = 1;
	im->order_id = id;
	im->geom = *ig;
	im->stride = stride;
	im->size = (ig->src_h - 1) * stride + ig->src_w * 4;
	TAILQ_INIT(&im->children);
	RB_INSERT(images, &images, im);
	image_count++;
	return (im);
}

/* Create an image, either with pixels or sharing a parent's frames. */
static struct image *
image_new(enum image_type type, struct image *parent, struct image_geom *ig,
    size_t stride, u_char *pixels)
{
	struct image	*im;
	size_t		 size = 0, limit;

	limit = options_get_number(global_options, "image-size-limit");
	if (ig->src_w == 0 || ig->src_h == 0)
		return (NULL);
	if (ig->canvas_width == 0 || ig->canvas_height == 0)
		return (NULL);
	if (ig->sx == 0 || ig->sy == 0 || (parent == NULL && pixels == NULL))
		return (NULL);
	if ((uint64_t)ig->src_w * ig->src_h > limit / 4)
		return (NULL);
	if (ig->sx > USHRT_MAX || ig->sy > USHRT_MAX)
		return (NULL);
	if (ig->sx * ig->sy > limit || stride < ig->src_w * 4)
		return (NULL);
	if (ig->scaled_width <= 0 || ig->scaled_height <= 0)
		return (NULL);

	/* Check limits before allocating the image. */
	if (parent == NULL)
		size = (ig->src_h - 1) * stride + ig->src_w * 4;
	if (image_check_limits(parent, size, 1) != 0)
		return (NULL);

	im = image_alloc(type, ig, stride);
	im->parent = parent;
	if (parent == NULL) {
		im->frames = xcalloc(1, sizeof *im->frames);
		im->nframes = 1;
		im->frames[0].pixels = pixels;
		im->frames[0].id = im->frames[0].generation =
		    ++image_next_frame;
		image_size += im->size;
	} else {
		im->order_id = parent->order_id;
		TAILQ_INSERT_TAIL(&parent->children, im, child_entry);
		image_ref(parent);
	}
	TAILQ_INSERT_HEAD(&all_images, im, all_entry);
	image_touch(im);
	return (im);
}

/* Create an image with the provided pixels. */
struct image *
image_create(enum image_type type, struct image_geom *ig, u_char *pixels)
{
	return (image_new(type, NULL, ig, ig->src_w * 4, pixels));
}

/* Create an image showing part of a parent image. */
struct image *
image_create_with_parent(enum image_type type, struct image *parent,
    struct image_geom *ig)
{
	return (image_new(type, parent, ig, parent->stride, NULL));
}

/* Add a reference to an image. */
void
image_ref(struct image *im)
{
	if (im->references == UINT_MAX)
		fatalx("invalid image reference %u", im->id);
	im->references++;
}

/* Free an image. */
void
image_free(struct image *im)
{
	u_int	i;

	if (im->references == 0)
		fatalx("invalid image reference %u", im->id);
	if (--im->references == 0) {
		RB_REMOVE(images, &images, im);
		TAILQ_REMOVE(&all_images, im, all_entry);
		image_count--;
		if (im->parent == NULL) {
			if (evtimer_initialized(&im->timer))
				evtimer_del(&im->timer);
			image_size -= im->size * im->nframes;
			for (i = 0; i < im->nframes; i++)
				free(im->frames[i].pixels);
			free(im->frames);
		} else {
			TAILQ_REMOVE(&im->parent->children, im, child_entry);
			image_free(im->parent);
		}
		free(im);
	}
}

/* Get the image geometry. */
struct image_geom *
image_get_geometry(struct image *im)
{
	return (&im->geom);
}

/* Get the image without a parent which holds an image's frames. */
struct image *
image_get_root(struct image *im)
{
	while (im->parent != NULL)
		im = im->parent;
	return (im);
}

/* Get the pixels of an image in a frame. */
static u_char *
image_frame_pixels(struct image *im, u_int n)
{
	u_char	*base;

	if (im->parent != NULL)
		base = image_frame_pixels(im->parent, n);
	else
		base = im->frames[n].pixels;
	return (base + im->geom.src_y * im->stride + im->geom.src_x * 4);
}

/* Get the image pixels in the current frame. */
u_char *
image_get_pixels(struct image *im, size_t *stride, size_t *size)
{
	if (stride != NULL)
		*stride = im->stride;
	if (size != NULL)
		*size = im->size;
	return (image_frame_pixels(im, image_get_root(im)->frame));
}

/* Convert an output X position to a source image position in pixels. */
static double
image_map_x(struct image_geom *ig, u_int x, u_int w)
{
	double	p;

	p = (double)x * ig->canvas_width;
	p -= (double)ig->canvas_xoff * w;
	p *= ig->src_w;
	return (p / (w * ig->scaled_width));
}

/* Convert an output Y position to a source image position in pixels. */
static double
image_map_y(struct image_geom *ig, u_int y, u_int h)
{
	double	p;

	p = ((double)y * ig->canvas_height) - ((double)ig->canvas_yoff * h);
	return ((p * ig->src_h) / (h * ig->scaled_height));
}

/* Find source image bounds for part of an output. */
int
image_get_bounds(struct image_geom *ig, u_int x, u_int y, u_int w, u_int h,
    u_int sx, u_int sy, struct image_bounds *ib)
{
	if (x >= sx || y >= sy || w == 0 || h == 0)
		return (0);
	if (w > sx - x)
		w = sx - x;
	if (h > sy - y)
		h = sy - y;

	/* Map the requested area and clip it to the source image. */
	ib->left = image_map_x(ig, x, sx);
	ib->top = image_map_y(ig, y, sy);
	ib->right = image_map_x(ig, x + w, sx);
	ib->bottom = image_map_y(ig, y + h, sy);
	if (ib->left >= ig->src_w ||
	    ib->top >= ig->src_h ||
	    ib->right <= 0 ||
	    ib->bottom <= 0)
		return (0);
	ib->area = (ib->right - ib->left) * (ib->bottom - ib->top);

	if (ib->left < 0)
		ib->left = 0;
	if (ib->top < 0)
		ib->top = 0;
	if (ib->right > ig->src_w)
		ib->right = ig->src_w;
	if (ib->bottom > ig->src_h)
		ib->bottom = ig->src_h;
	return (1);
}

/* Map a pixel in the full output canvas to the current frame's pixels. */
u_char *
image_get_pixel(struct image *im, u_int x, u_int y, u_int w, u_int h)
{
	struct image	*root = image_get_root(im);

	return (image_get_frame_pixel(im, root->frame, x, y, w, h));
}

/* Map a pixel in the full output canvas to a frame's pixels. */
u_char *
image_get_frame_pixel(struct image *im, u_int n, u_int x, u_int y, u_int w,
    u_int h)
{
	static u_char		 empty[4];
	struct image_geom	*ig = &im->geom;
	double			 px, py;
	u_int			 sx, sy;

	px = image_map_x(ig, x, w);
	py = image_map_y(ig, y, h);
	if (px < 0 || py < 0 || px >= ig->src_w || py >= ig->src_h)
		return (empty);
	sx = px;
	sy = py;
	return (image_frame_pixels(im, n) + sy * im->stride + sx * 4);
}

/* Calculate the cell size. */
void
image_size_in_cells(u_int w, u_int h, u_int xpixel, u_int ypixel, u_int *sx,
    u_int *sy)
{
	if (xpixel == 0)
		xpixel = 8;
	if (ypixel == 0)
		ypixel = 16;
	*sx = w / xpixel + (w % xpixel != 0);
	*sy = h / ypixel + (h % ypixel != 0);
}

/* Find the source pixels covered by a rectangle of placement cells. */
void
image_get_pixel_area(struct image *im, u_int x, u_int y, u_int w, u_int h,
    u_int *px, u_int *py, u_int *pw, u_int *ph)
{
	struct image_geom	*ig;
	struct image_bounds	 ib;

	*px = *py = *pw = *ph = 0;

	if (im != NULL) {
		ig = &im->geom;
		if (image_get_bounds(ig, x, y, w, h, ig->sx, ig->sy, &ib)) {
			*px = floor(ib.left);
			*py = floor(ib.top);
			*pw = ceil(ib.right) - *px;
			*ph = ceil(ib.bottom) - *py;
		}
	}
}

/* Get the number of frames of an image. */
u_int
image_get_frame_count(struct image *im)
{
	return (image_get_root(im)->nframes);
}

/* Get the current frame of an image. */
u_int
image_get_frame(struct image *im)
{
	return (image_get_root(im)->frame);
}

/* Get the full pixels of a frame, or NULL if there is no such frame. */
u_char *
image_get_frame_pixels(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return (NULL);
	return (image_frame_pixels(im, n));
}

/* Get the ID of a frame, which does not change while the frame exists. */
u_int
image_get_frame_id(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return (0);
	return (im->frames[n].id);
}

/* Get the generation of a frame, which changes with its pixels. */
u_int
image_get_frame_generation(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return (0);
	return (im->frames[n].generation);
}

/* Get the total time of all frames. */
static uint64_t
image_animation_duration(struct image *im)
{
	uint64_t	total = 0;
	u_int		i;

	for (i = 0; i < im->nframes; i++)
		total += im->frames[i].gap;
	return (total);
}

/* Check whether playback moves an image to other frames. */
static int
image_is_animating(struct image *im)
{
	if (im->animation == IMAGE_ANIMATION_STOPPED || im->nframes < 2)
		return (0);
	if (im->loops != 0 && im->loop >= im->loops)
		return (0);
	return (image_animation_duration(im) != 0);
}

/* Move to the next frame with a gap, unless playback stops first. */
static int
image_animation_advance(struct image *im)
{
	u_int	n = im->frame, next;

	for (;;) {
		next = (n + 1) % im->nframes;
		if (next == 0) {
			if (im->animation == IMAGE_ANIMATION_LOADING)
				return (0);
			im->loop++;
			if (im->loops != 0 && im->loop >= im->loops)
				return (0);
		}
		n = next;
		if (im->frames[n].gap != 0)
			break;
	}
	im->frame = n;
	return (1);
}

/* Start the playback timer to fire after a delay in milliseconds. */
static void
image_animation_schedule(struct image *im, uint64_t delay)
{
	struct timeval	tv;

	tv.tv_sec = delay / 1000;
	tv.tv_usec = (delay % 1000) * 1000;
	evtimer_add(&im->timer, &tv);
}

/*
 * Advance playback and update clients. The timer stops while the image is not
 * visible and is started again when it is drawn.
 */
static void
image_animation_timer(__unused int fd, __unused short events, void *arg)
{
	struct image	*im = arg;
	uint64_t	 now = get_timer(), next = 0;
	int		 flags, redraw;

	if (~image_show_frame(im, 0) & IMAGE_SHOWN) {
		im->redraw = 0;
		return;
	}

	if (image_is_animating(im)) {
		if (im->shown == 0 || im->shown > now)
			im->shown = now;
		next = im->shown + im->frames[im->frame].gap;
		if (now >= next && image_animation_advance(im)) {
			im->shown = now;
			im->redraw = 1;
			next = now + im->frames[im->frame].gap;
		}
		if (next <= now)
			next = 0;
	}

	redraw = (im->redraw && now >= im->redrawn + IMAGE_ANIMATION_REDRAW);
	flags = image_show_frame(im, redraw);
	if (~flags & IMAGE_SHOWN_REDRAW)
		im->redraw = 0;
	else if (redraw) {
		im->redraw = 0;
		im->redrawn = now;
	} else if (im->redraw &&
	    (next == 0 || im->redrawn + IMAGE_ANIMATION_REDRAW < next))
		next = im->redrawn + IMAGE_ANIMATION_REDRAW;

	if (next != 0)
		image_animation_schedule(im, next - now);
}

/* Run the playback timer soon to apply a change. */
static void
image_animation_update(struct image *im, int force)
{
	if (!evtimer_initialized(&im->timer))
		evtimer_set(&im->timer, image_animation_timer, im);
	if (!force && evtimer_pending(&im->timer, NULL))
		return;
	if (!image_is_animating(im) && !im->redraw)
		return;
	image_animation_schedule(im, 0);
}

/* Start playback of an image which has been drawn, if it is stopped. */
void
image_animation_drawn(struct image *im)
{
	im = image_get_root(im);
	if (im->nframes > 1)
		image_animation_update(im, 0);
}

/* Add a frame of full image pixels. Returns -1 if there is no space. */
int
image_add_frame(struct image *im, u_char *pixels, u_int gap)
{
	struct image_frame	*f;

	im = image_get_root(im);
	if (image_check_limits(im, im->size, 0) != 0)
		return (-1);

	im->frames = xreallocarray(im->frames, im->nframes + 1,
	    sizeof *im->frames);
	f = &im->frames[im->nframes++];
	f->pixels = pixels;
	f->gap = gap;
	f->id = f->generation = ++image_next_frame;
	image_size += im->size;

	image_animation_update(im, 1);
	return (0);
}

/* Remove a frame, unless it is the only one. */
void
image_delete_frame(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes || im->nframes == 1)
		return;

	free(im->frames[n].pixels);
	memmove(&im->frames[n], &im->frames[n + 1],
	    (im->nframes - n - 1) * sizeof *im->frames);
	im->nframes--;
	image_size -= im->size;

	if (im->frame > n)
		im->frame--;
	else if (im->frame == n) {
		if (im->frame == im->nframes)
			im->frame--;
		im->shown = 0;
		im->redraw = 1;
	}
	image_animation_update(im, 1);
}

/* Record that the pixels of a frame have changed. */
void
image_frame_changed(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return;
	im->frames[n].generation = ++image_next_frame;
	if (n == im->frame) {
		im->shown = 0;
		im->redraw = 1;
		image_animation_update(im, 1);
	}
}

/* Get the time a frame is shown in milliseconds; 0 means it is skipped. */
u_int
image_get_frame_gap(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return (0);
	return (im->frames[n].gap);
}

/* Set the time a frame is shown. */
void
image_set_frame_gap(struct image *im, u_int n, u_int gap)
{
	im = image_get_root(im);
	if (n >= im->nframes)
		return;
	im->frames[n].gap = gap;
	image_animation_update(im, 1);
}

/* Show a frame. */
void
image_set_frame(struct image *im, u_int n)
{
	im = image_get_root(im);
	if (n >= im->nframes || n == im->frame)
		return;
	im->frame = n;
	im->shown = 0;
	im->redraw = 1;
	image_animation_update(im, 1);
}

/* Set the playback state. This always resets the loop counter. */
void
image_set_animation(struct image *im, enum image_animation animation)
{
	im = image_get_root(im);
	if (im->animation == IMAGE_ANIMATION_STOPPED &&
	    animation != IMAGE_ANIMATION_STOPPED)
		im->shown = 0;
	im->animation = animation;
	im->loop = 0;
	image_animation_update(im, 1);
}

/* Set the number of loops to play; 0 means no limit. */
void
image_set_animation_loops(struct image *im, u_int loops)
{
	im = image_get_root(im);
	im->loops = loops;
	image_animation_update(im, 1);
}
