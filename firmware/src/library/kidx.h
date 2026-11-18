/*
 * The library index the iPod reads: /.knurl/library.bin and /.knurl/art.bin.
 * Written by PodManager (PodManager.Core/KnurlIndex.cs) at the end of every sync, so the
 * iPod never scans folders, reads tags or decodes images. All integers little-endian.
 * Keep this file and KnurlIndex.cs in step; bump the version on any layout change.
 */
#pragma once
#include <stdint.h>

#define KIDX_MAGIC   "KNURLIDX"
#define KIDX_VERSION 1
#define KIDX_NONE    0xFFFF

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t file_size;
    uint32_t n_tracks, n_albums, n_artists;
    uint32_t tracks_off;           /* kidx_track[n_tracks]: grouped by album (album order), disc/track order */
    uint32_t albums_off;           /* kidx_album[n_albums]: by album-artist sort name, then year, then title */
    uint32_t artists_off;          /* kidx_artist[n_artists]: by sort name ("The" ignored) */
    uint32_t songs_by_title_off;   /* uint16_t[n_tracks] */
    uint32_t albums_by_title_off;  /* uint16_t[n_albums] */
    uint32_t albums_by_added_off;  /* uint16_t[n_albums], newest first */
    uint32_t strings_off;          /* UTF-8, NUL-terminated; offset 0 is "" */
    uint32_t strings_size;
    uint32_t built_unix;
} kidx_header;                     /* 64 bytes */

typedef struct {
    uint32_t title, artist;        /* string offsets (track artist) */
    uint32_t path;                 /* '/'-separated, from the volume root, e.g. "Music/A/B (2001)/01 - C.mp3" */
    uint32_t duration_ms;
    uint32_t size;                 /* file size in bytes */
    uint16_t album;
    uint8_t  number, disc;
} kidx_track;                      /* 24 bytes */

typedef struct {
    uint32_t title, artist_name;   /* string offsets */
    uint16_t artist, year;         /* artist index (album artist) */
    uint32_t first_track;
    uint16_t n_tracks, art;        /* art: entry in art.bin, or KIDX_NONE */
    uint32_t color0, color1;       /* 0xRRGGBB, from the cover (top-left / bottom-right), for gradients */
    uint32_t added_unix;
} kidx_album;                      /* 32 bytes */

typedef struct {
    uint32_t name;
    uint32_t first_album;          /* the artist's albums are contiguous in album order */
    uint16_t n_albums, n_tracks;
} kidx_artist;                     /* 12 bytes */

/* art.bin: header, then one fixed-size entry per album with art, at data_off + i * entry_size.
 * Images are RGB565 little-endian, Bayer-dithered, square; the wash is RGB888, 40x30. */
#define KART_MAGIC   "KNURLART"
#define KART_VERSION 1
enum { KART_THUMB, KART_TILE, KART_GRID, KART_COVER, KART_WASH, KART_KINDS };
#define KART_THUMB_S 24            /* mini player */
#define KART_TILE_S  54            /* pinned shelf */
#define KART_GRID_S  66            /* albums grid, album header */
#define KART_COVER_S 140           /* now playing */
#define KART_WASH_W  40
#define KART_WASH_H  30

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t n_entries;
    uint32_t entry_size;
    uint32_t data_off;
    uint32_t off[KART_KINDS];      /* where each image sits inside an entry */
    uint32_t reserved[5];
} kart_header;                     /* 64 bytes */
