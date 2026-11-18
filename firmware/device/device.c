/*
 * The iPod side of platform.h: screen (BCM2722), click wheel, HOLD, time, and the main
 * loop, backlight and idle/sleep. Everything is polled with IRQs masked in the CPU; the
 * timer tick and the wheel are enabled as interrupt sources only so they can wake the
 * CPU from sleep (a masked interrupt still ends PROC_SLEEP). Audio DMA is the one FIQ.
 *
 * Safety rules for code at this level:
 *  - every hardware wait is bounded by an iteration count (never only by the timer);
 *  - if the screen can't work, reboot into Apple's disk mode, never a plain reboot
 *    (that would load this firmware again: a boot loop).
 */
#include "hw.h"
#include "platform.h"
#include "engine.h"
#include "app.h"
#include "gfx/gfx.h"
#include "fonts.h"
#include "dev.h"
#include "store.h"
#include "storage/fat.h"

int ata_init(void);

uint16_t fb[SCREEN_W * SCREEN_H] __attribute__((aligned(4)));

#define SPIN_LIMIT 2000000u

/* ---- reboot paths --------------------------------------------------------- */

static void __attribute__((noreturn)) reboot(void)
{
    audio_shutdown();                         /* mute first: no pop in the headphones */
    CPU_INT_DIS = 0xFFFFFFFF;
    COP_INT_DIS = 0xFFFFFFFF;
    DEV_RS |= DEV_SYSTEM;
    for (;;) { }
}

static void reboot_to_disk_mode(void)
{
    static const char magic[21] = "diskmode\0\0hotstuff\0\0\1";
    volatile char *dst = (volatile char *)DISKMODE_MAGIC_ADDR;
    for (unsigned i = 0; i < sizeof magic; i++)
        dst[i] = magic[i];
    reboot();
}

/* ---- cache and clock (sequences from Rockbox's system-pp502x.c) ------------------ */

#define NOP() __asm__ volatile("nop")

void cop_start(void);

void cache_init(uint32_t priority)
{
    CACHE_CTL &= ~(CACHE_CTL_ENABLE | CACHE_CTL_RUN);
    CACHE_CTL |= CACHE_CTL_INIT;
    CACHE_PRIORITY |= priority;          /* 0x10 CPU, 0x20 COP (Rockbox) */
    CACHE_MASK = 0x00001c00;           /* cache 0x00000000-0x03ffffff (and 0x2xxxxxxx) */
    CACHE_OPERATION = 0xfc0;
    CACHE_CTL |= CACHE_CTL_INIT | CACHE_CTL_ENABLE | CACHE_CTL_RUN;
    NOP(); NOP(); NOP(); NOP();
    /* Make every cache line valid by reading through 8 KB (not from address 0). */
    for (volatile char *p = (volatile char *)0x2000; p < (volatile char *)0x4000; p += 16)
        (void)*p;
}

/* Briefly hold the CPU while the clock source switches (Rockbox: scale_suspend_core). */
static inline void clock_hold(int on)
{
    CPU_CTL = on ? 0x48000003 : 0x4800001f;
    NOP();
}

/* 80 MHz = (20/3 x 24 MHz) / 2. If the PLL doesn't lock, stay on the 24 MHz crystal. */
static int cpu_80mhz(void)
{
    DEV_INIT2 |= INIT_PLL;
    PLL_CONTROL |= 0x88000000;
    clock_hold(1);
    CLOCK_SOURCE = 0x20002222;         /* run from 24 MHz while the PLL relocks */
    DEV_TIMING1 = 0x00000303;
    clock_hold(0);
    PLL_CONTROL = 0x8a121403;
    uint32_t n = 0;
    while (!(PLL_STATUS & PLL_LOCKED))
        if (++n > 1000000u) return 0;
    clock_hold(1);
    DEV_TIMING1 = 0x00000808;
    CLOCK_SOURCE = 0x20007777;         /* PLL */
    clock_hold(0);
    return 1;
}

/* ---- time -------------------------------------------------------------------- */

