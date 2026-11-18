/*
 * Disk mode: the iPod as a USB mass-storage disk (Bulk-Only Transport + SCSI) on the
 * PP5022's ARC (Chipidea) USB 2.0 device controller. Polled, CPU only; it runs instead of
 * the player (device.c has stopped audio). Register map, the PP5022 controller/PHY start-up
 * sequence and the 5G's VID/PID are hardware facts from Rockbox (target/arm/usb-drv-arc.c,
 * target/arm/pp/usb-fw-pp502x.c, export/config/ipodvideo.h); the code is ours.
 *
 * Everything the controller reads or writes (queue heads, transfer descriptors, buffers)
 * lives in SDRAM and is only ever touched through its uncached physical alias, which is
 * also the address the controller is given: no cache maintenance anywhere.
 *
 * Reads and writes are pipelined: while the USB controller moves one 64 KB chunk, the
 * disk moves the next one. With Ultra DMA (the normal case) both are DMA and the CPU only
 * polls; without it (DMA failed once) the CPU does PIO and one ATA command covers the
 * whole request. After a large or sequential read the next 64 KB is read ahead while the
 * host prepares its next command (its CBW lands in the primed buffer meanwhile), so
 * sequential reads don't pay the card's first-byte latency per command.
 */
#include <stddef.h>
#include "hw.h"
#include "platform.h"
#include "shared.h"
#include "dev.h"
#include "storage/fat.h"

int ata_udma_mode(void);

/* ---- controller ---------------------------------------------------------------------- */
#define UR(o)        REG32(0xc5000000 + (o))
#define USBCMD       UR(0x140)
#define USBSTS       UR(0x144)
#define DEVICEADDR   UR(0x154)
#define EPLISTADDR   UR(0x158)
#define PORTSC1      UR(0x184)
#define USBMODE      UR(0x1a8)
#define EPSETUPSTAT  UR(0x1ac)
#define EPPRIME      UR(0x1b0)
#define EPFLUSH      UR(0x1b4)
#define EPCOMPLETE   UR(0x1bc)
#define EPCTRL(n)    UR(0x1c0 + 4 * (n))

#define CMD_RUN      0x1u
#define CMD_RESET    0x2u
#define CMD_SUTW     (1u << 13)        /* setup tripwire */
#define STS_PORT     0x04u
#define STS_RESET    0x40u
#define STS_SUSPEND  0x100u
#define PORT_SPEED   0x0C000000u
#define PORT_HIGH    0x08000000u
#define EP_RX_STALL  (1u << 0)
#define EP_TX_STALL  (1u << 16)

#define DEV_USB0     0x00000008u
#define DEV_USB1     0x00400000u
#define INIT_USB     0x80000000u
#define USB_CTL28    REG32(0x70000028)
#define USB_PHY60    REG32(0x70000060)
#define USB_EN2C     REG32(0x7000002c)
#define XMB_RAM_CFG  REG32(0x7000003c)

#define TD_ACTIVE    0x80u
#define TD_ERRORS    0x68u             /* halted, buffer error, transaction error */
#define TD_IOC       (1u << 15)
#define QH_IOS       (1u << 15)
#define QH_NO_ZLT    (1u << 29)

typedef struct { uint32_t next, token, buf[5], pad; } td_t;                       /* 32 B */
typedef struct { uint32_t caps, cur; td_t ov; uint32_t setup[2], pad[4]; } qh_t;  /* 64 B */

#define TDS    4                      /* per pipe: up to 4 x 16 KB in one transfer */
#define CHUNK  65536                  /* bytes per pipelined disk/USB step (= TDS x 16 KB) */
#define AHEAD  65536                  /* read-ahead size */

static struct {
    qh_t qh[8];                       /* endpoints 0-3, OUT then IN; the list must be 2 KB aligned */
    td_t td[8][TDS];
    uint32_t ctrl[64];
    uint32_t cbw[128];
    uint32_t csw[4];
    uint32_t data[2][CHUNK / 4];
    uint32_t ahead[AHEAD / 4];         /* read-ahead */
} usb_store __attribute__((section(".noinit.usb"), aligned(2048)));
#define M ((volatile __typeof__(usb_store) *)UNCACHED(&usb_store))
#define PHYS(p) ((uint32_t)(uintptr_t)(p))      /* M's addresses are already physical */

