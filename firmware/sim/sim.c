#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
/*
 * Linux simulator: runs the exact app code against a scripted click wheel and
 * writes screenshots. Also counts how many pixels each step uploads to the screen,
 * which is the main cost on the iPod.
 *
 *   knurl-sim [--disk IMAGE] [--wav FILE] [--boot-shot NAME] OUTDIR token...
 *     NAME: also write the boot screen as OUTDIR/NAME.ppm
 *     IMAGE: a raw disk with a FAT32 volume; FILE: everything the app queued for audio
 *     w+N / w-N            turn the wheel N detents
 *     sel menu play next prev
 *     t+MS                 let MS milliseconds pass (playback, animations)
 *     shot:NAME            write OUTDIR/NAME.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "platform.h"
#include "app.h"
#include "engine.h"
#include "splash.h"

uint16_t fb[SCREEN_W * SCREEN_H];
static uint32_t now = 1000;
static long uploaded, presents;

void     platform_init(void) {}
void     platform_present(int x, int y, int w, int h) { (void)x; (void)y; uploaded += (long)w * h; presents++; }
uint32_t platform_now_ms(void) { return now; }
uint32_t platform_us(void) { return 0; }
void     platform_poll(input_t *in) { memset(in, 0, sizeof *in); }
void     platform_wait(void) {}
void     platform_exit(void) {}
int      platform_battery(void) { return 72; }
int      platform_battery_mv(void) { return 3900; }
int      platform_charging(void) { return 0; }
int      platform_cpu_busy(void) { return 0; }
/* settings.bin lives in memory: there, empty at the start of each run */
static uint8_t settings_bin[512];
int      platform_settings_load(void *buf) { memcpy(buf, settings_bin, 512); return 0; }
int      platform_settings_save(const void *buf) { memcpy(settings_bin, buf, 512); return 0; }
void     platform_set_power(uint32_t light_ms, uint32_t idle_ms) { (void)light_ms; (void)idle_ms; }

void boot_log(const char *line) { fprintf(stderr, "boot: %s\n", line); splash_log(line); }

static int disk_fd = -1;
static char disk_label[64] = "no disk image";
int platform_disk_read(uint64_t lba, uint32_t count, void *buf)
{
    if (disk_fd < 0) return -1;
    return pread(disk_fd, buf, (size_t)count * 512, (off_t)(lba * 512)) == (ssize_t)count * 512 ? 0 : -1;
}
const char *platform_disk_name(void) { return disk_label; }
uint64_t platform_disk_sectors(void) { return disk_fd < 0 ? 0 : (uint64_t)lseek(disk_fd, 0, SEEK_END) / 512; }

/* ---- audio: chunks are "played" in step with simulated time ---- */
static int16_t a_ring[AUDIO_CHUNKS][AUDIO_CHUNK_FRAMES * 2];
static uint32_t a_queued, a_played, a_underruns, a_last_ms = 1000, a_acc;
static int a_active, a_paused, a_volume = -30;
static FILE *wav;
static uint32_t wav_frames;

