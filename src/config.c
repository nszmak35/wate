/* config.c - config helper process and parsing */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "default_config.h"
#include "helper_py.h"

#define HELPER_TIMEOUT_MS 5000
#define MAX_OUTPUT (512 * 1024)

static const uint32_t default_palette[16] = {
	0x3a3f30, 0xb3766a, 0x7fa67c, 0xb8ad88,
	0x7f98b8, 0x9e90ae, 0x78a6a3, 0xc3c8b6,
	0x7a8168, 0xc98b7e, 0x9cc49a, 0xcbc29f,
	0xa2b8d6, 0xb5a8c4, 0x93c0bc, 0xeef0e6,
};

static char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");

	return p;
}

static void add_key(struct config *c, unsigned mods, const char *key,
		    enum key_action action, int arg)
{
	size_t i;
	struct key_binding *n;

	for (i = 0; i < c->nkeys; i++) {
		if (c->keys[i].mods == mods && strcasecmp(c->keys[i].key, key) == 0) {
			c->keys[i].action = action;
			c->keys[i].arg = arg;
			return;
		}
	}
	n = realloc(c->keys, (c->nkeys + 1) * sizeof(*n));
	if (!n)
		return;
	c->keys = n;
	n[c->nkeys].mods = mods;
	n[c->nkeys].key = xstrdup(key);
	n[c->nkeys].action = action;
	n[c->nkeys].arg = arg;
	if (n[c->nkeys].key)
		c->nkeys++;
}

static void add_button(struct config *c, unsigned mods, enum mouse_btn button,
		       enum key_action action, int arg)
{
	size_t i;
	struct button_binding *n;

	for (i = 0; i < c->nbuttons; i++) {
		if (c->buttons[i].mods == mods && c->buttons[i].button == button) {
			c->buttons[i].action = action;
			c->buttons[i].arg = arg;
			return;
		}
	}
	n = realloc(c->buttons, (c->nbuttons + 1) * sizeof(*n));
	if (!n)
		return;
	c->buttons = n;
	n[c->nbuttons].mods = mods;
	n[c->nbuttons].button = button;
	n[c->nbuttons].action = action;
	n[c->nbuttons].arg = arg;
	c->nbuttons++;
}

void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	c->font = xstrdup("Libertinus Mono:size=11");
	c->shell = xstrdup("");
	c->term = xstrdup("xterm-256color");
	c->title = xstrdup("wate");
	c->app_id = xstrdup("wate");
	c->cols = 80;
	c->rows = 24;
	c->pad_x = 12;
	c->pad_y = 12;
	c->scrollback = 1000;
	c->alpha = 1.0;
	c->hide_when_typing = true;
	c->process_saver = false;
	c->cursor_style = CURSOR_BLOCK;
	c->fg = 0xe2e5da;
	c->bg = 0x1c1e17;
	c->has_sel_fg = true;
	c->sel_fg = 0xeef0e6;
	c->has_sel_bg = true;
	c->sel_bg = 0x3a3f30;
	c->urls = 0xb8ad88;
	memcpy(c->palette, default_palette, sizeof(c->palette));

	add_key(c, MOD_CTRL | MOD_SHIFT, "c", ACT_COPY, 0);
	add_key(c, MOD_CTRL | MOD_SHIFT, "v", ACT_PASTE, 0);
	add_key(c, MOD_SHIFT, "Insert", ACT_PASTE_PRIMARY, 0);
	add_key(c, MOD_CTRL, "equal", ACT_FONTSIZE, 1);
	add_key(c, MOD_CTRL, "plus", ACT_FONTSIZE, 1);
	add_key(c, MOD_CTRL, "KP_Add", ACT_FONTSIZE, 1);
	add_key(c, MOD_CTRL, "minus", ACT_FONTSIZE, -1);
	add_key(c, MOD_CTRL, "KP_Subtract", ACT_FONTSIZE, -1);
	add_key(c, MOD_CTRL, "0", ACT_FONTSIZE_RESET, 0);
	add_key(c, MOD_SHIFT, "Page_Up", ACT_SCROLL_PAGE_UP, 0);
	add_key(c, MOD_SHIFT, "Page_Down", ACT_SCROLL_PAGE_DOWN, 0);
	add_key(c, MOD_CTRL | MOD_SHIFT, "n", ACT_NEW_TERMINAL, 0);

	add_button(c, MOD_CTRL, MB_WHEEL_UP, ACT_FONTSIZE, 1);
	add_button(c, MOD_CTRL, MB_WHEEL_DOWN, ACT_FONTSIZE, -1);
}

