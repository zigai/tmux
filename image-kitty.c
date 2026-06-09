/* $OpenBSD$ */

/*
 * Copyright (c) 2024 tmux contributors
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
#include <sys/stat.h>

#include <resolv.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Kitty graphics protocol support.
 *
 * Phase B: parser and query support.
 */

/* Maximum control data length. */
#define KITTY_MAX_CONTROL_LEN 4096

/* Maximum payload length for direct data. */
#define KITTY_MAX_PAYLOAD_LEN (1024 * 1024)

/* Kitty action types. */
enum kitty_action {
	KITTY_ACTION_QUERY = 'q',
	KITTY_ACTION_TRANSMIT = 't',
	KITTY_ACTION_TRANSMIT_AND_DISPLAY = 'T',
	KITTY_ACTION_PLACE = 'p',
	KITTY_ACTION_DELETE = 'd',
};

/* Parsed Kitty command. */
struct kitty_command {
	enum kitty_action	 action;
	uint32_t		 image_id;		/* i */
	uint32_t		 image_number;		/* I */
	uint32_t		 placement_id;		/* p */
	int			 format;		/* f */
	int			 compression;		/* o */
	int			 transmission;		/* t */
	u_int			 pixel_width;		/* s */
	u_int			 pixel_height;		/* v */
	u_int			 cols;			/* c */
	u_int			 rows;			/* r */
	u_int			 src_x;			/* x */
	u_int			 src_y;			/* y */
	u_int			 src_w;			/* w */
	u_int			 src_h;			/* h */
	u_int			 cell_xoff;		/* X */
	u_int			 cell_yoff;		/* Y */
	int32_t			 zindex;		/* z */
	int			 cursor_no_move;	/* C */
	int			 virtual;		/* U */
	int			 quiet;			/* q */
	int			 more;			/* m */

	u_char			*payload;
	size_t			 payload_len;
};

static void	kitty_command_free(struct kitty_command *);
static void	kitty_image_free_one(struct kitty_images *, struct kitty_image *);
static void	kitty_placement_free_one(struct kitty_placements *,
			    struct kitty_placement *);
static int	kitty_image_dispatch(struct screen *, struct kitty_command *,
			    char **);
static void	kitty_advance_cursor(struct screen *, struct kitty_placement *);

static void
kitty_advance_cursor(struct screen *s, struct kitty_placement *pl)
{
	u_int	sx, sy;

	if (s->grid == NULL) {
		if (pl->cols > 0)
			s->cx += pl->cols;
		if (pl->rows > 0)
			s->cy += pl->rows;
		return;
	}
	sx = screen_size_x(s);
	sy = screen_size_y(s);
	if (sx == 0 || sy == 0)
		return;
	if (pl->cols > 0)
		s->cx += pl->cols;
	if (pl->rows > 1)
		s->cy += pl->rows - 1;
	if (s->cx >= sx) {
		s->cy += s->cx / sx;
		s->cx %= sx;
	}
	if (s->cy >= sy) {
		s->cy = sy - 1;
		s->cx = sx - 1;
	}
}

/* Parse a key=value pair from control data. Returns -1 on error, 0 on skip. */
static int
kitty_parse_key_value(const char *key, const char *value,
    struct kitty_command *cmd)
{
	char		*end;
	long long	 ll;

	if (key[0] == '\0' || key[1] != '\0')
		return (0);

	/* Action and transmission type keys are characters, not numbers. */
	if (key[0] == 'a') {
		cmd->action = (enum kitty_action)(value[0]);
		return (0);
	}
	if (key[0] == 't') {
		cmd->transmission = (int)value[0];
		return (0);
	}

	ll = strtoll(value, &end, 10);
	if (*end != '\0')
		return (-1);