void delay_us(uint32_t us)
{
    uint32_t start = USEC_TIMER;
    for (uint32_t n = 0; n < us * 8u + 64u; n++)
        if (USEC_TIMER - start >= us) return;
}

static uint32_t last_us, now_ms, rem_us;

uint32_t platform_us(void) { return USEC_TIMER; }

uint32_t platform_now_ms(void)
{
    uint32_t t = USEC_TIMER;
    rem_us += t - last_us;
    last_us = t;
    now_ms += rem_us / 1000;
    rem_us %= 1000;
    return now_ms;
}

/* ---- screen -------------------------------------------------------------------- */

static int bcm_write_addr(uint32_t addr)
{
    BCM_WR_ADDR32 = addr;
    for (uint32_t n = 0; n < SPIN_LIMIT; n++)
        if (BCM_CONTROL & 0x2) return 1;
    return 0;
}

static int bcm_read32(uint32_t addr, uint32_t *out)
{
    uint32_t n = 0;
    while (!(BCM_RD_ADDR & 0x1))
        if (++n > SPIN_LIMIT) return 0;
    BCM_RD_ADDR32 = addr;
    for (n = 0; n < SPIN_LIMIT; n++)
        if (BCM_CONTROL & 0x10) { *out = BCM_DATA32; return 1; }
    return 0;
}

/* Wait (bounded) until the BCM has finished refreshing the panel. */
static void bcm_wait_idle(void)
{
    for (int tries = 0; tries < 20000; tries++) {
        uint32_t st;
        if (!bcm_read32(BCMA_COMMAND, &st)) return;
        if (st != BCMCMD_LCD_UPDATE && st != 0xFFFF) return;
    }
}

/* The BCM only executes a command after BCM_CONTROL = 0x31 (learned in Milestone 0). */
static void bcm_kick(void)
{
    if (bcm_write_addr(BCMA_COMMAND)) {
        BCM_DATA32 = BCMCMD_LCD_UPDATE;
        BCM_CONTROL = BCM_CONTROL_KICK;
    }
}

static int first_frame = 1;
/* Backlight off: the panel keeps its picture unlit and screen updates are held (the whole
 * frame goes out on wake). The BCM's LCD-sleep command is NOT used: tried 2026-11-17, a
 * plain update after it left the panel white; waking it needs the BCM re-bootstrapped. */
static int panel_asleep;

void platform_present(int x, int y, int w, int h)
{
    if (panel_asleep) return;     /* dark: fb keeps changing; the whole frame goes out on wake */
    int x0 = x & ~1, x1 = (x + w + 1) & ~1, y1 = y + h;     /* whole 32-bit words */
    if (x0 < 0) x0 = 0;
    if (y < 0) y = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    if (x0 >= x1 || y >= y1) return;
    if (first_frame) first_frame = 0; else bcm_wait_idle();
    for (int j = y; j < y1; j++) {
        if (!bcm_write_addr(BCMA_CMDPARAM + (uint32_t)(j * SCREEN_W + x0) * 2))
            return;
        const uint32_t *src = (const uint32_t *)&fb[j * SCREEN_W + x0];
        for (int i = 0, n = (x1 - x0) / 2; i < n; i++)
            BCM_DATA32 = src[i];
    }
    bcm_kick();
}

/* ---- backlight ----------------------------------------------------------------------
 * The backlight driver has a 32-step dimmer worked by pulses on GPIO D7: a short low pulse
 * (10 us) is one step brighter, a long one (200 us) one step dimmer, and it starts at step
 * 16 each time the backlight is powered up. Rockbox's bootloader leaves it at step 1, so it
 * is power-cycled at start. (Timings: Rockbox's backlight-nano_video.c.) */

#define BRIGHTNESS      24        /* 1..32 */
#define LIGHT_TIMEOUT_MS 20000    /* backlight and panel off after this long without input */

static int light;                 /* backlight on */

