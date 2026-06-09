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
	ret = kitty_parse_control("a=d,i=5", strlen("a=d,i=5"), &cmd);

	TEST_ASSERT("parse_delete: ret == 0", ret == 0);
	TEST_ASSERT("parse_delete: action == d", cmd.action == KITTY_ACTION_DELETE);
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

	memset(&cmd2, 0, sizeof cmd2);
	cmd2.image_id = 7;
	cmd2.format = KITTY_FORMAT_RGB;
	cmd2.pixel_width = 20;
	cmd2.pixel_height = 20;
	img = kitty_image_replace(&s, img, &cmd2);
	TEST_ASSERT("replace: replace OK", img != NULL);
	TEST_ASSERT("replace: format == RGB", img->format == KITTY_FORMAT_RGB);
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

	ret = kitty_delete_all(&s);
	TEST_ASSERT("delete_all: ret == 1", ret == 1);
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
	ret = kitty_delete_cursor(&s);
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

	ret = kitty_delete_zindex(&s, 5);
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
	cmd.quiet = 1;
	ret = kitty_handle_query(NULL, &cmd, &reply);
	TEST_ASSERT("query_quiet: ret == 0", ret == 0);
	TEST_ASSERT("query_quiet: reply == NULL", reply == NULL);

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
	cmd.quiet = 0;
	ret = kitty_handle_query(NULL, &cmd, &reply);
	TEST_ASSERT("query_loud: ret == 0", ret == 0);
	TEST_ASSERT("query_loud: reply != NULL", reply != NULL);
	TEST_ASSERT("query_loud: contains OK",
	    strstr(reply, "OK") != NULL);
	free(reply);

	TEST_PASS("query_loud");
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
	if (img != NULL)
		img->payload = NULL; /* prevent double free */

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

	kitty_image_free_all(&s);
	TEST_PASS("place_missing");
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

	ret = kitty_image_parse(&s, "a=t,i=1,f=100,s=1,v=1,m=1;chunk1",
	    strlen("a=t,i=1,f=100,s=1,v=1,m=1;chunk1"), &reply);
	TEST_ASSERT("chunked_upload: m=1 sets pending", ret == 0);
	TEST_ASSERT("chunked_upload: pending.active == 1",
	    s.kitty_pending.active == 1);
	TEST_ASSERT("chunked_upload: pending.id == 1",
	    s.kitty_pending.id == 1);
	TEST_ASSERT("chunked_upload: pending.payload_len == 6",
	    s.kitty_pending.payload_len == 6);
	free(reply);

	ret = kitty_image_parse(&s, "m=0;chunk2", strlen("m=0;chunk2"),
	    &reply);
	TEST_ASSERT("chunked_upload: m=0 completes", ret == 0);
	TEST_ASSERT("chunked_upload: pending.active == 0",
	    s.kitty_pending.active == 0);

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
	free(reply);

	pl = kitty_placement_find(&s, 9, 0);
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
	if (img != NULL)
		img->payload = NULL;
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
	if (img != NULL)
		img->payload = NULL;

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
	if (img != NULL)
		img->payload = NULL;
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
	    "a=T,i=44,f=24,s=2,v=2,c=4,r=1,p=7,C=1;UkdCWA==",
	    strlen("a=T,i=44,f=24,s=2,v=2,c=4,r=1,p=7,C=1;UkdCWA=="),
	    &reply);
	TEST_ASSERT("dispatch_T: ret == 0", ret == 0);
	TEST_ASSERT("dispatch_T: reply OK",
	    reply != NULL && strstr(reply, "OK") != NULL);
	free(reply);
	img = kitty_image_find(&s, 44);
	TEST_ASSERT("dispatch_T: image stored", img != NULL);
	TEST_ASSERT("dispatch_T: payload_len == 4", img->payload_len == 4);
	TEST_ASSERT("dispatch_T: payload copied",
	    img->payload != NULL && memcmp(img->payload, "RGBX", 4) == 0);
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