	switch (key[0]) {
	case 'i':
		cmd->image_id = (uint32_t)ll;
		break;
	case 'I':
		cmd->image_number = (uint32_t)ll;
		break;
	case 'p':
		cmd->placement_id = (uint32_t)ll;
		break;
	case 'f':
		cmd->format = (int)ll;
		break;
	case 'o':
		cmd->compression = (int)ll;
		break;
	case 's':
		cmd->pixel_width = (u_int)ll;
		break;
	case 'v':
		cmd->pixel_height = (u_int)ll;
		break;
	case 'c':
		cmd->cols = (u_int)ll;
		break;
	case 'r':
		cmd->rows = (u_int)ll;
		break;
	case 'x':
		cmd->src_x = (u_int)ll;
		break;
	case 'y':
		cmd->src_y = (u_int)ll;
		break;
	case 'w':
		cmd->src_w = (u_int)ll;
		break;
	case 'h':
		cmd->src_h = (u_int)ll;
		break;
	case 'X':
		cmd->cell_xoff = (u_int)ll;
		break;
	case 'Y':
		cmd->cell_yoff = (u_int)ll;
		break;
	case 'z':
		cmd->zindex = (int32_t)ll;
		break;
	case 'C':
		cmd->cursor_no_move = (int)ll;
		break;
	case 'U':
		cmd->virtual = (int)ll;
		break;
	case 'q':
		cmd->quiet = (int)ll;
		break;
	case 'm':
		cmd->more = (int)ll;
		break;
	default:
		return (0);
	}
	return (0);
}

/* Parse Kitty control data. Returns 0 on success, -1 on error. */
static int
kitty_parse_control(const char *data, size_t len, struct kitty_command *cmd)
{
	char		*copy, *cp, *key, *value;
	const char	*semicolon;
	int		 result = 0;

	/* Find semicolon separating control data from payload. */
	semicolon = memchr(data, ';', len);
	if (semicolon != NULL) {
		cmd->payload_len = len - (semicolon - data) - 1;
		if (cmd->payload_len > 0) {
			cmd->payload = xmalloc(cmd->payload_len);
			memcpy(cmd->payload, semicolon + 1, cmd->payload_len);
		}
		len = semicolon - data;
	}

	if (len > KITTY_MAX_CONTROL_LEN) {
		log_debug("kitty: control data too long: %zu", len);
		return (-1);
	}

	/* Copy and parse comma-separated key=value pairs. */
	copy = xstrndup(data, len);
	cp = copy;

	while ((key = strsep(&cp, ",")) != NULL) {
		if (*key == '\0')
			continue;
		value = strchr(key, '=');
		if (value == NULL) {
			log_debug("kitty: invalid key=value: %s", key);
			result = -1;
			break;
		}
		*value++ = '\0';
		if (kitty_parse_key_value(key, value, cmd) != 0) {
			log_debug("kitty: invalid key=%s value=%s", key, value);
			result = -1;
			break;
		}
	}

	free(copy);
	return (result);
}

/* Build a Kitty response string. */
static int
kitty_build_reply(__unused struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	if (cmd->image_id != 0)
		xasprintf(reply, "\033_Gi=%u;OK\033\\", cmd->image_id);
	else
		*reply = xstrdup("\033_Gi=0;OK\033\\");
	return (0);
}

/* Handle query action (a=q). */
static int
kitty_handle_query(struct screen *s, struct kitty_command *cmd, char **reply)
{
	if (cmd->quiet == 1) {
		*reply = NULL;
		return (0);
	}
	return (kitty_build_reply(s, cmd, reply));
}

/* Free a Kitty command. */
static void
kitty_command_free(struct kitty_command *cmd)
{
	free(cmd->payload);
}

/* Free a single Kitty image. */
static void
kitty_image_free_one(struct kitty_images *list, struct kitty_image *img)
{
	if (img == NULL)
		return;

	TAILQ_REMOVE(list, img, entry);
	free(img->payload);
	free(img);
}

/* Free a Kitty placement. */
static void
kitty_placement_free_one(struct kitty_placements *list,
    struct kitty_placement *pl)
{
	if (pl == NULL)
		return;

	TAILQ_REMOVE(list, pl, entry);
	if (pl->image != NULL)
		pl->image->refcount--;
	free(pl);
}

/* Find an image by ID. */
static struct kitty_image *
kitty_image_find(struct screen *s, uint32_t id)
{
	struct kitty_image	*img;

	TAILQ_FOREACH(img, &s->kitty_images, entry) {
		if (img->id == id)
			return (img);
	}
	return (NULL);
}

/* Find an image by number (I). */
static struct kitty_image *
kitty_image_find_by_number(struct screen *s, uint32_t number)
{
	struct kitty_image	*img;

	TAILQ_FOREACH(img, &s->kitty_images, entry) {
		if (img->number == number)
			return (img);
	}
	return (NULL);
}