static void dimmer_pulse(uint32_t low_us)
{
    uint32_t psr;
    __asm__ volatile("mrs %0, cpsr\n orr r1, %0, #0x40\n msr cpsr_c, r1" : "=r"(psr) :: "r1", "memory");
    GPIO_CLEAR(GPIOD_OUTPUT_VAL, BACKLIGHT_D);
    delay_us(low_us);
    GPIO_SET(GPIOD_OUTPUT_VAL, BACKLIGHT_D);
    __asm__ volatile("msr cpsr_c, %0" :: "r"(psr) : "memory");
    delay_us(10);
}

static void backlight(int on)
{
    if (on) {
        GPIO_SET(GPIOB_ENABLE, BACKLIGHT_B);
        GPIO_SET(GPIOB_OUTPUT_EN, BACKLIGHT_B);
        GPIO_SET(GPIOD_ENABLE, BACKLIGHT_D);
        GPIO_SET(GPIOD_OUTPUT_EN, BACKLIGHT_D);
        GPIO_SET(GPIOL_ENABLE, BACKLIGHT_LED_L);
        GPIO_SET(GPIOL_OUTPUT_EN, BACKLIGHT_LED_L);
        GPIO_SET(GPIOB_OUTPUT_VAL, BACKLIGHT_B);
        GPIO_SET(GPIOD_OUTPUT_VAL, BACKLIGHT_D);
        delay_us(10000);
        for (int lv = 16; lv < BRIGHTNESS; lv++) dimmer_pulse(10);
        for (int lv = 16; lv > BRIGHTNESS; lv--) dimmer_pulse(200);
        GPIO_SET(GPIOL_OUTPUT_VAL, BACKLIGHT_LED_L);
    } else {
        GPIO_CLEAR(GPIOL_OUTPUT_VAL, BACKLIGHT_LED_L);
        GPIO_CLEAR(GPIOD_OUTPUT_VAL, BACKLIGHT_D);
        GPIO_CLEAR(GPIOB_OUTPUT_VAL, BACKLIGHT_B);
        delay_us(50000);          /* off long enough that the next power-up resets the dimmer */
    }
    light = on;
}

/* ---- click wheel ------------------------------------------------------------------
 * The wheel controller sends 32-bit packets: 0x8000001A | buttons << 8 | position << 16
 * (bit 30 = finger on the wheel, position 0..95 clockwise). Polled here. */

#define WHEEL_CLICKS 96
#define WHEEL_STEP   4          /* raw clicks per detent (24 detents per turn) */

static int wheel_old = -1, wheel_acc, wheel_detents;
static unsigned btn_now, btn_prev = 0x1F;   /* buttons held at boot don't count until released */
static unsigned btn_pressed;

static void wheel_init(void)
{
    DEV_EN |= DEV_OPTO;
    DEV_RS |= DEV_OPTO;
    delay_us(5);
    DEV_RS &= ~DEV_OPTO;
    DEV_INIT1 |= INIT_BUTTONS;
    WHEEL_CTRL = 0xc00a1f00;
    WHEEL_STATUS = 0x01000000;
}

