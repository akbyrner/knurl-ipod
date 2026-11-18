/*
 * The playback engine, split across the iPod's two cores:
 *
 *   CPU (UI core)   reads the queue's files from disk into a compressed FIFO, marking where
 *                   each track starts (only the CPU ever touches the disk), and reads back
 *                   what's playing for the screen.
 *   COP (decoder)   engine_step(): decodes the FIFO (src/player.c, src/mp3) into the audio
 *                   ring, track after track with no gap, recording where each track starts
 *                   in the output.
 *
 * They share only the SHARED block below (uncached). The CPU restarts playback with a
 * stop / acknowledge / go handshake, so it never resets anything the COP is using.
 * The simulator has one core: engine_step() runs from platform_cop_kick().
 */
#pragma once
#include <stdint.h>

#define ENGINE_FIFO_BYTES (512 * 1024)      /* ~17 s of V0 MP3 */

/* ---- CPU side ---------------------------------------------------------------------- */
void     engine_init(void);
void     engine_restart(void);                 /* stop the decoder, drop all queued audio and data */
void     engine_go(void);                      /* after a restart and the first track mark */
int      engine_write_space(uint8_t **p);      /* contiguous free bytes at *p */
void     engine_wrote(int n);
int      engine_marks_free(void);              /* room for another track mark */
/* The bytes that follow are queue entry q, from start_ms into it. hold: -1 = the file's
 * start (its tag and gapless info are read from the data); >= 0 = resuming after a seek,
 * with LAME's padding (frames to drop at the end) given here. */
void     engine_mark_track(int q, int hold, uint32_t start_ms);
void     engine_end(void);                     /* no more tracks after what's been written */
uint32_t engine_heard_frames(void);            /* output frames that have reached the speaker */
int      engine_track_switch(uint32_t *frame, int *q, uint32_t *start_ms);   /* pop: entry q (from start_ms) starts at output frame */
int      engine_finished(void);                /* everything decoded and played */
uint32_t engine_buffered_bytes(void);

typedef struct {
    uint32_t dec_us, dec_ms, saturated, prof[4];
    char     err[24];
} engine_stats_t;
void     engine_stats(engine_stats_t *s);

/* ---- decoder side --------------------------------------------------------------------- */
int      engine_step(void);                    /* decode while there is room; 0 = nothing to do */