/* Test 54: t=f reads payload from an actual file path. */
static int
test_transmit_file_regular(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			 path[] = "/tmp/tmux-kitty-file.XXXXXX";
	char			 cmd[256];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_regular: mkstemp", fd >= 0);
	TEST_ASSERT("file_regular: write", write(fd, "FILEDATA", 8) == 8);
	close(fd);
	snprintf(cmd, sizeof cmd, "a=t,i=55,f=100,t=f,s=1,v=1;%s", path);
	ret = kitty_image_parse(&s, cmd, strlen(cmd), &reply);
	TEST_ASSERT("file_regular: ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 55);
	TEST_ASSERT("file_regular: image stored", img != NULL);
	TEST_ASSERT("file_regular: payload_len == 8", img->payload_len == 8);
	TEST_ASSERT("file_regular: payload content",
	    img->payload != NULL && memcmp(img->payload, "FILEDATA", 8) == 0);
	TEST_ASSERT("file_regular: source remains", access(path, F_OK) == 0);
	unlink(path);
	kitty_image_free_all(&s);
	TEST_PASS("file_regular");
	return (0);
}

/* Test 55: t=t reads and removes a temporary file. */
static int
test_transmit_file_temporary_removes_source(void)
{
	struct screen		 s;
	struct kitty_image	*img;
	char			 path[] = "/tmp/tmux-kitty-temp.XXXXXX";
	char			 cmd[256];
	char			*reply;
	int			 fd, ret;

	memset(&s, 0, sizeof s);
	TAILQ_INIT(&s.kitty_images);
	TAILQ_INIT(&s.kitty_placements);

	fd = mkstemp(path);
	TEST_ASSERT("file_temp: mkstemp", fd >= 0);
	TEST_ASSERT("file_temp: write", write(fd, "TMPDATA", 7) == 7);
	close(fd);
	snprintf(cmd, sizeof cmd, "a=t,i=56,f=100,t=t,s=1,v=1;%s", path);
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
	    "a=T,i=57,f=100,s=3,v=2,c=2,r=2,p=9,m=1;AAA",
	    strlen("a=T,i=57,f=100,s=3,v=2,c=2,r=2,p=9,m=1;AAA"),
	    &reply);
	TEST_ASSERT("chunked_T: first ret == 0", ret == 0);
	free(reply);
	ret = kitty_image_parse(&s, "m=0;BBB", strlen("m=0;BBB"), &reply);
	TEST_ASSERT("chunked_T: final ret == 0", ret == 0);
	free(reply);
	img = kitty_image_find(&s, 57);
	TEST_ASSERT("chunked_T: image id preserved", img != NULL);
	TEST_ASSERT("chunked_T: dimensions preserved",
	    img->pixel_width == 3 && img->pixel_height == 2);
	TEST_ASSERT("chunked_T: payload appended",
	    img->payload_len == 6 && memcmp(img->payload, "AAABBB", 6) == 0);
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

	kitty_image_free_all(&s);
	TEST_PASS("check_area");
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
	test_parse_quiet();
	test_parse_chunked();
	test_parse_image_number();
	test_parse_source_rect();
	test_parse_virtual();
	test_parse_empty();
	test_parse_semicolon_only();
	test_parse_payload_only();
	test_parse_duplicate_keys();
	test_parse_large_control();
	test_parse_negative_zindex();
	test_parse_multiple_commas();

	/* State tests. */
	test_image_store_find();
	test_image_number_find();
	test_image_store_with_payload();
	test_image_replace();
	test_image_replace_with_payload();
	test_parse_dispatch_transmit_display();
	test_transmit_direct_rgb_decodes();
	test_transmit_file_regular();
	test_transmit_file_temporary_removes_source();
	test_chunked_transmit_display_metadata();
	test_check_area_removes_overlapping_placement();
	test_scroll_up_moves_and_removes_placements();
	test_placement_create();
	test_free_all();

	/* Deletion tests. */
	test_delete_image();
	test_delete_all_placements();
	test_delete_all();
	test_delete_cursor();
	test_delete_zindex();

	/* Memory limit tests. */
	test_memory_limits_images();
	test_memory_limits_payload();
	test_memory_limits_placements();

	/* Handler tests. */
	test_build_reply();
	test_build_reply_zero();
	test_handle_query_quiet();
	test_handle_query_loud();
	test_handle_transmit_empty();
	test_handle_transmit_file_no_path();
	test_handle_transmit_shm();
	test_handle_transmit_cursor_move();
	test_handle_transmit_cursor_stay();
	test_handle_transmit_virtual();
	test_handle_place_missing();
	test_handle_place_replace();
	test_handle_place_virtual();
	test_handle_place_by_number();
	test_handle_delete_default();
	test_chunked_upload();

	printf("\n==================================\n");
	printf("Tests run: %d\n", tests_run);
	printf("Tests failed: %d\n", tests_failed);
	printf("Tests passed: %d\n", tests_run - tests_failed);

	return (tests_failed > 0 ? 1 : 0);
}

#endif /* KITTY_TEST */