static void wheel_service(void)
{
    if (!(WHEEL_STATUS & WHEEL_STATUS_READY)) return;
    delay_us(50);
    uint32_t st = WHEEL_DATA;
    if ((st & 0x800000ff) == 0x8000001a) {
        unsigned b = 0;
        if (st & 0x0100) b |= BTN_SELECT;
        if (st & 0x0200) b |= BTN_NEXT;
        if (st & 0x0400) b |= BTN_PREV;
        if (st & 0x0800) b |= BTN_PLAY;
        if (st & 0x1000) b |= BTN_MENU;
        btn_now = b;
        btn_pressed |= b & ~btn_prev;
        btn_prev = b;
        if (st & 0x40000000) {                         /* finger on the wheel */
            int pos = (int)((st >> 16) & 0x7f);
            if (wheel_old >= 0) {
                int d = pos - wheel_old;
                if (d < -WHEEL_CLICKS / 2) d += WHEEL_CLICKS;
                else if (d > WHEEL_CLICKS / 2) d -= WHEEL_CLICKS;
                wheel_acc += d;
                while (wheel_acc >= WHEEL_STEP)  { wheel_detents++; wheel_acc -= WHEEL_STEP; }
                while (wheel_acc <= -WHEEL_STEP) { wheel_detents--; wheel_acc += WHEEL_STEP; }
            }
            wheel_old = pos;
        } else {
            wheel_old = -1;                            /* finger lifted */
            wheel_acc = 0;
        }
    } else if ((st & 0x800000ff) != 0x8000003a && st != 0xFFFFFFFF) {
        delay_us(2000);                                /* unknown packet: resync the controller */
        WHEEL_CTRL &= ~0x60000000u;
        WHEEL_STATUS |= 0x04000000;
        WHEEL_CTRL |= 0x60000000u;
    }
    WHEEL_STATUS |= 0x0c000000;                        /* acknowledge, ready for the next packet */
    WHEEL_CTRL = 0x400a1f00;
}

static int hold_on(void) { return !(GPIOA_INPUT_VAL & HOLD_BIT); }

/* ---- platform.h ------------------------------------------------------------------ */

void platform_init(void)
{
    DEV_INIT1 |= INIT_BUTTONS;
    GPIO_SET(GPIOA_ENABLE, HOLD_BIT);
    GPIO_CLEAR(GPIOA_OUTPUT_EN, HOLD_BIT);
    backlight(0);                             /* power-cycle: resets the dimmer (see above) */
    backlight(1);
    i2c_init();                               /* before the wheel, as Rockbox does */
    power_init();
    boot_log("audio: codec");
    boot_log(audio_init() == 0 ? "audio: ready" : "audio: codec not responding");
    wheel_init();
    /* Wake sources for platform_wait's sleep (masked in the CPU, so no handlers):
     * a 10 ms tick, and the wheel. audio_init has just reset the interrupt enables. */
    TIMER1_CFG = 0;
    (void)TIMER1_VAL;
    TIMER1_CFG = 0xc0000000u | (10000 - 1);
    CPU_HI_INT_EN = WHEEL_HI_MASK;
    CPU_INT_EN = TIMER1_MASK | HI_MASK;
    last_us = USEC_TIMER;
    boot_log("disk: identify");
    if (ata_init() == 0) {
        char line[80], *p = line;
        const char *m = platform_disk_name();
        while (*m && p < line + 50) *p++ = *m++;
        *p++ = ',';
        *p++ = ' ';
        uint32_t gb = (uint32_t)(platform_disk_sectors() >> 21);   /* 2^21 sectors = 1 GiB */
        char tmp[12]; int n = 0;
        do { tmp[n++] = (char)('0' + gb % 10); gb /= 10; } while (gb);
        while (n) *p++ = tmp[--n];
        *p++ = ' '; *p++ = 'G'; *p++ = 'B'; *p = 0;
        boot_log(line);
        /* Before the app mounts it for the library: boot loader install (fwinstall.c),
         * then the battery log (power.c). Both only touch files that already exist. */
        if (fat_mount() == 0) {
            fw_maintenance();
            power_log_start();
        }
    } else {
        boot_log("disk: not responding");
    }
}

/* ---- power policy ----------------------------------------------------------------- */

#define PLAY_HOLD_OFF_MS 2000           /* hold PLAY this long: off (like Apple's firmware) */
#define IDLE_OFF_MS      (15u * 60 * 1000)   /* nothing playing and no input: off */

static uint32_t last_input_ms, last_sound_ms, play_down_ms;

/* USB plugged in while running, with HOLD on: disk mode (usb.c), until the computer ejects
 * the disk or the cable is pulled; then a restart once HOLD is off, which reloads the library
 * and any new firmware. Without HOLD a plug-in only charges. Booting with the cable in never
 * starts disk mode (HOLD at boot means Apple's firmware to the bootloader anyway). */
#define USB_SETTLE_MS 500

