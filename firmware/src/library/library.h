/*
 * The music library, loaded whole into RAM from /.knurl/library.bin, plus on-demand
 * artwork from /.knurl/art.bin (with small caches, since screens redraw often).
 */
#pragma once
#include <stdint.h>
#include "library/kidx.h"

typedef struct {
    int loaded;
    const char *status;            /* for the diagnostics screen */
    uint32_t n_tracks, n_albums, n_artists;
    const kidx_track  *tracks;
    const kidx_album  *albums;
    const kidx_artist *artists;
    const uint16_t *songs_by_title, *albums_by_title, *albums_by_added;
    const char *strings;
    uint32_t strings_size;
    int have_art;
    uint32_t built_unix;           /* when PodManager wrote the index */
} library_t;

extern library_t lib;

int         library_load(void);           /* 0 = ok; lib.status says why not */
const char *lib_str(uint32_t off);        /* never NULL */

/* Artwork for an album (KART_THUMB/TILE/GRID/COVER as RGB565, KART_WASH as RGB888).
 * Returns NULL if the album has no art or it can't be read. The pointer stays valid until
 * the same cache slot is reused, i.e. long enough to draw one frame. */
const void *lib_art(int album, int kind);
/* The same, but only if it's already in memory (no disk read): for drawing while moving fast. */
const void *lib_art_cached(int album, int kind);
