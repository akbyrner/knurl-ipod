/*
 * The disk (an iFlash card behind the PP5022's IDE controller), polled PIO.
 * The bootloader already powered and used the disk to load us; we set the fastest PIO mode
 * the disk offers (timings for 80 MHz from Rockbox's ata-pp5020.c, which took them from
 * Apple's firmware) and issue READ/WRITE SECTORS. LBA48 is used when the disk supports it.
 * Knurl itself writes only into a few preallocated files (store.c) and the boot loader's
 * place in the firmware partition (fwinstall.c); everything else comes from the computer in
 * disk mode (usb.c).
 * Every wait is bounded; on any failure the read returns an error, never hangs.
 */
#include "hw.h"
#include "platform.h"
#include "dev.h"

#define IDE0_PRI_TIMING0 REG32(0xc3000000)
#define IDE0_PRI_TIMING1 REG32(0xc3000004)
#define IDE0_CFG         REG32(0xc3000028)
#define ATA_DATA         REG16(0xc30001e0)
#define ATA_REG(o)       (*(volatile uint8_t *)(0xc3000000 + (o)))
#define ATA_ERROR        ATA_REG(0x1e4)
#define ATA_NSECTOR      ATA_REG(0x1e8)
#define ATA_SECTOR       ATA_REG(0x1ec)
#define ATA_LCYL         ATA_REG(0x1f0)
#define ATA_HCYL         ATA_REG(0x1f4)
#define ATA_SELECT       ATA_REG(0x1f8)
#define ATA_COMMAND      ATA_REG(0x1fc)   /* write: command, read: status */
#define ATA_CONTROL      ATA_REG(0x3f8)   /* write: control, read: alternate status */
#define IDE_DMA_CONTROL  REG32(0xc3000400)
#define IDE_DMA_LENGTH   REG32(0xc3000408)
#define IDE_DMA_ADDR     REG32(0xc300040c)
#define IDE_CLOCK_CFG    REG32(0x600060c4)
#define IDE_CFG_INTRQ    0x08             /* in IDE0_CFG: the disk raised INTRQ (write 1 clears) */

#define ST_BSY 0x80
#define ST_DRQ 0x08
#define ST_ERR 0x01

static int lba48;
static uint64_t total_sectors;
static char model[41] = "no disk";
static int ready;

static void delay_400ns(void) { for (int i = 0; i < 5; i++) (void)ATA_CONTROL; }

/* Wait until not busy; with want_drq, until data is ready (or an error). 0 = ok. */
static int wait_status(int want_drq, uint32_t timeout_us)
{
    uint32_t start = USEC_TIMER;
    for (uint32_t n = 0; n < 50000000u; n++) {
        uint8_t s = ATA_CONTROL;
        if (!(s & ST_BSY)) {
            if (s & ST_ERR) return -2;
            if (!want_drq || (s & ST_DRQ)) return 0;
        }
        if (USEC_TIMER - start > timeout_us) return -1;
    }
    return -1;
}

static const uint16_t pio80mhz[5] = { 0xC293, 0x43A2, 0x11A1, 0x7232, 0x3131 };
static int pio_mode;
/* Ultra DMA timings for 80 MHz (IDE0_PRI_TIMING1), modes 0-2; Rockbox uses up to mode 2 on
 * this model (ata-pp5020.c / ata-target.h). */
static const uint32_t tm_udma[3] = { 0x800037C1, 0x80003491, 0x80003371 };
static int udma_mode = -1;

static void read_words(void *buf)
{
    if (!((uintptr_t)buf & 1)) {
        uint16_t *p = buf;
        for (int i = 0; i < 256; i++) p[i] = ATA_DATA;
    } else {
        uint8_t *p = buf;
        for (int i = 0; i < 256; i++) { uint16_t w = ATA_DATA; p[2 * i] = (uint8_t)w; p[2 * i + 1] = (uint8_t)(w >> 8); }
    }
}

