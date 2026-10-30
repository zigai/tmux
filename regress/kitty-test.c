/* $OpenBSD$ */

/*
 * Kitty graphics protocol unit tests.
 *
 * This file includes image-kitty.c directly to test static functions and is
 * linked with the rest of the tmux objects. Each test feeds Kitty graphics
 * sequences to a pane and checks the replies, the stored images and the
 * placements in the pane's grid.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event.h>

#include "tmux.h"

/* Pull in the implementation. */
#include "image-kitty.c"

static int	 tests_run;
static int	 tests_failed;

#define TEST_ASSERT(name, cond) do {					\
	tests_run++;							\
	if (!(cond)) {							\
		printf("FAIL: %s\n", name);				\
		tests_failed++;						\
		test_pane_free();					\
		return (1);						\
	}								\
} while (0)

#define TEST_PASS(name) do {						\
	printf("PASS: %s\n", name);					\
	test_pane_free();						\
} while (0)

static struct event_base	*test_base;
static struct window		*test_window;
static struct window_pane	*test_wp;
static int			 test_fd = -1;
static char			 test_reply[4096];

/* A 1x1 red RGBA pixel. */
#define TEST_PIXEL "/wAA/w=="

/* A 1x1 red RGB pixel. */
#define TEST_RGB "/wAA"

/* A 1x1 green RGBA pixel. */
#define TEST_GREEN "AP8A/w=="

/* A 1x1 red RGBA PNG. */
#define TEST_PNG "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4n" \
	"GP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="

/* A 1x1 red RGB pixel compressed with zlib. */
#define TEST_ZLIB "eJz7z8AAAAMAAQA="

/* Red and green RGBA pixels. */
static const u_char	 test_red[4] = { 0xff, 0, 0, 0xff };
static const u_char	 test_green[4] = { 0, 0xff, 0, 0xff };

/* Create a pane with an 80x24 screen and 8x16 pixel cells. */
static void
test_pane_new(void)
{
	int	fds[2];

	test_window = window_create(80, 24, 8, 16);
	test_wp = window_add_pane(test_window, NULL, 0, 0);
	test_wp->ictx = input_init(test_wp, NULL);
	window_add_ref(test_window, __func__);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
		abort();
	setblocking(fds[1], 0);
	test_wp->fd = fds[0];
	test_fd = fds[1];
	test_wp->event = bufferevent_new(test_wp->fd, NULL, NULL, NULL, NULL);
	bufferevent_enable(test_wp->event, EV_WRITE);
}

/* Free the test pane and restore the image limits. */
static void
test_pane_free(void)
{
	const struct options_table_entry	*oe;

	for (oe = options_table; oe->name != NULL; oe++) {
		if (strncmp(oe->name, "image-", 6) == 0)
			options_default(global_options, oe);
	}
	if (test_window == NULL)
		return;
	window_remove_ref(test_window, __func__);
	close(test_fd);
	test_fd = -1;
	test_window = NULL;
	test_wp = NULL;
}

/* Feed bytes to the pane and return any replies. */
static const char *
test_input(const char *data)
{
	ssize_t	n;

	input_parse_buffer(test_wp, (u_char *)data, strlen(data));
	event_base_loop(test_base, EVLOOP_NONBLOCK);
	n = read(test_fd, test_reply, sizeof test_reply - 1);
	if (n < 0)
		n = 0;
	test_reply[n] = '\0';
	return (test_reply);
}

/* Feed a Kitty graphics command with a base64 payload. */
static const char *
test_graphics(const char *control, const char *payload)
{
	char		*data;
	const char	*reply;

	if (payload != NULL)
		xasprintf(&data, "\033_G%s;%s\033\\", control, payload);
	else
		xasprintf(&data, "\033_G%s\033\\", control);
	reply = test_input(data);
	free(data);
	return (reply);
}

/* Check that a reply is one Kitty graphics response. */
static int
test_replied(const char *reply, const char *expected)
{
	char	*s;
	int	 same;

	xasprintf(&s, "\033_G%s\033\\", expected);
	same = (strcmp(reply, s) == 0);
	free(s);
	return (same);
}

/* Move the cursor to a cell. */
static void
test_cursor(u_int x, u_int y)
{
	char	buf[32];

	xsnprintf(buf, sizeof buf, "\033[%u;%uH", y + 1, x + 1);
	test_input(buf);
}

/* Keep scrolled lines in the pane's history. */
static void
test_history(u_int limit)
{
	struct grid	*gd = test_wp->base.grid;

	gd->flags |= GRID_HISTORY;
	gd->hlimit = limit;
}

/* Parse control data into a command. */
static int
test_parse(struct kitty_command *cmd, const char *control, const char **m)
{
	kitty_command_reset(cmd);
	return (kitty_parse_control(cmd, (const u_char *)control,
	    strlen(control), m));
}

/* Store a 1x1 image without a reply. */
static void
test_store(u_int id)
{
	char	*control;

	xasprintf(&control, "a=t,q=2,f=32,s=1,v=1,i=%u", id);
	test_graphics(control, TEST_PIXEL);
	free(control);
}

/* Place a stored image at a cell without moving the cursor. */
static const char *
test_place(u_int x, u_int y, const char *control)
{
	char		*s;
	const char	*reply;

	test_cursor(x, y);
	xasprintf(&s, "a=p,C=1,%s", control);
	reply = test_graphics(s, NULL);
	free(s);
	return (reply);
}

/* Return the parser state of the test pane. */
static struct kitty_state *
test_state(void)
{
	return (TAILQ_LAST(&kitty_states, kitty_states));
}

/* Check that no transfer is in progress. */
static int
test_idle(void)
{
	struct kitty_state	*ks = test_state();

	if (ks == NULL)
		return (1);
	return (!ks->cmd.more && ks->data == NULL && ks->size == 0);
}

/* Count the stored images of the test pane. */
static u_int
test_count_images(void)
{
	struct kitty_state	*ks = test_state();
	struct kitty_image_map	*map;
	u_int			 n = 0;

	if (ks == NULL)
		return (0);
	TAILQ_FOREACH(map, &ks->images, entry)
		n++;
	return (n);
}

/* Find a stored image by Kitty image ID. */
static struct kitty_image_map *
test_map(u_int id)
{
	struct kitty_state	*ks = test_state();

	if (ks == NULL)
		return (NULL);
	return (kitty_image_map_find(ks, id));
}

/* Find the newest stored image with a Kitty image number. */
static struct kitty_image_map *
test_map_number(u_int number)
{
	struct kitty_state	*ks = test_state();

	if (ks == NULL)
		return (NULL);
	return (kitty_image_map_find_number(ks, number));
}

/* Return the source image stored for a Kitty image ID. */
static struct image *
test_image(u_int id)
{
	struct kitty_image_map	*map = test_map(id);

	if (map == NULL)
		return (NULL);
	return (image_find(map->image_id));
}

/* Check whether an image is placed in the pane's current or saved grid. */
static int
test_in_grid(u_int image_id)
{
	struct screen	*s = &test_wp->base;

	if (image_grid_has_image(s->grid, image_id))
		return (1);
	if (s->saved_grid == NULL)
		return (0);
	return (image_grid_has_image(s->saved_grid, image_id));
}

/* Find a placement of a Kitty image which is still in the grid. */
static struct kitty_placement_map *
test_placement(u_int id, u_int placement_id)
{
	struct kitty_image_map		*map = test_map(id);
	struct kitty_placement_map	*kpl;

	if (map == NULL)
		return (NULL);
	TAILQ_FOREACH(kpl, &map->placements, entry) {
		if (kpl->kitty_id == placement_id &&
		    test_in_grid(kpl->image_id))
			return (kpl);
	}
	return (NULL);
}

/* Count the placements of a Kitty image which are still in the grid. */
static u_int
test_count_placements(u_int id)
{
	struct kitty_image_map		*map = test_map(id);
	struct kitty_placement_map	*kpl;
	u_int				 n = 0;

	if (map == NULL)
		return (0);
	TAILQ_FOREACH(kpl, &map->placements, entry) {
		if (test_in_grid(kpl->image_id))
			n++;
	}
	return (n);
}

/* Check whether the pane has an image placement at a visible cell. */
static int
test_placed_at(u_int x, u_int y)
{
	struct grid	*gd = test_wp->base.grid;
	struct image	*im;
	u_int		 sx, sy;

	return (image_grid_get_at(gd, x, gd->hsize + y, &im, &sx, &sy) == 1);
}

/* Return the ID of the image drawn at a visible cell, or 0. */
static u_int
test_image_at(u_int x, u_int y)
{
	struct grid	*gd = test_wp->base.grid;
	struct image	*im;
	u_int		 sx, sy;

	if (image_grid_get_at(gd, x, gd->hsize + y, &im, &sx, &sy) != 1)
		return (0);
	return (image_get_id(im));
}

/* Check whether a placement is drawn at a visible cell. */
static int
test_placement_at(u_int id, u_int placement_id, u_int x, u_int y)
{
	struct kitty_placement_map	*kpl;

	kpl = test_placement(id, placement_id);
	return (kpl != NULL && test_image_at(x, y) == kpl->image_id);
}

/* Return the z-index of the image drawn at a visible cell. */
static int32_t
test_zindex_at(u_int x, u_int y)
{
	struct grid	*gd = test_wp->base.grid;
	struct image	*im;
	u_int		 sx, sy;

	if (image_grid_get_at(gd, x, gd->hsize + y, &im, &sx, &sy) != 1)
		return (INT32_MIN);
	return (image_get_zindex(im));
}

/* Check the size of a stored image in pixels. */
static int
test_size(u_int id, u_int w, u_int h)
{
	struct image		*im = test_image(id);
	struct image_geom	*ig;

	if (im == NULL)
		return (0);
	ig = image_get_geometry(im);
	return (ig->src_w == w && ig->src_h == h);
}

/* Check one RGBA pixel of a stored image. */
static int
test_pixel(u_int id, u_int x, u_int y, const u_char *rgba)
{
	struct image		*im = test_image(id);
	struct image_geom	*ig;
	u_char			*pixels;
	size_t			 stride;

	if (im == NULL)
		return (0);
	ig = image_get_geometry(im);
	if (x >= ig->src_w || y >= ig->src_h)
		return (0);
	pixels = image_get_pixels(im, &stride, NULL);
	return (memcmp(pixels + y * stride + x * 4, rgba, 4) == 0);
}

/* Create a temporary file from a mkstemp template and write data to it. */
static int
test_write_file(char *path, const void *data, size_t size)
{
	ssize_t	n;
	int	fd;

	fd = mkstemp(path);
	if (fd == -1)
		return (-1);
	n = write(fd, data, size);
	close(fd);
	if (n != (ssize_t)size) {
		unlink(path);
		return (-1);
	}
	return (0);
}

/* Encode data as a base64 payload. */
static const char *
test_base64(const void *data, size_t size)
{
	static char	encoded[4096];

	if (b64_ntop(data, size, encoded, sizeof encoded) == -1)
		abort();
	return (encoded);
}

/* Build a base64 payload for an RGBA image filled with one pixel. */
static const char *
test_fill(u_int w, u_int h, const u_char *rgba)
{
	u_char		 data[1024];
	u_int		 i;

	if (w * h * 4 > sizeof data)
		abort();
	for (i = 0; i < w * h; i++)
		memcpy(data + i * 4, rgba, 4);
	return (test_base64(data, w * h * 4));
}

/* Find a virtual placement of a Kitty image. */
static struct kitty_placement_map *
test_virtual(u_int id, u_int placement_id)
{
	struct kitty_image_map		*map = test_map(id);
	struct kitty_placement_map	*kpl;

	if (map == NULL)
		return (NULL);
	TAILQ_FOREACH(kpl, &map->placements, entry) {
		if ((kpl->flags & KITTY_PLACEMENT_VIRTUAL) &&
		    kpl->kitty_id == placement_id)
			return (kpl);
	}
	return (NULL);
}

/* Build a placeholder character with row, column and high diacritics. */
static const char *
test_placeholder(char *s, size_t size, int row, int col, int high)
{
	struct utf8_data	ud;
	int			values[] = { row, col, high };
	u_int			i;

	memcpy(ud.data, KITTY_PLACEHOLDER, 4);
	ud.size = 4;
	for (i = 0; i < nitems(values) && values[i] != -1; i++)
		kitty_append_utf8(&ud, kitty_diacritics[values[i]]);
	if ((size_t)ud.size >= size)
		abort();
	memcpy(s, ud.data, ud.size);
	s[ud.size] = '\0';
	return (s);
}

/* Check whether a visible cell shows any image through a placeholder. */
static int
test_resolves_any(u_int x, u_int y)
{
	struct grid		*gd = test_wp->base.grid;
	struct grid_cell	 gc;
	struct image		*im;
	u_int			 col, row;

	grid_view_get_cell(gd, x, y, &gc);
	if (!kitty_is_placeholder(&gc.data))
		return (0);
	return (kitty_placeholder_get(test_wp, NULL, gd, x, gd->hsize + y, &gc,
	    &im, &col, &row));
}

/* Check the image, placement and cell shown by a placeholder cell. */
static int
test_resolves(u_int x, u_int y, u_int id, u_int placement_id, u_int col,
    u_int row)
{
	struct grid			*gd = test_wp->base.grid;
	struct kitty_placement_map	*kpl = test_virtual(id, placement_id);
	struct grid_cell		 gc;
	struct image			*im;
	u_int				 c, r;

	if (kpl == NULL)
		return (0);
	grid_view_get_cell(gd, x, y, &gc);
	if (!kitty_placeholder_get(test_wp, NULL, gd, x, gd->hsize + y, &gc,
	    &im, &c, &r))
		return (0);
	return (image_get_id(im) == kpl->image_id && c == col && r == row);
}

