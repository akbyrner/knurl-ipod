/*
 * Knurl's Graphite Glass plane (ui/Plane.kt), composed on the iPod:
 *   graphite, under a soft wash of the playing album's colours (a 40x30 pre-blurred image
 *   from art.bin, stretched with bilinear filtering), fading into graphite towards the bottom
 *   by Knurl's gradient stops; then the soft light from the top and the grain (one 320x240
 *   alpha table, generated at build time); then Bayer-dithered down to RGB565.
 * Only runs when the album changes.
 */
#include "gfx/plane.h"
#include "light.h"          /* build/gen: light_alpha[320*240], 0..256 */

#define GR 0x18
#define GG 0x18
#define GB 0x1B

static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* Knurl's wash weight per row, 0..256: wash alpha x (1 - graphite overlay alpha). */
static int wash_weight(int y, int tall)
{
    /* overlay alpha: 0 at the top, a1 at s1, fully graphite from s2 down (all x1000) */
    int s1 = tall ? 450 : 400, a1 = tall ? 400 : 550, s2 = tall ? 680 : 620, wash = tall ? 700 : 600;
    int t = y * 1000 / (SCREEN_H - 1), g;
    if (t <= s1) g = a1 * t / s1;
    else if (t <= s2) g = a1 + (1000 - a1) * (t - s1) / (s2 - s1);
    else g = 1000;
    return wash * (1000 - g) / 1000 * 256 / 1000;
}

static inline uint16_t dither565(int r, int g, int b, int x, int y)
{
    int t = 2 * bayer[y & 3][x & 3] + 1;
    int r5 = (r * 31 * 32 + t * 255) / (255 * 32);
    int g6 = (g * 63 * 32 + t * 255) / (255 * 32);
    int b5 = (b * 31 * 32 + t * 255) / (255 * 32);
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

void plane_compose(uint16_t *dst, const uint8_t *wash, uint32_t flat, int tall)
{
    uint8_t line[KW * 3];
    for (int y = 0; y < SCREEN_H; y++) {
        int w = wash_weight(y, tall);
        if (wash) {                                         /* this row's colours, interpolated vertically */
            int fy = y * ((KH - 1) << 8) / (SCREEN_H - 1);
            int y0 = fy >> 8, y1 = y0 + 1 < KH ? y0 + 1 : y0, ty = fy & 255;
            for (int i = 0; i < KW * 3; i++)
                line[i] = (uint8_t)((wash[y0 * KW * 3 + i] * (256 - ty) + wash[y1 * KW * 3 + i] * ty) >> 8);
        } else {
            for (int i = 0; i < KW; i++) {
                line[i * 3] = (uint8_t)(flat >> 16); line[i * 3 + 1] = (uint8_t)(flat >> 8); line[i * 3 + 2] = (uint8_t)flat;
            }
        }
        const uint8_t *L = light_alpha + y * SCREEN_W;
        uint16_t *d = dst + y * SCREEN_W;
        for (int x = 0; x < SCREEN_W; x++) {
            int fx = x * ((KW - 1) << 8) / (SCREEN_W - 1);
            int x0 = fx >> 8, x1 = x0 + 1 < KW ? x0 + 1 : x0, tx = fx & 255;
            const uint8_t *a = line + x0 * 3, *b = line + x1 * 3;
            int wr = (a[0] * (256 - tx) + b[0] * tx) >> 8;
            int wg = (a[1] * (256 - tx) + b[1] * tx) >> 8;
            int wb = (a[2] * (256 - tx) + b[2] * tx) >> 8;
            int r = GR + (((wr - GR) * w) >> 8);
            int g = GG + (((wg - GG) * w) >> 8);
            int bl = GB + (((wb - GB) * w) >> 8);
            int l = L[x];                                   /* top light + grain */
            r += ((255 - r) * l) >> 8;
            g += ((255 - g) * l) >> 8;
            bl += ((255 - bl) * l) >> 8;
            d[x] = dither565(r, g, bl, x, y);
        }
    }
}