static int high_speed, configured, ejected, need_reset;
static uint32_t blocks, block_size, units;     /* units: 512-byte disk sectors per block */

static uint32_t mask_of(int ep, int in) { return in ? 1u << (16 + ep) : 1u << ep; }
static int mps(void) { return high_speed ? 512 : 64; }

/* Why a wait ended early. */
enum { OK = 0, ABORT_RESET = -1, ABORT_UNPLUG = -2, ABORT_SETUP = -3, ABORT_ERROR = -4 };

static int primed_tds[8];
static uint32_t primed_len[8][TDS];

static void prime(int ep, int in, uint32_t phys, uint32_t len)
{
    int pipe = ep * 2 + in;
    volatile td_t *td = M->td[pipe];
    int n = 0;
    do {
        uint32_t l = len > 16384 ? 16384 : len;
        td[n].next = 1;
        td[n].token = (l << 16) | TD_ACTIVE;
        td[n].buf[0] = phys;
        primed_len[pipe][n] = l;
        for (int i = 1; i < 5; i++) td[n].buf[i] = (phys & ~0xfffu) + 0x1000u * (uint32_t)i;
        if (n) td[n - 1].next = PHYS(&td[n]);
        phys += l;
        len -= l;
        n++;
    } while (len && n < TDS);
    td[n - 1].token |= TD_IOC;
    primed_tds[pipe] = n;
    volatile qh_t *q = &M->qh[pipe];
    q->ov.next = PHYS(&td[0]);
    q->ov.token &= ~(TD_ACTIVE | 0x40u);
    __asm__ volatile("" ::: "memory");
    EPPRIME |= mask_of(ep, in);
}

static void handle_setup(void);

/* Wait for the transfer primed on (ep, in); *got = bytes moved. Bulk waits keep answering
 * control requests (the host may send some mid-transfer). */
static int wait_xfer(int ep, int in, uint32_t *got)
{
    int pipe = ep * 2 + in;
    volatile td_t *td = M->td[pipe];
    int n = primed_tds[pipe];
    for (;;) {
        if (!power_usb()) return ABORT_UNPLUG;
        if (USBSTS & STS_RESET) return ABORT_RESET;
        if (EPSETUPSTAT & 1) {
            if (ep == 0) return ABORT_SETUP;
            handle_setup();
            continue;
        }
        if (EPPRIME & mask_of(ep, in)) continue;
        if (td[n - 1].token & TD_ACTIVE) continue;
        EPCOMPLETE = mask_of(ep, in);
        uint32_t moved = 0;
        int err = 0;
        for (int i = 0; i < n; i++) {
            moved += primed_len[pipe][i] - ((td[i].token >> 16) & 0x7fff);
            if (td[i].token & TD_ERRORS) err = 1;
        }
        if (got) *got = moved;
        return err ? ABORT_ERROR : OK;
    }
}

static void stall(int ep, int in) { EPCTRL(ep) |= in ? EP_TX_STALL : EP_RX_STALL; }

static int stalled(int ep, int in) { return (EPCTRL(ep) & (in ? EP_TX_STALL : EP_RX_STALL)) != 0; }

static void flush_all(void)
{
    EPFLUSH = ~0u;
    for (int i = 0; i < 100000 && EPFLUSH; i++) { }
}

static void bus_reset(void)
{
    DEVICEADDR = 0;
    EPSETUPSTAT = EPSETUPSTAT;
    EPCOMPLETE = EPCOMPLETE;
    for (int i = 0; i < 100000 && EPPRIME; i++) { }
    flush_all();
    configured = 0;
    need_reset = 0;
    for (int i = 0; i < 200000 && (PORTSC1 & (1u << 8)); i++) { }   /* port reset done */
    high_speed = (PORTSC1 & PORT_SPEED) == PORT_HIGH;
}

/* ---- descriptors --------------------------------------------------------------------- */

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0xac, 0x05, 0x09, 0x12,           /* Apple, iPod Video (as Rockbox and Apple's disk mode) */
    0x01, 0x00, 1, 2, 3, 1,
};

static const uint8_t qual_desc[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0 };

