/* $OpenBSD$ */

/*
 * Kitty graphics protocol unit tests.
 *
 * This file includes image-kitty.c directly to test static functions.
 */

#ifdef KITTY_TEST

#include <sys/types.h>
#include <sys/stat.h>

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

void *
xmalloc(size_t size)
{
	void	*ptr;

	ptr = malloc(size);
	if (ptr == NULL)
		abort();
	return (ptr);
}

void *
xcalloc(size_t nmemb, size_t size)
{
	void	*ptr;

	ptr = calloc(nmemb, size);
	if (ptr == NULL)
		abort();
	return (ptr);
}

void *
xrealloc(void *old, size_t size)
{
	void	*ptr;

	ptr = realloc(old, size);
	if (ptr == NULL)
		abort();
	return (ptr);
}

char *
xstrdup(const char *s)
{
	char	*copy;

	copy = strdup(s);
	if (copy == NULL)
		abort();
	return (copy);
}

char *
xstrndup(const char *s, size_t n)
{
	char	*copy;

	copy = strndup(s, n);
	if (copy == NULL)
		abort();
	return (copy);
}

int
xasprintf(char **ret, const char *fmt, ...)
{
	va_list	 ap;
	int	 n;

	va_start(ap, fmt);
	n = vasprintf(ret, fmt, ap);
	va_end(ap);
	if (n < 0)
		abort();
	return (n);
}

void
log_debug(__unused const char *fmt, ...)
{
}

void
tty_kitty_image_remove_uploaded(__unused struct kitty_image *img)
{
}

/* Pull in the implementation. */
#include "image-kitty.c"

static int	 tests_run = 0;
static int	 tests_failed = 0;

#define TEST_ASSERT(name, cond) do { \
	tests_run++; \
	if (!(cond)) { \
		printf("FAIL: %s\n", name); \
		tests_failed++; \
		return (1); \
	} \
} while (0)

#define TEST_PASS(name) do { \
	printf("PASS: %s\n", name); \
} while (0)

static u_int
test_count_images(struct screen *s)
{
	struct kitty_image	*img;
	u_int			 n = 0;

	TAILQ_FOREACH(img, &s->kitty_images, entry)
		n++;
	return (n);
}

static u_int
test_count_placements(struct screen *s)
{
	struct kitty_placement	*pl;
	u_int			 n = 0;

	TAILQ_FOREACH(pl, &s->kitty_placements, entry)
		n++;
	return (n);
}

/* Test 1: Parse a simple query action. */
static int
test_parse_query(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=q,i=1", strlen("a=q,i=1"), &cmd);

	TEST_ASSERT("parse_query: ret == 0", ret == 0);
	TEST_ASSERT("parse_query: action == q", cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_query: image_id == 1", cmd.image_id == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_query");
	return (0);
}

/* Test 2: Parse a transmit action with all fields. */
static int
test_parse_transmit(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control(
	    "a=T,i=5,f=100,s=80,v=40,c=10,r=5,o=1,t=d",
	    strlen("a=T,i=5,f=100,s=80,v=40,c=10,r=5,o=1,t=d"),
	    &cmd);

	TEST_ASSERT("parse_transmit: ret == 0", ret == 0);
	TEST_ASSERT("parse_transmit: action == T",
	    cmd.action == KITTY_ACTION_TRANSMIT_AND_DISPLAY);
	TEST_ASSERT("parse_transmit: image_id == 5", cmd.image_id == 5);
	TEST_ASSERT("parse_transmit: format == 100", cmd.format == 100);
	TEST_ASSERT("parse_transmit: pixel_width == 80", cmd.pixel_width == 80);
	TEST_ASSERT("parse_transmit: pixel_height == 40", cmd.pixel_height == 40);
	TEST_ASSERT("parse_transmit: cols == 10", cmd.cols == 10);
	TEST_ASSERT("parse_transmit: rows == 5", cmd.rows == 5);
	TEST_ASSERT("parse_transmit: compression == 1", cmd.compression == 1);
	TEST_ASSERT("parse_transmit: transmission == 'd'", cmd.transmission == 'd');

	kitty_command_free(&cmd);
	TEST_PASS("parse_transmit");
	return (0);
}

/* Test 3: Parse a place action with placement ID. */
static int
test_parse_place(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=5,p=1,X=2,Y=3,z=10,C=1",
	    strlen("a=p,i=5,p=1,X=2,Y=3,z=10,C=1"), &cmd);

	TEST_ASSERT("parse_place: ret == 0", ret == 0);
	TEST_ASSERT("parse_place: action == p", cmd.action == KITTY_ACTION_PLACE);
	TEST_ASSERT("parse_place: image_id == 5", cmd.image_id == 5);
	TEST_ASSERT("parse_place: placement_id == 1", cmd.placement_id == 1);
	TEST_ASSERT("parse_place: cell_xoff == 2", cmd.cell_xoff == 2);
	TEST_ASSERT("parse_place: cell_yoff == 3", cmd.cell_yoff == 3);
	TEST_ASSERT("parse_place: zindex == 10", cmd.zindex == 10);
	TEST_ASSERT("parse_place: cursor_no_move == 1", cmd.cursor_no_move == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_place");
	return (0);
}

/* Test 4: Parse a delete action. */
static int
test_parse_delete(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=d,d=I,i=5", strlen("a=d,d=I,i=5"),
	    &cmd);

	TEST_ASSERT("parse_delete: ret == 0", ret == 0);
	TEST_ASSERT("parse_delete: action == d", cmd.action == KITTY_ACTION_DELETE);
	TEST_ASSERT("parse_delete: delete_action == I",
	    cmd.delete_action == 'I');
	TEST_ASSERT("parse_delete: image_id == 5", cmd.image_id == 5);

	kitty_command_free(&cmd);
	TEST_PASS("parse_delete");
	return (0);
}

/* Test 5: Parse with payload after semicolon. */
static int
test_parse_payload(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,i=1,f=100;base64data",
	    strlen("a=t,i=1,f=100;base64data"), &cmd);

	TEST_ASSERT("parse_payload: ret == 0", ret == 0);
	TEST_ASSERT("parse_payload: action == t",
	    cmd.action == KITTY_ACTION_TRANSMIT);
	TEST_ASSERT("parse_payload: image_id == 1", cmd.image_id == 1);
	TEST_ASSERT("parse_payload: payload != NULL", cmd.payload != NULL);
	TEST_ASSERT("parse_payload: payload_len == 10", cmd.payload_len == 10);
	TEST_ASSERT("parse_payload: payload content",
	    memcmp(cmd.payload, "base64data", 10) == 0);

	kitty_command_free(&cmd);
	TEST_PASS("parse_payload");
	return (0);
}

/* Test 6: Parse empty payload (semicolon with nothing after). */
static int
test_parse_empty_payload(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,i=1;", strlen("a=t,i=1;"), &cmd);

	TEST_ASSERT("parse_empty_payload: ret == 0", ret == 0);
	TEST_ASSERT("parse_empty_payload: payload == NULL", cmd.payload == NULL);
	TEST_ASSERT("parse_empty_payload: payload_len == 0", cmd.payload_len == 0);

	kitty_command_free(&cmd);
	TEST_PASS("parse_empty_payload");
	return (0);
}

/* Test 7: Parse with unknown keys (should be ignored). */
static int
test_parse_unknown_keys(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=q,i=1,unknownkey=42",
	    strlen("a=q,i=1,unknownkey=42"), &cmd);

	TEST_ASSERT("parse_unknown_keys: ret == 0", ret == 0);
	TEST_ASSERT("parse_unknown_keys: action == q",
	    cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_unknown_keys: image_id == 1", cmd.image_id == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_unknown_keys");
	return (0);
}

