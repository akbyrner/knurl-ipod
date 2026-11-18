#include "library/library.h"
#include "storage/fat.h"
#include "mem.h"

library_t lib = { 0, "not loaded", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#define MAX_INDEX (16u << 20)

static int str_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int in_file(uint32_t off, uint32_t len, uint32_t size) { return off <= size && len <= size - off; }

const char *lib_str(uint32_t off) { return off < lib.strings_size ? lib.strings + off : ""; }

static fat_file art_file;
static kart_header art_hdr;

int library_load(void)
{
    fat_file f;
    lib.loaded = 0;
    if (fat_open("/.knurl/library.bin", &f)) { lib.status = "no /.knurl/library.bin (sync with PodManager)"; return -1; }
    if (f.size < sizeof(kidx_header) || f.size > MAX_INDEX) { lib.status = "library.bin has a bad size"; return -2; }
    uint8_t *data = mem_alloc(f.size);
    if (!data) { lib.status = "library.bin is too big for memory"; return -3; }
    if (fat_read(&f, data, f.size) != (int)f.size) { lib.status = "couldn't read library.bin"; return -4; }

    const kidx_header *h = (const kidx_header *)data;
    if (!str_eq(h->magic, KIDX_MAGIC, 8)) { lib.status = "library.bin isn't a Knurl index"; return -5; }
    if (h->version != KIDX_VERSION) { lib.status = "library.bin is a different version (update PodManager or the firmware)"; return -6; }
    uint32_t sz = f.size;
    if (h->file_size != sz || h->n_tracks > 65535 || h->n_albums > 65535 ||
        !in_file(h->tracks_off, h->n_tracks * sizeof(kidx_track), sz) ||
        !in_file(h->albums_off, h->n_albums * sizeof(kidx_album), sz) ||
        !in_file(h->artists_off, h->n_artists * sizeof(kidx_artist), sz) ||
        !in_file(h->songs_by_title_off, h->n_tracks * 2, sz) ||
        !in_file(h->albums_by_title_off, h->n_albums * 2, sz) ||
        !in_file(h->albums_by_added_off, h->n_albums * 2, sz) ||
        !in_file(h->strings_off, h->strings_size, sz) || !h->strings_size ||
        (h->tracks_off | h->albums_off | h->artists_off | h->songs_by_title_off |
         h->albums_by_title_off | h->albums_by_added_off) & 1) {
        lib.status = "library.bin is damaged";
        return -7;
    }
    lib.tracks = (const kidx_track *)(data + h->tracks_off);
    lib.albums = (const kidx_album *)(data + h->albums_off);
    lib.artists = (const kidx_artist *)(data + h->artists_off);
    lib.songs_by_title = (const uint16_t *)(data + h->songs_by_title_off);
    lib.albums_by_title = (const uint16_t *)(data + h->albums_by_title_off);
    lib.albums_by_added = (const uint16_t *)(data + h->albums_by_added_off);
    lib.strings = (const char *)(data + h->strings_off);
    lib.strings_size = h->strings_size;
    ((char *)lib.strings)[h->strings_size - 1] = 0;       /* strings can't run off the end */

    /* Cross-references must stay in range, so the UI never has to check. */
    for (uint32_t i = 0; i < h->n_tracks; i++)
        if (lib.tracks[i].album >= h->n_albums) { lib.status = "library.bin: bad album reference"; return -8; }
    for (uint32_t i = 0; i < h->n_albums; i++) {
        const kidx_album *a = &lib.albums[i];
        if (a->artist >= h->n_artists || a->first_track + a->n_tracks > h->n_tracks) { lib.status = "library.bin: bad album"; return -8; }
    }
    for (uint32_t i = 0; i < h->n_artists; i++)
        if (lib.artists[i].first_album + lib.artists[i].n_albums > h->n_albums) { lib.status = "library.bin: bad artist"; return -8; }
    for (uint32_t i = 0; i < h->n_tracks; i++)
        if (lib.songs_by_title[i] >= h->n_tracks) { lib.status = "library.bin: bad song order"; return -8; }
    for (uint32_t i = 0; i < h->n_albums; i++)
        if (lib.albums_by_title[i] >= h->n_albums || lib.albums_by_added[i] >= h->n_albums) { lib.status = "library.bin: bad album order"; return -8; }

    lib.n_tracks = h->n_tracks;
    lib.n_albums = h->n_albums;
    lib.n_artists = h->n_artists;
    lib.loaded = 1;
    lib.status = "loaded";

    lib.have_art = 0;
    if (!fat_open("/.knurl/art.bin", &art_file) && fat_read(&art_file, &art_hdr, sizeof art_hdr) == (int)sizeof art_hdr &&
        str_eq(art_hdr.magic, KART_MAGIC, 8) && art_hdr.version == KART_VERSION && art_hdr.entry_size <= (1u << 20))
        lib.have_art = 1;
    return 0;
}

/* ---- artwork caches ------------------------------------------------------------- */

static const uint32_t art_bytes[KART_KINDS] = {
    KART_THUMB_S * KART_THUMB_S * 2, KART_TILE_S * KART_TILE_S * 2, KART_GRID_S * KART_GRID_S * 2,
    KART_COVER_S * KART_COVER_S * 2, KART_WASH_W * KART_WASH_H * 3,
};
static const int art_slots[KART_KINDS] = { 32, 16, 48, 2, 4 };

typedef struct { uint8_t *data; int32_t *album; int next; } art_cache;
static art_cache caches[KART_KINDS];

const void *lib_art(int album, int kind)
{
    if (!lib.loaded || !lib.have_art || album < 0 || (uint32_t)album >= lib.n_albums || kind < 0 || kind >= KART_KINDS)
        return 0;
    uint16_t entry = lib.albums[album].art;
    if (entry == KIDX_NONE || entry >= art_hdr.n_entries) return 0;
    art_cache *c = &caches[kind];
    if (!c->data) {
        c->data = mem_alloc(art_bytes[kind] * art_slots[kind]);
        c->album = mem_alloc(sizeof(int32_t) * art_slots[kind]);
        if (!c->data || !c->album) { c->data = 0; return 0; }
        for (int i = 0; i < art_slots[kind]; i++) c->album[i] = -1;
    }
    for (int i = 0; i < art_slots[kind]; i++)
        if (c->album[i] == album) return c->data + art_bytes[kind] * i;
    int slot = c->next;
    c->next = (c->next + 1) % art_slots[kind];
    uint8_t *dst = c->data + art_bytes[kind] * slot;
    c->album[slot] = -1;
    if (fat_seek(&art_file, art_hdr.data_off + entry * art_hdr.entry_size + art_hdr.off[kind]) ||
        fat_read(&art_file, dst, art_bytes[kind]) != (int)art_bytes[kind])
        return 0;
    c->album[slot] = album;
    return dst;
}
