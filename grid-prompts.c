#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "grid-prompts.h"

const char *
grid_prompt_state_name(enum grid_prompt_state state)
{
	switch (state) {
	case GRID_PROMPT_RUNNING:
		return ("running");
	case GRID_PROMPT_COMPLETED:
		return ("completed");
	case GRID_PROMPT_TRUNCATED:
		return ("truncated");
	case GRID_PROMPT_BOUNDARY_UNKNOWN:
		return ("boundary-unknown");
	}
	return ("boundary-unknown");
}

static void
grid_prompt_block_at(struct grid *gd, u_int y, struct grid_prompt_block *block)
{
	struct grid_line	*gl;
	u_int			 i, limit;
	int			 prompt = 0, command = 0, ended = 0;
	int			 invalid = 0, known = 0;

	gl = grid_get_line(gd, y);
	block->output_y = y;
	block->output_x = gl->osc133_data.out_start_col;
	block->prompt_y = y;
	block->command_y = y;
	block->command_x = 0;
	block->command_present = 0;
	block->status = -1;
	limit = gd->hsize + gd->sy;
	block->end_y = limit - 1;
	block->end_x = grid_get_line(gd, limit - 1)->cellused;

	for (i = y + 1; i > 0; i--) {
		gl = grid_get_line(gd, i - 1);
		if (i - 1 != y && (gl->flags & GRID_LINE_START_OUTPUT))
			break;
		if (gl->flags & GRID_LINE_START_COMMAND) {
			command = 1;
			block->command_y = i - 1;
			block->command_x = gl->osc133_data.cmd_col;
		}
		if (gl->flags & GRID_LINE_START_PROMPT) {
			prompt = 1;
			block->prompt_y = i - 1;
			break;
		}
	}
	block->command_present = prompt && command;
	if (block->command_present &&
	    ((block->prompt_y == block->command_y &&
	      grid_get_line(gd, block->prompt_y)->osc133_data.prompt_col >
	      block->command_x) ||
	     (block->command_y == block->output_y &&
	      block->command_x > block->output_x)))
		block->command_present = 0;

	for (i = y; i < limit; i++) {
		gl = grid_get_line(gd, i);
		if (gl->flags & GRID_LINE_END_OUTPUT) {
			if (i == y && gl->osc133_data.out_end_col <
			    block->output_x) {
				invalid = 1;
				goto next_marker;
			}
			if (!(gl->flags & GRID_LINE_START_PROMPT) ||
			    gl->osc133_data.out_end_col <=
			    gl->osc133_data.prompt_col) {
				block->end_y = i;
				block->end_x = gl->osc133_data.out_end_col;
				block->status = gl->osc133_data.exit_status;
				known = gl->osc133_data.status_known;
				ended = 1;
				break;
			}
		}
 next_marker:
		if (i != y && (gl->flags & (GRID_LINE_START_PROMPT|
		    GRID_LINE_START_OUTPUT))) {
			block->end_y = i;
			block->end_x = (gl->flags & GRID_LINE_START_PROMPT) ?
			    gl->osc133_data.prompt_col :
			    gl->osc133_data.out_start_col;
			break;
		}
	}

	if (invalid)
		block->state = GRID_PROMPT_BOUNDARY_UNKNOWN;
	else if (!block->command_present) {
		if (gd->scroll_collected != 0 && !prompt)
			block->state = GRID_PROMPT_TRUNCATED;
		else
			block->state = GRID_PROMPT_BOUNDARY_UNKNOWN;
	} else if (ended && known)
		block->state = GRID_PROMPT_COMPLETED;
	else if (ended)
		block->state = GRID_PROMPT_BOUNDARY_UNKNOWN;
	else if (i == limit)
		block->state = GRID_PROMPT_RUNNING;
	else
		block->state = GRID_PROMPT_BOUNDARY_UNKNOWN;
	if (block->state != GRID_PROMPT_COMPLETED)
		block->status = -1;
}

int
grid_prompt_find(struct grid *gd, u_int line, u_int skip,
    struct grid_prompt_block *block)
{
	u_int			 i, limit, oldest;
	struct grid_line	*gl;

	limit = gd->hsize + gd->sy;
	if (limit == 0)
		return (0);
	if (line >= limit)
		line = limit - 1;
	oldest = line + 1;
	for (i = line + 1; i > 0; i--) {
		gl = grid_get_line(gd, i - 1);
		if (!(gl->flags & GRID_LINE_START_OUTPUT))
			continue;
		oldest = i - 1;
		if (skip-- != 0)
			continue;
		grid_prompt_block_at(gd, i - 1, block);
		return (1);
	}
	if (skip != 0 || gd->scroll_collected == 0)
		return (0);
	for (i = oldest; i > 0; i--) {
		gl = grid_get_line(gd, i - 1);
		if (!(gl->flags & GRID_LINE_END_OUTPUT))
			continue;
		block->state = GRID_PROMPT_TRUNCATED;
		block->prompt_y = 0;
		block->command_y = 0;
		block->command_x = 0;
		block->command_present = 0;
		block->output_y = 0;
		block->output_x = 0;
		block->end_y = i - 1;
		block->end_x = gl->osc133_data.out_end_col;
		block->status = -1;
		return (1);
	}
	return (0);
}

char *
grid_prompt_capture(struct grid *gd, struct screen *s,
    struct grid_prompt_block *block, size_t *len)
{
	struct grid_line	*gl;
	struct grid_cell	*gc = NULL;
	char			*buf, *line;
	size_t			 capacity, needed, used;
	u_int			 y, x, end;

	buf = xstrdup("");
	*len = 0;
	capacity = 1;
	for (y = block->output_y; y <= block->end_y; y++) {
		gl = grid_get_line(gd, y);
		x = y == block->output_y ? block->output_x : 0;
		end = y == block->end_y ? block->end_x : gd->sx;
		if (end > gd->sx)
			end = gd->sx;
		if (x > end)
			x = end;
		line = grid_string_cells(gd, x, y, end - x, &gc,
		    GRID_STRING_TRIM_SPACES|GRID_STRING_EMPTY_CELLS, s);
		used = strlen(line);
		needed = *len + used + 2;
		if (needed > capacity) {
			capacity = needed + needed / 2;
			buf = xrealloc(buf, capacity);
		}
		memcpy(buf + *len, line, used);
		*len += used;
		if (y != block->end_y && !(gl->flags & GRID_LINE_WRAPPED))
			buf[(*len)++] = '\n';
		buf[*len] = '\0';
		free(line);
	}
	return (buf);
}
