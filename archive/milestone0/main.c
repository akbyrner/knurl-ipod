/*
 * Milestone 0: prove the pipeline. Boots from the Rockbox bootloader, shows a
 * Knurl-style splash, animates the notched progress bar (proving the timer and
 * partial screen updates work), and reboots when HOLD is switched on (with HOLD
 * on, the bootloader then starts Apple's firmware, so there's always a way back).
 *
 * Safety rules for code at this level:
 *  - every hardware wait is bounded by an iteration count (never only by the
 *    timer, which might not be running);
 *  - anything that can't work falls back to Apple's disk mode, never to a plain
 *    reboot (which would load this firmware again: a boot loop).
 */
#include "hw.h"

extern const uint16_t splash_pixels[LCD_WIDTH * LCD_HEIGHT]; /* RGB565, from tools/render_splash.py */

static uint16_t fb[LCD_WIDTH * LCD_HEIGHT] __attribute__((aligned(4)));

#define SPIN_LIMIT 2000000u   /* far longer than any real BCM wait */

/* ---- reboot paths ------------------------------------------------------ */

static void reboot(void)
{
    CPU_INT_DIS = 0xFFFFFFFF;
    COP_INT_DIS = 0xFFFFFFFF;
    DEV_RS |= DEV_SYSTEM;
    for (;;) { }
}

/* Reboot into Apple's disk mode (USB drive), so a PC can always fix the iPod. */
static void reboot_to_disk_mode(void)
{
    static const char magic[21] = "diskmode\0\0hotstuff\0\0\1";
    volatile char *dst = (volatile char *)DISKMODE_MAGIC_ADDR;
    for (unsigned i = 0; i < sizeof magic; i++)
        dst[i] = magic[i];
    reboot();
}

/* ---- BCM2722 access (all waits bounded) ------------------------------- */

static int bcm_write_addr(uint32_t addr)
{
    BCM_WR_ADDR32 = addr;
    for (uint32_t n = 0; n < SPIN_LIMIT; n++)
        if (BCM_CONTROL & 0x2) return 1;      /* ready for data */
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

static void bcm_wait_idle(void)
{
    /* The command word reads back as the update command (or 0xFFFF) while the
     * BCM is still refreshing the panel. */
    for (int tries = 0; tries < 20000; tries++) {
        uint32_t s;
        if (!bcm_read32(BCMA_COMMAND, &s)) return;
        if (s != BCMCMD_LCD_UPDATE && s != 0xFFFF) return;
    }
}

/* Ask the BCM to refresh the panel from its framebuffer. The command word alone
 * does nothing: the BCM only acts on it after BCM_CONTROL is written with 0x31
 * (every Rockbox draw path does this; Milestone 0 v1 left it out and the screen
 * never changed). */
static void bcm_kick(void)
{
    if (bcm_write_addr(BCMA_COMMAND)) {
        BCM_DATA32 = BCMCMD_LCD_UPDATE;
        BCM_CONTROL = BCM_CONTROL_KICK;
    }
}

static int first_frame = 1;

/* Push rows [y0, y0+h) of the framebuffer, columns [x0, x0+w) (x0, w even). */
static void lcd_update_rect(int x0, int y0, int w, int h)
{
    if (first_frame)
        first_frame = 0;      /* like the bootloader: don't wait before the first frame */
    else
        bcm_wait_idle();      /* bounded; if the BCM looks stalled we re-kick anyway */
    for (int y = y0; y < y0 + h; y++) {
        if (!bcm_write_addr(BCMA_CMDPARAM + (uint32_t)(y * LCD_WIDTH + x0) * 2))
            return;
        const uint32_t *src = (const uint32_t *)&fb[y * LCD_WIDTH + x0];
        for (int i = 0; i < w / 2; i++)
            BCM_DATA32 = src[i];               /* two pixels per write */
    }
    bcm_kick();
}

/* ---- inputs, outputs, time ---------------------------------------------- */

static int hold_on(void) { return !(GPIOA_INPUT_VAL & HOLD_BIT); }

static void backlight_on(void)
{
    GPIO_SET(GPIOB_ENABLE, BACKLIGHT_B);
    GPIO_SET(GPIOB_OUTPUT_EN, BACKLIGHT_B);
    GPIO_SET(GPIOB_OUTPUT_VAL, BACKLIGHT_B);
    GPIO_SET(GPIOD_ENABLE, BACKLIGHT_D);
    GPIO_SET(GPIOD_OUTPUT_EN, BACKLIGHT_D);
    GPIO_SET(GPIOD_OUTPUT_VAL, BACKLIGHT_D);
    GPIO_SET(GPIOL_ENABLE, BACKLIGHT_LED_L);
    GPIO_SET(GPIOL_OUTPUT_EN, BACKLIGHT_LED_L);
    GPIO_SET(GPIOL_OUTPUT_VAL, BACKLIGHT_LED_L);
}

/* Sleep up to `us` microseconds; returns early (1) if HOLD goes on. Also capped
 * by an iteration count in case the timer isn't running. */
static int wait_us(uint32_t us)
{
    uint32_t start = USEC_TIMER;
    for (uint32_t n = 0; n < us * 8u; n++) {
        if (hold_on()) return 1;
        if (USEC_TIMER - start >= us) break;
    }
    return hold_on();
}

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            fb[j * LCD_WIDTH + i] = c;
}

