#include <sys/types.h>

#include <stdlib.h>
#include <unistd.h>

#include "tmux.h"

int
pane_command_prefix(struct spawn_context *sc, struct window_pane *wp,
    int *argc, char ***argv, char **cause)
{
	struct format_tree	*ft;
	const char		*prefix;
	char			*expanded, *error;

	*argc = 0;
	*argv = NULL;
	if ((sc->flags & (SPAWN_EMPTY|SPAWN_MODAL)) || wp->window->modal == wp)
		return (0);
	prefix = options_get_string(sc->s->options, "pane-command-prefix");
	if (*prefix == '\0')
		return (0);

	ft = format_create(NULL, NULL, FORMAT_NONE, FORMAT_NOJOBS);
	format_defaults(ft, sc->tc, sc->s, sc->wl, wp);
	expanded = format_expand(ft, prefix);
	format_free(ft);
	if (cmd_parse_argv(expanded, argc, argv, &error) == 0 &&
	    *argc != 0 && *(*argv)[0] != '\0') {
		free(expanded);
		return (0);
	}
	if (error == NULL)
		error = xstrdup("empty command");
	xasprintf(cause, "pane-command-prefix: %s", error);
	free(error);
	free(expanded);
	cmd_free_argv(*argc, *argv);
	*argc = 0;
	*argv = NULL;
	return (-1);
}

void
pane_command_exec(struct window_pane *wp, int argc, char **argv)
{
	int	index;

	if (argc == 0)
		return;

	if (wp->argc > 1) {
		for (index = 0; index < wp->argc; index++)
			cmd_append_argv(&argc, &argv, wp->argv[index]);
	} else {
		cmd_append_argv(&argc, &argv, wp->shell);
		if (wp->argc == 1) {
			cmd_append_argv(&argc, &argv, "-c");
			cmd_append_argv(&argc, &argv, wp->argv[0]);
		} else
			cmd_append_argv(&argc, &argv, "-l");
	}
	argv = xreallocarray(argv, argc + 1, sizeof *argv);
	argv[argc] = NULL;
	execvp(argv[0], argv);
	_exit(1);
}
