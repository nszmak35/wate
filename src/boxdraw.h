/* boxdraw.h - procedurally drawn box, block and braille characters */
#ifndef WATE_BOXDRAW_H
#define WATE_BOXDRAW_H

#include <stdbool.h>
#include <stdint.h>

bool boxdraw(uint32_t cp, uint8_t *cov, int w, int h);

#endif