int ata_init(void)
{
    IDE0_CFG |= 1 << 5;
    IDE0_CFG &= ~0x10000000u;             /* as Rockbox does on the Video (setting it, as for
                                           * the nano, slowed disk mode's writes 6.6 -> 3.9 MB/s) */
    IDE0_PRI_TIMING0 = 0xC293;            /* PIO mode 0 at 80 MHz */
    IDE0_PRI_TIMING1 = 0x80002150;
    ATA_CONTROL = 0x02;                   /* nIEN: no interrupts, we poll */
    ATA_SELECT = 0xE0;                    /* device 0, LBA */
    delay_400ns();
    if (wait_status(0, 5000000)) { ready = 0; return -1; }

    uint16_t id[256];
    ATA_COMMAND = 0xEC;                   /* IDENTIFY DEVICE */
    delay_400ns();
    if (wait_status(1, 5000000)) { ready = 0; return -2; }
    read_words(id);
    for (int i = 0; i < 20; i++) { model[2 * i] = (char)(id[27 + i] >> 8); model[2 * i + 1] = (char)id[27 + i]; }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
    lba48 = (id[83] & (1 << 10)) != 0;
    /* Fastest PIO mode: 3 or 4 if word 64 says so (word 53 bit 1: it is valid). Tell the
     * disk (SET FEATURES, transfer mode = PIO flow control | mode), then time the host side. */
    int mode = 2;
    if (id[53] & 2) mode = (id[64] & 2) ? 4 : (id[64] & 1) ? 3 : 2;
    ATA_ERROR = 0x03;                     /* feature: set transfer mode */
    ATA_NSECTOR = (uint8_t)(0x08 | mode);
    ATA_SELECT = 0xE0;
    ATA_COMMAND = 0xEF;
    delay_400ns();
    if (wait_status(0, 2000000) == 0) {
        pio_mode = mode;
        IDE0_PRI_TIMING0 = pio80mhz[mode];
    }
    /* Ultra DMA (used by disk mode, usb.c): the best of modes 0-2 the disk offers (word 88,
     * valid if word 53 bit 2). Controller setup as Rockbox's ata_dma_set_mode for 80 MHz. */
    int um = -1;
    if (id[53] & 4) um = (id[88] & 4) ? 2 : (id[88] & 2) ? 1 : (id[88] & 1) ? 0 : -1;
    if (um >= 0) {
        ATA_ERROR = 0x03;
        ATA_NSECTOR = (uint8_t)(0x40 | um);
        ATA_SELECT = 0xE0;
        ATA_COMMAND = 0xEF;
        delay_400ns();
        if (wait_status(0, 2000000) == 0) {
            IDE_CLOCK_CFG = 0xC0000000u;
            IDE0_PRI_TIMING1 = tm_udma[um];
            IDE0_CFG |= 0x20000000u;          /* ">= 50 MHz" */
            IDE_DMA_CONTROL |= 2;
            IDE_DMA_CONTROL &= ~1u;
            IDE0_CFG &= ~0x8010u;
            IDE0_CFG |= 0x20;
            udma_mode = um;
        }
    }
    if (lba48)
        total_sectors = id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        total_sectors = id[60] | ((uint32_t)id[61] << 16);
    ready = 1;
    return 0;
}

/* Address registers and command for a transfer of n (1..256) sectors at lba. */
static void issue(uint64_t lba, uint32_t n, uint8_t cmd28, uint8_t cmd48)
{
    if (lba48) {
        ATA_NSECTOR = (uint8_t)(n >> 8);         /* high bytes first */
        ATA_SECTOR = (uint8_t)(lba >> 24);
        ATA_LCYL = (uint8_t)(lba >> 32);
        ATA_HCYL = (uint8_t)(lba >> 40);
        ATA_NSECTOR = (uint8_t)n;
        ATA_SECTOR = (uint8_t)lba;
        ATA_LCYL = (uint8_t)(lba >> 8);
        ATA_HCYL = (uint8_t)(lba >> 16);
        ATA_SELECT = 0x40;
        ATA_COMMAND = cmd48;
    } else {
        ATA_NSECTOR = (uint8_t)n;
        ATA_SECTOR = (uint8_t)lba;
        ATA_LCYL = (uint8_t)(lba >> 8);
        ATA_HCYL = (uint8_t)(lba >> 16);
        ATA_SELECT = (uint8_t)(0xE0 | ((lba >> 24) & 0x0F));
        ATA_COMMAND = cmd28;
    }
    delay_400ns();
}

int platform_disk_read(uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *dst = buf;
    if (!ready) return -1;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        if (wait_status(0, 2000000)) return -1;
        issue(lba, n, 0x20, 0x24);                   /* READ SECTORS (EXT) */
        for (uint32_t i = 0; i < n; i++) {
            if (wait_status(1, 2000000)) { (void)ATA_COMMAND; return -1; }
            read_words(dst);
            dst += 512;
        }
        (void)ATA_COMMAND;                           /* read status: acknowledges the command */
        lba += n;
        count -= n;
    }
    return 0;
}

/* Disk mode (usb.c): streamed transfers. One ATA command covers up to 256 sectors (128 KB),
 * so a large request costs the card's per-command latency once, not once per chunk; the
 * caller moves the data a chunk at a time in between (and starts USB transfers meanwhile).
 * The buffer is uncached SDRAM, where every access is a bus cycle, so it is filled and read
 * 32 bits at a time; the data port is 16 bits. buf must be 4-aligned. */