static int usb_seen = -1;         /* -1 = not sampled yet; then the settled state */
static uint32_t usb_change_ms, usb_off_ms;
static int hold_was;
static uint32_t hold_on_ms;       /* when HOLD last went on */

static void disk_screen(const char *title, const char *l1, const char *l2, const char *l3)
{
    gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = RGB(0x18181B);
    gfx_text(&font_title, 14, 70, title, RGB(0xECEBE6), 0, 1);
    gfx_fill(14, 100, 292, 2, RGB(0xFF6A1A), 32);
    if (l1) gfx_text(&font_body, 14, 112, l1, RGB(0xECEBE6), 292, 0);
    if (l2) gfx_text(&font_body, 14, 130, l2, RGB(0xA8A8AD), 292, 0);
    if (l3) gfx_text(&font_mono, 14, 210, l3, RGB(0x8E9097), 0, 2);
    panel_asleep = 0;
    platform_present(0, 0, SCREEN_W, SCREEN_H);
    bcm_wait_idle();
}

static char *put_dec(char *p, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void put_str(char **p, const char *s) { while (*s) *(*p)++ = *s++; }

static char *put_rate(char *p, uint32_t bytes, uint32_t us)
{
    uint32_t r10 = us ? (uint32_t)((uint64_t)bytes * 10 / us) : 0;    /* bytes/us = MB/s */
    p = put_dec(p, r10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + r10 % 10);
    return p;
}

/* Called by usb.c about twice a second: a status line, for diagnosing the connection and
 * the speed (MB/s while inside READ / WRITE commands, since disk mode started). */
void usb_status_tick(void)
{
    static uint32_t shown_cmds = ~0u, shown_cfg = ~0u;
    uint32_t cfg = (uint32_t)(usb_stats.configured | usb_stats.high_speed << 1);
    if (usb_stats.commands == shown_cmds && cfg == shown_cfg) return;
    shown_cmds = usb_stats.commands;
    shown_cfg = cfg;
    char line[96], *p = line;
    if (!usb_stats.configured) put_str(&p, "WAITING FOR THE COMPUTER");
    else {
        put_str(&p, usb_stats.high_speed ? "USB 2.0 HS" : "USB FS");
        put_str(&p, "  RD "); p = put_rate(p, usb_stats.rd_bytes, usb_stats.rd_us);
        put_str(&p, "  WR "); p = put_rate(p, usb_stats.wr_bytes, usb_stats.wr_us);
        put_str(&p, " MB/S  PIO "); p = put_dec(p, (uint32_t)ata_pio_mode());
        int um = ata_udma_mode();
        put_str(&p, um >= 0 ? "  UDMA " : "  NO DMA");
        if (um >= 0) p = put_dec(p, (uint32_t)um);
    }
    *p = 0;
    gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
    gfx_fill(0, 204, SCREEN_W, 20, RGB(0x18181B), 32);
    gfx_text(&font_mono, 14, 210, line, RGB(0x8E9097), 0, 2);
    platform_present(0, 204, SCREEN_W, 20);
}

static void disk_mode(void) __attribute__((noreturn));
static void disk_mode(void)
{
    power_log_flush();                         /* the computer owns the disk from here on */
    audio_shutdown();
    if (!light) backlight(1);
    disk_screen("DISK MODE", "Connected to the computer.", "Eject the iPod there before unplugging it.", 0);
    int r = usb_disk_run();
    if (r == USB_END_EJECTED)
        disk_screen("EJECTED", "Safe to unplug.", "Slide HOLD off to restart Knurl.", 0);
    else
        disk_screen("UNPLUGGED", "The cable was pulled before the computer ejected the iPod.",
                    "Slide HOLD off to restart Knurl.", 0);
    uint32_t t0 = USEC_TIMER;
    while (hold_on()) {
        if (light && USEC_TIMER - t0 > 30000000u) backlight(0);
        delay_us(20000);
    }
    reboot();
}

static void usb_service(uint32_t now)
{
    int usb = power_usb();
    int h = hold_on();
    if (h && !hold_was) hold_on_ms = now;
    hold_was = h;
    if (usb_seen < 0) { usb_seen = usb; usb_off_ms = now; return; }
    if (usb == usb_seen) { usb_change_ms = 0; return; }
    if (!usb_change_ms) { usb_change_ms = now | 1; return; }
    if (now - usb_change_ms < USB_SETTLE_MS) return;
    uint32_t plugged_at = usb_change_ms;
    usb_seen = usb;
    usb_change_ms = 0;
    if (!usb) { usb_off_ms = now; return; }
    /* Only a real plug-in with HOLD already on: the cable was out (as seen by us) for a second
     * or more, and HOLD went on before the cable went in. Turning HOLD on with the cable in
     * never starts disk mode, even if the USB-detect line changes state later. */
    if (h && (int32_t)(plugged_at - hold_on_ms) > 200 && plugged_at - usb_off_ms >= 1000) disk_mode();
}

static void go_dark(void)
{
    bcm_wait_idle();
    backlight(0);
    panel_asleep = 1;
}

static void wake(void)
{
    panel_asleep = 0;
    platform_present(0, 0, SCREEN_W, SCREEN_H);   /* what changed in the dark */
    bcm_wait_idle();                              /* drawn before the light comes on */
    backlight(1);
}

static void shut_down(const char *why, int code) __attribute__((noreturn));
static void shut_down(const char *why, int code)
{
    power_log_event(code);
    if (!panel_asleep) {
        gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0;
        if (why) gfx_text(&font_mono, 14, 110, why, RGB(0xB8B9BE), 0, 2);
        platform_present(0, 0, SCREEN_W, SCREEN_H);
        bcm_wait_idle();
        if (why) delay_us(1500000);
    }
    backlight(0);
    power_off();
}

void platform_poll(input_t *in)
{
    for (int i = 0; i < 16 && (WHEEL_STATUS & WHEEL_STATUS_READY); i++)
        wheel_service();                  /* every waiting packet, so quick presses aren't lost */
    uint32_t now = platform_now_ms();
    power_service(now);
    usb_service(now);

    int locked = hold_on();
    if (locked) { wheel_detents = 0; btn_pressed = 0; }   /* HOLD: the buttons do nothing */
    int any = wheel_detents || btn_pressed || (btn_now && !locked);
    if (any) last_input_ms = now;
    if (audio_playing()) last_sound_ms = now;

    if (!light && any) {                  /* the first touch in the dark only wakes it */
        wake();
        wheel_detents = 0;
        btn_pressed = 0;
    } else if (light && now - last_input_ms >= LIGHT_TIMEOUT_MS) {
        go_dark();
    }

    if ((btn_now & BTN_PLAY) && !locked) {
        if (!play_down_ms) play_down_ms = now | 1;
        else if (now - play_down_ms >= PLAY_HOLD_OFF_MS) shut_down(0, LOG_OFF_BUTTON);
    } else {
        play_down_ms = 0;
    }
    if (now - last_input_ms >= IDLE_OFF_MS && now - last_sound_ms >= IDLE_OFF_MS) shut_down(0, LOG_OFF_IDLE);
    if (power_low_battery()) shut_down("BATTERY LOW", LOG_OFF_BATTERY);

    in->wheel = wheel_detents;
    in->pressed = btn_pressed;
    in->hold = hold_on();
    wheel_detents = 0;
    btn_pressed = 0;
}

/* ---- idle ---------------------------------------------------------------------------
 * Sleep until the next 10 ms tick, a wheel packet, or an audio FIQ, for up to ~10 ms in
 * all. (Before this the CPU spun here, busy 100% of the time.) */

static uint32_t awake_t0, asleep_us, busy_pct = 100;

void platform_wait(void)
{
    uint32_t start = USEC_TIMER;
    while (USEC_TIMER - start < 10000) {
        if (WHEEL_STATUS & WHEEL_STATUS_READY) break;
        if (hold_on()) break;
        (void)TIMER1_VAL;                 /* acknowledge the last tick */
        uint32_t t = USEC_TIMER;
        CPU_CTL = PROC_SLEEP;
        __asm__ volatile("nop\n nop\n nop");
        asleep_us += USEC_TIMER - t;
    }
    uint32_t span = USEC_TIMER - awake_t0;
    if (span >= 1000000) {
        busy_pct = asleep_us >= span ? 0 : (uint32_t)(100 - (uint64_t)asleep_us * 100 / span);
        awake_t0 = USEC_TIMER;
        asleep_us = 0;
    }
}

int platform_cpu_busy(void) { return (int)busy_pct; }

/* HOLD on: reboot; with HOLD on, the bootloader starts Apple's firmware. */
void platform_exit(void)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0x18C3;   /* graphite */
    for (int y = 196; y < 200; y++)
        for (int x = 24; x < 296; x++) fb[y * SCREEN_W + x] = 0xFB43;  /* orange line */
    platform_present(0, 0, SCREEN_W, SCREEN_H);
    power_log_flush();
    delay_us(300000);
    reboot();
}


