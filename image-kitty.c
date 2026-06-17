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
	KITTY_ACTION_TRANSMIT_FRAME = 'f',
	KITTY_ACTION_CONTROL_ANIMATION = 'a',
	KITTY_ACTION_COMPOSE_FRAME = 'c',
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
	size_t			 file_size;		/* S */
	off_t			 file_offset;		/* O */
	u_int			 cols;			/* c */
	u_int			 rows;			/* r */
	u_int			 src_x;			/* x */
	u_int			 src_y;			/* y */
	u_int			 src_w;			/* w */
	u_int			 src_h;			/* h */
	u_int			 cell_xoff;		/* X */
	u_int			 cell_yoff;		/* Y */
	uint32_t		 parent_image_id;	/* P */
	uint32_t		 parent_placement_id;	/* Q */
	int32_t			 parent_offset_x;	/* H */
	int32_t			 parent_offset_y;	/* V */
	int			 relative;
	int32_t			 zindex;		/* z */
	int			 cursor_no_move;	/* C */
	int			 virtual;		/* U */
	int			 quiet;			/* q */
	int			 delete_action;		/* d */
	int			 more;			/* m */

	u_char			*payload;
	size_t			 payload_len;
};

static void	kitty_command_free(struct kitty_command *);
static void	kitty_image_free_one(struct kitty_images *, struct kitty_image *);
static void	kitty_placement_free_one(struct kitty_placements *,
			    struct kitty_placement *);
static int	kitty_action_is_known(enum kitty_action);
static int	kitty_action_is_unsupported(enum kitty_action);
static int	kitty_decode_direct_payload(struct kitty_command *);
static int	kitty_handle_transmit_file(struct kitty_command *);
static const char *kitty_validate_payload_size(struct kitty_command *);
static int	kitty_format_valid(int);
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

/* Return if this is a known Kitty action. */
static int
kitty_action_is_known(enum kitty_action action)
{
	switch (action) {
	case KITTY_ACTION_QUERY:
	case KITTY_ACTION_TRANSMIT:
	case KITTY_ACTION_TRANSMIT_AND_DISPLAY:
	case KITTY_ACTION_TRANSMIT_FRAME:
	case KITTY_ACTION_CONTROL_ANIMATION:
	case KITTY_ACTION_COMPOSE_FRAME:
	case KITTY_ACTION_PLACE:
	case KITTY_ACTION_DELETE:
		return (1);
	default:
		return (0);
	}
}

/* Return if this is a documented Kitty action not implemented by tmux. */
static int
kitty_action_is_unsupported(enum kitty_action action)
{
	switch (action) {
	case KITTY_ACTION_TRANSMIT_FRAME:
	case KITTY_ACTION_CONTROL_ANIMATION:
	case KITTY_ACTION_COMPOSE_FRAME:
		return (1);
	default:
		return (0);
	}
}

/* Parse a key=value pair from control data. Returns -1 on error, 0 on skip. */
static int
kitty_parse_uint32(long long ll, uint32_t *value)
{
	if (ll < 0 || (uintmax_t)ll > UINT32_MAX)
		return (-1);
	*value = (uint32_t)ll;
	return (0);
}

static int
kitty_parse_u_int(long long ll, u_int *value)
{
	if (ll < 0 || (uintmax_t)ll > UINT_MAX)
		return (-1);
	*value = (u_int)ll;
	return (0);
}

static int
kitty_parse_int(long long ll, int *value)
{
	if (ll < 0 || ll > INT_MAX)
		return (-1);
	*value = (int)ll;
	return (0);
}

static int
kitty_parse_int32(long long ll, int32_t *value)
{
	if (ll < INT32_MIN || ll > INT32_MAX)
		return (-1);
	*value = (int32_t)ll;
	return (0);
}

static int
kitty_parse_size(long long ll, size_t *value)
{
	if (ll < 0 || (uintmax_t)ll > SIZE_MAX)
		return (-1);
	*value = (size_t)ll;
	return (0);
}

static int
kitty_parse_offset(long long ll, off_t *value)
{
	if (ll < 0)
		return (-1);
	*value = (off_t)ll;
	if ((long long)*value != ll)
		return (-1);
	return (0);
}

static int
kitty_parse_key_value(const char *key, const char *value,
    struct kitty_command *cmd)
{
	char		*end;
	int		 iv;
	long long	 ll;

	if (key[0] == '\0' || key[1] != '\0')
		return (0);

