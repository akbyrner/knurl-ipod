/* Device-internal interfaces shared between the files in device/ (not part of platform.h). */
#pragma once
#include <stdint.h>

/* i2c.c: the PP5022's I2C controller (codec 0x1a, PCF50605 power chip 0x08). CPU only. */
void i2c_init(void);
int  i2c_write(int dev, const uint8_t *data, int len);          /* len 1..4; 0 = ok */
int  i2c_read(int dev, int reg, uint8_t *data, int len);        /* len 1..4; 0 = ok */

/* power.c */
void power_init(void);                    /* supply voltages; before audio_init */
void power_service(uint32_t now_ms);      /* battery reading every 2 s; call often */
int  power_usb(void);                     /* USB power on the dock connector */
int  power_low_battery(void);             /* below the shutoff voltage, not charging */
void power_off(void) __attribute__((noreturn));

/* usb.c: disk mode. Runs until the computer ejects the disk or the cable is pulled. */
enum { USB_END_EJECTED = 1, USB_END_UNPLUGGED = 2 };
typedef struct {
    uint32_t resets, commands, writes, ahead_hits;
    uint32_t rd_bytes, rd_us, wr_bytes, wr_us;   /* time spent inside READ/WRITE commands */
    int configured, high_speed;
} usb_stats_t;
extern usb_stats_t usb_stats;
int  usb_disk_run(void);
void usb_status_tick(void);               /* device.c: refresh the status line */
int  ata_pio_mode(void);
int  ata_stream_start(uint64_t lba, uint32_t count, int write);
int  ata_stream_xfer(uint32_t *buf, uint32_t n);
int  ata_write(uint64_t lba, uint32_t count, const void *buf);   /* PIO, 0 = ok */
int  ata_flush(void);
int  ata_dma(uint64_t lba, uint32_t n, uint32_t phys, int write);
int  ata_udma_mode(void);             /* -1 = no DMA */

/* audio.c */
int  audio_playing(void);                 /* music is audible (playing, not paused) */

void delay_us(uint32_t us);
