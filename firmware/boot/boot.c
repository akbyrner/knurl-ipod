/*
 * knurlboot: Knurl's boot loader, in place of Rockbox's. It lives in the firmware partition,
 * appended to Apple's firmware (installed by Knurl itself: device/fwinstall.c), and decides:
 *
 *   HOLD on                 -> Apple's firmware, untouched (it is already in SDRAM)
 *   otherwise               -> /.knurl/knurl.ipod from the music partition
 *   that fails (missing, unreadable, bad checksum)
 *                           -> a red line on the screen for 3 s, then Apple's firmware
 *
 * knurl.ipod is the same format the Rockbox loader read (4-byte big-endian checksum = 5 + the
 * sum of the image bytes, "ipvd", the image). It is read whole into SDRAM at 16 MB and
 * checked there, and only then copied to 0x10000000 over Apple's firmware, so a bad file
 * never costs the fallback. The disk and FAT code are Knurl's own (device/ata.c,
 * src/storage/fat.c). The ROM has already powered the disk and the BCM; nothing else is set
 * up here: Knurl initialises its own hardware.
 *
 * Before starting Knurl it leaves a stamp at physical 0x11F00000 (above everything Knurl
 * uses): "KNBT" and its own checksum, which fwinstall.c requires before it will install a
 * loader into the firmware partition.
 */
#include <stdint.h>
#include "hw.h"
#include "platform.h"
#include "storage/fat.h"

int ata_init(void);
uint32_t boot_main(void);
extern const uint8_t _start[], _image_end[];

#define DRAM_START  0x10000000u
#define STAGE       ((uint8_t *)0x11000000)
#define MAX_IMAGE   (14u << 20)                 /* STAGE + this stays below the stamp */
#define STAMP       ((volatile uint32_t *)0x11F00000)
#define STAMP_MAGIC 0x4B4E4254u                 /* "KNBT" */
#define MODEL_NUM   5

void delay_us(uint32_t us)
{
    uint32_t t = USEC_TIMER;
    for (uint32_t n = 0; n < us * 8u + 64u; n++)
        if (USEC_TIMER - t >= us) return;
}

static uint32_t byte_sum(const uint8_t *p, uint32_t n)     /* word reads: SDRAM is uncached here */
{
    uint32_t s = 0;
    for (; n >= 4; n -= 4, p += 4) {
        uint32_t w = *(const uint32_t *)p;
        s += (w & 0xff) + ((w >> 8) & 0xff) + ((w >> 16) & 0xff) + (w >> 24);
    }
    while (n--) s += *p++;
    return s;
}

static int load_knurl(void)
{
    if (ata_init()) return -1;
    if (fat_mount()) return -2;
    fat_file f;
    if (fat_open("/.knurl/knurl.ipod", &f)) return -3;
    if (f.size < 8 + 4096 || f.size > MAX_IMAGE) return -4;
    if (fat_read(&f, STAGE, f.size) != (int)f.size) return -5;
    if (STAGE[4] != 'i' || STAGE[5] != 'p' || STAGE[6] != 'v' || STAGE[7] != 'd') return -6;
    uint32_t want = ((uint32_t)STAGE[0] << 24) | ((uint32_t)STAGE[1] << 16) | (STAGE[2] << 8) | STAGE[3];
    if (MODEL_NUM + byte_sum(STAGE + 8, f.size - 8) != want) return -7;
    const uint32_t *src = (const uint32_t *)(STAGE + 8);
    volatile uint32_t *dst = (volatile uint32_t *)DRAM_START;
    for (uint32_t i = 0, n = (f.size - 8 + 3) / 4; i < n; i++) dst[i] = src[i];
    return 0;
}

static int bcm_addr(uint32_t a)
{
    BCM_WR_ADDR32 = a;
    for (uint32_t n = 0; n < 2000000u; n++)
        if (BCM_CONTROL & 0x2) return 1;
    return 0;
}

/* Graphite with a red line across the middle: Knurl couldn't be started. */
static void failure_screen(void)
{
    if (!(GPO32_VAL & GPO32_BCM_POWER)) return;
    for (int y = 0; y < SCREEN_H; y++) {
        if (!bcm_addr(BCMA_CMDPARAM + (uint32_t)(y * SCREEN_W) * 2)) return;
        for (int x = 0; x < SCREEN_W; x += 2) {
            uint32_t c = (y >= 116 && y < 124 && x >= 24 && x < 296) ? 0xE8E4u : 0x18C3u;
            BCM_DATA32 = c | c << 16;
        }
    }
    if (bcm_addr(BCMA_COMMAND)) {
        BCM_DATA32 = BCMCMD_LCD_UPDATE;
        BCM_CONTROL = BCM_CONTROL_KICK;
    }
}

static int apple_in_ram(void)
{
    static const char tag[] = "portalplayer";             /* Apple's image, at +0x20 */
    const volatile uint8_t *p = (const volatile uint8_t *)(DRAM_START + 0x20);
    for (int i = 0; i < 12; i++)
        if (p[i] != (uint8_t)tag[i]) return 0;
    return 1;
}

uint32_t boot_main(void)
{
    uint32_t self = MODEL_NUM + byte_sum(_start, (uint32_t)(_image_end - _start));
    if (!(GPIOA_INPUT_VAL & HOLD_BIT)) return DRAM_START;  /* HOLD on: Apple's firmware */
    if (load_knurl() == 0) {
        STAMP[0] = STAMP_MAGIC;
        STAMP[1] = self;
        return DRAM_START;
    }
    failure_screen();
    if (apple_in_ram()) {
        delay_us(3000000);
        return DRAM_START;
    }
    /* Not started from the firmware partition (the test install): nothing to fall back to.
     * HOLD on restarts the iPod, and with HOLD on the loader in the firmware partition starts
     * Apple's firmware. */
    for (;;)
        if (!(GPIOA_INPUT_VAL & HOLD_BIT)) DEV_RS |= DEV_SYSTEM;
}
