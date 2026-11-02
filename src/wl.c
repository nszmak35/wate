/* wl.c - Wayland: window, keyboard, pointer, clipboard */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mouse.h"
#include "wate.h"

#define PASTE_MAX (8u << 20)
#define PASTE_TIMEOUT_MS 5000
#define CLICK_MS 400

/* the program's own title (OSC 0/2) wins over -t and the config */
static const char *window_title(void)
{
	if (W.osc_title && *W.osc_title)
		return W.osc_title;
	return W.opt.title ? W.opt.title : W.cfg.title;
}

static char *utf8_copy(const char *s)
{
	size_t n = strlen(s), i = 0, o = 0;
	char *out = malloc(n + 1);

	if (!out)
		return NULL;
	while (i < n) {
		const unsigned char *p = (const unsigned char *)s + i;
		size_t len = 0;

		if (p[0] < 0x80) {
			len = 1;
		} else if (p[0] >= 0xc2 && p[0] <= 0xdf && i + 1 < n && (p[1] & 0xc0) == 0x80) {
			len = 2;
		} else if (p[0] >= 0xe0 && p[0] <= 0xef && i + 2 < n && (p[1] & 0xc0) == 0x80 &&
			   (p[2] & 0xc0) == 0x80 && !(p[0] == 0xe0 && p[1] < 0xa0) &&
			   !(p[0] == 0xed && p[1] > 0x9f)) {
			len = 3;
		} else if (p[0] >= 0xf0 && p[0] <= 0xf4 && i + 3 < n && (p[1] & 0xc0) == 0x80 &&
			   (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80 &&
			   !(p[0] == 0xf0 && p[1] < 0x90) && !(p[0] == 0xf4 && p[1] > 0x8f)) {
			len = 4;
		}
		if (len == 0) {
			out[o++] = '?';
			i++;
		} else {
			memcpy(out + o, s + i, len);
			o += len;
			i += len;
		}
	}
	out[o] = '\0';
	return out;
}

void wl_title_check(void)
{
	unsigned int serial;
	const char *t;
	char *copy;

	if (!W.vte)
		return;
	t = tsm_vte_get_title(W.vte, &serial);
	if (serial == W.title_serial)
		return;
	W.title_serial = serial;
	copy = utf8_copy(t);
	if (!copy)
		return;
	free(W.osc_title);
	W.osc_title = copy;
	wl_title_changed();
}

static const char *window_app_id(void)
{
	return W.opt.app_id ? W.opt.app_id : W.cfg.app_id;
}

void wl_title_changed(void)
{
	if (W.toplvl) {
		xdg_toplevel_set_title(W.toplvl, window_title());
		xdg_toplevel_set_app_id(W.toplvl, window_app_id());
	}
}

static int grid_x(void)
{
	double px = wl_fixed_to_double(W.ptr_x) * (W.scale > 0 ? W.scale : 120) / 120.0;
	int x;

	if (W.fm.cell_w <= 0)
		return 0;
	x = (int)((px - W.padx) / W.fm.cell_w);
	if (px < W.padx)
		x = 0;
	if (x < 0)
		x = 0;
	if (x >= W.cols)
		x = W.cols - 1;
	return x < 0 ? 0 : x;
}

static int grid_y(void)
{
	double py = wl_fixed_to_double(W.ptr_y) * (W.scale > 0 ? W.scale : 120) / 120.0;
	int y;

	if (W.fm.cell_h <= 0)
		return 0;
	y = (int)((py - W.pady) / W.fm.cell_h);
	if (py < W.pady)
		y = 0;
	if (y < 0)
		y = 0;
	if (y >= W.rows)
		y = W.rows - 1;
	return y < 0 ? 0 : y;
}

static void cursor_apply(void)
{
	struct wl_cursor_image *img;
	struct wl_buffer *buffer;
	int w, h, hx, hy;

	if (!W.ptr || !W.ptr_inside)
		return;
	if (W.cursor_hidden || !W.cursor_surf || (!W.cursor_buf && !W.cursor_text)) {
		if (W.cursor_hidden)
			wl_pointer_set_cursor(W.ptr, W.ptr_serial, NULL, 0, 0);
		return;
	}
	if (W.cursor_buf) {
		buffer = W.cursor_buf;
		w = W.cursor_w;
		h = W.cursor_h;
		hx = W.cursor_hx;
		hy = W.cursor_hy;
	} else {
		if (W.cursor_text->image_count == 0)
			return;
		img = W.cursor_text->images[0];
		buffer = wl_cursor_image_get_buffer(img);
		w = (int)img->width;
		h = (int)img->height;
		hx = (int)img->hotspot_x;
		hy = (int)img->hotspot_y;
	}
	if (!buffer)
		return;
	wl_surface_attach(W.cursor_surf, buffer, 0, 0);
	wl_surface_damage_buffer(W.cursor_surf, 0, 0, w, h);
	wl_surface_commit(W.cursor_surf);
	wl_pointer_set_cursor(W.ptr, W.ptr_serial, W.cursor_surf, hx, hy);
}

/* reads one cursor image from an Xcursor file instead of the whole theme */
#define XC_IMAGE 0xfffd0002u
#define XC_MAX_FILE (1u << 20)

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool xcursor_parse(const unsigned char *d, size_t len, int want)
{
	uint32_t ntoc, i, best_pos = 0, w, h, xh, yh;
	int best_diff = 0, best_size = 0;
	size_t px;
	int fd;
	struct wl_shm_pool *pool;
	uint32_t *map;

	if (len < 16 || memcmp(d, "Xcur", 4) != 0)
		return false;
	ntoc = rd32(d + 12);
	if (ntoc == 0 || ntoc > 4096 || 16 + (size_t)ntoc * 12 > len)
		return false;
	for (i = 0; i < ntoc; i++) {
		const unsigned char *e = d + 16 + (size_t)i * 12;
		uint32_t sub = rd32(e + 4), pos = rd32(e + 8);
		int diff;

		if (rd32(e) != XC_IMAGE || pos > len || len - pos < 36)
			continue;
		diff = (int)sub - want;
		if (diff < 0)
			diff = -diff;
		if (!best_pos || diff < best_diff) {
			best_pos = pos;
			best_diff = diff;
			best_size = (int)sub;
		}
	}
	if (!best_pos || best_size <= 0)
		return false;
	w = rd32(d + best_pos + 16);
	h = rd32(d + best_pos + 20);
	xh = rd32(d + best_pos + 24);
	yh = rd32(d + best_pos + 28);
	if (w == 0 || h == 0 || w > 512 || h > 512 || xh > w || yh > h)
		return false;
	px = (size_t)w * h;
	if (len - best_pos - 36 < px * 4)
		return false;
	fd = make_shm_fd(px * 4);
	if (fd < 0)
		return false;
	map = mmap(NULL, px * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return false;
	}
	for (i = 0; i < px; i++)
		map[i] = rd32(d + best_pos + 36 + (size_t)i * 4);
	munmap(map, px * 4);
	pool = wl_shm_create_pool(W.shm, fd, (int32_t)(px * 4));
	close(fd);
	if (!pool)
		return false;
	W.cursor_buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)(w * 4),
						 WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (!W.cursor_buf)
		return false;
	W.cursor_w = (int)w;
	W.cursor_h = (int)h;
	W.cursor_hx = (int)xh;
	W.cursor_hy = (int)yh;
	return true;
}

