/* Host test for firmware/src/player.c: play a file through the player and write what it
 * returns as raw s16le stereo, for comparing with ffmpeg's gapless output.
 *   player_test IN.mp3 OUT.raw [stall]   stall: the source runs dry at random (as the FIFO
 *                                        can on the iPod) - the output must not change */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "player.h"

static FILE *fp;
static int stall;
static unsigned rng = 12345;
uint32_t platform_us(void) { return 0; }

static int file_source(uint8_t *buf, int max)
{
    if (stall) {
        rng = rng * 1103515245u + 12345u;
        if ((rng >> 16) % 3 == 0) return -1;            /* no data yet */
        max = 1 + (int)((rng >> 8) % (unsigned)max);    /* and odd-sized pieces */
    }
    size_t r = fread(buf, 1, (size_t)max, fp);
    return (int)r;                                      /* 0 = end */
}

int main(int argc, char **argv)
{
    if (argc < 3 || !(fp = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: player_test in.mp3 out.raw [stall]\n"); return 1; }
    stall = argc > 3 && !strcmp(argv[3], "stall");
    player_open(file_source, -1);
    FILE *o = fopen(argv[2], "wb");
    static int16_t buf[1024 * 2];
    long total = 0, stalls = 0;
    while (player_is_open()) {
        int n = player_read(buf, 1024);
        fwrite(buf, 4, (size_t)n, o);
        total += n;
        if (player_stalled()) stalls++;
        else if (n < 1024) break;
    }
    fclose(o);
    printf("%ld frames, %ld stalls, error '%s', saturated %u\n", total, stalls, player_error(), player_saturated());
    return 0;
}
