/*
 * The user interface: Knurl's screens on the iPod, driven by the click wheel, showing the
 * real library from /.knurl (Milestone 2), playing its MP3s (Milestone 3: src/player.c,
 * src/mp3) gaplessly through the audio ring.
 *
 * 320x240 landscape: a header, a scrolling content area, and the mini player at the
 * bottom while something is playing. The physical click wheel replaces Knurl's on-screen one.
 */
#include "app.h"
#include "gfx/gfx.h"
#include "gfx/plane.h"
#include "library/library.h"
#include "storage/fat.h"
#include "mem.h"
#include "engine.h"
#include "mp3/mp3info.h"

#define SHOW_DECODE_STATS 0            /* 1: the mini player shows decoder load instead of the artist */
#include "fonts.h"
#include "icons.h"

/* ---- Knurl's palette (ui/Chrome.kt) ------------------------------------------- */
#define C_BONE      RGB(0xECEBE6)
#define C_ORANGE    RGB(0xFF6A1A)
#define C_WHITE     RGB(0xFFFFFF)
#define C_SECONDARY RGB(0xA8A8AD)
#define C_TERTIARY  RGB(0x8E8E93)
#define C_LABEL     RGB(0xB8B9BE)
#define C_GLYPH     RGB(0xC4C4C4)
#define C_INK       RGB(0x111114)
#define C_INK2      RGB(0x55555A)
#define C_SELECTED  RGB(0xF2F2EF)
#define A_GLASS     ALPHA_PCT(7)
#define A_BORDER    ALPHA_PCT(12)

#define FADE   10
#define MINI_Y 200
#define MINI_H 34

/* Now Playing, landscape: the cover left, a glass card with the song's details right, and
 * the notched bar (song position, or volume for a moment after a wheel turn) along the
 * bottom. Knurl's phone layout stacks the same three pieces. */
#define NP_ART_X  14
#define NP_ART_Y  32
#define NP_INFO_X 164
#define NP_CARD_Y 182
#define NP_CARD_H 50

/* ---- screens ---------------------------------------------------------------------- */
enum { S_HOME, S_ALBUMS, S_ARTISTS, S_SONGS, S_ALBUM, S_ARTIST, S_RECENT, S_SOON, S_DIAG, S_NOW };

typedef struct {
    int kind, arg;                 /* arg: album / artist index, or milestone for S_SOON */
    int focus, scroll, target;
    const char *title;             /* S_SOON */
} screen_t;

static screen_t stack[8];
static int depth;
#define CUR (&stack[depth - 1])

/* Home's fixed parts */
#define TILE_N      4
#define TILE_S      KART_TILE_S
#define TILE_Y      16
#define TILE_X(i)   (16 + (i) * (TILE_S + 24))
#define LIB_LABEL_Y 92
#define ROW_Y0      104
#define ROW_PITCH   30
#define ROW_H       26
typedef struct { const char *label; unsigned icon; int screen, milestone; } home_row;
static const home_row home_rows[] = {
    { "Flow", ICON_FLOW, S_SOON, 5 },          { "Albums", ICON_ALBUMS, S_ALBUMS, 0 },
    { "Artists", ICON_ARTISTS, S_ARTISTS, 0 }, { "Songs", ICON_SONGS, S_SONGS, 0 },
    { "Playlists", ICON_PLAYLISTS, S_SOON, 6 },{ "Shuffle Songs", ICON_SHUFFLE, -1, 0 },
    { "Settings", ICON_SETTINGS, S_SOON, 6 },
};
#define HOME_ROWS ((int)(sizeof home_rows / sizeof home_rows[0]))
#define GRID_COLS 4
#define GRID_PITCH 100

/* ---- now playing -------------------------------------------------------------------------- */
static struct {
    int track;                     /* -1 = nothing */
    int playing;
    uint32_t pos_ms;               /* where in the track we are (heard, not decoded) */
    uint32_t start_ms;             /* the track's audio began this far in (after a seek) */
    uint16_t *queue;
    int qlen, qpos;
    /* Now Playing (Knurl: NowPlayingScreen.kt / KnurlController.kt) */
    int scrub;                     /* Select toggles: the wheel scrubs instead of setting volume */
    int scrub_pending;             /* a seek waits for the wheel to rest */
    uint32_t scrub_ms, scrub_at;
    int volume;                    /* 0..VOLUME_STEPS-1 */
    uint32_t volume_at;            /* when the volume bar was last shown (0 = not shown) */
} np = { -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0 };

#define VOLUME_STEPS   16          /* Knurl's */
#define SCRUB_NOTCHES  21
#define SCRUB_REST_MS  300         /* seek once the wheel has rested this long */
#define VOLUME_SHOW_MS 1500

/* Step 0 is silent; 7 (the start) is -30 dB; 15 is +2 dB (the headphone amp goes to +6). */
static int volume_db(int step) { return step <= 0 ? -90 : -58 + 4 * step; }

static uint16_t *plane;            /* the composed background */
static int plane_album = -2;
static uint32_t rng_state = 0x9E3779B9u;
static uint32_t last_ms, anim_ms;
static int shown_sec = -1, shown_prog = -1, shown_batt = -1, shown_lock, first = 1;

/* ---- dirty rectangle -------------------------------------------------------------- */
static int dx0, dy0, dx1, dy1;
static void dirty(int x, int y, int w, int h)
{
    if (dx1 <= dx0) { dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; return; }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
}
static void dirty_all(void) { dirty(0, 0, SCREEN_W, SCREEN_H); }

