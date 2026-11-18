/* Host test for firmware/src/mp3: decode an MP3 file to raw interleaved s16le.
 *   mp3dec_test IN.mp3 OUT.raw      prints channels, rate, frames, saturations, decode speed */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "mp3/mp3dec.h"

static mp3dec_t dec;
uint32_t platform_us(void) { return 0; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.mp3 out.raw\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);
    long pos = 0;
    if (n > 10 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {   /* skip ID3v2 (with any cover art) */
        long sz = ((buf[6] & 127) << 21) | ((buf[7] & 127) << 14) | ((buf[8] & 127) << 7) | (buf[9] & 127);
        pos = 10 + sz + ((buf[5] & 0x10) ? 10 : 0);
    }
    FILE *o = fopen(argv[2], "wb");
    mp3_init(&dec);
    static int16_t pcm[MP3_FRAME_SAMPLES * 2];
    long frames = 0, samples = 0;
    mp3_info_t info = {0};
    int ch = 0, hz = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (pos < n) {
        int s = mp3_decode_frame(&dec, buf + pos, (int)(n - pos), pcm, &info);
        if (!info.frame_bytes) break;
        pos += info.frame_bytes;
        if (s) {
            fwrite(pcm, 2, (size_t)s * info.channels, o);
            frames++; samples += s; ch = info.channels; hz = info.hz;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fclose(o);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d ch %d Hz, %ld frames, %ld samples, saturated %u, %.1fx realtime\n",
           ch, hz, frames, samples, dec.saturated, hz ? samples / (double)hz / sec : 0);
    return 0;
}