/* Test 8: Parse malformed - missing value. */
static int
test_parse_malformed(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=q,invalid", strlen("a=q,invalid"), &cmd);

	TEST_ASSERT("parse_malformed: ret == -1", ret == -1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_malformed");
	return (0);
}

/* Character-valued keys must have exactly one character. */
static int
test_parse_character_values(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=tt,i=1", strlen("a=tt,i=1"), &cmd);
	TEST_ASSERT("parse_char_values: action long", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=,i=1", strlen("a=,i=1"), &cmd);
	TEST_ASSERT("parse_char_values: action empty", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,t=dd,i=1", strlen("a=t,t=dd,i=1"),
	    &cmd);
	TEST_ASSERT("parse_char_values: transmission long", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=d,d=ii,i=1", strlen("a=d,d=ii,i=1"),
	    &cmd);
	TEST_ASSERT("parse_char_values: delete long", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=d,d=I,i=1", strlen("a=d,d=I,i=1"),
	    &cmd);
	TEST_ASSERT("parse_char_values: valid ret", ret == 0);
	TEST_ASSERT("parse_char_values: valid action",
	    cmd.action == KITTY_ACTION_DELETE && cmd.delete_action == 'I');

	kitty_command_free(&cmd);
	TEST_PASS("parse_char_values");
	return (0);
}

/* Test 9: Parse quiet mode. */
static int
test_parse_quiet(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=q,i=1,q=1", strlen("a=q,i=1,q=1"), &cmd);

	TEST_ASSERT("parse_quiet: ret == 0", ret == 0);
	TEST_ASSERT("parse_quiet: quiet == 1", cmd.quiet == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_quiet");
	return (0);
}

/* Test 10: Parse chunked upload flags. */
static int
test_parse_chunked(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,i=1,m=1", strlen("a=t,i=1,m=1"), &cmd);

	TEST_ASSERT("parse_chunked: ret == 0", ret == 0);
	TEST_ASSERT("parse_chunked: more == 1", cmd.more == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_chunked");
	return (0);
}

/* Test 11: Parse image number (I). */
static int
test_parse_image_number(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,I=42", strlen("a=t,I=42"), &cmd);

	TEST_ASSERT("parse_image_number: ret == 0", ret == 0);
	TEST_ASSERT("parse_image_number: image_number == 42", cmd.image_number == 42);

	kitty_command_free(&cmd);
	TEST_PASS("parse_image_number");
	return (0);
}

/* Test 12: Parse source rectangle (x,y,w,h). */
static int
test_parse_source_rect(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=1,x=10,y=20,w=30,h=40",
	    strlen("a=p,i=1,x=10,y=20,w=30,h=40"), &cmd);

	TEST_ASSERT("parse_source_rect: ret == 0", ret == 0);
	TEST_ASSERT("parse_source_rect: src_x == 10", cmd.src_x == 10);
	TEST_ASSERT("parse_source_rect: src_y == 20", cmd.src_y == 20);
	TEST_ASSERT("parse_source_rect: src_w == 30", cmd.src_w == 30);
	TEST_ASSERT("parse_source_rect: src_h == 40", cmd.src_h == 40);

	kitty_command_free(&cmd);
	TEST_PASS("parse_source_rect");
	return (0);
}

/* Parse relative placement keys (P,Q,H,V). */
static int
test_parse_relative_placement(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=1,P=2,Q=3,H=-1,V=4",
	    strlen("a=p,i=1,P=2,Q=3,H=-1,V=4"), &cmd);

	TEST_ASSERT("parse_relative: ret == 0", ret == 0);
	TEST_ASSERT("parse_relative: marked", cmd.relative == 1);
	TEST_ASSERT("parse_relative: parent image",
	    cmd.parent_image_id == 2);
	TEST_ASSERT("parse_relative: parent placement",
	    cmd.parent_placement_id == 3);
	TEST_ASSERT("parse_relative: offset x", cmd.parent_offset_x == -1);
	TEST_ASSERT("parse_relative: offset y", cmd.parent_offset_y == 4);

	kitty_command_free(&cmd);
	TEST_PASS("parse_relative");
	return (0);
}

/* Parse file offset and size keys. */
static int
test_parse_file_range(void)
{
	struct kitty_command	 cmd;
	int			 ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=t,i=1,t=f,S=4,O=3",
	    strlen("a=t,i=1,t=f,S=4,O=3"), &cmd);

	TEST_ASSERT("parse_file_range: ret == 0", ret == 0);
	TEST_ASSERT("parse_file_range: file_size == 4", cmd.file_size == 4);
	TEST_ASSERT("parse_file_range: file_offset == 3",
	    cmd.file_offset == 3);

	kitty_command_free(&cmd);
	TEST_PASS("parse_file_range");
	return (0);
}

/* Test 13: Parse virtual placement (U=1). */
static int
test_parse_virtual(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=T,i=1,U=1", strlen("a=T,i=1,U=1"), &cmd);

	TEST_ASSERT("parse_virtual: ret == 0", ret == 0);
	TEST_ASSERT("parse_virtual: virtual == 1", cmd.virtual == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_virtual");
	return (0);
}

/* Test 14: Image store and find. */
static int
test_image_store_find(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 42;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 100;
	cmd.pixel_height = 50;

	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("store_find: img != NULL", img != NULL);
	TEST_ASSERT("store_find: img->id == 42", img->id == 42);
	TEST_ASSERT("store_find: img->tty_id != 0", img->tty_id != 0);
	TEST_ASSERT("store_find: img->format == PNG",
	    img->format == KITTY_FORMAT_PNG);
	TEST_ASSERT("store_find: img->pixel_width == 100", img->pixel_width == 100);
	TEST_ASSERT("store_find: img->pixel_height == 50", img->pixel_height == 50);
	TEST_ASSERT("store_find: img->refcount == 1", img->refcount == 1);

	img = kitty_image_find(&s, 42);
	TEST_ASSERT("store_find: find == 42", img != NULL);
	TEST_ASSERT("store_find: find->id == 42", img->id == 42);

	img = kitty_image_find(&s, 99);
	TEST_ASSERT("store_find: find 99 == NULL", img == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("store_find");
	return (0);
}

/* Test 15: Image replace on same ID. */
static int
test_image_replace(void)
{
	struct screen			s;
	struct kitty_command		cmd1, cmd2;
	struct kitty_image		*img;
	uint32_t			tty_id;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd1, 0, sizeof cmd1);
	cmd1.image_id = 7;
	cmd1.format = KITTY_FORMAT_PNG;
	cmd1.pixel_width = 10;
	cmd1.pixel_height = 10;
	img = kitty_image_store(&s, &cmd1);
	TEST_ASSERT("replace: store 1", img != NULL);
	tty_id = img->tty_id;

	memset(&cmd2, 0, sizeof cmd2);
	cmd2.image_id = 7;
	cmd2.format = KITTY_FORMAT_RGB;
	cmd2.pixel_width = 20;
	cmd2.pixel_height = 20;
	img = kitty_image_replace(&s, img, &cmd2);
	TEST_ASSERT("replace: replace OK", img != NULL);
	TEST_ASSERT("replace: format == RGB", img->format == KITTY_FORMAT_RGB);
	TEST_ASSERT("replace: tty_id preserved", img->tty_id == tty_id);
	TEST_ASSERT("replace: width == 20", img->pixel_width == 20);
	TEST_ASSERT("replace: height == 20", img->pixel_height == 20);

	kitty_image_free_all(&s);
	TEST_PASS("replace");
	return (0);
}

/* Test 16: Placement create and find. */
static int
test_placement_create(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 100;
	cmd.pixel_height = 50;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("placement: store", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 99;
	cmd.cols = 10;
	cmd.rows = 5;
	cmd.cell_xoff = 2;
	cmd.cell_yoff = 3;
	cmd.zindex = 5;
	cmd.cursor_no_move = 1;
	pl = kitty_placement_create(&s, img, &cmd);
	TEST_ASSERT("placement: create", pl != NULL);
	TEST_ASSERT("placement: pl->image == img", pl->image == img);
	TEST_ASSERT("placement: pl->placement_id == 99", pl->placement_id == 99);
	TEST_ASSERT("placement: pl->cols == 10", pl->cols == 10);
	TEST_ASSERT("placement: pl->rows == 5", pl->rows == 5);
	TEST_ASSERT("placement: pl->cell_xoff == 2", pl->cell_xoff == 2);
	TEST_ASSERT("placement: pl->cell_yoff == 3", pl->cell_yoff == 3);
	TEST_ASSERT("placement: pl->zindex == 5", pl->zindex == 5);
	TEST_ASSERT("placement: pl->cursor_no_move == 1", pl->cursor_no_move == 1);
	TEST_ASSERT("placement: img refcount == 2", img->refcount == 2);

	pl = kitty_placement_find(&s, 99, 1);
	TEST_ASSERT("placement: find", pl != NULL);
	TEST_ASSERT("placement: find->placement_id == 99", pl->placement_id == 99);

	pl = kitty_placement_find(&s, 99, 2);
	TEST_ASSERT("placement: find wrong image == NULL", pl == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("placement");
	return (0);
}

/* Test 17: Delete image removes placements. */
static int
test_delete_image(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_image: store", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 1;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);
	TEST_ASSERT("delete_image: placement", pl != NULL);

	kitty_delete_image_placements(&s, 1);
	pl = kitty_placement_find(&s, 1, 1);
	TEST_ASSERT("delete_image: placement removed", pl == NULL);
	TEST_ASSERT("delete_image: img refcount == 1", img->refcount == 1);

	kitty_image_free_all(&s);
	TEST_PASS("delete_image");
	return (0);
}

/* Test 18: Delete all placements. */
static int
test_delete_all_placements(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 1;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 2;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);

	ret = kitty_delete_all_placements(&s);
	TEST_ASSERT("delete_all_placements: ret == 1", ret == 1);
	pl = kitty_placement_find(&s, 1, 1);
	TEST_ASSERT("delete_all_placements: pl 1 gone", pl == NULL);
	pl = kitty_placement_find(&s, 2, 1);
	TEST_ASSERT("delete_all_placements: pl 2 gone", pl == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_all_placements");
	return (0);
}

/* Test 19: Delete all (images + placements). */
static int
test_delete_all(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 1;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_DELETE;
	cmd.delete_action = 'A';
	ret = kitty_handle_delete(&s, &cmd, &reply);
	TEST_ASSERT("delete_all: ret == 0", ret == 0);
	TEST_ASSERT("delete_all: no anonymous reply", reply == NULL);
	img = kitty_image_find(&s, 1);
	TEST_ASSERT("delete_all: image gone", img == NULL);
	pl = kitty_placement_find(&s, 1, 1);
	TEST_ASSERT("delete_all: placement gone", pl == NULL);

	TEST_PASS("delete_all");
	return (0);
}

/* Test 20: Delete by cursor position. */
static int
test_delete_cursor(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 1;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);
	pl->pane_x = 0;
	pl->pane_y = 0;

	/* Cursor at (0,0) should be inside the placement */
	s.cx = 0;
	s.cy = 0;
	ret = kitty_delete_cursor(&s, 0);
	TEST_ASSERT("delete_cursor: ret == 1", ret == 1);
	pl = kitty_placement_find(&s, 1, 1);
	TEST_ASSERT("delete_cursor: placement gone", pl == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_cursor");
	return (0);
}

/* Test 21: Delete by z-index. */
static int
test_delete_zindex(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 1;
	cmd.cols = 10;
	cmd.rows = 5;
	cmd.zindex = 5;
	pl = kitty_placement_create(&s, img, &cmd);
	pl->zindex = 5;

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 2;
	cmd.cols = 10;
	cmd.rows = 5;
	cmd.zindex = 10;
	pl = kitty_placement_create(&s, img, &cmd);
	pl->zindex = 10;

	ret = kitty_delete_zindex(&s, 5, 0);
	TEST_ASSERT("delete_zindex: ret == 1", ret == 1);
	pl = kitty_placement_find(&s, 1, 1);
	TEST_ASSERT("delete_zindex: z=5 gone", pl == NULL);
	pl = kitty_placement_find(&s, 2, 1);
	TEST_ASSERT("delete_zindex: z=10 still there", pl != NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_zindex");
	return (0);
}

/* Test 22: Build reply. */
static int
test_build_reply(void)
{
	struct kitty_command	cmd;
	char			*reply;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 42;
	ret = kitty_build_reply(NULL, &cmd, &reply);
	TEST_ASSERT("build_reply: ret == 0", ret == 0);
	TEST_ASSERT("build_reply: reply != NULL", reply != NULL);
	TEST_ASSERT("build_reply: contains i=42",
	    strstr(reply, "i=42") != NULL);
	TEST_ASSERT("build_reply: contains OK",
	    strstr(reply, "OK") != NULL);
	free(reply);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_number = 7;
	ret = kitty_build_reply(NULL, &cmd, &reply);
	TEST_ASSERT("build_reply: number ret == 0", ret == 0);
	TEST_ASSERT("build_reply: contains I=7",
	    strstr(reply, "I=7") != NULL);
	TEST_ASSERT("build_reply: number no i=0",
	    strstr(reply, "i=0") == NULL);
	free(reply);

	TEST_PASS("build_reply");
	return (0);
}

/* Test 23: Handle query with quiet mode. */
static int
test_handle_query_quiet(void)
{
	struct kitty_command	cmd;
	char			*reply;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_QUERY;
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_RGB;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.quiet = 1;
	cmd.payload = (u_char *)xstrdup("QUJD");
	cmd.payload_len = 4;
	ret = kitty_handle_query(NULL, &cmd, &reply);
	TEST_ASSERT("query_quiet: ret == 0", ret == 0);
	TEST_ASSERT("query_quiet: reply == NULL", reply == NULL);
	kitty_command_free(&cmd);

	TEST_PASS("query_quiet");
	return (0);
}

/* Test 24: Handle query without quiet mode. */
static int
test_handle_query_loud(void)
{
	struct kitty_command	cmd;
	char			*reply;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_QUERY;
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_RGB;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.quiet = 0;
	cmd.payload = (u_char *)xstrdup("QUJD");
	cmd.payload_len = 4;
	ret = kitty_handle_query(NULL, &cmd, &reply);
	TEST_ASSERT("query_loud: ret == 0", ret == 0);
	TEST_ASSERT("query_loud: reply != NULL", reply != NULL);
	TEST_ASSERT("query_loud: contains OK",
	    strstr(reply, "OK") != NULL);
	free(reply);
	kitty_command_free(&cmd);

	TEST_PASS("query_loud");
	return (0);
}

/* Query without an image id fails. */
static int
test_handle_query_missing_id(void)
{
	struct kitty_command	 cmd;
	char			*reply;
	int			 ret;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_QUERY;
	ret = kitty_handle_query(NULL, &cmd, &reply);
	TEST_ASSERT("query_missing: ret == -1", ret == -1);
	TEST_ASSERT("query_missing: EINVAL",
	    reply != NULL && strstr(reply, "EINVAL") != NULL);
	free(reply);

	TEST_PASS("query_missing");
	return (0);
}

/* Query validates image data but does not store it. */
static int
test_handle_query_validation(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=q,i=151,f=24,s=1,v=1;QUJD",
	    strlen("a=q,i=151,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("query_validation: valid ret", ret == 0);
	TEST_ASSERT("query_validation: valid OK",
	    reply != NULL && strstr(reply, "i=151;OK") != NULL);
	free(reply);
	TEST_ASSERT("query_validation: no image stored",
	    test_count_images(&s) == 0);

	ret = kitty_image_parse(&s, "a=q,i=152,f=24,s=1,v=1;QQ==",
	    strlen("a=q,i=152,f=24,s=1,v=1;QQ=="), &reply);
	TEST_ASSERT("query_validation: short ret", ret == -1);
	TEST_ASSERT("query_validation: short ENODATA",
	    reply != NULL && strstr(reply, "ENODATA") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=q,i=153,f=24,t=s,s=1,v=1;QUJD",
	    strlen("a=q,i=153,f=24,t=s,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("query_validation: shm ret", ret == -1);
	TEST_ASSERT("query_validation: shm ENOTSUP",
	    reply != NULL && strstr(reply, "ENOTSUP") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=q,i=154,q=2,f=24,s=1,v=1;QQ==",
	    strlen("a=q,i=154,q=2,f=24,s=1,v=1;QQ=="), &reply);
	TEST_ASSERT("query_validation: q2 ret", ret == -1);
	TEST_ASSERT("query_validation: q2 no reply", reply == NULL);

	ret = kitty_image_parse(&s, "a=q,i=155,f=24,s=1,v=1,m=1;QUJD",
	    strlen("a=q,i=155,f=24,s=1,v=1,m=1;QUJD"), &reply);
	TEST_ASSERT("query_validation: chunk first ret", ret == 0);
	TEST_ASSERT("query_validation: chunk first no reply", reply == NULL);
	ret = kitty_image_parse(&s, "m=0", strlen("m=0"), &reply);
	TEST_ASSERT("query_validation: chunk final ret", ret == 0);
	TEST_ASSERT("query_validation: chunk final OK",
	    reply != NULL && strstr(reply, "i=155;OK") != NULL);
	free(reply);
	TEST_ASSERT("query_validation: chunk no image stored",
	    test_count_images(&s) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("query_validation");
	return (0);
}

/* Test 25: Memory limits - max images. */
static int
test_memory_limits_images(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	u_int				i;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	for (i = 0; i < KITTY_MAX_IMAGES + 5; i++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.image_id = i + 1;
		cmd.format = KITTY_FORMAT_PNG;
		cmd.pixel_width = 1;
		cmd.pixel_height = 1;
		img = kitty_image_store(&s, &cmd);
		if (i < KITTY_MAX_IMAGES)
			TEST_ASSERT("memlimit_images: store ok", img != NULL);
		else
			TEST_ASSERT("memlimit_images: store fail", img == NULL);
	}

	kitty_image_free_all(&s);
	TEST_PASS("memlimit_images");
	return (0);
}

/* Replacing an existing image should not consume another image slot. */
static int
test_memory_limits_replace_at_limit(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;
	u_int			 i;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	for (i = 0; i < KITTY_MAX_IMAGES; i++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.image_id = i + 1;
		img = kitty_image_store(&s, &cmd);
		TEST_ASSERT("memlimit_replace: store ok", img != NULL);
	}

	ret = kitty_image_parse(&s, "a=t,i=1,f=24,s=1,v=1;QUJD",
	    strlen("a=t,i=1,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("memlimit_replace: replace ret", ret == 0);
	TEST_ASSERT("memlimit_replace: replace reply",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	img = kitty_image_find(&s, 1);
	TEST_ASSERT("memlimit_replace: replaced payload",
	    img != NULL && img->payload_len == 3 &&
	    memcmp(img->payload, "ABC", 3) == 0);

	ret = kitty_image_parse(&s, "a=t,i=999,f=24,s=1,v=1;REVG",
	    strlen("a=t,i=999,f=24,s=1,v=1;REVG"), &reply);
	TEST_ASSERT("memlimit_replace: new image fails", ret == -1);
	TEST_ASSERT("memlimit_replace: failure reply",
	    reply != NULL && strstr(reply, "ENOMEM") != NULL);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("memlimit_replace");
	return (0);
}

/* Test 26: Memory limits - max payload. */
static int
test_memory_limits_payload(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.payload_len = KITTY_MAX_IMAGE_PAYLOAD + 1;
	cmd.payload = xmalloc(cmd.payload_len);
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("memlimit_payload: too large fails", img == NULL);
	free(cmd.payload);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 2;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.payload_len = 1024;
	cmd.payload = xmalloc(cmd.payload_len);
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("memlimit_payload: small ok", img != NULL);
	free(cmd.payload);

	kitty_image_free_all(&s);
	TEST_PASS("memlimit_payload");
	return (0);
}

/* Test 27: Handle transmit with empty IDs should fail. */
static int
test_handle_transmit_empty(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT;
	cmd.image_id = 0;
	cmd.image_number = 0;
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_empty: ret == -1", ret == -1);
	TEST_ASSERT("transmit_empty: reply != NULL", reply != NULL);
	TEST_ASSERT("transmit_empty: contains EINVAL",
	    strstr(reply, "EINVAL") != NULL);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_empty");
	return (0);
}

/* Test 28: Handle place with missing image should fail. */
static int
test_handle_place_missing(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	cmd.image_id = 99;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_missing: ret == -1", ret == -1);
	TEST_ASSERT("place_missing: reply != NULL", reply != NULL);
	TEST_ASSERT("place_missing: contains ENOENT",
	    strstr(reply, "ENOENT") != NULL);
	free(reply);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	cmd.image_number = 99;
	cmd.placement_id = 3;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_missing: number ret == -1", ret == -1);
	TEST_ASSERT("place_missing: number reply",
	    reply != NULL && strstr(reply, "I=99,p=3") != NULL &&
	    strstr(reply, "ENOENT") != NULL);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("place_missing");
	return (0);
}

/* Handle place without an image id or number should fail. */
static int
test_handle_place_no_reference(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_no_ref: ret == -1", ret == -1);
	TEST_ASSERT("place_no_ref: reply != NULL", reply != NULL);
	TEST_ASSERT("place_no_ref: contains EINVAL",
	    strstr(reply, "EINVAL") != NULL);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("place_no_ref");
	return (0);
}

/* Relative placements are parsed but not implemented. */
static int
test_relative_placement_unsupported(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=140,f=24,s=1,v=1;QUJD",
	    strlen("a=t,i=140,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("relative_unsupported: transmit ret", ret == 0);
	free(reply);

	ret = kitty_image_parse(&s, "a=p,i=140,p=1,P=140,Q=1",
	    strlen("a=p,i=140,p=1,P=140,Q=1"), &reply);
	TEST_ASSERT("relative_unsupported: place ret", ret == -1);
	TEST_ASSERT("relative_unsupported: place ENOTSUP",
	    reply != NULL && strstr(reply, "ENOTSUP") != NULL);
	free(reply);
	TEST_ASSERT("relative_unsupported: no placement",
	    test_count_placements(&s) == 0);

	ret = kitty_image_parse(&s,
	    "a=T,i=141,p=2,f=24,s=1,v=1,c=1,r=1,P=140,Q=1;QUJD",
	    strlen("a=T,i=141,p=2,f=24,s=1,v=1,c=1,r=1,P=140,Q=1;QUJD"),
	    &reply);
	TEST_ASSERT("relative_unsupported: T ret", ret == -1);
	TEST_ASSERT("relative_unsupported: T ENOTSUP",
	    reply != NULL && strstr(reply, "ENOTSUP") != NULL);
	free(reply);
	TEST_ASSERT("relative_unsupported: no new image",
	    kitty_image_find(&s, 141) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("relative_unsupported");
	return (0);
}

/* Animation actions are documented but not implemented by tmux. */
static int
test_unsupported_animation_actions(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=f,i=145,f=24,s=1,v=1;QUJD",
	    strlen("a=f,i=145,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("unsupported_actions: frame ret", ret == -1);
	TEST_ASSERT("unsupported_actions: frame ENOTSUP",
	    reply != NULL && strstr(reply, "i=145;ENOTSUP") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=a,i=145", strlen("a=a,i=145"),
	    &reply);
	TEST_ASSERT("unsupported_actions: animation ret", ret == -1);
	TEST_ASSERT("unsupported_actions: animation ENOTSUP",
	    reply != NULL && strstr(reply, "i=145;ENOTSUP") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=c,i=145", strlen("a=c,i=145"),
	    &reply);
	TEST_ASSERT("unsupported_actions: compose ret", ret == -1);
	TEST_ASSERT("unsupported_actions: compose ENOTSUP",
	    reply != NULL && strstr(reply, "i=145;ENOTSUP") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=f,i=146,q=2,f=24,s=1,v=1;QUJD",
	    strlen("a=f,i=146,q=2,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("unsupported_actions: quiet ret", ret == -1);
	TEST_ASSERT("unsupported_actions: quiet reply", reply == NULL);

	ret = kitty_image_parse(&s, "a=f,i=147,f=24,s=1,v=1,m=1;QUJD",
	    strlen("a=f,i=147,f=24,s=1,v=1,m=1;QUJD"), &reply);
	TEST_ASSERT("unsupported_actions: chunk ret", ret == -1);
	TEST_ASSERT("unsupported_actions: chunk ENOTSUP",
	    reply != NULL && strstr(reply, "i=147;ENOTSUP") != NULL);
	free(reply);
	TEST_ASSERT("unsupported_actions: no pending",
	    s.kitty_pending.active == 0);

	TEST_ASSERT("unsupported_actions: no images",
	    test_count_images(&s) == 0);
	TEST_ASSERT("unsupported_actions: no placements",
	    test_count_placements(&s) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("unsupported_actions");
	return (0);
}

/* Unknown actions are invalid and should not be buffered. */
static int
test_unknown_actions_rejected(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=Z,i=148", strlen("a=Z,i=148"),
	    &reply);
	TEST_ASSERT("unknown_actions: ret", ret == -1);
	TEST_ASSERT("unknown_actions: EINVAL",
	    reply != NULL && strstr(reply, "i=148;EINVAL") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=Z,i=149,m=1;QUJD",
	    strlen("a=Z,i=149,m=1;QUJD"), &reply);
	TEST_ASSERT("unknown_actions: chunk ret", ret == -1);
	TEST_ASSERT("unknown_actions: chunk EINVAL",
	    reply != NULL && strstr(reply, "i=149;EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("unknown_actions: no pending",
	    s.kitty_pending.active == 0);

	TEST_ASSERT("unknown_actions: no images", test_count_images(&s) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("unknown_actions");
	return (0);
}

/* Image id and image number are mutually exclusive references. */
static int
test_image_id_number_conflict(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=1,I=2,f=24,s=1,v=1;QUJD",
	    strlen("a=t,i=1,I=2,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("id_number_conflict: transmit ret", ret == -1);
	TEST_ASSERT("id_number_conflict: transmit EINVAL",
	    reply != NULL && strstr(reply, "i=1,I=2;EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("id_number_conflict: transmit no image",
	    test_count_images(&s) == 0);

	ret = kitty_image_parse(&s, "a=p,i=1,I=2,p=3",
	    strlen("a=p,i=1,I=2,p=3"), &reply);
	TEST_ASSERT("id_number_conflict: place ret", ret == -1);
	TEST_ASSERT("id_number_conflict: place EINVAL",
	    reply != NULL && strstr(reply, "i=1,I=2,p=3;EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("id_number_conflict: place no placement",
	    test_count_placements(&s) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("id_number_conflict");
	return (0);
}

/* Test 29: Handle delete with no specifier defaults to 'a'. */
static int
test_handle_delete_default(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_DELETE;
	ret = kitty_handle_delete(&s, &cmd, &reply);
	TEST_ASSERT("delete_default: ret == 0", ret == 0);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("delete_default");
	return (0);
}

/* Test 30: Handle transmit with file mode (t=f) without path should fail. */
static int
test_handle_transmit_file_no_path(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT;
	cmd.image_id = 1;
	cmd.transmission = 'f';
	cmd.payload = NULL;
	cmd.payload_len = 0;
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_file_no_path: ret == -1", ret == -1);
	TEST_ASSERT("transmit_file_no_path: reply != NULL", reply != NULL);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_file_no_path");
	return (0);
}

/* Test 31: Handle shared memory (t=s) should fail. */
static int
test_handle_transmit_shm(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT;
	cmd.image_id = 1;
	cmd.transmission = 's';
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_shm: ret == -1", ret == -1);
	TEST_ASSERT("transmit_shm: reply != NULL", reply != NULL);
	TEST_ASSERT("transmit_shm: contains ENOTSUP",
	    strstr(reply, "ENOTSUP") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=t,i=133,t=s,m=1;AAAA",
	    strlen("a=t,i=133,t=s,m=1;AAAA"), &reply);
	TEST_ASSERT("transmit_shm: m ignored ret", ret == -1);
	TEST_ASSERT("transmit_shm: m ignored ENOTSUP",
	    reply != NULL && strstr(reply, "ENOTSUP") != NULL);
	free(reply);
	TEST_ASSERT("transmit_shm: m ignored no pending",
	    s.kitty_pending.active == 0);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_shm");
	return (0);
}

/* Test 32: Chunked upload pending. */
static int
test_chunked_upload(void)
{
	struct screen			s;
	int				ret;
	char				*reply;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	memset(&s.kitty_pending, 0, sizeof s.kitty_pending);

	ret = kitty_image_parse(&s, "a=t,i=1,f=24,s=1,v=2,m=1;QUJD",
	    strlen("a=t,i=1,f=24,s=1,v=2,m=1;QUJD"), &reply);
	TEST_ASSERT("chunked_upload: m=1 sets pending", ret == 0);
	TEST_ASSERT("chunked_upload: pending.active == 1",
	    s.kitty_pending.active == 1);
	TEST_ASSERT("chunked_upload: pending.id == 1",
	    s.kitty_pending.id == 1);
	TEST_ASSERT("chunked_upload: pending.payload_len == 4",
	    s.kitty_pending.payload_len == 4);
	TEST_ASSERT("chunked_upload: m=1 no reply", reply == NULL);
	free(reply);

	ret = kitty_image_parse(&s, "m=0;REVG", strlen("m=0;REVG"),
	    &reply);
	TEST_ASSERT("chunked_upload: m=0 completes", ret == 0);
	TEST_ASSERT("chunked_upload: pending.active == 0",
	    s.kitty_pending.active == 0);
	free(reply);

	ret = kitty_image_parse(&s,
	    "a=t,i=130,f=24,s=1,v=2,m=1,q=1;QUJD",
	    strlen("a=t,i=130,f=24,s=1,v=2,m=1,q=1;QUJD"), &reply);
	TEST_ASSERT("chunked_upload: q1 first ret", ret == 0);
	TEST_ASSERT("chunked_upload: q1 first no reply", reply == NULL);
	free(reply);
	ret = kitty_image_parse(&s, "m=0;REVG", strlen("m=0;REVG"),
	    &reply);
	TEST_ASSERT("chunked_upload: q1 final ret", ret == 0);
	TEST_ASSERT("chunked_upload: q1 final no reply", reply == NULL);
	TEST_ASSERT("chunked_upload: q1 image stored",
	    kitty_image_find(&s, 130) != NULL);
	free(reply);

	ret = kitty_image_parse(&s,
	    "a=t,i=137,f=24,s=1,v=2,m=1,q=0;QUJD",
	    strlen("a=t,i=137,f=24,s=1,v=2,m=1,q=0;QUJD"), &reply);
	TEST_ASSERT("chunked_upload: q increase first ret", ret == 0);
	TEST_ASSERT("chunked_upload: q increase first no reply", reply == NULL);
	free(reply);
	ret = kitty_image_parse(&s, "m=0,q=1;REVG",
	    strlen("m=0,q=1;REVG"), &reply);
	TEST_ASSERT("chunked_upload: q increase final ret", ret == 0);
	TEST_ASSERT("chunked_upload: q increase final no reply", reply == NULL);
	TEST_ASSERT("chunked_upload: q increase image stored",
	    kitty_image_find(&s, 137) != NULL);
	free(reply);

	ret = kitty_image_parse(&s,
	    "a=t,i=131,f=24,s=2,v=2,m=1,q=2;QUJD",
	    strlen("a=t,i=131,f=24,s=2,v=2,m=1,q=2;QUJD"), &reply);
	TEST_ASSERT("chunked_upload: q2 first ret", ret == 0);
	TEST_ASSERT("chunked_upload: q2 first no reply", reply == NULL);
	free(reply);
	ret = kitty_image_parse(&s, "m=0;REVG", strlen("m=0;REVG"),
	    &reply);
	TEST_ASSERT("chunked_upload: q2 final ret", ret == -1);
	TEST_ASSERT("chunked_upload: q2 final no reply", reply == NULL);
	TEST_ASSERT("chunked_upload: q2 image not stored",
	    kitty_image_find(&s, 131) == NULL);
	TEST_ASSERT("chunked_upload: q2 pending cleared",
	    s.kitty_pending.active == 0);
	free(reply);

	kitty_image_free_all(&s);
	TEST_PASS("chunked_upload");
	return (0);
}

/* Test 33: Empty input should fail gracefully. */
static int
test_parse_empty(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("", strlen(""), &cmd);
	TEST_ASSERT("parse_empty: ret == 0", ret == 0);
	TEST_ASSERT("parse_empty: action == 0", cmd.action == 0);

	kitty_command_free(&cmd);
	TEST_PASS("parse_empty");
	return (0);
}

/* Test 34: Only semicolon (no payload). */
static int
test_parse_semicolon_only(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control(";", strlen(";"), &cmd);
	TEST_ASSERT("parse_semicolon_only: ret == 0", ret == 0);
	TEST_ASSERT("parse_semicolon_only: payload == NULL", cmd.payload == NULL);

	kitty_command_free(&cmd);
	TEST_PASS("parse_semicolon_only");
	return (0);
}

/* Test 35: Handle transmit with cursor movement. */
static int
test_handle_transmit_cursor_move(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 5;
	s.cy = 10;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT_AND_DISPLAY;
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.cols = 2;
	cmd.rows = 3;
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_cursor: ret == 0", ret == 0);
	free(reply);

	pl = kitty_placement_find(&s, 0, 1);
	TEST_ASSERT("transmit_cursor: placement exists", pl != NULL);
	TEST_ASSERT("transmit_cursor: pane_x == 5", pl->pane_x == 5);
	TEST_ASSERT("transmit_cursor: pane_y == 10", pl->pane_y == 10);
	TEST_ASSERT("transmit_cursor: cursor moved cx", s.cx == 7);
	TEST_ASSERT("transmit_cursor: cursor moved cy", s.cy == 13);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_cursor");
	return (0);
}

/* Test 36: Handle transmit with cursor_no_move (C=1). */
static int
test_handle_transmit_cursor_stay(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 5;
	s.cy = 10;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT_AND_DISPLAY;
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.cols = 2;
	cmd.rows = 3;
	cmd.cursor_no_move = 1;
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_stay: ret == 0", ret == 0);
	free(reply);

	TEST_ASSERT("transmit_stay: cx unchanged", s.cx == 5);
	TEST_ASSERT("transmit_stay: cy unchanged", s.cy == 10);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_stay");
	return (0);
}

/* Test 37: Handle virtual placement (no visible placement). */
static int
test_handle_transmit_virtual(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_TRANSMIT_AND_DISPLAY;
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.virtual = 1;
	ret = kitty_handle_transmit(&s, &cmd, &reply);
	TEST_ASSERT("transmit_virtual: ret == 0", ret == 0);
	free(reply);

	pl = TAILQ_FIRST(&s.kitty_placements);
	TEST_ASSERT("transmit_virtual: no placement", pl == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("transmit_virtual");
	return (0);
}

/* Test 38: Handle place replacing existing placement. */
static int
test_handle_place_replace(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;
	struct kitty_image		*img;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 0;
	s.cy = 0;

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.placement_id = 5;
	cmd.cols = 10;
	cmd.rows = 5;
	pl = kitty_placement_create(&s, img, &cmd);
	TEST_ASSERT("place_replace: initial", pl != NULL);
	pl->pane_x = 0;
	pl->pane_y = 0;

	s.cx = 3;
	s.cy = 4;

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	cmd.image_id = 1;
	cmd.placement_id = 5;
	cmd.cols = 2;
	cmd.rows = 2;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_replace: ret == 0", ret == 0);
	free(reply);

	pl = kitty_placement_find(&s, 5, 1);
	TEST_ASSERT("place_replace: still one", pl != NULL);
	TEST_ASSERT("place_replace: cols updated", pl->cols == 2);
	TEST_ASSERT("place_replace: rows updated", pl->rows == 2);
	TEST_ASSERT("place_replace: x moved", pl->pane_x == 3);
	TEST_ASSERT("place_replace: y moved", pl->pane_y == 4);

	kitty_image_free_all(&s);
	TEST_PASS("place_replace");
	return (0);
}

/* Test 39: Handle place with virtual (U=1) should not create placement. */
static int
test_handle_place_virtual(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;
	struct kitty_image		*img;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("place_virtual: image stored", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	cmd.image_id = 1;
	cmd.virtual = 1;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_virtual: ret == 0", ret == 0);
	free(reply);

	pl = TAILQ_FIRST(&s.kitty_placements);
	TEST_ASSERT("place_virtual: no placement", pl == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("place_virtual");
	return (0);
}

/* Test 40: Handle place by image number. */
static int
test_handle_place_by_number(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	char				*reply;
	int				ret;
	struct kitty_image		*img;
	struct kitty_placement		*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 17;
	cmd.image_number = 42;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("place_number: image stored", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.action = KITTY_ACTION_PLACE;
	cmd.image_number = 42;
	cmd.placement_id = 9;
	cmd.cols = 2;
	cmd.rows = 3;
	ret = kitty_handle_place(&s, &cmd, &reply);
	TEST_ASSERT("place_number: ret == 0", ret == 0);
	TEST_ASSERT("place_number: reply ids",
	    reply != NULL && strstr(reply, "i=17,I=42,p=9") != NULL);
	free(reply);

	pl = kitty_placement_find(&s, 9, 17);
	TEST_ASSERT("place_number: placement exists", pl != NULL);
	TEST_ASSERT("place_number: placement image", pl->image == img);

	kitty_image_free_all(&s);
	TEST_PASS("place_number");
	return (0);
}

/* Test 41: Memory limit - max placements. */
static int
test_memory_limits_placements(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;
	struct kitty_placement		*pl;
	u_int				i;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);

	for (i = 0; i < KITTY_MAX_PLACEMENTS + 5; i++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.image_id = 1;
		cmd.placement_id = i + 1;
		cmd.cols = 1;
		cmd.rows = 1;
		pl = kitty_placement_create(&s, img, &cmd);
		if (i < KITTY_MAX_PLACEMENTS)
			TEST_ASSERT("memlimit_placements: create ok",
			    pl != NULL);
		else
			TEST_ASSERT("memlimit_placements: create fail",
			    pl == NULL);
	}

	kitty_image_free_all(&s);
	TEST_PASS("memlimit_placements");
	return (0);
}

/* Test 42: Image number find. */
static int
test_image_number_find(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.image_number = 42;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("number_find: store", img != NULL);

	img = kitty_image_find_by_number(&s, 42);
	TEST_ASSERT("number_find: found", img != NULL);
	TEST_ASSERT("number_find: id == 1", img->id == 1);

	img = kitty_image_find_by_number(&s, 99);
	TEST_ASSERT("number_find: not found", img == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("number_find");
	return (0);
}

/* Test 43: Build reply with image_id 0. */
static int
test_build_reply_zero(void)
{
	struct kitty_command	cmd;
	char			*reply;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 0;
	ret = kitty_build_reply(NULL, &cmd, &reply);
	TEST_ASSERT("build_reply_zero: ret == 0", ret == 0);
	TEST_ASSERT("build_reply_zero: reply != NULL", reply != NULL);
	TEST_ASSERT("build_reply_zero: contains i=0",
	    strstr(reply, "i=0") != NULL);
	free(reply);

	TEST_PASS("build_reply_zero");
	return (0);
}

/* Test 44: Free all images. */
static int
test_free_all(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("free_all: store", img != NULL);

	kitty_image_free_all(&s);
	img = kitty_image_find(&s, 1);
	TEST_ASSERT("free_all: gone", img == NULL);
	TEST_ASSERT("free_all: list empty",
	    TAILQ_EMPTY(&s.kitty_images));
	TEST_ASSERT("free_all: placements empty",
	    TAILQ_EMPTY(&s.kitty_placements));

	TEST_PASS("free_all");
	return (0);
}

/* Freeing arbitrary image lists handles saved alternate-screen queues. */
static int
test_free_saved_lists(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	TAILQ_INIT(&s.saved_kitty_images);
	TAILQ_INIT(&s.saved_kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 2;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 1;
	cmd.pixel_height = 1;
	cmd.cols = 1;
	cmd.rows = 1;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("free_saved: store", img != NULL);
	TEST_ASSERT("free_saved: placement",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	TAILQ_CONCAT(&s.saved_kitty_images, &s.kitty_images, entry);
	TAILQ_CONCAT(&s.saved_kitty_placements, &s.kitty_placements, entry);
	kitty_image_free_lists(&s.saved_kitty_images,
	    &s.saved_kitty_placements);

	TEST_ASSERT("free_saved: images empty",
	    TAILQ_EMPTY(&s.saved_kitty_images));
	TEST_ASSERT("free_saved: placements empty",
	    TAILQ_EMPTY(&s.saved_kitty_placements));
	TEST_ASSERT("free_saved: active images empty",
	    TAILQ_EMPTY(&s.kitty_images));
	TEST_ASSERT("free_saved: active placements empty",
	    TAILQ_EMPTY(&s.kitty_placements));

	TEST_PASS("free_saved");
	return (0);
}

/* Test 45: Empty control data with only semicolon and payload. */
static int
test_parse_payload_only(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control(";hello", strlen(";hello"), &cmd);
	TEST_ASSERT("parse_payload_only: ret == 0", ret == 0);
	TEST_ASSERT("parse_payload_only: payload != NULL", cmd.payload != NULL);
	TEST_ASSERT("parse_payload_only: payload_len == 5",
	    cmd.payload_len == 5);
	TEST_ASSERT("parse_payload_only: content",
	    memcmp(cmd.payload, "hello", 5) == 0);

	kitty_command_free(&cmd);
	TEST_PASS("parse_payload_only");
	return (0);
}

/* Test 46: Duplicate keys (last one wins). */
static int
test_parse_duplicate_keys(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("i=1,i=2,i=3", strlen("i=1,i=2,i=3"), &cmd);
	TEST_ASSERT("parse_duplicate: ret == 0", ret == 0);
	TEST_ASSERT("parse_duplicate: image_id == 3", cmd.image_id == 3);

	kitty_command_free(&cmd);
	TEST_PASS("parse_duplicate");
	return (0);
}

/* Test 47: Large control data should be rejected. */
static int
test_parse_large_control(void)
{
	struct kitty_command	cmd;
	char			*data;
	int			ret;

	data = xmalloc(KITTY_MAX_CONTROL_LEN + 10);
	memset(data, 'a', KITTY_MAX_CONTROL_LEN + 10);
	data[KITTY_MAX_CONTROL_LEN + 9] = '\0';

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control(data, KITTY_MAX_CONTROL_LEN + 10, &cmd);
	TEST_ASSERT("parse_large: ret == -1", ret == -1);

	free(data);
	kitty_command_free(&cmd);
	TEST_PASS("parse_large");
	return (0);
}

/* Test 48: Negative z-index. */
static int
test_parse_negative_zindex(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("z=-5", strlen("z=-5"), &cmd);
	TEST_ASSERT("parse_negative_z: ret == 0", ret == 0);
	TEST_ASSERT("parse_negative_z: zindex == -5", cmd.zindex == -5);

	kitty_command_free(&cmd);
	TEST_PASS("parse_negative_z");
	return (0);
}

/* Negative values for unsigned keys should be rejected. */
static int
test_parse_negative_unsigned(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=-1", strlen("a=p,i=-1"), &cmd);
	TEST_ASSERT("parse_negative_unsigned: id rejected", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,x=-1", strlen("a=p,x=-1"), &cmd);
	TEST_ASSERT("parse_negative_unsigned: x rejected", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=4294967296",
	    strlen("a=p,i=4294967296"), &cmd);
	TEST_ASSERT("parse_unsigned_bounds: id rejected", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("z=2147483648",
	    strlen("z=2147483648"), &cmd);
	TEST_ASSERT("parse_unsigned_bounds: z rejected", ret == -1);
	kitty_command_free(&cmd);

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=p,i=999999999999999999999999999999",
	    strlen("a=p,i=999999999999999999999999999999"), &cmd);
	TEST_ASSERT("parse_unsigned_bounds: overflow rejected", ret == -1);
	kitty_command_free(&cmd);

	TEST_PASS("parse_unsigned_bounds");
	return (0);
}

/* Test 49: Multiple commas in sequence. */
static int
test_parse_multiple_commas(void)
{
	struct kitty_command	cmd;
	int			ret;

	memset(&cmd, 0, sizeof cmd);
	ret = kitty_parse_control("a=q,,i=1", strlen("a=q,,i=1"), &cmd);
	TEST_ASSERT("parse_multi_commas: ret == 0", ret == 0);
	TEST_ASSERT("parse_multi_commas: action == q",
	    cmd.action == KITTY_ACTION_QUERY);
	TEST_ASSERT("parse_multi_commas: image_id == 1", cmd.image_id == 1);

	kitty_command_free(&cmd);
	TEST_PASS("parse_multi_commas");
	return (0);
}

/* Test 50: Image store with payload. */
static int
test_image_store_with_payload(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 10;
	cmd.pixel_height = 10;
	cmd.payload_len = 100;
	cmd.payload = xmalloc(100);
	memset(cmd.payload, 0x42, 100);
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("store_payload: img != NULL", img != NULL);
	TEST_ASSERT("store_payload: payload_len == 100", img->payload_len == 100);
	TEST_ASSERT("store_payload: payload content",
	    img->payload != NULL && img->payload[0] == 0x42);

	free(cmd.payload);
	kitty_image_free_all(&s);
	TEST_PASS("store_payload");
	return (0);
}

/* Test 51: Image replace with payload. */
static int
test_image_replace_with_payload(void)
{
	struct screen			s;
	struct kitty_command		cmd;
	struct kitty_image		*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_PNG;
	cmd.pixel_width = 10;
	cmd.pixel_height = 10;
	cmd.payload_len = 50;
	cmd.payload = xmalloc(50);
	memset(cmd.payload, 0xAA, 50);
	img = kitty_image_store(&s, &cmd);
	free(cmd.payload);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 1;
	cmd.format = KITTY_FORMAT_RGB;
	cmd.pixel_width = 20;
	cmd.pixel_height = 20;
	cmd.payload_len = 100;
	cmd.payload = xmalloc(100);
	memset(cmd.payload, 0xBB, 100);
	img = kitty_image_replace(&s, img, &cmd);
	free(cmd.payload);
	TEST_ASSERT("replace_payload: img != NULL", img != NULL);
	TEST_ASSERT("replace_payload: format == RGB",
	    img->format == KITTY_FORMAT_RGB);
	TEST_ASSERT("replace_payload: width == 20", img->pixel_width == 20);
	TEST_ASSERT("replace_payload: height == 20", img->pixel_height == 20);

	kitty_image_free_all(&s);
	TEST_PASS("replace_payload");
	return (0);
}


/* Test 52: a=T stores an image and placement through the public entry. */
static int
test_parse_dispatch_transmit_display(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 2;
	s.cy = 3;

	ret = kitty_image_parse(&s,
	    "a=T,i=44,f=24,s=2,v=2,c=4,r=1,p=7,C=1;QUJDREVGR0hJSktM",
	    strlen("a=T,i=44,f=24,s=2,v=2,c=4,r=1,p=7,C=1;QUJDREVGR0hJSktM"),
	    &reply);
	TEST_ASSERT("dispatch_T: ret == 0", ret == 0);
	TEST_ASSERT("dispatch_T: reply OK",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	img = kitty_image_find(&s, 44);
	TEST_ASSERT("dispatch_T: image stored", img != NULL);
	TEST_ASSERT("dispatch_T: payload_len == 12", img->payload_len == 12);
	TEST_ASSERT("dispatch_T: payload copied",
	    img->payload != NULL &&
	    memcmp(img->payload, "ABCDEFGHIJKL", 12) == 0);
	pl = kitty_placement_find(&s, 7, 44);
	TEST_ASSERT("dispatch_T: placement stored", pl != NULL);
	TEST_ASSERT("dispatch_T: placement x", pl->pane_x == 2);
	TEST_ASSERT("dispatch_T: placement y", pl->pane_y == 3);
	TEST_ASSERT("dispatch_T: placement cols", pl->cols == 4);
	TEST_ASSERT("dispatch_T: cursor stayed", s.cx == 2 && s.cy == 3);

	kitty_image_free_all(&s);
	TEST_PASS("dispatch_T");
	return (0);
}

/* Test 53: Direct RGB payload is base64 decoded before storage. */
static int
test_transmit_direct_rgb_decodes(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=45,f=24,s=1,v=1;QUJD",
	    strlen("a=t,i=45,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("direct_rgb: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 45);
	TEST_ASSERT("direct_rgb: image stored", img != NULL);
	TEST_ASSERT("direct_rgb: decoded len", img->payload_len == 3);
	TEST_ASSERT("direct_rgb: decoded payload",
	    img->payload != NULL && memcmp(img->payload, "ABC", 3) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("direct_rgb");
	return (0);
}

/* Missing action defaults to transmit. */
static int
test_default_action_transmit(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "i=48,f=24,s=1,v=1;QUJD",
	    strlen("i=48,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("default_action: ret == 0", ret == 0);
	TEST_ASSERT("default_action: reply OK",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	img = kitty_image_find(&s, 48);
	TEST_ASSERT("default_action: image stored", img != NULL);
	TEST_ASSERT("default_action: payload decoded",
	    img != NULL && img->payload_len == 3 &&
	    memcmp(img->payload, "ABC", 3) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("default_action");
	return (0);
}

/* Missing f defaults to RGBA. */
static int
test_default_format_rgba(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=134,s=1,v=1;QUJDRA==",
	    strlen("a=t,i=134,s=1,v=1;QUJDRA=="), &reply);
	TEST_ASSERT("default_format: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 134);
	TEST_ASSERT("default_format: image stored", img != NULL);
	TEST_ASSERT("default_format: format RGBA",
	    img != NULL && img->format == KITTY_FORMAT_RGBA);
	TEST_ASSERT("default_format: payload decoded",
	    img != NULL && img->payload_len == 4 &&
	    memcmp(img->payload, "ABCD", 4) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("default_format");
	return (0);
}

/* Invalid image format or compression is rejected. */
static int
test_transmit_invalid_format_compression(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=135,f=7,s=1,v=1;AAAA",
	    strlen("a=t,i=135,f=7,s=1,v=1;AAAA"), &reply);
	TEST_ASSERT("invalid_format: f ret", ret == -1);
	TEST_ASSERT("invalid_format: f EINVAL",
	    reply != NULL && strstr(reply, "EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("invalid_format: f image not stored",
	    kitty_image_find(&s, 135) == NULL);

	ret = kitty_image_parse(&s, "a=t,i=136,f=24,o=2,s=1,v=1;QUJD",
	    strlen("a=t,i=136,f=24,o=2,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("invalid_format: o ret", ret == -1);
	TEST_ASSERT("invalid_format: o EINVAL",
	    reply != NULL && strstr(reply, "EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("invalid_format: o image not stored",
	    kitty_image_find(&s, 136) == NULL);

	ret = kitty_image_parse(&s, "a=t,i=138,f=24,t=x,s=1,v=1;QUJD",
	    strlen("a=t,i=138,f=24,t=x,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("invalid_format: t ret", ret == -1);
	TEST_ASSERT("invalid_format: t EINVAL",
	    reply != NULL && strstr(reply, "EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("invalid_format: t image not stored",
	    kitty_image_find(&s, 138) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("invalid_format");
	return (0);
}

/* Direct RGB payload must match the declared dimensions. */
static int
test_transmit_direct_rgb_size_mismatch(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=46,f=24,s=2,v=2;QUJD",
	    strlen("a=t,i=46,f=24,s=2,v=2;QUJD"), &reply);
	TEST_ASSERT("direct_rgb_size: ret == -1", ret == -1);
	TEST_ASSERT("direct_rgb_size: ENODATA",
	    reply != NULL && strstr(reply, "ENODATA") != NULL);
	free(reply);
	TEST_ASSERT("direct_rgb_size: image not stored",
	    kitty_image_find(&s, 46) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("direct_rgb_size");
	return (0);
}

/* Direct PNG-format payload is still base64 decoded before storage. */
static int
test_transmit_direct_png_decodes(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=47,f=100,s=1,v=1;QUJD",
	    strlen("a=t,i=47,f=100,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("direct_png: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 47);
	TEST_ASSERT("direct_png: image stored", img != NULL);
	TEST_ASSERT("direct_png: decoded payload",
	    img != NULL && img->payload_len == 3 &&
	    memcmp(img->payload, "ABC", 3) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("direct_png");
	return (0);
}

/* Direct PNG-format payload is still required to be base64 encoded. */
static int
test_transmit_direct_png_rejects_raw(void)
{
	static const char	 data[] =
	    "a=t,i=49,f=100,s=1,v=1;\211PNG\r\n\032\n";
	struct screen		 s;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, data, sizeof data - 1, &reply);
	TEST_ASSERT("direct_png_raw: ret == -1", ret == -1);
	TEST_ASSERT("direct_png_raw: EINVAL",
	    reply != NULL && strstr(reply, "EINVAL") != NULL);
	free(reply);
	TEST_ASSERT("direct_png_raw: image not stored",
	    kitty_image_find(&s, 49) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("direct_png_raw");
	return (0);
}

/* Direct compressed payload preserves the compression flag. */
static int
test_transmit_direct_compression_preserved(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,i=54,f=24,o=z,s=1,v=1;QUJD",
	    strlen("a=t,i=54,f=24,o=z,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("direct_z: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 54);
	TEST_ASSERT("direct_z: image stored", img != NULL);
	TEST_ASSERT("direct_z: compression preserved",
	    img != NULL && img->compression == 1);
	TEST_ASSERT("direct_z: payload decoded",
	    img != NULL && img->payload_len == 3 &&
	    memcmp(img->payload, "ABC", 3) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("direct_z");
	return (0);
}

/* Test 54: t=f reads payload from an actual file path. */
static int
test_transmit_file_regular(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			 path[] = "/tmp/tmux-kitty-file.XXXXXX";
	char			 cmd[1024], encoded[512];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_regular: mkstemp", fd >= 0);
	TEST_ASSERT("file_regular: write", write(fd, "FILEDATA", 8) == 8);
	close(fd);
	TEST_ASSERT("file_regular: encode path",
	    b64_ntop((u_char *)path, strlen(path), encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=55,f=100,t=f,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_regular: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 55);
	TEST_ASSERT("file_regular: image stored", img != NULL);
	TEST_ASSERT("file_regular: payload_len == 8", img->payload_len == 8);
	TEST_ASSERT("file_regular: payload content",
	    img->payload != NULL && memcmp(img->payload, "FILEDATA", 8) == 0);
	TEST_ASSERT("file_regular: source remains", access(path, F_OK) == 0);

	snprintf(cmd, sizeof cmd, "a=t,i=132,f=100,t=f,m=1,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_regular: m ignored ret", ret == 0);
	TEST_ASSERT("file_regular: m ignored reply",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	TEST_ASSERT("file_regular: m ignored no pending",
	    s.kitty_pending.active == 0);
	img = kitty_image_find(&s, 132);
	TEST_ASSERT("file_regular: m ignored image", img != NULL);
	TEST_ASSERT("file_regular: m ignored payload",
	    img != NULL && img->payload_len == 8 &&
	    memcmp(img->payload, "FILEDATA", 8) == 0);

	unlink(path);
	kitty_image_free_all(&s);
	TEST_PASS("file_regular");
	return (0);
}

/* t=f honors byte offset and size. */
static int
test_transmit_file_range(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			 path[] = "/tmp/tmux-kitty-file-range.XXXXXX";
	char			 cmd[1024], encoded[512];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_range: mkstemp", fd >= 0);
	TEST_ASSERT("file_range: write", write(fd, "0123456789", 10) == 10);
	close(fd);
	TEST_ASSERT("file_range: encode path",
	    b64_ntop((u_char *)path, strlen(path), encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=58,f=100,t=f,S=4,O=3,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_range: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 58);
	TEST_ASSERT("file_range: image stored", img != NULL);
	TEST_ASSERT("file_range: sliced payload",
	    img != NULL && img->payload_len == 4 &&
	    memcmp(img->payload, "3456", 4) == 0);
	unlink(path);

	kitty_image_free_all(&s);
	TEST_PASS("file_range");
	return (0);
}

/* t=f rejects invalid and NUL-containing path payloads. */
static int
test_transmit_file_bad_path_payload(void)
{
	static const char	 path[] =
	    { '/', 't', 'm', 'p', '/', 'f', 'o', 'o', '\0', 'b', 'a', 'r' };
	struct screen		 s;
	char			 cmd[1024], encoded[512];
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	TEST_ASSERT("file_bad_path: encode path",
	    b64_ntop((u_char *)path, sizeof path, encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=59,f=100,t=f,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_bad_path: NUL ret", ret == -1);
	TEST_ASSERT("file_bad_path: NUL ENOENT",
	    reply != NULL && strstr(reply, "ENOENT") != NULL);
	free(reply);
	TEST_ASSERT("file_bad_path: NUL image not stored",
	    kitty_image_find(&s, 59) == NULL);

	ret = kitty_image_parse(&s, "a=t,i=60,f=100,t=f,s=1,v=1;!!!",
	    strlen("a=t,i=60,f=100,t=f,s=1,v=1;!!!"), &reply);
	TEST_ASSERT("file_bad_path: invalid ret", ret == -1);
	TEST_ASSERT("file_bad_path: invalid ENOENT",
	    reply != NULL && strstr(reply, "ENOENT") != NULL);
	free(reply);
	TEST_ASSERT("file_bad_path: invalid image not stored",
	    kitty_image_find(&s, 60) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("file_bad_path");
	return (0);
}

/* Test 55: t=t reads and removes a temporary file. */
static int
test_transmit_file_temporary_removes_source(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			 path[] =
	    "/tmp/tty-graphics-protocol-tmux-kitty-temp.XXXXXX";
	char			 cmd[1024], encoded[512];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_temp: mkstemp", fd >= 0);
	TEST_ASSERT("file_temp: write", write(fd, "TMPDATA", 7) == 7);
	close(fd);
	TEST_ASSERT("file_temp: encode path",
	    b64_ntop((u_char *)path, strlen(path), encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=56,f=100,t=t,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_temp: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 56);
	TEST_ASSERT("file_temp: image stored", img != NULL);
	TEST_ASSERT("file_temp: payload content",
	    img->payload != NULL && memcmp(img->payload, "TMPDATA", 7) == 0);
	TEST_ASSERT("file_temp: source removed", access(path, F_OK) == -1);
	kitty_image_free_all(&s);
	TEST_PASS("file_temp");
	return (0);
}

/* Test 56: chunked a=T preserves first-chunk metadata. */
static int
test_chunked_transmit_display_metadata(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 4;
	s.cy = 5;

	ret = kitty_image_parse(&s,
	    "a=T,i=57,f=24,s=1,v=2,c=2,r=2,p=9,m=1;QUJD",
	    strlen("a=T,i=57,f=24,s=1,v=2,c=2,r=2,p=9,m=1;QUJD"),
	    &reply);
	TEST_ASSERT("chunked_T: first ret == 0", ret == 0);
	TEST_ASSERT("chunked_T: first no reply", reply == NULL);
	free(reply);
	ret = kitty_image_parse(&s, "m=0;REVG", strlen("m=0;REVG"), &reply);
	TEST_ASSERT("chunked_T: final ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 57);
	TEST_ASSERT("chunked_T: image id preserved", img != NULL);
	TEST_ASSERT("chunked_T: dimensions preserved",
	    img->pixel_width == 1 && img->pixel_height == 2);
	TEST_ASSERT("chunked_T: payload appended",
	    img->payload_len == 6 && memcmp(img->payload, "ABCDEF", 6) == 0);
	pl = kitty_placement_find(&s, 9, 57);
	TEST_ASSERT("chunked_T: placement created", pl != NULL);
	TEST_ASSERT("chunked_T: placement position",
	    pl->pane_x == 4 && pl->pane_y == 5);
	kitty_image_free_all(&s);
	TEST_PASS("chunked_T");
	return (0);
}


/* Test 57: Writing over an image removes overlapping placements. */
static int
test_check_area_removes_overlapping_placement(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 70;
	cmd.pixel_width = 10;
	cmd.pixel_height = 10;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("check_area: image stored", img != NULL);

	s.cx = 2;
	s.cy = 3;
	cmd.placement_id = 1;
	cmd.cols = 4;
	cmd.rows = 2;
	TEST_ASSERT("check_area: placement created",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	kitty_image_check_area(&s, 0, 0, 1, 1);
	TEST_ASSERT("check_area: non-overlap preserved",
	    TAILQ_FIRST(&s.kitty_placements) != NULL);
	kitty_image_check_area(&s, 3, 4, 1, 1);
	TEST_ASSERT("check_area: overlap removed",
	    TAILQ_FIRST(&s.kitty_placements) == NULL);
	TEST_ASSERT("check_area: named image remains",
	    kitty_image_find(&s, 70) == img);

	kitty_image_free_all(&s);
	TEST_PASS("check_area");
	return (0);
}

/* Screen erases remove anonymous image data with anonymous placements. */
static int
test_anonymous_check_area_removes_data(void)
{
	struct screen	 s;
	char		*reply;
	int		 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 2;
	s.cy = 3;

	ret = kitty_image_parse(&s, "a=T,f=24,s=1,v=1,c=1,r=1;QUJD",
	    strlen("a=T,f=24,s=1,v=1,c=1,r=1;QUJD"), &reply);
	TEST_ASSERT("anonymous_check_area: ret == 0", ret == 0);
	TEST_ASSERT("anonymous_check_area: no reply", reply == NULL);
	TEST_ASSERT("anonymous_check_area: one image",
	    test_count_images(&s) == 1);

	kitty_image_check_area(&s, 2, 3, 1, 1);
	TEST_ASSERT("anonymous_check_area: placement gone",
	    test_count_placements(&s) == 0);
	TEST_ASSERT("anonymous_check_area: data gone",
	    test_count_images(&s) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("anonymous_check_area");
	return (0);
}

/* Test 58: Scrolling moves image placements and removes those scrolled out. */
static int
test_scroll_up_moves_and_removes_placements(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	struct kitty_placement	*pl;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.rupper = 0;
	s.rlower = 10;

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 71;
	cmd.pixel_width = 10;
	cmd.pixel_height = 10;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("scroll_up: image stored", img != NULL);

	s.cx = 1;
	s.cy = 5;
	cmd.placement_id = 1;
	cmd.cols = 2;
	cmd.rows = 2;
	pl = kitty_placement_create(&s, img, &cmd);
	TEST_ASSERT("scroll_up: placement created", pl != NULL);
	kitty_image_scroll_up(&s, 2);
	pl = TAILQ_FIRST(&s.kitty_placements);
	TEST_ASSERT("scroll_up: placement remains", pl != NULL);
	TEST_ASSERT("scroll_up: placement moved", pl->pane_y == 3);
	kitty_image_scroll_up(&s, 4);
	TEST_ASSERT("scroll_up: placement removed",
	    TAILQ_FIRST(&s.kitty_placements) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("scroll_up");
	return (0);
}

/* Test 59: a=T without an id displays an anonymous image. */
static int
test_anonymous_transmit_display(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	s.cx = 1;
	s.cy = 2;

	ret = kitty_image_parse(&s, "a=T,f=24,s=1,v=1,c=1,r=1;QUJD",
	    strlen("a=T,f=24,s=1,v=1,c=1,r=1;QUJD"), &reply);
	TEST_ASSERT("anonymous_T: ret == 0", ret == 0);
	TEST_ASSERT("anonymous_T: no reply", reply == NULL);
	TEST_ASSERT("anonymous_T: one image", test_count_images(&s) == 1);
	TEST_ASSERT("anonymous_T: one placement",
	    test_count_placements(&s) == 1);
	img = TAILQ_FIRST(&s.kitty_images);
	TEST_ASSERT("anonymous_T: id zero", img != NULL && img->id == 0);
	TEST_ASSERT("anonymous_T: decoded payload",
	    img->payload_len == 3 && memcmp(img->payload, "ABC", 3) == 0);
	pl = TAILQ_FIRST(&s.kitty_placements);
	TEST_ASSERT("anonymous_T: placement position",
	    pl != NULL && pl->pane_x == 1 && pl->pane_y == 2);

	kitty_image_free_all(&s);
	TEST_PASS("anonymous_T");
	return (0);
}

/* Test 60: deleting placements frees unreferencable anonymous image data. */
static int
test_anonymous_delete_removes_data(void)
{
	struct screen		 s;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=T,i=130,f=24,s=1,v=1,c=1,r=1;QUJD",
	    strlen("a=T,i=130,f=24,s=1,v=1,c=1,r=1;QUJD"), &reply);
	TEST_ASSERT("anonymous_delete: named ret", ret == 0);
	free(reply);
	ret = kitty_image_parse(&s, "a=T,f=24,s=1,v=1,c=1,r=1;REVG",
	    strlen("a=T,f=24,s=1,v=1,c=1,r=1;REVG"), &reply);
	TEST_ASSERT("anonymous_delete: anonymous ret", ret == 0);
	TEST_ASSERT("anonymous_delete: anonymous no reply", reply == NULL);
	TEST_ASSERT("anonymous_delete: two images", test_count_images(&s) == 2);
	TEST_ASSERT("anonymous_delete: two placements",
	    test_count_placements(&s) == 2);

	ret = kitty_image_parse(&s, "a=d,d=a", strlen("a=d,d=a"), &reply);
	TEST_ASSERT("anonymous_delete: delete ret", ret == 0);
	TEST_ASSERT("anonymous_delete: delete no reply", reply == NULL);
	TEST_ASSERT("anonymous_delete: placements gone",
	    test_count_placements(&s) == 0);
	TEST_ASSERT("anonymous_delete: named data remains",
	    kitty_image_find(&s, 130) != NULL);
	TEST_ASSERT("anonymous_delete: anonymous data removed",
	    test_count_images(&s) == 1);

	kitty_image_free_all(&s);
	TEST_PASS("anonymous_delete");
	return (0);
}

/* Test 61: image number lookup uses the newest matching image. */
static int
test_image_number_newest(void)
{
	struct screen		 s;
	struct kitty_image	*img, *first, *second;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=t,I=42,f=24,s=1,v=1;QUJD",
	    strlen("a=t,I=42,f=24,s=1,v=1;QUJD"), &reply);
	TEST_ASSERT("number_newest: first ret", ret == 0);
	TEST_ASSERT("number_newest: first reply has I",
	    reply != NULL && strstr(reply, "I=42") != NULL);
	free(reply);
	first = TAILQ_FIRST(&s.kitty_images);

	ret = kitty_image_parse(&s, "a=t,I=42,f=24,s=1,v=1;REVG",
	    strlen("a=t,I=42,f=24,s=1,v=1;REVG"), &reply);
	TEST_ASSERT("number_newest: second ret", ret == 0);
	free(reply);
	second = TAILQ_LAST(&s.kitty_images, kitty_images);

	TEST_ASSERT("number_newest: two images", test_count_images(&s) == 2);
	TEST_ASSERT("number_newest: ids assigned",
	    first != NULL && second != NULL && first->id != 0 &&
	    second->id != 0 && first->id != second->id);
	img = kitty_image_find_by_number(&s, 42);
	TEST_ASSERT("number_newest: newest returned", img == second);
	TEST_ASSERT("number_newest: newest payload",
	    img->payload_len == 3 && memcmp(img->payload, "DEF", 3) == 0);

	kitty_image_free_all(&s);
	TEST_PASS("number_newest");
	return (0);
}

/* Test 61: d=i with p deletes only the selected placement. */
static int
test_delete_selector_image_placement(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 80;
	cmd.format = KITTY_FORMAT_PNG;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_i_p: image stored", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.placement_id = 1;
	cmd.cols = 1;
	cmd.rows = 1;
	TEST_ASSERT("delete_i_p: placement 1",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	cmd.placement_id = 2;
	TEST_ASSERT("delete_i_p: placement 2",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=i,i=80,p=1",
	    strlen("a=d,d=i,i=80,p=1"), &reply);
	TEST_ASSERT("delete_i_p: ret == 0", ret == 0);
	TEST_ASSERT("delete_i_p: reply OK",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	pl = kitty_placement_find(&s, 1, 80);
	TEST_ASSERT("delete_i_p: placement 1 gone", pl == NULL);
	pl = kitty_placement_find(&s, 2, 80);
	TEST_ASSERT("delete_i_p: placement 2 remains", pl != NULL);
	TEST_ASSERT("delete_i_p: image remains",
	    kitty_image_find(&s, 80) == img);

	kitty_image_free_all(&s);
	TEST_PASS("delete_i_p");
	return (0);
}

/* Test 62: uppercase d=I deletes image data after placements are gone. */
static int
test_delete_selector_image_data(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 81;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_I: image stored", img != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.cols = 1;
	cmd.rows = 1;
	TEST_ASSERT("delete_I: placement",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=I,i=81",
	    strlen("a=d,d=I,i=81"), &reply);
	TEST_ASSERT("delete_I: ret == 0", ret == 0);
	free(reply);
	TEST_ASSERT("delete_I: image gone", kitty_image_find(&s, 81) == NULL);
	TEST_ASSERT("delete_I: placements gone",
	    TAILQ_EMPTY(&s.kitty_placements));

	kitty_image_free_all(&s);
	TEST_PASS("delete_I");
	return (0);
}

/* Test 63: d=p deletes placements intersecting a one-based cell. */
static int
test_delete_selector_cell(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 82;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_cell: image stored", img != NULL);

	s.cx = 2;
	s.cy = 3;
	memset(&cmd, 0, sizeof cmd);
	cmd.placement_id = 1;
	cmd.cols = 2;
	cmd.rows = 2;
	TEST_ASSERT("delete_cell: placement 1",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	s.cx = 10;
	s.cy = 10;
	cmd.placement_id = 2;
	TEST_ASSERT("delete_cell: placement 2",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=p,x=3,y=4",
	    strlen("a=d,d=p,x=3,y=4"), &reply);
	TEST_ASSERT("delete_cell: ret == 0", ret == 0);
	free(reply);
	pl = kitty_placement_find(&s, 1, 82);
	TEST_ASSERT("delete_cell: placement 1 gone", pl == NULL);
	pl = kitty_placement_find(&s, 2, 82);
	TEST_ASSERT("delete_cell: placement 2 remains", pl != NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_cell");
	return (0);
}

/* Uppercase positional selectors only delete affected image data. */
static int
test_delete_selector_data_scope(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*matched, *unplaced, *other;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 820;
	matched = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_data_scope: matched image", matched != NULL);
	s.cx = 2;
	s.cy = 3;
	cmd.placement_id = 1;
	cmd.cols = 2;
	cmd.rows = 2;
	cmd.zindex = 5;
	TEST_ASSERT("delete_data_scope: matched placement",
	    kitty_placement_create(&s, matched, &cmd) != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 821;
	unplaced = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_data_scope: unplaced image", unplaced != NULL);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 822;
	other = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_data_scope: other image", other != NULL);
	s.cx = 10;
	s.cy = 10;
	cmd.placement_id = 2;
	cmd.cols = 1;
	cmd.rows = 1;
	cmd.zindex = 7;
	TEST_ASSERT("delete_data_scope: other placement",
	    kitty_placement_create(&s, other, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=P,x=3,y=4",
	    strlen("a=d,d=P,x=3,y=4"), &reply);
	TEST_ASSERT("delete_data_scope: P ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_data_scope: matched gone",
	    kitty_image_find(&s, 820) == NULL);
	TEST_ASSERT("delete_data_scope: unplaced remains",
	    kitty_image_find(&s, 821) == unplaced);
	TEST_ASSERT("delete_data_scope: other remains",
	    kitty_image_find(&s, 822) == other);

	ret = kitty_image_parse(&s, "a=d,d=Z,z=7",
	    strlen("a=d,d=Z,z=7"), &reply);
	TEST_ASSERT("delete_data_scope: Z ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_data_scope: other gone",
	    kitty_image_find(&s, 822) == NULL);
	TEST_ASSERT("delete_data_scope: unplaced still remains",
	    kitty_image_find(&s, 821) == unplaced);

	kitty_image_free_all(&s);
	TEST_PASS("delete_data_scope");
	return (0);
}

/* Test 64: d=q deletes placements matching a cell and z-index. */
static int
test_delete_selector_cell_zindex(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	struct kitty_placement	*pl;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 83;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_cell_z: image stored", img != NULL);

	s.cx = 2;
	s.cy = 3;
	memset(&cmd, 0, sizeof cmd);
	cmd.placement_id = 1;
	cmd.cols = 2;
	cmd.rows = 2;
	cmd.zindex = 5;
	TEST_ASSERT("delete_cell_z: placement 1",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	cmd.placement_id = 2;
	cmd.zindex = 7;
	TEST_ASSERT("delete_cell_z: placement 2",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	s.cx = 10;
	s.cy = 10;
	cmd.placement_id = 3;
	cmd.zindex = 5;
	TEST_ASSERT("delete_cell_z: placement 3",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=q,x=3,y=4,z=5",
	    strlen("a=d,d=q,x=3,y=4,z=5"), &reply);
	TEST_ASSERT("delete_cell_z: ret == 0", ret == 0);
	free(reply);
	pl = kitty_placement_find(&s, 1, 83);
	TEST_ASSERT("delete_cell_z: placement 1 gone", pl == NULL);
	pl = kitty_placement_find(&s, 2, 83);
	TEST_ASSERT("delete_cell_z: placement 2 remains", pl != NULL);
	pl = kitty_placement_find(&s, 3, 83);
	TEST_ASSERT("delete_cell_z: placement 3 remains", pl != NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_cell_z");
	return (0);
}

/* Test 65: d=x, d=y, and d=z delete matching placements. */
static int
test_delete_selector_row_column_zindex(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 84;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_rcz: image stored", img != NULL);

	s.cx = 2;
	s.cy = 3;
	memset(&cmd, 0, sizeof cmd);
	cmd.placement_id = 1;
	cmd.cols = 2;
	cmd.rows = 2;
	cmd.zindex = 1;
	TEST_ASSERT("delete_rcz: placement 1",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	s.cx = 10;
	s.cy = 3;
	cmd.placement_id = 2;
	cmd.cols = 1;
	cmd.rows = 1;
	cmd.zindex = 2;
	TEST_ASSERT("delete_rcz: placement 2",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	s.cx = 7;
	s.cy = 10;
	cmd.placement_id = 3;
	cmd.zindex = 3;
	TEST_ASSERT("delete_rcz: placement 3",
	    kitty_placement_create(&s, img, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=x,x=3",
	    strlen("a=d,d=x,x=3"), &reply);
	TEST_ASSERT("delete_rcz: d=x ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_rcz: placement 1 gone",
	    kitty_placement_find(&s, 1, 84) == NULL);
	TEST_ASSERT("delete_rcz: placement 2 remains",
	    kitty_placement_find(&s, 2, 84) != NULL);
	TEST_ASSERT("delete_rcz: placement 3 remains",
	    kitty_placement_find(&s, 3, 84) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=y,y=4",
	    strlen("a=d,d=y,y=4"), &reply);
	TEST_ASSERT("delete_rcz: d=y ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_rcz: placement 2 gone",
	    kitty_placement_find(&s, 2, 84) == NULL);
	TEST_ASSERT("delete_rcz: placement 3 still remains",
	    kitty_placement_find(&s, 3, 84) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=z,z=3",
	    strlen("a=d,d=z,z=3"), &reply);
	TEST_ASSERT("delete_rcz: d=z ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_rcz: placement 3 gone",
	    kitty_placement_find(&s, 3, 84) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_rcz");
	return (0);
}

/* Test 66: d=n targets the newest image with the requested image number. */
static int
test_delete_selector_number_newest(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*first, *second;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 90;
	cmd.image_number = 9;
	first = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_n: first image", first != NULL);
	TEST_ASSERT("delete_n: first placement",
	    kitty_placement_create(&s, first, &cmd) != NULL);
	cmd.image_id = 91;
	second = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_n: second image", second != NULL);
	TEST_ASSERT("delete_n: second placement",
	    kitty_placement_create(&s, second, &cmd) != NULL);

	ret = kitty_image_parse(&s, "a=d,d=n,I=9",
	    strlen("a=d,d=n,I=9"), &reply);
	TEST_ASSERT("delete_n: ret == 0", ret == 0);
	free(reply);
	TEST_ASSERT("delete_n: newest placement gone",
	    kitty_image_has_placements(&s, second) == 0);
	TEST_ASSERT("delete_n: older placement remains",
	    kitty_image_has_placements(&s, first) == 1);
	TEST_ASSERT("delete_n: data remains",
	    kitty_image_find(&s, 91) == second);

	ret = kitty_image_parse(&s, "a=d,d=N,I=9",
	    strlen("a=d,d=N,I=9"), &reply);
	TEST_ASSERT("delete_n: uppercase ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_n: newest data gone",
	    kitty_image_find(&s, 91) == NULL);
	TEST_ASSERT("delete_n: older now newest",
	    kitty_image_find_by_number(&s, 9) == first);

	ret = kitty_image_parse(&s, "a=d,d=N,I=9",
	    strlen("a=d,d=N,I=9"), &reply);
	TEST_ASSERT("delete_n: uppercase older ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_n: older data gone",
	    kitty_image_find(&s, 90) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_n");
	return (0);
}

/* Test 65: d=R deletes image id ranges and their data. */
static int
test_delete_selector_id_range(void)
{
	struct screen		 s;
	struct kitty_command	 cmd;
	struct kitty_image	*img;
	char			*reply;
	int			 ret;
	u_int			 i;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	for (i = 100; i <= 102; i++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.image_id = i;
		img = kitty_image_store(&s, &cmd);
		TEST_ASSERT("delete_range: image", img != NULL);
		TEST_ASSERT("delete_range: placement",
		    kitty_placement_create(&s, img, &cmd) != NULL);
	}

	ret = kitty_image_parse(&s, "a=d,d=R,x=100,y=101",
	    strlen("a=d,d=R,x=100,y=101"), &reply);
	TEST_ASSERT("delete_range: ret == 0", ret == 0);
	free(reply);
	TEST_ASSERT("delete_range: 100 gone", kitty_image_find(&s, 100) == NULL);
	TEST_ASSERT("delete_range: 101 gone", kitty_image_find(&s, 101) == NULL);
	TEST_ASSERT("delete_range: 102 remains",
	    kitty_image_find(&s, 102) != NULL);

	kitty_image_free_all(&s);

	for (i = 1; i <= 3; i++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.image_id = i;
		img = kitty_image_store(&s, &cmd);
		TEST_ASSERT("delete_range: low image", img != NULL);
		TEST_ASSERT("delete_range: low placement",
		    kitty_placement_create(&s, img, &cmd) != NULL);
	}
	ret = kitty_image_parse(&s, "a=d,d=R,y=2",
	    strlen("a=d,d=R,y=2"), &reply);
	TEST_ASSERT("delete_range: omitted low ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_range: 1 gone", kitty_image_find(&s, 1) == NULL);
	TEST_ASSERT("delete_range: 2 gone", kitty_image_find(&s, 2) == NULL);
	TEST_ASSERT("delete_range: 3 remains",
	    kitty_image_find(&s, 3) != NULL);

	kitty_image_free_all(&s);

	memset(&cmd, 0, sizeof cmd);
	cmd.image_id = 200;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_range: unplaced image", img != NULL);
	cmd.image_id = 201;
	img = kitty_image_store(&s, &cmd);
	TEST_ASSERT("delete_range: targeted image", img != NULL);
	TEST_ASSERT("delete_range: targeted placement",
	    kitty_placement_create(&s, img, &cmd) != NULL);
	ret = kitty_image_parse(&s, "a=d,d=R,x=201,y=201",
	    strlen("a=d,d=R,x=201,y=201"), &reply);
	TEST_ASSERT("delete_range: targeted ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_range: unplaced remains",
	    kitty_image_find(&s, 200) != NULL);
	TEST_ASSERT("delete_range: targeted gone",
	    kitty_image_find(&s, 201) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_range");
	return (0);
}

/* Test 66: a delete command aborts an incomplete chunked upload. */
static int
test_delete_aborts_chunked_upload(void)
{
	struct screen		 s;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);
	memset(&s.kitty_pending, 0, sizeof s.kitty_pending);

	ret = kitty_image_parse(&s, "a=T,i=110,f=24,s=1,v=1,m=1;QUJD",
	    strlen("a=T,i=110,f=24,s=1,v=1,m=1;QUJD"), &reply);
	TEST_ASSERT("delete_abort: first ret", ret == 0);
	TEST_ASSERT("delete_abort: first no reply", reply == NULL);
	free(reply);
	TEST_ASSERT("delete_abort: pending active", s.kitty_pending.active == 1);

	ret = kitty_image_parse(&s, "a=d,d=a", strlen("a=d,d=a"), &reply);
	TEST_ASSERT("delete_abort: delete ret", ret == 0);
	free(reply);
	TEST_ASSERT("delete_abort: pending cleared", s.kitty_pending.active == 0);
	TEST_ASSERT("delete_abort: no partial image",
	    kitty_image_find(&s, 110) == NULL);

	kitty_image_free_all(&s);
	TEST_PASS("delete_abort");
	return (0);
}

/* Test 67: quiet mode distinguishes OK and failure responses. */
static int
test_quiet_failure_modes(void)
{
	struct screen		 s;
	char			*reply;
	int			 ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	ret = kitty_image_parse(&s, "a=p,i=999,q=1",
	    strlen("a=p,i=999,q=1"), &reply);
	TEST_ASSERT("quiet_failure: q=1 ret", ret == -1);
	TEST_ASSERT("quiet_failure: q=1 replies",
	    reply != NULL && strstr(reply, "ENOENT") != NULL);
	free(reply);

	ret = kitty_image_parse(&s, "a=p,i=999,q=2",
	    strlen("a=p,i=999,q=2"), &reply);
	TEST_ASSERT("quiet_failure: q=2 ret", ret == -1);
	TEST_ASSERT("quiet_failure: q=2 no reply", reply == NULL);

	ret = kitty_image_parse(&s, "a=t,i=121,f=24,s=1,v=1,q=2;QUJD",
	    strlen("a=t,i=121,f=24,s=1,v=1,q=2;QUJD"), &reply);
	TEST_ASSERT("quiet_failure: q=2 OK ret", ret == 0);
	TEST_ASSERT("quiet_failure: q=2 OK no reply", reply == NULL);
	TEST_ASSERT("quiet_failure: q=2 image stored",
	    kitty_image_find(&s, 121) != NULL);

	kitty_image_free_all(&s);
	TEST_PASS("quiet_failure");
	return (0);
}

/* Test 68: t=t only removes protocol-approved temporary paths. */
static int
test_transmit_file_temporary_keeps_unapproved_source(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	FILE			*f;
	char			 dir[] = "/tmp/tty-graphics-protocol-dir.XXXXXX";
	char			 path[] = "/tmp/tmux-kitty-unsafe-temp.XXXXXX";
	char			 path2[512];
	char			 cmd[1024], encoded[512];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_temp_keep: mkstemp", fd >= 0);
	TEST_ASSERT("file_temp_keep: write", write(fd, "KEEP", 4) == 4);
	close(fd);
	TEST_ASSERT("file_temp_keep: encode path",
	    b64_ntop((u_char *)path, strlen(path), encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=120,f=100,t=t,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_temp_keep: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 120);
	TEST_ASSERT("file_temp_keep: image stored", img != NULL);
	TEST_ASSERT("file_temp_keep: source remains", access(path, F_OK) == 0);
	unlink(path);

	TEST_ASSERT("file_temp_keep: mkdtemp", mkdtemp(dir) != NULL);
	snprintf(path2, sizeof path2, "%s/victim", dir);
	f = fopen(path2, "wb");
	TEST_ASSERT("file_temp_keep: fopen nested", f != NULL);
	TEST_ASSERT("file_temp_keep: fwrite nested",
	    fwrite("DIRK", 1, 4, f) == 4);
	fclose(f);
	TEST_ASSERT("file_temp_keep: encode nested path",
	    b64_ntop((u_char *)path2, strlen(path2), encoded,
	    sizeof encoded) != -1);
	snprintf(cmd, sizeof cmd, "a=t,i=122,f=100,t=t,s=1,v=1;%s",
	    encoded);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_temp_keep: nested ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 122);
	TEST_ASSERT("file_temp_keep: nested image stored", img != NULL);
	TEST_ASSERT("file_temp_keep: nested source remains",
	    access(path2, F_OK) == 0);
	unlink(path2);
	rmdir(dir);

	kitty_image_free_all(&s);
	TEST_PASS("file_temp_keep");
	return (0);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("Kitty graphics protocol unit tests\n");
	printf("==================================\n\n");

	/* Parser tests. */
	test_parse_query();
	test_parse_transmit();
	test_parse_place();
	test_parse_delete();
	test_parse_payload();
	test_parse_empty_payload();
	test_parse_unknown_keys();
	test_parse_malformed();
	test_parse_character_values();
	test_parse_quiet();
	test_parse_chunked();
	test_parse_image_number();
	test_parse_source_rect();
	test_parse_relative_placement();
	test_parse_file_range();
	test_parse_virtual();
	test_parse_empty();
	test_parse_semicolon_only();
	test_parse_payload_only();
	test_parse_duplicate_keys();
	test_parse_large_control();
	test_parse_negative_zindex();
	test_parse_negative_unsigned();
	test_parse_multiple_commas();

	/* State tests. */
	test_image_store_find();
	test_image_number_find();
	test_image_store_with_payload();
	test_image_replace();
	test_image_replace_with_payload();
	test_parse_dispatch_transmit_display();
	test_transmit_direct_rgb_decodes();
	test_default_action_transmit();
	test_default_format_rgba();
	test_transmit_invalid_format_compression();
	test_transmit_direct_rgb_size_mismatch();
	test_transmit_direct_png_decodes();
	test_transmit_direct_png_rejects_raw();
	test_transmit_direct_compression_preserved();
	test_transmit_file_regular();
	test_transmit_file_range();
	test_transmit_file_bad_path_payload();
	test_transmit_file_temporary_removes_source();
	test_transmit_file_temporary_keeps_unapproved_source();
	test_chunked_transmit_display_metadata();
	test_anonymous_transmit_display();
	test_anonymous_delete_removes_data();
	test_image_number_newest();
	test_check_area_removes_overlapping_placement();
	test_anonymous_check_area_removes_data();
	test_scroll_up_moves_and_removes_placements();
	test_placement_create();
	test_free_all();
	test_free_saved_lists();

	/* Deletion tests. */
	test_delete_image();
	test_delete_all_placements();
	test_delete_all();
	test_delete_cursor();
	test_delete_zindex();
	test_delete_selector_image_placement();
	test_delete_selector_image_data();
	test_delete_selector_cell();
	test_delete_selector_data_scope();
	test_delete_selector_cell_zindex();
	test_delete_selector_row_column_zindex();
	test_delete_selector_number_newest();
	test_delete_selector_id_range();
	test_delete_aborts_chunked_upload();

	/* Memory limit tests. */
	test_memory_limits_images();
	test_memory_limits_replace_at_limit();
	test_memory_limits_payload();
	test_memory_limits_placements();

	/* Handler tests. */
	test_build_reply();
	test_build_reply_zero();
	test_handle_query_quiet();
	test_handle_query_loud();
	test_handle_query_missing_id();
	test_handle_query_validation();
	test_handle_transmit_empty();
	test_handle_transmit_file_no_path();
	test_handle_transmit_shm();
	test_handle_transmit_cursor_move();
	test_handle_transmit_cursor_stay();
	test_handle_transmit_virtual();
	test_handle_place_missing();
	test_handle_place_no_reference();
	test_relative_placement_unsupported();
	test_unsupported_animation_actions();
	test_unknown_actions_rejected();
	test_image_id_number_conflict();
	test_handle_place_replace();
	test_handle_place_virtual();
	test_handle_place_by_number();
	test_handle_delete_default();
	test_quiet_failure_modes();
	test_chunked_upload();

	printf("\n==================================\n");
	printf("Tests run: %d\n", tests_run);
	printf("Tests failed: %d\n", tests_failed);
	printf("Tests passed: %d\n", tests_run - tests_failed);

	return (tests_failed > 0 ? 1 : 0);
}

#endif /* KITTY_TEST */