static bool xcursor_try_file(const char *path, int want)
{
	FILE *fp = fopen(path, "rb");
	unsigned char *buf;
	size_t n;
	bool ok;

	if (!fp)
		return false;
	buf = malloc(XC_MAX_FILE);
	if (!buf) {
		fclose(fp);
		return false;
	}
	n = fread(buf, 1, XC_MAX_FILE, fp);
	fclose(fp);
	ok = xcursor_parse(buf, n, want);
	free(buf);
	return ok;
}

static size_t xcursor_dirs(char dirs[][512], size_t max)
{
	const char *xp = getenv("XCURSOR_PATH"), *home = getenv("HOME");
	const char *xdg = getenv("XDG_DATA_HOME");
	size_t n = 0;

	if (xp && *xp) {
		char tmp[1024], *tok, *save = NULL;

		snprintf(tmp, sizeof(tmp), "%s", xp);
		for (tok = strtok_r(tmp, ":", &save); tok && n < max; tok = strtok_r(NULL, ":", &save)) {
			if (tok[0] == '~' && home)
				snprintf(dirs[n], 512, "%s%s", home, tok + 1);
			else
				snprintf(dirs[n], 512, "%s", tok);
			n++;
		}
		return n;
	}
	if (xdg && *xdg == '/' && n < max)
		snprintf(dirs[n++], 512, "%s/icons", xdg);
	else if (home && *home && n < max)
		snprintf(dirs[n++], 512, "%s/.local/share/icons", home);
	if (home && *home && n < max)
		snprintf(dirs[n++], 512, "%s/.icons", home);
	if (n < max)
		snprintf(dirs[n++], 512, "/usr/share/icons");
	if (n < max)
		snprintf(dirs[n++], 512, "/usr/share/pixmaps");
	return n;
}

static bool xcursor_theme(const char *theme, const char *name, int want, int depth)
{
	char dirs[6][512], path[1100], line[512];
	size_t nd = xcursor_dirs(dirs, 6), i;

	if (depth > 6 || !*theme || strchr(theme, '/'))
		return false;
	for (i = 0; i < nd; i++) {
		snprintf(path, sizeof(path), "%s/%s/cursors/%s", dirs[i], theme, name);
		if (xcursor_try_file(path, want))
			return true;
	}
	for (i = 0; i < nd; i++) {
		FILE *fp;

		snprintf(path, sizeof(path), "%s/%s/index.theme", dirs[i], theme);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		while (fgets(line, sizeof(line), fp)) {
			char *p, *tok, *save = NULL;

			if (strncmp(line, "Inherits", 8) != 0)
				continue;
			p = strchr(line, '=');
			if (!p)
				continue;
			for (tok = strtok_r(p + 1, ",; \t\r\n", &save); tok;
			     tok = strtok_r(NULL, ",; \t\r\n", &save)) {
				if (xcursor_theme(tok, name, want, depth + 1)) {
					fclose(fp);
					return true;
				}
			}
		}
		fclose(fp);
	}
	return false;
}

static bool cursor_load_light(const char *theme, int px)
{
	static const char *const names[] = { "text", "xterm", "left_ptr" };
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (xcursor_theme(theme, names[i], px, 0))
			return true;
		if (strcmp(theme, "default") && xcursor_theme("default", names[i], px, 0))
			return true;
	}
	return false;
}

static void cursor_load(void)
{
	const char *theme = getenv("XCURSOR_THEME");
	const char *size = getenv("XCURSOR_SIZE");
	int px = size ? atoi(size) : 0;

	if (px <= 0 || px > 256)
		px = 24;
	W.cursor_surf = wl_compositor_create_surface(W.cp);
	if (cursor_load_light(theme && *theme ? theme : "default", px))
		return;
	W.cursor_theme = wl_cursor_theme_load(theme && *theme ? theme : NULL, px, W.shm);
	if (!W.cursor_theme)
		return;
	W.cursor_text = wl_cursor_theme_get_cursor(W.cursor_theme, "text");
	if (!W.cursor_text)
		W.cursor_text = wl_cursor_theme_get_cursor(W.cursor_theme, "xterm");
	if (!W.cursor_text)
		W.cursor_text = wl_cursor_theme_get_cursor(W.cursor_theme, "left_ptr");
}

void wl_hide_cursor(bool hide)
{
	if (W.cursor_hidden == hide)
		return;
	W.cursor_hidden = hide;
	cursor_apply();
}

static void free_send(struct send_job *s)
{
	if (s->fd >= 0)
		close(s->fd);
	free(s->data);
	s->fd = -1;
	s->data = NULL;
	s->len = s->off = 0;
}

static bool send_step(struct send_job *s)
{
	while (s->off < s->len) {
		ssize_t n = write(s->fd, s->data + s->off, s->len - s->off);

		if (n > 0) {
			s->off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return false;
		break;
	}
	return true;
}

static void start_send(const char *text, int fd)
{
	int i;
	struct send_job *s = NULL;

	if (!text) {
		close(fd);
		return;
	}
	for (i = 0; i < MAX_SENDS; i++) {
		if (W.sends[i].fd < 0) {
			s = &W.sends[i];
			break;
		}
	}
	if (!s) {
		close(fd);
		return;
	}
	s->data = strdup(text);
	if (!s->data) {
		close(fd);
		return;
	}
	s->fd = fd;
	s->len = strlen(text);
	s->off = 0;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (send_step(s))
		free_send(s);
}

static void ds_target(void *data, struct wl_data_source *src, const char *mime)
{
	(void)data;
	(void)src;
	(void)mime;
}

static void ds_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd)
{
	(void)data;
	(void)mime;
	if (src != W.d_src) {
		close(fd);
		return;
	}
	start_send(W.d_text, fd);
}

static void ds_cancelled(void *data, struct wl_data_source *src)
{
	(void)data;
	wl_data_source_destroy(src);
	if (src == W.d_src) {
		W.d_src = NULL;
		free(W.d_text);
		W.d_text = NULL;
	}
}

static void ds_dnd_drop_performed(void *data, struct wl_data_source *src)
{
	(void)data;
	(void)src;
}

static void ds_dnd_finished(void *data, struct wl_data_source *src)
{
	(void)data;
	(void)src;
}

static void ds_action(void *data, struct wl_data_source *src, uint32_t action)
{
	(void)data;
	(void)src;
	(void)action;
}

static const struct wl_data_source_listener ds_listener = {
	.target = ds_target,
	.send = ds_send,
	.cancelled = ds_cancelled,
	.dnd_drop_performed = ds_dnd_drop_performed,
	.dnd_finished = ds_dnd_finished,
	.action = ds_action,
};

static void pss_send(void *data, struct zwp_primary_selection_source_v1 *src,
		     const char *mime, int32_t fd)
{
	(void)data;
	(void)mime;
	if (src != W.ps_src) {
		close(fd);
		return;
	}
	start_send(W.ps_text, fd);
}

