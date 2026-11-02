/* mouse.h - xterm mouse reports */
#ifndef WATE_MOUSE_H
#define WATE_MOUSE_H

#include <stdbool.h>
#include <stddef.h>

#define MOUSE_CODE_SHIFT 4
#define MOUSE_CODE_ALT 8
#define MOUSE_CODE_CTRL 16
#define MOUSE_CODE_MOTION 32
#define MOUSE_CODE_WHEEL 64

size_t mouse_encode(char *out, unsigned int flags, int code, bool release, int col, int row);

#endif

