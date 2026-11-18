#include "mp3info.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int mp3_parse_xing(const uint8_t *f, int len, mp3_xing_t *x)
{
    if (len < 4 || f[0] != 0xff) return 0;
    int mono = (f[3] & 0xC0) == 0xC0;
    int p = 4 + (mono ? 17 : 32);                 /* after the MPEG-1 side info */
    if (len < p + 8) return 0;
    if (!((f[p] == 'X' && f[p + 1] == 'i' && f[p + 2] == 'n' && f[p + 3] == 'g') ||
          (f[p] == 'I' && f[p + 1] == 'n' && f[p + 2] == 'f' && f[p + 3] == 'o'))) return 0;
    uint32_t flags = be32(f + p + 4);
    p += 8;
    x->frames = x->bytes = 0;
    x->has_toc = 0;
    x->delay = x->padding = -1;
    if (flags & 1) { if (p + 4 > len) return 1; x->frames = be32(f + p); p += 4; }
    if (flags & 2) { if (p + 4 > len) return 1; x->bytes = be32(f + p); p += 4; }
    if (flags & 4) {
        if (p + 100 > len) return 1;
        for (int i = 0; i < 100; i++) x->toc[i] = f[p + i];
        x->has_toc = 1;
        p += 100;
    }
    if (flags & 8) p += 4;
    /* The LAME tag starts with the encoder's name: "LAME3.100", or "Lavc60.31" when ffmpeg
     * drove LAME; delay and padding are 12 bits each at offset 21. */
    int named = p + 24 <= len;
    for (int i = 0; named && i < 4; i++) named = f[p + i] >= 0x20 && f[p + i] < 0x7f;
    if (named) {
        x->delay = (f[p + 21] << 4) | (f[p + 22] >> 4);
        x->padding = ((f[p + 22] & 15) << 8) | f[p + 23];
    }
    return 1;
}

uint32_t mp3_seek_offset(const uint8_t *head, int n, uint32_t head_pos, uint32_t file_size,
                         uint32_t ms, uint32_t dur, int *hold)
{
    *hold = 0;
    int i = 0;
    while (i + 4 <= n && !(head[i] == 0xff && (head[i + 1] & 0xE0) == 0xE0)) i++;
    uint32_t audio = head_pos + (uint32_t)i;
    uint32_t len = file_size > audio + 128 ? file_size - audio - 128 : 0;   /* (ID3v1 at the end) */
    mp3_xing_t x;
    if (i + 4 <= n && mp3_parse_xing(head + i, n - i, &x)) {
        if (x.bytes && x.bytes <= len + 128) len = x.bytes;
        if (x.padding >= 0) *hold = x.padding > 529 ? x.padding - 529 : 0;
        if (x.frames) dur = (uint32_t)((uint64_t)x.frames * 1152 * 1000 / 44100);
        if (x.has_toc && dur) {
            uint32_t p = (uint32_t)((uint64_t)ms * 25600 / dur);         /* percent, in 256ths */
            if (p > 99 * 256 + 255) p = 99 * 256 + 255;
            uint32_t k = p >> 8, fr = p & 255;
            uint32_t a = x.toc[k], b = k < 99 ? x.toc[k + 1] : 256;
            uint32_t pos = a * 256 + (b - a) * fr;                        /* 65536ths of len */
            return audio + (uint32_t)((uint64_t)len * pos >> 16);
        }
    }
    if (!dur) return audio;
    if (ms > dur) ms = dur;
    return audio + (uint32_t)((uint64_t)len * ms / dur);
}

uint32_t mp3_id3v2_size(const uint8_t *h, int len)
{
    if (len < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
    uint32_t sz = ((uint32_t)(h[6] & 127) << 21) | ((uint32_t)(h[7] & 127) << 14) |
                  ((uint32_t)(h[8] & 127) << 7) | (h[9] & 127);
    return sz + 10 + ((h[5] & 0x10) ? 10 : 0);
}
