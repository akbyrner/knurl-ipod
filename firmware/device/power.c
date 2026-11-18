/*
 * Power: the PCF50605 power chip (I2C 0x08) for supply voltages, the battery ADC and
 * deep sleep, plus the charger pins. Register numbers, voltages and the battery curve are
 * hardware facts from Rockbox (drivers/pcf50605.c, export/pcf5060x.h,
 * target/arm/ipod/{power-ipod,adc-ipod-pcf,powermgmt-ipod-pcf}.c).
 */
#include "hw.h"
#include "platform.h"
#include "dev.h"
#include "store.h"

#define PCF_ADDR    0x08
#define PCF_OOCC1   0x08          /* on/off control: GOSTDBY | wake sources */
#define PCF_DCDC1   0x1b
#define PCF_DCDC2   0x1c
#define PCF_DCUDC1  0x21
#define PCF_IOREGC  0x23
#define PCF_D1REGC1 0x24
#define PCF_D3REGC1 0x26
#define PCF_ADCC2   0x2f          /* write (channel << 1) | 1: start a 10-bit conversion */
#define PCF_ADCS1   0x30          /* result: 8 high bits, then 2 low bits in ADCS2 */
#define ADC_BATTERY 2             /* ADCVIN1: battery through a divider, 1024 = 6000 mV */
#define GOSTDBY     0x01
#define CHGWAK      0x20          /* a charger wakes it */
#define EXTONWAK    0x40          /* a button wakes it */

#define GPIOB_INPUT_VAL REG32(0x6000d034)
#define GPIOL_INPUT_VAL REG32(0x6000d13c)
#define USB_BIT  0x10             /* GPIO L4 high: USB power (Rockbox usb-fw-pp502x.c) */
#define MAIN_BIT 0x08             /* GPIO L3 low: FireWire / main charger */

static void pcf_write(int reg, int val)
{
    uint8_t b[2] = { (uint8_t)reg, (uint8_t)val };
    i2c_write(PCF_ADDR, b, 2);
}

/* The supply voltages Rockbox runs this model at (lower than Apple's defaults, same
 * hardware): I/O 3.0 V, core 1.2 V, DCDC2 off, 1.8 V rail, codec 2.5 V, LCD 2.6 V. */
void power_init(void)
{
    GPIO_SET(GPIOL_ENABLE, USB_BIT);                      /* USB detect: GPIO input */
    GPIO_CLEAR(GPIOL_OUTPUT_EN, USB_BIT);
    /* USB charging at 500 mA: the LTC4066 charger's HPWR pin (GPIO A2) high, SUSP (GPIO L2)
     * low. Without this it draws 100 mA, a day to fill a 2000 mAh cell. (Pins: Rockbox's
     * power-ipod.c, which does the same in its "force" USB charging mode.) */
    GPIO_SET(GPIOA_OUTPUT_VAL, 0x04);
    GPIO_CLEAR(GPIOL_OUTPUT_VAL, 0x04);
    pcf_write(PCF_IOREGC, 0xf5);
    pcf_write(PCF_DCDC1, 0xec);
    pcf_write(PCF_DCDC2, 0x0c);
    pcf_write(PCF_DCUDC1, 0xe3);
    pcf_write(PCF_D1REGC1, 0xf0);
    pcf_write(PCF_D3REGC1, 0xf1);
}

/* ---- battery ------------------------------------------------------------------------ */

/* mV at 0%, 10%, ... 100% (iPod Video, Rockbox's table for the stock cell). To fit this iPod's
 * 2000 mAh cell instead: run it down once, then tools/battery_fit.py on /.knurl/battery.log. */
static const uint16_t curve[11] = { 3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180 };
#define SHUTOFF_MV 3300

static uint32_t next_read_ms;
static int avg_mv = -1;           /* smoothed battery voltage */
static int low_count;

/* ---- battery log: /.knurl/battery.log ----------------------------------------------------
 * The computer creates the file zero-filled (any multiple of 512 bytes; 1 MB holds three
 * months at one record a minute) and Knurl fills it in place (store.c), oldest first; when it
 * is full, logging stops. 8-byte records, little-endian:
 *   tag   0xB0 start (Knurl booted), 0xB1 sample (every 60 s), 0xB2 off (code = LOG_OFF_*)
 *   flags bit 0 charging, 1 USB power, 2 playing, 3 below the shutoff voltage; for 0xB2 the code
 *   mV    smoothed battery voltage
 *   s     seconds since Knurl started (u32)
 * The unwritten sector is kept in RAM and saved every 10 records and before any restart, disk
 * mode or power-off. tools/battery_fit.py turns a full discharge into the curve below. */
#define LOG_EVERY_MS 60000u
#define LOG_SAVE_EVERY 10
static fat_file log_file;
static int log_ok;
static uint32_t log_sector, log_rec, log_unsaved, next_log_ms;
static uint8_t log_buf[512] __attribute__((aligned(4)));

