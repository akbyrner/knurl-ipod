/*
 * Audio output: WM8758 codec (I2C control, I2S data) fed by DMA from a ring of chunks.
 *
 * Register sequences follow Rockbox's wm8758.c / wmcodec-pp.c / i2s-pp.c / pcm-pp.c
 * (reference/rockbox); the playback design is ours:
 *
 *  - The ring (AUDIO_CHUNKS x AUDIO_CHUNK_FRAMES stereo frames) lives in SDRAM and is only
 *    ever touched through its uncached physical alias, so the CPU's cache never holds a
 *    stale or dirty copy of what DMA reads. No cache flushing anywhere.
 *  - DMA channel 0 plays one chunk at a time. Its completion interrupt is routed to FIQ;
 *    the handler (audio_fiq) starts the next queued chunk, or a short block of silence if
 *    the producer fell behind, so DMA never stops and a late producer costs a gap, never
 *    a stuck sound. Producer and FIQ share two counters (queued, played): the producer
 *    only writes `queued`, the FIQ only writes `played`, so no locking is needed.
 *  - The producer is the decoder core (COP). Everything both cores touch is in the SHARED
 *    block, read and written uncached (src/shared.h).
 */
#include <stddef.h>
#include "hw.h"
#include "platform.h"
#include "shared.h"
#include "dev.h"

/* ---- WM8758 registers (see the WM8758/WM8983 datasheets; values as Rockbox uses them) */
enum {
    WM_PWRMGMT1 = 0x01, WM_PWRMGMT2 = 0x02, WM_PWRMGMT3 = 0x03, WM_AINTFCE = 0x04,
    WM_CLKCTRL = 0x06, WM_ADDCTRL = 0x07, WM_DACCTRL = 0x0a, WM_LDACVOL = 0x0b,
    WM_RDACVOL = 0x0c, WM_PLLN = 0x24, WM_OUT4TOADC = 0x2a, WM_OUTCTRL = 0x31,
    WM_LOUTMIX = 0x32, WM_ROUTMIX = 0x33, WM_LOUT1VOL = 0x34, WM_ROUT1VOL = 0x35,
    WM_LOUT2VOL = 0x36, WM_ROUT2VOL = 0x37, WM_OUT3MIX = 0x38, WM_OUT4MIX = 0x39,
    WM_BIASCTRL = 0x3d,
};
#define WM_ADDR 0x1a

#define CHUNK_BYTES (AUDIO_CHUNK_FRAMES * 4)
#define SILENCE_BYTES 1024                     /* 256 frames = 5.8 ms */

static uint32_t ring_store[(AUDIO_CHUNKS * CHUNK_BYTES + SILENCE_BYTES) / 4]
    __attribute__((section(".noinit.audio"), aligned(32)));

#define RING_PHYS    (SDRAM_PHYS + (uint32_t)(uintptr_t)ring_store)
#define SILENCE_PHYS (RING_PHYS + AUDIO_CHUNKS * CHUNK_BYTES)

static struct {
    uint32_t queued;                           /* producer (COP) */
    uint32_t played, underruns, cur_real;      /* FIQ (CPU) */
    uint32_t active, paused, ready;            /* CPU */
    uint32_t armed;                            /* a real chunk has played since the last flush */
} shared_store SHARED;
#define A ((volatile __typeof__(shared_store) *)UNCACHED(&shared_store))

/* ---- codec control over I2C (i2c.c) ------------------------------------------------ */

static int wm_write(int reg, int val)
{
    uint8_t b[2] = { (uint8_t)((reg << 1) | ((val >> 8) & 1)), (uint8_t)(val & 0xff) };
    return i2c_write(WM_ADDR, b, 2);
}

/* ---- DMA + FIQ -------------------------------------------------------------------- */

#define DMA_PLAY ((DMA_REQ_IIS << 16) | DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | \
                  DMA_CMD_WAIT_REQ | DMA_CMD_INTR | DMA_CMD_START)

static inline void dma_play(uint32_t phys, uint32_t bytes)
{
    DMA0_RAM_ADDR = phys;
    DMA0_CMD = DMA_PLAY | (bytes - 4);
}

static inline void dma_next(void)
{
    if (A->queued != A->played && !A->paused) {
        A->cur_real = 1;
        A->armed = 1;
        dma_play(RING_PHYS + (A->played % AUDIO_CHUNKS) * CHUNK_BYTES, CHUNK_BYTES);
    } else {
        /* Running dry mid-song is a gap; right after a skip (flush) it's just the new track
         * still decoding, so it doesn't count until that track's audio has started. */
        if (A->cur_real && A->active && !A->paused && A->armed) A->underruns = A->underruns + 1;
        A->cur_real = 0;
        dma_play(SILENCE_PHYS, SILENCE_BYTES);
    }
}

