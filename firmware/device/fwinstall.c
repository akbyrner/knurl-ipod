/*
 * Knurl installs its own boot loader (boot/, build/knurlboot.ipod) into the iPod's firmware
 * partition, in place of the Rockbox boot loader.
 *
 * The firmware partition (the first partition) holds Apple's image directory and the "osos"
 * image the boot ROM loads into SDRAM at 0x10000000. A boot loader is appended to Apple's
 * firmware inside osos and the directory's entryOffset points at it, so the ROM jumps there;
 * Apple's firmware stays where it was and the loader can still start it. This is the layout
 * Rockbox's ipodpatcher writes (reference/rockbox/utils/ipodpatcher: read_directory,
 * add_bootloader); we only ever replace an already-installed loader, in its existing place:
 *
 *   loader bytes at  fwoffset + devOffset + entryOffset     (fwoffset = partition + 1 sector)
 *   directory entry  len = entryOffset + loader length, chksum = byte sum of the whole image
 *
 * It runs only when /.knurl/knurlboot.ipod exists, and refuses (with a report in
 * /.knurl/install.log) unless everything checks out:
 *   - the file is a valid .ipod image for this model;
 *   - THAT loader started this very Knurl (boot/ leaves a stamp with its checksum): so the
 *     loader is proven on this iPod before it ever goes into the firmware partition. To
 *     prove it, install it once as .rockbox/rockbox.ipod under the old loader;
 *   - the directory parses, the osos image is first and already has a loader appended, the
 *     new loader fits before the next image, and the stored checksum equals Apple's bytes plus
 *     the old loader's bytes (so our reading of the format is right on this disk).
 * Every write is read back and compared. Then the iPod restarts through the new loader.
 * No backup is kept: if the loader ever fails, HOLD on at start gives Apple's firmware.
 * Every boot shows which loader started Knurl.
 */
#include <stdarg.h>
#include "hw.h"
#include "platform.h"
#include "dev.h"
#include "shared.h"
#include "storage/fat.h"
#include "store.h"

void boot_log(const char *line);

#define MODEL_NUM   5
#define LOADER_MAX  (32 * 1024)         /* boot/link.ld: the loader runs from 32 KB of IRAM */
#define OLD_MAX     (256 * 1024)           /* the loader being replaced, read to compare */
#define STAMP       ((volatile uint32_t *)0x11F00000)   /* boot/boot.c: physical, above Knurl's 30 MB */
#define STAMP_MAGIC 0x4B4E4254u         /* "KNBT" */

static uint32_t booted_by;              /* checksum of the Knurl loader that started us; 0 = other */

void boot_stamp_read(void)
{
    if (STAMP[0] == STAMP_MAGIC) booted_by = STAMP[1];
    STAMP[0] = 0;                       /* a warm restart through another loader must not see it */
    STAMP[1] = 0;
}

/* ---- the report ------------------------------------------------------------------------ */

static char report[4096] __attribute__((aligned(4)));
static int report_len;

static char *put_u(char *p, uint32_t v)
{
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static char *put_x(char *p, uint32_t v)
{
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[(v >> i) & 15];
    return p;
}

/* %s, %u, %x, and %t: a rate, bytes and microseconds -> "12.3 MB/s". Shown and kept. */
static void say(const char *fmt, ...)
{
    char line[160], *p = line, *end = line + sizeof line - 24;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') { *p++ = *fmt; continue; }
        switch (*++fmt) {
        case 's': for (const char *s = va_arg(ap, const char *); *s && p < end; ) *p++ = *s++; break;
        case 'u': p = put_u(p, va_arg(ap, uint32_t)); break;
        case 'x': p = put_x(p, va_arg(ap, uint32_t)); break;
        case 't': {
            uint32_t bytes = va_arg(ap, uint32_t), us = va_arg(ap, uint32_t);
            uint32_t tenths = us ? (uint32_t)((uint64_t)bytes * 10 / us) : 0;   /* bytes/us = MB/s */
            p = put_u(p, tenths / 10); *p++ = '.'; p = put_u(p, tenths % 10);
            *p++ = ' '; *p++ = 'M'; *p++ = 'B'; *p++ = '/'; *p++ = 's';
            break;
        }
        default: *p++ = '?'; break;
        }
    }
    va_end(ap);
    *p = 0;
    boot_log(line);
    for (char *s = line; *s && report_len < (int)sizeof report - 2; ) report[report_len++] = *s++;
    report[report_len++] = '\n';
}