/* Find a placement by ID and image. */
static struct kitty_placement *
kitty_placement_find(struct screen *s, uint32_t placement_id,
    uint32_t image_id)
{
	struct kitty_placement	*pl;

	TAILQ_FOREACH(pl, &s->kitty_placements, entry) {
		if (pl->placement_id == placement_id &&
		    pl->image != NULL &&
		    pl->image->id == image_id)
			return (pl);
	}
	return (NULL);
}

/* Delete all placements for an image. */
static void
kitty_image_delete_placements(struct screen *s, struct kitty_image *img)
{
	struct kitty_placement	*pl, *pl_next;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->image == img)
			kitty_placement_free_one(&s->kitty_placements, pl);
	}
}

/* Maximum total image bytes per screen. */
#define KITTY_MAX_TOTAL_BYTES (64 * 1024 * 1024)

/* Maximum single image payload. */
#define KITTY_MAX_IMAGE_PAYLOAD (16 * 1024 * 1024)

/* Maximum number of images per screen. */
#define KITTY_MAX_IMAGES 256

/* Maximum number of placements per screen. */
#define KITTY_MAX_PLACEMENTS 512

/* Read a file into memory. */
static u_char *
kitty_read_file(const char *path, size_t *len)
{
	FILE		*f;
	struct stat	 sb;
	u_char		*buf;
	size_t		 n;

	if (stat(path, &sb) != 0)
		return (NULL);
	if (!S_ISREG(sb.st_mode)) {
		log_debug("kitty: not a regular file: %s", path);
		return (NULL);
	}
	if (sb.st_size > KITTY_MAX_IMAGE_PAYLOAD) {
		log_debug("kitty: file too large: %s", path);
		return (NULL);
	}
	if (sb.st_size == 0)
		return (NULL);

	f = fopen(path, "rb");
	if (f == NULL)
		return (NULL);

	buf = xmalloc(sb.st_size);
	n = fread(buf, 1, sb.st_size, f);
	fclose(f);

	if (n != (size_t)sb.st_size) {
		free(buf);
		return (NULL);
	}

	*len = n;
	return (buf);
}

/* Check memory limits before storing. */
static int
kitty_image_check_limits(struct screen *s, size_t new_payload)
{
	struct kitty_image	*img;
	size_t			 total = 0;
	u_int			 count = 0;

	if (new_payload > KITTY_MAX_IMAGE_PAYLOAD) {
		log_debug("kitty: payload too large: %zu", new_payload);
		return (-1);
	}

	TAILQ_FOREACH(img, &s->kitty_images, entry) {
		total += img->payload_len;
		count++;
	}

	if (total + new_payload > KITTY_MAX_TOTAL_BYTES) {
		log_debug("kitty: total bytes exceeded");
		return (-1);
	}

	if (count >= KITTY_MAX_IMAGES) {
		log_debug("kitty: max images reached");
		return (-1);
	}

	return (0);
}

/* Store a new image. */
static struct kitty_image *
kitty_image_store(struct screen *s, struct kitty_command *cmd)
{
	struct kitty_image	*img;

	if (kitty_image_check_limits(s, cmd->payload_len) != 0)
		return (NULL);

	img = xcalloc(1, sizeof *img);
	img->id = cmd->image_id;
	img->number = cmd->image_number;
	img->format = cmd->format;
	img->compression = cmd->compression;
	img->pixel_width = cmd->pixel_width;
	img->pixel_height = cmd->pixel_height;
	img->refcount = 1;

	if (cmd->payload_len > 0) {
		img->payload = xmalloc(cmd->payload_len);
		memcpy(img->payload, cmd->payload, cmd->payload_len);
		img->payload_len = cmd->payload_len;
	}

	TAILQ_INSERT_TAIL(&s->kitty_images, img, entry);
	log_debug("kitty: stored image id=%u size=%zu", img->id,
	    img->payload_len);

	return (img);
}

/* Replace an existing image. */
static struct kitty_image *
kitty_image_replace(struct screen *s, struct kitty_image *old,
    struct kitty_command *cmd)
{
	struct kitty_image	*img;

	if (kitty_image_check_limits(s, cmd->payload_len) != 0)
		return (NULL);