void __attribute__((interrupt("FIQ"))) audio_fiq(void)
{
    (void)DMA0_STATUS;                         /* acknowledge */
    if (A->cur_real) A->played = A->played + 1;
    dma_next();
}

static void fiq_enable(void)  { __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x40\n msr cpsr_c, r0" ::: "r0", "memory"); }
static void fiq_disable(void) { __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0x40\n msr cpsr_c, r0" ::: "r0", "memory"); }

/* ---- volume ----------------------------------------------------------------------- */

static int volume_db = -30;

void audio_set_volume(int db)
{
    if (db > 6) db = 6;
    volume_db = db;
    if (!A->ready) return;
    /* Rockbox's split: above -57 dB the DAC stays at 0 dB and the headphone amp moves
     * (better noise); below that the amp sits at its minimum and the DAC attenuates. */
    int dac, amp, v = db <= -90 ? 0 : db + 90;
    if (v > 32)      { dac = 255; amp = v - 90 + 57; }
    else if (v > 0)  { dac = (v - 33) * 2 + 255; amp = 0; }
    else             { dac = 0; amp = 0x40; }   /* mute */
    wm_write(WM_LDACVOL, dac);
    wm_write(WM_RDACVOL, dac | 0x100);         /* DACVU: both channels update together */
    wm_write(WM_LOUT1VOL, amp | 0x80);         /* zero-cross */
    wm_write(WM_ROUT1VOL, amp | 0x80 | 0x100); /* OUT1VU */
}

int audio_volume(void) { return volume_db; }

/* ---- platform.h ------------------------------------------------------------------- */

static void wait_us(uint32_t us) { uint32_t t = USEC_TIMER; while (USEC_TIMER - t < us) {} }

int audio_init(void)
{
    A->queued = A->played = A->underruns = A->cur_real = 0;
    A->active = A->paused = A->ready = 0;
    volatile uint32_t *sil = (volatile uint32_t *)SILENCE_PHYS;
    for (int i = 0; i < SILENCE_BYTES / 4; i++) sil[i] = 0;

    /* I2S pins, block, and the codec's 24 MHz master clock */
    DEV_INIT2 &= ~0x300u;
    DEV_INIT1 &= ~0x3000000u;
    DEV_RS |= DEV_I2S;
    DEV_RS &= ~DEV_I2S;
    DEV_EN |= DEV_I2S | DEV_EXTCLOCKS;
    DEV_EXTCLOCK_CFG &= ~0xcu;

    IISCONFIG |= IIS_RESET;
    IISCONFIG &= ~IIS_RESET;
    IISCONFIG &= ~(IIS_FORMAT_MASK | IIS_SIZE_MASK);             /* I2S, 16 bit */
    IISCONFIG = (IISCONFIG & ~IIS_FIFO_FORMAT_MASK) | IIS_FIFO_FORMAT_LE16_2;
    IISFIFO_CFG |= IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4;
    IISFIFO_CFG |= IIS_RXCLR | IIS_TXCLR;

    /* Codec power-up with every output muted (the order Rockbox uses, to avoid pops). */
    int err = 0;
    err |= wm_write(WM_BIASCTRL, 0x100);                         /* low bias */
    err |= wm_write(WM_OUTCTRL, 0x100 | 0x80 | 0x04 | 0x02 | 0x01);
    err |= wm_write(WM_LOUT1VOL, 0x140);
    err |= wm_write(WM_ROUT1VOL, 0x140);
    err |= wm_write(WM_LOUT2VOL, 0x140);
    err |= wm_write(WM_ROUT2VOL, 0x140);
    err |= wm_write(WM_OUT3MIX, 0x40);
    err |= wm_write(WM_OUT4MIX, 0x40);
    err |= wm_write(WM_PWRMGMT2, 0x180);                         /* LOUT1, ROUT1 */
    err |= wm_write(WM_OUT4TOADC, 0x04);                         /* VMID independent bias */
    err |= wm_write(WM_PWRMGMT3, 0x0f);                          /* DACs, mixers */
    err |= wm_write(WM_PWRMGMT1, 0x20 | 0x08 | 0x04 | 0x03);     /* PLL, bias, buffers, VMID 10k */
    err |= wm_write(WM_AINTFCE, 2 << 3);                         /* I2S, 16 bit */
    err |= wm_write(WM_CLKCTRL, 0x01);                           /* codec is clock master */
    /* 44.1 kHz: 12 MHz in (24 MHz / prescale 2) x 7.5264 = 90.3168 MHz, /4 = 22.5792 MHz,
     * /2 = 11.2896 MHz = 256 fs. */
    err |= wm_write(WM_PLLN, 0x10 | 0x7);
    err |= wm_write(WM_PLLN + 1, 0x21);
    err |= wm_write(WM_PLLN + 2, 0x161);
    err |= wm_write(WM_PLLN + 3, 0x26);
    err |= wm_write(WM_CLKCTRL, 0x100 | (2 << 5) | (1 << 2) | 0x01);
    err |= wm_write(WM_ADDCTRL, 0x01);                           /* 48k filters, slow clock */
    err |= wm_write(WM_LOUTMIX, 0x01);                           /* DAC -> mixer */
    err |= wm_write(WM_ROUTMIX, 0x01);
    err |= wm_write(WM_OUT4TOADC, 0);
    if (err) return -1;

    /* DMA channel 0 -> I2S FIFO; its interrupt is the only one we take, as FIQ. */
    CPU_INT_DIS = ~0u;
    CPU_HI_INT_DIS = ~0u;
    CPU_HI_INT_PRIORITY = 0;
    CPU_INT_PRIORITY = DMA_IRQ_MASK;
    DMA_MASTER_CONTROL |= DMA_MASTER_EN;
    DMA_REQ_STATUS |= 1u << DMA_REQ_IIS;
    (void)DMA0_STATUS;
    DMA0_PER_ADDR = (uint32_t)(uintptr_t)&IISFIFO_WR;
    DMA0_FLAGS = DMA_FLAGS_UNK26;
    DMA0_INCR = DMA_INCR_RANGE_FIXED | DMA_INCR_WIDTH_32BIT;
    IISCONFIG |= IIS_TXFIFOEN;

    dma_next();                                /* silence until something is queued */
    CPU_INT_EN = DMA_IRQ_MASK;
    fiq_enable();

    /* Let VMID charge with the outputs still muted, then drop to the low-power VMID,
     * unmute the DAC and set the volume. */
    wait_us(250000);
    wm_write(WM_PWRMGMT1, 0x20 | 0x08 | 0x04 | 0x02);
    wm_write(WM_BIASCTRL, 0);
    wm_write(WM_DACCTRL, 0x08);                                  /* 128x oversampling, unmuted */
    A->ready = 1;
    audio_set_volume(volume_db);
    return 0;
}

int audio_free_chunks(void)
{
    if (!A->ready) return 0;
    return AUDIO_CHUNKS - (int)(A->queued - A->played);
}

int16_t *audio_chunk(void)
{
    if (audio_free_chunks() <= 0) return NULL;
    return (int16_t *)(RING_PHYS + (A->queued % AUDIO_CHUNKS) * CHUNK_BYTES);
}

void audio_commit(void) { if (audio_free_chunks() > 0) A->queued = A->queued + 1; }
int  audio_pending(void) { return A->ready ? (int)(A->queued - A->played) : 0; }
void audio_active(int on) { A->active = (uint32_t)on; }
int  audio_playing(void) { return A->ready && A->active && !A->paused; }
uint32_t audio_underruns(void) { return A->underruns; }

/* Pause: soft-mute (the codec ramps down, no click), then let the FIQ play silence without
 * consuming the ring, so resuming carries on exactly where it stopped. */
void audio_pause(int on)
{
    if (!A->ready || (uint32_t)on == A->paused) return;
    if (on) {
        wm_write(WM_DACCTRL, 0x08 | 0x40);
        wait_us(12000);
        A->paused = 1;
    } else {
        A->paused = 0;
        wm_write(WM_DACCTRL, 0x08);
    }
}

/* Drop everything queued except the chunk DMA is playing right now (skip, new track).
 * Only while the producer is stopped (engine_restart). */
void audio_flush(void)
{
    if (!A->ready) return;
    fiq_disable();
    A->queued = A->played + A->cur_real;
    A->armed = 0;
    fiq_enable();
}

/* Mute before a reboot so the headphones don't pop. */
void audio_shutdown(void)
{
    if (!A->ready) return;
    wm_write(WM_DACCTRL, 0x40);                                  /* soft mute */
    wm_write(WM_LOUT1VOL, 0x140);
    wm_write(WM_ROUT1VOL, 0x140);
    wait_us(20000);
    fiq_disable();
    CPU_INT_DIS = DMA_IRQ_MASK;
    DMA0_CMD = 0;
    A->ready = 0;
}
