/* wate.h - shared state and interfaces */
#ifndef WATE_H
#define WATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include <sys/types.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "config.h"
#include "font.h"
#include "protocol/fractional-scale-v1.h"
#include "protocol/primary-selection-unstable-v1.h"
#include "protocol/viewporter.h"
#include "protocol/xdg-decoration-unstable-v1.h"
#include "protocol/xdg-shell.h"
#include "tsm/libtsm.h"

#ifndef WATE_VERSION
#define WATE_VERSION "0.1"
#endif

#define MAX_SENDS 8

struct buffer {
	struct wl_buffer *wl;
	uint32_t *px;
	size_t size;
	int w, h;
	bool busy;		/* held by the compositor */
	tsm_age_t age;
};

struct send_job {
	int fd;
	char *data;
	size_t len, off;
};

enum sel_state {
	SEL_NONE,
	SEL_PRESSED,
	SEL_DRAG,
	SEL_ACTIVE,
};

struct wate {
	struct {
		const char *config;
		const char *shell;
		const char *command;
		char **argv;
		bool hold;
		const char *title;
		const char *app_id;
		const char *display;
	} opt;

	struct config cfg;
	char *cfg_path;
	int zoom;
	bool die;
	int exit_code;

	int master;
	pid_t child;
	bool child_started, child_exited;
	int child_status;
	char *outq;
	size_t outlen, outcap;

	struct tsm_screen *screen;
	struct tsm_vte *vte;
	int cols, rows;

	struct font *font;
	struct font_metrics fm;
	uint8_t *scratch;
	size_t scratch_size;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *cp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_data_device_manager *data_dm;
	struct zwp_primary_selection_device_manager_v1 *ps_dm;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct wp_viewporter *vpm;
	struct wp_fractional_scale_manager_v1 *fsm;
	struct wl_surface *surf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *toplvl;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wp_viewport *vp;
	struct wp_fractional_scale_v1 *fs;
	bool shm_argb;
	bool configured;
	bool focused;
	int scale;
	int pend_w, pend_h;
	int conf_w, conf_h;
	int bw, bh;
	int padx, pady;
	struct buffer buf[2];
	struct wl_callback *frame_cb;
	bool can_redraw;
	long long frame_deadline;
	long long retry_at;
	bool need_redraw;
	bool full_redraw;

	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose_state;
	xkb_mod_index_t idx_shift, idx_ctrl, idx_alt, idx_logo;
	unsigned mods;
	struct {
		bool active;
		uint32_t key;
		xkb_keysym_t sym;
		uint32_t unicode;
		long long deadline;
		int interval_ms, delay_ms;
	} repeat;
	uint32_t last_serial;
	wl_fixed_t ptr_x, ptr_y;
	uint32_t ptr_serial;
	bool ptr_inside;
	bool cursor_hidden;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *cursor_text;
	struct wl_buffer *cursor_buf;
	int cursor_w, cursor_h, cursor_hx, cursor_hy;
	struct wl_surface *cursor_surf;
	double scroll_pending;
	double wheel_pending;
	unsigned bound_buttons;
	unsigned mouse_buttons;
	int mouse_col, mouse_row;
	bool compose_tried;
	char *osc_title;
	unsigned title_serial;
	long long buf1_used;
	enum sel_state sel;
	int click_count, click_x, click_y;
	long long click_time;

	struct wl_data_device *dd;
	struct zwp_primary_selection_device_v1 *psd;
	struct wl_data_offer *d_offer;
	const char *d_mime;
	struct zwp_primary_selection_offer_v1 *ps_offer;
	const char *ps_mime;
	struct wl_data_source *d_src;
	char *d_text;
	struct zwp_primary_selection_source_v1 *ps_src;
	char *ps_text;
	struct send_job sends[MAX_SENDS];
	struct {
		int fd;
		char *buf;
		size_t len, cap;
		bool primary;
		long long deadline;
	} paste;

	int watch_fd;
	char *watch_name[2];
	int nwatch;
	long long reload_deadline;
	struct cfg_job *job;
	bool reload_again;
};

extern struct wate W;

/* give freed heap memory back to the system */
#ifdef __GLIBC__
#include <malloc.h>
#define mem_trim() ((void)malloc_trim(0))
#else
#define mem_trim() ((void)0)
#endif

long long now_ms(void);
void pty_write(const char *data, size_t len);
void pty_resize(void);
void child_maybe_spawn(void);
void new_terminal(void);

bool render_font_reload(void);
void render_palette(void);
void render_layout(void);
bool render_can_draw(void);
void render_redraw(bool force);
int make_shm_fd(size_t size);
void render_trim(void);
long long render_deadline(void);
void render_set_opaque(void);
void render_free(void);

bool wl_setup(char *err, size_t errlen);
void wl_teardown(void);
void wl_scale_changed(void);
void wl_title_changed(void);
void wl_title_check(void);
void wl_repeat_tick(void);
void wl_hide_cursor(bool hide);
int wl_clipboard_fds(struct pollfd *fds, int max);
void wl_clipboard_dispatch(const struct pollfd *fds, int n);

#endif

