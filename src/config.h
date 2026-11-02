/* config.h - the Python configuration, read by a short-lived python3 helper */
#ifndef WATE_CONFIG_H
#define WATE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
	MOD_SHIFT = 1,
	MOD_CTRL = 2,
	MOD_ALT = 4,
	MOD_SUPER = 8,
};

enum key_action {
	ACT_COPY,
	ACT_PASTE,
	ACT_PASTE_PRIMARY,
	ACT_FONTSIZE,
	ACT_FONTSIZE_RESET,
	ACT_SCROLL_UP,
	ACT_SCROLL_DOWN,
	ACT_SCROLL_PAGE_UP,
	ACT_SCROLL_PAGE_DOWN,
	ACT_SCROLL_TOP,
	ACT_SCROLL_BOTTOM,
	ACT_RESET,
	ACT_NEW_TERMINAL,
	ACT_NONE,
};

enum cursor_style {
	CURSOR_BLOCK,
	CURSOR_BEAM,
	CURSOR_UNDERLINE,
};

struct key_binding {
	unsigned mods;
	char *key;
	enum key_action action;
	int arg;
};

enum mouse_btn {
	MB_LEFT,
	MB_MIDDLE,
	MB_RIGHT,
	MB_BACK,
	MB_FORWARD,
	MB_WHEEL_UP,
	MB_WHEEL_DOWN,
};

struct button_binding {
	unsigned mods;
	enum mouse_btn button;
	enum key_action action;
	int arg;
};

struct config {
	char *font;
	char *shell;
	char *term;
	char *title;
	char *app_id;
	int pad_x, pad_y;
	int cols, rows;
	int scrollback;
	double alpha;
	bool hide_when_typing;
	bool process_saver;
	enum cursor_style cursor_style;
	bool has_cursor_color;
	uint32_t cursor_color;
	uint32_t fg, bg;
	uint32_t palette[16];
	bool has_sel_fg, has_sel_bg;
	uint32_t sel_fg, sel_bg;
	uint32_t urls;
	struct key_binding *keys;
	size_t nkeys;
	struct button_binding *buttons;
	size_t nbuttons;
};

void config_defaults(struct config *c);
void config_free(struct config *c);
void config_copy(struct config *dst, const struct config *src);
bool config_equal(const struct config *a, const struct config *b);

char *config_default_path(void);

void config_seed(const char *path);

struct cfg_job;

struct cfg_job *cfg_job_start(const char *path, char *err, size_t errlen);
int cfg_job_fd(const struct cfg_job *j);
int cfg_job_read(struct cfg_job *j);
bool cfg_job_expired(const struct cfg_job *j);
void cfg_job_kill(struct cfg_job *j);
int cfg_job_finish(struct cfg_job *j, struct config *out, bool strict, char **msg);

int config_load(const char *path, struct config *out, bool strict, char **msg);

#endif