static int config_desc(uint8_t *d, int type, int hs)
{
    static const uint8_t c[32] = {
        9, 2, 32, 0, 1, 1, 0, 0xC0, 250,          /* self-powered (battery); 500 mA */
        9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,       /* mass storage, SCSI, bulk-only */
        7, 5, 0x81, 2, 0, 0, 0,
        7, 5, 0x01, 2, 0, 0, 0,
    };
    for (int i = 0; i < 32; i++) d[i] = c[i];
    d[1] = (uint8_t)type;
    int m = hs ? 512 : 64;
    d[22] = d[29] = (uint8_t)m;
    d[23] = d[30] = (uint8_t)(m >> 8);
    return 32;
}

static int string_desc(uint8_t *d, int idx)
{
    static const char *const s[] = { 0, "Knurl", "iPod (Knurl disk mode)", "4B4E55524C31" };
    if (idx == 0) { d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04; return 4; }
    if (idx > 3) return -1;
    int n = 2;
    for (const char *p = s[idx]; *p; p++) { d[n++] = (uint8_t)*p; d[n++] = 0; }
    d[0] = (uint8_t)n;
    d[1] = 3;
    return n;
}

/* ---- control endpoint ---------------------------------------------------------------- */

static void ctrl_in(const uint8_t *data, int len, int wlength)
{
    if (len > wlength) len = wlength;
    volatile uint8_t *b = (volatile uint8_t *)M->ctrl;
    for (int i = 0; i < len; i++) b[i] = data[i];
    prime(0, 1, PHYS(M->ctrl), (uint32_t)len);
    if (wait_xfer(0, 1, 0) != OK) return;
    prime(0, 0, PHYS(M->ctrl), 0);                 /* status stage */
    wait_xfer(0, 0, 0);
}

static void ctrl_ack(void)
{
    prime(0, 1, PHYS(M->ctrl), 0);
    wait_xfer(0, 1, 0);
}

static void ctrl_stall(void) { EPCTRL(0) |= EP_TX_STALL | EP_RX_STALL; }

static void configure_bulk(void)
{
    volatile qh_t *o = &M->qh[2], *i = &M->qh[3];
    o->caps = i->caps = ((uint32_t)mps() << 16) | QH_NO_ZLT;
    o->ov.next = i->ov.next = 1;
    o->ov.token = i->ov.token = 0;
    EPCTRL(1) = (1u << 23) | (1u << 22) | (2u << 18) | (1u << 7) | (1u << 6) | (2u << 2);
}

