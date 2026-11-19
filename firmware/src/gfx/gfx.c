#include "gfx/gfx.h"

static int clip_x0 = 0, clip_y0 = 0, clip_x1 = SCREEN_W, clip_y1 = SCREEN_H;
static int bnd_x0 = 0, bnd_y0 = 0, bnd_x1 = SCREEN_W, bnd_y1 = SCREEN_H;

void gfx_bounds(int x, int y, int w, int h)
{
    bnd_x0 = x < 0 ? 0 : x;
    bnd_y0 = y < 0 ? 0 : y;
    bnd_x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    bnd_y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    gfx_noclip();
}

void gfx_clip(int x, int y, int w, int h)
{
    clip_x0 = x < bnd_x0 ? bnd_x0 : x;
    clip_y0 = y < bnd_y0 ? bnd_y0 : y;
    clip_x1 = x + w > bnd_x1 ? bnd_x1 : x + w;
    clip_y1 = y + h > bnd_y1 ? bnd_y1 : y + h;
}

void gfx_noclip(void)
{
    clip_x0 = bnd_x0; clip_y0 = bnd_y0; clip_x1 = bnd_x1; clip_y1 = bnd_y1;
}

/* Intersect [x, x+w) x [y, y+h) with the clip; returns 0 if nothing is left. */
static int clipped(int *x0, int *y0, int *x1, int *y1, int x, int y, int w, int h)
{
    *x0 = x < clip_x0 ? clip_x0 : x;
    *y0 = y < clip_y0 ? clip_y0 : y;
    *x1 = x + w > clip_x1 ? clip_x1 : x + w;
    *y1 = y + h > clip_y1 ? clip_y1 : y + h;
    return *x0 < *x1 && *y0 < *y1;
}

/* Blend fg over bg with alpha 0..32. The three channels are spread out into one 32-bit
 * word (green on top) so a single multiply blends them all. */
static inline color_t blend(color_t bg, color_t fg, int a)
{
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    b = (b + (((f - b) * (uint32_t)a) >> 5)) & 0x07E0F81Fu;
    return (color_t)(b | (b >> 16));
}

color_t gfx_mix(color_t a, color_t b, int t) { return blend(a, b, t); }

void gfx_image(const uint16_t *img)
{
    for (int y = clip_y0; y < clip_y1; y++) {
        const uint16_t *s = img + y * SCREEN_W;
        uint16_t *d = fb + y * SCREEN_W;
        int x = clip_x0;
        if ((x & 1) && x < clip_x1) { d[x] = s[x]; x++; }
        for (; x + 1 < clip_x1; x += 2)              /* two pixels per word */
            *(uint32_t *)(d + x) = *(const uint32_t *)(s + x);
        if (x < clip_x1) d[x] = s[x];
    }
}

void gfx_image_blend(const uint16_t *img, int x, int y, int w, int h, int a)
{
    int x0, y0, x1, y1;
    if (a <= 0 || !clipped(&x0, &y0, &x1, &y1, x, y, w, h)) return;
    for (int j = y0; j < y1; j++) {
        uint16_t *d = fb + j * SCREEN_W;
        const uint16_t *s = img + j * SCREEN_W;
        for (int i = x0; i < x1; i++) d[i] = a >= 32 ? s[i] : blend(d[i], s[i], a);
    }
}

void gfx_fill(int x, int y, int w, int h, color_t c, int a)
{
    int x0, y0, x1, y1;
    if (a <= 0 || !clipped(&x0, &y0, &x1, &y1, x, y, w, h)) return;
    for (int j = y0; j < y1; j++) {
        uint16_t *d = fb + j * SCREEN_W;
        if (a >= 32) for (int i = x0; i < x1; i++) d[i] = c;
        else         for (int i = x0; i < x1; i++) d[i] = blend(d[i], c, a);
    }
}

/* ---- rounded corners ----------------------------------------------------
 * corner[r][dy][dx] = how much of pixel (dx, dy) of an r-radius top-left corner is
 * inside the circle, in 16ths (4x4 samples). Built once on first use. */
#define MAX_R 16
static uint8_t corner[MAX_R + 1][MAX_R][MAX_R];
static int corners_ready;

static void build_corners(void)
{
    for (int r = 1; r <= MAX_R; r++)
        for (int dy = 0; dy < r; dy++)
            for (int dx = 0; dx < r; dx++) {
                int n = 0, c = r * 8, rr = c * c;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) {
                        int sx = c - (dx * 8 + i * 2 + 1), sy = c - (dy * 8 + j * 2 + 1);
                        if (sx * sx + sy * sy <= rr) n++;
                    }
                corner[r][dy][dx] = (uint8_t)n;
            }
    corners_ready = 1;
}

