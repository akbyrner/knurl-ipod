/*
 * One MP3 track as a stream of 44.1 kHz stereo frames, decoded from bytes handed over by a
 * source function (on the iPod: the compressed FIFO the CPU fills from disk). Skips the
 * ID3v2 tag (cover art and all) and trims LAME's encoder delay and padding (from the
 * Info/Xing frame's LAME tag) so album tracks join without a gap.
 */
#pragma once
#include <stdint.h>

/* Copy up to max bytes of the track into buf: > 0 bytes copied, 0 = end of the track,
 * -1 = no data yet (try again later). */
typedef int (*player_source)(uint8_t *buf, int max);

/* resume_holdback: -1 = start of a track (skip its tag, read its gapless info); >= 0 =
 * resuming mid-track after a seek, dropping that many frames at the end (LAME's padding). */
void        player_open(player_source src, int resume_holdback);
void        player_close(void);
int         player_is_open(void);                  /* still has frames to give */
/* Up to `frames` stereo frames (L,R interleaved) into lr. Fewer if the track ended or the
 * source ran dry: player_stalled() says which. */
int         player_read(int16_t *lr, int frames);
int         player_stalled(void);
const char *player_error(void);                    /* why the track stopped early */
/* Decode cost since open: microseconds in the decoder, and the audio that produced. */
uint32_t    player_decode_us(void);
uint32_t    player_decoded_ms(void);
uint32_t    player_saturated(void);
const uint32_t *player_profile(void);              /* us per stage, see mp3dec.h */
