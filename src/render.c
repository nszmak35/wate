/* render.c - geometry, shm buffers and drawing */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "boxdraw.h"
#include "wate.h"

#define MAX_DIM 16384

static inline uint32_t rgb3(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t premul(uint32_t rgb, unsigned alpha)
{
	unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;

	if (alpha >= 255)
		return 0xff000000u | rgb;
	r = (r * alpha + 127) / 255;
	g = (g * alpha + 127) / 255;
	b = (b * alpha + 127) / 255;
	return (alpha << 24) | (r << 16) | (g << 8) | b;
}

static void fill_rect(struct buffer *b, int x0, int y0, int x1, int y1, uint32_t argb)
{
	int x, y;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > b->w)
		x1 = b->w;
	if (y1 > b->h)
		y1 = b->h;
	for (y = y0; y < y1; y++) {
		uint32_t *row = b->px + (size_t)y * b->w;

		for (x = x0; x < x1; x++)
			row[x] = argb;
	}
}

static inline uint32_t over(uint32_t dst, uint32_t rgb, unsigned c)
{
	unsigned out = 0, s;

	for (s = 0; s < 32; s += 8) {
		unsigned d = (dst >> s) & 0xff;
		unsigned sc = s == 24 ? 255 : (rgb >> s) & 0xff;

		out |= ((d * (255 - c) + sc * c + 127) / 255) << s;
	}
	return out;
}

static void blit(struct buffer *b, const uint8_t *cov, int w, int h, int ox, int oy,
		 int cx0, int cy0, int cx1, int cy1, uint32_t rgb)
{
	int x, y, xa, xb, ya, yb;

	xa = ox > cx0 ? ox : cx0;
	ya = oy > cy0 ? oy : cy0;
	xb = ox + w < cx1 ? ox + w : cx1;
	yb = oy + h < cy1 ? oy + h : cy1;
	if (xa < 0)
		xa = 0;
	if (ya < 0)
		ya = 0;
	if (xb > b->w)
		xb = b->w;
	if (yb > b->h)
		yb = b->h;
	for (y = ya; y < yb; y++) {
		const uint8_t *src = cov + (size_t)(y - oy) * w;
		uint32_t *dst = b->px + (size_t)y * b->w;

		for (x = xa; x < xb; x++) {
			unsigned c = src[x - ox];

			if (c == 0)
				continue;
			dst[x] = c == 255 ? (0xff000000u | rgb) : over(dst[x], rgb, c);
		}
	}
}

struct drawctx {
	struct buffer *b;
	int minx, miny, maxx, maxy;
	bool any;
};

static unsigned bg_alpha(void)
{
	double a = W.cfg.alpha;

	if (a >= 1.0)
		return 255;
	if (a <= 0.0)
		return 0;
	return (unsigned)(a * 255.0 + 0.5);
}