void config_free(struct config *c)
{
	size_t i;

	free(c->font);
	free(c->shell);
	free(c->term);
	free(c->title);
	free(c->app_id);
	for (i = 0; i < c->nkeys; i++)
		free(c->keys[i].key);
	free(c->keys);
	free(c->buttons);
	memset(c, 0, sizeof(*c));
}

void config_copy(struct config *dst, const struct config *src)
{
	size_t i;

	*dst = *src;
	dst->font = xstrdup(src->font);
	dst->shell = xstrdup(src->shell);
	dst->term = xstrdup(src->term);
	dst->title = xstrdup(src->title);
	dst->app_id = xstrdup(src->app_id);
	dst->keys = NULL;
	dst->nkeys = 0;
	dst->buttons = NULL;
	dst->nbuttons = 0;
	if (src->nbuttons) {
		dst->buttons = calloc(src->nbuttons, sizeof(*dst->buttons));
		if (dst->buttons) {
			memcpy(dst->buttons, src->buttons, src->nbuttons * sizeof(*dst->buttons));
			dst->nbuttons = src->nbuttons;
		}
	}
	if (src->nkeys) {
		dst->keys = calloc(src->nkeys, sizeof(*dst->keys));
		if (dst->keys) {
			for (i = 0; i < src->nkeys; i++) {
				dst->keys[i] = src->keys[i];
				dst->keys[i].key = xstrdup(src->keys[i].key);
			}
			dst->nkeys = src->nkeys;
		}
	}
}

static bool streq(const char *a, const char *b)
{
	return strcmp(a ? a : "", b ? b : "") == 0;
}

bool config_equal(const struct config *a, const struct config *b)
{
	size_t i;

	if (!streq(a->font, b->font) || !streq(a->shell, b->shell) ||
	    !streq(a->term, b->term) || !streq(a->title, b->title) ||
	    !streq(a->app_id, b->app_id))
		return false;
	if (a->pad_x != b->pad_x || a->pad_y != b->pad_y || a->cols != b->cols ||
	    a->rows != b->rows || a->scrollback != b->scrollback ||
	    a->alpha != b->alpha || a->hide_when_typing != b->hide_when_typing ||
	    a->process_saver != b->process_saver ||
	    a->cursor_style != b->cursor_style ||
	    a->has_cursor_color != b->has_cursor_color ||
	    a->cursor_color != b->cursor_color || a->fg != b->fg || a->bg != b->bg ||
	    a->has_sel_fg != b->has_sel_fg || a->has_sel_bg != b->has_sel_bg ||
	    a->sel_fg != b->sel_fg || a->sel_bg != b->sel_bg || a->urls != b->urls ||
	    a->nkeys != b->nkeys || a->nbuttons != b->nbuttons)
		return false;
	if (memcmp(a->palette, b->palette, sizeof(a->palette)) != 0)
		return false;
	for (i = 0; i < a->nkeys; i++) {
		if (a->keys[i].mods != b->keys[i].mods ||
		    a->keys[i].action != b->keys[i].action ||
		    a->keys[i].arg != b->keys[i].arg ||
		    !streq(a->keys[i].key, b->keys[i].key))
			return false;
	}
	for (i = 0; i < a->nbuttons; i++) {
		if (a->buttons[i].mods != b->buttons[i].mods ||
		    a->buttons[i].button != b->buttons[i].button ||
		    a->buttons[i].action != b->buttons[i].action ||
		    a->buttons[i].arg != b->buttons[i].arg)
			return false;
	}
	return true;
}

char *config_default_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *path;
	size_t n;

	if (xdg && *xdg == '/') {
		n = strlen(xdg) + sizeof("/wate/config.py");
		path = malloc(n);
		if (path)
			snprintf(path, n, "%s/wate/config.py", xdg);
		return path;
	}
	if (home && *home) {
		n = strlen(home) + sizeof("/.config/wate/config.py");
		path = malloc(n);
		if (path)
			snprintf(path, n, "%s/.config/wate/config.py", home);
		return path;
	}
	return NULL;
}

