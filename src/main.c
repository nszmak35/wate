/* main.c - command line, pty, config reload and the event loop */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "wate.h"

#define RELOAD_DEBOUNCE_MS 150
#define OUTQ_MAX (16u << 20)

struct wate W;

long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void usage(FILE *out)
{
	fprintf(out,
"Usage: wate [OPTION]... [COMMAND [ARGUMENT]...]\n"
"       wate [OPTION]... -s SHELL [\"COMMAND LINE\"]\n"
"\n"
"wate " WATE_VERSION " - a small Wayland terminal emulator.\n"
"\n"
"Options:\n"
"  -c FILE     Use FILE (a Python file) as the configuration.  An empty\n"
"              name means: no file, built-in defaults.\n"
"              Default: ~/.config/wate/config.py\n"
"  -s SHELL    Use SHELL (bash, zsh, sh, ...) instead of the configured\n"
"              one.  With a command line after it, run that command in\n"
"              the shell:  wate -s bash \"fastfetch --logo lfs\"\n"
"  -t TITLE    Window title.\n"
"  -a APP_ID   Wayland application id.\n"
"  -d DISPLAY  Wayland display to connect to.\n"
"  -l          Keep the window open after the program has exited.\n"
"  -v          Print the version and exit.\n"
"  -h          Print this help and exit.\n"
"\n"
"Without -s, the arguments after the options are a program and its\n"
"arguments, started directly:  wate htop\n"
"\n"
"The configuration is reloaded in every open window as soon as the file is\n"
"saved.  A file with an error is ignored and the old settings stay.\n");
}

void pty_resize(void)
{
	struct winsize ws;

	if (W.master < 0)
		return;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = (unsigned short)W.rows;
	ws.ws_col = (unsigned short)W.cols;
	ws.ws_xpixel = (unsigned short)(W.cols * W.fm.cell_w);
	ws.ws_ypixel = (unsigned short)(W.rows * W.fm.cell_h);
	if (ioctl(W.master, TIOCSWINSZ, &ws) < 0 && errno != EBADF)
		fprintf(stderr, "wate: cannot resize the pty: %s\n", strerror(errno));
}

static void outq_append(const char *data, size_t len)
{
	if (W.outlen + len > OUTQ_MAX) {
		fprintf(stderr, "wate: too much pending input, dropping it\n");
		return;
	}
	if (W.outlen + len > W.outcap) {
		size_t ncap = W.outcap ? W.outcap : 4096;
		char *nb;

		while (ncap < W.outlen + len)
			ncap *= 2;
		nb = realloc(W.outq, ncap);
		if (!nb)
			return;
		W.outq = nb;
		W.outcap = ncap;
	}
	memcpy(W.outq + W.outlen, data, len);
	W.outlen += len;
}

static void pty_flush(void)
{
	size_t off = 0;

	while (off < W.outlen && W.master >= 0) {
		ssize_t n = write(W.master, W.outq + off, W.outlen - off);

		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		off = W.outlen;
		break;
	}
	if (off > 0) {
		memmove(W.outq, W.outq + off, W.outlen - off);
		W.outlen -= off;
	}
}

void pty_write(const char *data, size_t len)
{
	if (W.master < 0 || len == 0)
		return;
	if (W.outlen > 0) {
		outq_append(data, len);
		pty_flush();
		return;
	}
	outq_append(data, len);
	pty_flush();
}

static void tsm_write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	(void)vte;
	(void)data;
	pty_write(u8, len);
}

static void child_exec(void)
{
	sigset_t none;
	const char *shell, *prog;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);
	setenv("TERM", W.cfg.term, 1);
	setenv("COLORTERM", "truecolor", 1);
	unsetenv("LINES");
	unsetenv("COLUMNS");

	if (W.opt.argv && *W.opt.argv && !W.opt.shell) {
		prog = W.opt.argv[0];
		execvp(prog, W.opt.argv);
	} else {
		shell = W.opt.shell;
		if (!shell || !*shell)
			shell = W.cfg.shell[0] ? W.cfg.shell : getenv("SHELL");
		if (!shell || !*shell)
			shell = "/bin/sh";
		prog = shell;
		if (W.opt.command)
			execlp(shell, shell, "-c", W.opt.command, (char *)NULL);
		else
			execlp(shell, shell, (char *)NULL);
	}
	fprintf(stderr, "wate: cannot run %s: %s\nwate: starting /bin/sh instead\n", prog,
		strerror(errno));
	execl("/bin/sh", "sh", (char *)NULL);
	fprintf(stderr, "wate: cannot run /bin/sh: %s\n", strerror(errno));
	_exit(127);
}