static void pss_cancelled(void *data, struct zwp_primary_selection_source_v1 *src)
{
	(void)data;
	zwp_primary_selection_source_v1_destroy(src);
	if (src == W.ps_src) {
		W.ps_src = NULL;
		free(W.ps_text);
		W.ps_text = NULL;
	}
}

static const struct zwp_primary_selection_source_v1_listener pss_listener = {
	.send = pss_send,
	.cancelled = pss_cancelled,
};

static char *selection_text(void)
{
	char *text = NULL;
	int n = tsm_screen_selection_copy(W.screen, &text);

	if (n <= 0 || !text) {
		free(text);
		return NULL;
	}
	return text;
}

static void copy_to_clipboard(uint32_t serial)
{
	char *text = selection_text();
	struct wl_data_source *src;

	if (!text || !W.data_dm || !W.dd) {
		free(text);
		return;
	}
	if (W.d_src) {
		wl_data_source_destroy(W.d_src);
		W.d_src = NULL;
	}
	free(W.d_text);
	W.d_text = text;
	src = wl_data_device_manager_create_data_source(W.data_dm);
	if (!src) {
		free(W.d_text);
		W.d_text = NULL;
		return;
	}
	wl_data_source_offer(src, "text/plain;charset=utf-8");
	wl_data_source_offer(src, "UTF8_STRING");
	wl_data_source_offer(src, "text/plain");
	wl_data_source_add_listener(src, &ds_listener, NULL);
	W.d_src = src;
	wl_data_device_set_selection(W.dd, src, serial);
}

static void copy_to_primary(uint32_t serial)
{
	char *text = selection_text();
	struct zwp_primary_selection_source_v1 *src;

	if (!text || !W.ps_dm || !W.psd) {
		free(text);
		return;
	}
	if (W.ps_src) {
		zwp_primary_selection_source_v1_destroy(W.ps_src);
		W.ps_src = NULL;
	}
	free(W.ps_text);
	W.ps_text = text;
	src = zwp_primary_selection_device_manager_v1_create_source(W.ps_dm);
	if (!src) {
		free(W.ps_text);
		W.ps_text = NULL;
		return;
	}
	zwp_primary_selection_source_v1_offer(src, "text/plain;charset=utf-8");
	zwp_primary_selection_source_v1_offer(src, "UTF8_STRING");
	zwp_primary_selection_source_v1_offer(src, "text/plain");
	zwp_primary_selection_source_v1_add_listener(src, &pss_listener, NULL);
	W.ps_src = src;
	zwp_primary_selection_device_v1_set_selection(W.psd, src, serial);
}

static void do_offer(void *data, struct wl_data_offer *o, const char *mime)
{
	(void)data;
	(void)o;
	if (!strcmp(mime, "text/plain;charset=utf-8"))
		W.d_mime = "text/plain;charset=utf-8";
	else if (!strcmp(mime, "UTF8_STRING") && (!W.d_mime || strcmp(W.d_mime, "text/plain;charset=utf-8")))
		W.d_mime = "UTF8_STRING";
	else if (!W.d_mime && !strcmp(mime, "text/plain"))
		W.d_mime = "text/plain";
}

static void do_source_actions(void *data, struct wl_data_offer *o, uint32_t a)
{
	(void)data;
	(void)o;
	(void)a;
}

static void do_action(void *data, struct wl_data_offer *o, uint32_t a)
{
	(void)data;
	(void)o;
	(void)a;
}

static const struct wl_data_offer_listener do_listener = {
	.offer = do_offer,
	.source_actions = do_source_actions,
	.action = do_action,
};

static void dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *offer)
{
	(void)data;
	(void)dd;
	if (W.d_offer)
		wl_data_offer_destroy(W.d_offer);
	W.d_offer = offer;
	W.d_mime = NULL;
	wl_data_offer_add_listener(offer, &do_listener, NULL);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
		     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
	(void)data;
	(void)dd;
	(void)serial;
	(void)s;
	(void)x;
	(void)y;
	(void)o;
}

static void dd_leave(void *data, struct wl_data_device *dd)
{
	(void)data;
	(void)dd;
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t t, wl_fixed_t x,
		      wl_fixed_t y)
{
	(void)data;
	(void)dd;
	(void)t;
	(void)x;
	(void)y;
}

static void dd_drop(void *data, struct wl_data_device *dd)
{
	(void)data;
	(void)dd;
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)data;
	(void)dd;
	if (!o && W.d_offer) {
		wl_data_offer_destroy(W.d_offer);
		W.d_offer = NULL;
		W.d_mime = NULL;
	}
}

static const struct wl_data_device_listener dd_listener = {
	.data_offer = dd_data_offer,
	.enter = dd_enter,
	.leave = dd_leave,
	.motion = dd_motion,
	.drop = dd_drop,
	.selection = dd_selection,
};

static void pso_offer(void *data, struct zwp_primary_selection_offer_v1 *o, const char *mime)
{
	(void)data;
	(void)o;
	if (!strcmp(mime, "text/plain;charset=utf-8"))
		W.ps_mime = "text/plain;charset=utf-8";
	else if (!strcmp(mime, "UTF8_STRING") && (!W.ps_mime || strcmp(W.ps_mime, "text/plain;charset=utf-8")))
		W.ps_mime = "UTF8_STRING";
	else if (!W.ps_mime && !strcmp(mime, "text/plain"))
		W.ps_mime = "text/plain";
}

static const struct zwp_primary_selection_offer_v1_listener pso_listener = {
	.offer = pso_offer,
};

static void psd_data_offer(void *data, struct zwp_primary_selection_device_v1 *d,
			   struct zwp_primary_selection_offer_v1 *offer)
{
	(void)data;
	(void)d;
	if (W.ps_offer)
		zwp_primary_selection_offer_v1_destroy(W.ps_offer);
	W.ps_offer = offer;
	W.ps_mime = NULL;
	zwp_primary_selection_offer_v1_add_listener(offer, &pso_listener, NULL);
}

static void psd_selection(void *data, struct zwp_primary_selection_device_v1 *d,
			  struct zwp_primary_selection_offer_v1 *o)
{
	(void)data;
	(void)d;
	if (!o && W.ps_offer) {
		zwp_primary_selection_offer_v1_destroy(W.ps_offer);
		W.ps_offer = NULL;
		W.ps_mime = NULL;
	}
}

static const struct zwp_primary_selection_device_v1_listener psd_listener = {
	.data_offer = psd_data_offer,
	.selection = psd_selection,
};

static void paste_reset(void)
{
	if (W.paste.fd >= 0)
		close(W.paste.fd);
	W.paste.fd = -1;
	free(W.paste.buf);
	W.paste.buf = NULL;
	W.paste.len = W.paste.cap = 0;
}

