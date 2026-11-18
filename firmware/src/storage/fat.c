#include "storage/fat.h"
#include "platform.h"

/* Geometry, in 512-byte disk sectors unless named otherwise. */
static struct {
    uint32_t part_lba;            /* first sector of the FAT32 volume */
    uint32_t bps;                 /* bytes per FAT sector (512, 1024, 2048 or 4096) */
    uint32_t spc;                 /* FAT sectors per cluster */
    uint32_t fat_lba;             /* first sector of the first FAT */
    uint32_t data_lba;            /* first sector of cluster 2 */
    uint32_t root_cluster;
    uint32_t cluster_sectors;     /* 512-byte sectors per cluster */
    uint32_t cluster_bytes;
    uint32_t n_clusters;
    int      mounted;
} v;

static const char *status = "not mounted";
const char *fat_status(void) { return status; }
uint32_t fat_cluster_bytes(void) { return v.cluster_bytes; }

static uint8_t  sec[512] __attribute__((aligned(4)));   /* one-sector cache for partial reads */
static uint32_t sec_lba = 0xFFFFFFFF;
static uint8_t  fatsec[512] __attribute__((aligned(4)));/* one-sector FAT cache */
static uint32_t fatsec_lba = 0xFFFFFFFF;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int read_sector(uint32_t lba)
{
    if (lba == sec_lba) return 0;
    if (platform_disk_read(lba, 1, sec)) { sec_lba = 0xFFFFFFFF; return -1; }
    sec_lba = lba;
    return 0;
}

/* Is the sector a FAT32 boot sector? Fills the geometry if so. */
static int try_boot_sector(uint32_t lba)
{
    if (read_sector(lba)) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    uint32_t bps = rd16(sec + 11), spc = sec[13], reserved = rd16(sec + 14), nfats = sec[16];
    uint32_t fatsz = rd32(sec + 36), total = rd32(sec + 32);
    if (rd16(sec + 22) != 0 || fatsz == 0) return 0;         /* FAT12/16 or garbage */
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1)) || !nfats)
        return 0;
    uint32_t f = bps / 512;
    v.part_lba = lba;
    v.bps = bps;
    v.spc = spc;
    v.fat_lba = lba + reserved * f;
    v.data_lba = v.fat_lba + nfats * fatsz * f;
    v.root_cluster = rd32(sec + 44);
    v.cluster_sectors = spc * f;
    v.cluster_bytes = v.cluster_sectors * 512;
    v.n_clusters = (total - (reserved + nfats * fatsz)) / spc;
    return 1;
}

/* The partition table's unit in 512-byte sectors: 1, or 4 on iPods formatted by iTunes
 * (2048-byte sectors). Disk mode shows the computer blocks of this size. */
static int mbr_units = 1;
int fat_mbr_units(void) { return mbr_units; }

int fat_mount(void)
{
    v.mounted = 0;
    if (read_sector(0)) { status = "can't read the disk"; return -1; }
    if (try_boot_sector(0)) { v.mounted = 1; status = "FAT32 (no partition table)"; return 0; }
    if (read_sector(0) || sec[510] != 0x55 || sec[511] != 0xAA) { status = "no partition table"; return -2; }
    uint8_t table[64];
    for (int i = 0; i < 64; i++) table[i] = sec[446 + i];
    for (int p = 0; p < 4; p++) {
        const uint8_t *e = table + p * 16;
        uint32_t start = rd32(e + 8);
        if (!start || e[4] == 0) continue;
        /* iPods formatted by iTunes give partition starts in 2048-byte units; try both. */
        if (try_boot_sector(start) || (try_boot_sector(start * 4) && (mbr_units = 4))) {
            v.mounted = 1;
            status = "FAT32";
            return 0;
        }
    }
    status = "no FAT32 partition";
    return -3;
}

static uint32_t cluster_lba(uint32_t c) { return v.data_lba + (c - 2) * v.cluster_sectors; }

/* Next cluster in the chain; 0 at the end or on error. */
static uint32_t next_cluster(uint32_t c)
{
    uint32_t off = c * 4;
    uint32_t lba = v.fat_lba + off / 512;
    if (lba != fatsec_lba) {
        if (platform_disk_read(lba, 1, fatsec)) { fatsec_lba = 0xFFFFFFFF; return 0; }
        fatsec_lba = lba;
    }
    uint32_t n = rd32(fatsec + (off & 511)) & 0x0FFFFFFF;
    if (n < 2 || n >= v.n_clusters + 2) return 0;          /* end of chain (>= 0x0FFFFFF8) or bad */
    return n;
}

/* Position f->cluster on the cluster holding f->pos. */
static int locate(fat_file *f)
{
    uint32_t want = f->pos / v.cluster_bytes;
    if (want < f->cluster_idx || !f->cluster) {
        f->cluster = f->first_cluster;
        f->cluster_idx = 0;
    }
    while (f->cluster_idx < want) {
        uint32_t n = next_cluster(f->cluster);
        if (!n) return -1;
        f->cluster = n;
        f->cluster_idx++;
    }
    return 0;
}

int fat_seek(fat_file *f, uint32_t pos)
{
    f->pos = pos > f->size ? f->size : pos;
    return 0;
}

int fat_file_lba(fat_file *f, uint32_t pos, uint32_t *lba, uint32_t *run)
{
    if (!v.mounted || pos >= f->size) return -1;
    f->pos = pos;
    if (locate(f)) return -1;
    uint32_t off = pos % v.cluster_bytes;
    *lba = cluster_lba(f->cluster) + off / 512;
    *run = (v.cluster_bytes - off) / 512;
    return 0;
}

void fat_forget(void) { sec_lba = fatsec_lba = 0xFFFFFFFF; }

