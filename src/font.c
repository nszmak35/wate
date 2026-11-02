/* font.c - FreeType glyphs; fontconfig is used only to list font files, then shut down */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"

#define MAX_FACES 24
#define HASH_MIN 256u
#define HASH_MAX 4096u
#define MAX_ENTRIES 3000

struct slot {
	char *path;
	int index;
	FT_Face face;
	bool tried;
	bool usable;
};

struct entry {
	uint64_t key;
	bool used;
	bool has_glyph;
	struct glyph g;
};

struct font {
	FT_Library ft;
	struct slot reg[MAX_FACES];
	int nreg;
	struct slot bold[MAX_FACES];
	int nbold;
	double base_size;
	double px;
	struct font_metrics m;

	struct entry *table;
	unsigned cap;
	unsigned nentries;
};

static void set_err(char *err, size_t len, const char *fmt, const char *a)
{
	if (err && len)
		snprintf(err, len, fmt, a ? a : "");
}

static void free_slot(struct slot *s)
{
	if (s->face)
		FT_Done_Face(s->face);
	free(s->path);
	memset(s, 0, sizeof(*s));
}

static void clear_cache(struct font *f)
{
	unsigned i;

	if (!f->table)
		return;
	for (i = 0; i < f->cap; i++)
		if (f->table[i].used)
			free(f->table[i].g.data);
	memset(f->table, 0, f->cap * sizeof(*f->table));
	f->nentries = 0;
}

static int list_add(struct slot *list, int n, const char *path, int index)
{
	int i;

	for (i = 0; i < n; i++)
		if (list[i].index == index && strcmp(list[i].path, path) == 0)
			return n;
	list[n].path = strdup(path);
	if (!list[n].path)
		return n;
	list[n].index = index;
	return n + 1;
}

static int build_list(struct slot *list, FcPattern *pattern, bool want_bold)
{
	FcPattern *p = FcPatternDuplicate(pattern);
	FcFontSet *set;
	FcCharSet *cs = NULL;
	FcResult res;
	int i, n = 0;

	if (!p)
		return 0;
	if (want_bold) {
		FcPatternDel(p, FC_WEIGHT);
		FcPatternAddInteger(p, FC_WEIGHT, FC_WEIGHT_BOLD);
	}
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);

	set = FcFontSort(NULL, p, FcTrue, &cs, &res);
	if (set) {
		for (i = 0; i < set->nfont && n < MAX_FACES; i++) {
			FcChar8 *file = NULL;
			int index = 0;
			FcBool scalable = FcTrue;

			if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !file)
				continue;
			FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
			FcPatternGetBool(set->fonts[i], FC_SCALABLE, 0, &scalable);
			if (!scalable)
				continue;
			n = list_add(list, n, (const char *)file, index);
		}
		FcFontSetDestroy(set);
	}
	if (cs)
		FcCharSetDestroy(cs);
	FcPatternDestroy(p);
	return n;
}

static FT_Face open_slot(struct font *f, struct slot *s)
{
	FT_Face face;

	if (s->tried)
		return s->usable ? s->face : NULL;
	s->tried = true;
	if (FT_New_Face(f->ft, s->path, s->index, &face) != 0)
		return NULL;
	if (!FT_IS_SCALABLE(face) ||
	    FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(f->px * 64.0), 72, 72) != 0) {
		FT_Done_Face(face);
		return NULL;
	}
	s->face = face;
	s->usable = true;
	return face;
}