static void log_add(int tag, int flags, uint32_t now_ms)
{
    if (!log_ok) return;
    uint8_t *r = log_buf + log_rec * 8;
    int mv = avg_mv < 0 ? 0 : avg_mv;
    uint32_t s = now_ms / 1000;
    r[0] = (uint8_t)tag; r[1] = (uint8_t)flags; r[2] = (uint8_t)mv; r[3] = (uint8_t)(mv >> 8);
    r[4] = (uint8_t)s; r[5] = (uint8_t)(s >> 8); r[6] = (uint8_t)(s >> 16); r[7] = (uint8_t)(s >> 24);
    log_unsaved++;
    if (++log_rec == 64) {                       /* sector full: save it, start the next */
        power_log_flush();
        log_sector++;
        log_rec = 0;
        for (int i = 0; i < 512; i++) log_buf[i] = 0;
        if (log_sector * 512 >= log_file.size) log_ok = 0;
    } else if (log_unsaved >= LOG_SAVE_EVERY) {
        power_log_flush();
    }
}

void power_log_flush(void)
{
    if (!log_ok || !log_unsaved) return;
    if (store_write(&log_file, log_sector * 512, log_buf, 512) || ata_flush()) { log_ok = 0; return; }
    log_unsaved = 0;
}

static int log_flags(void)
{
    return (platform_charging() ? 1 : 0) | (power_usb() ? 2 : 0) | (audio_playing() ? 4 : 0) |
           (avg_mv >= 0 && avg_mv < SHUTOFF_MV ? 8 : 0);
}

/* Find the end of what's written (records fill the file in order: binary search on sectors). */
void power_log_start(void)
{
    if (store_open("/.knurl/battery.log", &log_file, 512)) return;
    uint32_t lo = 0, hi = log_file.size / 512;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (store_read(&log_file, mid * 512, log_buf, 512)) return;
        if (log_buf[0]) lo = mid + 1; else hi = mid;
    }
    log_sector = lo;
    log_rec = 0;
    for (int i = 0; i < 512; i++) log_buf[i] = 0;
    if (lo > 0) {                                /* continue in the last used sector if it has room */
        if (store_read(&log_file, (lo - 1) * 512, log_buf, 512)) return;
        uint32_t r = 0;
        while (r < 64 && log_buf[r * 8]) r++;
        if (r < 64) { log_sector = lo - 1; log_rec = r; }
        else for (int i = 0; i < 512; i++) log_buf[i] = 0;
    }
    if (log_sector * 512 >= log_file.size) return;   /* full */
    log_ok = 1;
    log_add(0xB0, log_flags(), 0);
    power_log_flush();
}

static void log_sample(uint32_t now_ms)
{
    if (!log_ok || (int32_t)(now_ms - next_log_ms) < 0) return;
    next_log_ms = now_ms + LOG_EVERY_MS;
    log_add(0xB1, log_flags(), now_ms);
}

void power_log_event(int code)
{
    log_add(0xB2, code, platform_now_ms());
    power_log_flush();
}

static int read_mv(void)
{
    uint8_t d[2];
    pcf_write(PCF_ADCC2, (ADC_BATTERY << 1) | 1);
    if (i2c_read(PCF_ADDR, PCF_ADCS1, d, 2)) return -1;
    int raw = (d[0] << 2) | (d[1] & 3);
    return raw * 6000 >> 10;
}

int platform_charging(void)  { return !(GPIOB_INPUT_VAL & 0x01); }
int power_usb(void)          { return (GPIOL_INPUT_VAL & USB_BIT) != 0; }
static int on_power(void)    { return !(GPIOL_INPUT_VAL & MAIN_BIT) || power_usb(); }

void power_service(uint32_t now_ms)
{
    if (avg_mv >= 0 && (int32_t)(now_ms - next_read_ms) < 0) return;
    next_read_ms = now_ms + 2000;
    int mv = read_mv();
    if (mv < 2000 || mv > 5000) return;                  /* not a believable reading */
    avg_mv = avg_mv < 0 ? mv : (avg_mv * 7 + mv + 4) / 8;
    low_count = (mv < SHUTOFF_MV && !on_power()) ? low_count + 1 : 0;
    log_sample(now_ms);
}

int power_low_battery(void) { return low_count >= 3; }   /* three readings in a row: 6 s */

int platform_battery(void)
{
    if (avg_mv < 0) return -1;
    if (avg_mv <= curve[0]) return 0;
    for (int i = 1; i < 11; i++)
        if (avg_mv < curve[i])
            return (i - 1) * 10 + (avg_mv - curve[i - 1]) * 10 / (curve[i] - curve[i - 1]);
    return 100;
}

int platform_battery_mv(void) { return avg_mv; }

/* ---- off ---------------------------------------------------------------------------- */

/* Deep sleep: the power chip cuts everything but the clock; a button (or a charger) cold
 * boots the iPod again, through the bootloader, back into this firmware. Like Rockbox, the
 * top 80 KB of IRAM is cleared first: Apple's firmware keeps a boot-failure flag there. */
void power_off(void)
{
    audio_shutdown();
    delay_us(50000);                                     /* let the COP finish and sleep */
    CPU_INT_DIS = ~0u;
    CPU_HI_INT_DIS = ~0u;
    __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0xc0\n msr cpsr_c, r0" ::: "r0", "memory");
    for (volatile uint32_t *p = (volatile uint32_t *)0x4000c000; p < (volatile uint32_t *)0x40020000; p++)
        *p = 0;
    pcf_write(PCF_OOCC1, GOSTDBY | CHGWAK | EXTONWAK);
    delay_us(2000000);
    DEV_RS |= DEV_SYSTEM;                                /* still awake (charger?): reboot */
    for (;;) { }
}