	img = xcalloc(1, sizeof *img);
	img->id = cmd->image_id;
	img->number = cmd->image_number;
	img->format = cmd->format;
	img->compression = cmd->compression;
	img->pixel_width = cmd->pixel_width;
	img->pixel_height = cmd->pixel_height;
	img->refcount = old->refcount;

	if (cmd->payload_len > 0) {
		img->payload = xmalloc(cmd->payload_len);
		memcpy(img->payload, cmd->payload, cmd->payload_len);
		img->payload_len = cmd->payload_len;
	}

	/* Delete old placements for this image. */
	kitty_image_delete_placements(s, old);

	/* Replace old image in the list. */
	TAILQ_INSERT_AFTER(&s->kitty_images, old, img, entry);
	TAILQ_REMOVE(&s->kitty_images, old, entry);

	if (old->refcount > 0)
		old->refcount = 0;
	free(old->payload);
	free(old);

	log_debug("kitty: replaced image id=%u size=%zu", img->id,
	    img->payload_len);

	return (img);
}

/* Create a placement. */
static struct kitty_placement *
kitty_placement_create(struct screen *s, struct kitty_image *img,
    struct kitty_command *cmd)
{
	struct kitty_placement	*pl;
	u_int			 count = 0;

	TAILQ_FOREACH(pl, &s->kitty_placements, entry)
		count++;
	if (count >= KITTY_MAX_PLACEMENTS) {
		log_debug("kitty: max placements reached");
		return (NULL);
	}

	pl = xcalloc(1, sizeof *pl);
	pl->placement_id = cmd->placement_id;
	pl->image = img;
	pl->pane_x = s->cx;
	pl->pane_y = s->cy;
	pl->cols = cmd->cols;
	pl->rows = cmd->rows;
	pl->src_x = cmd->src_x;
	pl->src_y = cmd->src_y;
	pl->src_w = cmd->src_w;
	pl->src_h = cmd->src_h;
	pl->cell_xoff = cmd->cell_xoff;
	pl->cell_yoff = cmd->cell_yoff;
	pl->zindex = cmd->zindex;
	pl->cursor_no_move = cmd->cursor_no_move;
	pl->virtual = cmd->virtual;

	img->refcount++;
	TAILQ_INSERT_TAIL(&s->kitty_placements, pl, entry);

	log_debug("kitty: placement id=%u image=%u at %d,%d", pl->placement_id,
	    img->id, pl->pane_x, pl->pane_y);

	return (pl);
}

/* Initialize Kitty image state for a screen. */
void
kitty_image_init(struct screen *s)
{
	TAILQ_INIT(&s->kitty_images);
	TAILQ_INIT(&s->kitty_placements);
	memset(&s->kitty_pending, 0, sizeof s->kitty_pending);
}

/* Free all Kitty images and placements for a screen. */
void
kitty_image_free_all(struct screen *s)
{
	struct kitty_image	*img, *img_next;
	struct kitty_placement	*pl, *pl_next;

	/* Placements hold references to images, so free them before images. */
	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next)
		kitty_placement_free_one(&s->kitty_placements, pl);

	TAILQ_FOREACH_SAFE(img, &s->kitty_images, entry, img_next)
		kitty_image_free_one(&s->kitty_images, img);

	if (s->kitty_pending.active) {
		free(s->kitty_pending.payload);
		memset(&s->kitty_pending, 0, sizeof s->kitty_pending);
	}
}

/* Append data to a pending chunked upload. */
static int
kitty_pending_append(struct screen *s, u_char *payload, size_t len)
{
	struct kitty_pending	*pending = &s->kitty_pending;
	size_t			 new_size;

	new_size = pending->payload_len + len;
	if (new_size > KITTY_MAX_IMAGE_PAYLOAD) {
		log_debug("kitty: chunked upload too large");
		free(pending->payload);
		memset(pending, 0, sizeof *pending);
		return (-1);
	}

	if (pending->payload_space < new_size) {
		pending->payload_space = new_size + 4096;
		pending->payload = xrealloc(pending->payload,
		    pending->payload_space);
	}

	memcpy(pending->payload + pending->payload_len, payload, len);
	pending->payload_len = new_size;
	return (0);
}

/* Parse a Kitty graphics command. Returns 0 on success, -1 on error.
 * If a reply is needed, *reply is set to an allocated string.
 */
