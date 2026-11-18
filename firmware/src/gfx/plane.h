#pragma once
#include <stdint.h>
#include "platform.h"

#define KW 40   /* wash image size (art.bin, RGB888) */
#define KH 30

/* Compose a full-screen plane into dst (320x240 RGB565). wash: 40x30 RGB888, or NULL to use
 * the flat colour `flat` (0xRRGGBB) as the wash. tall: Now Playing's bigger, stronger wash. */
void plane_compose(uint16_t *dst, const uint8_t *wash, uint32_t flat, int tall);