void config_seed(const char *path)
{
	char dir[4096];
	char *slash;
	int fd;
	size_t len = strlen(path);

	if (access(path, F_OK) == 0 || len >= sizeof(dir))
		return;
	memcpy(dir, path, len + 1);
	slash = strrchr(dir, '/');
	if (!slash || slash == dir)
		return;
	*slash = '\0';
	{
		char *p = strrchr(dir, '/');

		if (p && p != dir) {
			*p = '\0';
			(void)mkdir(dir, 0755);
			*p = '/';
		}
	}
	(void)mkdir(dir, 0755);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, wate_default_config, sizeof(wate_default_config) - 1) < 0) {
	}
	close(fd);
}

struct cfg_job {
	pid_t pid;
	int fd;
	char *buf;
	size_t len, cap;
	long long deadline;
	bool eof;
	bool overflow;
	char path[4096];
};

static long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void put_err(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (!err || !n)
		return;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

struct cfg_job *cfg_job_start(const char *path, char *err, size_t errlen)
{
	struct cfg_job *j;
	int p[2];
	sigset_t none;

	if (!path || strlen(path) >= sizeof(j->path)) {
		put_err(err, errlen, "bad configuration path");
		return NULL;
	}
	j = calloc(1, sizeof(*j));
	if (!j) {
		put_err(err, errlen, "out of memory");
		return NULL;
	}
	strcpy(j->path, path);
	j->fd = -1;
	if (pipe2(p, O_CLOEXEC) < 0) {
		put_err(err, errlen, "pipe: %s", strerror(errno));
		free(j);
		return NULL;
	}
	j->pid = fork();
	if (j->pid < 0) {
		put_err(err, errlen, "fork: %s", strerror(errno));
		close(p[0]);
		close(p[1]);
		free(j);
		return NULL;
	}
	if (j->pid == 0) {
		int null = open("/dev/null", O_RDONLY);

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (null >= 0)
			dup2(null, 0);
		dup2(p[1], 1);
		execlp("python3", "python3", "-B", "-c", helper_script, path, (char *)NULL);
		execlp("python", "python", "-B", "-c", helper_script, path, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	j->fd = p[0];
	fcntl(j->fd, F_SETFL, fcntl(j->fd, F_GETFL) | O_NONBLOCK);
	j->deadline = now_ms() + HELPER_TIMEOUT_MS;
	return j;
}

int cfg_job_fd(const struct cfg_job *j)
{
	return j->fd;
}

int cfg_job_read(struct cfg_job *j)
{
	for (;;) {
		ssize_t n;

		if (j->len + 4096 + 1 > j->cap) {
			size_t ncap = j->cap ? j->cap * 2 : 8192;
			char *nb;

			if (ncap > MAX_OUTPUT * 2) {
				j->overflow = true;
				j->eof = true;
				return 1;
			}
			nb = realloc(j->buf, ncap);
			if (!nb) {
				j->overflow = true;
				j->eof = true;
				return 1;
			}
			j->buf = nb;
			j->cap = ncap;
		}
		n = read(j->fd, j->buf + j->len, j->cap - j->len - 1);
		if (n > 0) {
			j->len += (size_t)n;
			if (j->len > MAX_OUTPUT) {
				j->overflow = true;
				j->eof = true;
				return 1;
			}
			continue;
		}
		if (n == 0) {
			j->eof = true;
			return 1;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		j->eof = true;
		return 1;
	}
}

bool cfg_job_expired(const struct cfg_job *j)
{
	return now_ms() > j->deadline;
}

static int reap(struct cfg_job *j, bool kill_it)
{
	int status = 0;

	if (j->pid > 0) {
		if (!kill_it) {
			int i;

			for (i = 0; i < 50; i++) {
				pid_t r = waitpid(j->pid, &status, WNOHANG);

				if (r == j->pid || (r < 0 && errno != EINTR)) {
					j->pid = 0;
					return status;
				}
				usleep(10000);
			}
		}
		kill(j->pid, SIGKILL);
		while (waitpid(j->pid, &status, 0) < 0 && errno == EINTR)
			;
		j->pid = 0;
	}
	return status;
}

static void job_free(struct cfg_job *j)
{
	if (j->fd >= 0)
		close(j->fd);
	free(j->buf);
	free(j);
}

void cfg_job_kill(struct cfg_job *j)
{
	if (!j)
		return;
	reap(j, true);
	job_free(j);
}

struct errlist {
	char *s;
	size_t len;
	int count;
};

static void add_error(struct errlist *e, const char *fmt, ...)
{
	char line[600];
	va_list ap;
	size_t n;
	char *p;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	e->count++;
	n = strlen(line);
	line[n++] = '\n';
	line[n] = '\0';
	p = realloc(e->s, e->len + n + 1);
	if (!p)
		return;
	e->s = p;
	memcpy(e->s + e->len, line, n + 1);
	e->len += n;
}

static void unescape(char *s)
{
	char *w = s;

	while (*s) {
		if (*s == '\\' && s[1]) {
			s++;
			*w++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
			s++;
		} else {
			*w++ = *s++;
		}
	}
	*w = '\0';
}

static bool parse_color(const char *s, uint32_t *rgb, int *alpha)
{
	size_t len, i;
	unsigned long v;
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	else if (s[0] == '#')
		s++;
	len = strlen(s);
	while (len && isspace((unsigned char)s[len - 1]))
		len--;
	if (len != 6 && len != 8)
		return false;
	for (i = 0; i < len; i++)
		if (!isxdigit((unsigned char)s[i]))
			return false;
	{
		char tmp[9];

		memcpy(tmp, s, len);
		tmp[len] = '\0';
		v = strtoul(tmp, &end, 16);
		if (*end)
			return false;
	}
	if (len == 8) {
		*rgb = (uint32_t)(v >> 8) & 0xffffff;
		if (alpha)
			*alpha = (int)(v & 0xff);
	} else {
		*rgb = (uint32_t)v & 0xffffff;
		if (alpha)
			*alpha = 255;
	}
	return true;
}

static bool parse_pair(const char *s, int min, int max, int *x, int *y)
{
	char *end;
	long a, b;

	errno = 0;
	a = strtol(s, &end, 10);
	if (errno || end == s)
		return false;
	if (*end == 'x' || *end == 'X') {
		const char *q = end + 1;

		b = strtol(q, &end, 10);
		if (errno || end == q || *end)
			return false;
	} else if (*end == '\0') {
		b = a;
	} else {
		return false;
	}
	if (a < min || a > max || b < min || b > max)
		return false;
	*x = (int)a;
	*y = (int)b;
	return true;
}

static bool set_string(char **dst, const char *v)
{
	char *n = strdup(v);

	if (!n)
		return false;
	free(*dst);
	*dst = n;
	return true;
}

struct option {
	const char *name;
	const char *types;
	bool (*set)(struct config *c, const char *v, int idx, char *why, size_t n);
	int index;
};

#define WHY(...) snprintf(why, n, __VA_ARGS__)

static bool o_font(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	if (!*v) {
		WHY("must not be empty");
		return false;
	}
	return set_string(&c->font, v);
}

static bool o_pad(struct config *c, const char *v, int idx, char *why, size_t n)
{
	int x, y;

	(void)idx;
	if (!parse_pair(v, 0, 1000, &x, &y)) {
		WHY("expected \"XxY\" or \"N\" (0..1000), got \"%s\"", v);
		return false;
	}
	c->pad_x = x;
	c->pad_y = y;
	return true;
}

static bool o_size(struct config *c, const char *v, int idx, char *why, size_t n)
{
	int x, y;

	(void)idx;
	if (!parse_pair(v, 1, 1000, &x, &y)) {
		WHY("expected \"COLSxROWS\" (1..1000), got \"%s\"", v);
		return false;
	}
	c->cols = x;
	c->rows = y;
	return true;
}

static bool o_scrollback(struct config *c, const char *v, int idx, char *why, size_t n)
{
	char *end;
	long x = strtol(v, &end, 10);

	(void)idx;
	if (*end || x < 0 || x > 1000000) {
		WHY("expected an integer 0..1000000, got \"%s\"", v);
		return false;
	}
	c->scrollback = (int)x;
	return true;
}

static bool o_shell(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	if (strchr(v, '\n')) {
		WHY("must not contain a newline");
		return false;
	}
	return set_string(&c->shell, v);
}

static bool o_term(struct config *c, const char *v, int idx, char *why, size_t n)
{
	size_t i;

	(void)idx;
	if (!*v) {
		WHY("must not be empty");
		return false;
	}
	for (i = 0; v[i]; i++) {
		if (!isalnum((unsigned char)v[i]) && !strchr("-_.+", v[i])) {
			WHY("invalid character in \"%s\"", v);
			return false;
		}
	}
	return set_string(&c->term, v);
}

static bool o_title(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	(void)why;
	(void)n;
	return set_string(&c->title, v);
}

static bool o_app_id(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	if (!*v || strchr(v, '\n')) {
		WHY("must be a non-empty single line");
		return false;
	}
	return set_string(&c->app_id, v);
}

static bool o_alpha(struct config *c, const char *v, int idx, char *why, size_t n)
{
	char *end;
	double x = strtod(v, &end);

	(void)idx;
	if (*end || !isfinite(x) || x < 0.0 || x > 1.0) {
		WHY("expected a number 0.0 .. 1.0, got \"%s\"", v);
		return false;
	}
	c->alpha = x;
	return true;
}

static bool o_hide(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	(void)why;
	(void)n;
	c->hide_when_typing = v[0] == '1';
	return true;
}

static bool o_saver(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	(void)why;
	(void)n;
	c->process_saver = v[0] == '1';
	return true;
}

static bool o_cstyle(struct config *c, const char *v, int idx, char *why, size_t n)
{
	(void)idx;
	if (!strcmp(v, "block"))
		c->cursor_style = CURSOR_BLOCK;
	else if (!strcmp(v, "beam") || !strcmp(v, "bar"))
		c->cursor_style = CURSOR_BEAM;
	else if (!strcmp(v, "underline"))
		c->cursor_style = CURSOR_UNDERLINE;
	else {
		WHY("expected block, beam or underline, got \"%s\"", v);
		return false;
	}
	return true;
}

static bool o_ccolor(struct config *c, const char *v, int idx, char *why, size_t n)
{
	uint32_t rgb;

	(void)idx;
	if (!*v) {
		c->has_cursor_color = false;
		return true;
	}
	if (!parse_color(v, &rgb, NULL)) {
		WHY("expected \"0xRRGGBB[AA]\", got \"%s\"", v);
		return false;
	}
	c->has_cursor_color = true;
	c->cursor_color = rgb;
	return true;
}

static bool o_color(struct config *c, const char *v, int idx, char *why, size_t n);

static const struct option options[] = {
	{ "font", "s", o_font, 0 },
	{ "pad", "si", o_pad, 0 },
	{ "size", "s", o_size, 0 },
	{ "scrollback", "i", o_scrollback, 0 },
	{ "shell", "s", o_shell, 0 },
	{ "term", "s", o_term, 0 },
	{ "title", "s", o_title, 0 },
	{ "app_id", "s", o_app_id, 0 },
	{ "alpha", "fi", o_alpha, 0 },
	{ "process_saver", "b", o_saver, 0 },
	{ "Mouse.hide_when_typing", "b", o_hide, 0 },
	{ "Cursor.style", "s", o_cstyle, 0 },
	{ "Cursor.color", "s", o_ccolor, 0 },
	{ "Colors.fg", "s", o_color, 0 },
	{ "Colors.bg", "s", o_color, 1 },
	{ "Selection_Links_colors.selection_fg", "s", o_color, 2 },
	{ "Selection_Links_colors.selection_bg", "s", o_color, 3 },
	{ "Selection_Links_colors.urls", "s", o_color, 4 },
	{ "Ansi_Colors.regular0", "s", o_color, 16 },
	{ "Ansi_Colors.regular1", "s", o_color, 17 },
	{ "Ansi_Colors.regular2", "s", o_color, 18 },
	{ "Ansi_Colors.regular3", "s", o_color, 19 },
	{ "Ansi_Colors.regular4", "s", o_color, 20 },
	{ "Ansi_Colors.regular5", "s", o_color, 21 },
	{ "Ansi_Colors.regular6", "s", o_color, 22 },
	{ "Ansi_Colors.regular7", "s", o_color, 23 },
	{ "Ansi_Colors.bright0", "s", o_color, 24 },
	{ "Ansi_Colors.bright1", "s", o_color, 25 },
	{ "Ansi_Colors.bright2", "s", o_color, 26 },
	{ "Ansi_Colors.bright3", "s", o_color, 27 },
	{ "Ansi_Colors.bright4", "s", o_color, 28 },
	{ "Ansi_Colors.bright5", "s", o_color, 29 },
	{ "Ansi_Colors.bright6", "s", o_color, 30 },
	{ "Ansi_Colors.bright7", "s", o_color, 31 },
};

static const struct option *find_option(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(options) / sizeof(options[0]); i++)
		if (strcmp(options[i].name, name) == 0)
			return &options[i];
	return NULL;
}

static bool o_color(struct config *c, const char *v, int idx, char *why, size_t n)
{
	uint32_t rgb;
	int alpha = 255;

	if (!parse_color(v, &rgb, &alpha)) {
		WHY("expected \"0xRRGGBB[AA]\", got \"%s\"", v);
		return false;
	}
	switch (idx) {
	case 0:
		c->fg = rgb;
		break;
	case 1:
		c->bg = rgb;
		break;
	case 2:
		c->sel_fg = rgb;
		c->has_sel_fg = true;
		break;
	case 3:
		c->sel_bg = rgb;
		c->has_sel_bg = true;
		break;
	case 4:
		c->urls = rgb;
		break;
	default:
		c->palette[idx - 16] = rgb;
		break;
	}
	return true;
}

static const struct {
	const char *name;
	enum key_action action;
	bool takes_arg;
} actions[] = {
	{ "copy", ACT_COPY, false },
	{ "paste", ACT_PASTE, false },
	{ "paste_primary", ACT_PASTE_PRIMARY, false },
	{ "fontsize", ACT_FONTSIZE, true },
	{ "fontsize_reset", ACT_FONTSIZE_RESET, false },
	{ "scroll_up", ACT_SCROLL_UP, true },
	{ "scroll_down", ACT_SCROLL_DOWN, true },
	{ "scroll_page_up", ACT_SCROLL_PAGE_UP, false },
	{ "scroll_page_down", ACT_SCROLL_PAGE_DOWN, false },
	{ "scroll_top", ACT_SCROLL_TOP, false },
	{ "scroll_bottom", ACT_SCROLL_BOTTOM, false },
	{ "reset", ACT_RESET, false },
	{ "new_terminal", ACT_NEW_TERMINAL, false },
	{ "none", ACT_NONE, false },
};

static bool parse_mods(const char *s, unsigned *out)
{
	char tmp[128];
	char *tok, *save = NULL;
	unsigned m = 0;

	if (strlen(s) >= sizeof(tmp))
		return false;
	strcpy(tmp, s);
	for (tok = strtok_r(tmp, "+| ,", &save); tok; tok = strtok_r(NULL, "+| ,", &save)) {
		if (!strcasecmp(tok, "SHIFT"))
			m |= MOD_SHIFT;
		else if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL"))
			m |= MOD_CTRL;
		else if (!strcasecmp(tok, "ALT"))
			m |= MOD_ALT;
		else if (!strcasecmp(tok, "SUPER") || !strcasecmp(tok, "LOGO") ||
			 !strcasecmp(tok, "META"))
			m |= MOD_SUPER;
		else if (!strcasecmp(tok, "NONE"))
			;
		else
			return false;
	}
	*out = m;
	return true;
}

static bool parse_action(const char *what, char **f, enum key_action *act, int *argout,
			 struct errlist *e)
{
	size_t i;
	long a;
	char *end;

	for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
		if (!strcmp(f[2], actions[i].name))
			break;
	if (i == sizeof(actions) / sizeof(actions[0])) {
		add_error(e, "%s: unknown action \"%s\"", what, f[2]);
		return false;
	}
	*argout = 0;
	if (actions[i].takes_arg) {
		errno = 0;
		a = strtol(f[3], &end, 10);
		if (!*f[3] || *end || errno || a < -1000 || a > 1000) {
			add_error(e, "%s: action \"%s\" needs an integer argument, got \"%s\"",
				  what, f[2], f[3]);
			return false;
		}
		*argout = (int)a;
	} else if (*f[3]) {
		add_error(e, "%s: action \"%s\" takes no argument", what, f[2]);
		return false;
	}
	*act = actions[i].action;
	return true;
}

static void parse_key(struct config *c, char **f, int nf, struct errlist *e)
{
	unsigned mods;
	int arg;
	enum key_action act;

	if (nf < 4)
		return;
	if (!parse_mods(f[0], &mods)) {
		add_error(e, "keys: unknown modifier in \"%s\"", f[0]);
		return;
	}
	if (!*f[1] || strlen(f[1]) > 64) {
		add_error(e, "keys: bad key name \"%s\"", f[1]);
		return;
	}
	if (!parse_action("keys", f, &act, &arg, e))
		return;
	add_key(c, mods, f[1], act, arg);
}

static const struct {
	const char *name;
	enum mouse_btn button;
} button_names[] = {
	{ "left", MB_LEFT },
	{ "middle", MB_MIDDLE },
	{ "right", MB_RIGHT },
	{ "back", MB_BACK },
	{ "forward", MB_FORWARD },
	{ "wheel_up", MB_WHEEL_UP },
	{ "wheel_down", MB_WHEEL_DOWN },
};

static void parse_button(struct config *c, char **f, int nf, struct errlist *e)
{
	unsigned mods;
	int arg;
	size_t i;
	enum key_action act;

	if (nf < 4)
		return;
	if (!parse_mods(f[0], &mods)) {
		add_error(e, "buttons: unknown modifier in \"%s\"", f[0]);
		return;
	}
	for (i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++)
		if (!strcasecmp(f[1], button_names[i].name))
			break;
	if (i == sizeof(button_names) / sizeof(button_names[0])) {
		add_error(e, "buttons: unknown button \"%s\"", f[1]);
		return;
	}
	if (!parse_action("buttons", f, &act, &arg, e))
		return;
	add_button(c, mods, button_names[i].button, act, arg);
}

static int parse_output(char *buf, size_t len, struct config *c, struct errlist *e)
{
	char *line, *save = NULL;

	buf[len] = '\0';
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[6];
		int nf = 0;
		char *p = line;

		f[nf++] = p;
		while (nf < 6 && (p = strchr(p, '\t'))) {
			*p++ = '\0';
			f[nf++] = p;
		}
		{
			int i;

			for (i = 0; i < nf; i++)
				unescape(f[i]);
		}
		if (!strcmp(f[0], "E") && nf >= 2) {
			add_error(e, "%s", f[1]);
		} else if (!strcmp(f[0], "V") && nf >= 4) {
			const struct option *o = find_option(f[2]);
			char why[300];

			if (!o) {
				fprintf(stderr, "wate: config: ignoring unknown setting \"%s\"\n", f[2]);
				continue;
			}
			if (!f[1][0] || !strchr(o->types, f[1][0])) {
				add_error(e, "%s: wrong type (expected %s)", f[2],
					  o->types[0] == 's' ? "a string" :
					  o->types[0] == 'b' ? "True/False" :
					  o->types[0] == 'i' ? "an integer" : "a number");
				continue;
			}
			why[0] = '\0';
			if (!o->set(c, f[3], o->index, why, sizeof(why)))
				add_error(e, "%s: %s", f[2], why[0] ? why : "invalid value");
		} else if (!strcmp(f[0], "K")) {
			parse_key(c, f + 1, nf - 1, e);
		} else if (!strcmp(f[0], "B")) {
			parse_button(c, f + 1, nf - 1, e);
		}
	}
	return e->count;
}

int cfg_job_finish(struct cfg_job *j, struct config *out, bool strict, char **msg)
{
	struct errlist e = { NULL, 0, 0 };
	struct config work;
	int status, rc = 0;

	if (msg)
		*msg = NULL;
	status = reap(j, !j->eof);
	if (j->overflow) {
		add_error(&e, "configuration output is too large");
	} else if (j->len == 0) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
			add_error(&e, "cannot run python3 (is it installed?)");
		else if (j->deadline < now_ms() && !j->eof)
			add_error(&e, "the configuration did not finish within %d seconds",
				  HELPER_TIMEOUT_MS / 1000);
		else
			add_error(&e, "python3 failed while reading %s", j->path);
	}

	config_copy(&work, out);
	if (!j->overflow && j->len > 0 && j->buf) {
		parse_output(j->buf, j->len, &work, &e);
	}

	if (e.count && (strict || j->len == 0 || j->overflow)) {
		rc = -1;
		config_free(&work);
	} else {
		config_free(out);
		*out = work;
	}
	if (msg) {
		*msg = e.s;
	} else {
		free(e.s);
	}
	job_free(j);
	return rc;
}

int config_load(const char *path, struct config *out, bool strict, char **msg)
{
	struct cfg_job *j;
	char err[256];

	if (msg)
		*msg = NULL;
	j = cfg_job_start(path, err, sizeof(err));
	if (!j) {
		if (msg)
			*msg = strdup(err);
		return -1;
	}
	for (;;) {
		struct pollfd pfd = { cfg_job_fd(j), POLLIN, 0 };

		if (cfg_job_read(j))
			break;
		if (cfg_job_expired(j)) {
			cfg_job_kill(j);
			if (msg)
				*msg = strdup("the configuration did not finish in time");
			return -1;
		}
		(void)poll(&pfd, 1, 100);
	}
	return cfg_job_finish(j, out, strict, msg);
}

