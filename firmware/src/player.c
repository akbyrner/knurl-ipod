#include "player.h"
#include "platform.h"
#include "shared.h"
#include "mp3/mp3dec.h"
#include "mp3/mp3info.h"

#define IN_BYTES      (16 * 1024)
#define FIFO          4096                    /* stereo frames between decoder and caller */
#define DECODER_DELAY 529                     /* samples every MPEG-1 Layer III decoder adds */

/* All of this belongs to the decoder core. */
static COP_DATA mp3dec_t dec;
static COP_DATA uint8_t in[IN_BYTES];
static COP_DATA int16_t pcm[MP3_FRAME_SAMPLES * 2];
static COP_DATA int16_t fifo[FIFO * 2];
static COP_DATA struct {
    player_source src;
    int in_pos, in_len, src_end, open, stalled;
    int f_rd, f_n;                            /* fifo read index and fill, in frames */
    int skip, holdback, first_frame;
    uint32_t id3_skip;                        /* bytes of ID3v2 tag still to throw away */
    uint32_t dec_us, dec_samples;
    const char *err;
} P;

/* Top up the input buffer. 1 = got bytes, 0 = end of track, -1 = nothing yet. */
static int refill(void)
{
    if (P.src_end) return 0;
    if (P.in_pos > 0) {                       /* keep the unread tail */
        int rest = P.in_len - P.in_pos;
        for (int i = 0; i < rest; i++) in[i] = in[P.in_pos + i];
        P.in_len = rest;
        P.in_pos = 0;
    }
    if (P.in_len == IN_BYTES) return 1;
    int n = P.src(in + P.in_len, IN_BYTES - P.in_len);
    if (n == 0) { P.src_end = 1; return 0; }
    if (n < 0) return -1;
    P.in_len += n;
    if (P.id3_skip) {                         /* still inside a big ID3v2 tag */
        uint32_t k = P.id3_skip < (uint32_t)P.in_len ? P.id3_skip : (uint32_t)P.in_len;
        P.in_pos = (int)k;
        P.id3_skip -= k;
    }
    return 1;
}

void player_open(player_source src, int resume_holdback)
{
    P.src = src;
    P.in_pos = P.in_len = P.src_end = P.stalled = 0;
    P.f_rd = P.f_n = 0;
    P.skip = P.holdback = 0;
    P.first_frame = 1;
    P.id3_skip = 0;
    if (resume_holdback >= 0) {               /* mid-track (a seek): no tag, no Info frame here */
        P.first_frame = 0;
        P.holdback = resume_holdback > FIFO - MP3_FRAME_SAMPLES ? FIFO - MP3_FRAME_SAMPLES : resume_holdback;
    }
    P.dec_us = P.dec_samples = 0;
    P.err = "";
    dec.saturated = 0;
    mp3_init(&dec);
    P.open = 1;
}

void player_close(void) { P.open = 0; P.f_n = 0; P.holdback = 0; }
int  player_is_open(void) { return P.open || P.f_n > 0; }
int  player_stalled(void) { return P.stalled; }

/* Decode one more frame into the fifo: 1 = done, 0 = end of track, -1 = waiting for data. */
static int decode_more(void)
{
    for (;;) {
        /* Never decode from less than a whole frame (2304 bytes is the largest), or a frame cut
         * short would send the sync search off after a false header. */
        while (P.in_len - P.in_pos < 2304 && !P.src_end) {
            int r = refill();
            if (r < 0) return -1;
            if (r == 0) break;
        }
        if (P.first_frame == 1 && P.in_len - P.in_pos >= 10) {
            P.first_frame = 2;                /* look for an ID3v2 tag once, at the very start */
            uint32_t sz = mp3_id3v2_size(in + P.in_pos, P.in_len - P.in_pos);
            if (sz) {
                uint32_t have = (uint32_t)(P.in_len - P.in_pos);
                if (sz <= have) P.in_pos += (int)sz;
                else { P.id3_skip = sz - have; P.in_pos = P.in_len; }
                continue;
            }
        }
        if (P.id3_skip) { if (P.src_end) return 0; continue; }
        if (P.in_len - P.in_pos < 4) return 0;
        mp3_info_t info;
        uint32_t t0 = platform_us();
        int s = mp3_decode_frame(&dec, in + P.in_pos, P.in_len - P.in_pos, pcm, &info);
        P.dec_us += platform_us() - t0;
        if (!info.frame_bytes) {              /* a frame starts here but isn't complete */
            if (P.src_end) return 0;
            if (P.in_pos == 0 && P.in_len == IN_BYTES) P.in_pos = P.in_len;   /* garbage: drop it */
            if (refill() < 0) return -1;
            continue;
        }
        if (P.first_frame && s) {
            P.first_frame = 0;
            mp3_xing_t x;
            if (mp3_parse_xing(in + P.in_pos + info.frame_offset, info.frame_bytes - info.frame_offset, &x)) {
                P.in_pos += info.frame_bytes;     /* an Info frame: no audio of its own */
                if (x.delay >= 0) {
                    P.skip = x.delay + DECODER_DELAY;
                    P.holdback = x.padding - DECODER_DELAY;
                    if (P.holdback < 0) P.holdback = 0;
                    if (P.holdback > FIFO - MP3_FRAME_SAMPLES) P.holdback = FIFO - MP3_FRAME_SAMPLES;
                }
                continue;
            }
        }
        P.in_pos += info.frame_bytes;
        if (!s) continue;
        if (info.hz != AUDIO_RATE) { P.err = "not 44.1 kHz"; return 0; }
        P.dec_samples += (uint32_t)s;
        int i = 0;
        if (P.skip) { int k = P.skip < s ? P.skip : s; P.skip -= k; i = k; }
        for (; i < s; i++) {
            int w = (P.f_rd + P.f_n) % FIFO;
            if (info.channels == 2) { fifo[2 * w] = pcm[2 * i]; fifo[2 * w + 1] = pcm[2 * i + 1]; }
            else fifo[2 * w] = fifo[2 * w + 1] = pcm[i];
            P.f_n++;
        }
        return 1;
    }
}

int player_read(int16_t *lr, int frames)
{
    int done = 0;
    P.stalled = 0;
    while (done < frames) {
        /* Keep `holdback` frames back while the track is still decoding: at its end they are
         * LAME's padding and get dropped. */
        while (P.open && P.f_n - P.holdback < frames - done && P.f_n + MP3_FRAME_SAMPLES <= FIFO) {
            int r = decode_more();
            if (r < 0) { P.stalled = 1; break; }
            if (r == 0) {
                P.open = 0;
                P.f_n -= P.holdback < P.f_n ? P.holdback : P.f_n;
                P.holdback = 0;
            }
        }
        int avail = P.f_n - P.holdback;
        if (avail <= 0) break;
        int n = frames - done < avail ? frames - done : avail;
        for (int k = 0; k < n; k++) {
            lr[2 * (done + k)] = fifo[2 * P.f_rd];
            lr[2 * (done + k) + 1] = fifo[2 * P.f_rd + 1];
            P.f_rd = (P.f_rd + 1) % FIFO;
        }
        P.f_n -= n;
        done += n;
        if (P.stalled) break;
    }
    return done;
}

const char *player_error(void) { return P.err ? P.err : ""; }
uint32_t    player_decode_us(void) { return P.dec_us; }
uint32_t    player_decoded_ms(void) { return (uint32_t)((uint64_t)P.dec_samples * 1000 / AUDIO_RATE); }
uint32_t    player_saturated(void) { return dec.saturated; }
const uint32_t *player_profile(void) { return dec.prof; }