/* ---- the notched progress bar (Knurl's NotchedBar) --------------------- */

#define BAR_X     24
#define BAR_W     272
#define BAR_Y     196
#define BAR_STEPS 16

static void draw_bar(int filled /* 0..BAR_W */)
{
    const uint16_t track = rgb565(0x3A3A40), bone = rgb565(0xECEBE6),
                   notch = rgb565(0x7A4326),  /* orange at ~45% over graphite */
                   head = rgb565(0xFFB27A), orange = rgb565(0xFF6A1A);
    for (int j = BAR_Y - 4; j < BAR_Y + 8; j++)                    /* background back */
        for (int i = BAR_X - 4; i < BAR_X + BAR_W + 4; i++)
            fb[j * LCD_WIDTH + i] = splash_pixels[j * LCD_WIDTH + i];
    for (int s = 0; s <= BAR_STEPS; s++)                           /* notches */
        fill(BAR_X + (BAR_W - 2) * s / BAR_STEPS, BAR_Y - 2, 2, 8, notch);
    fill(BAR_X, BAR_Y, BAR_W, 4, track);                            /* track */
    fill(BAR_X, BAR_Y, filled, 4, bone);                            /* played */
    int hx = BAR_X + filled - 1;
    if (hx < BAR_X) hx = BAR_X;
    fill(hx - 1, BAR_Y - 3, 3, 10, orange);                         /* playhead */
    fill(hx, BAR_Y - 2, 1, 8, head);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    if (!(GPO32_VAL & GPO32_BCM_POWER))
        reboot_to_disk_mode();               /* can't show anything: be recoverable */

    DEV_INIT1 |= INIT_BUTTONS;               /* needed for HOLD detection */
    GPIO_SET(GPIOA_ENABLE, HOLD_BIT);
    GPIO_CLEAR(GPIOA_OUTPUT_EN, HOLD_BIT);
    backlight_on();

    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        fb[i] = splash_pixels[i];
    draw_bar(0);
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    int filled = 0;
    for (;;) {
        if (wait_us(40000))
            break;
        filled = (filled + 2) % (BAR_W + 1);
        draw_bar(filled);
        lcd_update_rect(BAR_X - 4, BAR_Y - 4, BAR_W + 8, 12);
    }

    /* HOLD is on: show an orange line, then reboot into the bootloader (which
     * starts Apple's firmware because HOLD is on). */
    fill(0, 0, LCD_WIDTH, LCD_HEIGHT, rgb565(0x18181B));
    fill(BAR_X, BAR_Y, BAR_W, 4, rgb565(0xFF6A1A));
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    wait_us(300000);
    reboot();
    return 0;
}
