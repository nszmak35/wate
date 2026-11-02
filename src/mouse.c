/* mouse.c - xterm mouse reports */

#include <stdio.h>
#include <string.h>

#include "mouse.h"
#include "tsm/libtsm.h"

static size_t put_utf8(char *out, int v)
{
	if (v < 0x80) {
		out[0] = (char)v;
		return 1;
	}
	out[0] = (char)(0xc0 | (v >> 6));
	out[1] = (char)(0x80 | (v & 0x3f));
	return 2;
}

size_t mouse_encode(char *out, unsigned int flags, int code, bool release, int col, int row)
{
	int n;

	if (col < 1 || row < 1 || code < 0)
		return 0;

	if (flags & TSM_MOUSE_ENC_SGR) {
		n = snprintf(out, 40, "\033[<%d;%d;%d%c", code, col, row, release ? 'm' : 'M');
		return n > 0 && n < 40 ? (size_t)n : 0;
	}
	if (release)
		code = (code & ~3) | 3;

	if (flags & TSM_MOUSE_ENC_URXVT) {
		n = snprintf(out, 40, "\033[%d;%d;%dM", code + 32, col, row);
		return n > 0 && n < 40 ? (size_t)n : 0;
	}
	if (flags & TSM_MOUSE_ENC_UTF8) {
		size_t len = 3;

		if (code + 32 > 2015 || col + 32 > 2015 || row + 32 > 2015)
			return 0;
		memcpy(out, "\033[M", 3);
		len += put_utf8(out + len, code + 32);
		len += put_utf8(out + len, col + 32);
		len += put_utf8(out + len, row + 32);
		return len;
	}
	if (code + 32 > 255 || col + 32 > 255 || row + 32 > 255)
		return 0;
	out[0] = '\033';
	out[1] = '[';
	out[2] = 'M';
	out[3] = (char)(code + 32);
	out[4] = (char)(col + 32);
	out[5] = (char)(row + 32);
	return 6;
}

