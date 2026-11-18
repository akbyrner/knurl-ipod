/*
 * What the app needs from the hardware. Two implementations:
 *   platform/device: the iPod (BCM2722 screen, click wheel, timers)
 *   platform/sim:    the Linux simulator (frames -> images, scripted input)
 * Everything above this line (gfx, ui, app) is shared, so screens are designed and
 * checked in the simulator before they ever run on the iPod.
 */
#pragma once
#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 240

/* The single RGB565 framebuffer every screen draws into. */
extern uint16_t fb[SCREEN_W * SCREEN_H];

enum {
    BTN_MENU   = 1 << 0,
    BTN_PLAY   = 1 << 1,
    BTN_PREV   = 1 << 2,
    BTN_NEXT   = 1 << 3,
    BTN_SELECT = 1 << 4,
};

typedef struct {
    int      wheel;      /* detents turned since last poll: + clockwise, - counter-clockwise */
    unsigned pressed;    /* buttons that went down since last poll (BTN_*) */
    int      hold;       /* HOLD switch is on */
} input_t;

void     platform_init(void);
/* Show the rectangle of fb on screen (x and w are rounded out to even pixels). */
void     platform_present(int x, int y, int w, int h);
uint32_t platform_now_ms(void);
uint32_t platform_us(void);              /* free-running microseconds (for measuring) */
/* Fill *in with what happened since the last call. */
void     platform_poll(input_t *in);
/* Wait a little (about one frame) for something to happen. */
void     platform_wait(void);
/* Leave the firmware (device: reboot to Apple's firmware; sim: end of script). */
void     platform_exit(void);
/* The disk, in 512-byte sectors. 0 = ok. */
int      platform_disk_read(uint64_t lba, uint32_t count, void *buf);
const char *platform_disk_name(void);
uint64_t platform_disk_sectors(void);
/* Start-up progress: one line per step, shown before the step runs (device: on screen). */
void     boot_log(const char *line);
/* Audio out: 44.1 kHz, 16-bit stereo, L/R interleaved, written a chunk at a time into a
 * ring of AUDIO_CHUNKS chunks. If the ring runs dry the output plays silence (a gap)
 * and carries on as soon as a chunk is queued again. */
#define AUDIO_RATE         44100
#define AUDIO_CHUNK_FRAMES 1024           /* 23.2 ms */
#define AUDIO_CHUNKS       16             /* 372 ms of buffering */
int      audio_init(void);                /* 0 = codec set up and output running */
int      audio_free_chunks(void);
int16_t *audio_chunk(void);               /* the next free chunk to fill, or NULL if full */
void     audio_commit(void);              /* queue the chunk just filled */
int      audio_pending(void);             /* chunks queued, including the one playing */
void     audio_active(int on);            /* while on, running dry counts as an underrun */
void     audio_pause(int on);             /* instant: holds its place in the ring */
void     audio_flush(void);               /* drop what's queued (new track, skip) */
uint32_t audio_underruns(void);
void     audio_set_volume(int db);        /* -90 (mute) .. +6 dB */
int      audio_volume(void);
void     audio_shutdown(void);
/* Wake the decoder core (device: the COP; simulator: runs engine_step() right here). */
void     platform_cop_kick(void);
/* Battery level 0..100, or -1 if unknown (device: from the voltage, smoothed). */
int      platform_battery(void);
int      platform_battery_mv(void);       /* smoothed battery voltage, -1 if unknown */
int      platform_charging(void);         /* the charger is charging the battery */
int      platform_cpu_busy(void);         /* % of the last second the CPU was awake */