static void draw_cell(struct tsm_screen *con, uint32_t id, const uint32_t *ch, size_t len,
		      int width, int x, int y, const struct tsm_screen_attr *a,
		      tsm_age_t age, void *data)
{
	struct drawctx *c = data;
	struct buffer *b = c->b;
	int cw = W.fm.cell_w, chh = W.fm.cell_h;
	int px, py, pw, i;
	uint32_t fg, bg, cell_bg, text_fg, cur_color;
	unsigned alpha = 255;
	bool translucent_ok;

	(void)con;
	(void)id;
	if (width <= 0)
		return;
	if (age && age <= b->age)
		return;
	if (x < 0 || y < 0 || x >= W.cols || y >= W.rows)
		return;

	px = W.padx + x * cw;
	py = W.pady + y * chh;
	pw = width * cw;
	if (px >= b->w || py >= b->h)
		return;

	fg = rgb3(a->fr, a->fg, a->fb);
	bg = rgb3(a->br, a->bg, a->bb);
	translucent_ok = !a->inverse && !a->selected && a->bccode == TSM_COLOR_BACKGROUND;
	if (a->inverse) {
		uint32_t t = fg;

		fg = bg;
		bg = t;
	}
	if (a->selected) {
		if (!W.cfg.has_sel_fg && !W.cfg.has_sel_bg) {
			uint32_t t = fg;

			fg = bg;
			bg = t;
		} else {
			if (W.cfg.has_sel_fg)
				fg = W.cfg.sel_fg;
			if (W.cfg.has_sel_bg)
				bg = W.cfg.sel_bg;
		}
	}

	cur_color = W.cfg.has_cursor_color ? W.cfg.cursor_color : fg;
	cell_bg = bg;
	text_fg = fg;
	if (a->cursor && W.focused && W.cfg.cursor_style == CURSOR_BLOCK) {
		cell_bg = cur_color;
		text_fg = bg;
		translucent_ok = false;
	}
	if (translucent_ok)
		alpha = bg_alpha();

	fill_rect(b, px, py, px + pw, py + chh, premul(cell_bg, alpha));

	for (i = 0; i < (int)len; i++) {
		uint32_t cp = ch[i];
		const struct glyph *g;

		if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
			continue;
		if (i == 0 && W.scratch && (size_t)pw * chh <= W.scratch_size &&
		    boxdraw(cp, W.scratch, pw, chh)) {
			blit(b, W.scratch, pw, chh, px, py, px, py, px + pw, py + chh, text_fg);
			continue;
		}
		g = font_glyph(W.font, cp, a->bold);
		if (!g || !g->data)
			continue;
		blit(b, g->data, g->w, g->h,
		     px + g->left + (i > 0 && g->advance == 0 ? pw : 0),
		     py + W.fm.ascent - g->top, px, py, px + pw, py + chh, text_fg);
	}

	if (a->underline)
		fill_rect(b, px, py + W.fm.underline_pos, px + pw,
			  py + W.fm.underline_pos + W.fm.underline_thick, premul(text_fg, 255));

	if (a->cursor) {
		int t = (2 * W.scale + 60) / 120;
		uint32_t cc = premul(cur_color, 255);

		if (t < 1)
			t = 1;
		if (!W.focused) {
			fill_rect(b, px, py, px + pw, py + t, cc);
			fill_rect(b, px, py + chh - t, px + pw, py + chh, cc);
			fill_rect(b, px, py, px + t, py + chh, cc);
			fill_rect(b, px + pw - t, py, px + pw, py + chh, cc);
		} else if (W.cfg.cursor_style == CURSOR_BEAM) {
			fill_rect(b, px, py, px + t, py + chh, cc);
		} else if (W.cfg.cursor_style == CURSOR_UNDERLINE) {
			fill_rect(b, px, py + chh - t, px + pw, py + chh, cc);
		}
	}

	if (!c->any || px < c->minx)
		c->minx = px;
	if (!c->any || py < c->miny)
		c->miny = py;
	if (!c->any || px + pw > c->maxx)
		c->maxx = px + pw;
	if (!c->any || py + chh > c->maxy)
		c->maxy = py + chh;
	c->any = true;
}

int make_shm_fd(size_t size)
{
	int fd = memfd_create("wate-buffer", MFD_CLOEXEC);

	if (fd < 0) {
		const char *dir = getenv("XDG_RUNTIME_DIR");
		char path[512];

		if (!dir || !*dir)
			return -1;
		snprintf(path, sizeof(path), "%s/wate-XXXXXX", dir);
		fd = mkostemp(path, O_CLOEXEC);
		if (fd < 0)
			return -1;
		unlink(path);
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct buffer *b = data;

	(void)wl;
	b->busy = false;
}

static const struct wl_buffer_listener buf_listener = {
	.release = buf_release,
};

static void buffer_destroy(struct buffer *b)
{
	if (b->wl)
		wl_buffer_destroy(b->wl);
	if (b->px)
		munmap(b->px, b->size);
	memset(b, 0, sizeof(*b));
}

static bool buffer_create(struct buffer *b, int w, int h)
{
	size_t size;
	int fd;
	void *map;
	struct wl_shm_pool *pool;

	if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM)
		return false;
	size = (size_t)w * h * 4;
	if (size > (size_t)1 << 30)
		return false;
	fd = make_shm_fd(size);
	if (fd < 0)
		return false;
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return false;
	}
	pool = wl_shm_create_pool(W.shm, fd, (int32_t)size);
	close(fd);
	if (!pool) {
		munmap(map, size);
		return false;
	}
	memset(b, 0, sizeof(*b));
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	if (!b->wl) {
		munmap(map, size);
		memset(b, 0, sizeof(*b));
		return false;
	}
	b->px = map;
	b->size = size;
	b->w = w;
	b->h = h;
	b->age = 0;
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	return true;
}

#define SPARE_BUFFER_MS 3000

/* The second buffer exists only while the compositor holds the first. */
static struct buffer *pick_buffer(void)
{
	long long now = now_ms();
	int i;

	for (i = 0; i < 2; i++)
		if (!W.buf[i].busy && W.buf[i].wl && (W.buf[i].w != W.bw || W.buf[i].h != W.bh))
			buffer_destroy(&W.buf[i]);