/* ---- small helpers -------------------------------------------------------------------- */
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static char *put_uint(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static void time_text(char *out, uint32_t ms)
{
    uint32_t sec = ms / 1000, m = sec / 60, s = sec % 60;
    char *p = put_uint(out, m);
    *p++ = ':';
    *p++ = (char)('0' + s / 10);
    *p++ = (char)('0' + s % 10);
    *p = 0;
}

/* "435 SONGS" / "1 SONG" */
static void count_text(char *out, uint32_t n, const char *one, const char *many)
{
    char *p = put_uint(out, n);
    *p++ = ' ';
    put_str(p, n == 1 ? one : many);
}

static void upper_ascii(char *dst, const char *src, int max)
{
    int i = 0;
    for (; src[i] && i < max - 1; i++) dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    dst[i] = 0;
}

static int has_mini(void) { return np.track >= 0 && depth && stack[depth - 1].kind != S_NOW; }

/* ---- geometry per screen ---------------------------------------------------------------- */
static int content_top(int kind)
{
    switch (kind) {
    case S_HOME: return 22;
    case S_ALBUM: return 82;
    default: return 44;
    }
}
static int content_bottom(void) { return has_mini() ? MINI_Y - 4 : SCREEN_H - 4; }

static int list_pitch(int kind)
{
    switch (kind) {
    case S_SONGS: case S_ARTIST: case S_RECENT: return 34;
    case S_ARTISTS: return 30;
    case S_ALBUM: return 28;
    default: return 30;
    }
}

/* Home lists Now Playing last while something is loaded (Knurl's MenuEntry.NowPlaying). */
static int home_rows_n(void) { return HOME_ROWS + (np.track >= 0); }

static int item_count(const screen_t *s)
{
    if (!lib.loaded) return s->kind == S_HOME ? TILE_N + HOME_ROWS : 0;
    switch (s->kind) {
    case S_HOME: return TILE_N + home_rows_n();
    case S_ALBUMS: return (int)lib.n_albums;
    case S_ARTISTS: return (int)lib.n_artists;
    case S_SONGS: return (int)lib.n_tracks;
    case S_ALBUM: return lib.albums[s->arg].n_tracks;
    case S_ARTIST: return lib.artists[s->arg].n_albums;
    case S_RECENT: return lib.n_albums < 50 ? (int)lib.n_albums : 50;
    default: return 0;
    }
}

/* Item i's top and height, in content coordinates. */
static void item_box(const screen_t *s, int i, int *y, int *h)
{
    if (s->kind == S_HOME) {
        if (i < TILE_N) { *y = TILE_Y; *h = TILE_S; }
        else { *y = ROW_Y0 + (i - TILE_N) * ROW_PITCH; *h = ROW_H; }
    } else if (s->kind == S_ALBUMS) {
        *y = 6 + (i / GRID_COLS) * GRID_PITCH;
        *h = GRID_PITCH - 6;
    } else {
        *y = 4 + i * list_pitch(s->kind);
        *h = list_pitch(s->kind) - 4;
    }
}

static int content_total(const screen_t *s)
{
    int n = item_count(s), y, h;
    if (!n) return 0;
    item_box(s, n - 1, &y, &h);
    return y + h + 6;
}

static void aim_scroll(screen_t *s)
{
    int view = content_bottom() - content_top(s->kind);
    int y, h, t = s->target;
    if (s->kind == S_HOME && s->focus < TILE_N) {
        t = 0;
    } else {
        item_box(s, s->focus, &y, &h);
        if (y - t < FADE + 8) t = y - (FADE + 8);
        if (y + h - t > view - FADE) t = y + h - (view - FADE);
        if (s->kind == S_HOME && s->focus == TILE_N && t > LIB_LABEL_Y - 4) t = LIB_LABEL_Y - 4;
    }
    int max = content_total(s) - view;
    if (t > max) t = max;
    if (t < 0) t = 0;
    s->target = t;
}

/* ---- planes ------------------------------------------------------------------------- */
static void update_plane(void)
{
    int album = np.track >= 0 ? lib.tracks[np.track].album : -1;
    if (album == plane_album) return;
    plane_album = album;
    const uint8_t *wash = album >= 0 ? lib_art(album, KART_WASH) : 0;
    plane_compose(plane, wash, album >= 0 ? lib.albums[album].color0 : 0x18181B, 0);
    dirty_all();
}

/* ---- drawing pieces ---------------------------------------------------------------- */
static void mono_label(int x, int y, const char *t, color_t c) { gfx_text(&font_mono, x, y, t, c, 0, 2); }

static void battery(int x, int y)
{
    int level = platform_battery();
    gfx_rrect(x, y, 22, 11, 3, 0, 0, C_GLYPH, 32);
    gfx_rrect(x, y + 1, 22, 9, 3, 0, 0, C_GLYPH, 10);
    if (level >= 0) {
        int w = (18 * (level < 4 ? 4 : level) + 50) / 100;
        color_t c = platform_charging() ? C_ORANGE : level <= 15 ? RGB(0xFF6B5E) : C_GLYPH;
        gfx_fill(x + 2, y + 2, w, 7, c, 32);
    }
    gfx_fill(x + 23, y + 3, 2, 5, C_GLYPH, 32);
    if (shown_lock) {                             /* HOLD is on: a small padlock */
        gfx_rrect(x - 17, y - 2, 8, 9, 3, 0, 0, C_GLYPH, 32);
        gfx_rrect(x - 19, y + 3, 12, 9, 2, C_GLYPH, 32, 0, 0);
    }
}

static void header(const char *title, const char *label)
{
    char up[64];
    upper_ascii(up, title, sizeof up);
    gfx_text(&font_title, 14, 4, up, C_BONE, 256, 1);
    if (label) mono_label(14, 29, label, C_LABEL);
    battery(282, 8);
}

/* An album's cover at size s (one of the art.bin sizes), or a gradient with its initial. */
static void album_art(int album, int kind, int x, int y, int s, int r)
{
    const kidx_album *a = &lib.albums[album];
    const uint16_t *px = lib_art(album, kind);
    if (px) {
        gfx_pixels(x, y, s, s, px, r);
    } else {
        gfx_rrect_gradient(x, y, s, s, r, RGB(a->color0), RGB(a->color1));
        if (s >= 40) {
            char letter[5] = { 0 };
            const char *t = lib_str(a->title);
            int n = 1;
            unsigned char c0 = (unsigned char)t[0];
            if (c0 >= 0xC0) n = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : 2;
            for (int i = 0; i < n && t[i]; i++) letter[i] = (t[i] >= 'a' && t[i] <= 'z') ? (char)(t[i] - 32) : t[i];
            if (!letter[0]) letter[0] = '?';
            int w = gfx_text_width(&font_letter, letter, 0);
            gfx_text(&font_letter, x + (s - w) / 2, y + (s - font_letter.line) / 2, letter, C_WHITE, 0, 0);
        }
    }
    gfx_rrect(x, y, s, s, r, 0, 0, C_WHITE, A_BORDER);
}

static void ring(int x, int y, int w, int h, int r)
{
    gfx_rrect(x - 3, y - 3, w + 6, h + 6, r + 3, 0, 0, C_WHITE, 32);
    gfx_rrect(x - 2, y - 2, w + 4, h + 4, r + 2, 0, 0, C_WHITE, 32);
}

static void glass_row(int y, int h, int selected)
{
    if (selected) gfx_rrect(10, y, 300, h, 9, C_SELECTED, 32, 0, 0);
    else gfx_rrect(10, y, 300, h, 9, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
}

#if SHOW_DECODE_STATS
static char *put_num(char *p, const char *label, uint32_t v)
{
    while (*label) *p++ = *label++;
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}
#endif

static void mini_player(void)
{
    if (!has_mini()) return;
    const int x = 10, y = MINI_Y, w = 300, h = MINI_H;
    const kidx_track *t = &lib.tracks[np.track];
    gfx_rrect(x, y, w, h, 11, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
    album_art(t->album, KART_THUMB, x + 6, y + 5, KART_THUMB_S, 5);
    gfx_text(&font_strong, x + 38, y + 3, lib_str(t->title), C_WHITE, 190, 0);
#if SHOW_DECODE_STATS
    /* Decoder load (share of the COP), dropouts, and where the decode time goes:
     * bitstream+Huffman / stereo+IMDCT / DCT / synthesis, as % of the decode time. */
    char st[64], *p = st;
    engine_stats_t es;
    engine_stats(&es);
    if (es.err[0]) { const char *e = es.err; while (*e && p < st + 40) *p++ = *e++; }
    else {
        uint32_t pct10 = es.dec_ms ? es.dec_us / es.dec_ms : 0;     /* us per ms of audio = 0.1 % */
        p = put_num(p, "", pct10 / 10); *p++ = '.'; *p++ = (char)('0' + pct10 % 10); *p++ = '%';
        p = put_num(p, " G", audio_underruns());
        uint32_t tot = es.prof[0] + es.prof[1] + es.prof[2] + es.prof[3];
        for (int i = 0; i < 4 && tot; i++) p = put_num(p, i ? "/" : " ", (uint32_t)((uint64_t)es.prof[i] * 100 / tot));
        if (es.saturated) p = put_num(p, " SAT", es.saturated);
        p = put_num(p, " C", (uint32_t)platform_cpu_busy()); *p++ = '%';
        p = put_num(p, " ", (uint32_t)(platform_battery_mv() > 0 ? platform_battery_mv() : 0)); *p++ = 'm'; *p++ = 'V';
    }
    *p = 0;
    gfx_text(&font_mono, x + 38, y + 20, st, C_SECONDARY, 190, 1);
#else
    gfx_text(&font_body, x + 38, y + 18, lib_str(t->artist), C_SECONDARY, 190, 0);
#endif
    char tm[12];
    time_text(tm, np.pos_ms);
    int tw = gfx_text_width(&font_mono, tm, 0);
    gfx_text(&font_mono, x + w - 32 - tw, y + 12, tm, C_TERTIARY, 0, 0);
    gfx_glyph(&font_icons, np.playing ? ICON_PAUSE : ICON_PLAY, x + w - 22, y + 11, C_WHITE);
    uint32_t dur = t->duration_ms ? t->duration_ms : 1;
    uint32_t pos = np.pos_ms > dur ? dur : np.pos_ms;
    gfx_fill(x + 10, y + h - 3, (int)((uint64_t)pos * 280 / dur), 2, C_ORANGE, 32);
}

/* ---- items ---------------------------------------------------------------------------- */
static void draw_home_item(int i, int y, int sel)
{
    if (i < TILE_N) {
        int x = TILE_X(i);
        const char *label = "";
        if (sel) ring(x, y, TILE_S, TILE_S, 11);
        if (i == 0) {
            gfx_rrect_gradient(x, y, TILE_S, TILE_S, 11, RGB(0x5B3CF5), RGB(0xE0338A));
            gfx_glyph(&font_icons, ICON_HEART, x + (TILE_S - 18) / 2, y + (TILE_S - 18) / 2, C_WHITE);
            gfx_rrect(x, y, TILE_S, TILE_S, 11, 0, 0, C_WHITE, A_BORDER);
            label = "Liked Songs";
        } else if (i == 3 || !lib.loaded || !lib.n_albums) {
            gfx_rrect_gradient(x, y, TILE_S, TILE_S, 11, RGB(0x34343A), RGB(0x222226));
            gfx_glyph(&font_icons, ICON_SHUFFLE, x + (TILE_S - 14) / 2, y + (TILE_S - 14) / 2, C_WHITE);
            gfx_rrect(x, y, TILE_S, TILE_S, 11, 0, 0, C_WHITE, A_BORDER);
            label = i == 3 ? "Shuffle All" : "";
        } else {
            int album = i == 1 ? (np.track >= 0 ? lib.tracks[np.track].album : lib.albums_by_added[0]) : lib.albums_by_added[0];
            album_art(album, KART_TILE, x, y, TILE_S, 11);
            label = i == 1 ? lib_str(lib.albums[album].title) : "Recently Added";
        }
        int maxw = TILE_S + 16, lw = gfx_text_width(&font_body, label, 0);
        if (lw > maxw) lw = maxw;
        gfx_text(&font_body, x + (TILE_S - lw) / 2, y + TILE_S + 4, label, sel ? C_WHITE : C_BONE, maxw, 0);
        return;
    }
    static const home_row now_row = { "Now Playing", ICON_PLAY, S_NOW, 0 };
    const home_row *r = i - TILE_N < HOME_ROWS ? &home_rows[i - TILE_N] : &now_row;
    glass_row(y, ROW_H, sel);
    gfx_rrect(15, y + 4, 18, 18, 5, sel ? C_INK : C_WHITE, sel ? 3 : 2, sel ? C_INK : C_WHITE, sel ? 7 : 5);
    gfx_glyph(&font_icons, r->icon, 17, y + 6, sel ? C_INK : C_GLYPH);
    gfx_text(&font_row, 42, y + (ROW_H - font_row.line) / 2, r->label, sel ? C_INK : C_WHITE, 220, 0);
    gfx_glyph(&font_icons, ICON_CHEVRON, 294, y + 7, sel ? C_INK : C_SECONDARY);
}

static void draw_grid_item(int i, int y, int sel)
{
    int album = lib.albums_by_title[i];
    int x = 12 + (i % GRID_COLS) * 76;
    if (sel) ring(x, y, KART_GRID_S, KART_GRID_S, 8);
    album_art(album, KART_GRID, x, y, KART_GRID_S, 8);
    gfx_text(&font_body, x, y + KART_GRID_S + 4, lib_str(lib.albums[album].title), sel ? C_WHITE : C_BONE, 70, 0);
    gfx_text(&font_body, x, y + KART_GRID_S + 17, lib_str(lib.albums[album].artist_name), C_TERTIARY, 70, 0);
}

static void draw_song_row(int track, int y, int h, int sel)
{
    const kidx_track *t = &lib.tracks[track];
    glass_row(y, h, sel);
    char tm[12];
    time_text(tm, t->duration_ms);
    int tw = gfx_text_width(&font_mono, tm, 0);
    gfx_text(&font_row, 22, y + 2, lib_str(t->title), sel ? C_INK : C_WHITE, 266 - tw - 22, 0);
    gfx_text(&font_body, 22, y + 17, lib_str(t->artist), sel ? C_INK2 : C_SECONDARY, 266 - tw - 22, 0);
    gfx_text(&font_mono, 298 - tw, y + 10, tm, track == np.track ? C_ORANGE : sel ? C_INK2 : C_TERTIARY, 0, 0);
}

static void draw_album_row(int album, int y, int h, int sel)
{
    const kidx_album *a = &lib.albums[album];
    glass_row(y, h, sel);
    album_art(album, KART_THUMB, 15, y + 3, KART_THUMB_S, 5);
    gfx_text(&font_row, 48, y + 2, lib_str(a->title), sel ? C_INK : C_WHITE, 230, 0);
    char sub[96], *p = sub;
    const char *artist = lib_str(a->artist_name);
    int n = 0;
    while (artist[n] && n < 60) { *p++ = artist[n]; n++; }
    *p = 0;
    if (a->year) { p = put_str(p, " \xC2\xB7 "); put_uint(p, a->year); }
    gfx_text(&font_body, 48, y + 17, sub, sel ? C_INK2 : C_SECONDARY, 230, 0);
    gfx_glyph(&font_icons, ICON_CHEVRON, 294, y + 9, sel ? C_INK : C_SECONDARY);
}

static void draw_artist_row(int artist, int y, int h, int sel)
{
    const kidx_artist *a = &lib.artists[artist];
    glass_row(y, h, sel);
    char n[24];
    count_text(n, a->n_albums, "ALBUM", "ALBUMS");
    int nw = gfx_text_width(&font_mono_s, n, 1);
    gfx_text(&font_row, 22, y + (h - font_row.line) / 2, lib_str(a->name), sel ? C_INK : C_WHITE, 250 - nw, 0);
    gfx_text(&font_mono_s, 286 - nw, y + 9, n, sel ? C_INK2 : C_TERTIARY, 0, 1);
    gfx_glyph(&font_icons, ICON_CHEVRON, 294, y + 7, sel ? C_INK : C_SECONDARY);
}

static void draw_track_row(int track, int y, int h, int sel)
{
    const kidx_track *t = &lib.tracks[track];
    glass_row(y, h, sel);
    char num[8], tm[12];
    put_uint(num, t->number);
    time_text(tm, t->duration_ms);
    int tw = gfx_text_width(&font_mono, tm, 0);
    color_t accent = track == np.track ? C_ORANGE : sel ? C_INK2 : C_TERTIARY;
    gfx_text(&font_mono, 20, y + 7, t->number ? num : "\xC2\xB7", accent, 0, 0);
    gfx_text(&font_row, 44, y + (h - font_row.line) / 2, lib_str(t->title), sel ? C_INK : C_WHITE, 248 - tw - 20, 0);
    gfx_text(&font_mono, 298 - tw, y + 7, tm, accent, 0, 0);
}

static void draw_item(const screen_t *s, int i, int y, int h, int sel)
{
    switch (s->kind) {
    case S_HOME: draw_home_item(i, y, sel); break;
    case S_ALBUMS: draw_grid_item(i, y, sel); break;
    case S_SONGS: draw_song_row(lib.songs_by_title[i], y, h, sel); break;
    case S_ALBUM: draw_track_row((int)lib.albums[s->arg].first_track + i, y, h, sel); break;
    case S_ARTISTS: draw_artist_row(i, y, h, sel); break;
    case S_ARTIST: draw_album_row((int)lib.artists[s->arg].first_album + i, y, h, sel); break;
    case S_RECENT: draw_album_row(lib.albums_by_added[i], y, h, sel); break;
    }
}

/* ---- screen drawing ----------------------------------------------------------------- */
static void draw_items(const screen_t *s)
{
    int top = content_top(s->kind), bottom = content_bottom(), n = item_count(s);
    gfx_clip(0, top, SCREEN_W, bottom - top);
    int oy = top - s->scroll;
    if (s->kind == S_HOME) {
        mono_label(14, oy + 2, "PINNED", C_LABEL);
        mono_label(14, oy + LIB_LABEL_Y, "LIBRARY", C_LABEL);
    }
    /* only the items in view (lists can be thousands long) */
    int first = 0;
    if (s->kind != S_HOME) {
        int per = s->kind == S_ALBUMS ? GRID_PITCH : list_pitch(s->kind);
        first = (s->scroll / per) * (s->kind == S_ALBUMS ? GRID_COLS : 1);
        first -= s->kind == S_ALBUMS ? GRID_COLS : 1;
        if (first < 0) first = 0;
    }
    for (int i = first; i < n; i++) {
        int y, h;
        item_box(s, i, &y, &h);
        if (oy + y > bottom + 20) break;
        if (oy + y + h + 30 < top) continue;
        draw_item(s, i, oy + y, h, i == s->focus);
    }
    gfx_noclip();
    for (int k = 0; k < FADE; k++) {                     /* soft edges */
        int a = 32 * (FADE - k) / (FADE + 1);
        if (s->scroll > 0) gfx_image_blend(plane, 0, top + k, SCREEN_W, 1, a);
        if (s->scroll < content_total(s) - (bottom - top)) gfx_image_blend(plane, 0, bottom - 1 - k, SCREEN_W, 1, a);
    }
}

static void draw_album_header(int album)
{
    const kidx_album *a = &lib.albums[album];
    album_art(album, KART_GRID, 14, 8, KART_GRID_S, 8);
    gfx_text(&font_row, 92, 12, lib_str(a->title), C_WHITE, 180, 0);
    gfx_text(&font_body, 92, 29, lib_str(a->artist_name), C_SECONDARY, 210, 0);
    char info[48], *p = info;
    uint32_t total = 0;
    for (int i = 0; i < a->n_tracks; i++) total += lib.tracks[a->first_track + i].duration_ms;
    if (a->year) { p = put_uint(p, a->year); p = put_str(p, " \xC2\xB7 "); }
    char cnt[16];
    count_text(cnt, a->n_tracks, "SONG", "SONGS");
    p = put_str(p, cnt);
    p = put_str(p, " \xC2\xB7 ");
    p = put_uint(p, (total + 30000) / 60000);
    put_str(p, " MIN");
    mono_label(92, 50, info, C_LABEL);
    battery(282, 8);
}

/* Knurl's notched bar (NowPlayingScreen.kt Bar): a thin track, white where it has played,
 * dim orange notches at each step, and the step you're on glowing as the playhead. */
static void notched_bar(int x, int y, int w, uint32_t num, uint32_t den, int steps)
{
    if (!den) den = 1;
    if (num > den) num = den;
    gfx_rrect(x, y + 5, w, 4, 2, C_WHITE, ALPHA_PCT(18), 0, 0);
    int pw = (int)((uint64_t)w * num / den);
    if (pw >= 4) gfx_rrect(x, y + 5, pw, 4, 2, C_BONE, 32, 0, 0);
    else if (pw > 0) gfx_fill(x, y + 5, pw, 4, C_BONE, 32);
    int k = (int)(((uint64_t)num * (uint64_t)steps * 2 + den) / (2 * (uint64_t)den));
    for (int i = 0; i <= steps; i++) {
        if (i == k) continue;
        gfx_rrect(x + (w - 3) * i / steps, y + 3, 3, 8, 1, C_ORANGE, ALPHA_PCT(45), 0, 0);
    }
    int px = x + (w - 3) * k / steps;
    gfx_rrect(px - 3, y - 2, 9, 18, 4, C_ORANGE, ALPHA_PCT(18), 0, 0);   /* the glow */
    gfx_rrect(px - 1, y - 1, 5, 16, 2, C_ORANGE, ALPHA_PCT(30), 0, 0);
    gfx_rrect(px, y, 3, 14, 1, RGB(0xFFB27A), 32, 0, 0);
}

/* The volume as a level meter: VOLUME_STEPS - 1 bars rising left to right, filled up to the
 * level, the top filled bar orange. A different shape from the song bar on purpose: Knurl
 * uses one bar for both, which on the iPod made the two hard to tell apart. */
static void volume_meter(int x, int y, int w, int level)
{
    const int n = VOLUME_STEPS - 1, bw = 9;
    for (int i = 0; i < n; i++) {
        int h = 4 + (i * 12) / (n - 1);                     /* 4..16 px */
        int bx = x + (w - bw) * i / (n - 1), by = y + 16 - h;
        if (i < level) gfx_rrect(bx, by, bw, h, 2, i == level - 1 ? C_ORANGE : C_BONE, 32, 0, 0);
        else gfx_rrect(bx, by, bw, h, 2, C_WHITE, ALPHA_PCT(18), 0, 0);
    }
}

/* Text in up to two lines, broken at a space; returns the y below it. */
static int text_2lines(const font_t *f, int x, int y, const char *t, color_t c, int maxw)
{
    if (gfx_text_width(f, t, 0) <= maxw) { gfx_text(f, x, y, t, c, maxw, 0); return y + f->line; }
    char line[128];
    int best = -1, n = 0;
    for (; t[n] && n < (int)sizeof line - 1; n++) {
        if (t[n] == ' ') {
            for (int k = 0; k < n; k++) line[k] = t[k];
            line[n] = 0;
            if (gfx_text_width(f, line, 0) <= maxw) best = n; else break;
        }
    }
    if (best <= 0) { gfx_text(f, x, y, t, c, maxw, 0); return y + f->line; }
    for (int k = 0; k < best; k++) line[k] = t[k];
    line[best] = 0;
    gfx_text(f, x, y, line, c, maxw, 0);
    gfx_text(f, x, y + f->line, t + best + 1, c, maxw, 0);
    return y + 2 * f->line;
}

static void draw_now(void)
{
    gfx_glyph(&font_icons, np.playing ? ICON_PAUSE : ICON_PLAY, 15, 7, C_GLYPH);
    const char *lab = np.scrub ? "NOW PLAYING \xC2\xB7 SCRUBBING" : "NOW PLAYING";
    mono_label((SCREEN_W - gfx_text_width(&font_mono, lab, 2)) / 2, 9, lab, C_LABEL);
    battery(282, 8);
    if (np.track < 0) {
        gfx_rrect(10, 56, 300, 60, 14, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
        gfx_text(&font_row, 24, 76, "Nothing playing", C_WHITE, 270, 0);
        return;
    }
    const kidx_track *t = &lib.tracks[np.track];
    const kidx_album *a = &lib.albums[t->album];

    /* the cover, on a soft shadow */
    for (int i = 6; i >= 1; i--)
        gfx_rrect(NP_ART_X - i, NP_ART_Y - i + 3, KART_COVER_S + 2 * i, KART_COVER_S + 2 * i, 10 + i, 0, 2, 0, 0);
    album_art(t->album, KART_COVER, NP_ART_X, NP_ART_Y, KART_COVER_S, 10);

    /* song, artist, album (with track n/N), each with its symbol */
    const int ix = NP_INFO_X, iw = SCREEN_W - 10 - ix;
    gfx_rrect(ix, NP_ART_Y, iw, KART_COVER_S, 14, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
    const color_t info = RGB(0xD0D0D4), icon = RGB(0xA9A9AC);
    int tx = ix + 30, tw = iw - 30 - 10, y = NP_ART_Y + 14;
    gfx_glyph(&font_icons, ICON_INFO_SONG, ix + 11, y + 2, icon);
    y = text_2lines(&font_row, tx, y, lib_str(t->title), C_WHITE, tw) + 8;
    gfx_glyph(&font_icons, ICON_INFO_ARTIST, ix + 11, y + 1, icon);
    y = text_2lines(&font_body, tx, y, lib_str(t->artist), info, tw) + 8;
    gfx_glyph(&font_icons, ICON_INFO_ALBUM, ix + 11, y + 1, icon);
    y = text_2lines(&font_body, tx, y, lib_str(a->title), info, tw) + 4;
    char nn[24], *p = nn;
    p = put_uint(p, (uint32_t)(np.track - (int)a->first_track + 1));
    *p++ = '/';
    put_uint(p, a->n_tracks);
    if (y + font_mono.line < NP_ART_Y + KART_COVER_S - 8) {
        char yl[32];
        p = put_str(yl, "TRACK ");
        p = put_str(p, nn);
        if (a->year) { p = put_str(p, " \xC2\xB7 "); put_uint(p, a->year); }
        mono_label(tx, y, yl, C_TERTIARY);
    }

    /* the bar: song position, or (just after a wheel turn) the volume */
    /* scrubbing: an orange edge, so it's clear the wheel moves the song, not the volume */
    if (np.scrub) gfx_rrect(10, NP_CARD_Y, 300, NP_CARD_H, 14, C_WHITE, A_GLASS, C_ORANGE, ALPHA_PCT(70));
    else gfx_rrect(10, NP_CARD_Y, 300, NP_CARD_H, 14, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
    const color_t dim = RGB(0xB0B0B5);
    char l[24], rr[24];
    if (np.volume_at && !np.scrub) {
        gfx_glyph(&font_icons, ICON_SPEAKER, 22, NP_CARD_Y + 12, icon);
        gfx_glyph(&font_icons, ICON_SPEAKER_LOUD, 286, NP_CARD_Y + 12, icon);
        volume_meter(42, NP_CARD_Y + 8, 236, np.volume);
        if (np.volume == 0) put_str(rr, "MUTE");
        else { p = put_uint(rr, (uint32_t)np.volume); p = put_str(p, " / "); put_uint(p, VOLUME_STEPS - 1); }
        mono_label(22, NP_CARD_Y + 32, "VOLUME", dim);
        mono_label(298 - gfx_text_width(&font_mono, rr, 2), NP_CARD_Y + 32, rr, dim);
    } else {
        uint32_t dur = t->duration_ms, pos = np.scrub_pending ? np.scrub_ms : np.pos_ms;
        if (pos > dur) pos = dur;
        notched_bar(22, NP_CARD_Y + 11, 276, pos, dur, SCRUB_NOTCHES - 1);
        time_text(l, pos);
        rr[0] = '-';
        time_text(rr + 1, dur - pos);
        mono_label(22, NP_CARD_Y + 32, l, np.scrub ? C_WHITE : dim);
        mono_label(298 - gfx_text_width(&font_mono, rr, 2), NP_CARD_Y + 32, rr, dim);
    }
}

static void draw_diag(void)
{
    header("Knurl", "NO LIBRARY YET");
    gfx_rrect(10, 50, 300, 150, 14, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
    char line[96], *p;
    int y = 62;
    mono_label(24, y, "DISK", C_LABEL);
    p = put_str(line, platform_disk_name());
    uint64_t sectors = platform_disk_sectors();
    if (sectors) { p = put_str(p, ", "); p = put_uint(p, (uint32_t)(sectors / 2000000u)); put_str(p, " GB"); }
    gfx_text(&font_body, 90, y - 1, line, C_WHITE, 210, 0);
    y += 22;
    mono_label(24, y, "VOLUME", C_LABEL);
    gfx_text(&font_body, 90, y - 1, fat_status(), C_WHITE, 210, 0);
    y += 22;
    mono_label(24, y, "LIBRARY", C_LABEL);
    gfx_text(&font_body, 90, y - 1, lib.status, C_WHITE, 210, 0);
    y += 36;
    gfx_text(&font_row, 24, y, "Sync with PodManager", C_WHITE, 270, 0);
    gfx_text(&font_body, 24, y + 18, "to put your library on this iPod.", C_SECONDARY, 270, 0);
}

static void draw(void)
{
    screen_t *s = CUR;
    gfx_noclip();
    gfx_image(plane);
    char label[40];
    switch (s->kind) {
    case S_HOME:
        mono_label(14, 9, "HELLO", C_LABEL);
        battery(282, 8);
        break;
    case S_ALBUMS: count_text(label, lib.n_albums, "ALBUM", "ALBUMS"); header("Albums", label); break;
    case S_ARTISTS: count_text(label, lib.n_artists, "ARTIST", "ARTISTS"); header("Artists", label); break;
    case S_SONGS: count_text(label, lib.n_tracks, "SONG", "SONGS"); header("Songs", label); break;
    case S_RECENT: header("Recently Added", "NEWEST FIRST"); break;
    case S_ARTIST: count_text(label, lib.artists[s->arg].n_tracks, "SONG", "SONGS"); header(lib_str(lib.artists[s->arg].name), label); break;
    case S_ALBUM: draw_album_header(s->arg); break;
    case S_SOON: {
        char *p = put_str(label, "COMING IN MILESTONE ");
        put_uint(p, (uint32_t)s->arg);
        header(s->title, label);
        gfx_rrect(10, 56, 300, 110, 14, C_WHITE, A_GLASS, C_WHITE, A_BORDER);
        gfx_text(&font_row, 24, 70, "Not built yet.", C_WHITE, 270, 0);
        gfx_text(&font_body, 24, 92, "This part of Knurl comes in a later milestone.", C_SECONDARY, 270, 0);
        mono_label(24, 140, "PRESS MENU TO GO BACK", C_LABEL);
        break;
    }
    case S_DIAG: draw_diag(); break;
    case S_NOW: draw_now(); break;
    }
    if (s->kind != S_SOON && s->kind != S_DIAG && s->kind != S_NOW) draw_items(s);
    mini_player();
}

/* ---- behaviour --------------------------------------------------------------------- */
static void push(int kind, int arg, const char *title)
{
    if (depth >= (int)(sizeof stack / sizeof stack[0])) return;
    screen_t *s = &stack[depth++];
    s->kind = kind; s->arg = arg; s->title = title;
    s->focus = 0; s->scroll = 0; s->target = 0;
    dirty_all();
}

/*
 * Playback (src/engine.h). This core only reads files: it streams the queue, track after
 * track, into the decoder core's FIFO, marking where each track starts. The decoder runs
 * ahead of what you hear (the audio ring, 372 ms) and straight on into the next track, so
 * albums play without gaps; it reports the output frame where each track begins and the
 * screen switches when the audio actually gets there.
 */
#define NO_SWITCH UINT32_MAX
static fat_file feed_file;
static int feed_q = -1, feed_open;
static uint32_t feed_seek_ms;              /* the next track opened starts here (0 = from the top) */
static uint8_t seek_buf[2048];

/* Where to start reading f to play from ms in (src/mp3/mp3info.c), and the padding to drop. */
static uint32_t seek_offset(fat_file *f, uint32_t ms, int *hold)
{
    int n = fat_read(f, seek_buf, 10);
    uint32_t start = mp3_id3v2_size(seek_buf, n);
    if (fat_seek(f, start)) { *hold = 0; return 0; }
    n = fat_read(f, seek_buf, sizeof seek_buf);
    return mp3_seek_offset(seek_buf, n < 0 ? 0 : n, start, f->size, ms,
                           np.track >= 0 ? lib.tracks[np.track].duration_ms : 0, hold);
}
static uint32_t heard_start;
static struct { uint32_t frame, start_ms; int q; } sw[8];
static int sw_n;

/* Keep the FIFO topped up. At most 128 KB per call (a few ms of disk) so the UI stays quick. */
static void feed(void)
{
    int budget = 128 * 1024;
    while (feed_q >= 0 && budget > 0) {
        if (!feed_open) {
            if (!engine_marks_free()) return;
            while (feed_q < np.qlen && fat_open(lib_str(lib.tracks[np.queue[feed_q]].path), &feed_file)) feed_q++;
            if (feed_q >= np.qlen) { engine_end(); feed_q = -1; break; }
            if (feed_seek_ms) {
                int hold;
                uint32_t at = seek_offset(&feed_file, feed_seek_ms, &hold);
                fat_seek(&feed_file, at);
                engine_mark_track(feed_q, hold, feed_seek_ms);
                feed_seek_ms = 0;
            } else {
                engine_mark_track(feed_q, -1, 0);
            }
            feed_open = 1;
        }
        uint8_t *p;
        int room = engine_write_space(&p);
        if (room < 16 * 1024) {
            if (room <= 0 || engine_buffered_bytes() > 64 * 1024) return;   /* plenty queued: later */
        }
        if (room > budget) room = budget;
        int n = fat_read(&feed_file, p, (uint32_t)room);
        if (n > 0) { engine_wrote(n); budget -= n; }
        if (n < room) {                                  /* end of this file: on to the next */
            feed_open = 0;
            if (++feed_q >= np.qlen) { engine_end(); feed_q = -1; }
        }
    }
}

static void show_track(int q)
{
    np.qpos = q;
    np.track = np.queue[q];
    np.pos_ms = np.start_ms;
    shown_sec = -1;
    update_plane();
    dirty_all();
}

/* Play queue entry q now (a choice, NEXT/PREV): stop the decoder, drop what's queued, restart. */
static void start_track(int q)
{
    engine_restart();
    sw_n = 0;
    heard_start = 0;
    feed_q = q;
    feed_open = 0;
    feed_seek_ms = 0;
    np.start_ms = 0;
    np.scrub_pending = 0;
    feed();
    engine_go();
    show_track(q);
    np.playing = 1;
    audio_pause(0);
}

/* Carry on from ms into the current track (scrubbing). Keeps playing or paused as it was. */
static void seek_to(uint32_t ms)
{
    if (np.track < 0) return;
    engine_restart();
    sw_n = 0;
    heard_start = 0;
    feed_q = np.qpos;
    feed_open = 0;
    feed_seek_ms = ms ? ms : 1;
    np.start_ms = ms;
    np.pos_ms = ms;
    feed();
    engine_go();
}

static void stop_playback(void)
{
    engine_restart();
    feed_q = -1;
    feed_open = 0;
    sw_n = 0;
    np.playing = 0;
    np.pos_ms = 0;
    dirty_all();
}

static void audio_pump(void)
{
    audio_active(np.playing);
    feed();
    platform_cop_kick();
}

/* Keep the screen in step with what is being heard. */
static void follow_playback(void)
{
    if (np.track < 0) return;
    uint32_t f;
    int q;
    uint32_t ms0;
    while (sw_n < 8 && engine_track_switch(&f, &q, &ms0)) { sw[sw_n].frame = f; sw[sw_n].q = q; sw[sw_n].start_ms = ms0; sw_n++; }
    uint32_t h = engine_heard_frames();
    while (sw_n && h >= sw[0].frame) {
        heard_start = sw[0].frame;
        np.start_ms = sw[0].start_ms;
        if (sw[0].q != np.qpos || np.track != np.queue[sw[0].q]) show_track(sw[0].q);
        for (int i = 1; i < sw_n; i++) sw[i - 1] = sw[i];
        sw_n--;
    }
    np.pos_ms = np.start_ms + (h > heard_start ? (uint32_t)((uint64_t)(h - heard_start) * 1000 / AUDIO_RATE) : 0);
    if (np.playing && engine_finished()) { np.playing = 0; dirty_all(); }
}

/* Play `count` tracks taken from `src` (or tracks first..first+count-1 if src is NULL), from `start`. */
static void play_list(const uint16_t *src, int first, int count, int start)
{
    if (count <= 0) return;
    for (int i = 0; i < count; i++) np.queue[i] = src ? src[first + i] : (uint16_t)(first + i);
    np.qlen = count;
    start_track(start);
}

static void shuffle_all(void)
{
    if (!lib.loaded || !lib.n_tracks) return;
    int n = (int)lib.n_tracks;
    for (int i = 0; i < n; i++) np.queue[i] = (uint16_t)i;
    for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); uint16_t t = np.queue[i]; np.queue[i] = np.queue[j]; np.queue[j] = t; }
    np.qlen = n;
    start_track(0);
    if (CUR->kind != S_NOW) push(S_NOW, 0, 0);
}

static void step_track(int d)
{
    if (np.track < 0) return;
    int q = np.qpos + d;
    if (q < 0) q = 0;
    if (q >= np.qlen) { stop_playback(); return; }
    start_track(q);
}

static void select_item(screen_t *s)
{
    int i = s->focus;
    switch (s->kind) {
    case S_HOME:
        if (i < TILE_N) {
            if (i == 0) push(S_SOON, 6, "Liked Songs");
            else if (i == 3 || !lib.n_albums) shuffle_all();
            else if (i == 1) push(S_ALBUM, np.track >= 0 ? lib.tracks[np.track].album : lib.albums_by_added[0], 0);
            else push(S_RECENT, 0, 0);
        } else {
            if (i - TILE_N >= HOME_ROWS) { push(S_NOW, 0, 0); break; }
            const home_row *r = &home_rows[i - TILE_N];
            if (r->screen < 0) shuffle_all();
            else if (r->screen == S_SOON) push(S_SOON, r->milestone, r->label);
            else push(r->screen, 0, 0);
        }
        break;
    case S_ALBUMS: push(S_ALBUM, lib.albums_by_title[i], 0); break;
    case S_RECENT: push(S_ALBUM, lib.albums_by_added[i], 0); break;
    case S_ARTISTS: push(S_ARTIST, i, 0); break;
    case S_ARTIST: push(S_ALBUM, (int)lib.artists[s->arg].first_album + i, 0); break;
    case S_ALBUM: { const kidx_album *a = &lib.albums[s->arg]; play_list(0, (int)a->first_track, a->n_tracks, i); push(S_NOW, 0, 0); break; }
    case S_SONGS: play_list(lib.songs_by_title, 0, (int)lib.n_tracks, i); push(S_NOW, 0, 0); break;
    }
}

static void handle(const input_t *in)
{
    screen_t *s = CUR;
    if (in->pressed & BTN_PLAY) {
        if (np.track >= 0) {
            if (!np.playing && engine_finished()) start_track(np.qpos);   /* finished: play again */
            else { np.playing = !np.playing; audio_pause(!np.playing); }
            if (s->kind == S_NOW) dirty(0, 0, SCREEN_W, 26);
            else dirty(10, MINI_Y, 300, MINI_H);
        }
        else shuffle_all();
    }
    if (in->pressed & BTN_NEXT) step_track(1);
    if (in->pressed & BTN_PREV) {
        if (np.pos_ms > 3000) start_track(np.qpos);
        else step_track(-1);
    }
    if (s->kind == S_NOW) {
        if (in->pressed & BTN_MENU) {
            if (np.scrub) { np.scrub = 0; dirty_all(); }       /* Menu leaves scrubbing first */
            else if (depth > 1) { depth--; dirty_all(); aim_scroll(CUR); }
            return;
        }
        if (in->pressed & BTN_SELECT) { np.scrub = !np.scrub; np.volume_at = 0; dirty_all(); }
        if (in->wheel && np.track >= 0) {
            if (np.scrub) {
                uint32_t dur = lib.tracks[np.track].duration_ms;
                int32_t step = (int32_t)(dur / (SCRUB_NOTCHES - 1));
                if (step < 1000) step = 1000;
                int64_t t = (int64_t)(np.scrub_pending ? np.scrub_ms : np.pos_ms) + (int64_t)in->wheel * step;
                if (t < 0) t = 0;
                if (dur > 1000 && t > (int64_t)dur - 1000) t = dur - 1000;
                np.scrub_ms = (uint32_t)t;
                np.scrub_pending = 1;
                np.scrub_at = last_ms;
            } else {
                int v = np.volume + in->wheel;
                if (v < 0) v = 0;
                if (v > VOLUME_STEPS - 1) v = VOLUME_STEPS - 1;
                np.volume = v;
                audio_set_volume(volume_db(v));
                np.volume_at = last_ms ? last_ms : 1;
            }
            dirty(10, NP_CARD_Y, 300, NP_CARD_H);
        }
        return;
    }
    if ((in->pressed & BTN_MENU) && depth > 1) { depth--; dirty_all(); aim_scroll(CUR); return; }

    int n = item_count(s);
    if (in->wheel && n) {
        int f = s->focus + in->wheel;
        if (f < 0) f = 0;
        if (f > n - 1) f = n - 1;
        if (f != s->focus) {
            s->focus = f;
            aim_scroll(s);
            dirty(0, content_top(s->kind), SCREEN_W, content_bottom() - content_top(s->kind));
        }
    }
    if ((in->pressed & BTN_SELECT) && n) select_item(s);
}

static int animate(void)
{
    screen_t *s = CUR;
    int d = s->target - s->scroll;
    if (!d) return 0;
    int step = d / 3;
    if (!step) step = d > 0 ? 1 : -1;
    s->scroll += step;
    dirty(0, content_top(s->kind), SCREEN_W, content_bottom() - content_top(s->kind));
    return s->scroll != s->target;
}

void app_init(uint32_t now)
{
    last_ms = anim_ms = now;
    rng_state ^= now * 2654435761u;
    plane = mem_alloc(SCREEN_W * SCREEN_H * 2);
    boot_log("volume: mounting");
    if (fat_mount() == 0) {
        boot_log(fat_status());
        boot_log("library: loading");
        library_load();
    }
    boot_log(lib.loaded ? "library: loaded" : lib.status);
    np.queue = lib.loaded ? mem_alloc(sizeof(uint16_t) * (lib.n_tracks ? lib.n_tracks : 1)) : 0;
    depth = 0;
    push(lib.loaded ? S_HOME : S_DIAG, 0, 0);
    CUR->focus = TILE_N + 1;               /* Albums */
    if (!lib.loaded) CUR->focus = 0;
    aim_scroll(CUR);
    CUR->scroll = CUR->target;
    boot_log("drawing");
    update_plane();
}

int app_step(const input_t *in, uint32_t now)
{
    dx0 = dy0 = dx1 = dy1 = 0;
    if (first) { first = 0; dirty_all(); }

    uint32_t elapsed = now - last_ms;
    last_ms = now;
    int had_mini = has_mini();
    (void)elapsed;
    follow_playback();
    if (in && in->hold != shown_lock) { shown_lock = in->hold; dirty(256, 2, 64, 22); }
    if (in) handle(in);
    audio_pump();
    if (has_mini() != had_mini) { dirty_all(); aim_scroll(CUR); }

    int moving = 0;
    while (now - anim_ms >= 16) { anim_ms += 16; moving = animate(); }
    if (!moving) anim_ms = now;
    moving = CUR->scroll != CUR->target;

    if (np.scrub_pending && now - np.scrub_at >= SCRUB_REST_MS) {
        np.scrub_pending = 0;
        seek_to(np.scrub_ms);
    }
    if (np.volume_at && now - np.volume_at >= VOLUME_SHOW_MS) {
        np.volume_at = 0;
        if (CUR->kind == S_NOW) dirty(10, NP_CARD_Y, 300, NP_CARD_H);
    }
    int batt = platform_battery() / 4 * 2 + platform_charging();   /* icon steps (4%) + charge */
    if (batt != shown_batt) { shown_batt = batt; dirty(276, 4, 44, 18); }
    if (np.track >= 0) {
        uint32_t dur = lib.tracks[np.track].duration_ms ? lib.tracks[np.track].duration_ms : 1;
        int sec = (int)(np.pos_ms / 1000), prog = (int)((uint64_t)(np.pos_ms > dur ? dur : np.pos_ms) * 280 / dur);
        if (sec != shown_sec || prog != shown_prog) {
            shown_sec = sec;
            shown_prog = prog;
            if (CUR->kind == S_NOW) dirty(10, NP_CARD_Y, 300, NP_CARD_H);
            else dirty(10, MINI_Y, 300, MINI_H);
        }
    }

    if (dx1 > dx0) {
        gfx_bounds(dx0, dy0, dx1 - dx0, dy1 - dy0);
        draw();
        gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
        platform_present(dx0, dy0, dx1 - dx0, dy1 - dy0);
    }
    return moving;
}