static void handle_setup(void)
{
    uint32_t s0, s1;
    int tries = 0;
    do {
        USBCMD |= CMD_SUTW;
        s0 = M->qh[0].setup[0];
        s1 = M->qh[0].setup[1];
    } while (!(USBCMD & CMD_SUTW) && ++tries < 1000);
    USBCMD &= ~CMD_SUTW;
    EPSETUPSTAT = 1;
    EPFLUSH = 0x00010001u;                         /* a new setup cancels the old EP0 transfer */
    for (int i = 0; i < 100000 && (EPFLUSH & 0x00010001u); i++) { }
    EPCOMPLETE = 0x00010001u;

    int type = (int)(s0 & 0xff), req = (int)((s0 >> 8) & 0xff);
    int value = (int)(s0 >> 16), index = (int)(s1 & 0xffff), length = (int)(s1 >> 16);
    uint8_t d[64];

    if ((type & 0x60) == 0x20) {                   /* class requests (mass storage) */
        if (req == 0xFF && type == 0x21) {         /* bulk-only reset */
            need_reset = 0;
            EPFLUSH = 0x00020002u;
            for (int i = 0; i < 100000 && (EPFLUSH & 0x00020002u); i++) { }
            EPCTRL(1) &= ~(EP_TX_STALL | EP_RX_STALL);
            EPCTRL(1) |= (1u << 22) | (1u << 6);
            ctrl_ack();
        } else if (req == 0xFE && type == 0xA1) {  /* get max LUN */
            d[0] = 0;
            ctrl_in(d, 1, length);
        } else ctrl_stall();
        return;
    }
    if ((type & 0x60) != 0) { ctrl_stall(); return; }

    switch (req) {
    case 0x06: {                                   /* GET_DESCRIPTOR */
        int n = -1, dt = value >> 8;
        if (dt == 1) { for (int i = 0; i < 18; i++) d[i] = dev_desc[i]; n = 18; }
        else if (dt == 2) n = config_desc(d, 2, high_speed);
        else if (dt == 7) n = config_desc(d, 7, !high_speed);
        else if (dt == 6) { for (int i = 0; i < 10; i++) d[i] = qual_desc[i]; n = 10; }
        else if (dt == 3) n = string_desc(d, value & 0xff);
        if (n < 0) ctrl_stall(); else ctrl_in(d, n, length);
        break;
    }
    case 0x05:                                     /* SET_ADDRESS */
        /* The status stage still goes out at address 0, so the new address is set only once
         * it has completed. (Writing it first with the "advance" bit, as newer Chipidea cores
         * allow, made Windows report "Set Address Failed": this core doesn't have it. Rockbox
         * also sets it after the status.) */
        prime(0, 1, PHYS(M->ctrl), 0);
        if (wait_xfer(0, 1, 0) == OK) DEVICEADDR = (uint32_t)(value & 0x7f) << 25;
        break;
    case 0x09:                                     /* SET_CONFIGURATION */
        if (value == 1) { configure_bulk(); configured = 1; }
        else if (value == 0) { configured = 0; EPCTRL(1) = 0; }
        else { ctrl_stall(); break; }
        ctrl_ack();
        break;
    case 0x08: d[0] = (uint8_t)configured; ctrl_in(d, 1, length); break;
    case 0x00:                                     /* GET_STATUS */
        d[0] = d[1] = 0;
        if ((type & 0x1f) == 0) d[0] = 1;          /* self-powered */
        else if ((type & 0x1f) == 2) d[0] = (uint8_t)stalled(index & 0x0f, (index & 0x80) != 0);
        ctrl_in(d, 2, length);
        break;
    case 0x01:                                     /* CLEAR_FEATURE */
        if ((type & 0x1f) == 2 && value == 0 && (index & 0x0f) == 1 && !need_reset) {
            int in = (index & 0x80) != 0;
            EPCTRL(1) &= ~(in ? EP_TX_STALL : EP_RX_STALL);
            EPCTRL(1) |= in ? (1u << 22) : (1u << 6);      /* data toggle back to DATA0 */
        }
        ctrl_ack();
        break;
    case 0x03:                                     /* SET_FEATURE */
        if ((type & 0x1f) == 2 && value == 0 && (index & 0x0f) == 1) stall(1, (index & 0x80) != 0);
        ctrl_ack();
        break;
    case 0x0A: d[0] = 0; ctrl_in(d, 1, length); break;
    case 0x0B: ctrl_ack(); break;
    default: ctrl_stall(); break;
    }
}

/* ---- SCSI ------------------------------------------------------------------------------ */

static uint8_t sense_key, sense_asc;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int send_csw(uint32_t tag, uint32_t residue, int status)
{
    while (stalled(1, 1)) {                        /* the host clears the stall first */
        if (!power_usb()) return ABORT_UNPLUG;
        if (USBSTS & STS_RESET) return ABORT_RESET;
        if (EPSETUPSTAT & 1) handle_setup();
    }
    M->csw[0] = 0x53425355u;
    M->csw[1] = tag;
    M->csw[2] = residue;
    M->csw[3] = (uint32_t)status;
    prime(1, 1, PHYS(M->csw), 13);
    return wait_xfer(1, 1, 0);
}

/* Small IN reply: at most what the host asked for; stall if it wanted more and we ended on a
 * packet boundary (otherwise our short packet ends the data stage). Returns bytes sent. */
static uint32_t reply(const uint8_t *data, uint32_t len, uint32_t dlen, int *rc)
{
    if (len > dlen) len = dlen;
    volatile uint8_t *b = (volatile uint8_t *)M->data[0];
    for (uint32_t i = 0; i < len; i++) b[i] = data[i];
    uint32_t got = 0;
    if (len) {
        prime(1, 1, PHYS(M->data[0]), len);
        *rc = wait_xfer(1, 1, &got);
    }
    if (got < dlen && got % (uint32_t)mps() == 0) stall(1, 1);
    return got;
}

static int fail(uint8_t key, uint8_t asc) { sense_key = key; sense_asc = asc; return 1; }

/* Disk <-> buffer for c (<= 256) sectors at alba: Ultra DMA when it works, else PIO. */
static int disk_move(uint64_t alba, uint32_t c, volatile uint32_t *buf, int write)
{
    if (ata_udma_mode() >= 0 && ata_dma(alba, c, PHYS(buf), write) == 0) return 0;
    if (ata_stream_start(alba, c, write)) return -1;
    return ata_stream_xfer((uint32_t *)buf, c);
}