	for (i = 0; i < 2; i++) {
		struct buffer *b = &W.buf[i];

		if (b->busy)
			continue;
		if (!b->wl && !buffer_create(b, W.bw, W.bh)) {
			fprintf(stderr, "wate: cannot allocate a %dx%d buffer\n", W.bw, W.bh);
			W.retry_at = now + 1000;
			return NULL;
		}
		if (i == 1)
			W.buf1_used = now;
		else if (W.buf[1].wl && !W.buf[1].busy && now - W.buf1_used > SPARE_BUFFER_MS)
			buffer_destroy(&W.buf[1]);
		return b;
	}
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	(void)data;
	(void)time;
	wl_callback_destroy(cb);
	if (W.frame_cb == cb)
		W.frame_cb = NULL;
	W.can_redraw = true;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

bool render_can_draw(void)
{
	long long now;
	int i;
	bool free_buf = false;

	if (!W.configured || !W.surf || W.bw <= 0 || W.bh <= 0)
		return false;
	now = now_ms();
	if (W.retry_at) {
		if (now < W.retry_at)
			return false;
		W.retry_at = 0;
	}
	if (!W.can_redraw) {
		if (W.frame_deadline && now >= W.frame_deadline)
			W.can_redraw = true;
		else
			return false;
	}
	for (i = 0; i < 2; i++)
		if (!W.buf[i].busy)
			free_buf = true;
	return free_buf;
}

static long long spare_expiry(void)
{
	if (W.buf[1].wl && !W.buf[1].busy)
		return W.buf1_used + SPARE_BUFFER_MS;
	return -1;
}

/* frees the idle second buffer */
void render_trim(void)
{
	long long e = spare_expiry();

	if (e >= 0 && now_ms() >= e)
		buffer_destroy(&W.buf[1]);
}

long long render_deadline(void)
{
	long long d = spare_expiry(), r = -1;

	if (W.need_redraw && W.configured) {
		if (W.retry_at && W.retry_at > now_ms())
			r = W.retry_at;
		else if (!W.can_redraw && W.frame_deadline)
			r = W.frame_deadline;
	}
	if (r >= 0 && (d < 0 || r < d))
		d = r;
	return d;
}

void render_set_opaque(void)
{
	if (!W.surf)
		return;
	if (W.cfg.alpha >= 1.0 && W.bw > 0) {
		struct wl_region *r = wl_compositor_create_region(W.cp);
		int dw = W.conf_w > 0 ? W.conf_w : (int)(((long long)W.bw * 120 + W.scale / 2) / W.scale);
		int dh = W.conf_h > 0 ? W.conf_h : (int)(((long long)W.bh * 120 + W.scale / 2) / W.scale);

		if (r) {
			wl_region_add(r, 0, 0, dw, dh);
			wl_surface_set_opaque_region(W.surf, r);
			wl_region_destroy(r);
		}
	} else {
		wl_surface_set_opaque_region(W.surf, NULL);
	}
}

void render_redraw(bool force)
{
	struct buffer *b;
	struct drawctx ctx;
	bool fresh;
	tsm_age_t age;

	if (!W.configured || !W.surf || W.bw <= 0 || W.bh <= 0 || !W.font)
		return;
	if (!force && !render_can_draw())
		return;
	b = pick_buffer();
	if (!b)
		return;

	if (W.full_redraw) {
		W.buf[0].age = 0;
		W.buf[1].age = 0;
		W.full_redraw = false;
	}
	fresh = b->age == 0;
	if (fresh)
		fill_rect(b, 0, 0, b->w, b->h, premul(W.cfg.bg, bg_alpha()));

	memset(&ctx, 0, sizeof(ctx));
	ctx.b = b;
	age = tsm_screen_draw(W.screen, draw_cell, &ctx);
	b->age = age;

	if (!fresh && !ctx.any && !force) {
		W.need_redraw = false;
		return;
	}

	wl_surface_attach(W.surf, b->wl, 0, 0);
	if (fresh)
		wl_surface_damage_buffer(W.surf, 0, 0, b->w, b->h);
	else if (ctx.any)
		wl_surface_damage_buffer(W.surf, ctx.minx, ctx.miny,
					 ctx.maxx - ctx.minx, ctx.maxy - ctx.miny);
	if (W.can_redraw || !W.frame_cb) {
		if (W.frame_cb)
			wl_callback_destroy(W.frame_cb);
		W.frame_cb = wl_surface_frame(W.surf);
		if (W.frame_cb)
			wl_callback_add_listener(W.frame_cb, &frame_listener, NULL);
		W.can_redraw = false;
		W.frame_deadline = now_ms() + 100;
	}
	wl_surface_commit(W.surf);
	b->busy = true;
	W.need_redraw = false;
}

void render_layout(void)
{
	int scale = W.scale > 0 ? W.scale : 120;
	int padx = (W.cfg.pad_x * scale + 60) / 120;
	int pady = (W.cfg.pad_y * scale + 60) / 120;
	int cw = W.fm.cell_w, ch = W.fm.cell_h;
	int bw, bh, cols, rows, dw, dh;
	bool size_changed, grid_changed;

	if (cw <= 0 || ch <= 0)
		return;
	bw = W.conf_w > 0 ? (int)(((long long)W.conf_w * scale + 60) / 120) : W.cfg.cols * cw + 2 * padx;
	bh = W.conf_h > 0 ? (int)(((long long)W.conf_h * scale + 60) / 120) : W.cfg.rows * ch + 2 * pady;
	if (bw < cw)
		bw = cw;
	if (bh < ch)
		bh = ch;
	if (bw > MAX_DIM)
		bw = MAX_DIM;
	if (bh > MAX_DIM)
		bh = MAX_DIM;
	if (padx > (bw - cw) / 2)
		padx = (bw - cw) / 2;
	if (pady > (bh - ch) / 2)
		pady = (bh - ch) / 2;
	cols = (bw - 2 * padx) / cw;
	rows = (bh - 2 * pady) / ch;
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	if (cols > 2000)
		cols = 2000;
	if (rows > 2000)
		rows = 2000;

	size_changed = bw != W.bw || bh != W.bh || padx != W.padx || pady != W.pady;
	grid_changed = cols != W.cols || rows != W.rows;
	W.bw = bw;
	W.bh = bh;
	W.padx = padx;
	W.pady = pady;

	dw = W.conf_w > 0 ? W.conf_w : (int)(((long long)bw * 120 + scale / 2) / scale);
	dh = W.conf_h > 0 ? W.conf_h : (int)(((long long)bh * 120 + scale / 2) / scale);
	if (W.vp && dw > 0 && dh > 0)
		wp_viewport_set_destination(W.vp, dw, dh);

	if (grid_changed) {
		W.cols = cols;
		W.rows = rows;
		tsm_screen_resize(W.screen, cols, rows);
		pty_resize();
	}
	if (size_changed || grid_changed) {
		W.full_redraw = true;
		W.need_redraw = true;
		render_set_opaque();
	}
}

bool render_font_reload(void)
{
	char err[300];
	struct font *nf;
	const struct font_metrics *m;
	uint8_t *scratch;
	size_t ssize;
	struct font *old;

	nf = font_open(W.cfg.font, W.zoom, W.scale > 0 ? W.scale : 120, err, sizeof(err));
	if (!nf) {
		fprintf(stderr, "wate: %s\n", err);
		return false;
	}
	m = font_metrics(nf);
	ssize = (size_t)2 * m->cell_w * m->cell_h;
	scratch = malloc(ssize);
	if (!scratch) {
		font_close(nf);
		fprintf(stderr, "wate: out of memory\n");
		return false;
	}
	old = W.font;
	W.font = nf;
	W.fm = *m;
	free(W.scratch);
	W.scratch = scratch;
	W.scratch_size = ssize;
	font_close(old);
	mem_trim();
	W.full_redraw = true;
	W.need_redraw = true;
	return true;
}

static void set_default(int idx, uint32_t rgb)
{
	tsm_set_default_color(idx, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
	if (W.screen)
		tsm_screen_set_color(W.screen, idx, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff,
				     rgb & 0xff);
}

void render_palette(void)
{
	int i;

	for (i = 0; i < 16; i++)
		set_default(i, W.cfg.palette[i]);
	tsm_set_default_foreground((W.cfg.fg >> 16) & 0xff, (W.cfg.fg >> 8) & 0xff, W.cfg.fg & 0xff);
	tsm_set_default_background((W.cfg.bg >> 16) & 0xff, (W.cfg.bg >> 8) & 0xff, W.cfg.bg & 0xff);
	if (W.screen) {
		tsm_screen_set_color(W.screen, TSM_COLOR_FOREGROUND, (W.cfg.fg >> 16) & 0xff,
				     (W.cfg.fg >> 8) & 0xff, W.cfg.fg & 0xff);
		tsm_screen_set_color(W.screen, TSM_COLOR_BACKGROUND, (W.cfg.bg >> 16) & 0xff,
				     (W.cfg.bg >> 8) & 0xff, W.cfg.bg & 0xff);
	}
	W.full_redraw = true;
	W.need_redraw = true;
}

void render_free(void)
{
	buffer_destroy(&W.buf[0]);
	buffer_destroy(&W.buf[1]);
	if (W.frame_cb) {
		wl_callback_destroy(W.frame_cb);
		W.frame_cb = NULL;
	}
	font_close(W.font);
	W.font = NULL;
	free(W.scratch);
	W.scratch = NULL;
}