int
kitty_image_parse(struct screen *s, const char *data, size_t len,
    char **reply)
{
	struct kitty_command	 cmd;
	int			 result;

	memset(&cmd, 0, sizeof cmd);
	*reply = NULL;

	log_debug("kitty: received %zu bytes", len);

	result = kitty_parse_control(data, len, &cmd);
	if (result != 0)
		goto done;

	/* Handle chunked uploads. */
	if (s->kitty_pending.active) {
		/* Continuing a chunked upload. */
		if (cmd.payload_len > 0) {
			if (kitty_pending_append(s, cmd.payload, cmd.payload_len)
			    != 0) {
				result = -1;
				goto done;
			}
		}
		if (cmd.more == 1) {
			/* More chunks coming. */
			if (!cmd.quiet)
				result = kitty_build_reply(s, &cmd, reply);
			goto done;
		}
		/* Final chunk - process the complete command using metadata
		 * from the first chunk. */
		free(cmd.payload);
		cmd.action = (enum kitty_action)s->kitty_pending.action;
		cmd.image_id = s->kitty_pending.id;
		cmd.image_number = s->kitty_pending.number;
		cmd.placement_id = s->kitty_pending.placement_id;
		cmd.format = s->kitty_pending.format;
		cmd.compression = s->kitty_pending.compression;
		cmd.transmission = s->kitty_pending.transmission;
		cmd.pixel_width = s->kitty_pending.pixel_width;
		cmd.pixel_height = s->kitty_pending.pixel_height;
		cmd.cols = s->kitty_pending.cols;
		cmd.rows = s->kitty_pending.rows;
		cmd.src_x = s->kitty_pending.src_x;
		cmd.src_y = s->kitty_pending.src_y;
		cmd.src_w = s->kitty_pending.src_w;
		cmd.src_h = s->kitty_pending.src_h;
		cmd.cell_xoff = s->kitty_pending.cell_xoff;
		cmd.cell_yoff = s->kitty_pending.cell_yoff;
		cmd.zindex = s->kitty_pending.zindex;
		cmd.cursor_no_move = s->kitty_pending.cursor_no_move;
		cmd.virtual = s->kitty_pending.virtual;
		cmd.quiet = s->kitty_pending.quiet;
		cmd.payload = s->kitty_pending.payload;
		cmd.payload_len = s->kitty_pending.payload_len;
		result = kitty_image_dispatch(s, &cmd, reply);
		memset(&s->kitty_pending, 0, sizeof s->kitty_pending);
		goto done;
	}

	if (cmd.more == 1) {
		/* Start of a chunked upload. */
		s->kitty_pending.active = 1;
		s->kitty_pending.id = cmd.image_id;
		s->kitty_pending.number = cmd.image_number;
		s->kitty_pending.action = cmd.action;
		s->kitty_pending.placement_id = cmd.placement_id;
		s->kitty_pending.format = cmd.format;
		s->kitty_pending.compression = cmd.compression;
		s->kitty_pending.transmission = cmd.transmission;
		s->kitty_pending.pixel_width = cmd.pixel_width;
		s->kitty_pending.pixel_height = cmd.pixel_height;
		s->kitty_pending.cols = cmd.cols;
		s->kitty_pending.rows = cmd.rows;
		s->kitty_pending.src_x = cmd.src_x;
		s->kitty_pending.src_y = cmd.src_y;
		s->kitty_pending.src_w = cmd.src_w;
		s->kitty_pending.src_h = cmd.src_h;
		s->kitty_pending.cell_xoff = cmd.cell_xoff;
		s->kitty_pending.cell_yoff = cmd.cell_yoff;
		s->kitty_pending.zindex = cmd.zindex;
		s->kitty_pending.cursor_no_move = cmd.cursor_no_move;
		s->kitty_pending.virtual = cmd.virtual;
		s->kitty_pending.quiet = cmd.quiet;
		if (cmd.payload_len > 0) {
			if (kitty_pending_append(s, cmd.payload, cmd.payload_len)
			    != 0) {
				result = -1;
				goto done;
			}
		}
		if (!cmd.quiet)
			result = kitty_build_reply(s, &cmd, reply);
		goto done;
	}

	result = kitty_image_dispatch(s, &cmd, reply);

done:
	kitty_command_free(&cmd);
	return (result);
}

