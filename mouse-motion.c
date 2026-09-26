#include <sys/types.h>

#include <stdlib.h>

#include "tmux.h"

struct mouse_motion_state {
	u_int	pane;
	u_int	x;
	u_int	y;
	u_int	viewport;
	u_int	generation;
};

void
mouse_motion_fire(struct client *c, struct window_pane *wp, u_int x, u_int y)
{
	struct mouse_motion_state	*state = c->mouse_motion;
	struct event_payload		*ep;
	struct cmd_find_state		 fs;
	struct window_mode_entry	*wme;
	struct grid			*gd = wp->base.grid;
	u_int				 offset, size, scroll = 0, viewport;

	wme = TAILQ_FIRST(&wp->modes);
	if (wme != NULL && wme->mode == &window_copy_mode &&
	    window_copy_get_current_offset(wp, &offset, &size))
		scroll = size - offset;
	viewport = gd->scroll_collected + gd->hsize - scroll;
	if (state != NULL && state->pane == wp->id && state->x == x &&
	    state->y == y && state->viewport == viewport &&
	    state->generation == gd->scroll_generation)
		return;
	if (state == NULL)
		state = c->mouse_motion = xcalloc(1, sizeof *state);
	state->pane = wp->id;
	state->x = x;
	state->y = y;
	state->viewport = viewport;
	state->generation = gd->scroll_generation;

	ep = event_payload_create();
	cmd_find_from_pane(&fs, wp, 0);
	event_payload_set_target(ep, &fs);
	event_payload_set_client(ep, "client", c);
	event_payload_set_pane(ep, "pane", wp);
	event_payload_set_uint(ep, "x", x);
	event_payload_set_uint(ep, "y", y);
	event_payload_set_uint(ep, "scroll_position", scroll);
	event_payload_set_uint(ep, "history_generation", gd->scroll_generation);
	events_fire("pane-mouse-moved", ep);
}

void
mouse_motion_free(struct client *c)
{
	free(c->mouse_motion);
	c->mouse_motion = NULL;
}
