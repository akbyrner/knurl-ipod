/* Host test for seeking: where mp3_seek_offset() says to start for a time, and what the
 * player (in resume mode) decodes from there, written as raw s16le stereo.
 *   seek_test IN.mp3 MS OUT.raw */
#include <stdio.h>
#include <stdlib.h>
#include "player.h"
#include "mp3/mp3info.h"

static FILE *fp;
uint32_t platform_us(void) { return 0; }
static int src(uint8_t *buf, int max) { return (int)fread(buf, 1, (size_t)max, fp); }

int main(int argc, char **argv)
{
    if (argc < 4 || !(fp = fopen(argv[1], "rb"))) return 2;
    uint32_t ms = (uint32_t)atol(argv[2]);
    fseek(fp, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(fp);
    static uint8_t head[2048];
    fseek(fp, 0, SEEK_SET);
    int n = (int)fread(head, 1, 10, fp);
    uint32_t start = mp3_id3v2_size(head, n);
    fseek(fp, start, SEEK_SET);
    n = (int)fread(head, 1, sizeof head, fp);
    int hold;
    uint32_t at = mp3_seek_offset(head, n, start, size, ms, 0, &hold);
    fseek(fp, at, SEEK_SET);
    player_open(src, hold);
    FILE *o = fopen(argv[3], "wb");
    static int16_t buf[2048];
    long total = 0;
    int k;
    while ((k = player_read(buf, 1024)) > 0) { fwrite(buf, 4, (size_t)k, o); total += k; if (k < 1024) break; }
    fclose(o);
    printf("offset %u of %u, hold %d, %ld frames\n", at, size, hold, total);
    return 0;
}