#define STREAM_CMD_MAX 256

static struct { uint64_t lba; uint32_t left, in_cmd; int write; } st;

int ata_stream_start(uint64_t lba, uint32_t count, int write)
{
    if (!ready) return -1;
    st.lba = lba;
    st.left = count;
    st.in_cmd = 0;
    st.write = write;
    return 0;
}

/* Move the next n sectors (n <= what is left). 0 = ok. */
int ata_stream_xfer(uint32_t *buf, uint32_t n)
{
    if (!ready || ((uintptr_t)buf & 3) || n > st.left) return -1;
    while (n) {
        if (!st.in_cmd) {
            if (wait_status(0, 2000000)) return -1;
            uint32_t c = st.left > STREAM_CMD_MAX ? STREAM_CMD_MAX : st.left;
            if (st.write) issue(st.lba, c, 0x30, 0x34);
            else issue(st.lba, c, 0x20, 0x24);          /* LBA28: count 0 = 256 */
            st.in_cmd = c;
        }
        if (wait_status(1, 2000000)) { (void)ATA_COMMAND; st.in_cmd = 0; return -1; }
        if (st.write) for (int w = 0; w < 128; w++) { uint32_t v = *buf++; ATA_DATA = (uint16_t)v; ATA_DATA = (uint16_t)(v >> 16); }
        else for (int w = 0; w < 128; w++) { uint32_t lo = ATA_DATA; uint32_t hi = ATA_DATA; *buf++ = lo | hi << 16; }
        st.lba++;
        st.left--;
        n--;
        if (!--st.in_cmd) {
            if (st.write) { if (wait_status(0, 5000000)) return -1; }   /* written, no error */
            else (void)ATA_COMMAND;                                     /* acknowledge */
        }
    }
    return 0;
}

/* Write count sectors from buf (4-aligned), PIO. For Knurl's own small writes (store.c);
 * disk mode uses the streaming and DMA paths. 0 = ok. */
int ata_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (ata_stream_start(lba, count, 1)) return -1;
    return ata_stream_xfer((uint32_t *)(uintptr_t)buf, count);
}

int ata_flush(void)
{
    if (!ready || wait_status(0, 2000000)) return -1;
    ATA_SELECT = 0xE0;
    ATA_COMMAND = lba48 ? 0xEA : 0xE7;               /* FLUSH CACHE (EXT) */
    delay_400ns();
    return wait_status(0, 30000000);
}

int ata_pio_mode(void) { return pio_mode; }
int ata_udma_mode(void) { return udma_mode; }

/* One DMA transfer of n (1..256) sectors to/from phys (a physical address, 16-aligned).
 * The CPU only polls for the end: status must not be read while DMA runs. 0 = ok; on any
 * failure DMA is switched off for good (callers fall back to PIO) after a soft reset. */
int ata_dma(uint64_t lba, uint32_t n, uint32_t phys, int write)
{
    if (!ready || udma_mode < 0 || !n || n > 256 || (phys & 15)) return -1;
    if (wait_status(0, 2000000)) return -1;
    ATA_CONTROL = 0;                      /* nIEN off: INTRQ marks the end of the transfer */
    IDE0_CFG |= IDE_CFG_INTRQ;            /* clear a pending one */
    IDE_DMA_CONTROL |= 2;
    IDE_DMA_LENGTH = n * 512 - 4;
    IDE_DMA_ADDR = phys;
    if (write) IDE_DMA_CONTROL &= ~8u; else IDE_DMA_CONTROL |= 8;
    IDE0_CFG |= 0x8000;
    issue(lba, n, write ? 0xCA : 0xC8, write ? 0x35 : 0x25);   /* READ/WRITE DMA (EXT) */
    IDE_DMA_CONTROL |= 1;
    int done = 0;
    uint32_t t = USEC_TIMER;
    for (uint32_t i = 0; i < 100000000u && USEC_TIMER - t < 5000000; i++)
        if (IDE0_CFG & IDE_CFG_INTRQ) { done = 1; break; }
    IDE0_CFG &= ~0x8000u;
    IDE_DMA_CONTROL &= ~0x80000001u;
    if (done && wait_status(0, 2000000) == 0) {   /* not busy, no error */
        (void)ATA_COMMAND;                        /* acknowledge */
        ATA_CONTROL = 0x02;
        return 0;
    }
    udma_mode = -1;
    ATA_CONTROL = 0x02 | 0x04;            /* soft reset */
    delay_us(10);
    ATA_CONTROL = 0x02;
    delay_us(2000);
    wait_status(0, 5000000);
    return -1;
}

const char *platform_disk_name(void) { return model; }
uint64_t platform_disk_sectors(void) { return total_sectors; }