static void paste_start(bool primary)
{
	int p[2];

	if (W.paste.fd >= 0)
		return;
	if (primary) {
		if (!W.ps_offer || !W.ps_mime)
			return;
	} else {
		if (!W.d_offer || !W.d_mime)
			return;
	}
	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	if (primary)
		zwp_primary_selection_offer_v1_receive(W.ps_offer, W.ps_mime, p[1]);
	else
		wl_data_offer_receive(W.d_offer, W.d_mime, p[1]);
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	W.paste.fd = p[0];
	W.paste.primary = primary;
	W.paste.len = 0;
	W.paste.deadline = now_ms() + PASTE_TIMEOUT_MS;
}

static void paste_finish(void)
{
	size_t i, n = 0;
	char *out;

	if (W.paste.len == 0 || !W.paste.buf) {
		paste_reset();
		return;
	}
	out = malloc(W.paste.len + 1);
	if (!out) {
		paste_reset();
		return;
	}
	for (i = 0; i < W.paste.len; i++) {
		unsigned char c = (unsigned char)W.paste.buf[i];

		if (c == '\r') {
			if (i + 1 < W.paste.len && W.paste.buf[i + 1] == '\n')
				i++;
			out[n++] = '\r';
		} else if (c == '\n') {
			out[n++] = '\r';
		} else if (c == '\t' || c >= 0x20) {
			if (c != 0x7f)
				out[n++] = (char)c;
		}
	}
	if (n > 0) {
		tsm_vte_paste_begin(W.vte);
		pty_write(out, n);
		tsm_vte_paste_end(W.vte);
		if (tsm_screen_sb_reset(W.screen))
			W.need_redraw = true;
	}
	free(out);
	paste_reset();
}

int wl_clipboard_fds(struct pollfd *fds, int max)
{
	int n = 0, i;

	if (W.paste.fd >= 0 && n < max) {
		fds[n].fd = W.paste.fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	for (i = 0; i < MAX_SENDS && n < max; i++) {
		if (W.sends[i].fd >= 0) {
			fds[n].fd = W.sends[i].fd;
			fds[n].events = POLLOUT;
			fds[n].revents = 0;
			n++;
		}
	}
	return n;
}

void wl_clipboard_dispatch(const struct pollfd *fds, int n)
{
	int i, j;

	for (i = 0; i < n; i++) {
		if (!fds[i].revents)
			continue;
		if (fds[i].fd == W.paste.fd) {
			for (;;) {
				ssize_t r;

				if (W.paste.len + 4096 > W.paste.cap) {
					size_t ncap = W.paste.cap ? W.paste.cap * 2 : 8192;
					char *nb;

					if (ncap > PASTE_MAX)
						ncap = PASTE_MAX;
					if (W.paste.len + 1 >= ncap)
						break;
					nb = realloc(W.paste.buf, ncap);
					if (!nb)
						break;
					W.paste.buf = nb;
					W.paste.cap = ncap;
				}
				r = read(W.paste.fd, W.paste.buf + W.paste.len,
					 W.paste.cap - W.paste.len);
				if (r > 0) {
					W.paste.len += (size_t)r;
					continue;
				}
				if (r < 0 && errno == EINTR)
					continue;
				if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					goto next;
				break;
			}
			paste_finish();
			continue;
		}
		for (j = 0; j < MAX_SENDS; j++) {
			if (W.sends[j].fd == fds[i].fd) {
				if (send_step(&W.sends[j]) || (fds[i].revents & (POLLERR | POLLHUP)))
					free_send(&W.sends[j]);
				break;
			}
		}
next:
		;
	}
	if (W.paste.fd >= 0 && now_ms() > W.paste.deadline)
		paste_finish();
}

static void action_zoom(int delta)
{
	int old = W.zoom;

	W.zoom += delta;
	if (W.zoom < -20)
		W.zoom = -20;
	if (W.zoom > 60)
		W.zoom = 60;
	if (W.zoom == old)
		return;
	if (!render_font_reload()) {
		W.zoom = old;
		return;
	}
	render_layout();
	W.full_redraw = true;
	W.need_redraw = true;
}

static void action_run(enum key_action act, int arg, uint32_t serial)
{
	switch (act) {
	case ACT_COPY:
		copy_to_clipboard(serial);
		break;
	case ACT_PASTE:
		paste_start(false);
		break;
	case ACT_PASTE_PRIMARY:
		paste_start(true);
		break;
	case ACT_FONTSIZE:
		action_zoom(arg);
		break;
	case ACT_FONTSIZE_RESET:
		action_zoom(-W.zoom);
		break;
	case ACT_SCROLL_UP:
		tsm_screen_sb_up(W.screen, arg > 0 ? arg : 1);
		W.need_redraw = true;
		break;
	case ACT_SCROLL_DOWN:
		tsm_screen_sb_down(W.screen, arg > 0 ? arg : 1);
		W.need_redraw = true;
		break;
	case ACT_SCROLL_PAGE_UP:
		tsm_screen_sb_page_up(W.screen, 1);
		W.need_redraw = true;
		break;
	case ACT_SCROLL_PAGE_DOWN:
		tsm_screen_sb_page_down(W.screen, 1);
		W.need_redraw = true;
		break;
	case ACT_SCROLL_TOP:
		tsm_screen_sb_up(W.screen, W.cfg.scrollback + W.rows);
		W.need_redraw = true;
		break;
	case ACT_SCROLL_BOTTOM:
		tsm_screen_sb_reset(W.screen);
		W.need_redraw = true;
		break;
	case ACT_RESET:
		tsm_vte_hard_reset(W.vte);
		W.need_redraw = true;
		break;
	case ACT_NEW_TERMINAL:
		new_terminal();
		break;
	case ACT_NONE:
		break;
	}
}

/* the compose table is read on the first dead key or Multi_key */
static void load_compose(void)
{
	const char *locale = getenv("LC_ALL");

	if (!locale || !*locale)
		locale = getenv("LC_CTYPE");
	if (!locale || !*locale)
		locale = getenv("LANG");
	if (!locale || !*locale)
		locale = "C";
	xkb_compose_state_unref(W.compose_state);
	xkb_compose_table_unref(W.compose_table);
	W.compose_state = NULL;
	W.compose_table = xkb_compose_table_new_from_locale(W.xkb_ctx, locale,
							    XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (W.compose_table)
		W.compose_state = xkb_compose_state_new(W.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt, int32_t fd,
		       uint32_t size)
{
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	char *map;

	(void)data;
	(void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}
	keymap = xkb_keymap_new_from_string(W.xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
					    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);
	if (!keymap) {
		fprintf(stderr, "wate: failed to compile the keymap\n");
		return;
	}
	state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}
	xkb_keymap_unref(W.xkb_keymap);
	xkb_state_unref(W.xkb_state);
	W.xkb_keymap = keymap;
	W.xkb_state = state;
	W.idx_shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
	W.idx_ctrl = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	W.idx_alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
	W.idx_logo = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_LOGO);
	W.mods = 0;
	xkb_compose_state_unref(W.compose_state);
	xkb_compose_table_unref(W.compose_table);
	W.compose_state = NULL;
	W.compose_table = NULL;
	W.compose_tried = false;
	mem_trim();
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *s, struct wl_array *keys)
{
	(void)data;
	(void)k;
	(void)s;
	(void)keys;
	W.last_serial = serial;
	W.focused = true;
	W.full_redraw = true;
	W.need_redraw = true;
	if (W.vte && (tsm_vte_get_mouse_flags(W.vte) & TSM_MOUSE_FOCUS))
		pty_write("\033[I", 3);
}

static void repeat_stop(void)
{
	W.repeat.active = false;
	W.repeat.deadline = 0;
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)data;
	(void)k;
	(void)s;
	W.last_serial = serial;
	W.focused = false;
	repeat_stop();
	W.full_redraw = true;
	W.need_redraw = true;
	if (W.vte && (tsm_vte_get_mouse_flags(W.vte) & TSM_MOUSE_FOCUS))
		pty_write("\033[O", 3);
}

