/*
 * Writing into files on the music disk without writing the file system.
 *
 * Knurl's FAT code is read-only and stays that way: a bug in a FAT writer can wreck the whole
 * library. Knurl only needs to keep a few small records (the battery log, the boot loader
 * install report, its backup), so the computer creates those files at their full size
 * (zero-filled: FAT has no sparse files, so every cluster is really allocated), and Knurl
 * overwrites sectors inside them in place. The directory entry, the FAT and the file's size
 * never change; the computer simply reads the new contents the next time it mounts the disk.
 *
 * Writes are whole 512-byte sectors at sector-aligned offsets. CPU main loop only (the same
 * place every disk read happens), never during disk mode.
 */
#include "hw.h"
#include "platform.h"
#include "dev.h"
#include "storage/fat.h"
#include "store.h"

int store_open(const char *path, fat_file *f, uint32_t min_size)
{
    if (fat_open(path, f)) return -1;
    if (f->size == 0xFFFFFFFF || f->size < min_size || f->size < 512) return -2;
    return 0;
}

/* Sectors at byte offset pos (512-aligned), n bytes (a multiple of 512), all inside the file. */
static int span(fat_file *f, uint32_t pos, uint32_t n, const void *wbuf, void *rbuf)
{
    if ((pos & 511) || (n & 511) || pos > f->size || n > f->size - pos) return -1;
    if (((uintptr_t)wbuf | (uintptr_t)rbuf) & 3) return -1;
    const uint8_t *w = wbuf;
    uint8_t *r = rbuf;
    while (n) {
        uint32_t lba, run;
        if (fat_file_lba(f, pos, &lba, &run)) return -1;
        uint32_t k = n / 512;
        if (k > run) k = run;
        if (k > 128) k = 128;
        if (w ? ata_write(lba, k, w) : platform_disk_read(lba, k, r)) return -1;
        if (w) w += k * 512; else r += k * 512;
        pos += k * 512;
        n -= k * 512;
    }
    return 0;
}

int store_write(fat_file *f, uint32_t pos, const void *buf, uint32_t n)
{
    int r = span(f, pos, n, buf, 0);
    fat_forget();
    return r;
}

int store_read(fat_file *f, uint32_t pos, void *buf, uint32_t n)
{
    return span(f, pos, n, 0, buf);
}

/* ---- settings: /.knurl/settings.bin, one sector (app.c decides what's in it) ---------------- */
static fat_file settings_file;
static int settings_open;

int platform_settings_load(void *buf)
{
    settings_open = 0;
    if (store_open("/.knurl/settings.bin", &settings_file, 512) || store_read(&settings_file, 0, buf, 512)) return -1;
    settings_open = 1;
    return 0;
}

int platform_settings_save(const void *buf)
{
    if (!settings_open) return -1;
    return store_write(&settings_file, 0, buf, 512) || ata_flush() ? -1 : 0;
}