/* Decode a base64 direct payload if needed. */
static int
kitty_decode_direct_payload(struct kitty_command *cmd)
{
	u_char	*decoded;
	char	*encoded;
	int	 outlen;

	if (cmd->payload_len == 0)
		return (0);

	/* PNG payload may already have been decoded by older paths. */
	if (cmd->format == KITTY_FORMAT_PNG && cmd->payload_len >= 8 &&
	    memcmp(cmd->payload, "\211PNG\r\n\032\n", 8) == 0)
		return (0);

	encoded = xstrndup((const char *)cmd->payload, cmd->payload_len);
	decoded = xmalloc(cmd->payload_len);
	outlen = b64_pton(encoded, decoded, cmd->payload_len);
	free(encoded);

	if (outlen == -1) {
		free(decoded);
		if (cmd->format == KITTY_FORMAT_PNG)
			return (0);
		return (-1);
	}

	if (cmd->format == KITTY_FORMAT_PNG &&
	    (outlen < 8 || memcmp(decoded, "\211PNG\r\n\032\n", 8) != 0)) {
		free(decoded);
		return (0);
	}

	free(cmd->payload);
	cmd->payload = decoded;
	cmd->payload_len = (size_t)outlen;

	if (cmd->format != KITTY_FORMAT_PNG)
		return (0);

	if (cmd->pixel_width == 0 && cmd->payload_len >= 24) {
		cmd->pixel_width = ((u_int)cmd->payload[16] << 24) |
		    ((u_int)cmd->payload[17] << 16) |
		    ((u_int)cmd->payload[18] << 8) |
		    (u_int)cmd->payload[19];
	}
	if (cmd->pixel_height == 0 && cmd->payload_len >= 24) {
		cmd->pixel_height = ((u_int)cmd->payload[20] << 24) |
		    ((u_int)cmd->payload[21] << 16) |
		    ((u_int)cmd->payload[22] << 8) |
		    (u_int)cmd->payload[23];
	}

	return (0);
}

/* Read file payload for transmit action. */
static int
kitty_handle_transmit_file(struct kitty_command *cmd)
{
	u_char	*buf;
	size_t	 len;
	char	*path;

	/* t=f (regular file) or t=t (temporary file) */
	if (cmd->payload_len == 0)
		return (-1);

	path = xstrndup((const char *)cmd->payload, cmd->payload_len);
	buf = kitty_read_file(path, &len);
	if (buf == NULL) {
		free(path);
		return (-1);
	}

	free(cmd->payload);
	cmd->payload = buf;
	cmd->payload_len = len;

	/* For temporary files, try to delete after reading. */
	if (cmd->transmission == 't')
		unlink(path);
	free(path);

	return (0);
}

/* Handle transmit action (a=t or a=T). */
static int
kitty_handle_transmit(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	struct kitty_image	*img;

	if (cmd->image_id == 0 && cmd->image_number == 0) {
		log_debug("kitty: transmit without image id or number");
		if (!cmd->quiet)
			*reply = xstrdup("\033_Gi=0;EINVAL\033\\");
		return (-1);
	}

	/* Handle file transmission if needed. */
	if (cmd->transmission == 'f' || cmd->transmission == 't') {
		if (kitty_handle_transmit_file(cmd) != 0) {
			if (!cmd->quiet)
				*reply = xstrdup("\033_Gi=0;ENOENT\033\\");
			return (-1);
		}
	} else if (cmd->transmission == 's') {
		log_debug("kitty: shared memory not supported");
		if (!cmd->quiet)
			*reply = xstrdup("\033_Gi=0;ENOTSUP\033\\");
		return (-1);
	} else if (kitty_decode_direct_payload(cmd) != 0) {
		if (!cmd->quiet)
			*reply = xstrdup("\033_Gi=0;EINVAL\033\\");
		return (-1);
	}

	if (cmd->image_id != 0) {
		img = kitty_image_find(s, cmd->image_id);
		if (img != NULL)
			img = kitty_image_replace(s, img, cmd);
		else
			img = kitty_image_store(s, cmd);
	} else {
		img = kitty_image_find_by_number(s, cmd->image_number);
		if (img != NULL)
			img = kitty_image_replace(s, img, cmd);
		else
			img = kitty_image_store(s, cmd);
	}

	if (img == NULL) {
		if (!cmd->quiet)
			*reply = xstrdup("\033_Gi=0;ENOMEM\033\\");
		return (-1);
	}

	/* For a=T, also create a placement. */
	if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
		if (cmd->virtual != 1) {
			struct kitty_placement	*pl;
			pl = kitty_placement_create(s, img, cmd);
			if (pl == NULL && !cmd->quiet) {
				*reply = xstrdup("\033_Gi=0;ENOMEM\033\\");
				return (-1);
			}
			if (pl != NULL && !cmd->cursor_no_move)
				kitty_advance_cursor(s, pl);
		}
	}

	if (!cmd->quiet)
		return (kitty_build_reply(s, cmd, reply));
	return (0);
}