static uint8_t *copy_bitmap(const FT_Bitmap *b)
{
	size_t n = (size_t)b->width * b->rows;
	uint8_t *out;
	unsigned x, y;

	if (n == 0)
		return NULL;
	out = malloc(n);
	if (!out)
		return NULL;
	for (y = 0; y < b->rows; y++) {
		const uint8_t *src = b->buffer + (long)y * b->pitch;
		uint8_t *dst = out + (size_t)y * b->width;

		if (b->pitch < 0)
			src = b->buffer + (long)(b->rows - 1 - y) * -b->pitch;
		if (b->pixel_mode == FT_PIXEL_MODE_GRAY) {
			memcpy(dst, src, b->width);
		} else if (b->pixel_mode == FT_PIXEL_MODE_MONO) {
			for (x = 0; x < b->width; x++)
				dst[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
		} else {
			free(out);
			return NULL;
		}
	}
	return out;
}

static bool render(FT_Face face, FT_UInt gi, struct glyph *g)
{
	FT_GlyphSlot slot;

	if (FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER) != 0)
		return false;
	slot = face->glyph;
	if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
	    slot->bitmap.pixel_mode != FT_PIXEL_MODE_MONO)
		return false;
	if (slot->bitmap.width > 4096 || slot->bitmap.rows > 4096)
		return false;
	g->w = (int)slot->bitmap.width;
	g->h = (int)slot->bitmap.rows;
	g->left = slot->bitmap_left;
	g->advance = (int)((slot->advance.x + 32) >> 6);
	g->top = slot->bitmap_top;
	g->data = copy_bitmap(&slot->bitmap);
	if (g->w * g->h > 0 && !g->data)
		return false;
	return true;
}

static bool lookup_in(struct font *f, struct slot *list, int n, uint32_t cp,
		      struct glyph *out)
{
	int i;

	for (i = 0; i < n; i++) {
		FT_Face face = open_slot(f, &list[i]);
		FT_UInt gi;

		if (!face)
			continue;
		gi = FT_Get_Char_Index(face, cp);
		if (gi == 0)
			continue;
		if (render(face, gi, out))
			return true;
	}
	return false;
}

static bool measure(struct font *f)
{
	FT_Face face;
	FT_Size_Metrics *sm;
	int asc, desc, adv;
	FT_UInt gi;

	face = f->nreg ? open_slot(f, &f->reg[0]) : NULL;
	if (!face)
		return false;
	sm = &face->size->metrics;
	asc = (int)((sm->ascender + 63) >> 6);
	desc = (int)((-sm->descender + 63) >> 6);
	if (asc <= 0 || asc + desc <= 0)
		return false;

	gi = FT_Get_Char_Index(face, 'M');
	if (gi == 0 || FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT) != 0)
		return false;
	adv = (int)((face->glyph->advance.x + 32) >> 6);
	if (adv <= 0)
		return false;

	f->m.cell_w = adv;
	f->m.cell_h = asc + desc;
	f->m.ascent = asc;
	f->m.underline_thick = (int)(f->m.cell_h / 18);
	if (f->m.underline_thick < 1)
		f->m.underline_thick = 1;
	f->m.underline_pos = asc + desc / 2;
	if (face->underline_position != 0) {
		long up = FT_MulFix(face->underline_position, sm->y_scale);
		int pos = asc - (int)(up >> 6);

		if (pos > asc && pos < asc + desc)
			f->m.underline_pos = pos;
	}
	if (f->m.underline_pos + f->m.underline_thick > f->m.cell_h)
		f->m.underline_pos = f->m.cell_h - f->m.underline_thick;
	return true;
}

