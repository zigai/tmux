#ifndef GRID_PROMPTS_H
#define GRID_PROMPTS_H

enum grid_prompt_state {
	GRID_PROMPT_RUNNING,
	GRID_PROMPT_COMPLETED,
	GRID_PROMPT_TRUNCATED,
	GRID_PROMPT_BOUNDARY_UNKNOWN
};

struct grid_prompt_block {
	enum grid_prompt_state	state;
	u_int			prompt_y;
	u_int			command_y;
	u_int			command_x;
	u_int			output_y;
	u_int			output_x;
	u_int			end_y;
	u_int			end_x;
	int			command_present;
	int			status;
};

int grid_prompt_find(struct grid *, u_int, u_int, struct grid_prompt_block *);
const char *grid_prompt_state_name(enum grid_prompt_state);
char *grid_prompt_capture(struct grid *, struct screen *,
    struct grid_prompt_block *, size_t *);

#endif
