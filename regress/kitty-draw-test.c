/* $OpenBSD$ */

/*
 * End-to-end PTY draw, caching, and multi-client test for Kitty graphics.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__APPLE__)
#include <util.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char	 tmux_bin[1024];
static char	 socket_label[64];
static char	 data_file[128];

static void
run_tmux(const char *cmd)
{
	char	buf[2048];

	snprintf(buf, sizeof buf, "%s -L%s -f/dev/null %s >/dev/null 2>&1",
	    tmux_bin, socket_label, cmd);
	system(buf);
}

static int
check_support(void)
{
	char	 buf[2048], out[256];
	FILE	*f;

	snprintf(buf, sizeof buf,
	    "%s -L%s -f/dev/null start-server \\; display -p '#{image_support}'",
	    tmux_bin, socket_label);
	f = popen(buf, "r");
	if (f == NULL)
		return (0);
	if (fgets(out, sizeof out, f) == NULL) {
		pclose(f);
		return (0);
	}
	pclose(f);
	run_tmux("kill-server");
	return (strstr(out, "kitty") != NULL);
}

static size_t
read_pty_until(int fd, char *buf, size_t max, const char *needle,
    double timeout)
{
	struct pollfd	 pfd;
	size_t		 len = 0;
	time_t		 start = time(NULL);
	ssize_t		 n;

	buf[0] = '\0';
	while (difftime(time(NULL), start) < timeout && len < max - 1) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, buf + len, max - 1 - len);
		if (n <= 0) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			break;
		}
		len += n;
		buf[len] = '\0';
		if (needle != NULL && strstr(buf, needle) != NULL)
			break;
	}
	return (len);
}

static void
write_test_images(void)
{
	FILE	*f;
	int	 fd;

	snprintf(data_file, sizeof data_file, "/tmp/kdraw_%ld.bin",
	    (long)getpid());
	fd = open(data_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	f = fdopen(fd, "wb");
	if (f == NULL) {
		close(fd);
		return;
	}

	/* 1. Image 77: compressed, placement 9 and 10. */
	fprintf(f, "\033_Ga=T,i=77,p=9,f=24,o=z,s=2,v=2,x=1,y=2,w=3,h=4,q=1,"
	    "X=5,Y=6,c=7,r=8,z=-3;QUJDREVGR0hJSktM\033\\");
	fprintf(f, "\033_Ga=p,i=77,p=10,c=1,r=1,q=1\033\\");

	/* 2. Image 88: offscreen at col 101 (p=20) and onscreen at col 1 (p=21). */
	fprintf(f,
	    "\033[1;101H\033_Ga=T,i=88,p=20,f=24,s=1,v=1,c=1,r=1,q=1;QUJD\033\\");
	fprintf(f, "\033[1;1H\033_Ga=p,i=88,p=21,c=1,r=1,q=1\033\\");

	/* 3. Anonymous direct image. */
	fprintf(f, "\033_Ga=T,f=24,s=1,v=1,c=1,r=1;QUJD\033\\");

	fclose(f);
}