/* Parse a query action. */
static int
test_parse_query(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	test_pane_new();
	kitty_command_reset(&cmd);
	TEST_ASSERT("parse_query: ret == 0",
	    kitty_parse_control(&cmd, "a=q,i=1", 7, &m) == 0);
	TEST_ASSERT("parse_query: action == q",
	    cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_query: image_id == 1", cmd.image_id == 1);
	TEST_PASS("parse_query");
	return (0);
}

/* Parse a transmit action with all fields. */
static int
test_parse_transmit(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_transmit: ret == 0", test_parse(&cmd,
	    "a=T,i=5,f=100,s=80,v=40,c=10,r=5,o=z,t=d", &m) == 0);
	TEST_ASSERT("parse_transmit: action == T",
	    cmd.action == KITTY_ACTION_TRANSMIT_AND_DISPLAY);
	TEST_ASSERT("parse_transmit: image_id == 5", cmd.image_id == 5);
	TEST_ASSERT("parse_transmit: format == 100", cmd.format == 100);
	TEST_ASSERT("parse_transmit: size", cmd.pixel_width == 80 &&
	    cmd.pixel_height == 40);
	TEST_ASSERT("parse_transmit: cells", cmd.cols == 10 && cmd.rows == 5);
	TEST_ASSERT("parse_transmit: compression == z",
	    cmd.compression == 'z');
	TEST_ASSERT("parse_transmit: transmission == d",
	    cmd.transmission == 'd');
	TEST_PASS("parse_transmit");
	return (0);
}

/* Parse a place action with a placement ID. */
static int
test_parse_place(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_place: ret == 0",
	    test_parse(&cmd, "a=p,i=5,p=1,X=2,Y=3,z=10,C=1", &m) == 0);
	TEST_ASSERT("parse_place: action == p",
	    cmd.action == KITTY_ACTION_PLACE);
	TEST_ASSERT("parse_place: image_id == 5", cmd.image_id == 5);
	TEST_ASSERT("parse_place: placement_id == 1", cmd.placement_id == 1);
	TEST_ASSERT("parse_place: offsets",
	    cmd.cell_xoff == 2 && cmd.cell_yoff == 3);
	TEST_ASSERT("parse_place: zindex == 10", cmd.zindex == 10);
	TEST_ASSERT("parse_place: cursor_no_move", cmd.cursor_no_move == 1);
	TEST_PASS("parse_place");
	return (0);
}

/* Parse a delete action. */
static int
test_parse_delete(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_delete: ret == 0",
	    test_parse(&cmd, "a=d,d=I,i=5", &m) == 0);
	TEST_ASSERT("parse_delete: action == d",
	    cmd.action == KITTY_ACTION_DELETE);
	TEST_ASSERT("parse_delete: delete_action == I",
	    cmd.delete_action == 'I');
	TEST_ASSERT("parse_delete: image_id == 5", cmd.image_id == 5);
	TEST_PASS("parse_delete");
	return (0);
}

/* The payload after the semicolon is decoded and stored. */
static int
test_parse_payload(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("parse_payload: reply", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("parse_payload: stored", test_size(1, 1, 1));
	TEST_ASSERT("parse_payload: content", test_pixel(1, 0, 0, test_red));
	TEST_PASS("parse_payload");
	return (0);
}

/* An empty payload after the semicolon is invalid. */
static int
test_parse_empty_payload(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_input("\033_Ga=t,i=1,f=32,s=1,v=1;\033\\");
	TEST_ASSERT("parse_empty_payload: EINVAL",
	    test_replied(reply, "i=1;EINVAL"));
	TEST_ASSERT("parse_empty_payload: not stored", test_map(1) == NULL);
	TEST_PASS("parse_empty_payload");
	return (0);
}

/* Unknown single-letter keys are ignored, longer keys are invalid. */
static int
test_parse_unknown_keys(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_unknown_keys: letter ret == 0",
	    test_parse(&cmd, "a=q,i=1,k=42", &m) == 0);
	TEST_ASSERT("parse_unknown_keys: letter action == q",
	    cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_unknown_keys: letter image_id == 1",
	    cmd.image_id == 1);

	TEST_ASSERT("parse_unknown_keys: word ret == -1",
	    test_parse(&cmd, "a=q,unknownkey=42,i=1", &m) == -1);
	TEST_ASSERT("parse_unknown_keys: word EINVAL",
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unknown_keys: word keeps parsing",
	    cmd.action == KITTY_ACTION_QUERY && cmd.image_id == 1);
	TEST_PASS("parse_unknown_keys");
	return (0);
}

/* A key without a value is invalid. */
static int
test_parse_malformed(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_malformed: ret == -1",
	    test_parse(&cmd, "a=q,invalid", &m) == -1);
	TEST_ASSERT("parse_malformed: EINVAL", strcmp(m, "EINVAL") == 0);
	TEST_PASS("parse_malformed");
	return (0);
}

/* Character-valued keys must have exactly one character. */
static int
test_parse_character_values(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_char_values: action long",
	    test_parse(&cmd, "a=tt,i=1", &m) == -1);
	TEST_ASSERT("parse_char_values: action empty",
	    test_parse(&cmd, "a=,i=1", &m) == -1);
	TEST_ASSERT("parse_char_values: transmission long",
	    test_parse(&cmd, "a=t,t=dd,i=1", &m) == -1);
	TEST_ASSERT("parse_char_values: compression long",
	    test_parse(&cmd, "a=t,o=zz,i=1", &m) == -1);
	TEST_ASSERT("parse_char_values: delete long",
	    test_parse(&cmd, "a=d,d=ii,i=1", &m) == -1);
	TEST_ASSERT("parse_char_values: valid ret",
	    test_parse(&cmd, "a=d,d=I,i=1", &m) == 0);
	TEST_ASSERT("parse_char_values: valid action",
	    cmd.action == KITTY_ACTION_DELETE && cmd.delete_action == 'I');
	TEST_PASS("parse_char_values");
	return (0);
}

/* Parse quiet mode. */
static int
test_parse_quiet(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_quiet: ret == 0",
	    test_parse(&cmd, "a=q,i=1,q=1", &m) == 0);
	TEST_ASSERT("parse_quiet: quiet == 1", cmd.quiet == 1);
	TEST_PASS("parse_quiet");
	return (0);
}

/* Parse the chunked upload flag. */
static int
test_parse_chunked(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_chunked: ret == 0",
	    test_parse(&cmd, "a=t,i=1,m=1", &m) == 0);
	TEST_ASSERT("parse_chunked: more == 1", cmd.more == 1);
	TEST_PASS("parse_chunked");
	return (0);
}

/* Parse an image number. */
static int
test_parse_image_number(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_image_number: ret == 0",
	    test_parse(&cmd, "a=t,I=42", &m) == 0);
	TEST_ASSERT("parse_image_number: image_number == 42",
	    cmd.image_number == 42);
	TEST_PASS("parse_image_number");
	return (0);
}

/* Parse a source rectangle. */
static int
test_parse_source_rect(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_source_rect: ret == 0",
	    test_parse(&cmd, "a=p,i=1,x=10,y=20,w=30,h=40", &m) == 0);
	TEST_ASSERT("parse_source_rect: position",
	    cmd.src_x == 10 && cmd.src_y == 20);
	TEST_ASSERT("parse_source_rect: size",
	    cmd.src_w == 30 && cmd.src_h == 40);
	TEST_PASS("parse_source_rect");
	return (0);
}

/* Parse relative placement keys, with signed offsets. */
static int
test_parse_relative_placement(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_relative: ret == 0",
	    test_parse(&cmd, "a=p,i=1,P=2,Q=3,H=-1,V=4", &m) == 0);
	TEST_ASSERT("parse_relative: values",
	    cmd.parent_image == 2 && cmd.parent_placement == 3 &&
	    cmd.parent_xoff == -1 && cmd.parent_yoff == 4);
	TEST_ASSERT("parse_relative: offset range",
	    test_parse(&cmd, "H=2147483648", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_relative: negative parent",
	    test_parse(&cmd, "P=-1", &m) == -1 && strcmp(m, "EINVAL") == 0);
	TEST_PASS("parse_relative");
	return (0);
}

/* Parse file offset and size keys. */
static int
test_parse_file_range(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_file_range: ret == 0",
	    test_parse(&cmd, "a=t,i=1,t=f,S=4,O=3", &m) == 0);
	TEST_ASSERT("parse_file_range: data_size == 4", cmd.data_size == 4);
	TEST_ASSERT("parse_file_range: data_offset == 3",
	    cmd.data_offset == 3);
	TEST_ASSERT("parse_file_range: transmission == f",
	    cmd.transmission == 'f');
	TEST_PASS("parse_file_range");
	return (0);
}

/* Any non-zero U key asks for a virtual placement. */
static int
test_parse_virtual(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_virtual: U=1",
	    test_parse(&cmd, "a=T,i=1,U=1", &m) == 0 && cmd.unicode);
	TEST_ASSERT("parse_virtual: U=2",
	    test_parse(&cmd, "a=T,i=1,U=2", &m) == 0 && cmd.unicode);
	TEST_ASSERT("parse_virtual: U=0",
	    test_parse(&cmd, "a=T,i=1,U=0", &m) == 0 && !cmd.unicode);
	TEST_PASS("parse_virtual");
	return (0);
}

/* Empty control data keeps the defaults. */
static int
test_parse_empty(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_empty: ret == 0", test_parse(&cmd, "", &m) == 0);
	TEST_ASSERT("parse_empty: action == t",
	    cmd.action == KITTY_ACTION_TRANSMIT);
	TEST_ASSERT("parse_empty: format == RGBA",
	    cmd.format == KITTY_FORMAT_RGBA);
	TEST_ASSERT("parse_empty: transmission == d",
	    cmd.transmission == 'd');
	TEST_ASSERT("parse_empty: delete_action == a",
	    cmd.delete_action == 'a');
	TEST_PASS("parse_empty");
	return (0);
}

/* A sequence with only a semicolon does nothing. */
static int
test_parse_semicolon_only(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_input("\033_G;\033\\");
	TEST_ASSERT("parse_semicolon_only: no reply", *reply == '\0');
	TEST_ASSERT("parse_semicolon_only: no image", test_count_images() == 0);
	TEST_ASSERT("parse_semicolon_only: idle", test_idle());
	TEST_PASS("parse_semicolon_only");
	return (0);
}

/* A sequence with only a payload does nothing. */
static int
test_parse_payload_only(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_input("\033_G;" TEST_PIXEL "\033\\");
	TEST_ASSERT("parse_payload_only: no reply", *reply == '\0');
	TEST_ASSERT("parse_payload_only: no image", test_count_images() == 0);
	TEST_ASSERT("parse_payload_only: not placed", !test_placed_at(0, 0));
	TEST_ASSERT("parse_payload_only: idle", test_idle());
	TEST_PASS("parse_payload_only");
	return (0);
}

/* Duplicate keys use the last value. */
static int
test_parse_duplicate_keys(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_duplicate: ret == 0",
	    test_parse(&cmd, "i=1,i=2,i=3", &m) == 0);
	TEST_ASSERT("parse_duplicate: image_id == 3", cmd.image_id == 3);
	TEST_PASS("parse_duplicate");
	return (0);
}

/* Large malformed control data is rejected. */
static int
test_parse_large_control(void)
{
	struct kitty_command	 cmd;
	const char		*m;
	char			*data;
	int			 ret;

	data = xmalloc(8192);
	memset(data, 'a', 8191);
	data[8191] = '\0';
	ret = test_parse(&cmd, data, &m);
	free(data);
	TEST_ASSERT("parse_large: ret == -1", ret == -1);
	TEST_ASSERT("parse_large: EINVAL", strcmp(m, "EINVAL") == 0);
	TEST_PASS("parse_large");
	return (0);
}

/* Parse a negative z-index. */
static int
test_parse_negative_zindex(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_negative_z: ret == 0",
	    test_parse(&cmd, "z=-5", &m) == 0);
	TEST_ASSERT("parse_negative_z: zindex == -5", cmd.zindex == -5);
	TEST_PASS("parse_negative_z");
	return (0);
}