static xkb_keysym_t compose(xkb_keysym_t sym)
{
	if (!W.compose_state && !W.compose_tried && sym != XKB_KEY_NoSymbol &&
	    (sym == 0xff20  || (sym >= 0xfe50 && sym <= 0xfe9f ))) {
		W.compose_tried = true;
		load_compose();
	}
	if (!W.compose_state || sym == XKB_KEY_NoSymbol)
		return sym;
	if (xkb_compose_state_feed(W.compose_state, sym) != XKB_COMPOSE_FEED_ACCEPTED)
		return sym;
	switch (xkb_compose_state_get_status(W.compose_state)) {
	case XKB_COMPOSE_COMPOSED: {
		xkb_keysym_t out = xkb_compose_state_get_one_sym(W.compose_state);

		xkb_compose_state_reset(W.compose_state);
		return out;
	}
	case XKB_COMPOSE_COMPOSING:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_CANCELLED:
		xkb_compose_state_reset(W.compose_state);
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_NOTHING:
	default:
		return sym;
	}
}

static bool key_has_sym(xkb_keycode_t code, xkb_keysym_t want)
{
	xkb_layout_index_t nl, l;

	if (!W.xkb_keymap)
		return false;
	nl = xkb_keymap_num_layouts_for_key(W.xkb_keymap, code);
	for (l = 0; l < nl; l++) {
		xkb_level_index_t lv;

		for (lv = 0; lv < 2; lv++) {
			const xkb_keysym_t *syms = NULL;
			int n = xkb_keymap_key_get_syms_by_level(W.xkb_keymap, code, l, lv, &syms);

			if (n > 0 && syms && xkb_keysym_to_lower(syms[0]) == want)
				return true;
		}
	}
	return false;
}

static unsigned consumed_mods(xkb_keycode_t code)
{
	unsigned m = 0;

	if (xkb_state_mod_index_is_consumed(W.xkb_state, code, W.idx_shift) == 1)
		m |= MOD_SHIFT;
	if (xkb_state_mod_index_is_consumed(W.xkb_state, code, W.idx_ctrl) == 1)
		m |= MOD_CTRL;
	if (xkb_state_mod_index_is_consumed(W.xkb_state, code, W.idx_alt) == 1)
		m |= MOD_ALT;
	if (xkb_state_mod_index_is_consumed(W.xkb_state, code, W.idx_logo) == 1)
		m |= MOD_SUPER;
	return m;
}

static const struct key_binding *find_binding(xkb_keycode_t code, xkb_keysym_t sym)
{
	size_t i;
	xkb_keysym_t lower = xkb_keysym_to_lower(sym);
	unsigned reduced = W.mods & ~consumed_mods(code);

	for (i = 0; i < W.cfg.nkeys; i++) {
		const struct key_binding *b = &W.cfg.keys[i];
		xkb_keysym_t want;

		if (b->mods != W.mods && b->mods != reduced)
			continue;
		want = xkb_keysym_from_name(b->key, XKB_KEYSYM_CASE_INSENSITIVE);
		if (want == XKB_KEY_NoSymbol)
			continue;
		want = xkb_keysym_to_lower(want);
		if (want == lower || key_has_sym(code, want))
			return b;
	}
	return NULL;
}

static unsigned tsm_mods(void)
{
	unsigned m = 0;

	if (W.mods & MOD_SHIFT)
		m |= TSM_SHIFT_MASK;
	if (W.mods & MOD_CTRL)
		m |= TSM_CONTROL_MASK;
	if (W.mods & MOD_ALT)
		m |= TSM_ALT_MASK;
	if (W.mods & MOD_SUPER)
		m |= TSM_LOGO_MASK;
	return m;
}

static uint32_t ascii_for_key(xkb_keycode_t code, xkb_keysym_t sym)
{
	xkb_layout_index_t nl, l;

	if (sym < 0x80)
		return sym;
	nl = xkb_keymap_num_layouts_for_key(W.xkb_keymap, code);
	for (l = 0; l < nl; l++) {
		const xkb_keysym_t *syms = NULL;
		int n = xkb_keymap_key_get_syms_by_level(W.xkb_keymap, code, l, 0, &syms);

		if (n > 0 && syms && syms[0] != XKB_KEY_NoSymbol && syms[0] < 0x80)
			return syms[0];
	}
	return XKB_KEY_NoSymbol;
}

static void handle_key(uint32_t key, xkb_keysym_t sym, uint32_t serial)
{
	xkb_keycode_t code = key + 8;
	const struct key_binding *b;
	uint32_t unicode;

	b = find_binding(code, sym);
	if (b) {
		action_run(b->action, b->arg, serial);
		return;
	}
	unicode = xkb_keysym_to_utf32(sym);
	if (unicode == 0)
		unicode = TSM_VTE_INVALID;
	if (tsm_vte_handle_keyboard(W.vte, sym, ascii_for_key(code, sym), tsm_mods(), unicode)) {
		if (tsm_screen_sb_reset(W.screen))
			W.need_redraw = true;
		if (W.cfg.hide_when_typing)
			wl_hide_cursor(true);
	}
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		    uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;

	(void)data;
	(void)k;
	(void)time;
	if (!W.xkb_keymap || !W.xkb_state)
		return;
	W.last_serial = serial;
	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (W.repeat.active && W.repeat.key == key)
			repeat_stop();
		return;
	}
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	sym = compose(xkb_state_key_get_one_sym(W.xkb_state, key + 8));
	if (sym == XKB_KEY_NoSymbol)
		return;
	handle_key(key, sym, serial);

	if (xkb_keymap_key_repeats(W.xkb_keymap, key + 8) && W.repeat.delay_ms > 0 &&
	    W.repeat.interval_ms > 0) {
		W.repeat.active = true;
		W.repeat.key = key;
		W.repeat.sym = sym;
		W.repeat.deadline = now_ms() + W.repeat.delay_ms;
	} else {
		repeat_stop();
	}
}