/* ---- boot log and crash screen ------------------------------------------------------ */

static int boot_line = -1;

void boot_log(const char *line)
{
    gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
    if (boot_line < 0) {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = RGB(0x18181B);
        gfx_text(&font_mono, 14, 10, "KNURL  STARTING", RGB(0xB8B9BE), 0, 2);
        platform_present(0, 0, SCREEN_W, SCREEN_H);
        boot_line = 0;
    }
    int y = 30 + boot_line * 14;
    if (y > SCREEN_H - 16) return;
    gfx_text(&font_body, 14, y, line, RGB(0xECEBE6), 292, 0);
    platform_present(0, y, SCREEN_W, 14);
    boot_line++;
}

static char *hex32(char *p, uint32_t v)
{
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[(v >> i) & 15];
    *p = 0;
    return p;
}

void crash(int kind, uint32_t at, uint32_t lr) __attribute__((noreturn));
void crash(int kind, uint32_t at, uint32_t lr)
{
    static const char *const names[] = { "?", "undefined instruction", "SWI", "prefetch abort",
                                         "data abort", "reserved vector", "IRQ", "FIQ" };
    char a[40], b[40], *p;
    gfx_bounds(0, 0, SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = RGB(0x18181B);
    gfx_fill(14, 44, 292, 2, RGB(0xFF6A1A), 32);
    gfx_text(&font_title, 14, 10, "CRASH", RGB(0xECEBE6), 0, 1);
    gfx_text(&font_row, 14, 56, names[kind & 7], RGB(0xFFFFFF), 292, 0);
    p = a; p = hex32(p, at);
    gfx_text(&font_mono, 14, 80, "AT", RGB(0x8E9097), 0, 2);
    gfx_text(&font_mono, 60, 80, a, RGB(0xECEBE6), 0, 1);
    p = b; p = hex32(p, lr);
    gfx_text(&font_mono, 14, 96, "LR", RGB(0x8E9097), 0, 2);
    gfx_text(&font_mono, 60, 96, b, RGB(0xECEBE6), 0, 1);
    gfx_text(&font_body, 14, 130, "Flip HOLD on to restart into Apple's software.", RGB(0xA8A8AD), 292, 0);
    first_frame = 1;
    platform_present(0, 0, SCREEN_W, SCREEN_H);
    for (;;)
        if (hold_on()) reboot();
}   /* PMU/ADC driver comes later */

int main(void)
{
    boot_stamp_read();                    /* which loader started us (fwinstall.c) */
    if (!(GPO32_VAL & GPO32_BCM_POWER))
        reboot_to_disk_mode();
    cache_init(0x10);
    cpu_80mhz();
    platform_init();
    engine_init();
    cop_start();                          /* the decoder core (device/cop.c) */
    app_init(platform_now_ms());
    input_t in;
    for (;;) {
        platform_poll(&in);
        if (!app_step(&in, platform_now_ms()))
            platform_wait();
    }
}
