/*
 * The boot screen (Knurl's SplashOverlay): the mark, KNURL, and under them the start-up log, one
 * line per step, newest at the bottom in bone, older ones dimmer. When the log fills its space it
 * scrolls, so the last steps reached are always on screen (a hang shows where it stopped).
 */
#include "splash.h"
#include "gfx/gfx.h"
#include "fonts.h"
#include "logo.h"

#define LOG_Y     160
#define LOG_PITCH 12
#define LOG_LINES 6
#define LOG_LEN   64

static char lines[LOG_LINES][LOG_LEN];
static int n_lines = -1;          /* -1: the screen isn't drawn yet */

static void centred(const font_t *f, int y, const char *t, color_t c, int track)
{
    int w = gfx_text_width(f, t, track);
    if (w > SCREEN_W - 28) w = SCREEN_W - 28;
    gfx_text(f, (SCREEN_W - w) / 2, y, t, c, SCREEN_W - 28, track);
}

void splash_log(const char *line)
{
    gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
    if (n_lines < 0) {
        gfx_fill(0, 0, SCREEN_W, SCREEN_H, RGB(0x18181B), 32);
        gfx_pixels((SCREEN_W - LOGO_S) / 2, 12, LOGO_S, LOGO_S, logo_px, 0);
        centred(&font_brand, 102, "KNURL", RGB(0xECEBE6), 3);
        centred(&font_mono, 140, "CLICK-WHEEL AUDIO", RGB(0x8E9097), 2);
        gfx_fill(SCREEN_W / 2 - 60, LOG_Y - 5, 120, 1, RGB(0xFFFFFF), ALPHA_PCT(12));
        n_lines = 0;
        platform_present(0, 0, SCREEN_W, SCREEN_H);
    }
    if (n_lines == LOG_LINES) {                       /* full: scroll up one */
        for (int i = 1; i < LOG_LINES; i++)
            for (int k = 0; k < LOG_LEN; k++) lines[i - 1][k] = lines[i][k];
        n_lines--;
    }
    int k = 0;
    for (; line[k] && k < LOG_LEN - 1; k++) lines[n_lines][k] = line[k];
    lines[n_lines][k] = 0;
    n_lines++;
    gfx_fill(0, LOG_Y, SCREEN_W, LOG_LINES * LOG_PITCH, RGB(0x18181B), 32);
    for (int i = 0; i < n_lines; i++)
        centred(&font_mono, LOG_Y + i * LOG_PITCH, lines[i],
                i == n_lines - 1 ? RGB(0xECEBE6) : i == n_lines - 2 ? RGB(0xA8A8AD) : RGB(0x6E6F75), 0);
    platform_present(0, LOG_Y, SCREEN_W, LOG_LINES * LOG_PITCH);
}
