/*
 * Fixed-point MP3 decoder: MPEG-1 Layer III (32 / 44.1 / 48 kHz, mono or stereo, joint
 * stereo with M/S and intensity), which is everything the server's LAME V0 encode produces.
 * Integer-only (32x32->64 multiplies), so it runs on the iPod's ARM7TDMI without an FPU.
 *
 * Structure ported from minimp3 (github.com/lieff/minimp3, CC0 public domain): the same
 * steps and fast IMDCT / DCT / polyphase algorithms, with every float turned into fixed
 * point: spectral values and the filterbank carry MP3_FRAC fractional bits.
 * Verified sample-by-sample against ffmpeg's decoder on the build machine (tools/mp3check.sh).
 */
#pragma once
#include <stdint.h>

#define MP3_FRAME_SAMPLES 1152         /* per channel, MPEG-1 Layer III */
#define MP3_FRAC 24

typedef struct {
    int frame_bytes;                   /* bytes consumed, including skipped junk */
    int frame_offset;                  /* where the frame started within the input */
    int channels, hz, bitrate_kbps;
} mp3_info_t;

typedef struct {
    const uint8_t *sfbtab;
    uint16_t part_23_length, big_values, scalefac_compress;
    uint8_t global_gain, block_type, mixed_block_flag, n_long_sfb, n_short_sfb;
    uint8_t table_select[3], region_count[3], subblock_gain[3];
    uint8_t preflag, scalefac_scale, count1_table, scfsi;
} mp3_granule_t;

typedef struct {
    /* state carried between frames */
    int32_t mdct_overlap[2][9 * 32];
    int32_t qmf_state[15 * 2 * 32];
    int     reserv, free_format_bytes;
    uint8_t header[4], reserv_buf[511];
    /* per-frame scratch, kept here rather than on the stack (about 16 KB) */
    struct { const uint8_t *buf; int pos, limit; } bs;
    uint8_t maindata[511 + 2304];
    mp3_granule_t gr_info[4];
    int32_t grbuf[2][576];
    int32_t syn[18 + 15][2 * 32];
    int32_t scf_mul[40];
    int8_t  scf_sh[40];
    uint8_t ist_pos[2][39];
    uint32_t saturated;                /* values that overflowed the fixed-point range (should stay 0) */
    uint32_t prof[4];                  /* us in: bitstream+Huffman, stereo+IMDCT, DCT, synthesis */
} mp3dec_t;

void mp3_init(mp3dec_t *d);
/* Decode the first frame found in mp3[0..bytes). Returns samples per channel written to pcm
 * (interleaved int16, channels from info), or 0 if no frame was decoded (then skip
 * info->frame_bytes and feed more data). Frames other than MPEG-1 Layer III are skipped. */
int  mp3_decode_frame(mp3dec_t *d, const uint8_t *mp3, int bytes, int16_t *pcm, mp3_info_t *info);
