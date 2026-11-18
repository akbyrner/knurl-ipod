#pragma once
#include <stdint.h>

/* One glyph in a pre-rasterised sheet (see tools/build_fonts.py). Bitmaps are 4-bit
 * alpha, two pixels per byte, each row padded to a whole byte. */
typedef struct {
    uint16_t cp;        /* Unicode code point */
    int8_t   xoff;      /* from pen x to the bitmap's left edge */
    int8_t   yoff;      /* from the top of the line box to the bitmap's top edge */
    uint8_t  w, h;      /* bitmap size */
    uint8_t  adv;       /* pen advance */
    uint32_t off;       /* offset into the font's bits */
} glyph_t;

typedef struct {
    const glyph_t *glyphs;   /* sorted by cp */
    uint16_t       count;
    const uint8_t *bits;
    uint8_t        ascent;
    uint8_t        line;     /* line height */
} font_t;