static void a_advance(void)
{
    a_acc += (now - a_last_ms) * AUDIO_RATE / 1000;
    a_last_ms = now;
    while (a_acc >= AUDIO_CHUNK_FRAMES) {
        if (a_paused) { a_acc = 0; break; }
        if (a_played == a_queued) { if (a_active) a_underruns++; a_acc = 0; break; }
        a_played++;
        a_acc -= AUDIO_CHUNK_FRAMES;
    }
}
int      audio_init(void) { return 0; }
int      audio_free_chunks(void) { a_advance(); return AUDIO_CHUNKS - (int)(a_queued - a_played); }
int16_t *audio_chunk(void) { return audio_free_chunks() > 0 ? a_ring[a_queued % AUDIO_CHUNKS] : NULL; }
void     audio_commit(void)
{
    if (audio_free_chunks() <= 0) return;
    if (wav) { fwrite(a_ring[a_queued % AUDIO_CHUNKS], 4, AUDIO_CHUNK_FRAMES, wav); wav_frames += AUDIO_CHUNK_FRAMES; }
    a_queued++;
}
int      audio_pending(void) { a_advance(); return (int)(a_queued - a_played); }
void     audio_active(int on) { a_active = on; }
void     audio_pause(int on) { a_paused = on; }
void     audio_flush(void) { a_advance(); a_queued = a_played; }
uint32_t audio_underruns(void) { return a_underruns; }
void     audio_set_volume(int db) { a_volume = db; }
int      audio_volume(void) { return a_volume; }
void     audio_shutdown(void) {}
void     platform_cop_kick(void) { while (engine_step()) {} }

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt[4] = { 16, 1 | (2u << 16), AUDIO_RATE, AUDIO_RATE * 4 };
    uint16_t tail[2] = { 4, 16 };
    fwrite(fmt, 4, 4, f); fwrite(tail, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static void shot(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t c = fb[i];
        unsigned char px[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31),
                                (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                (unsigned char)((c & 31) * 255 / 31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

/* Run frames until the animations settle (at most 2 s). */
static int settle(void)
{
    int frames = 0;
    while (frames < 125) {
        now += 16;
        frames++;
        if (!app_step(NULL, now)) break;
    }
    return frames;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--disk")) {
        disk_fd = open(argv[2], O_RDONLY);
        if (disk_fd < 0) { perror(argv[2]); return 2; }
        snprintf(disk_label, sizeof disk_label, "SIM %s", strrchr(argv[2], '/') ? strrchr(argv[2], '/') + 1 : argv[2]);
        argv += 2; argc -= 2;
    }
    if (argc > 2 && !strcmp(argv[1], "--wav")) {
        wav = fopen(argv[2], "wb");
        if (!wav) { perror(argv[2]); return 2; }
        wav_header(wav, 0);
        argv += 2; argc -= 2;
    }
    const char *boot_shot = 0;
    if (argc > 2 && !strcmp(argv[1], "--boot-shot")) { boot_shot = argv[2]; argv += 2; argc -= 2; }
    if (argc < 2) { fprintf(stderr, "usage: knurl-sim [--disk IMAGE] [--wav FILE] [--boot-shot NAME] OUTDIR token...\n"); return 2; }
    engine_init();
    app_init(now);
    if (boot_shot) shot(argv[1], boot_shot);       /* the boot screen, before the first frame covers it */
    app_step(NULL, now);
    printf("%-22s %8s %8s %6s\n", "step", "presents", "pixels", "frames");
    printf("%-22s %8ld %8ld %6s\n", "(first frame)", presents, uploaded, "-");
    for (int i = 2; i < argc; i++) {
        const char *t = argv[i];
        if (!strncmp(t, "shot:", 5)) { shot(argv[1], t + 5); continue; }
        input_t in = {0};
        presents = uploaded = 0;
        int frames = 0;
        if (t[0] == 'w') in.wheel = atoi(t + 1);
        else if (!strcmp(t, "sel"))  in.pressed = BTN_SELECT;
        else if (!strcmp(t, "menu")) in.pressed = BTN_MENU;
        else if (!strcmp(t, "play")) in.pressed = BTN_PLAY;
        else if (!strcmp(t, "next")) in.pressed = BTN_NEXT;
        else if (!strcmp(t, "prev")) in.pressed = BTN_PREV;
        else if (t[0] == 't') {
            int ms = atoi(t + 1);
            for (int e = 0; e < ms; e += 16) { now += 16; app_step(NULL, now); frames++; }
            printf("%-22s %8ld %8ld %6d\n", t, presents, uploaded, frames);
            continue;
        } else { fprintf(stderr, "unknown token %s\n", t); return 2; }
        app_step(&in, now);
        frames = settle();
        printf("%-22s %8ld %8ld %6d\n", t, presents, uploaded, frames);
    }
    if (wav) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, wav_frames);
        fclose(wav);
        printf("audio: %u frames queued, %u underruns\n", wav_frames, a_underruns);
    }
    return 0;
}
