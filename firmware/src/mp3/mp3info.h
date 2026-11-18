/*
 * The Info / Xing frame LAME writes at the start of every file: total frames, a 100-entry
 * seek table, and the encoder delay and padding (for gapless playback). Pure functions,
 * used by the decoder core at a track's start and by the CPU to find where to seek.
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t frames;          /* audio frames after this one, 0 = unknown */
    uint32_t bytes;           /* audio bytes, 0 = unknown */
    int      has_toc;
    uint8_t  toc[100];        /* toc[i]: where i% of the time is, in 256ths of the audio bytes */
    int      delay, padding;  /* samples; -1 = unknown */
} mp3_xing_t;

/* frame: a complete MPEG-1 Layer III frame (header included). 1 if it is an Info/Xing frame. */
int mp3_parse_xing(const uint8_t *frame, int len, mp3_xing_t *x);

/* Where to start reading a file to play from ms in. head: bytes of the file read from
 * head_pos (where the ID3v2 tag ends), enough to hold the first frame. Uses LAME's seek
 * table when there is one, else assumes a constant bitrate. *hold: the padding to drop at
 * the end (the decoder won't see the Info frame when it starts mid-file). dur_ms: the
 * duration to assume if the file doesn't say. */
uint32_t mp3_seek_offset(const uint8_t *head, int n, uint32_t head_pos, uint32_t file_size,
                         uint32_t ms, uint32_t dur_ms, int *hold);

/* ID3v2 tag size at p (header + body + footer), or 0 if there is none. */
uint32_t mp3_id3v2_size(const uint8_t *p, int len);