/* Read-ahead: sectors [ahead_lba, ahead_lba + ahead_n) are in M->ahead. */
static uint64_t ahead_lba, next_lba = ~0ull;
static uint32_t ahead_n;
static int ahead_due;                  /* main loop: fill it before the next command */

static void read_ahead(void)
{
    ahead_due = 0;
    ahead_n = 0;
    uint64_t total = platform_disk_sectors();
    if (next_lba >= total) return;
    uint32_t n = AHEAD / 512;
    if (total - next_lba < n) n = (uint32_t)(total - next_lba);
    if (disk_move(next_lba, n, M->ahead, 0)) return;
    ahead_lba = next_lba;
    ahead_n = n;
}

/* READ/WRITE of n blocks at lba. Returns 0 ok, 1 failed (sense set), <0 aborted. */
static int disk_xfer(int write, uint32_t lba, uint32_t n, uint32_t dlen, uint32_t *done)
{
    *done = 0;
    if ((uint64_t)lba + n > blocks) return fail(5, 0x21);         /* LBA out of range */
    uint64_t alba = (uint64_t)lba * units;
    uint32_t left = n * units;                                     /* 512-byte sectors */
    if ((uint64_t)left * 512 > dlen) left = dlen / 512;
    uint32_t t0 = USEC_TIMER, total = left;
    int k = 0, status = 0, rc;
    if (!write) {
        int pending = 0;
        int sequential = alba == next_lba;
        if (ahead_n && alba == ahead_lba) {                        /* already read ahead */
            uint32_t c = left < ahead_n ? left : ahead_n;
            prime(1, 1, PHYS(M->ahead), c * 512);
            pending = 1;
            *done += c * 512;
            alba += c;
            left -= c;
            usb_stats.ahead_hits++;
        }
        ahead_n = 0;
        while (left && !status) {
            uint32_t c = left > CHUNK / 512 ? CHUNK / 512 : left;
            if (disk_move(alba, c, M->data[k], 0)) { status = fail(3, 0x11); break; }
            if (pending && (rc = wait_xfer(1, 1, 0)) < 0) return rc;
            prime(1, 1, PHYS(M->data[k]), c * 512);
            pending = 1;
            *done += c * 512;
            alba += c;
            left -= c;
            k ^= 1;
        }
        if (pending && (rc = wait_xfer(1, 1, 0)) < 0) return rc;
        if (*done < dlen) stall(1, 1);
        next_lba = alba;
        if (!status && (sequential || total >= 64)) ahead_due = 1;
        usb_stats.rd_bytes += *done;
        usb_stats.rd_us += USEC_TIMER - t0;
    } else {
        ahead_n = 0;                                               /* may overlap: drop it */
        next_lba = ~0ull;
        uint32_t c = left > CHUNK / 512 ? CHUNK / 512 : left;
        if (c) prime(1, 0, PHYS(M->data[0]), c * 512);
        while (left) {
            uint32_t got;
            if ((rc = wait_xfer(1, 0, &got)) < 0) return rc;
            uint32_t next = left - c > CHUNK / 512 ? CHUNK / 512 : left - c;
            if (next) prime(1, 0, PHYS(M->data[k ^ 1]), next * 512);
            if (!status && disk_move(alba, c, M->data[k], 1)) status = fail(3, 0x0C);
            *done += c * 512;
            alba += c;
            left -= c;
            c = next;
            k ^= 1;
        }
        if (*done < dlen) stall(1, 0);
        usb_stats.wr_bytes += *done;
        usb_stats.wr_us += USEC_TIMER - t0;
    }
    return status;
}