static void kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
			  uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)data;
	(void)k;
	(void)serial;
	if (!W.xkb_keymap || !W.xkb_state)
		return;
	xkb_state_update_mask(W.xkb_state, dep, lat, lock, 0, 0, group);
	W.mods = 0;
	if (xkb_state_mod_index_is_active(W.xkb_state, W.idx_shift, XKB_STATE_MODS_EFFECTIVE) == 1)
		W.mods |= MOD_SHIFT;
	if (xkb_state_mod_index_is_active(W.xkb_state, W.idx_ctrl, XKB_STATE_MODS_EFFECTIVE) == 1)
		W.mods |= MOD_CTRL;
	if (xkb_state_mod_index_is_active(W.xkb_state, W.idx_alt, XKB_STATE_MODS_EFFECTIVE) == 1)
		W.mods |= MOD_ALT;
	if (xkb_state_mod_index_is_active(W.xkb_state, W.idx_logo, XKB_STATE_MODS_EFFECTIVE) == 1)
		W.mods |= MOD_SUPER;
}

static void kbd_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)data;
	(void)k;
	if (rate > 1000)
		rate = 1000;
	W.repeat.interval_ms = rate > 0 ? 1000 / rate : 0;
	if (W.repeat.interval_ms < 1 && rate > 0)
		W.repeat.interval_ms = 1;
	W.repeat.delay_ms = rate > 0 && delay > 0 ? delay : 0;
	if (rate <= 0)
		repeat_stop();
}

static const struct wl_keyboard_listener kbd_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_modifiers,
	.repeat_info = kbd_repeat_info,
};

void wl_repeat_tick(void)
{
	long long now;

	if (!W.repeat.active || !W.xkb_keymap)
		return;
	now = now_ms();
	if (now < W.repeat.deadline)
		return;
	handle_key(W.repeat.key, W.repeat.sym, W.last_serial);
	W.repeat.deadline = now + W.repeat.interval_ms;
}

static int press_x, press_y;

static unsigned mouse_flags(void)
{
	return W.vte ? tsm_vte_get_mouse_flags(W.vte) : 0;
}

static bool mouse_reporting(void)
{
	return (mouse_flags() & TSM_MOUSE_TRACK_MASK) != TSM_MOUSE_TRACK_NONE &&
	       !(W.mods & MOD_SHIFT);
}

static void mouse_send(int code, bool release)
{
	char buf[40];
	unsigned f = mouse_flags();
	size_t n;

	if ((f & TSM_MOUSE_TRACK_MASK) != TSM_MOUSE_TRACK_X10) {
		if (W.mods & MOD_ALT)
			code |= MOUSE_CODE_ALT;
		if (W.mods & MOD_CTRL)
			code |= MOUSE_CODE_CTRL;
	}
	n = mouse_encode(buf, f, code, release, grid_x() + 1, grid_y() + 1);
	if (n)
		pty_write(buf, n);
}

static int lowest_button(void)
{
	int b;

	for (b = 0; b < 3; b++)
		if (W.mouse_buttons & (1u << b))
			return b;
	return 3;
}

/* bindings with modifiers also win over mouse reporting */
static const struct button_binding *find_button(enum mouse_btn btn)
{
	size_t i;

	for (i = 0; i < W.cfg.nbuttons; i++) {
		const struct button_binding *b = &W.cfg.buttons[i];

		if (b->button == btn && b->mods == W.mods && b->action != ACT_NONE &&
		    (b->mods || !mouse_reporting()))
			return b;
	}
	return NULL;
}

static int wl_button_id(uint32_t button)
{
	switch (button) {
	case 0x110:
		return MB_LEFT;
	case 0x112:
		return MB_MIDDLE;
	case 0x111:
		return MB_RIGHT;
	case 0x113:
		return MB_BACK;
	case 0x114:
		return MB_FORWARD;
	}
	return -1;
}