struct font *font_open(const char *pattern, double size_adjust, int scale120,
		       char *err, size_t errlen)
{
	struct font *f;
	FcPattern *pat;
	double size = 0, pixel = 0;
	double scale = scale120 > 0 ? scale120 / 120.0 : 1.0;

	if (!pattern || !*pattern)
		pattern = "monospace";

	f = calloc(1, sizeof(*f));
	if (!f) {
		set_err(err, errlen, "out of memory%s", NULL);
		return NULL;
	}
	f->cap = HASH_MIN;
	f->table = calloc(f->cap, sizeof(*f->table));
	if (!f->table || FT_Init_FreeType(&f->ft) != 0) {
		set_err(err, errlen, "cannot initialise FreeType%s", NULL);
		goto fail;
	}

	if (!FcInit()) {
		set_err(err, errlen, "cannot initialise fontconfig%s", NULL);
		goto fail;
	}
	pat = FcNameParse((const FcChar8 *)pattern);
	if (!pat) {
		set_err(err, errlen, "cannot parse font '%s'", pattern);
		FcFini();
		goto fail;
	}
	FcPatternGetDouble(pat, FC_SIZE, 0, &size);
	FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &pixel);
	if (size <= 0 && pixel > 0)
		size = pixel * 72.0 / 96.0;
	if (size <= 0)
		size = 11.0;
	f->base_size = size;
	size += size_adjust;
	if (size < 4.0)
		size = 4.0;
	if (size > 200.0)
		size = 200.0;
	f->px = size * 96.0 / 72.0 * scale;
	if (f->px < 4.0)
		f->px = 4.0;

	f->nreg = build_list(f->reg, pat, false);
	f->nbold = build_list(f->bold, pat, true);
	FcPatternDestroy(pat);
	FcFini();

	if (f->nreg == 0 || !measure(f)) {
		set_err(err, errlen, "cannot load font '%s'", pattern);
		goto fail;
	}
	return f;

fail:
	font_close(f);
	return NULL;
}

void font_close(struct font *f)
{
	int i;

	if (!f)
		return;
	clear_cache(f);
	free(f->table);
	for (i = 0; i < MAX_FACES; i++) {
		free_slot(&f->reg[i]);
		free_slot(&f->bold[i]);
	}
	if (f->ft)
		FT_Done_FreeType(f->ft);
	free(f);
}

const struct font_metrics *font_metrics(const struct font *f)
{
	return &f->m;
}

double font_base_size(const struct font *f)
{
	return f->base_size;
}

void font_trim(struct font *f)
{
	(void)f;
}

static unsigned hash_key(uint64_t k)
{
	k *= 0x9E3779B97F4A7C15ull;
	return (unsigned)(k >> 32);
}

static void table_grow(struct font *f)
{
	struct entry *nt;
	unsigned ncap = f->cap * 2, i, j;

	nt = calloc(ncap, sizeof(*nt));
	if (!nt)
		return;
	for (i = 0; i < f->cap; i++) {
		if (!f->table[i].used)
			continue;
		j = hash_key(f->table[i].key) & (ncap - 1);
		while (nt[j].used)
			j = (j + 1) & (ncap - 1);
		nt[j] = f->table[i];
	}
	free(f->table);
	f->table = nt;
	f->cap = ncap;
}

const struct glyph *font_glyph(struct font *f, uint32_t cp, bool bold)
{
	uint64_t key = ((uint64_t)(bold ? 1 : 0) << 32) | cp;
	unsigned h, i;
	struct entry *e;

	h = hash_key(key) & (f->cap - 1);
	for (i = 0; i < f->cap; i++) {
		e = &f->table[(h + i) & (f->cap - 1)];
		if (!e->used)
			break;
		if (e->key == key)
			return e->has_glyph ? &e->g : NULL;
	}

	if (f->nentries >= MAX_ENTRIES)
		clear_cache(f);
	else if (f->nentries * 2 >= f->cap && f->cap < HASH_MAX)
		table_grow(f);
	h = hash_key(key) & (f->cap - 1);
	for (i = 0; i < f->cap; i++) {
		e = &f->table[(h + i) & (f->cap - 1)];
		if (!e->used)
			break;
	}
	if (i == f->cap)
		return NULL;

	e->used = true;
	e->key = key;
	f->nentries++;
	memset(&e->g, 0, sizeof(e->g));
	e->has_glyph = false;
	if (bold && lookup_in(f, f->bold, f->nbold, cp, &e->g))
		e->has_glyph = true;
	else if (lookup_in(f, f->reg, f->nreg, cp, &e->g))
		e->has_glyph = true;
	return e->has_glyph ? &e->g : NULL;
}