/* Handle place action (a=p). */
static int
kitty_handle_place(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl;

	if (cmd->image_id != 0)
		img = kitty_image_find(s, cmd->image_id);
	else
		img = kitty_image_find_by_number(s, cmd->image_number);
	if (img == NULL) {
		log_debug("kitty: place image not found: %u/%u",
		    cmd->image_id, cmd->image_number);
		if (!cmd->quiet)
			*reply = xstrdup("\033_Gi=0;ENOENT\033\\");
		return (-1);
	}

	/* Check if replacing existing placement. */
	if (cmd->placement_id != 0) {
		pl = kitty_placement_find(s, cmd->placement_id, img->id);
		if (pl != NULL) {
			/* Replace existing placement. */
			pl->pane_x = s->cx;
			pl->pane_y = s->cy;
			pl->cols = cmd->cols;
			pl->rows = cmd->rows;
			pl->src_x = cmd->src_x;
			pl->src_y = cmd->src_y;
			pl->src_w = cmd->src_w;
			pl->src_h = cmd->src_h;
			pl->cell_xoff = cmd->cell_xoff;
			pl->cell_yoff = cmd->cell_yoff;
			pl->zindex = cmd->zindex;
			pl->cursor_no_move = cmd->cursor_no_move;
			pl->virtual = cmd->virtual;
			log_debug("kitty: replaced placement id=%u",
			    pl->placement_id);
			if (!cmd->quiet)
				return (kitty_build_reply(s, cmd, reply));
			return (0);
		}
	}

	if (cmd->virtual != 1) {
		pl = kitty_placement_create(s, img, cmd);
		if (pl == NULL && !cmd->quiet) {
			*reply = xstrdup("\033_Gi=0;ENOMEM\033\\");
			return (-1);
		}
		if (pl != NULL && !cmd->cursor_no_move)
			kitty_advance_cursor(s, pl);
	}

	if (!cmd->quiet)
		return (kitty_build_reply(s, cmd, reply));
	return (0);
}

/* Delete a specific image by ID. */
static int
kitty_delete_image(struct screen *s, uint32_t image_id, int delete_data)
{
	struct kitty_image	*img, *img_next;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->image != NULL && pl->image->id == image_id) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
		}
	}

	if (delete_data) {
		TAILQ_FOREACH_SAFE(img, &s->kitty_images, entry, img_next) {
			if (img->id == image_id) {
				kitty_image_free_one(&s->kitty_images, img);
				found = 1;
				break;
			}
		}
	}

	return (found);
}

/* Delete all placements for an image. */
static int
kitty_delete_image_placements(struct screen *s, uint32_t image_id)
{
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->image != NULL && pl->image->id == image_id) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
		}
	}
	return (found);
}

/* Delete all visible placements. */
static int
kitty_delete_all_placements(struct screen *s)
{
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		kitty_placement_free_one(&s->kitty_placements, pl);
		found = 1;
	}
	return (found);
}

/* Delete all images and placements. */
static int
kitty_delete_all(struct screen *s)
{
	struct kitty_image	*img, *img_next;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		kitty_placement_free_one(&s->kitty_placements, pl);
		found = 1;
	}
	TAILQ_FOREACH_SAFE(img, &s->kitty_images, entry, img_next) {
		kitty_image_free_one(&s->kitty_images, img);
		found = 1;
	}
	return (found);
}

/* Delete placement at cursor position. */
static int
kitty_delete_cursor(struct screen *s)
{
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->pane_x <= (int)s->cx &&
		    (int)s->cx < pl->pane_x + (int)pl->cols &&
		    pl->pane_y <= (int)s->cy &&
		    (int)s->cy < pl->pane_y + (int)pl->rows) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
		}
	}
	return (found);
}