/* Coverage (0..16) of pixel (px, py) by the rounded rectangle. */
static int cover(int x, int y, int w, int h, int r, int px, int py)
{
    int lx = px - x, ly = py - y;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) return 0;
    if (r <= 0) return 16;
    int cx = lx < r ? lx : (lx >= w - r ? w - 1 - lx : -1);
    int cy = ly < r ? ly : (ly >= h - r ? h - 1 - ly : -1);
    return (cx >= 0 && cy >= 0) ? corner[r][cy][cx] : 16;
}

static int fit_radius(int r, int w, int h)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > MAX_R) r = MAX_R;
    return r < 0 ? 0 : r;
}

void gfx_rrect(int x, int y, int w, int h, int r, color_t fill, int fa, color_t border, int ba)
{
    int x0, y0, x1, y1;
    if (!clipped(&x0, &y0, &x1, &y1, x, y, w, h)) return;
    if (!corners_ready) build_corners();
    r = fit_radius(r, w, h);
    int ri = r > 0 ? r - 1 : 0;
    for (int j = y0; j < y1; j++) {
        uint16_t *d = fb + j * SCREEN_W;
        int corner_row = (j - y < r) || (j >= y + h - r);
        int edge_row = (j == y) || (j == y + h - 1);
        for (int i = x0; i < x1; i++) {
            int co, ci;
            if (!corner_row && i >= x + r && i < x + w - r) {      /* fast path: straight edges */
                co = 16;
                ci = (edge_row) ? 0 : 16;
                if (!edge_row && (i == x || i == x + w - 1)) ci = 0;
            } else {
                co = cover(x, y, w, h, r, i, j);
                if (!co) continue;
                ci = ba ? cover(x + 1, y + 1, w - 2, h - 2, ri, i, j) : co;
            }
            if (!ba) ci = co;
            if (fa && ci) d[i] = blend(d[i], fill, (fa * ci) >> 4);
            if (ba && co > ci) d[i] = blend(d[i], border, (ba * (co - ci)) >> 4);
        }
    }
}

void gfx_pixels(int x, int y, int w, int h, const uint16_t *px, int r)
{
    int x0, y0, x1, y1;
    if (!px || !clipped(&x0, &y0, &x1, &y1, x, y, w, h)) return;
    if (!corners_ready) build_corners();
    r = fit_radius(r, w, h);
    for (int j = y0; j < y1; j++) {
        uint16_t *d = fb + j * SCREEN_W;
        const uint16_t *s = px + (j - y) * w - x;
        int corner_row = (j - y < r) || (j >= y + h - r);
        for (int i = x0; i < x1; i++) {
            if (!corner_row || (i >= x + r && i < x + w - r)) { d[i] = s[i]; continue; }
            int co = cover(x, y, w, h, r, i, j);
            if (co) d[i] = co >= 16 ? s[i] : blend(d[i], s[i], co * 2);
        }
    }
}

void gfx_rrect_gradient(int x, int y, int w, int h, int r, color_t c0, color_t c1)
{
    int x0, y0, x1, y1;
    if (!clipped(&x0, &y0, &x1, &y1, x, y, w, h)) return;
    if (!corners_ready) build_corners();
    r = fit_radius(r, w, h);
    int span = w + h - 2 > 0 ? w + h - 2 : 1;
    for (int j = y0; j < y1; j++) {
        uint16_t *d = fb + j * SCREEN_W;
        for (int i = x0; i < x1; i++) {
            int co = cover(x, y, w, h, r, i, j);
            if (!co) continue;
            color_t c = blend(c0, c1, ((i - x) + (j - y)) * 32 / span);
            d[i] = co >= 16 ? c : blend(d[i], c, co * 2);
        }
    }
}

/* ---- text ----------------------------------------------------------------- */

static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); p += 3;
    } else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u); p += 2;
    } else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) {
        c = ((c & 31) << 6) | (p[0] & 63u); p += 1;
    }
    *s = (const char *)p;
    return c;
}