static void save_report(void)
{
    fat_file f;
    if (store_open("/.knurl/install.log", &f, sizeof report)) return;
    for (int i = report_len; i < (int)sizeof report; i++) report[i] = 0;
    store_write(&f, 0, report, sizeof report);
    ata_flush();
}

/* ---- helpers ----------------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static uint32_t round_up(uint32_t v, uint32_t to) { return (v + to - 1) / to * to; }

static uint8_t newbl[LOADER_MAX + 512] __attribute__((aligned(4)));
static uint8_t oldbl[OLD_MAX] __attribute__((aligned(4)));
static uint8_t chunk[65536] __attribute__((aligned(16)));
static uint8_t dir[1024] __attribute__((aligned(4)));
static uint8_t sec0[512] __attribute__((aligned(4)));

/* Write n bytes (a multiple of 512) at a byte offset on the disk, then read them back. */
static int write_verified(uint32_t at, const uint8_t *buf, uint32_t n)
{
    if ((at | n) & 511) return -1;
    for (uint32_t done = 0; done < n; ) {
        uint32_t k = n - done > sizeof chunk ? sizeof chunk : n - done;
        if (ata_write((at + done) / 512, k / 512, buf + done)) return -2;
        if (platform_disk_read((at + done) / 512, k / 512, chunk) || !same(chunk, buf + done, k)) return -3;
        done += k;
    }
    return ata_flush() ? -4 : 0;
}

/* ---- the install ------------------------------------------------------------------------- */