static double bind_pending;

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		      wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)s;
	W.ptr_x = x;
	W.ptr_y = y;
	W.ptr_serial = serial;
	W.ptr_inside = true;
	W.cursor_hidden = false;
	W.mouse_col = W.mouse_row = -1;
	cursor_apply();
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)s;
	W.ptr_inside = false;
	W.scroll_pending = 0;
	W.wheel_pending = 0;
	bind_pending = 0;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		       wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)time;
	W.ptr_x = x;
	W.ptr_y = y;
	if (W.cursor_hidden)
		wl_hide_cursor(false);
	if (mouse_reporting()) {
		unsigned track = mouse_flags() & TSM_MOUSE_TRACK_MASK;
		int c = grid_x(), r = grid_y();

		if (c != W.mouse_col || r != W.mouse_row) {
			W.mouse_col = c;
			W.mouse_row = r;
			if (track == TSM_MOUSE_TRACK_DRAG && W.mouse_buttons)
				mouse_send(lowest_button() | MOUSE_CODE_MOTION, false);
			else if (track == TSM_MOUSE_TRACK_MOTION)
				mouse_send(lowest_button() | MOUSE_CODE_MOTION, false);
		}
		return;
	}
	if (W.sel == SEL_PRESSED) {
		if (grid_x() != press_x || grid_y() != press_y) {
			tsm_screen_selection_start(W.screen, TSM_SM_CHAR, press_x, press_y);
			tsm_screen_selection_target(W.screen, grid_x(), grid_y());
			W.sel = SEL_DRAG;
			W.need_redraw = true;
		}
	} else if (W.sel == SEL_DRAG) {
		tsm_screen_selection_target(W.screen, grid_x(), grid_y());
		W.need_redraw = true;
	}
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		       uint32_t button, uint32_t state)
{
	long long now = now_ms();

	(void)data;
	(void)p;
	(void)time;
	W.last_serial = serial;
	if (W.cursor_hidden)
		wl_hide_cursor(false);

	{
		int id = wl_button_id(button);
		bool press = state == WL_POINTER_BUTTON_STATE_PRESSED;

		if (id >= 0) {
			const struct button_binding *bb = find_button((enum mouse_btn)id);

			if (bb) {
				if (press) {
					W.bound_buttons |= 1u << id;
					action_run(bb->action, bb->arg, serial);
				} else {
					W.bound_buttons &= ~(1u << id);
				}
				return;
			}
			if (!press && (W.bound_buttons & (1u << id))) {
				W.bound_buttons &= ~(1u << id);
				return;
			}
		}
	}

	{
		int b = button == 0x110 ? 0 : button == 0x112 ? 1 : button == 0x111 ? 2 : -1;
		bool press = state == WL_POINTER_BUTTON_STATE_PRESSED;
		bool was_down = b >= 0 && (W.mouse_buttons & (1u << b));
		unsigned track = mouse_flags() & TSM_MOUSE_TRACK_MASK;

		if (b >= 0 && track != TSM_MOUSE_TRACK_NONE &&
		    (mouse_reporting() || (!press && was_down))) {
			if (press)
				W.mouse_buttons |= 1u << b;
			else
				W.mouse_buttons &= ~(1u << b);
			W.mouse_col = grid_x();
			W.mouse_row = grid_y();
			if (press || track != TSM_MOUSE_TRACK_X10)
				mouse_send(b, !press);
			return;
		}
		if (b >= 0 && !press)
			W.mouse_buttons &= ~(1u << b);
		if (track != TSM_MOUSE_TRACK_NONE && !(W.mods & MOD_SHIFT) && b < 0)
			return;
	}

	if (button == 0x110) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			int gx = grid_x(), gy = grid_y();

			if (W.sel == SEL_ACTIVE || W.sel == SEL_DRAG) {
				tsm_screen_selection_reset(W.screen);
				W.need_redraw = true;
			}
			if (W.click_count > 0 && W.click_x == gx && W.click_y == gy &&
			    now - W.click_time < CLICK_MS)
				W.click_count++;
			else
				W.click_count = 1;
			W.click_x = gx;
			W.click_y = gy;
			W.click_time = now;
			press_x = gx;
			press_y = gy;

			if (W.click_count == 2) {
				tsm_screen_selection_start(W.screen, TSM_SM_WORD, gx, gy);
				W.sel = SEL_DRAG;
				W.need_redraw = true;
			} else if (W.click_count >= 3) {
				tsm_screen_selection_start(W.screen, TSM_SM_LINE, gx, gy);
				W.sel = SEL_DRAG;
				W.click_count = 0;
				W.need_redraw = true;
			} else {
				W.sel = SEL_PRESSED;
			}
		} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
			if (W.sel == SEL_DRAG) {
				tsm_screen_selection_finish(W.screen);
				copy_to_primary(serial);
				W.sel = SEL_ACTIVE;
				W.need_redraw = true;
			} else {
				W.sel = SEL_NONE;
			}
		}
	} else if (button == 0x112 && state == WL_POINTER_BUTTON_STATE_RELEASED) {
		paste_start(true);
	}
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		     wl_fixed_t value)
{
	int v;

	(void)data;
	(void)p;
	(void)time;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	if (W.cfg.nbuttons) {
		enum mouse_btn dir = value < 0 ? MB_WHEEL_UP : MB_WHEEL_DOWN;
		const struct button_binding *bb = find_button(dir);

		if (bb) {
			int steps, k;

			bind_pending += wl_fixed_to_double(value) / 10.0;
			steps = (int)bind_pending;
			bind_pending -= steps;
			if (steps < 0)
				steps = -steps;
			if (steps > 12)
				steps = 12;
			if (bb->action == ACT_FONTSIZE && steps)
				action_zoom(bb->arg * steps);
			else
				for (k = 0; k < steps; k++)
					action_run(bb->action, bb->arg, W.last_serial);
			return;
		}
	}
	if (mouse_reporting() && (mouse_flags() & TSM_MOUSE_TRACK_MASK) >= TSM_MOUSE_TRACK_PRESS) {
		int steps, k;

		W.wheel_pending += wl_fixed_to_double(value) / 10.0;
		steps = (int)W.wheel_pending;
		W.wheel_pending -= steps;
		if (steps > 12)
			steps = 12;
		if (steps < -12)
			steps = -12;
		for (k = 0; k < (steps < 0 ? -steps : steps); k++)
			mouse_send(MOUSE_CODE_WHEEL + (steps > 0 ? 1 : 0), false);
		return;
	}
	W.scroll_pending += wl_fixed_to_double(value) / 3.0;
	v = (int)W.scroll_pending;
	W.scroll_pending -= v;
	if (v == 0)
		return;
	if ((mouse_flags() & TSM_MOUSE_TRACK_MASK) == TSM_MOUSE_TRACK_NONE &&
	    (mouse_flags() & TSM_MOUSE_ALT_SCROLL) &&
	    (tsm_screen_get_flags(W.screen) & TSM_SCREEN_ALTERNATE)) {
		int k, n = v < 0 ? -v : v;

		if (n > 30)
			n = 30;
		for (k = 0; k < n; k++)
			tsm_vte_handle_keyboard(W.vte, v > 0 ? XKB_KEY_Down : XKB_KEY_Up,
						XKB_KEY_NoSymbol, 0, TSM_VTE_INVALID);
		return;
	}
	if (v > 0)
		tsm_screen_sb_down(W.screen, v);
	else
		tsm_screen_sb_up(W.screen, -v);
	W.need_redraw = true;
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src)
{
	(void)data;
	(void)p;
	(void)src;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)p;
	(void)time;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		W.scroll_pending = 0;
		bind_pending = 0;
	}
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t d)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)d;
}

static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !W.kbd) {
		W.kbd = wl_seat_get_keyboard(seat);
		if (W.kbd)
			wl_keyboard_add_listener(W.kbd, &kbd_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && W.kbd) {
		wl_keyboard_release(W.kbd);
		W.kbd = NULL;
		W.focused = false;
		repeat_stop();
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !W.ptr) {
		W.ptr = wl_seat_get_pointer(seat);
		if (W.ptr)
			wl_pointer_add_listener(W.ptr, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && W.ptr) {
		wl_pointer_release(W.ptr);
		W.ptr = NULL;
		W.ptr_inside = false;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

void wl_scale_changed(void)
{
	if (!W.configured)
		return;
	if (render_font_reload())
		render_layout();
	W.full_redraw = true;
	W.need_redraw = true;
}

static void fs_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
	int s = (int)scale;

	(void)data;
	(void)fs;
	if (W.vp && s >= 60 && s <= 960 && s != W.scale) {
		W.scale = s;
		wl_scale_changed();
	}
}

static const struct wp_fractional_scale_v1_listener fs_listener = {
	.preferred_scale = fs_preferred_scale,
};

static void toplvl_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height,
			     struct wl_array *states)
{
	(void)data;
	(void)t;
	(void)states;
	W.pend_w = width > 0 ? width : 0;
	W.pend_h = height > 0 ? height : 0;
}

static void toplvl_close(void *data, struct xdg_toplevel *t)
{
	(void)data;
	(void)t;
	W.die = true;
}

static const struct xdg_toplevel_listener toplvl_listener = {
	.configure = toplvl_configure,
	.close = toplvl_close,
};

static bool first_frame_done;

static void xsurf_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(s, serial);
	if (W.pend_w > 0)
		W.conf_w = W.pend_w;
	if (W.pend_h > 0)
		W.conf_h = W.pend_h;
	render_layout();
	W.configured = true;
	child_maybe_spawn();
	W.need_redraw = true;
	render_redraw(true);
	if (!first_frame_done) {
		first_frame_done = true;
		mem_trim();
	}
}