	/* Action and transmission type keys are characters, not numbers. */
	if (key[0] == 'a') {
		if (value[0] == '\0' || value[1] != '\0')
			return (-1);
		cmd->action = (enum kitty_action)(value[0]);
		return (0);
	}
	if (key[0] == 't') {
		if (value[0] == '\0' || value[1] != '\0')
			return (-1);
		cmd->transmission = (int)value[0];
		return (0);
	}
	if (key[0] == 'd') {
		if (value[0] == '\0' || value[1] != '\0')
			return (-1);
		cmd->delete_action = (int)value[0];
		return (0);
	}
	if (key[0] == 'o' && value[0] == 'z' && value[1] == '\0') {
		cmd->compression = 1;
		return (0);
	}

	errno = 0;
	ll = strtoll(value, &end, 10);
	if (*end != '\0' || errno == ERANGE)
		return (-1);

	switch (key[0]) {
	case 'i':
		if (kitty_parse_uint32(ll, &cmd->image_id) != 0)
			return (-1);
		break;
	case 'I':
		if (kitty_parse_uint32(ll, &cmd->image_number) != 0)
			return (-1);
		break;
	case 'p':
		if (kitty_parse_uint32(ll, &cmd->placement_id) != 0)
			return (-1);
		break;
	case 'f':
		if (kitty_parse_int(ll, &iv) != 0)
			return (-1);
		cmd->format = (enum kitty_image_format)iv;
		break;
	case 'o':
		if (kitty_parse_int(ll, &cmd->compression) != 0)
			return (-1);
		break;
	case 's':
		if (kitty_parse_u_int(ll, &cmd->pixel_width) != 0)
			return (-1);
		break;
	case 'v':
		if (kitty_parse_u_int(ll, &cmd->pixel_height) != 0)
			return (-1);
		break;
	case 'S':
		if (kitty_parse_size(ll, &cmd->file_size) != 0)
			return (-1);
		break;
	case 'O':
		if (kitty_parse_offset(ll, &cmd->file_offset) != 0)
			return (-1);
		break;
	case 'c':
		if (kitty_parse_u_int(ll, &cmd->cols) != 0)
			return (-1);
		break;
	case 'r':
		if (kitty_parse_u_int(ll, &cmd->rows) != 0)
			return (-1);
		break;
	case 'x':
		if (kitty_parse_u_int(ll, &cmd->src_x) != 0)
			return (-1);
		break;
	case 'y':
		if (kitty_parse_u_int(ll, &cmd->src_y) != 0)
			return (-1);
		break;
	case 'w':
		if (kitty_parse_u_int(ll, &cmd->src_w) != 0)
			return (-1);
		break;
	case 'h':
		if (kitty_parse_u_int(ll, &cmd->src_h) != 0)
			return (-1);
		break;
	case 'X':
		if (kitty_parse_u_int(ll, &cmd->cell_xoff) != 0)
			return (-1);
		break;
	case 'Y':
		if (kitty_parse_u_int(ll, &cmd->cell_yoff) != 0)
			return (-1);
		break;
	case 'P':
		if (kitty_parse_uint32(ll, &cmd->parent_image_id) != 0)
			return (-1);
		cmd->relative = 1;
		break;
	case 'Q':
		if (kitty_parse_uint32(ll, &cmd->parent_placement_id) != 0)
			return (-1);
		cmd->relative = 1;
		break;
	case 'H':
		if (kitty_parse_int32(ll, &cmd->parent_offset_x) != 0)
			return (-1);
		cmd->relative = 1;
		break;
	case 'V':
		if (kitty_parse_int32(ll, &cmd->parent_offset_y) != 0)
			return (-1);
		cmd->relative = 1;
		break;
	case 'z':
		if (kitty_parse_int32(ll, &cmd->zindex) != 0)
			return (-1);
		break;
	case 'C':
		if (kitty_parse_int(ll, &cmd->cursor_no_move) != 0)
			return (-1);
		break;
	case 'U':
		if (kitty_parse_int(ll, &cmd->virtual) != 0)
			return (-1);
		break;
	case 'q':
		if (kitty_parse_int(ll, &cmd->quiet) != 0)
			return (-1);
		break;
	case 'm':
		if (kitty_parse_int(ll, &cmd->more) != 0)
			return (-1);
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
kitty_build_reply_status(__unused struct screen *s, struct kitty_command *cmd,
    const char *status, char **reply)
{
	if (cmd->image_id != 0 && cmd->image_number != 0 &&
	    cmd->placement_id != 0)
		xasprintf(reply, "\033_Gi=%u,I=%u,p=%u;%s\033\\",
		    cmd->image_id, cmd->image_number, cmd->placement_id,
		    status);
	else if (cmd->image_id != 0 && cmd->image_number != 0)
		xasprintf(reply, "\033_Gi=%u,I=%u;%s\033\\",
		    cmd->image_id, cmd->image_number, status);
	else if (cmd->image_id != 0 && cmd->placement_id != 0)
		xasprintf(reply, "\033_Gi=%u,p=%u;%s\033\\",
		    cmd->image_id, cmd->placement_id, status);
	else if (cmd->image_id != 0)
		xasprintf(reply, "\033_Gi=%u;%s\033\\", cmd->image_id,
		    status);
	else if (cmd->image_number != 0 && cmd->placement_id != 0)
		xasprintf(reply, "\033_GI=%u,p=%u;%s\033\\",
		    cmd->image_number, cmd->placement_id, status);
	else if (cmd->image_number != 0)
		xasprintf(reply, "\033_GI=%u;%s\033\\",
		    cmd->image_number, status);
	else
		xasprintf(reply, "\033_Gi=0;%s\033\\", status);
	return (0);
}

/* Build a Kitty OK response string. */
static int
kitty_build_reply(struct screen *s, struct kitty_command *cmd, char **reply)
{
	return (kitty_build_reply_status(s, cmd, "OK", reply));
}

/* Return if a response of this type should be sent. */
static int
kitty_should_reply(struct kitty_command *cmd, int error)
{
	if (!error && cmd->quiet >= 1)
		return (0);
	if (error && cmd->quiet >= 2)
		return (0);
	return (1);
}

/* Build a response unless it is suppressed by the quiet flag. */
static int
kitty_build_reply_if_needed(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	if (cmd->image_id == 0 && cmd->image_number == 0 &&
	    cmd->action != KITTY_ACTION_QUERY) {
		*reply = NULL;
		return (0);
	}
	if (!kitty_should_reply(cmd, 0)) {
		*reply = NULL;
		return (0);
	}
	return (kitty_build_reply(s, cmd, reply));
}

/* Build an error response unless it is suppressed by the quiet flag. */
static void
kitty_build_error_if_needed(struct screen *s, struct kitty_command *cmd,
    const char *error, char **reply)
{
	if (!kitty_should_reply(cmd, 1)) {
		*reply = NULL;
		return;
	}
	kitty_build_reply_status(s, cmd, error, reply);
}

/* Handle query action (a=q). */
static int
kitty_handle_query(struct screen *s, struct kitty_command *cmd, char **reply)
{
	const char	*error;

	if (cmd->image_id == 0) {
		log_debug("kitty: query without image id");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->format == 0)
		cmd->format = KITTY_FORMAT_RGBA;
	if (!kitty_format_valid(cmd->format) ||
	    (cmd->compression != 0 && cmd->compression != 1)) {
		log_debug("kitty: invalid query format or compression");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->transmission != 0 && cmd->transmission != 'd' &&
	    cmd->transmission != 'f' && cmd->transmission != 't' &&
	    cmd->transmission != 's') {
		log_debug("kitty: invalid query transmission medium");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}

	if (cmd->transmission == 'f' || cmd->transmission == 't') {
		if (kitty_handle_transmit_file(cmd) != 0) {
			kitty_build_error_if_needed(s, cmd, "ENOENT", reply);
			return (-1);
		}
	} else if (cmd->transmission == 's') {
		log_debug("kitty: shared memory query not supported");
		kitty_build_error_if_needed(s, cmd, "ENOTSUP", reply);
		return (-1);
	} else if (kitty_decode_direct_payload(cmd) != 0) {
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	error = kitty_validate_payload_size(cmd);
	if (error != NULL) {
		kitty_build_error_if_needed(s, cmd, error, reply);
		return (-1);
	}
	return (kitty_build_reply_if_needed(s, cmd, reply));
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

	tty_kitty_image_remove_uploaded(img);
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

	TAILQ_FOREACH_REVERSE(img, &s->kitty_images, kitty_images, entry) {
		if (img->number == number)
			return (img);
	}
	return (NULL);
}

/* Allocate the next nonzero image id. */
static uint32_t
kitty_image_next_id(struct screen *s)
{
	struct kitty_image	*img;
	uint32_t		 next = 1;

	TAILQ_FOREACH(img, &s->kitty_images, entry) {
		if (img->id >= next)
			next = img->id + 1;
		if (next == 0)
			next = 1;
	}
	while (kitty_image_find(s, next) != NULL) {
		next++;
		if (next == 0)
			next = 1;
	}
	return (next);
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

/* Next terminal-side image ID. */
static uint32_t kitty_next_tty_id = 0x80000000U;

/* Allocate a terminal-side image ID. */
static uint32_t
kitty_image_next_tty_id(void)
{
	uint32_t	id;

	id = kitty_next_tty_id++;
	if (kitty_next_tty_id == 0)
		kitty_next_tty_id = 0x80000000U;
	return (id);
}

/* Read a file into memory. */
static u_char *
kitty_read_file(const char *path, off_t offset, size_t size, size_t *len)
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
	if (offset < 0 || offset > sb.st_size)
		return (NULL);
	if (size == 0)
		size = (size_t)(sb.st_size - offset);
	if (size > (size_t)(sb.st_size - offset))
		return (NULL);
	if (size > KITTY_MAX_IMAGE_PAYLOAD) {
		log_debug("kitty: file too large: %s", path);
		return (NULL);
	}
	if (size == 0)
		return (NULL);

	f = fopen(path, "rb");
	if (f == NULL)
		return (NULL);
	if (fseeko(f, offset, SEEK_SET) != 0) {
		fclose(f);
		return (NULL);
	}

	buf = xmalloc(size);
	n = fread(buf, 1, size, f);
	fclose(f);

	if (n != size) {
		free(buf);
		return (NULL);
	}

	*len = n;
	return (buf);
}

/* Decode a base64 encoded string payload. */
static char *
kitty_decode_string_payload(struct kitty_command *cmd)
{
	u_char	*decoded;
	char	*encoded;
	int	 outlen;

	if (cmd->payload_len == 0)
		return (NULL);

	encoded = xstrndup((const char *)cmd->payload, cmd->payload_len);
	decoded = xmalloc(cmd->payload_len + 1);
	outlen = b64_pton(encoded, decoded, cmd->payload_len);
	free(encoded);

	if (outlen == -1 || memchr(decoded, '\0', outlen) != NULL) {
		free(decoded);
		return (NULL);
	}
	decoded[outlen] = '\0';
	return ((char *)decoded);
}

/* Return if a temporary path may be removed after reading. */
static int
kitty_temp_path_allowed(const char *path)
{
	const char	*base, *tmpdir;
	size_t		 len;

	base = strrchr(path, '/');
	if (base == NULL)
		return (0);
	base++;
	if (strstr(base, "tty-graphics-protocol") == NULL)
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

/* Validate uncompressed raw RGB/RGBA payload size. */
static const char *
kitty_validate_payload_size(struct kitty_command *cmd)
{
	size_t	pixels, required, bytes;

	if (cmd->compression != 0)
		return (NULL);
	if (cmd->format != KITTY_FORMAT_RGB &&
	    cmd->format != KITTY_FORMAT_RGBA)
		return (NULL);
	if (cmd->pixel_width == 0 || cmd->pixel_height == 0)
		return ("EINVAL");
	if (cmd->pixel_width > SIZE_MAX / cmd->pixel_height)
		return ("EINVAL");
	pixels = (size_t)cmd->pixel_width * cmd->pixel_height;
	bytes = cmd->format / 8;
	if (pixels > SIZE_MAX / bytes)
		return ("EINVAL");
	required = pixels * bytes;
	if (cmd->payload_len < required)
		return ("ENODATA");
	if (cmd->payload_len > required)
		return ("EINVAL");
	return (NULL);
}

/* Return if an image format is supported. */
static int
kitty_format_valid(int format)
{
	return (format == KITTY_FORMAT_RGB ||
	    format == KITTY_FORMAT_RGBA ||
	    format == KITTY_FORMAT_PNG);
}

/* Check memory limits before storing. */
static int
kitty_image_check_limits(struct screen *s, size_t new_payload,
    struct kitty_image *old)
{
	struct kitty_image	*img;
	size_t			 total = 0;
	u_int			 count = 0;

	if (new_payload > KITTY_MAX_IMAGE_PAYLOAD) {
		log_debug("kitty: payload too large: %zu", new_payload);
		return (-1);
	}

	TAILQ_FOREACH(img, &s->kitty_images, entry) {
		if (img == old)
			continue;
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

	if (kitty_image_check_limits(s, cmd->payload_len, NULL) != 0)
		return (NULL);

	img = xcalloc(1, sizeof *img);
	img->id = cmd->image_id;
	img->number = cmd->image_number;
	if (img->id != 0)
		img->tty_id = kitty_image_next_tty_id();
	img->generation = 1;
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

	if (kitty_image_check_limits(s, cmd->payload_len, old) != 0)
		return (NULL);

	img = xcalloc(1, sizeof *img);
	img->id = cmd->image_id;
	img->number = cmd->image_number;
	img->tty_id = old->tty_id;
	img->generation = old->generation + 1;
	if (img->generation == 0)
		img->generation = 1;
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

/* Free Kitty image and placement lists. */
void
kitty_image_free_lists(struct kitty_images *images,
    struct kitty_placements *placements)
{
	struct kitty_image	*img, *img_next;
	struct kitty_placement	*pl, *pl_next;

	/* Placements hold references to images, so free them before images. */
	TAILQ_FOREACH_SAFE(pl, placements, entry, pl_next)
		kitty_placement_free_one(placements, pl);

	TAILQ_FOREACH_SAFE(img, images, entry, img_next)
		kitty_image_free_one(images, img);
}

/* Free all Kitty images and placements for a screen. */
void
kitty_image_free_all(struct screen *s)
{
	kitty_image_free_lists(&s->kitty_images, &s->kitty_placements);

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

	if (cmd.image_id != 0 && cmd.image_number != 0) {
		kitty_build_error_if_needed(s, &cmd, "EINVAL", reply);
		result = -1;
		goto done;
	}
	if (cmd.action == 0)
		cmd.action = KITTY_ACTION_TRANSMIT;
	if (cmd.format == 0)
		cmd.format = KITTY_FORMAT_RGBA;
	if (cmd.more == 1 && cmd.transmission != 0 &&
	    cmd.transmission != 'd')
		cmd.more = 0;
	if (cmd.more == 1 && (!kitty_action_is_known(cmd.action) ||
	    kitty_action_is_unsupported(cmd.action))) {
		result = kitty_image_dispatch(s, &cmd, reply);
		goto done;
	}

	/* Handle chunked uploads. */
	if (s->kitty_pending.active) {
		if (cmd.action == KITTY_ACTION_DELETE) {
			free(s->kitty_pending.payload);
			memset(&s->kitty_pending, 0, sizeof s->kitty_pending);
			result = kitty_image_dispatch(s, &cmd, reply);
			goto done;
		}

		/* Continuing a chunked upload. */
		if (cmd.payload_len > 0) {
			if (kitty_pending_append(s, cmd.payload, cmd.payload_len)
			    != 0) {
				result = -1;
				goto done;
			}
		}
		if (cmd.quiet >= 1)
			s->kitty_pending.quiet = cmd.quiet;
		if (cmd.more == 1) {
			/* More chunks coming. */
			result = 0;
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
		cmd.file_size = s->kitty_pending.file_size;
		cmd.file_offset = s->kitty_pending.file_offset;
		cmd.cols = s->kitty_pending.cols;
		cmd.rows = s->kitty_pending.rows;
		cmd.src_x = s->kitty_pending.src_x;
		cmd.src_y = s->kitty_pending.src_y;
		cmd.src_w = s->kitty_pending.src_w;
		cmd.src_h = s->kitty_pending.src_h;
		cmd.cell_xoff = s->kitty_pending.cell_xoff;
		cmd.cell_yoff = s->kitty_pending.cell_yoff;
		cmd.parent_image_id = s->kitty_pending.parent_image_id;
		cmd.parent_placement_id =
		    s->kitty_pending.parent_placement_id;
		cmd.parent_offset_x = s->kitty_pending.parent_offset_x;
		cmd.parent_offset_y = s->kitty_pending.parent_offset_y;
		cmd.relative = s->kitty_pending.relative;
		cmd.zindex = s->kitty_pending.zindex;
		cmd.cursor_no_move = s->kitty_pending.cursor_no_move;
		cmd.virtual = s->kitty_pending.virtual;
		cmd.quiet = s->kitty_pending.quiet;
		cmd.delete_action = s->kitty_pending.delete_action;
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
		s->kitty_pending.file_size = cmd.file_size;
		s->kitty_pending.file_offset = cmd.file_offset;
		s->kitty_pending.cols = cmd.cols;
		s->kitty_pending.rows = cmd.rows;
		s->kitty_pending.src_x = cmd.src_x;
		s->kitty_pending.src_y = cmd.src_y;
		s->kitty_pending.src_w = cmd.src_w;
		s->kitty_pending.src_h = cmd.src_h;
		s->kitty_pending.cell_xoff = cmd.cell_xoff;
		s->kitty_pending.cell_yoff = cmd.cell_yoff;
		s->kitty_pending.parent_image_id = cmd.parent_image_id;
		s->kitty_pending.parent_placement_id =
		    cmd.parent_placement_id;
		s->kitty_pending.parent_offset_x = cmd.parent_offset_x;
		s->kitty_pending.parent_offset_y = cmd.parent_offset_y;
		s->kitty_pending.relative = cmd.relative;
		s->kitty_pending.zindex = cmd.zindex;
		s->kitty_pending.cursor_no_move = cmd.cursor_no_move;
		s->kitty_pending.virtual = cmd.virtual;
		s->kitty_pending.quiet = cmd.quiet;
		s->kitty_pending.delete_action = cmd.delete_action;
		if (cmd.payload_len > 0) {
			if (kitty_pending_append(s, cmd.payload, cmd.payload_len)
			    != 0) {
				result = -1;
				goto done;
			}
		}
		result = 0;
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

	encoded = xstrndup((const char *)cmd->payload, cmd->payload_len);
	decoded = xmalloc(cmd->payload_len);
	outlen = b64_pton(encoded, decoded, cmd->payload_len);
	free(encoded);

	if (outlen == -1) {
		free(decoded);
		return (-1);
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

	path = kitty_decode_string_payload(cmd);
	if (path == NULL)
		return (-1);
	buf = kitty_read_file(path, cmd->file_offset, cmd->file_size, &len);
	if (buf == NULL) {
		free(path);
		return (-1);
	}

	free(cmd->payload);
	cmd->payload = buf;
	cmd->payload_len = len;

	/* For temporary files, try to delete after reading. */
	if (cmd->transmission == 't' && kitty_temp_path_allowed(path))
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
	struct kitty_placement	*pl;
	const char		*error;
	int			 anonymous;

	if (cmd->format == 0)
		cmd->format = KITTY_FORMAT_RGBA;

	anonymous = (cmd->image_id == 0 && cmd->image_number == 0);
	if (cmd->image_id == 0 && cmd->image_number == 0) {
		if (cmd->action != KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
			log_debug("kitty: transmit without image id or number");
			kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
			return (-1);
		}
	}
	if (cmd->image_id != 0 && cmd->image_number != 0) {
		log_debug("kitty: transmit with both id and number");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->relative) {
		log_debug("kitty: relative placements not supported");
		kitty_build_error_if_needed(s, cmd, "ENOTSUP", reply);
		return (-1);
	}
	if (!kitty_format_valid(cmd->format) ||
	    (cmd->compression != 0 && cmd->compression != 1)) {
		log_debug("kitty: invalid format or compression");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->transmission != 0 && cmd->transmission != 'd' &&
	    cmd->transmission != 'f' && cmd->transmission != 't' &&
	    cmd->transmission != 's') {
		log_debug("kitty: invalid transmission medium");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}

	/* Handle file transmission if needed. */
	if (cmd->transmission == 'f' || cmd->transmission == 't') {
		if (kitty_handle_transmit_file(cmd) != 0) {
			kitty_build_error_if_needed(s, cmd, "ENOENT", reply);
			return (-1);
		}
	} else if (cmd->transmission == 's') {
		log_debug("kitty: shared memory not supported");
		kitty_build_error_if_needed(s, cmd, "ENOTSUP", reply);
		return (-1);
	} else if (kitty_decode_direct_payload(cmd) != 0) {
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	error = kitty_validate_payload_size(cmd);
	if (error != NULL) {
		kitty_build_error_if_needed(s, cmd, error, reply);
		return (-1);
	}

	if (cmd->image_id != 0) {
		img = kitty_image_find(s, cmd->image_id);
		if (img != NULL)
			img = kitty_image_replace(s, img, cmd);
		else
			img = kitty_image_store(s, cmd);
	} else {
		if (cmd->image_number != 0)
			cmd->image_id = kitty_image_next_id(s);
		img = kitty_image_store(s, cmd);
	}

	if (img == NULL) {
		kitty_build_error_if_needed(s, cmd, "ENOMEM", reply);
		return (-1);
	}

	/* For a=T, also create a placement. */
	if (cmd->action == KITTY_ACTION_TRANSMIT_AND_DISPLAY) {
		if (cmd->virtual != 1) {
			pl = kitty_placement_create(s, img, cmd);
			if (pl == NULL) {
				kitty_build_error_if_needed(s, cmd, "ENOMEM",
				    reply);
				return (-1);
			}
			if (pl != NULL && !cmd->cursor_no_move)
				kitty_advance_cursor(s, pl);
		}
	}

	if (anonymous) {
		*reply = NULL;
		return (0);
	}
	return (kitty_build_reply_if_needed(s, cmd, reply));
}

/* Handle place action (a=p). */
static int
kitty_handle_place(struct screen *s, struct kitty_command *cmd,
    char **reply)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl;

	if (cmd->image_id != 0 && cmd->image_number != 0) {
		log_debug("kitty: place with both id and number");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->image_id == 0 && cmd->image_number == 0) {
		log_debug("kitty: place without image id or number");
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		return (-1);
	}
	if (cmd->relative) {
		log_debug("kitty: relative placements not supported");
		kitty_build_error_if_needed(s, cmd, "ENOTSUP", reply);
		return (-1);
	}

	if (cmd->image_id != 0)
		img = kitty_image_find(s, cmd->image_id);
	else
		img = kitty_image_find_by_number(s, cmd->image_number);
	if (img == NULL) {
		log_debug("kitty: place image not found: %u/%u",
		    cmd->image_id, cmd->image_number);
		kitty_build_error_if_needed(s, cmd, "ENOENT", reply);
		return (-1);
	}
	cmd->image_id = img->id;

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
			return (kitty_build_reply_if_needed(s, cmd, reply));
		}
	}

	if (cmd->virtual != 1) {
		pl = kitty_placement_create(s, img, cmd);
		if (pl == NULL) {
			kitty_build_error_if_needed(s, cmd, "ENOMEM", reply);
			return (-1);
		}
		if (pl != NULL && !cmd->cursor_no_move)
			kitty_advance_cursor(s, pl);
	}

	return (kitty_build_reply_if_needed(s, cmd, reply));
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

/* Delete one placement for an image. */
static int
kitty_delete_placement(struct screen *s, struct kitty_image *img,
    uint32_t placement_id)
{
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->image == img && pl->placement_id == placement_id) {
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			break;
		}
	}
	return (found);
}

/* Return if an image has any placements. */
static int
kitty_image_has_placements(struct screen *s, struct kitty_image *img)
{
	struct kitty_placement	*pl;

	TAILQ_FOREACH(pl, &s->kitty_placements, entry) {
		if (pl->image == img)
			return (1);
	}
	return (0);
}

/* Delete image data when it no longer has placements. */
static int
kitty_delete_image_if_unplaced(struct screen *s, struct kitty_image *img)
{
	if (img == NULL || kitty_image_has_placements(s, img))
		return (0);
	kitty_image_free_one(&s->kitty_images, img);
	return (1);
}

/* Delete image data that no longer has placements. */
static int
kitty_delete_unplaced_images(struct screen *s, int anonymous_only)
{
	struct kitty_image	*img, *img_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(img, &s->kitty_images, entry, img_next) {
		if (anonymous_only && img->id != 0)
			continue;
		if (!kitty_image_has_placements(s, img)) {
			kitty_image_free_one(&s->kitty_images, img);
			found = 1;
		}
	}
	return (found);
}

/* Return if a placement intersects a zero-based cell. */
static int
kitty_placement_intersects_cell(struct kitty_placement *pl, u_int x, u_int y)
{
	if (pl->cols == 0 || pl->rows == 0)
		return (0);
	if (pl->pane_x < 0 || pl->pane_y < 0)
		return (0);
	if ((u_int)pl->pane_x <= x && x < (u_int)pl->pane_x + pl->cols &&
	    (u_int)pl->pane_y <= y && y < (u_int)pl->pane_y + pl->rows)
		return (1);
	return (0);
}

/* Delete placements that intersect a cell, optionally matching z-index. */
static int
kitty_delete_cell(struct screen *s, u_int x, u_int y, int have_z,
    int32_t zindex, int delete_data)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	if (x == 0 || y == 0)
		return (0);
	x--;
	y--;
	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (have_z && pl->zindex != zindex)
			continue;
		if (kitty_placement_intersects_cell(pl, x, y)) {
			img = pl->image;
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			if (delete_data)
				found |= kitty_delete_image_if_unplaced(s, img);
		}
	}
	return (found);
}

/* Delete placements that intersect a one-based column. */
static int
kitty_delete_column(struct screen *s, u_int x, int delete_data)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	if (x == 0)
		return (0);
	x--;
	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->cols == 0 || pl->pane_x < 0)
			continue;
		if ((u_int)pl->pane_x <= x &&
		    x < (u_int)pl->pane_x + pl->cols) {
			img = pl->image;
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			if (delete_data)
				found |= kitty_delete_image_if_unplaced(s, img);
		}
	}
	return (found);
}

/* Delete placements that intersect a one-based row. */
static int
kitty_delete_row(struct screen *s, u_int y, int delete_data)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	if (y == 0)
		return (0);
	y--;
	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->rows == 0 || pl->pane_y < 0)
			continue;
		if ((u_int)pl->pane_y <= y &&
		    y < (u_int)pl->pane_y + pl->rows) {
			img = pl->image;
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			if (delete_data)
				found |= kitty_delete_image_if_unplaced(s, img);
		}
	}
	return (found);
}