/* 1 = a loader was installed (restart now), 0 = nothing to do, -1 = refused. */
static int install_loader(void)
{
    fat_file bf;
    if (fat_open("/.knurl/knurlboot.ipod", &bf)) return 0;
    if (bf.size <= 8 + 256 || bf.size > 8 + LOADER_MAX) { say("loader: knurlboot.ipod is %u bytes", bf.size); return -1; }
    if (fat_read(&bf, newbl, bf.size) != (int)bf.size) { say("loader: can't read knurlboot.ipod"); return -1; }
    uint32_t L = bf.size - 8, file_sum = MODEL_NUM;
    for (uint32_t i = 0; i < L; i++) file_sum += newbl[8 + i];
    if (!same(newbl + 4, (const uint8_t *)"ipvd", 4) || be32(newbl) != file_sum) {
        say("loader: knurlboot.ipod is not an iPod Video image (bad header or checksum)");
        return -1;
    }
    for (uint32_t i = 0; i < L; i++) newbl[i] = newbl[8 + i];    /* the body, as installed */
    uint32_t P = round_up(L, 512);
    for (uint32_t i = L; i < P; i++) newbl[i] = 0;

    /* Where things are. */
    if (platform_disk_read(0, 1, sec0) || sec0[510] != 0x55 || sec0[511] != 0xAA) { say("loader: no partition table"); return -1; }
    uint32_t ss = 512u * (uint32_t)fat_mbr_units();               /* ipodpatcher's sector_size */
    uint32_t part = rd32(sec0 + 0x1be + 8) * ss;                   /* first partition, bytes */
    if (!part || part % 512) { say("loader: odd firmware partition start %x", part); return -1; }
    if (platform_disk_read(part / 512, 1, sec0) || !same(sec0 + 0x100, (const uint8_t *)"]ih[", 4)) {
        say("loader: no Apple firmware directory");
        return -1;
    }
    uint32_t version = sec0[0x10a] | (sec0[0x10b] << 8);
    uint32_t dir_at = part + rd32(sec0 + 0x104) + 0x200, x = dir_at % 512;
    if (platform_disk_read(dir_at / 512, 2, dir)) { say("loader: can't read the directory"); return -1; }
    int n = 0;
    uint32_t dev[10], len[10];
    while (n < 10 && x + (uint32_t)n * 40 + 40 <= sizeof dir && same(dir + x + n * 40, (const uint8_t *)"!ATA", 4)) {
        dev[n] = rd32(dir + x + n * 40 + 12);
        len[n] = rd32(dir + x + n * 40 + 16);
        n++;
    }
    uint8_t *e = dir + x;                                          /* entry 0 */
    if (!n || !same(e + 4, (const uint8_t *)"soso", 4)) { say("loader: first image isn't osos (%u images)", (uint32_t)n); return -1; }
    uint32_t entry = rd32(e + 24), sum0 = rd32(e + 28);
    uint32_t fwoffset = part + ((n > 1 && version == 2) ? 0 : ss);
    uint32_t base = fwoffset + dev[0];                             /* osos image on the disk */
    say("firmware: %u images, osos %u bytes, loader at +%u", (uint32_t)n, len[0], entry);
    if (!entry || entry % 512 || base % 512 || len[0] <= entry) { say("loader: no loader appended to Apple's firmware; not touching it"); return -1; }
    uint32_t old_len = len[0] - entry, oldP = round_up(old_len, 512);
    if (oldP > sizeof oldbl) { say("loader: the current loader is %u bytes, too big to back up", old_len); return -1; }
    if (n > 1 && dev[0] + entry + round_up(L, ss) > dev[1]) { say("loader: no room before the next image"); return -1; }

    if (platform_disk_read((base + entry) / 512, oldP / 512, oldbl)) { say("loader: can't read the current loader"); return -1; }
    if (old_len == L && same(oldbl, newbl, L)) {                 /* already installed */
        boot_log("loader: Knurl's loader is in the firmware partition");
        return 0;
    }
    say("loader: the firmware partition has a different loader (%u bytes)", old_len);

    if (booted_by != file_sum) {
        say("loader: not installing knurlboot.ipod: it hasn't started Knurl on this iPod yet.");
        say("  Install it once as .rockbox/rockbox.ipod, restart, and it installs itself.");
        return -1;
    }

    /* Checksum: Apple's bytes (everything before entryOffset) plus the loader's. */
    uint32_t apple = 0;
    for (uint32_t done = 0; done < entry; ) {
        uint32_t k = entry - done > sizeof chunk ? sizeof chunk : entry - done;
        if (platform_disk_read((base + done) / 512, k / 512, chunk)) { say("loader: read error in Apple's firmware"); return -1; }
        for (uint32_t i = 0; i < k; i++) apple += chunk[i];
        done += k;
    }
    uint32_t old_sum = apple;
    for (uint32_t i = 0; i < old_len; i++) old_sum += oldbl[i];
    if (old_sum != sum0) {
        say("loader: stored checksum %x but the bytes add up to %x; not touching it", sum0, old_sum);
        return -1;
    }
    uint32_t new_sum = apple;
    for (uint32_t i = 0; i < L; i++) new_sum += newbl[i];

    /* The loader, then the directory entry. */
    int r = write_verified(base + entry, newbl, P);
    if (r) { say("loader: writing the loader failed (%u)", (uint32_t)-r); return -1; }
    wr32(e + 16, entry + L);
    wr32(e + 28, new_sum);
    wr32(e + 36, 0xFFFFFFFF);                                      /* loadAddr: the default */
    r = write_verified(dir_at - x, dir, sizeof dir);
    if (r) { say("loader: writing the directory failed (%u)", (uint32_t)-r); return -1; }
    say("loader: Knurl's loader installed (%u bytes, checksum %x)", L, new_sum);
    return 1;
}

void fw_maintenance(void)
{
    boot_log(booted_by ? "boot: started by Knurl's loader" : "boot: started by another loader");
    int r = install_loader();
    if (report_len) save_report();
    if (r > 0) {
        boot_log("Restarting through the new loader...");
        delay_us(2500000);
        audio_shutdown();
        DEV_RS |= DEV_SYSTEM;
        for (;;) { }
    }
}