static const struct xdg_surface_listener xsurf_listener = {
	.configure = xsurf_configure,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {
	.ping = wm_ping,
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	(void)data;
	(void)shm;
	if (format == WL_SHM_FORMAT_ARGB8888)
		W.shm_argb = true;
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t id, const char *iface,
		       uint32_t version)
{
	(void)data;
	if (!strcmp(iface, "wl_compositor")) {
		W.cp = wl_registry_bind(r, id, &wl_compositor_interface, version < 4 ? version : 4);
	} else if (!strcmp(iface, "wl_shm")) {
		W.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
		if (W.shm)
			wl_shm_add_listener(W.shm, &shm_listener, NULL);
	} else if (!strcmp(iface, "xdg_wm_base")) {
		W.wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
		if (W.wm_base)
			xdg_wm_base_add_listener(W.wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, "wl_seat")) {
		W.seat = wl_registry_bind(r, id, &wl_seat_interface, version < 5 ? version : 5);
		if (W.seat)
			wl_seat_add_listener(W.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, "wl_data_device_manager")) {
		W.data_dm = wl_registry_bind(r, id, &wl_data_device_manager_interface,
					     version < 2 ? version : 2);
	} else if (!strcmp(iface, "zwp_primary_selection_device_manager_v1")) {
		W.ps_dm = wl_registry_bind(r, id, &zwp_primary_selection_device_manager_v1_interface, 1);
	} else if (!strcmp(iface, "zxdg_decoration_manager_v1")) {
		W.deco_mgr = wl_registry_bind(r, id, &zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, "wp_viewporter")) {
		W.vpm = wl_registry_bind(r, id, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, "wp_fractional_scale_manager_v1")) {
		W.fsm = wl_registry_bind(r, id, &wp_fractional_scale_manager_v1_interface, 1);
	}
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

bool wl_setup(char *err, size_t errlen)
{
	int i;

	for (i = 0; i < MAX_SENDS; i++)
		W.sends[i].fd = -1;
	W.paste.fd = -1;
	if (W.scale <= 0)
		W.scale = 120;

	W.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!W.xkb_ctx) {
		snprintf(err, errlen, "failed to create the xkb context");
		return false;
	}
	W.display = wl_display_connect(W.opt.display);
	if (!W.display) {
		snprintf(err, errlen, "cannot connect to the Wayland display%s%s",
			 W.opt.display ? " " : "", W.opt.display ? W.opt.display : "");
		return false;
	}
	W.registry = wl_display_get_registry(W.display);
	if (!W.registry) {
		snprintf(err, errlen, "cannot get the Wayland registry");
		return false;
	}
	wl_registry_add_listener(W.registry, &reg_listener, NULL);
	if (wl_display_roundtrip(W.display) < 0 || wl_display_roundtrip(W.display) < 0) {
		snprintf(err, errlen, "Wayland roundtrip failed");
		return false;
	}
	if (!W.cp || !W.shm) {
		snprintf(err, errlen, "the compositor lacks wl_compositor or wl_shm");
		return false;
	}
	if (!W.wm_base) {
		snprintf(err, errlen, "the compositor does not support xdg_wm_base");
		return false;
	}
	if (!W.shm_argb) {
		snprintf(err, errlen, "the compositor lacks the ARGB8888 buffer format");
		return false;
	}

	if (W.data_dm && W.seat) {
		W.dd = wl_data_device_manager_get_data_device(W.data_dm, W.seat);
		if (W.dd)
			wl_data_device_add_listener(W.dd, &dd_listener, NULL);
	}
	if (W.ps_dm && W.seat) {
		W.psd = zwp_primary_selection_device_manager_v1_get_device(W.ps_dm, W.seat);
		if (W.psd)
			zwp_primary_selection_device_v1_add_listener(W.psd, &psd_listener, NULL);
	}
	cursor_load();

	W.surf = wl_compositor_create_surface(W.cp);
	if (!W.surf) {
		snprintf(err, errlen, "cannot create a surface");
		return false;
	}
	W.xsurf = xdg_wm_base_get_xdg_surface(W.wm_base, W.surf);
	if (!W.xsurf) {
		snprintf(err, errlen, "cannot create an xdg_surface");
		return false;
	}
	xdg_surface_add_listener(W.xsurf, &xsurf_listener, NULL);
	W.toplvl = xdg_surface_get_toplevel(W.xsurf);
	if (!W.toplvl) {
		snprintf(err, errlen, "cannot create an xdg_toplevel");
		return false;
	}
	xdg_toplevel_add_listener(W.toplvl, &toplvl_listener, NULL);
	wl_title_changed();

	if (W.vpm && W.fsm) {
		W.vp = wp_viewporter_get_viewport(W.vpm, W.surf);
		W.fs = wp_fractional_scale_manager_v1_get_fractional_scale(W.fsm, W.surf);
		if (W.vp && W.fs)
			wp_fractional_scale_v1_add_listener(W.fs, &fs_listener, NULL);
	}
	if (W.deco_mgr) {
		W.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(W.deco_mgr, W.toplvl);
		if (W.deco)
			zxdg_toplevel_decoration_v1_unset_mode(W.deco);
	}

	W.can_redraw = true;
	wl_surface_commit(W.surf);
	return true;
}

void wl_teardown(void)
{
	int i;

	paste_reset();
	for (i = 0; i < MAX_SENDS; i++)
		free_send(&W.sends[i]);
	render_free();
	free(W.osc_title);
	W.osc_title = NULL;

	if (W.d_src)
		wl_data_source_destroy(W.d_src);
	if (W.ps_src)
		zwp_primary_selection_source_v1_destroy(W.ps_src);
	free(W.d_text);
	free(W.ps_text);
	if (W.d_offer)
		wl_data_offer_destroy(W.d_offer);
	if (W.ps_offer)
		zwp_primary_selection_offer_v1_destroy(W.ps_offer);
	if (W.dd)
		wl_data_device_release(W.dd);
	if (W.psd)
		zwp_primary_selection_device_v1_destroy(W.psd);
	if (W.ptr)
		wl_pointer_release(W.ptr);
	if (W.kbd)
		wl_keyboard_release(W.kbd);
	if (W.cursor_surf)
		wl_surface_destroy(W.cursor_surf);
	if (W.cursor_buf)
		wl_buffer_destroy(W.cursor_buf);
	if (W.cursor_theme)
		wl_cursor_theme_destroy(W.cursor_theme);
	if (W.deco)
		zxdg_toplevel_decoration_v1_destroy(W.deco);
	if (W.fs)
		wp_fractional_scale_v1_destroy(W.fs);
	if (W.vp)
		wp_viewport_destroy(W.vp);
	if (W.toplvl)
		xdg_toplevel_destroy(W.toplvl);
	if (W.xsurf)
		xdg_surface_destroy(W.xsurf);
	if (W.surf)
		wl_surface_destroy(W.surf);
	if (W.deco_mgr)
		zxdg_decoration_manager_v1_destroy(W.deco_mgr);
	if (W.fsm)
		wp_fractional_scale_manager_v1_destroy(W.fsm);
	if (W.vpm)
		wp_viewporter_destroy(W.vpm);
	if (W.ps_dm)
		zwp_primary_selection_device_manager_v1_destroy(W.ps_dm);
	if (W.data_dm)
		wl_data_device_manager_destroy(W.data_dm);
	if (W.seat)
		wl_seat_destroy(W.seat);
	if (W.wm_base)
		xdg_wm_base_destroy(W.wm_base);
	if (W.shm)
		wl_shm_destroy(W.shm);
	if (W.cp)
		wl_compositor_destroy(W.cp);
	if (W.registry)
		wl_registry_destroy(W.registry);
	if (W.display) {
		wl_display_flush(W.display);
		wl_display_disconnect(W.display);
	}
	xkb_compose_state_unref(W.compose_state);
	xkb_compose_table_unref(W.compose_table);
	xkb_state_unref(W.xkb_state);
	xkb_keymap_unref(W.xkb_keymap);
	xkb_context_unref(W.xkb_ctx);
	memset(&W.buf, 0, sizeof(W.buf));
}