static int handle_cbw(const uint8_t *w, uint32_t len)
{
    if (len != 31 || be32(w) != 0x55534243u) {    /* "USBC", little-endian on the wire */
        stall(1, 1);
        stall(1, 0);
        need_reset = 1;
        return OK;
    }
    uint32_t tag = (uint32_t)w[4] | (uint32_t)w[5] << 8 | (uint32_t)w[6] << 16 | (uint32_t)w[7] << 24;
    uint32_t dlen = (uint32_t)w[8] | (uint32_t)w[9] << 8 | (uint32_t)w[10] << 16 | (uint32_t)w[11] << 24;
    int in = (w[12] & 0x80) != 0;
    const uint8_t *cb = w + 15;
    uint8_t r[36];
    uint32_t sent = 0;
    int status = 0, rc = OK;

    switch (cb[0]) {
    case 0x00:                                     /* TEST UNIT READY */
        if (ejected) status = fail(2, 0x3A);
        break;
    case 0x03:                                     /* REQUEST SENSE */
        for (int i = 0; i < 18; i++) r[i] = 0;
        r[0] = 0x70; r[2] = sense_key; r[7] = 10; r[12] = sense_asc;
        sent = reply(r, 18, dlen, &rc);
        sense_key = sense_asc = 0;
        break;
    case 0x12: {                                   /* INQUIRY */
        if (cb[1] & 1) { status = fail(5, 0x24); break; }
        static const char id[] = "Knurl   iPod            1.0 ";
        r[0] = 0x00; r[1] = 0x80; r[2] = 0x02; r[3] = 0x02; r[4] = 31; r[5] = r[6] = r[7] = 0;
        for (int i = 0; i < 28; i++) r[8 + i] = (uint8_t)id[i];
        sent = reply(r, 36, dlen, &rc);
        break;
    }
    case 0x25:                                     /* READ CAPACITY (10) */
        if (ejected) { status = fail(2, 0x3A); break; }
        put_be32(r, blocks - 1);
        put_be32(r + 4, block_size);
        sent = reply(r, 8, dlen, &rc);
        break;
    case 0x23:                                     /* READ FORMAT CAPACITIES */
        r[0] = r[1] = r[2] = 0; r[3] = 8;
        put_be32(r + 4, blocks);
        put_be32(r + 8, block_size);
        r[8] = 0x02;                               /* formatted media */
        sent = reply(r, 12, dlen, &rc);
        break;
    case 0x1A:                                     /* MODE SENSE (6) */
        r[0] = 3; r[1] = r[2] = r[3] = 0;
        sent = reply(r, 4, dlen, &rc);
        break;
    case 0x5A:                                     /* MODE SENSE (10) */
        for (int i = 0; i < 8; i++) r[i] = 0;
        r[1] = 6;
        sent = reply(r, 8, dlen, &rc);
        break;
    case 0x1E:                                     /* PREVENT/ALLOW MEDIUM REMOVAL */
    case 0x2F:                                     /* VERIFY (10) */
        break;
    case 0x35:                                     /* SYNCHRONIZE CACHE (10) */
        if (ata_flush()) status = fail(3, 0x0C);
        break;
    case 0x1B:                                     /* START STOP UNIT */
        if ((cb[4] & 3) == 2) {                    /* eject */
            ata_flush();
            ejected = 1;
        }
        break;
    case 0x28: case 0x2A: case 0xA8: case 0xAA: {  /* READ/WRITE (10)/(12) */
        int write = cb[0] == 0x2A || cb[0] == 0xAA;
        uint32_t lba = be32(cb + 2);
        uint32_t n = cb[0] & 0x80 ? be32(cb + 6) : (uint32_t)cb[7] << 8 | cb[8];
        if (ejected) { status = fail(2, 0x3A); break; }
        if (write == in && dlen) { status = fail(5, 0x24); break; }
        rc = disk_xfer(write, lba, n, dlen, &sent);
        if (rc > 0) { status = 1; rc = OK; }
        if (rc < 0) return rc;
        break;
    }
    default:
        status = fail(5, 0x20);                    /* invalid command */
        break;
    }
    if (rc < 0) return rc;
    if (sent < dlen && !stalled(1, in)) {
        if (in) stall(1, 1);
        else if (sent == 0) stall(1, 0);
    }
    return send_csw(tag, dlen - sent, status);
}

/* ---- run --------------------------------------------------------------------------------- */