int fat_read(fat_file *f, void *buf, uint32_t n)
{
    uint8_t *dst = buf;
    uint32_t done = 0;
    if (!v.mounted) return -1;
    if (f->pos >= f->size) return 0;
    if (n > f->size - f->pos) n = f->size - f->pos;
    while (done < n) {
        if (locate(f)) return f->size == 0xFFFFFFFF ? (int)done : -1;  /* a directory just ends */
        uint32_t off = f->pos % v.cluster_bytes;
        uint32_t lba = cluster_lba(f->cluster) + off / 512;
        uint32_t within = off & 511;
        uint32_t left = n - done;
        if (!within && left >= 512 && !((uintptr_t)(dst + done) & 1)) {
            /* whole sectors straight into the caller's buffer, up to the end of the cluster */
            uint32_t count = left / 512, room = (v.cluster_bytes - off) / 512;
            if (count > room) count = room;
            if (platform_disk_read(lba, count, dst + done)) return -1;
            done += count * 512;
            f->pos += count * 512;
        } else {
            if (read_sector(lba)) return -1;
            uint32_t k = 512 - within;
            if (k > left) k = left;
            for (uint32_t i = 0; i < k; i++) dst[done + i] = sec[within + i];
            done += k;
            f->pos += k;
        }
    }
    return (int)done;
}

/* ---- directories ------------------------------------------------------------- */

static unsigned utf8_next(const char *a, int alen, int *i)
{
    unsigned c = (unsigned char)a[(*i)++];
    if (c >= 0xF0 && *i + 2 < alen + 1) {
        c = ((c & 7) << 18) | ((a[*i] & 63u) << 12) | ((a[*i + 1] & 63u) << 6) | (a[*i + 2] & 63u); *i += 3;
    } else if (c >= 0xE0 && *i + 1 < alen + 1) {
        c = ((c & 15) << 12) | ((a[*i] & 63u) << 6) | (a[*i + 1] & 63u); *i += 2;
    } else if (c >= 0xC0 && *i < alen) {
        c = ((c & 31) << 6) | (a[*i] & 63u); *i += 1;
    }
    return c;
}

/* A UTF-8 name equals a UTF-16 name? ASCII letters compare case-insensitively, like Windows. */
static int name_eq(const char *a, int alen, const uint16_t *b, int blen)
{
    int i = 0, j = 0;
    while (i < alen && j < blen) {
        unsigned c = utf8_next(a, alen, &i);
        unsigned d = b[j++];
        if (d >= 0xD800 && d < 0xDC00 && j < blen) d = 0x10000 + ((d - 0xD800) << 10) + (b[j++] - 0xDC00u);
        if (c >= 'A' && c <= 'Z') c += 32;
        if (d >= 'A' && d <= 'Z') d += 32;
        if (c != d) return 0;
    }
    return i == alen && j == blen;
}

static uint8_t lfn_checksum(const uint8_t *e)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* Find `name` (len bytes of UTF-8) in the directory whose chain starts at `dir`. */
static int find_entry(uint32_t dir, const char *name, int len, uint32_t *cluster, uint32_t *size, int *is_dir)
{
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    fat_file d = { dir, 0xFFFFFFFF, 0, 0, 0 };
    uint16_t lfn[256];
    int lfn_len = 0, lfn_ok = 0;
    uint8_t lfn_sum = 0, e[32];
    for (;;) {
        if (fat_read(&d, e, 32) != 32) return -1;
        if (e[0] == 0) return -1;                             /* end of directory */
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }           /* deleted */
        if (e[11] == 0x0F) {                                  /* a piece of a long name (they come last piece first) */
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { lfn_ok = 1; lfn_sum = e[13]; lfn_len = seq * 13 > 255 ? 255 : seq * 13; }
            if (!lfn_ok || seq < 1 || seq > 20 || e[13] != lfn_sum) { lfn_ok = 0; continue; }
            for (int k = 0; k < 13; k++) {
                uint16_t ch = rd16(e + pos[k]);
                int at = (seq - 1) * 13 + k;
                if (ch == 0x0000 || ch == 0xFFFF) { if (at < lfn_len) lfn_len = at; break; }
                if (at < 255) lfn[at] = ch;
            }
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; }           /* volume label */
        int match;
        if (lfn_ok && lfn_len > 0 && lfn_checksum(e) == lfn_sum) {
            match = name_eq(name, len, lfn, lfn_len);
        } else {                                              /* plain 8.3 name */
            uint16_t sn[13];
            int n = 0;
            for (int k = 0; k < 8 && e[k] != ' '; k++) sn[n++] = e[k];
            if (e[8] != ' ') {
                sn[n++] = '.';
                for (int k = 8; k < 11 && e[k] != ' '; k++) sn[n++] = e[k];
            }
            match = name_eq(name, len, sn, n);
        }
        lfn_ok = 0;
        if (match) {
            *cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
            *size = rd32(e + 28);
            *is_dir = (e[11] & 0x10) != 0;
            return 0;
        }
    }
}

int fat_open(const char *path, fat_file *f)
{
    if (!v.mounted) return -1;
    uint32_t cl = v.root_cluster, size = 0xFFFFFFFF;
    int is_dir = 1;
    while (*path == '/') path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/') end++;
        if (!is_dir) return -1;
        if (find_entry(cl, path, (int)(end - path), &cl, &size, &is_dir)) return -1;
        if (is_dir) { size = 0xFFFFFFFF; if (!cl) cl = v.root_cluster; }
        path = end;
        while (*path == '/') path++;
    }
    f->first_cluster = cl;
    f->size = size;
    f->pos = 0;
    f->cluster = 0;
    f->cluster_idx = 0;
    return 0;
}
