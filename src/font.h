/* font.h - fontconfig + FreeType fonts and glyphs */
#ifndef WATE_FONT_H
#define WATE_FONT_H

#include <stdbool.h>
#include <stdint.h>

struct font;

struct font_metrics {
	int cell_w;
	int cell_h;
	int ascent;
	int underline_pos;
	int underline_thick;
};

struct glyph {
	int w, h;
	int left, top;
	int advance;
	uint8_t *data;
};

struct font *font_open(const char *pattern, double size_adjust, int scale120,
		       char *err, size_t errlen);
void font_close(struct font *f);

const struct font_metrics *font_metrics(const struct font *f);

double font_base_size(const struct font *f);

const struct glyph *font_glyph(struct font *f, uint32_t cp, bool bold);

void font_trim(struct font *f);

#endif