int
main(void)
{
	const char	*env_tmux;
	char		 setup_cmd[4096];
	char		 out[65536], redraw[65536], out2[65536];
	struct winsize	 ws;
	pid_t		 child1, child2;
	int		 master1, master2;
	size_t		 len;

	env_tmux = getenv("TEST_TMUX");
	if (env_tmux != NULL && *env_tmux != '\0')
		strlcpy(tmux_bin, env_tmux, sizeof tmux_bin);
	else
		strlcpy(tmux_bin, "../tmux", sizeof tmux_bin);

	snprintf(socket_label, sizeof socket_label, "test-kdraw-%ld",
	    (long)getpid());

	if (!check_support()) {
		printf("Kitty graphics not supported by binary, skipping\n");
		return (0);
	}

	/* Clean previous server if any. */
	run_tmux("kill-server");

	write_test_images();

	snprintf(setup_cmd, sizeof setup_cmd,
	    "%s -L%s -f/dev/null start-server \\; "
	    "set-option -g terminal-features 'xterm*:kitty' \\; "
	    "set-option -g window-size manual \\; "
	    "new-session -x 120 -y 24 -d -s %s "
	    "\"cat %s; sleep 30\"",
	    tmux_bin, socket_label, socket_label, data_file);

	if (system(setup_cmd) != 0) {
		fprintf(stderr, "FAIL: could not start tmux session\n");
		unlink(data_file);
		return (1);
	}

	/* Client 1: PTY with 80x24 cells, 800x480 pixels, xterm (has kitty). */
	memset(&ws, 0, sizeof ws);
	ws.ws_row = 24;
	ws.ws_col = 80;
	ws.ws_xpixel = 800;
	ws.ws_ypixel = 480;

	child1 = forkpty(&master1, NULL, NULL, &ws);
	if (child1 < 0) {
		perror("forkpty");
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}

	if (child1 == 0) {
		setenv("TERM", "xterm", 1);
		execl(tmux_bin, tmux_bin, "-L", socket_label, "-f/dev/null",
		    "attach-session", "-t", socket_label, NULL);
		_exit(127);
	}

	/* Read initial draw from Client 1. */
	len = read_pty_until(master1, out, sizeof out, "z=-3", 5.0);
	if (len == 0) {
		fprintf(stderr, "FAIL: did not receive initial draw\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}

	/* Assert expected protocol elements are present for Client 1. */
	if (strstr(out, "QUJDREVGR0hJSktM") == NULL) {
		fprintf(stderr, "FAIL: missing payload data in draw\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}
	if (strstr(out, "z=-3") == NULL || strstr(out, "o=z") == NULL) {
		fprintf(stderr, "FAIL: missing z-index or compression flags\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}
	if (strstr(out, "p=9") == NULL || strstr(out, "p=10") == NULL) {
		fprintf(stderr, "FAIL: missing placement 9 or 10\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}
	if (strstr(out, "p=21") == NULL) {
		fprintf(stderr, "FAIL: missing onscreen placement 21\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}
	if (strstr(out, "p=20") != NULL) {
		fprintf(stderr, "FAIL: offscreen placement 20 was emitted\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}
	if (strstr(out, "\033_Ga=p,i=0") != NULL) {
		fprintf(stderr,
		    "FAIL: unexpected anonymous placement with id zero\n");
		kill(child1, SIGKILL);
		run_tmux("kill-server");
		unlink(data_file);
		return (1);
	}

	/* Redraw Caching: verify redraw does NOT re-upload image bytes. */
	run_tmux("refresh-client");
	len = read_pty_until(master1, redraw, sizeof redraw, NULL, 1.5);
	if (len > 0) {
		if (strstr(redraw, "QUJDREVGR0hJSktM") != NULL) {
			fprintf(stderr,
			    "FAIL: redraw re-uploaded image payload\n");
			kill(child1, SIGKILL);
			run_tmux("kill-server");
			unlink(data_file);
			return (1);
		}
	}

	/*
	 * Multi-Client Isolation Verification:
	 * Attach Client 2 with TERM=dumb (no kitty capability).
	 * Client 2 MUST receive [KITTY IMAGE ...] fallback placeholder,
	 * NOT raw escape sequences.
	 */
	child2 = forkpty(&master2, NULL, NULL, &ws);
	if (child2 >= 0) {
		if (child2 == 0) {
			setenv("TERM", "vt100", 1);
			execl(tmux_bin, tmux_bin, "-L", socket_label,
			    "-f/dev/null", "attach-session", "-t", socket_label,
			    NULL);
			_exit(127);
		}

		len = read_pty_until(master2, out2, sizeof out2, "KITTY IMAGE",
		    3.0);
		if (strstr(out2, "KITTY IMAGE") == NULL) {
			fprintf(stderr,
			    "FAIL: client 2 did not receive fallback placeholder\n");
			kill(child1, SIGKILL);
			kill(child2, SIGKILL);
			run_tmux("kill-server");
			unlink(data_file);
			return (1);
		}

		kill(child2, SIGHUP);
		waitpid(child2, NULL, 0);
		close(master2);
	}

	/* Clean up. */
	kill(child1, SIGHUP);
	waitpid(child1, NULL, 0);
	close(master1);
	run_tmux("kill-server");
	unlink(data_file);

	printf("PASS: kitty_draw_caching_and_multiclient_pty\n");
	return (0);
}
