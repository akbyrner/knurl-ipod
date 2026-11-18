/*
 * Drawing into the RGB565 framebuffer. Integer-only, no allocation, everything clipped
 * to the current clip rectangle. Alpha is 0..32 (5 bits) throughout: enough for glass
 * and anti-aliasing, and blending three channels costs one multiply.
 */
#pragma once
#include <stdint.h>
#include "platform.h"
#include "gfx/font.h"

typedef uint16_t color_t;

/* 0xRRGGBB -> RGB565, usable in constant expressions. */
#define RGB(hex) ((color_t)((((hex) >> 8) & 0xF800) | (((hex) >> 5) & 0x07E0) | (((hex) >> 3) & 0x001F)))
#define ALPHA_PCT(p) (((p) * 32 + 50) / 100)

typedef struct { int16_t x, y, w, h; } rect_t;

/* Outer bounds for all drawing (the area being redrawn); gfx_clip() intersects with it. */
void    gfx_bounds(int x, int y, int w, int h);
void    gfx_clip(int x, int y, int w, int h);   /* intersects with the bounds */
void    gfx_noclip(void);                       /* clip = the bounds */

/* Copy a full-screen image (e.g. the plane) into the framebuffer, within the clip. */
void    gfx_image(const uint16_t *screen_image);
/* Blend a full-screen image over the framebuffer in a rectangle (alpha 0..32). */
void    gfx_image_blend(const uint16_t *screen_image, int x, int y, int w, int h, int alpha);
void    gfx_fill(int x, int y, int w, int h, color_t c, int alpha);
/* Rounded rectangle with anti-aliased corners: fill, then a 1 px border inside the edge. */
void    gfx_rrect(int x, int y, int w, int h, int r, color_t fill, int fill_alpha,
                  color_t border, int border_alpha);
/* An RGB565 image (w x h, row-major) with anti-aliased rounded corners (r = 0: square). */
void    gfx_pixels(int x, int y, int w, int h, const uint16_t *px, int r);
/* Opaque rounded rectangle filled with a diagonal gradient from c0 (top-left) to c1. */
void    gfx_rrect_gradient(int x, int y, int w, int h, int r, color_t c0, color_t c1);

/* Text. y is the top of the line box. track adds pixels between letters. If maxw > 0 the
 * text is cut with an ellipsis to fit. Returns the drawn width. */
int     gfx_text(const font_t *f, int x, int y, const char *utf8, color_t c, int maxw, int track);
int     gfx_text_width(const font_t *f, const char *utf8, int track);
/* One glyph by code point (icons live in a font too). */
void    gfx_glyph(const font_t *f, unsigned cp, int x, int y, color_t c);

color_t gfx_mix(color_t a, color_t b, int t /* 0..32: 0 = a */);