void child_maybe_spawn(void)
{
	struct winsize ws;
	pid_t pid;

	if (W.child_started)
		return;
	W.child_started = true;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = (unsigned short)W.rows;
	ws.ws_col = (unsigned short)W.cols;
	ws.ws_xpixel = (unsigned short)(W.cols * W.fm.cell_w);
	ws.ws_ypixel = (unsigned short)(W.rows * W.fm.cell_h);
	pid = forkpty(&W.master, NULL, NULL, &ws);
	if (pid < 0) {
		fprintf(stderr, "wate: forkpty failed: %s\n", strerror(errno));
		W.master = -1;
		W.die = true;
		W.exit_code = 1;
		return;
	}
	if (pid == 0)
		child_exec();
	W.child = pid;
	fcntl(W.master, F_SETFD, FD_CLOEXEC);
	fcntl(W.master, F_SETFL, fcntl(W.master, F_GETFL) | O_NONBLOCK);
}

static void pty_closed(void)
{
	if (W.master >= 0)
		close(W.master);
	W.master = -1;
	W.outlen = 0;
}

static void pty_read(void)
{
	char buf[16384];
	int loops;

	for (loops = 0; loops < 16 && W.master >= 0; loops++) {
		ssize_t n = read(W.master, buf, sizeof(buf));

		if (n > 0) {
			tsm_vte_input(W.vte, buf, (size_t)n);
			W.need_redraw = true;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		pty_closed();
		return;
	}
}

static void child_reaped(int status)
{
	char msg[96];

	W.child_exited = true;
	W.child_status = status;
	pty_read();
	if (WIFEXITED(status))
		W.exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		W.exit_code = 128 + WTERMSIG(status);
	if (W.opt.hold) {
		snprintf(msg, sizeof(msg), "\r\n[program exited with status %d]\r\n", W.exit_code);
		tsm_vte_input(W.vte, msg, strlen(msg));
		W.need_redraw = true;
		pty_closed();
	} else {
		W.die = true;
	}
}

void new_terminal(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return;
	if (pid == 0) {
		pid_t p2 = fork();

		if (p2 != 0)
			_exit(0);
		{
			sigset_t none;
			char *args[6];
			int n = 0;

			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			signal(SIGPIPE, SIG_DFL);
			setsid();
			args[n++] = "wate";
			if (W.opt.config) {
				args[n++] = "-c";
				args[n++] = (char *)W.opt.config;
			}
			args[n] = NULL;
			execv("/proc/self/exe", args);
			_exit(127);
		}
	}
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

static void apply_config(struct config *n)
{
	struct config old;
	bool font_changed;

	if (config_equal(n, &W.cfg)) {
		config_free(n);
		return;
	}
	old = W.cfg;
	W.cfg = *n;
	memset(n, 0, sizeof(*n));

	font_changed = strcmp(old.font, W.cfg.font) != 0;
	if (font_changed && !render_font_reload()) {
		free(W.cfg.font);
		W.cfg.font = strdup(old.font);
		if (!W.cfg.font)
			W.cfg.font = strdup("monospace:size=11");
	}
	if (old.scrollback != W.cfg.scrollback)
		tsm_screen_set_max_sb(W.screen, W.cfg.scrollback);
	render_palette();
	wl_title_changed();
	if (W.cfg.hide_when_typing == false)
		wl_hide_cursor(false);
	render_layout();
	render_set_opaque();
	W.full_redraw = true;
	W.need_redraw = true;
	config_free(&old);
	mem_trim();
}

static void start_reload(void)
{
	char err[256];

	if (!W.cfg_path)
		return;
	if (W.job) {
		W.reload_again = true;
		return;
	}
	W.job = cfg_job_start(W.cfg_path, err, sizeof(err));
	if (!W.job)
		fprintf(stderr, "wate: cannot reload the configuration: %s\n", err);
}

static void finish_reload(void)
{
	struct config n;
	char *msg = NULL;
	int rc;

	config_defaults(&n);
	rc = cfg_job_finish(W.job, &n, true, &msg);
	W.job = NULL;
	if (rc < 0) {
		fprintf(stderr, "wate: error in %s, not reloaded:\n%s", W.cfg_path,
			msg ? msg : "unknown error\n");
		config_free(&n);
	} else {
		apply_config(&n);
	}
	free(msg);
	if (W.reload_again) {
		W.reload_again = false;
		start_reload();
	}
}

static void watch_add(const char *path)
{
	char dir[PATH_MAX], resolved[PATH_MAX];
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	size_t dlen;
	int i;

	if (W.nwatch >= 2 || !*name)
		return;
	if (!slash) {
		strcpy(dir, ".");
	} else {
		dlen = slash == path ? 1 : (size_t)(slash - path);
		if (dlen >= sizeof(dir))
			return;
		memcpy(dir, path, dlen);
		dir[dlen] = '\0';
	}
	if (!realpath(dir, resolved)) {
		fprintf(stderr, "wate: cannot watch %s: %s\n", dir, strerror(errno));
		return;
	}
	if (inotify_add_watch(W.watch_fd, resolved, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
		fprintf(stderr, "wate: cannot watch %s: %s\n", resolved, strerror(errno));
		return;
	}
	for (i = 0; i < W.nwatch; i++)
		if (!strcmp(W.watch_name[i], name))
			return;
	W.watch_name[W.nwatch] = strdup(name);
	if (W.watch_name[W.nwatch])
		W.nwatch++;
}

static void watch_init(void)
{
	char real[PATH_MAX];

	W.watch_fd = -1;
	if (!W.cfg_path)
		return;
	W.watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (W.watch_fd < 0) {
		fprintf(stderr, "wate: no automatic reload: %s\n", strerror(errno));
		return;
	}
	watch_add(W.cfg_path);
	if (realpath(W.cfg_path, real) && strcmp(real, W.cfg_path))
		watch_add(real);
	if (W.nwatch == 0) {
		close(W.watch_fd);
		W.watch_fd = -1;
	}
}

static void watch_read(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool matched = false;
	ssize_t n;

	while ((n = read(W.watch_fd, buf, sizeof(buf))) > 0) {
		char *p;

		for (p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			int i;

			if ((ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) && ev->len > 0)
				for (i = 0; i < W.nwatch; i++)
					if (!strcmp(ev->name, W.watch_name[i]))
						matched = true;
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
	if (matched)
		W.reload_deadline = now_ms() + RELOAD_DEBOUNCE_MS + (long long)(getpid() % 40);
}

/* process_saver = False: the program goes away with its window */
static void end_child(void)
{
	pid_t fg = W.master >= 0 ? tcgetpgrp(W.master) : -1;
	int i;

	if (fg > 1)
		kill(-fg, SIGHUP);
	kill(W.child, SIGHUP);
	if (W.master >= 0) {
		close(W.master);
		W.master = -1;
	}
	for (i = 0; i < 60; i++) {
		if (waitpid(W.child, NULL, WNOHANG) == W.child)
			return;
		usleep(10000);
	}
	if (fg > 1)
		kill(-fg, SIGKILL);
	kill(W.child, SIGKILL);
	for (i = 0; i < 20; i++) {
		if (waitpid(W.child, NULL, WNOHANG) == W.child)
			return;
		usleep(10000);
	}
}

/* process_saver = True: a background process keeps the pty until the program ends */
static bool keep_child(void)
{
	pid_t pid = fork();
	sigset_t none;
	int null;

	if (pid < 0)
		return false;
	if (pid > 0) {
		close(W.master);
		W.master = -1;
		W.child_exited = true;
		return true;
	}

	setsid();
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		if (null > 2)
			close(null);
	}
	if (W.watch_fd >= 0)
		close(W.watch_fd);
	tsm_vte_unref(W.vte);
	tsm_screen_unref(W.screen);
	W.vte = NULL;
	W.screen = NULL;
	config_free(&W.cfg);
	free(W.outq);
	W.outq = NULL;
	mem_trim();
	for (;;) {
		struct pollfd p = { W.master, POLLIN, 0 };
		bool gone = false;
		char buf[8192];

		if (poll(&p, 1, 1000) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t n = read(W.master, buf, sizeof(buf));

			if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
				gone = true;
		}
		if (waitpid(W.child, NULL, WNOHANG) == W.child)
			break;
		if (gone) {
			close(W.master);
			W.master = -1;
			while (waitpid(W.child, NULL, 0) < 0 && errno == EINTR)
				;
			break;
		}
	}
	_exit(0);
}

static void fatal_wayland(void)
{
	int e = wl_display_get_error(W.display);

	if (e == EPROTO) {
		const struct wl_interface *iface = NULL;
		uint32_t id = 0;
		uint32_t code = wl_display_get_protocol_error(W.display, &iface, &id);

		fprintf(stderr, "wate: Wayland protocol error %u on %s@%u\n", code,
			iface ? iface->name : "?", id);
	} else {
		fprintf(stderr, "wate: lost the Wayland connection: %s\n",
			strerror(e ? e : EPIPE));
	}
	W.die = true;
	W.exit_code = 1;
}

static bool parse_args(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'v' },
		{ "config", required_argument, NULL, 'c' },
		{ "shell", required_argument, NULL, 's' },
		{ "title", required_argument, NULL, 't' },
		{ "app-id", required_argument, NULL, 'a' },
		{ "display", required_argument, NULL, 'd' },
		{ "hold", no_argument, NULL, 'l' },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	opterr = 0;
	while ((c = getopt_long(argc, argv, "+c:s:t:a:d:lhv", longopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			W.opt.config = optarg;
			break;
		case 's':
			W.opt.shell = optarg;
			break;
		case 't':
			W.opt.title = optarg;
			break;
		case 'a':
			W.opt.app_id = optarg;
			break;
		case 'd':
			W.opt.display = optarg;
			break;
		case 'l':
			W.opt.hold = true;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		case 'v':
			printf("wate " WATE_VERSION "\n");
			exit(0);
		default:
			if (optopt && strchr("cstad", optopt))
				fprintf(stderr, "wate: option '-%c' needs an argument\n", optopt);
			else if (optopt)
				fprintf(stderr, "wate: unknown option '-%c'\n", optopt);
			else
				fprintf(stderr, "wate: unknown option '%s'\n", argv[optind - 1]);
			fprintf(stderr, "Try 'wate -h' for help.\n");
			return false;
		}
	}
	if (optind < argc) {
		if (W.opt.shell) {
			static char joined[8192];
			size_t len = 0;
			int i;

			for (i = optind; i < argc; i++) {
				size_t l = strlen(argv[i]);

				if (len + l + 2 >= sizeof(joined)) {
					fprintf(stderr, "wate: the command is too long\n");
					return false;
				}
				if (i > optind)
					joined[len++] = ' ';
				memcpy(joined + len, argv[i], l);
				len += l;
			}
			joined[len] = '\0';
			W.opt.command = joined;
		} else {
			W.opt.argv = &argv[optind];
		}
	}
	return true;
}

static void load_initial_config(void)
{
	char *msg = NULL;

	config_defaults(&W.cfg);
	if (W.opt.config) {
		if (!*W.opt.config)
			return;
		W.cfg_path = strdup(W.opt.config);
	} else {
		W.cfg_path = config_default_path();
		if (W.cfg_path)
			config_seed(W.cfg_path);
	}
	if (!W.cfg_path)
		return;
	if (config_load(W.cfg_path, &W.cfg, false, &msg) < 0 || msg)
		fprintf(stderr, "wate: problem in %s:\n%s", W.cfg_path,
			msg ? msg : "unknown error\n");
	free(msg);
}

int main(int argc, char **argv)
{
	sigset_t mask;
	int sfd;
	char err[256];
	int i;

	W.master = -1;
	W.watch_fd = -1;
	W.scale = 120;
	W.paste.fd = -1;
	for (i = 0; i < MAX_SENDS; i++)
		W.sends[i].fd = -1;
	W.repeat.delay_ms = 400;
	W.repeat.interval_ms = 40;

	if (!parse_args(argc, argv))
		return 2;

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);
	sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0) {
		fprintf(stderr, "wate: signalfd: %s\n", strerror(errno));
		return 1;
	}

	load_initial_config();

	if (tsm_screen_new(&W.screen) < 0 ||
	    tsm_vte_new(&W.vte, W.screen, tsm_write_cb, NULL) < 0) {
		fprintf(stderr, "wate: cannot create the terminal\n");
		return 1;
	}
	tsm_screen_set_max_sb(W.screen, W.cfg.scrollback);
	render_palette();

	if (!render_font_reload()) {
		free(W.cfg.font);
		W.cfg.font = strdup("monospace:size=11");
		if (!W.cfg.font || !render_font_reload()) {
			fprintf(stderr, "wate: no usable font found\n");
			return 1;
		}
	}

	if (!wl_setup(err, sizeof(err))) {
		fprintf(stderr, "wate: %s\n", err);
		wl_teardown();
		return 1;
	}
	watch_init();

	while (!W.die) {
		struct pollfd fds[8 + MAX_SENDS + 1];
		int nfds = 0, i_wl, i_pty, i_sig, i_watch = -1, i_job = -1, i_clip, nclip;
		long long now, deadline = -1, d;
		int timeout, fr;
		bool want_out;

		now = now_ms();
		if (W.reload_deadline && now >= W.reload_deadline) {
			W.reload_deadline = 0;
			start_reload();
		}
		if (W.job && cfg_job_expired(W.job)) {
			cfg_job_kill(W.job);
			W.job = NULL;
			fprintf(stderr, "wate: reading the configuration took too long, not reloaded\n");
			if (W.reload_again) {
				W.reload_again = false;
				start_reload();
			}
		}
		wl_repeat_tick();
		render_trim();
		if (W.need_redraw && render_can_draw())
			render_redraw(false);
		if (W.die)
			break;

		while (wl_display_prepare_read(W.display) != 0) {
			if (wl_display_dispatch_pending(W.display) < 0) {
				fatal_wayland();
				break;
			}
		}
		if (W.die) {
			wl_display_cancel_read(W.display);
			break;
		}
		want_out = false;
		fr = wl_display_flush(W.display);
		if (fr < 0) {
			if (errno == EAGAIN) {
				want_out = true;
			} else {
				wl_display_cancel_read(W.display);
				fatal_wayland();
				break;
			}
		}

		i_wl = nfds;
		fds[nfds].fd = wl_display_get_fd(W.display);
		fds[nfds].events = POLLIN | (want_out ? POLLOUT : 0);
		fds[nfds++].revents = 0;
		i_pty = nfds;
		fds[nfds].fd = W.master;
		fds[nfds].events = POLLIN | (W.outlen ? POLLOUT : 0);
		fds[nfds++].revents = 0;
		i_sig = nfds;
		fds[nfds].fd = sfd;
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		if (W.watch_fd >= 0) {
			i_watch = nfds;
			fds[nfds].fd = W.watch_fd;
			fds[nfds].events = POLLIN;
			fds[nfds++].revents = 0;
		}
		if (W.job) {
			i_job = nfds;
			fds[nfds].fd = cfg_job_fd(W.job);
			fds[nfds].events = POLLIN;
			fds[nfds++].revents = 0;
		}
		i_clip = nfds;
		nclip = wl_clipboard_fds(&fds[nfds], (int)(sizeof(fds) / sizeof(fds[0])) - nfds);
		nfds += nclip;

		if (W.repeat.active)
			deadline = W.repeat.deadline;
		if (W.reload_deadline && (deadline < 0 || W.reload_deadline < deadline))
			deadline = W.reload_deadline;
		d = render_deadline();
		if (d >= 0 && (deadline < 0 || d < deadline))
			deadline = d;
		if (W.paste.fd >= 0 || W.job) {
			d = now_ms() + 250;
			if (deadline < 0 || d < deadline)
				deadline = d;
		}
		if (deadline < 0) {
			timeout = -1;
		} else {
			long long t = deadline - now_ms();

			timeout = t < 0 ? 0 : t > 60000 ? 60000 : (int)t;
		}

		if (poll(fds, (nfds_t)nfds, timeout) < 0) {
			wl_display_cancel_read(W.display);
			if (errno == EINTR)
				continue;
			fprintf(stderr, "wate: poll: %s\n", strerror(errno));
			W.die = true;
			W.exit_code = 1;
			break;
		}

		if (fds[i_wl].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (wl_display_read_events(W.display) < 0) {
				fatal_wayland();
				break;
			}
		} else {
			wl_display_cancel_read(W.display);
		}
		if (wl_display_dispatch_pending(W.display) < 0) {
			fatal_wayland();
			break;
		}

		if (fds[i_pty].revents & POLLOUT)
			pty_flush();
		if (fds[i_pty].revents & (POLLIN | POLLHUP | POLLERR)) {
			pty_read();
			wl_title_check();
		}
		if (fds[i_sig].revents & POLLIN) {
			struct signalfd_siginfo si;

			while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
				if (si.ssi_signo == SIGCHLD) {
					int status;

					if (W.child > 0 && !W.child_exited &&
					    waitpid(W.child, &status, WNOHANG) == W.child)
						child_reaped(status);
				} else {
					W.die = true;
				}
			}
		}
		if (i_watch >= 0 && (fds[i_watch].revents & POLLIN))
			watch_read();
		if (i_job >= 0 && W.job && (fds[i_job].revents & (POLLIN | POLLHUP | POLLERR))) {
			if (cfg_job_read(W.job))
				finish_reload();
		}
		wl_clipboard_dispatch(&fds[i_clip], nclip);
	}

	if (W.job)
		cfg_job_kill(W.job);
	wl_teardown();
	if (W.child > 0 && !W.child_exited) {
		if (!(W.cfg.process_saver && W.master >= 0 && keep_child()))
			end_child();
	}
	if (W.master >= 0)
		close(W.master);
	tsm_vte_unref(W.vte);
	tsm_screen_unref(W.screen);
	if (W.watch_fd >= 0)
		close(W.watch_fd);
	for (i = 0; i < W.nwatch; i++)
		free(W.watch_name[i]);
	free(W.outq);
	free(W.cfg_path);
	config_free(&W.cfg);
	close(sfd);
	return W.exit_code;
}