static void controller_start(void)
{
    USB_EN2C |= 0x3000000u;
    DEV_EN |= DEV_USB0 | DEV_USB1;
    DEV_RS |= DEV_USB0; DEV_RS &= ~DEV_USB0;
    DEV_RS |= DEV_USB1; DEV_RS &= ~DEV_USB1;
    DEV_INIT2 |= INIT_USB;
    for (int i = 0; i < 1000000 && !(USB_CTL28 & 0x80); i++) { }
    USB_CTL28 |= 2;
    delay_us(100000);
    XMB_RAM_CFG |= 0x47A;

    USBCMD &= ~CMD_RUN;
    delay_us(50000);
    USBCMD |= CMD_RESET;
    for (int i = 0; i < 1000000 && (USBCMD & CMD_RESET); i++) { }
    /* PP5022 PHY start-up, after the controller reset (sequence from Rockbox). */
    USB_PHY60 |= 0xF;
    USB_CTL28 |= 0x10000; USB_CTL28 &= ~0x10000u;
    USB_PHY60 &= ~0x20u; delay_us(10);
    USB_PHY60 |= 0x20; delay_us(10);
    USB_PHY60 = (USB_PHY60 & ~0xFu) | 4; delay_us(10);
    USB_PHY60 &= ~0x20u; delay_us(10);
    USB_PHY60 &= ~0xFu; delay_us(10);
    USB_PHY60 |= 0x20; delay_us(10);
    USB_CTL28 |= 0x800; USB_CTL28 &= ~0x800u;
    for (int i = 0; i < 1000000 && !(USB_CTL28 & 0x80); i++) { }

    USBMODE = 2;                                   /* device controller */
    volatile uint32_t *z = (volatile uint32_t *)M->qh;
    for (unsigned i = 0; i < (sizeof usb_store.qh + sizeof usb_store.td) / 4; i++) z[i] = 0;
    M->qh[0].caps = (64u << 16) | QH_IOS | QH_NO_ZLT;
    M->qh[1].caps = (64u << 16) | QH_NO_ZLT;
    M->qh[0].ov.next = M->qh[1].ov.next = 1;
    EPLISTADDR = PHYS(M->qh);
    DEVICEADDR = 0;
    USBCMD |= CMD_RUN;                             /* attach: the host resets the bus next */
}

static void controller_stop(void)
{
    USBCMD &= ~CMD_RUN;                            /* detach */
    delay_us(20000);
    DEV_EN &= ~(DEV_USB0 | DEV_USB1);
    DEV_INIT2 &= ~INIT_USB;
}

usb_stats_t usb_stats;

int usb_disk_run(void)
{
    units = (uint32_t)fat_mbr_units();
    block_size = 512 * units;
    blocks = (uint32_t)(platform_disk_sectors() / units);
    configured = ejected = need_reset = high_speed = 0;
    ahead_n = 0; ahead_due = 0; next_lba = ~0ull;
    sense_key = sense_asc = 0;
    controller_start();

    int cbw_primed = 0, result;
    uint32_t ejected_ms = 0, tick = USEC_TIMER;
    for (;;) {
        if (USEC_TIMER - tick > 500000) { tick = USEC_TIMER; usb_status_tick(); }
        if (!power_usb()) { result = USB_END_UNPLUGGED; break; }
        uint32_t st = USBSTS;
        USBSTS = st & (STS_PORT | STS_RESET | STS_SUSPEND);
        if (st & STS_RESET) { bus_reset(); cbw_primed = 0; usb_stats.resets++; }
        if (st & STS_PORT) high_speed = (PORTSC1 & PORT_SPEED) == PORT_HIGH;
        if (EPSETUPSTAT & 1) handle_setup();
        usb_stats.configured = configured;
        usb_stats.high_speed = high_speed;
        if (configured && !need_reset) {
            if (!cbw_primed) { prime(1, 0, PHYS(M->cbw), 512); cbw_primed = 1; }
            if (ahead_due) read_ahead();           /* the host's next CBW can land meanwhile */
            if (!(EPPRIME & 1) && !(M->td[2][0].token & TD_ACTIVE)) {
                cbw_primed = 0;
                EPCOMPLETE = 1;
                uint32_t len = 512 - ((M->td[2][0].token >> 16) & 0x7fff);
                uint8_t w[32];
                for (int i = 0; i < 31; i++) w[i] = ((volatile uint8_t *)M->cbw)[i];
                usb_stats.commands++;
                if (w[15] == 0x2A || w[15] == 0xAA) usb_stats.writes++;
                if (handle_cbw(w, len) == ABORT_UNPLUG) { result = USB_END_UNPLUGGED; break; }
            }
        }
        if (ejected) {
            uint32_t now = platform_now_ms();
            if (!ejected_ms) ejected_ms = now | 1;
            else if (now - ejected_ms > 1000) { result = USB_END_EJECTED; break; }
        }
    }
    ata_flush();
    controller_stop();
    return result;
}