/* Delete images whose ids fall in an inclusive range. */
static int
kitty_delete_id_range(struct screen *s, uint32_t first, uint32_t last,
    int delete_data)
{
	struct kitty_image	*img, *img_next;
	int			 found = 0;

	if (first > last)
		return (0);

	TAILQ_FOREACH_SAFE(img, &s->kitty_images, entry, img_next) {
		if (img->id < first || img->id > last)
			continue;
		if (delete_data) {
			found |= kitty_delete_image(s, img->id, 1);
			continue;
		}
		found |= kitty_delete_image_placements(s, img->id);
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

/* Delete placement at cursor position. */
static int
kitty_delete_cursor(struct screen *s, int delete_data)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->pane_x <= (int)s->cx &&
		    (int)s->cx < pl->pane_x + (int)pl->cols &&
		    pl->pane_y <= (int)s->cy &&
		    (int)s->cy < pl->pane_y + (int)pl->rows) {
			img = pl->image;
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			if (delete_data)
				found |= kitty_delete_image_if_unplaced(s, img);
		}
	}
	return (found);
}

/* Delete placements by z-index. */
static int
kitty_delete_zindex(struct screen *s, int32_t zindex, int delete_data)
{
	struct kitty_image	*img;
	struct kitty_placement	*pl, *pl_next;
	int			 found = 0;

	TAILQ_FOREACH_SAFE(pl, &s->kitty_placements, entry, pl_next) {
		if (pl->zindex == zindex) {
			img = pl->image;
			kitty_placement_free_one(&s->kitty_placements, pl);
			found = 1;
			if (delete_data)
				found |= kitty_delete_image_if_unplaced(s, img);
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

	/* Default is 'a' (all visible placements). */
	if (cmd->delete_action == 0)
		delete_spec = 'a';
	else
		delete_spec = (char)cmd->delete_action;

	/* Uppercase specifiers also delete image data. */
	if (delete_spec >= 'A' && delete_spec <= 'Z') {
		delete_data = 1;
		delete_spec = delete_spec - 'A' + 'a';
	}

	switch (delete_spec) {
	case 'a':
		found = kitty_delete_all_placements(s);
		found |= kitty_delete_unplaced_images(s, !delete_data);
		break;
	case 'i':
		if (cmd->image_id != 0) {
			struct kitty_image	*img;

			img = kitty_image_find(s, cmd->image_id);
			if (img != NULL && cmd->placement_id != 0)
				found = kitty_delete_placement(s, img,
				    cmd->placement_id);
			else
				found = kitty_delete_image_placements(s,
				    cmd->image_id);
			if (delete_data && img != NULL)
				found |= kitty_delete_image_if_unplaced(s, img);
		} else if (cmd->image_number != 0) {
			struct kitty_image	*img;

			img = kitty_image_find_by_number(s, cmd->image_number);
			if (img != NULL) {
				if (cmd->placement_id != 0)
					found = kitty_delete_placement(s, img,
					    cmd->placement_id);
				else
					found = kitty_delete_image_placements(s,
					    img->id);
				if (delete_data)
					found |= kitty_delete_image_if_unplaced(s,
					    img);
			}
		}
		break;
	case 'n':
		if (cmd->image_number != 0) {
			struct kitty_image	*img;

			img = kitty_image_find_by_number(s, cmd->image_number);
			if (img != NULL) {
				if (cmd->placement_id != 0)
					found = kitty_delete_placement(s, img,
					    cmd->placement_id);
				else
					found = kitty_delete_image_placements(s,
					    img->id);
				if (delete_data)
					found |= kitty_delete_image_if_unplaced(s,
					    img);
			}
		}
		break;
	case 'c':
		found = kitty_delete_cursor(s, delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'p':
		found = kitty_delete_cell(s, cmd->src_x, cmd->src_y, 0, 0,
		    delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'q':
		found = kitty_delete_cell(s, cmd->src_x, cmd->src_y, 1,
		    cmd->zindex, delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'r':
		found = kitty_delete_id_range(s, cmd->src_x, cmd->src_y,
		    delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'x':
		found = kitty_delete_column(s, cmd->src_x, delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'y':
		found = kitty_delete_row(s, cmd->src_y, delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'z':
		found = kitty_delete_zindex(s, cmd->zindex, delete_data);
		if (!delete_data)
			found |= kitty_delete_unplaced_images(s, 1);
		break;
	case 'f':
		/* Animation frames are not supported. */
		break;
	default:
		log_debug("kitty: unknown delete specifier: %c", delete_spec);
		break;
	}

	log_debug("kitty: delete %c, found=%d", delete_spec, found);

	return (kitty_build_reply_if_needed(s, cmd, reply));
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
	case KITTY_ACTION_TRANSMIT_FRAME:
	case KITTY_ACTION_CONTROL_ANIMATION:
	case KITTY_ACTION_COMPOSE_FRAME:
		log_debug("kitty: unsupported action: %c",
		    (char)cmd->action);
		kitty_build_error_if_needed(s, cmd, "ENOTSUP", reply);
		result = -1;
		break;
	case KITTY_ACTION_PLACE:
		result = kitty_handle_place(s, cmd, reply);
		break;
	case KITTY_ACTION_DELETE:
		result = kitty_handle_delete(s, cmd, reply);
		break;
	default:
		log_debug("kitty: unknown action: %c", (char)cmd->action);
		kitty_build_error_if_needed(s, cmd, "EINVAL", reply);
		result = -1;
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
	kitty_delete_unplaced_images(s, 1);
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
	kitty_delete_unplaced_images(s, 1);
}