/* Delete placements by z-index. */
static int
kitty_delete_zindex(struct screen *s, int32_t zindex)
{
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->zindex == zindex) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
		}
	}
	return (found);
}

/* Handle delete action (a=d). */
static int
kitty_handle_delete(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	char		delete_spec;
	int		delete_data = 0;
	int		found = 0;

	/* Parse delete specifier. Default is 'a' (all visible placements). */
	if (cmd->image_id != 0 || cmd->image_number != 0)
		delete_spec = 'i';
	else if (cmd->placement_id != 0)
		delete_spec = 'i';
	else if (cmd->zindex != 0)
		delete_spec = 'z';
	else
		delete_spec = 'a';

	/* Uppercase specifiers also delete image data. */
	if (delete_spec >= 'A' && delete_spec <= 'Z') {
		delete_data = 1;
		delete_spec = delete_spec - 'A' + 'a';
	}

	switch (delete_spec) {
	case 'a':
		found = kitty_delete_all_placements(s);
		if (delete_data)
			kitty_delete_all(s);
		break;
	case 'i':
		if (cmd->image_id != 0) {
			found = kitty_delete_image_placements(s, cmd->image_id);
			if (delete_data)
				kitty_delete_image(s, cmd->image_id, 1);
		} else if (cmd->image_number != 0) {
			struct kitty_image	*img;
			img = kitty_image_find_by_number(s, cmd->image_number);
			if (img != NULL) {
				found = kitty_delete_image_placements(s, img->id);
				if (delete_data)
					kitty_delete_image(s, img->id, 1);
			}
		}
		break;
	case 'c':
		found = kitty_delete_cursor(s);
		break;
	case 'z':
		found = kitty_delete_zindex(s, cmd->zindex);
		break;
	default:
		log_debug("kitty: unknown delete specifier: %c", delete_spec);
		break;
	}

	log_debug("kitty: delete %c, found=%d", delete_spec, found);

	if (!cmd->quiet)
		return (kitty_build_reply(s, cmd, reply));
	return (0);
}

/* Main dispatcher. */
static int
kitty_image_dispatch(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	int	result = 0;

	*reply = NULL;

	switch (cmd->action) {
	case KITTY_ACTION_QUERY:
		result = kitty_handle_query(s, cmd, reply);
		break;
	case KITTY_ACTION_TRANSMIT:
	case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
		result = kitty_handle_transmit(s, cmd, reply);
		break;
	case KITTY_ACTION_PLACE:
		result = kitty_handle_place(s, cmd, reply);
		break;
	case KITTY_ACTION_DELETE:
		result = kitty_handle_delete(s, cmd, reply);
		break;
	default:
		log_debug("kitty: unknown action: %c", (char)cmd->action);
		break;
	}

	return (result);
}

/* Handle scroll up for Kitty images. */
void
kitty_image_scroll_up(struct screen *s, u_int lines)
{
	struct kitty_placement	*pl, *pl_next;
	u_int			 rupper = s->rupper, rlower = s->rlower;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if ((u_int)pl->pane_y < rupper || (u_int)pl->pane_y > rlower)
			continue;
		if ((u_int)pl->pane_y < rupper + lines) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			continue;
		}
		pl->pane_y -= lines;
		if ((u_int)pl->pane_y > rlower)
			kitty_placement_free_one(&s->kitty_placements, pl);
	}
}

/* Check if an area overlaps with any Kitty placements. */
void
kitty_image_check_area(struct screen *s, u_int px, u_int py, u_int nx,
    u_int ny)
{
	struct kitty_placement	*pl, *pl_next;
	u_int			 plx, ply, plnx, plny;

	if (nx == 0 || ny == 0)
		return;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->cols == 0 || pl->rows == 0)
			continue;
		if (pl->pane_x < 0 || pl->pane_y < 0)
			continue;
		plx = (u_int)pl->pane_x;
		ply = (u_int)pl->pane_y;
		plnx = pl->cols;
		plny = pl->rows;
		if (px < plx + plnx && px + nx > plx &&
		    py < ply + plny && py + ny > ply)
			kitty_placement_free_one(&s->kitty_placements, pl);
	}
}