static const glyph_t *find(const font_t *f, unsigned cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static const glyph_t *glyph_or_fallback(const font_t *f, unsigned cp)
{
    const glyph_t *g = find(f, cp);
    return g ? g : find(f, '?');
}

static void draw_glyph(const font_t *f, const glyph_t *g, int x, int y, color_t c)
{
    int gx = x + g->xoff, gy = y + g->yoff, x0, y0, x1, y1;
    if (!g->w || !clipped(&x0, &y0, &x1, &y1, gx, gy, g->w, g->h)) return;
    int stride = (g->w + 1) >> 1;
    const uint8_t *bits = f->bits + g->off;
    for (int j = y0; j < y1; j++) {
        const uint8_t *row = bits + (j - gy) * stride;
        uint16_t *d = fb + j * SCREEN_W;
        for (int i = x0; i < x1; i++) {
            int k = i - gx;
            int a4 = (k & 1) ? (row[k >> 1] & 15) : (row[k >> 1] >> 4);
            if (a4) d[i] = a4 == 15 ? c : blend(d[i], c, (a4 * 32 + 7) / 15);
        }
    }
}

void gfx_glyph(const font_t *f, unsigned cp, int x, int y, color_t c)
{
    const glyph_t *g = find(f, cp);
    if (g) draw_glyph(f, g, x, y, c);
}

int gfx_text_width(const font_t *f, const char *s, int track)
{
    int w = 0;
    while (*s) {
        const glyph_t *g = glyph_or_fallback(f, next_cp(&s));
        if (g) w += g->adv + track;
    }
    return w > 0 ? w - track : 0;
}

int gfx_text(const font_t *f, int x, int y, const char *s, color_t c, int maxw, int track)
{
    int limit = 0x7FFF;
    const glyph_t *ell = find(f, 0x2026);
    if (maxw > 0 && gfx_text_width(f, s, track) > maxw)
        limit = maxw - (ell ? ell->adv : 0);
    int pen = x;
    while (*s) {
        const glyph_t *g = glyph_or_fallback(f, next_cp(&s));
        if (!g) continue;
        if (pen + g->adv - x > limit) {
            if (ell) { draw_glyph(f, ell, pen, y, c); pen += ell->adv; }
            return pen - x;
        }
        draw_glyph(f, g, pen, y, c);
        pen += g->adv + track;
    }
    return pen > x ? pen - x - track : 0;
}

/* ---- Flow covers ------------------------------------------------------------------------
 * Column by column: each screen column is one source column, stretched to that column's
 * height. 1/z is linear across the screen, so the source column is t*hr / h (perspective-correct),
 * and the height itself runs straight from hl to hr. Nearest-neighbour sampling throughout:
 * the covers are moving whenever they aren't face-on, and face-on they are close to their size. */
void gfx_cover(const uint16_t *px, int s, color_t c0, color_t c1, int xl, int xr, int cy, int hl, int hr,
               int shade, int r, int refl, int refl_a)
{
    int w = xr - xl;
    if (w <= 0 || hl <= 0 || hr <= 0 || s <= 0) return;
    if (!corners_ready) build_corners();
    r = hl == hr ? fit_radius(r, w, hl) : 0;
    uint32_t grad = (16u << 16) / (uint32_t)s;          /* gradient: (u + v) * 16 / s, in Q16 */
    int x0 = xl < clip_x0 ? clip_x0 : xl, x1 = xr > clip_x1 ? clip_x1 : xr;
    for (int x = x0; x < x1; x++) {
        uint32_t t = ((uint32_t)((x - xl) * 2 + 1) << 15) / (uint32_t)w;            /* Q16 */
        uint32_t hq = (65536 - t) * (uint32_t)hl + t * (uint32_t)hr;               /* Q16 */
        int h = (int)((hq + 32768) >> 16);
        if (h <= 0) continue;
        uint32_t u = t * (uint32_t)hr / (uint32_t)h;                                /* Q16 */
        int su = (int)((u * (uint32_t)s) >> 16);
        if (su >= s) su = s - 1;
        int top = cy - h / 2;
        uint32_t vstep = ((uint32_t)s << 16) / (uint32_t)h;
        int y0 = top < clip_y0 ? clip_y0 : top, y1 = top + h > clip_y1 ? clip_y1 : top + h;
        uint32_t v = (uint32_t)(y0 - top) * vstep + vstep / 2;
        uint16_t *d = fb + x;
        for (int y = y0; y < y1; y++, v += vstep) {
            int sv = (int)(v >> 16);
            color_t c = px ? px[sv * s + su] : blend(c0, c1, (int)(((uint32_t)(su + sv) * grad) >> 16));
            if (shade) c = blend(c, 0, shade);
            int a = r ? cover(xl, top, w, h, r, x, y) * 2 : 32;
            d[y * SCREEN_W] = a >= 32 ? c : blend(d[y * SCREEN_W], c, a);
        }
        /* the reflection: the cover upside down below it, fading out */
        int rh = (h * refl) >> 8;
        if (rh <= 0 || refl_a <= 0) continue;
        int ry = top + h + 1;
        uint32_t aq = (uint32_t)refl_a << 16, astep = aq / (uint32_t)rh;
        v = vstep / 2;
        for (int k = 0; k < rh; k++, v += vstep, aq -= astep) {
            int y = ry + k, sv = s - 1 - (int)(v >> 16);
            if (sv < 0 || y >= clip_y1) break;
            if (y < clip_y0) continue;
            color_t c = px ? px[sv * s + su] : blend(c0, c1, (int)(((uint32_t)(su + sv) * grad) >> 16));
            if (shade) c = blend(c, 0, shade);
            d[y * SCREEN_W] = blend(d[y * SCREEN_W], c, (int)(aq >> 16));
        }
    }
}
