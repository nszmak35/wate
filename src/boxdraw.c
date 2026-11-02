/* boxdraw.c - procedurally drawn box, block and braille characters */

#include <string.h>

#include "boxdraw.h"

static const char arms[128][5] = {
	 "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022",
	 "1100", "2200", "0011", "0022", "0101", "0201", "0102", "0202",
	 "1001", "2001", "1002", "2002", "0110", "0210", "0120", "0220",
	 "1010", "2010", "1020", "2020", "0111", "0211", "0121", "0112",
	 "0122", "0221", "0212", "0222", "1011", "2011", "1021", "1012",
	 "1022", "2021", "2012", "2022", "1101", "2101", "1201", "2201",
	 "1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210",
	 "1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211",
	 "1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221",
	 "2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022",
	 "3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003",
	 "0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133",
	 "0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310",
	 "1130", "3330", "3311", "1133", "3333", "0101", "1001", "1010",
	 "0110", "0000", "0000", "0000", "1000", "0010", "0100", "0001",
	 "2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021",
};

static void fill(uint8_t *cov, int w, int h, int x0, int y0, int x1, int y1,
		 uint8_t v)
{
	int x, y;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > w)
		x1 = w;
	if (y1 > h)
		y1 = h;
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			cov[y * w + x] = v;
}

static int bundle(int style, int light, int heavy)
{
	switch (style) {
	case 1:
		return light;
	case 2:
		return heavy;
	case 3:
		return 3 * light;
	}
	return 0;
}

static int imax(int a, int b)
{
	return a > b ? a : b;
}

struct axis {
	uint8_t *cov;
	int w, h;
	bool horizontal;
};

static void put(const struct axis *ax, int a0, int a1, int c0, int c1)
{
	if (ax->horizontal)
		fill(ax->cov, ax->w, ax->h, a0, c0, a1, c1, 255);
	else
		fill(ax->cov, ax->w, ax->h, c0, a0, c1, a1, 255);
}

static void draw_axis(const struct axis *ax, int len, int cross, int sa, int sb,
		      int pa, int pb, int light, int heavy)
{
	int pt = imax(bundle(pa, light, heavy), bundle(pb, light, heavy));
	int at = imax(bundle(sa, light, heavy), bundle(sb, light, heavy));
	int p0, p1, c0, s;

	if (pt == 0)
		pt = imax(bundle(sa, light, heavy), bundle(sb, light, heavy));
	if (at == 0)
		return;
	p0 = (len - pt) / 2;
	p1 = p0 + pt;
	c0 = (cross - at) / 2;

	for (s = 0; s < 2; s++) {
		int style = s ? sb : sa;
		int arm_style_perp_near = s ? pb : pa;
		int from, to;

		(void)arm_style_perp_near;
		if (style == 0)
			continue;
		if (s == 0) {
			from = 0;
			to = p1;
		} else {
			from = p0;
			to = len;
		}
		if (style == 3) {
			int strokes[2];
			int k;

			strokes[0] = c0;
			strokes[1] = c0 + 2 * light;
			for (k = 0; k < 2; k++) {
				int perp = k ? pb : pa;
				int f = from, t = to;

				if (perp == 3) {
					if (s == 0)
						t = p0 + light;
					else
						f = p1 - light;
				} else if (perp != 0) {
					if (s == 0)
						t = p0;
					else
						f = p1;
				}
				put(ax, f, t, strokes[k], strokes[k] + light);
			}
		} else {
			int t = bundle(style, light, heavy);
			int cs = (cross - t) / 2;

			put(ax, from, to, cs, cs + t);
		}
	}
}