/* Negative or too large values for numeric keys are rejected. */
static int
test_parse_negative_unsigned(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_unsigned_bounds: negative id",
	    test_parse(&cmd, "a=p,i=-1", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unsigned_bounds: negative x",
	    test_parse(&cmd, "a=p,x=-1", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unsigned_bounds: large id",
	    test_parse(&cmd, "a=p,i=4294967296", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unsigned_bounds: large z",
	    test_parse(&cmd, "z=2147483648", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unsigned_bounds: overflow",
	    test_parse(&cmd, "i=999999999999999999999999", &m) == -1 &&
	    strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_unsigned_bounds: maximum id",
	    test_parse(&cmd, "a=p,i=4294967295", &m) == 0 &&
	    cmd.image_id == 4294967295U);
	TEST_PASS("parse_unsigned_bounds");
	return (0);
}

/* An empty key between commas is invalid but parsing continues. */
static int
test_parse_multiple_commas(void)
{
	struct kitty_command	 cmd;
	const char		*m;

	TEST_ASSERT("parse_multi_commas: ret == -1",
	    test_parse(&cmd, "a=q,,i=1", &m) == -1);
	TEST_ASSERT("parse_multi_commas: EINVAL", strcmp(m, "EINVAL") == 0);
	TEST_ASSERT("parse_multi_commas: action == q",
	    cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_multi_commas: image_id == 1", cmd.image_id == 1);
	TEST_ASSERT("parse_multi_commas: trailing comma",
	    test_parse(&cmd, "a=q,i=1,", &m) == 0);
	TEST_PASS("parse_multi_commas");
	return (0);
}

/* A stored image can be found by its ID. */
static int
test_image_store_find(void)
{
	static const u_char	 data[] = {
		0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff
	};
	struct kitty_image_map	*map;
	const char		*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=42,f=32,s=2,v=1",
	    test_base64(data, sizeof data));
	TEST_ASSERT("store_find: reply", test_replied(reply, "i=42;OK"));
	map = test_map(42);
	TEST_ASSERT("store_find: found", map != NULL && map->kitty_id == 42);
	TEST_ASSERT("store_find: image", image_find(map->image_id) != NULL);
	TEST_ASSERT("store_find: size", test_size(42, 2, 1));
	TEST_ASSERT("store_find: no placement",
	    TAILQ_EMPTY(&map->placements) && !test_placed_at(0, 0));
	TEST_ASSERT("store_find: missing", test_map(99) == NULL);
	TEST_PASS("store_find");
	return (0);
}

/* An image number finds the image it was assigned to. */
static int
test_image_number_find(void)
{
	struct kitty_image_map	*map;
	const char		*reply;

	test_pane_new();
	reply = test_graphics("a=t,I=42,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("number_find: reply", test_replied(reply, "i=1,I=42;OK"));
	map = test_map_number(42);
	TEST_ASSERT("number_find: found", map != NULL);
	TEST_ASSERT("number_find: id == 1", map->kitty_id == 1);
	TEST_ASSERT("number_find: same image", map == test_map(1));
	TEST_ASSERT("number_find: missing", test_map_number(99) == NULL);
	TEST_PASS("number_find");
	return (0);
}

/* The stored image keeps the payload pixels. */
static int
test_image_store_with_payload(void)
{
	static const u_char	 data[] = {
		0x42, 0x42, 0x42, 0xff, 1, 2, 3, 4,
		5, 6, 7, 8, 0x42, 0x42, 0x42, 0x80
	};
	static const u_char	 last[] = { 0x42, 0x42, 0x42, 0x80 };
	const char		*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,f=32,s=2,v=2",
	    test_base64(data, sizeof data));
	TEST_ASSERT("store_payload: reply", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("store_payload: size", test_size(1, 2, 2));
	TEST_ASSERT("store_payload: first", test_pixel(1, 0, 0, data));
	TEST_ASSERT("store_payload: second", test_pixel(1, 1, 0, data + 4));
	TEST_ASSERT("store_payload: last", test_pixel(1, 1, 1, last));
	TEST_PASS("store_payload");
	return (0);
}

/* Transmitting an existing ID replaces the image in the same layer order. */
static int
test_image_replace(void)
{
	const char	*reply;
	u_int		 order;

	test_pane_new();
	test_store(7);
	TEST_ASSERT("replace: stored", test_image(7) != NULL);
	order = image_get_order_id(test_image(7));

	reply = test_graphics("a=t,i=7,f=24,s=2,v=1", "/wAAAP8A");
	TEST_ASSERT("replace: reply", test_replied(reply, "i=7;OK"));
	TEST_ASSERT("replace: one image", test_count_images() == 1);
	TEST_ASSERT("replace: size", test_size(7, 2, 1));
	TEST_ASSERT("replace: RGB converted", test_pixel(7, 0, 0, test_red) &&
	    test_pixel(7, 1, 0, test_green));
	TEST_ASSERT("replace: order preserved",
	    image_get_order_id(test_image(7)) == order);
	TEST_PASS("replace");
	return (0);
}

/* Replacing an image removes the placements of the old image. */
static int
test_image_replace_with_payload(void)
{
	const char	*reply;

	test_pane_new();
	test_graphics("a=T,i=1,q=2,f=32,s=1,v=1,c=1,r=1,C=1", TEST_PIXEL);
	TEST_ASSERT("replace_payload: placed", test_placed_at(0, 0));

	reply = test_graphics("a=t,i=1,f=32,s=1,v=1", TEST_GREEN);
	TEST_ASSERT("replace_payload: reply", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("replace_payload: content",
	    test_pixel(1, 0, 0, test_green));
	TEST_ASSERT("replace_payload: old placement gone",
	    !test_placed_at(0, 0) && test_count_placements(1) == 0);
	TEST_PASS("replace_payload");
	return (0);
}

/* A transmit-and-display stores the image and a named placement. */
static int
test_parse_dispatch_transmit_display(void)
{
	static const u_char	 white[] = { 0xff, 0xff, 0xff, 0xff };
	const char		*reply;

	test_pane_new();
	test_cursor(2, 3);
	reply = test_graphics("a=T,i=44,f=24,s=2,v=2,c=4,r=1,p=7,C=1",
	    "/wAAAP8AAAD/////");
	TEST_ASSERT("dispatch_T: reply", test_replied(reply, "i=44,p=7;OK"));
	TEST_ASSERT("dispatch_T: image stored", test_size(44, 2, 2));
	TEST_ASSERT("dispatch_T: payload decoded",
	    test_pixel(44, 0, 0, test_red) &&
	    test_pixel(44, 1, 0, test_green) && test_pixel(44, 1, 1, white));
	TEST_ASSERT("dispatch_T: placement stored",
	    test_placement(44, 7) != NULL);
	TEST_ASSERT("dispatch_T: placement cells",
	    test_placement_at(44, 7, 2, 3) && test_placement_at(44, 7, 5, 3));
	TEST_ASSERT("dispatch_T: placement size", !test_placed_at(1, 3) &&
	    !test_placed_at(6, 3) && !test_placed_at(2, 4));
	TEST_ASSERT("dispatch_T: cursor stayed",
	    test_wp->base.cx == 2 && test_wp->base.cy == 3);
	TEST_PASS("dispatch_T");
	return (0);
}

/* A direct RGB payload is decoded to RGBA. */
static int
test_transmit_direct_rgb_decodes(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=45,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("direct_rgb: reply", test_replied(reply, "i=45;OK"));
	TEST_ASSERT("direct_rgb: image stored", test_size(45, 1, 1));
	TEST_ASSERT("direct_rgb: decoded", test_pixel(45, 0, 0, test_red));
	TEST_PASS("direct_rgb");
	return (0);
}

/* A missing action transmits. */
static int
test_default_action_transmit(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("i=48,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("default_action: reply", test_replied(reply, "i=48;OK"));
	TEST_ASSERT("default_action: decoded", test_pixel(48, 0, 0, test_red));
	TEST_ASSERT("default_action: not placed", !test_placed_at(0, 0));
	TEST_PASS("default_action");
	return (0);
}

/* A missing format is RGBA. */
static int
test_default_format_rgba(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=134,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("default_format: reply", test_replied(reply, "i=134;OK"));
	TEST_ASSERT("default_format: decoded",
	    test_pixel(134, 0, 0, test_red));
	reply = test_graphics("a=t,i=135,s=1,v=1", TEST_RGB);
	TEST_ASSERT("default_format: RGB size rejected",
	    test_replied(reply, "i=135;EINVAL") && test_map(135) == NULL);
	TEST_PASS("default_format");
	return (0);
}

/* An invalid format, compression or medium is rejected. */
static int
test_transmit_invalid_format_compression(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=135,f=7,s=1,v=1", "AAAA");
	TEST_ASSERT("invalid_format: f EINVAL",
	    test_replied(reply, "i=135;EINVAL"));
	reply = test_graphics("a=t,i=136,f=24,o=2,s=1,v=1", TEST_RGB);
	TEST_ASSERT("invalid_format: o EINVAL",
	    test_replied(reply, "i=136;EINVAL"));
	reply = test_graphics("a=t,i=138,f=24,t=x,s=1,v=1", TEST_RGB);
	TEST_ASSERT("invalid_format: t EINVAL",
	    test_replied(reply, "i=138;EINVAL"));
	TEST_ASSERT("invalid_format: no images", test_count_images() == 0);
	TEST_PASS("invalid_format");
	return (0);
}

/* A direct RGB payload must match the declared size. */
static int
test_transmit_direct_rgb_size_mismatch(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=46,f=24,s=2,v=2", TEST_RGB);
	TEST_ASSERT("direct_rgb_size: short EINVAL",
	    test_replied(reply, "i=46;EINVAL"));
	reply = test_graphics("a=t,i=46,f=24,s=1,v=1", "/wAA/wAA");
	TEST_ASSERT("direct_rgb_size: long EINVAL",
	    test_replied(reply, "i=46;EINVAL"));
	TEST_ASSERT("direct_rgb_size: not stored", test_map(46) == NULL);
	TEST_PASS("direct_rgb_size");
	return (0);
}

/* A direct PNG payload is decoded and invalid PNG data is rejected. */
static int
test_transmit_direct_png_decodes(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=47,f=100", TEST_PNG);
	TEST_ASSERT("direct_png: reply", test_replied(reply, "i=47;OK"));
	TEST_ASSERT("direct_png: size", test_size(47, 1, 1));
	TEST_ASSERT("direct_png: decoded", test_pixel(47, 0, 0, test_red));
	reply = test_graphics("a=t,i=50,f=100,s=1,v=1", "QUJD");
	TEST_ASSERT("direct_png: invalid EINVAL",
	    test_replied(reply, "i=50;EINVAL") && test_map(50) == NULL);
	TEST_PASS("direct_png");
	return (0);
}

/* A direct PNG payload must be base64 encoded. */
static int
test_transmit_direct_png_rejects_raw(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_input("\033_Ga=t,i=49,f=100;\211PNG\033\\");
	TEST_ASSERT("direct_png_raw: EINVAL",
	    test_replied(reply, "i=49;EINVAL"));
	TEST_ASSERT("direct_png_raw: not stored", test_map(49) == NULL);
	TEST_PASS("direct_png_raw");
	return (0);
}

/* A direct zlib-compressed payload is decompressed. */
static int
test_transmit_direct_compression_preserved(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=54,f=24,o=z,s=1,v=1", TEST_ZLIB);
	TEST_ASSERT("direct_z: reply", test_replied(reply, "i=54;OK"));
	TEST_ASSERT("direct_z: decompressed", test_pixel(54, 0, 0, test_red));
	reply = test_graphics("a=t,i=55,f=24,o=z,s=1,v=1", TEST_RGB);
	TEST_ASSERT("direct_z: uncompressed EINVAL",
	    test_replied(reply, "i=55;EINVAL") && test_map(55) == NULL);
	TEST_PASS("direct_z");
	return (0);
}

/* A file transfer reads the named file and keeps it. */
static int
test_transmit_file_regular(void)
{
	char		 path[] = "/tmp/tmux-kitty-file.XXXXXX";
	const char	*reply;

	test_pane_new();
	TEST_ASSERT("file_regular: write",
	    test_write_file(path, test_red, sizeof test_red) == 0);
	reply = test_graphics("a=t,i=55,f=32,t=f,s=1,v=1",
	    test_base64(path, strlen(path)));
	TEST_ASSERT("file_regular: reply", test_replied(reply, "i=55;OK"));
	TEST_ASSERT("file_regular: content", test_pixel(55, 0, 0, test_red));
	TEST_ASSERT("file_regular: source remains", access(path, F_OK) == 0);

	reply = test_graphics("a=t,i=132,f=32,t=f,m=1,s=1,v=1",
	    test_base64(path, strlen(path)));
	unlink(path);
	TEST_ASSERT("file_regular: m ignored reply",
	    test_replied(reply, "i=132;OK"));
	TEST_ASSERT("file_regular: m ignored idle", test_idle());
	TEST_ASSERT("file_regular: m ignored content",
	    test_pixel(132, 0, 0, test_red));
	TEST_PASS("file_regular");
	return (0);
}

/* A file transfer honours the offset and size. */
static int
test_transmit_file_range(void)
{
	static const u_char	 data[] = {
		'0', '1', '2', 0xff, 0, 0, 0xff, '7', '8', '9'
	};
	char			 path[] = "/tmp/tmux-kitty-file-range.XXXXXX";
	const char		*reply;

	test_pane_new();
	TEST_ASSERT("file_range: write",
	    test_write_file(path, data, sizeof data) == 0);
	reply = test_graphics("a=t,i=58,f=32,t=f,S=4,O=3,s=1,v=1",
	    test_base64(path, strlen(path)));
	TEST_ASSERT("file_range: reply", test_replied(reply, "i=58;OK"));
	TEST_ASSERT("file_range: sliced", test_pixel(58, 0, 0, test_red));
	reply = test_graphics("a=t,i=59,f=32,t=f,S=4,O=7,s=1,v=1",
	    test_base64(path, strlen(path)));
	unlink(path);
	TEST_ASSERT("file_range: short EBADF",
	    test_replied(reply, "i=59;EBADF") && test_map(59) == NULL);
	TEST_PASS("file_range");
	return (0);
}

/* A file transfer rejects a path with NUL or invalid base64. */
static int
test_transmit_file_bad_path_payload(void)
{
	static const char	 path[] = {
		'/', 't', 'm', 'p', '/', 'f', 'o', 'o', '\0', 'b', 'a', 'r'
	};
	const char		*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=59,f=32,t=f,s=1,v=1",
	    test_base64(path, sizeof path));
	TEST_ASSERT("file_bad_path: NUL EBADF",
	    test_replied(reply, "i=59;EBADF"));
	reply = test_graphics("a=t,i=60,f=32,t=f,s=1,v=1", "!!!");
	TEST_ASSERT("file_bad_path: invalid EINVAL",
	    test_replied(reply, "i=60;EINVAL"));
	reply = test_graphics("a=t,i=61,f=32,t=f,s=1,v=1",
	    test_base64("/nonexistent/tmux-kitty", 23));
	TEST_ASSERT("file_bad_path: missing EBADF",
	    test_replied(reply, "i=61;EBADF"));
	reply = test_graphics("a=t,i=62,f=32,t=f,s=1,v=1",
	    test_base64("/tmp", 4));
	TEST_ASSERT("file_bad_path: directory EBADF",
	    test_replied(reply, "i=62;EBADF"));
	TEST_ASSERT("file_bad_path: no images", test_count_images() == 0);
	TEST_PASS("file_bad_path");
	return (0);
}

/* A temporary file transfer removes a marked file. */
static int
test_transmit_file_temporary_removes_source(void)
{
	char		 path[] = "/tmp/tty-graphics-protocol-tmux-kitty.XXXXXX";
	const char	*reply;
	int		 removed;

	test_pane_new();
	TEST_ASSERT("file_temp: write",
	    test_write_file(path, test_red, sizeof test_red) == 0);
	reply = test_graphics("a=t,i=56,f=32,t=t,s=1,v=1",
	    test_base64(path, strlen(path)));
	removed = (access(path, F_OK) == -1);
	unlink(path);
	TEST_ASSERT("file_temp: reply", test_replied(reply, "i=56;OK"));
	TEST_ASSERT("file_temp: content", test_pixel(56, 0, 0, test_red));
	TEST_ASSERT("file_temp: source removed", removed);
	TEST_PASS("file_temp");
	return (0);
}

/* A temporary file transfer keeps files which are not marked. */
static int
test_transmit_file_temporary_keeps_unapproved_source(void)
{
	char		 dir[] = "/tmp/tty-graphics-protocol-dir.XXXXXX";
	char		 path[] = "/tmp/tmux-kitty-unsafe-temp.XXXXXX";
	char		 nested[512];
	const char	*reply;
	FILE		*f;
	int		 kept;

	test_pane_new();
	TEST_ASSERT("file_temp_keep: write",
	    test_write_file(path, test_red, sizeof test_red) == 0);
	reply = test_graphics("a=t,i=120,f=32,t=t,s=1,v=1",
	    test_base64(path, strlen(path)));
	kept = (access(path, F_OK) == 0);
	unlink(path);
	TEST_ASSERT("file_temp_keep: reply", test_replied(reply, "i=120;OK"));
	TEST_ASSERT("file_temp_keep: source remains", kept);

	TEST_ASSERT("file_temp_keep: mkdtemp", mkdtemp(dir) != NULL);
	xsnprintf(nested, sizeof nested, "%s/victim", dir);
	f = fopen(nested, "wb");
	if (f != NULL) {
		fwrite(test_red, 1, sizeof test_red, f);
		fclose(f);
	}
	reply = test_graphics("a=t,i=122,f=32,t=t,s=1,v=1",
	    test_base64(nested, strlen(nested)));
	kept = (access(nested, F_OK) == 0);
	unlink(nested);
	rmdir(dir);
	TEST_ASSERT("file_temp_keep: nested reply",
	    test_replied(reply, "i=122;OK"));
	TEST_ASSERT("file_temp_keep: nested source remains", kept);
	TEST_PASS("file_temp_keep");
	return (0);
}

/* A chunked transmit-and-display keeps the first chunk's controls. */
static int
test_chunked_transmit_display_metadata(void)
{
	const char	*reply;

	test_pane_new();
	test_cursor(4, 5);
	reply = test_graphics("a=T,i=57,f=24,s=1,v=2,c=2,r=2,p=9,m=1",
	    TEST_RGB);
	TEST_ASSERT("chunked_T: first no reply", *reply == '\0');
	reply = test_graphics("m=0", "AP8A");
	TEST_ASSERT("chunked_T: final reply",
	    test_replied(reply, "i=57,p=9;OK"));
	TEST_ASSERT("chunked_T: size", test_size(57, 1, 2));
	TEST_ASSERT("chunked_T: payload appended",
	    test_pixel(57, 0, 0, test_red) && test_pixel(57, 0, 1, test_green));
	TEST_ASSERT("chunked_T: placement",
	    test_placement_at(57, 9, 4, 5) && test_placement_at(57, 9, 5, 6));
	TEST_ASSERT("chunked_T: cursor",
	    test_wp->base.cx == 6 && test_wp->base.cy == 6);
	TEST_PASS("chunked_T");
	return (0);
}

/* Scrolling moves placements up and into the history. */
static int
test_scroll_up_moves_and_removes_placements(void)
{
	struct grid	*gd;
	struct image	*im;
	u_int		 sx, sy;

	test_pane_new();
	test_history(100);
	test_cursor(1, 5);
	test_graphics("a=T,i=71,q=2,f=32,s=1,v=1,c=2,r=2,C=1", TEST_PIXEL);
	TEST_ASSERT("scroll_up: placed", test_placement_at(71, 0, 1, 5));

	test_input("\033[2S");
	TEST_ASSERT("scroll_up: moved", test_placement_at(71, 0, 1, 3) &&
	    test_placement_at(71, 0, 2, 4) && !test_placed_at(1, 5));

	test_input("\033[5S");
	gd = test_wp->base.grid;
	TEST_ASSERT("scroll_up: not visible", !test_placed_at(1, 0));
	TEST_ASSERT("scroll_up: kept in history",
	    test_count_placements(71) == 1 && gd->hsize >= 2 &&
	    image_grid_get_at(gd, 1, gd->hsize - 1, &im, &sx, &sy) == 1);
	TEST_PASS("scroll_up");
	return (0);
}

/* A transmit-and-display without an ID places an unstored image. */
static int
test_anonymous_transmit_display(void)
{
	const char	*reply;

	test_pane_new();
	test_cursor(1, 2);
	reply = test_graphics("a=T,f=24,s=1,v=1,c=1,r=1", TEST_RGB);
	TEST_ASSERT("anonymous_T: no reply", *reply == '\0');
	TEST_ASSERT("anonymous_T: not stored", test_count_images() == 0);
	TEST_ASSERT("anonymous_T: placed", test_placed_at(1, 2) &&
	    !test_placed_at(0, 2) && !test_placed_at(2, 2));
	TEST_ASSERT("anonymous_T: cursor",
	    test_wp->base.cx == 2 && test_wp->base.cy == 2);
	TEST_PASS("anonymous_T");
	return (0);
}

/* Deleting placements frees anonymous image data. */
static int
test_anonymous_delete_removes_data(void)
{
	const char	*reply;
	u_int		 anonymous;

	test_pane_new();
	reply = test_graphics("a=T,i=130,f=24,s=1,v=1,c=1,r=1", TEST_RGB);
	TEST_ASSERT("anonymous_delete: named reply",
	    test_replied(reply, "i=130;OK"));
	reply = test_graphics("a=T,f=24,s=1,v=1,c=1,r=1", TEST_RGB);
	TEST_ASSERT("anonymous_delete: anonymous no reply", *reply == '\0');
	anonymous = test_image_at(1, 0);
	TEST_ASSERT("anonymous_delete: two placed",
	    test_placement_at(130, 0, 0, 0) && anonymous != 0);
	TEST_ASSERT("anonymous_delete: one stored", test_count_images() == 1);

	reply = test_graphics("a=d,d=a", NULL);
	TEST_ASSERT("anonymous_delete: delete no reply", *reply == '\0');
	TEST_ASSERT("anonymous_delete: placements gone",
	    !test_placed_at(0, 0) && !test_placed_at(1, 0));
	TEST_ASSERT("anonymous_delete: named data remains",
	    test_image(130) != NULL);
	TEST_ASSERT("anonymous_delete: anonymous data removed",
	    image_find(anonymous) == NULL);
	TEST_PASS("anonymous_delete");
	return (0);
}

/* An image number selects the newest image with that number. */
static int
test_image_number_newest(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,I=42,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("number_newest: first reply",
	    test_replied(reply, "i=1,I=42;OK"));
	reply = test_graphics("a=t,I=42,f=32,s=1,v=1", TEST_GREEN);
	TEST_ASSERT("number_newest: second reply",
	    test_replied(reply, "i=2,I=42;OK"));
	TEST_ASSERT("number_newest: two images", test_count_images() == 2);
	TEST_ASSERT("number_newest: newest returned",
	    test_map_number(42) == test_map(2));
	TEST_ASSERT("number_newest: newest payload",
	    test_pixel(2, 0, 0, test_green) && test_pixel(1, 0, 0, test_red));
	TEST_PASS("number_newest");
	return (0);
}

/* A placement uses the requested size, offsets and layer. */
static int
test_placement_create(void)
{
	struct kitty_placement_map	*kpl;
	struct image_geom		*ig;
	const char			*reply;

	test_pane_new();
	test_store(1);
	reply = test_graphics("a=p,i=1,p=99,c=10,r=5,X=2,Y=3,z=5,C=1", NULL);
	TEST_ASSERT("placement: reply", test_replied(reply, "i=1,p=99;OK"));
	kpl = test_placement(1, 99);
	TEST_ASSERT("placement: found", kpl != NULL);
	ig = image_get_geometry(image_find(kpl->image_id));
	TEST_ASSERT("placement: cells", ig->sx == 10 && ig->sy == 5);
	TEST_ASSERT("placement: offsets",
	    ig->canvas_xoff == 2 && ig->canvas_yoff == 3);
	TEST_ASSERT("placement: zindex", test_zindex_at(0, 0) == 5);
	TEST_ASSERT("placement: area", test_placement_at(1, 99, 9, 4) &&
	    !test_placed_at(10, 0) && !test_placed_at(0, 5));
	TEST_ASSERT("placement: cursor_no_move",
	    test_wp->base.cx == 0 && test_wp->base.cy == 0);
	TEST_ASSERT("placement: wrong image", test_placement(2, 99) == NULL);
	TEST_PASS("placement");
	return (0);
}

/* Freeing the pane frees its images and placements. */
static int
test_free_all(void)
{
	u_int	src, placed;

	test_pane_new();
	test_graphics("a=T,i=1,q=2,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("free_all: stored", test_map(1) != NULL);
	src = test_map(1)->image_id;
	placed = test_image_at(0, 0);
	TEST_ASSERT("free_all: placed", placed != 0);

	test_pane_free();
	TEST_ASSERT("free_all: state gone", TAILQ_EMPTY(&kitty_states));
	TEST_ASSERT("free_all: image gone", image_find(src) == NULL);
	TEST_ASSERT("free_all: placement gone", image_find(placed) == NULL);
	TEST_PASS("free_all");
	return (0);
}

/* Freeing the pane frees placements saved by the alternate screen. */
static int
test_free_saved_lists(void)
{
	u_int	primary, alternate;

	test_pane_new();
	test_graphics("a=T,i=2,q=2,f=32,s=1,v=1,C=1", TEST_PIXEL);
	primary = test_image_at(0, 0);
	test_input("\033[?1049h");
	test_graphics("a=T,i=3,q=2,f=32,s=1,v=1,C=1", TEST_PIXEL);
	alternate = test_image_at(0, 0);
	TEST_ASSERT("free_saved: placed", primary != 0 && alternate != 0 &&
	    primary != alternate);
	TEST_ASSERT("free_saved: saved placement",
	    test_count_placements(2) == 1);

	test_pane_free();
	TEST_ASSERT("free_saved: state gone", TAILQ_EMPTY(&kitty_states));
	TEST_ASSERT("free_saved: saved placement freed",
	    image_find(primary) == NULL);
	TEST_ASSERT("free_saved: alternate placement freed",
	    image_find(alternate) == NULL);
	TEST_PASS("free_saved");
	return (0);
}

/* The alternate screen saves and restores primary placements. */
static int
test_alternate_screen_transitions(void)
{
	test_pane_new();
	test_graphics("a=T,i=101,q=2,f=32,s=1,v=1,c=2,r=2,p=1,C=1",
	    TEST_PIXEL);
	TEST_ASSERT("alt_screen: primary placed",
	    test_placement_at(101, 1, 0, 0));

	test_input("\033[?1049h");
	TEST_ASSERT("alt_screen: hidden in alternate", !test_placed_at(0, 0));
	TEST_ASSERT("alt_screen: primary saved",
	    test_placement(101, 1) != NULL);

	test_graphics("a=T,i=202,q=2,f=32,s=1,v=1,c=3,r=3,p=2,C=1",
	    TEST_GREEN);
	TEST_ASSERT("alt_screen: alternate placed",
	    test_placement_at(202, 2, 2, 2));

	test_input("\033[?1049l");
	TEST_ASSERT("alt_screen: primary restored",
	    test_placement_at(101, 1, 0, 0) &&
	    test_placement_at(101, 1, 1, 1));
	TEST_ASSERT("alt_screen: alternate placement gone",
	    test_placement(202, 2) == NULL && !test_placed_at(2, 2));
	TEST_PASS("alternate_screen_transitions");
	return (0);
}

/* Placements are freed when their lines leave the history. */
static int
test_scrollback_truncation_high_load(void)
{
	char	*scroll;
	u_int	 i, placed;

	test_pane_new();
	test_history(10);
	test_cursor(0, 20);
	test_graphics("a=T,i=301,q=2,f=32,s=1,v=1,c=2,r=2,C=1", TEST_PIXEL);
	placed = test_image_at(0, 20);
	TEST_ASSERT("truncation: placed", placed != 0);

	scroll = xmalloc(1000 * 3 + 1);
	for (i = 0; i < 1000; i++)
		memcpy(scroll + i * 3, "\033[S", 3);
	scroll[1000 * 3] = '\0';
	test_input(scroll);
	free(scroll);
	TEST_ASSERT("truncation: placement removed",
	    test_count_placements(301) == 0 && image_find(placed) == NULL);

	test_input("\033[100000S");
	TEST_ASSERT("truncation: still empty", test_count_placements(301) == 0);
	TEST_ASSERT("truncation: image data kept", test_image(301) != NULL);
	TEST_PASS("scrollback_truncation_high_load");
	return (0);
}

/* A placement survives resizing the pane. */
static int
test_pane_resize_reflow(void)
{
	test_pane_new();
	test_cursor(10, 10);
	test_graphics("a=T,i=401,q=2,f=32,s=1,v=1,c=8,r=6,C=1", TEST_PIXEL);
	TEST_ASSERT("resize_reflow: placed",
	    test_placement_at(401, 0, 10, 10) &&
	    test_placement_at(401, 0, 17, 15));

	screen_resize(&test_wp->base, 40, 20, 1);
	TEST_ASSERT("resize_reflow: still placed",
	    test_count_placements(401) == 1 &&
	    test_placement_at(401, 0, 10, 10));
	TEST_PASS("pane_resize_reflow");
	return (0);
}

/* Deleting by image ID removes its placements and keeps the data. */
static int
test_delete_image(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=1,c=10,r=5,q=2");
	TEST_ASSERT("delete_image: placed", test_placement_at(1, 1, 0, 0));
	reply = test_graphics("a=d,d=i,i=1", NULL);
	TEST_ASSERT("delete_image: no reply", *reply == '\0');
	TEST_ASSERT("delete_image: placement removed",
	    test_count_placements(1) == 0 && !test_placed_at(0, 0));
	TEST_ASSERT("delete_image: data remains", test_image(1) != NULL);
	TEST_PASS("delete_image");
	return (0);
}

/* Deleting all placements keeps the data. */
static int
test_delete_all_placements(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=1,c=10,r=5,q=2");
	test_place(20, 10, "i=1,p=2,c=10,r=5,q=2");
	TEST_ASSERT("delete_all_placements: placed",
	    test_count_placements(1) == 2);
	reply = test_graphics("a=d,d=a", NULL);
	TEST_ASSERT("delete_all_placements: no reply", *reply == '\0');
	TEST_ASSERT("delete_all_placements: placements gone",
	    test_count_placements(1) == 0 && !test_placed_at(0, 0) &&
	    !test_placed_at(20, 10));
	TEST_ASSERT("delete_all_placements: data remains",
	    test_image(1) != NULL);
	TEST_PASS("delete_all_placements");
	return (0);
}

/* Deleting all with uppercase removes placed image data. */
static int
test_delete_all(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=1,c=10,r=5,q=2");
	reply = test_graphics("a=d,d=A", NULL);
	TEST_ASSERT("delete_all: no reply", *reply == '\0');
	TEST_ASSERT("delete_all: image gone", test_map(1) == NULL);
	TEST_ASSERT("delete_all: placement gone", !test_placed_at(0, 0));
	TEST_PASS("delete_all");
	return (0);
}

/* Deleting at the cursor removes the placement under it. */
static int
test_delete_cursor(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=1,c=10,r=5,q=2");
	test_place(20, 10, "i=1,p=2,c=2,r=2,q=2");
	test_cursor(3, 2);
	reply = test_graphics("a=d,d=c", NULL);
	TEST_ASSERT("delete_cursor: no reply", *reply == '\0');
	TEST_ASSERT("delete_cursor: placement gone",
	    test_placement(1, 1) == NULL && !test_placed_at(0, 0));
	TEST_ASSERT("delete_cursor: other remains",
	    test_placement_at(1, 2, 20, 10));
	TEST_PASS("delete_cursor");
	return (0);
}

/* Deleting by z-index removes only that layer. */
static int
test_delete_zindex(void)
{
	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=1,c=10,r=5,z=5,q=2");
	test_place(20, 10, "i=1,p=2,c=10,r=5,z=10,q=2");
	TEST_ASSERT("delete_zindex: layers",
	    test_zindex_at(0, 0) == 5 && test_zindex_at(20, 10) == 10);
	test_graphics("a=d,d=z,z=5", NULL);
	TEST_ASSERT("delete_zindex: z=5 gone",
	    test_placement(1, 1) == NULL && !test_placed_at(0, 0));
	TEST_ASSERT("delete_zindex: z=10 remains",
	    test_placement_at(1, 2, 20, 10));
	TEST_PASS("delete_zindex");
	return (0);
}

/* Deleting by image and placement ID removes only that placement. */
static int
test_delete_selector_image_placement(void)
{
	const char	*reply;

	test_pane_new();
	test_store(80);
	test_place(0, 0, "i=80,p=1,c=1,r=1,q=2");
	test_place(5, 0, "i=80,p=2,c=1,r=1,q=2");
	reply = test_graphics("a=d,d=i,i=80,p=1", NULL);
	TEST_ASSERT("delete_i_p: no reply", *reply == '\0');
	TEST_ASSERT("delete_i_p: placement 1 gone",
	    test_placement(80, 1) == NULL && !test_placed_at(0, 0));
	TEST_ASSERT("delete_i_p: placement 2 remains",
	    test_placement_at(80, 2, 5, 0));
	TEST_ASSERT("delete_i_p: image remains", test_image(80) != NULL);
	TEST_PASS("delete_i_p");
	return (0);
}

/* Deleting by image ID with uppercase removes the data. */
static int
test_delete_selector_image_data(void)
{
	test_pane_new();
	test_store(81);
	test_place(0, 0, "i=81,c=1,r=1,q=2");
	TEST_ASSERT("delete_I: placed", test_placed_at(0, 0));
	test_graphics("a=d,d=I,i=81", NULL);
	TEST_ASSERT("delete_I: image gone", test_map(81) == NULL);
	TEST_ASSERT("delete_I: placements gone", !test_placed_at(0, 0));
	TEST_PASS("delete_I");
	return (0);
}

/* Deleting at a one-based cell removes placements covering it. */
static int
test_delete_selector_cell(void)
{
	test_pane_new();
	test_store(82);
	test_place(2, 3, "i=82,p=1,c=2,r=2,q=2");
	test_place(10, 10, "i=82,p=2,c=2,r=2,q=2");
	test_graphics("a=d,d=p,x=4,y=5", NULL);
	TEST_ASSERT("delete_cell: placement 1 gone",
	    test_placement(82, 1) == NULL && !test_placed_at(2, 3));
	TEST_ASSERT("delete_cell: placement 2 remains",
	    test_placement_at(82, 2, 10, 10));
	TEST_ASSERT("delete_cell: data remains", test_image(82) != NULL);
	TEST_PASS("delete_cell");
	return (0);
}

/* Uppercase position selectors only delete the affected image data. */
static int
test_delete_selector_data_scope(void)
{
	test_pane_new();
	test_store(820);
	test_place(2, 3, "i=820,p=1,c=2,r=2,z=5,q=2");
	test_store(821);
	test_store(822);
	test_place(10, 10, "i=822,p=2,c=1,r=1,z=7,q=2");

	test_graphics("a=d,d=P,x=3,y=4", NULL);
	TEST_ASSERT("delete_data_scope: matched gone", test_map(820) == NULL);
	TEST_ASSERT("delete_data_scope: unplaced remains",
	    test_map(821) != NULL);
	TEST_ASSERT("delete_data_scope: other remains",
	    test_placement_at(822, 2, 10, 10));

	test_graphics("a=d,d=Z,z=7", NULL);
	TEST_ASSERT("delete_data_scope: other gone", test_map(822) == NULL &&
	    !test_placed_at(10, 10));
	TEST_ASSERT("delete_data_scope: unplaced still remains",
	    test_map(821) != NULL);
	TEST_PASS("delete_data_scope");
	return (0);
}

/* Deleting at a cell and z-index removes only matching placements. */
static int
test_delete_selector_cell_zindex(void)
{
	test_pane_new();
	test_store(83);
	test_place(2, 3, "i=83,p=1,c=2,r=2,z=5,q=2");
	test_place(2, 3, "i=83,p=2,c=2,r=2,z=7,q=2");
	test_place(10, 10, "i=83,p=3,c=2,r=2,z=5,q=2");
	test_graphics("a=d,d=q,x=3,y=4,z=5", NULL);
	TEST_ASSERT("delete_cell_z: placement 1 gone",
	    test_placement(83, 1) == NULL);
	TEST_ASSERT("delete_cell_z: placement 2 remains",
	    test_placement_at(83, 2, 2, 3));
	TEST_ASSERT("delete_cell_z: placement 3 remains",
	    test_placement_at(83, 3, 10, 10));
	TEST_PASS("delete_cell_z");
	return (0);
}

/* Deleting by column, row and z-index removes matching placements. */
static int
test_delete_selector_row_column_zindex(void)
{
	test_pane_new();
	test_store(84);
	test_place(2, 3, "i=84,p=1,c=2,r=2,z=1,q=2");
	test_place(10, 3, "i=84,p=2,c=1,r=1,z=2,q=2");
	test_place(7, 10, "i=84,p=3,c=1,r=1,z=3,q=2");

	test_graphics("a=d,d=x,x=3", NULL);
	TEST_ASSERT("delete_rcz: placement 1 gone",
	    test_placement(84, 1) == NULL);
	TEST_ASSERT("delete_rcz: placement 2 remains",
	    test_placement(84, 2) != NULL);
	TEST_ASSERT("delete_rcz: placement 3 remains",
	    test_placement(84, 3) != NULL);

	test_graphics("a=d,d=y,y=4", NULL);
	TEST_ASSERT("delete_rcz: placement 2 gone",
	    test_placement(84, 2) == NULL);
	TEST_ASSERT("delete_rcz: placement 3 still remains",
	    test_placement(84, 3) != NULL);

	test_graphics("a=d,d=z,z=3", NULL);
	TEST_ASSERT("delete_rcz: placement 3 gone",
	    test_placement(84, 3) == NULL);
	TEST_ASSERT("delete_rcz: data remains", test_image(84) != NULL);
	TEST_PASS("delete_rcz");
	return (0);
}

/* Deleting by image number targets the newest image with that number. */
static int
test_delete_selector_number_newest(void)
{
	const char	*reply;

	test_pane_new();
	test_graphics("a=T,I=9,q=2,f=32,s=1,v=1,c=1,r=1,C=1", TEST_PIXEL);
	test_cursor(2, 0);
	test_graphics("a=T,I=9,q=2,f=32,s=1,v=1,c=1,r=1,C=1", TEST_PIXEL);
	TEST_ASSERT("delete_n: placed", test_placement_at(1, 0, 0, 0) &&
	    test_placement_at(2, 0, 2, 0));

	reply = test_graphics("a=d,d=n,I=9", NULL);
	TEST_ASSERT("delete_n: no reply", *reply == '\0');
	TEST_ASSERT("delete_n: newest placement gone",
	    test_count_placements(2) == 0 && !test_placed_at(2, 0));
	TEST_ASSERT("delete_n: older placement remains",
	    test_placement_at(1, 0, 0, 0));
	TEST_ASSERT("delete_n: data remains", test_map(2) != NULL);

	test_graphics("a=d,d=N,I=9", NULL);
	TEST_ASSERT("delete_n: newest data gone", test_map(2) == NULL);
	TEST_ASSERT("delete_n: older now newest",
	    test_map_number(9) == test_map(1) && test_map(1) != NULL);

	test_graphics("a=d,d=N,I=9", NULL);
	TEST_ASSERT("delete_n: older data gone", test_map(1) == NULL &&
	    !test_placed_at(0, 0));
	TEST_PASS("delete_n");
	return (0);
}

/* Deleting an ID range removes those images and their data. */
static int
test_delete_selector_id_range(void)
{
	char	control[64];
	u_int	i;

	test_pane_new();
	for (i = 100; i <= 102; i++) {
		test_store(i);
		xsnprintf(control, sizeof control, "i=%u,c=1,r=1,q=2", i);
		test_place(i - 100, 0, control);
	}
	test_graphics("a=d,d=R,x=100,y=101", NULL);
	TEST_ASSERT("delete_range: 100 gone",
	    test_map(100) == NULL && !test_placed_at(0, 0));
	TEST_ASSERT("delete_range: 101 gone",
	    test_map(101) == NULL && !test_placed_at(1, 0));
	TEST_ASSERT("delete_range: 102 remains",
	    test_placement_at(102, 0, 2, 0));

	for (i = 1; i <= 3; i++) {
		test_store(i);
		xsnprintf(control, sizeof control, "i=%u,c=1,r=1,q=2", i);
		test_place(i, 1, control);
	}
	test_graphics("a=d,d=R,y=2", NULL);
	TEST_ASSERT("delete_range: omitted low 1 gone", test_map(1) == NULL);
	TEST_ASSERT("delete_range: omitted low 2 gone", test_map(2) == NULL);
	TEST_ASSERT("delete_range: omitted low 3 remains",
	    test_placement_at(3, 0, 3, 1));

	test_store(200);
	test_store(201);
	test_place(0, 2, "i=201,c=1,r=1,q=2");
	test_graphics("a=d,d=R,x=201,y=201", NULL);
	TEST_ASSERT("delete_range: unplaced remains", test_map(200) != NULL);
	TEST_ASSERT("delete_range: targeted gone",
	    test_map(201) == NULL && !test_placed_at(0, 2));
	TEST_PASS("delete_range");
	return (0);
}

/* A delete aborts an unfinished chunked upload. */
static int
test_delete_aborts_chunked_upload(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=T,i=110,f=24,s=1,v=1,m=1", TEST_RGB);
	TEST_ASSERT("delete_abort: first no reply", *reply == '\0');
	TEST_ASSERT("delete_abort: pending", !test_idle());

	reply = test_graphics("a=d,d=a", NULL);
	TEST_ASSERT("delete_abort: delete no reply", *reply == '\0');
	TEST_ASSERT("delete_abort: pending cleared", test_idle());
	TEST_ASSERT("delete_abort: no partial image", test_map(110) == NULL);

	reply = test_graphics("m=0", NULL);
	TEST_ASSERT("delete_abort: final chunk ignored", *reply == '\0' &&
	    test_map(110) == NULL && !test_placed_at(0, 0));
	TEST_PASS("delete_abort");
	return (0);
}

/* The oldest images are removed when the count limit is reached. */
static int
test_memory_limits_images(void)
{
	char		 control[64], expected[32];
	const char	*reply;
	u_int		 i;

	test_pane_new();
	options_set_number(global_options, "image-total-count-limit", 4);
	for (i = 1; i <= 9; i++) {
		xsnprintf(control, sizeof control, "a=t,i=%u,f=32,s=1,v=1", i);
		xsnprintf(expected, sizeof expected, "i=%u;OK", i);
		reply = test_graphics(control, TEST_PIXEL);
		TEST_ASSERT("memlimit_images: stored",
		    test_replied(reply, expected));
	}
	TEST_ASSERT("memlimit_images: limited", test_count_images() == 4);
	TEST_ASSERT("memlimit_images: newest kept", test_map(9) != NULL &&
	    test_map(6) != NULL);
	TEST_ASSERT("memlimit_images: oldest removed", test_map(1) == NULL &&
	    test_map(5) == NULL);
	TEST_PASS("memlimit_images");
	return (0);
}

/* Replacing an image at the count limit does not remove other images. */
static int
test_memory_limits_replace_at_limit(void)
{
	const char	*reply;

	test_pane_new();
	options_set_number(global_options, "image-total-count-limit", 2);
	test_store(1);
	test_store(2);
	TEST_ASSERT("memlimit_replace: stored", test_count_images() == 2);

	reply = test_graphics("a=t,i=1,f=32,s=1,v=1", TEST_GREEN);
	TEST_ASSERT("memlimit_replace: replace reply",
	    test_replied(reply, "i=1;OK"));
	TEST_ASSERT("memlimit_replace: replaced payload",
	    test_pixel(1, 0, 0, test_green));
	TEST_ASSERT("memlimit_replace: other kept", test_map(2) != NULL);

	reply = test_graphics("a=t,i=999,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("memlimit_replace: new image reply",
	    test_replied(reply, "i=999;OK"));
	TEST_ASSERT("memlimit_replace: limited", test_count_images() == 2);
	TEST_ASSERT("memlimit_replace: oldest removed", test_map(2) == NULL &&
	    test_map(1) != NULL && test_map(999) != NULL);
	TEST_PASS("memlimit_replace");
	return (0);
}

/* Payloads larger than the image size limit are rejected. */
static int
test_memory_limits_payload(void)
{
	const char	*reply;

	test_pane_new();
	options_set_number(global_options, "image-size-limit", 64);
	reply = test_graphics("a=t,i=1,f=32,s=5,v=5",
	    test_fill(5, 5, test_red));
	TEST_ASSERT("memlimit_payload: too large EINVAL",
	    test_replied(reply, "i=1;EINVAL") && test_map(1) == NULL);
	reply = test_graphics("a=t,i=2,f=32,s=2,v=2",
	    test_fill(2, 2, test_red));
	TEST_ASSERT("memlimit_payload: small ok",
	    test_replied(reply, "i=2;OK") && test_size(2, 2, 2));
	TEST_PASS("memlimit_payload");
	return (0);
}

/* Replies carry the image ID, number and placement ID. */
static int
test_build_reply(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q,i=42,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("build_reply: id", test_replied(reply, "i=42;OK"));
	reply = test_graphics("a=q,i=42,p=3,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("build_reply: placement",
	    test_replied(reply, "i=42,p=3;OK"));
	reply = test_graphics("a=t,I=7,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("build_reply: number",
	    test_replied(reply, "i=1,I=7;OK"));
	reply = test_graphics("a=p,I=99", NULL);
	TEST_ASSERT("build_reply: number without id",
	    test_replied(reply, "I=99;ENOENT"));
	TEST_PASS("build_reply");
	return (0);
}

/* A query without an ID replies with ID zero. */
static int
test_build_reply_zero(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("build_reply_zero: i=0", test_replied(reply, "i=0;OK"));
	TEST_PASS("build_reply_zero");
	return (0);
}

/* A quiet query sends no OK reply. */
static int
test_handle_query_quiet(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q,i=1,f=24,s=1,v=1,q=1", TEST_RGB);
	TEST_ASSERT("query_quiet: no reply", *reply == '\0');
	TEST_ASSERT("query_quiet: not stored", test_count_images() == 0);
	TEST_PASS("query_quiet");
	return (0);
}

/* A query replies OK. */
static int
test_handle_query_loud(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q,i=1,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("query_loud: OK", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("query_loud: not stored", test_count_images() == 0);
	TEST_PASS("query_loud");
	return (0);
}

/* A query without an ID or data fails with ID zero. */
static int
test_handle_query_missing_id(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q", NULL);
	TEST_ASSERT("query_missing: EINVAL", test_replied(reply, "i=0;EINVAL"));
	TEST_PASS("query_missing");
	return (0);
}

/* A query validates image data but does not store it. */
static int
test_handle_query_validation(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=q,i=151,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("query_validation: valid OK",
	    test_replied(reply, "i=151;OK"));
	TEST_ASSERT("query_validation: no image stored",
	    test_count_images() == 0);

	reply = test_graphics("a=q,i=152,f=24,s=1,v=1", "QQ==");
	TEST_ASSERT("query_validation: short EINVAL",
	    test_replied(reply, "i=152;EINVAL"));

	reply = test_graphics("a=q,i=153,f=24,t=s,s=1,v=1", TEST_RGB);
	TEST_ASSERT("query_validation: shm ENOTSUP",
	    test_replied(reply, "i=153;ENOTSUP"));

	reply = test_graphics("a=q,i=154,q=2,f=24,s=1,v=1", "QQ==");
	TEST_ASSERT("query_validation: q2 no reply", *reply == '\0');

	reply = test_graphics("a=q,i=155,f=32,s=1,v=1,m=1", "/wAA");
	TEST_ASSERT("query_validation: chunk first no reply", *reply == '\0');
	reply = test_graphics("m=0", "/w==");
	TEST_ASSERT("query_validation: chunk final OK",
	    test_replied(reply, "i=155;OK"));
	TEST_ASSERT("query_validation: chunk no image stored",
	    test_count_images() == 0);
	TEST_PASS("query_validation");
	return (0);
}

/* A transmit without an ID or number sends no reply and stores nothing. */
static int
test_handle_transmit_empty(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t", NULL);
	TEST_ASSERT("transmit_empty: invalid no reply", *reply == '\0');
	reply = test_graphics("a=t,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("transmit_empty: valid no reply", *reply == '\0');
	TEST_ASSERT("transmit_empty: not stored", test_count_images() == 0);
	TEST_PASS("transmit_empty");
	return (0);
}

/* A file transfer without a path fails. */
static int
test_handle_transmit_file_no_path(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,t=f", NULL);
	TEST_ASSERT("transmit_file_no_path: EBADF",
	    test_replied(reply, "i=1;EBADF"));
	TEST_ASSERT("transmit_file_no_path: not stored", test_map(1) == NULL);
	TEST_PASS("transmit_file_no_path");
	return (0);
}

/* A shared memory transfer is not supported. */
static int
test_handle_transmit_shm(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,t=s", NULL);
	TEST_ASSERT("transmit_shm: ENOTSUP",
	    test_replied(reply, "i=1;ENOTSUP"));
	reply = test_graphics("a=t,i=133,t=s,m=1", "AAAA");
	TEST_ASSERT("transmit_shm: m ignored ENOTSUP",
	    test_replied(reply, "i=133;ENOTSUP"));
	TEST_ASSERT("transmit_shm: m ignored idle", test_idle());
	TEST_ASSERT("transmit_shm: not stored", test_count_images() == 0);
	TEST_PASS("transmit_shm");
	return (0);
}

/* A transmit-and-display moves the cursor past the placement. */
static int
test_handle_transmit_cursor_move(void)
{
	const char	*reply;

	test_pane_new();
	test_cursor(5, 10);
	reply = test_graphics("a=T,i=1,f=32,s=1,v=1,c=2,r=3", TEST_PIXEL);
	TEST_ASSERT("transmit_cursor: reply", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("transmit_cursor: placement position",
	    test_placement_at(1, 0, 5, 10) && test_placement_at(1, 0, 6, 12) &&
	    !test_placed_at(4, 10) && !test_placed_at(5, 9));
	TEST_ASSERT("transmit_cursor: cursor moved",
	    test_wp->base.cx == 7 && test_wp->base.cy == 12);
	TEST_PASS("transmit_cursor");
	return (0);
}

/* A transmit-and-display with C=1 does not move the cursor. */
static int
test_handle_transmit_cursor_stay(void)
{
	const char	*reply;

	test_pane_new();
	test_cursor(5, 10);
	reply = test_graphics("a=T,i=1,f=32,s=1,v=1,c=2,r=3,C=1", TEST_PIXEL);
	TEST_ASSERT("transmit_stay: reply", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("transmit_stay: placed", test_placement_at(1, 0, 6, 12));
	TEST_ASSERT("transmit_stay: cursor unchanged",
	    test_wp->base.cx == 5 && test_wp->base.cy == 10);
	TEST_PASS("transmit_stay");
	return (0);
}

/* A virtual transmit-and-display stores the image without placing it. */
static int
test_handle_transmit_virtual(void)
{
	const char	*reply;

	test_pane_new();
	test_cursor(3, 2);
	reply = test_graphics("a=T,i=1,f=32,s=1,v=1,U=1", TEST_PIXEL);
	TEST_ASSERT("transmit_virtual: OK", test_replied(reply, "i=1;OK"));
	TEST_ASSERT("transmit_virtual: stored", test_image(1) != NULL);
	TEST_ASSERT("transmit_virtual: virtual", test_virtual(1, 0) != NULL);
	TEST_ASSERT("transmit_virtual: no cells", !test_placed_at(3, 2));
	TEST_ASSERT("transmit_virtual: cursor unchanged",
	    test_wp->base.cx == 3 && test_wp->base.cy == 2);
	reply = test_graphics("a=T,i=2,f=32,s=1,v=1,U=1,P=1", TEST_PIXEL);
	TEST_ASSERT("transmit_virtual: parent EINVAL",
	    test_replied(reply, "i=2;EINVAL") && test_image(2) == NULL);
	TEST_PASS("transmit_virtual");
	return (0);
}

/* Placing a missing image fails. */
static int
test_handle_place_missing(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=p,i=99", NULL);
	TEST_ASSERT("place_missing: ENOENT",
	    test_replied(reply, "i=99;ENOENT"));
	reply = test_graphics("a=p,I=99,p=3", NULL);
	TEST_ASSERT("place_missing: number ENOENT",
	    test_replied(reply, "I=99,p=3;ENOENT"));
	TEST_ASSERT("place_missing: nothing placed", !test_placed_at(0, 0));
	TEST_PASS("place_missing");
	return (0);
}

/* Placing without an ID or number does nothing and sends no reply. */
static int
test_handle_place_no_reference(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	reply = test_graphics("a=p", NULL);
	TEST_ASSERT("place_no_ref: no reply", *reply == '\0');
	TEST_ASSERT("place_no_ref: nothing placed",
	    !test_placed_at(0, 0) && test_count_placements(1) == 0);
	TEST_ASSERT("place_no_ref: cursor unchanged",
	    test_wp->base.cx == 0 && test_wp->base.cy == 0);
	TEST_PASS("place_no_ref");
	return (0);
}

/* A relative placement starts at its parent's origin plus H and V. */
static int
test_relative_position(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_store(2);
	test_place(10, 5, "i=1,p=1,c=2,r=2,q=2");
	test_cursor(0, 0);
	reply = test_graphics("a=p,i=2,p=1,P=1,Q=1,H=3,V=-2,c=1,r=1", NULL);
	TEST_ASSERT("relative_position: OK", test_replied(reply, "i=2,p=1;OK"));
	TEST_ASSERT("relative_position: placed",
	    test_placement_at(2, 1, 13, 3) && !test_placed_at(12, 3) &&
	    !test_placed_at(13, 4));
	TEST_ASSERT("relative_position: cursor unchanged",
	    test_wp->base.cx == 0 && test_wp->base.cy == 0);

	/* Without Q, the parent is the placement with the lowest ID. */
	test_place(30, 10, "i=1,p=7,c=1,r=1,q=2");
	reply = test_graphics("a=T,i=3,p=2,f=32,s=1,v=1,P=1,H=-1",
	    TEST_PIXEL);
	TEST_ASSERT("relative_position: T OK",
	    test_replied(reply, "i=3,p=2;OK"));
	TEST_ASSERT("relative_position: lowest parent ID",
	    test_placement_at(3, 2, 9, 5) && !test_placed_at(29, 10));

	/* A parent placed partly off the grid still has an origin. */
	reply = test_graphics("a=p,i=2,p=3,P=1,Q=1,H=-12,V=0,c=4,r=1", NULL);
	TEST_ASSERT("relative_position: clipped",
	    test_placement_at(2, 3, 0, 5) && test_placement_at(2, 3, 1, 5) &&
	    !test_placed_at(2, 5));
	TEST_PASS("relative_position");
	return (0);
}

/* Relative placements report missing parents, cycles and deep chains. */
static int
test_relative_errors(void)
{
	const char	*reply;
	char		*s;
	u_int		 i;

	test_pane_new();
	test_store(1);
	test_store(2);
	test_place(0, 0, "i=1,p=1,q=2");
	reply = test_graphics("a=p,i=2,p=1,P=99", NULL);
	TEST_ASSERT("relative_errors: no parent image",
	    test_replied(reply, "i=2,p=1;ENOPARENT"));
	reply = test_graphics("a=p,i=2,p=1,P=1,Q=9", NULL);
	TEST_ASSERT("relative_errors: no parent placement",
	    test_replied(reply, "i=2,p=1;ENOPARENT"));
	reply = test_graphics("a=p,i=1,p=1,P=1,Q=1", NULL);
	TEST_ASSERT("relative_errors: self", test_replied(reply,
	    "i=1,p=1;EINVAL") && test_placement_at(1, 1, 0, 0));
	reply = test_graphics("a=p,i=2,p=1,P=1,Q=1,U=1", NULL);
	TEST_ASSERT("relative_errors: virtual",
	    test_replied(reply, "i=2,p=1;EINVAL"));
	TEST_ASSERT("relative_errors: nothing placed",
	    test_count_placements(2) == 0 && test_virtual(2, 1) == NULL);

	/* Making the parent relative to its own child is a cycle. */
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=1,q=2", NULL);
	reply = test_graphics("a=p,i=1,p=1,P=2,Q=1", NULL);
	TEST_ASSERT("relative_errors: cycle",
	    test_replied(reply, "i=1,p=1;ECYCLE"));
	TEST_ASSERT("relative_errors: cycle keeps both",
	    test_placement_at(1, 1, 0, 0) && test_placement_at(2, 1, 1, 0));

	/* A chain of eight links is allowed, nine is too deep. */
	test_store(10);
	test_place(0, 10, "i=10,p=1,q=2");
	for (i = 11; i <= 19; i++) {
		test_store(i);
		xasprintf(&s, "a=p,i=%u,p=1,P=%u,Q=1,H=1", i, i - 1);
		reply = test_graphics(s, NULL);
		free(s);
		if (i < 19) {
			TEST_ASSERT("relative_errors: chain allowed",
			    test_placement_at(i, 1, i - 10, 10));
		}
	}
	TEST_ASSERT("relative_errors: too deep",
	    test_replied(reply, "i=19,p=1;ETOODEEP") &&
	    test_count_placements(19) == 0);
	TEST_PASS("relative_errors");
	return (0);
}

/* Removing a parent removes its relative placements too. */
static int
test_relative_cascade(void)
{
	test_pane_new();
	test_store(1);
	test_store(2);
	test_store(3);
	test_place(5, 5, "i=1,p=1,q=2");
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=1,q=2", NULL);
	test_graphics("a=p,i=3,p=1,P=2,Q=1,V=1,q=2", NULL);
	TEST_ASSERT("relative_cascade: placed",
	    test_placement_at(2, 1, 6, 5) && test_placement_at(3, 1, 6, 6));

	/* Deleting by area does not match relative placements. */
	test_graphics("a=d,d=p,x=7,y=6", NULL);
	TEST_ASSERT("relative_cascade: area keeps child",
	    test_placement_at(2, 1, 6, 5));

	test_graphics("a=d,d=i,i=1", NULL);
	TEST_ASSERT("relative_cascade: all removed",
	    !test_placed_at(5, 5) && !test_placed_at(6, 5) &&
	    !test_placed_at(6, 6) && test_count_placements(2) == 0 &&
	    test_count_placements(3) == 0);
	TEST_ASSERT("relative_cascade: images kept",
	    test_image(2) != NULL && test_image(3) != NULL);

	/* An uppercase delete frees the images left without placements. */
	test_place(5, 5, "i=1,p=1,q=2");
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=1,q=2", NULL);
	test_graphics("a=p,i=3,p=1,P=2,Q=1,V=1,q=2", NULL);
	test_graphics("a=d,d=I,i=1", NULL);
	TEST_ASSERT("relative_cascade: images freed",
	    test_image(1) == NULL && test_image(2) == NULL &&
	    test_image(3) == NULL && !test_placed_at(6, 6));

	/* Retransmitting the parent's image removes its placements. */
	test_store(1);
	test_store(2);
	test_place(5, 5, "i=1,p=1,q=2");
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=1,q=2", NULL);
	test_store(1);
	TEST_ASSERT("relative_cascade: retransmit",
	    !test_placed_at(6, 5) && test_count_placements(2) == 0);

	/* Deleting by z-index matches relative placements. */
	test_place(5, 5, "i=1,p=1,q=2");
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=1,z=3,q=2", NULL);
	test_graphics("a=d,d=z,z=3", NULL);
	TEST_ASSERT("relative_cascade: z-index",
	    !test_placed_at(6, 5) && test_placement_at(1, 1, 5, 5));
	TEST_PASS("relative_cascade");
	return (0);
}

/* Relative placements follow a replaced parent and scroll with it. */
static int
test_relative_follow(void)
{
	u_int	i;

	test_pane_new();
	test_history(100);
	test_store(1);
	test_store(2);
	test_store(3);
	test_place(5, 5, "i=1,p=1,q=2");
	test_graphics("a=p,i=2,p=1,P=1,Q=1,H=2,q=2", NULL);
	test_graphics("a=p,i=3,p=1,P=2,Q=1,V=2,q=2", NULL);
	test_place(20, 10, "i=1,p=1,q=2");
	TEST_ASSERT("relative_follow: replaced parent",
	    test_placement_at(2, 1, 22, 10) &&
	    test_placement_at(3, 1, 22, 12) &&
	    !test_placed_at(7, 5) && !test_placed_at(7, 7));

	test_cursor(0, 23);
	for (i = 0; i < 3; i++)
		test_input("\n");
	TEST_ASSERT("relative_follow: scrolled",
	    test_placement_at(1, 1, 20, 7) && test_placement_at(2, 1, 22, 7) &&
	    test_placement_at(3, 1, 22, 9));
	TEST_PASS("relative_follow");
	return (0);
}

/* Animation actions are not supported. */
static int
test_unsupported_animation_actions(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=f,i=145,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("unsupported_actions: frame ENOTSUP",
	    test_replied(reply, "i=145;ENOTSUP"));
	reply = test_graphics("a=a,i=145", NULL);
	TEST_ASSERT("unsupported_actions: animation ENOTSUP",
	    test_replied(reply, "i=145;ENOTSUP"));
	reply = test_graphics("a=c,i=145", NULL);
	TEST_ASSERT("unsupported_actions: compose ENOTSUP",
	    test_replied(reply, "i=145;ENOTSUP"));
	reply = test_graphics("a=f,i=146,q=2,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("unsupported_actions: quiet no reply", *reply == '\0');
	reply = test_graphics("a=f,i=147,f=24,s=1,v=1,m=1", TEST_RGB);
	TEST_ASSERT("unsupported_actions: chunk ENOTSUP",
	    test_replied(reply, "i=147;ENOTSUP"));
	TEST_ASSERT("unsupported_actions: idle", test_idle());
	TEST_ASSERT("unsupported_actions: no images", test_count_images() == 0);
	TEST_ASSERT("unsupported_actions: no placements",
	    !test_placed_at(0, 0));
	TEST_PASS("unsupported_actions");
	return (0);
}

/* Unknown actions are invalid and are not buffered. */
static int
test_unknown_actions_rejected(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=Z,i=148", NULL);
	TEST_ASSERT("unknown_actions: EINVAL",
	    test_replied(reply, "i=148;EINVAL"));
	reply = test_graphics("a=Z,i=149,m=1", TEST_RGB);
	TEST_ASSERT("unknown_actions: chunk EINVAL",
	    test_replied(reply, "i=149;EINVAL"));
	TEST_ASSERT("unknown_actions: idle", test_idle());
	TEST_ASSERT("unknown_actions: no images", test_count_images() == 0);
	TEST_PASS("unknown_actions");
	return (0);
}

/* An image ID and an image number cannot be used together. */
static int
test_image_id_number_conflict(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,I=2,f=24,s=1,v=1", TEST_RGB);
	TEST_ASSERT("id_number_conflict: transmit EINVAL",
	    test_replied(reply, "i=1,I=2;EINVAL"));
	TEST_ASSERT("id_number_conflict: transmit no image",
	    test_count_images() == 0);
	test_store(1);
	reply = test_graphics("a=p,i=1,I=2,p=3", NULL);
	TEST_ASSERT("id_number_conflict: place EINVAL",
	    test_replied(reply, "i=1,I=2,p=3;EINVAL"));
	TEST_ASSERT("id_number_conflict: place no placement",
	    test_count_placements(1) == 0 && !test_placed_at(0, 0));
	TEST_PASS("id_number_conflict");
	return (0);
}

/* Placing with an existing placement ID replaces that placement. */
static int
test_handle_place_replace(void)
{
	struct kitty_placement_map	*kpl;
	struct image_geom		*ig;
	const char			*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,p=5,c=10,r=5,q=2");
	TEST_ASSERT("place_replace: initial", test_placement_at(1, 5, 0, 0));

	test_cursor(3, 4);
	reply = test_graphics("a=p,i=1,p=5,c=2,r=2", NULL);
	TEST_ASSERT("place_replace: reply", test_replied(reply, "i=1,p=5;OK"));
	TEST_ASSERT("place_replace: still one", test_count_placements(1) == 1);
	kpl = test_placement(1, 5);
	TEST_ASSERT("place_replace: found", kpl != NULL);
	ig = image_get_geometry(image_find(kpl->image_id));
	TEST_ASSERT("place_replace: size updated", ig->sx == 2 && ig->sy == 2);
	TEST_ASSERT("place_replace: moved", test_placement_at(1, 5, 3, 4) &&
	    test_placement_at(1, 5, 4, 5) && !test_placed_at(0, 0) &&
	    !test_placed_at(5, 4));
	TEST_PASS("place_replace");
	return (0);
}

/* Check the cells and fit of a virtual placement. */
static int
test_virtual_fit(u_int id, u_int p, u_int sx, u_int sy, u_int xoff,
    u_int yoff, double w, double h)
{
	struct kitty_placement_map	*kpl = test_virtual(id, p);
	struct image_geom		*ig;

	if (kpl == NULL)
		return (0);
	ig = image_get_geometry(image_find(kpl->image_id));
	return (ig->sx == sx && ig->sy == sy && ig->canvas_xoff == xoff &&
	    ig->canvas_yoff == yoff && fabs(ig->scaled_width - w) < 0.001 &&
	    fabs(ig->scaled_height - h) < 0.001);
}

/* A virtual placement fits the whole image to its cells. */
static int
test_handle_place_virtual(void)
{
	const char	*reply;

	test_pane_new();
	test_graphics("a=t,i=1,f=32,s=2,v=1,q=2", test_fill(2, 1, test_red));
	test_cursor(4, 4);
	reply = test_graphics("a=p,i=1,p=3,U=1,c=2,r=2,x=1,X=3", NULL);
	TEST_ASSERT("place_virtual: OK", test_replied(reply, "i=1,p=3;OK"));
	TEST_ASSERT("place_virtual: no cells",
	    test_count_placements(1) == 0 && !test_placed_at(4, 4));
	TEST_ASSERT("place_virtual: cursor unchanged",
	    test_wp->base.cx == 4 && test_wp->base.cy == 4);
	TEST_ASSERT("place_virtual: wide centred vertically",
	    test_virtual_fit(1, 3, 2, 2, 0, 12, 16, 8));

	test_graphics("a=t,i=2,f=32,s=1,v=4,q=2", test_fill(1, 4, test_red));
	test_graphics("a=p,i=2,U=1,c=2,r=1,q=2", NULL);
	TEST_ASSERT("place_virtual: tall centred horizontally",
	    test_virtual_fit(2, 0, 2, 1, 6, 0, 4, 16));

	test_graphics("a=t,i=3,f=32,s=12,v=20,q=2",
	    test_fill(12, 20, test_red));
	test_graphics("a=p,i=3,U=1,q=2", NULL);
	TEST_ASSERT("place_virtual: natural size",
	    test_virtual_fit(3, 0, 2, 2, 0, 3, 16, 80.0 / 3));

	/* Area deletes leave virtual placements; image deletes remove them. */
	test_graphics("a=d,d=a", NULL);
	TEST_ASSERT("place_virtual: kept by d=a", test_virtual(1, 3) != NULL);
	test_graphics("a=d,d=i,i=1,p=3", NULL);
	TEST_ASSERT("place_virtual: deleted",
	    test_virtual(1, 3) == NULL && test_image(1) != NULL);
	test_graphics("a=d,d=I,i=2", NULL);
	TEST_ASSERT("place_virtual: image freed",
	    test_virtual(2, 0) == NULL && test_image(2) == NULL);
	TEST_PASS("place_virtual");
	return (0);
}

/* Placeholder cells refer to images by colour and to cells by diacritic. */
static int
test_placeholder_cells(void)
{
	struct grid_cell	 gc;
	char			 s[256];

	test_pane_new();
	test_store(42);
	test_graphics("a=p,i=42,U=1,c=3,r=2,q=2", NULL);

	/* Each placeholder and its diacritics are one cell of width one. */
	test_input("\033[38;5;42m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input(test_placeholder(s, sizeof s, 0, 1, -1));
	test_input(test_placeholder(s, sizeof s, 0, 2, -1));
	grid_view_get_cell(test_wp->base.grid, 1, 0, &gc);
	TEST_ASSERT("placeholder_cells: combined",
	    test_wp->base.cx == 3 && gc.data.width == 1 &&
	    gc.data.size == 8 && kitty_is_placeholder(&gc.data));
	TEST_ASSERT("placeholder_cells: explicit",
	    test_resolves(0, 0, 42, 0, 0, 0) &&
	    test_resolves(2, 0, 42, 0, 2, 0));

	/* Missing columns continue the run from the left. */
	test_cursor(0, 1);
	test_input(test_placeholder(s, sizeof s, 1, -1, -1));
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	TEST_ASSERT("placeholder_cells: inferred",
	    test_resolves(1, 1, 42, 0, 1, 1) &&
	    test_resolves(2, 1, 42, 0, 2, 1) && !test_resolves_any(3, 1));

	/* A different colour starts a new run at the first column. */
	test_cursor(0, 2);
	test_input(test_placeholder(s, sizeof s, 1, 1, -1));
	test_input("\033[38;5;43m");
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	test_input("\033[38;5;42m");
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	TEST_ASSERT("placeholder_cells: new run",
	    test_resolves(0, 2, 42, 0, 1, 1) && !test_resolves_any(1, 2) &&
	    test_resolves(2, 2, 42, 0, 0, 0));

	/* The third diacritic is the high byte of the image ID. */
	test_store(42 + (3 << 24));
	test_graphics("a=p,i=50331690,U=1,c=2,r=1,q=2", NULL);
	test_cursor(0, 3);
	test_input(test_placeholder(s, sizeof s, 0, 0, 3));
	test_input(test_placeholder(s, sizeof s, -1, -1, -1));
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("placeholder_cells: high byte",
	    test_resolves(0, 3, 50331690, 0, 0, 0) &&
	    test_resolves(1, 3, 50331690, 0, 1, 0) &&
	    test_resolves(2, 3, 42, 0, 0, 0));

	/* True colour and basic colours give the image ID too. */
	test_store(0x123456);
	test_graphics("a=p,i=1193046,U=1,q=2", NULL);
	test_store(1);
	test_graphics("a=p,i=1,U=1,q=2", NULL);
	test_store(9);
	test_graphics("a=p,i=9,U=1,q=2", NULL);
	test_cursor(0, 4);
	test_input("\033[38;2;18;52;86m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[31m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[91m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[39m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("placeholder_cells: colour forms",
	    test_resolves(0, 4, 0x123456, 0, 0, 0) &&
	    test_resolves(1, 4, 1, 0, 0, 0) &&
	    test_resolves(2, 4, 9, 0, 0, 0) && !test_resolves_any(3, 4));

	/* The underline colour selects the placement. */
	test_store(7);
	test_graphics("a=p,i=7,p=6,U=1,c=2,r=2,q=2", NULL);
	test_graphics("a=p,i=7,p=5,U=1,c=1,r=1,q=2", NULL);
	test_cursor(0, 5);
	test_input("\033[38;5;7;58:5:6m");
	test_input(test_placeholder(s, sizeof s, 1, 1, -1));
	test_input("\033[58:5:5m");
	test_input(test_placeholder(s, sizeof s, 1, 1, -1));
	test_input("\033[59m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[58:5:8m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("placeholder_cells: placement ID",
	    test_resolves(0, 5, 7, 6, 1, 1) && !test_resolves_any(1, 5) &&
	    test_resolves(2, 5, 7, 5, 0, 0) && !test_resolves_any(3, 5));

	/* Unknown images and missing virtual placements show nothing. */
	test_store(8);
	test_cursor(0, 6);
	test_input("\033[59;38;5;99m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[38;5;8m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("placeholder_cells: unknown",
	    !test_resolves_any(0, 6) && !test_resolves_any(1, 6));

	/* Deleting the virtual placement hides its cells. */
	test_graphics("a=d,d=i,i=42", NULL);
	TEST_ASSERT("placeholder_cells: deleted", !test_resolves_any(0, 0));
	TEST_PASS("placeholder_cells");
	return (0);
}

/* Relative placements of a virtual placement follow its placeholders. */
static int
test_relative_virtual_parent(void)
{
	const char	*reply;
	char		 s[256];

	test_pane_new();
	test_store(50);
	test_store(2);
	test_graphics("a=p,i=50,p=1,U=1,c=1,r=1,q=2", NULL);
	reply = test_graphics("a=p,i=2,p=1,P=50,Q=1,H=1", NULL);
	TEST_ASSERT("relative_virtual: OK", test_replied(reply, "i=2,p=1;OK"));
	TEST_ASSERT("relative_virtual: waits for placeholders",
	    test_count_placements(2) == 0);

	test_cursor(5, 3);
	test_input("\033[38;5;50m");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("relative_virtual: anchored",
	    test_placement_at(2, 1, 6, 3));

	/* The origin is the smallest column and row of all placeholders. */
	test_input("\033[2J\033[8;5H");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	test_input("\033[10;3H");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("relative_virtual: moved",
	    test_placement_at(2, 1, 3, 7) && !test_placed_at(6, 3));

	/* Without placeholders the relative placement stays where it was. */
	test_input("\033[0m\033[8;5H \033[10;3H ");
	TEST_ASSERT("relative_virtual: kept", test_placement_at(2, 1, 3, 7));

	/* Placeholders printed again after a clear bring it back. */
	test_input("\033[2J\033[38;5;50m\033[8;3H");
	test_input(test_placeholder(s, sizeof s, 0, 0, -1));
	TEST_ASSERT("relative_virtual: restored",
	    test_placement_at(2, 1, 3, 7));

	test_graphics("a=d,d=i,i=50", NULL);
	TEST_ASSERT("relative_virtual: removed with parent",
	    test_count_placements(2) == 0 && !test_placed_at(3, 7));
	TEST_PASS("relative_virtual");
	return (0);
}

/* Placing by image number uses the assigned image ID. */
static int
test_handle_place_by_number(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=t,I=42,f=32,s=1,v=1", TEST_PIXEL);
	TEST_ASSERT("place_number: stored",
	    test_replied(reply, "i=1,I=42;OK"));
	reply = test_graphics("a=p,I=42,p=9,c=2,r=3", NULL);
	TEST_ASSERT("place_number: reply ids",
	    test_replied(reply, "i=1,I=42,p=9;OK"));
	TEST_ASSERT("place_number: placement exists",
	    test_placement_at(1, 9, 0, 0) && test_placement_at(1, 9, 1, 2));
	TEST_PASS("place_number");
	return (0);
}

/* A delete without a selector deletes all placements. */
static int
test_handle_delete_default(void)
{
	const char	*reply;

	test_pane_new();
	test_store(1);
	test_place(0, 0, "i=1,c=1,r=1,q=2");
	reply = test_graphics("a=d", NULL);
	TEST_ASSERT("delete_default: no reply", *reply == '\0');
	TEST_ASSERT("delete_default: placement gone",
	    test_count_placements(1) == 0 && !test_placed_at(0, 0));
	TEST_ASSERT("delete_default: data remains", test_image(1) != NULL);
	TEST_PASS("delete_default");
	return (0);
}

/* Quiet mode 1 suppresses OK, quiet mode 2 suppresses everything. */
static int
test_quiet_failure_modes(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=p,i=999,q=1", NULL);
	TEST_ASSERT("quiet_failure: q=1 error replies",
	    test_replied(reply, "i=999;ENOENT"));
	reply = test_graphics("a=p,i=999,q=2", NULL);
	TEST_ASSERT("quiet_failure: q=2 error no reply", *reply == '\0');
	reply = test_graphics("a=t,i=121,f=24,s=1,v=1,q=1", TEST_RGB);
	TEST_ASSERT("quiet_failure: q=1 OK no reply", *reply == '\0');
	reply = test_graphics("a=t,i=122,f=24,s=1,v=1,q=2", TEST_RGB);
	TEST_ASSERT("quiet_failure: q=2 OK no reply", *reply == '\0');
	TEST_ASSERT("quiet_failure: images stored",
	    test_map(121) != NULL && test_map(122) != NULL);
	TEST_PASS("quiet_failure");
	return (0);
}

/* Chunked uploads collect data until the last chunk. */
static int
test_chunked_upload(void)
{
	struct kitty_state	*ks;
	const char		*reply;

	test_pane_new();
	reply = test_graphics("a=t,i=1,f=24,s=1,v=2,m=1", TEST_RGB);
	TEST_ASSERT("chunked_upload: m=1 no reply", *reply == '\0');
	ks = test_state();
	TEST_ASSERT("chunked_upload: pending", ks != NULL && ks->cmd.more &&
	    ks->cmd.image_id == 1 && ks->size == 3);
	reply = test_graphics("m=0", "AP8A");
	TEST_ASSERT("chunked_upload: m=0 completes",
	    test_replied(reply, "i=1;OK") && test_idle());
	TEST_ASSERT("chunked_upload: data appended",
	    test_pixel(1, 0, 0, test_red) && test_pixel(1, 0, 1, test_green));

	reply = test_graphics("a=t,i=130,f=24,s=1,v=2,m=1,q=1", TEST_RGB);
	TEST_ASSERT("chunked_upload: q1 first no reply", *reply == '\0');
	reply = test_graphics("m=0", "AP8A");
	TEST_ASSERT("chunked_upload: q1 final no reply", *reply == '\0');
	TEST_ASSERT("chunked_upload: q1 stored", test_map(130) != NULL);

	reply = test_graphics("a=t,i=137,f=24,s=1,v=2,m=1,q=0", TEST_RGB);
	TEST_ASSERT("chunked_upload: q increase first no reply",
	    *reply == '\0');
	reply = test_graphics("m=0,q=1", "AP8A");
	TEST_ASSERT("chunked_upload: q increase final no reply",
	    *reply == '\0');
	TEST_ASSERT("chunked_upload: q increase stored", test_map(137) != NULL);

	reply = test_graphics("a=t,i=131,f=24,s=2,v=2,m=1,q=2", TEST_RGB);
	TEST_ASSERT("chunked_upload: q2 first no reply", *reply == '\0');
	reply = test_graphics("m=0", "AP8A");
	TEST_ASSERT("chunked_upload: q2 error no reply", *reply == '\0');
	TEST_ASSERT("chunked_upload: q2 not stored", test_map(131) == NULL);
	TEST_ASSERT("chunked_upload: q2 idle", test_idle());
	TEST_PASS("chunked_upload");
	return (0);
}

/* A transmit-and-display places the image and moves the cursor. */
static int
test_transmit_display(void)
{
	const char	*reply;

	test_pane_new();
	reply = test_graphics("a=T,i=5,f=32,s=1,v=1,c=2,r=1", TEST_PIXEL);
	TEST_ASSERT("transmit_display: reply",
	    strcmp(reply, "\033_Gi=5;OK\033\\") == 0);
	TEST_ASSERT("transmit_display: one image", test_count_images() == 1);
	TEST_ASSERT("transmit_display: placed", test_placed_at(0, 0) &&
	    test_placed_at(1, 0) && !test_placed_at(2, 0));
	TEST_ASSERT("transmit_display: cursor",
	    test_wp->base.cx == 2 && test_wp->base.cy == 0);
	TEST_ASSERT("transmit_display: layer", test_zindex_at(0, 0) == 0);
	TEST_PASS("transmit_display");
	return (0);
}

/* Text written or erased over a placement leaves the placement in place. */
static int
test_text_over_placement(void)
{
	test_pane_new();
	test_store(7);
	test_place(0, 0, "i=7,c=4,r=2");
	TEST_ASSERT("text_over_placement: placed", test_placed_at(3, 1));
	test_input("\033[1;1Htext\033[2;1Hmore");
	TEST_ASSERT("text_over_placement: after text",
	    test_placed_at(0, 0) && test_placed_at(3, 1));
	test_input("\033[1;1H\033[2K\033[2;2H\033[2X\033[2;1H\033[K");
	TEST_ASSERT("text_over_placement: after erase",
	    test_placed_at(0, 0) && test_placed_at(3, 1));
	TEST_PASS("text_over_placement");
	return (0);
}

int
main(void)
{
	const struct options_table_entry	*oe;
	int					 (*tests[])(void) = {
		/* Parser tests. */
		test_parse_query,
		test_parse_transmit,
		test_parse_place,
		test_parse_delete,
		test_parse_payload,
		test_parse_empty_payload,
		test_parse_unknown_keys,
		test_parse_malformed,
		test_parse_character_values,
		test_parse_quiet,
		test_parse_chunked,
		test_parse_image_number,
		test_parse_source_rect,
		test_parse_relative_placement,
		test_parse_file_range,
		test_parse_virtual,
		test_parse_empty,
		test_parse_semicolon_only,
		test_parse_payload_only,
		test_parse_duplicate_keys,
		test_parse_large_control,
		test_parse_negative_zindex,
		test_parse_negative_unsigned,
		test_parse_multiple_commas,

		/* State tests. */
		test_image_store_find,
		test_image_number_find,
		test_image_store_with_payload,
		test_image_replace,
		test_image_replace_with_payload,
		test_parse_dispatch_transmit_display,
		test_transmit_direct_rgb_decodes,
		test_default_action_transmit,
		test_default_format_rgba,
		test_transmit_invalid_format_compression,
		test_transmit_direct_rgb_size_mismatch,
		test_transmit_direct_png_decodes,
		test_transmit_direct_png_rejects_raw,
		test_transmit_direct_compression_preserved,
		test_transmit_file_regular,
		test_transmit_file_range,
		test_transmit_file_bad_path_payload,
		test_transmit_file_temporary_removes_source,
		test_transmit_file_temporary_keeps_unapproved_source,
		test_chunked_transmit_display_metadata,
		test_anonymous_transmit_display,
		test_anonymous_delete_removes_data,
		test_image_number_newest,
		test_scroll_up_moves_and_removes_placements,
		test_placement_create,
		test_free_all,
		test_free_saved_lists,
		test_alternate_screen_transitions,
		test_scrollback_truncation_high_load,
		test_pane_resize_reflow,
		test_transmit_display,
		test_text_over_placement,

		/* Deletion tests. */
		test_delete_image,
		test_delete_all_placements,
		test_delete_all,
		test_delete_cursor,
		test_delete_zindex,
		test_delete_selector_image_placement,
		test_delete_selector_image_data,
		test_delete_selector_cell,
		test_delete_selector_data_scope,
		test_delete_selector_cell_zindex,
		test_delete_selector_row_column_zindex,
		test_delete_selector_number_newest,
		test_delete_selector_id_range,
		test_delete_aborts_chunked_upload,

		/* Memory limit tests. */
		test_memory_limits_images,
		test_memory_limits_replace_at_limit,
		test_memory_limits_payload,

		/* Handler tests. */
		test_build_reply,
		test_build_reply_zero,
		test_handle_query_quiet,
		test_handle_query_loud,
		test_handle_query_missing_id,
		test_handle_query_validation,
		test_handle_transmit_empty,
		test_handle_transmit_file_no_path,
		test_handle_transmit_shm,
		test_handle_transmit_cursor_move,
		test_handle_transmit_cursor_stay,
		test_handle_transmit_virtual,
		test_handle_place_missing,
		test_handle_place_no_reference,
		test_relative_position,
		test_relative_errors,
		test_relative_cascade,
		test_relative_follow,
		test_relative_virtual_parent,
		test_placeholder_cells,
		test_unsupported_animation_actions,
		test_unknown_actions_rejected,
		test_image_id_number_conflict,
		test_handle_place_replace,
		test_handle_place_virtual,
		test_handle_place_by_number,
		test_handle_delete_default,
		test_quiet_failure_modes,
		test_chunked_upload,
	};
	u_int					 i;

	if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL &&
	    setlocale(LC_CTYPE, "C.UTF-8") == NULL)
		abort();
	global_environ = environ_create();
	global_options = options_create(NULL);
	global_s_options = options_create(NULL);
	global_w_options = options_create(NULL);
	for (oe = options_table; oe->name != NULL; oe++) {
		if (oe->scope & OPTIONS_TABLE_SERVER)
			options_default(global_options, oe);
		if (oe->scope & OPTIONS_TABLE_SESSION)
			options_default(global_s_options, oe);
		if (oe->scope & OPTIONS_TABLE_WINDOW)
			options_default(global_w_options, oe);
	}
	test_base = osdep_event_init();
	socket_path = xstrdup("dummy");
	utf8_update_width_cache();

	for (i = 0; i < nitems(tests); i++)
		tests[i]();

	printf("\n%d tests, %d failed\n", tests_run, tests_failed);
	return (tests_failed != 0);
}