static bool lines(uint32_t cp, uint8_t *cov, int w, int h)
{
	const char *a = arms[cp - 0x2500];
	int l = a[0] - '0', r = a[1] - '0', u = a[2] - '0', d = a[3] - '0';
	int light, heavy;
	struct axis hx = { cov, w, h, true };
	struct axis vx = { cov, w, h, false };

	if (!(l | r | u | d))
		return false;
	light = (h + 8) / 16;
	if (light < 1)
		light = 1;
	if (light > w / 3 && w / 3 >= 1)
		light = w / 3;
	heavy = light * 2;
	if (heavy < 2)
		heavy = 2;

	memset(cov, 0, (size_t)w * h);
	draw_axis(&hx, w, h, l, r, u, d, light, heavy);
	draw_axis(&vx, h, w, u, d, l, r, light, heavy);
	return true;
}

static bool blocks(uint32_t cp, uint8_t *cov, int w, int h)
{
	int x, y;

	memset(cov, 0, (size_t)w * h);
	if (cp == 0x2580) {
		fill(cov, w, h, 0, 0, w, h / 2, 255);
	} else if (cp >= 0x2581 && cp <= 0x2588) {
		int n = (int)(cp - 0x2580);

		fill(cov, w, h, 0, h - (h * n + 4) / 8, w, h, 255);
	} else if (cp >= 0x2589 && cp <= 0x258f) {
		int n = (int)(0x2590 - cp);

		fill(cov, w, h, 0, 0, (w * n + 4) / 8, h, 255);
	} else if (cp == 0x2590) {
		fill(cov, w, h, w / 2, 0, w, h, 255);
	} else if (cp >= 0x2591 && cp <= 0x2593) {
		fill(cov, w, h, 0, 0, w, h, (uint8_t)(64 * (cp - 0x2590)));
	} else if (cp == 0x2594) {
		fill(cov, w, h, 0, 0, w, (h + 4) / 8 ? (h + 4) / 8 : 1, 255);
	} else if (cp == 0x2595) {
		int t = (w + 4) / 8 ? (w + 4) / 8 : 1;

		fill(cov, w, h, w - t, 0, w, h, 255);
	} else if (cp >= 0x2596 && cp <= 0x259f) {
		static const uint8_t q[10] = {
			0x4, 0x8, 0x1, 0xd, 0x9, 0x7, 0xb, 0x2, 0x6, 0xe,
		};
		uint8_t m = q[cp - 0x2596];

		for (y = 0; y < 2; y++)
			for (x = 0; x < 2; x++)
				if (m & (1 << (y * 2 + x)))
					fill(cov, w, h, x * w / 2, y * h / 2,
					     (x + 1) * w / 2 + (x ? w % 2 : 0),
					     (y + 1) * h / 2 + (y ? h % 2 : 0), 255);
		if (m & 0x2)
			fill(cov, w, h, w / 2, 0, w, h / 2, 255);
	} else {
		return false;
	}
	return true;
}

static bool braille(uint32_t cp, uint8_t *cov, int w, int h)
{
	static const int col[8] = { 0, 0, 0, 1, 1, 1, 0, 1 };
	static const int row[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
	unsigned bits = cp - 0x2800;
	int i;
	int cw = w / 2, rh = h / 4;
	int dw = cw * 3 / 5, dh = rh * 3 / 5;

	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	memset(cov, 0, (size_t)w * h);
	for (i = 0; i < 8; i++) {
		int x0, y0;

		if (!(bits & (1u << i)))
			continue;
		x0 = col[i] * (w - cw) + (cw - dw) / 2;
		if (!col[i])
			x0 = (cw - dw) / 2;
		y0 = row[i] * rh + (rh - dh) / 2;
		fill(cov, w, h, x0, y0, x0 + dw, y0 + dh, 255);
	}
	return true;
}

bool boxdraw(uint32_t cp, uint8_t *cov, int w, int h)
{
	if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
		return false;
	if (cp >= 0x2500 && cp <= 0x257f)
		return lines(cp, cov, w, h);
	if (cp >= 0x2580 && cp <= 0x259f)
		return blocks(cp, cov, w, h);
	if (cp >= 0x2800 && cp <= 0x28ff)
		return braille(cp, cov, w, h);
	return false;
}

