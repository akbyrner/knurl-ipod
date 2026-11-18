#include "engine.h"
#include "platform.h"
#include "player.h"
#include "shared.h"

#define MARKS 16
#define SWITCHES 16

typedef struct {
    /* CPU -> COP */
    uint32_t gen;                    /* bumped to stop the decoder */
    uint32_t go;                     /* = gen: decode */
    uint32_t wr;                     /* bytes written into fifo (monotonic) */
    uint32_t mark_w;                 /* marks written */
    uint32_t mark_pos[MARKS];        /* stream position where ... */
    int32_t  mark_q[MARKS];          /* ... queue entry q begins */
    int32_t  mark_hold[MARKS];       /* -1, or the padding to drop when resuming mid-track */
    uint32_t mark_ms[MARKS];         /* where in the track the bytes start */
    uint32_t ended;                  /* no tracks beyond wr */
    /* COP -> CPU */
    uint32_t gen_ack;
    uint32_t rd;                     /* bytes consumed */
    uint32_t mark_r;                 /* marks consumed */
    uint32_t committed;              /* frames handed to the audio ring since go */
    uint32_t sw_w;
    uint32_t sw_frame[SWITCHES];
    int32_t  sw_q[SWITCHES];
    uint32_t sw_ms[SWITCHES];
    uint32_t done;                   /* decoded everything */
    engine_stats_t stats;
} engine_shared_t;

static engine_shared_t store SHARED;
static uint8_t fifo_store[ENGINE_FIFO_BYTES] SHARED;
#define E    ((volatile engine_shared_t *)UNCACHED(&store))
#define FIFO ((volatile uint8_t *)UNCACHED(fifo_store))

static uint32_t sw_r;                /* CPU: switches consumed */

/* ---- CPU side ---------------------------------------------------------------------- */

static void clear_shared(void)
{
    volatile uint32_t *w = (volatile uint32_t *)E;
    uint32_t gen = E->gen;
    for (unsigned i = 0; i < sizeof(engine_shared_t) / 4; i++) w[i] = 0;
    E->gen = E->gen_ack = gen;
    sw_r = 0;
}

void engine_init(void)
{
    volatile uint32_t *w = (volatile uint32_t *)E;
    for (unsigned i = 0; i < sizeof(engine_shared_t) / 4; i++) w[i] = 0;
    sw_r = 0;
}

void engine_restart(void)
{
    uint32_t g = E->gen + 1;
    E->go = 0;
    E->gen = g;
    uint32_t t0 = platform_us();
    while (E->gen_ack != g && platform_us() - t0 < 300000) platform_cop_kick();
    audio_flush();
    clear_shared();
}

void engine_go(void) { E->go = E->gen; platform_cop_kick(); }

int engine_write_space(uint8_t **p)
{
    uint32_t used = E->wr - E->rd;
    uint32_t free = ENGINE_FIFO_BYTES - used;
    uint32_t at = E->wr % ENGINE_FIFO_BYTES;
    uint32_t run = ENGINE_FIFO_BYTES - at;
    *p = (uint8_t *)(FIFO + at);
    return (int)(free < run ? free : run);
}

void engine_wrote(int n) { if (n > 0) E->wr = E->wr + (uint32_t)n; }

int engine_marks_free(void) { return MARKS - (int)(E->mark_w - E->mark_r); }

void engine_mark_track(int q, int hold, uint32_t start_ms)
{
    uint32_t i = E->mark_w;
    if (i - E->mark_r >= MARKS) return;      /* the feeder checks engine_marks_free() first */
    E->mark_pos[i % MARKS] = E->wr;
    E->mark_q[i % MARKS] = q;
    E->mark_hold[i % MARKS] = hold;
    E->mark_ms[i % MARKS] = start_ms;
    E->mark_w = i + 1;
}

void engine_end(void) { E->ended = 1; }

uint32_t engine_heard_frames(void)
{
    uint32_t c = E->committed, q = (uint32_t)audio_pending() * AUDIO_CHUNK_FRAMES;
    return c > q ? c - q : 0;
}

int engine_track_switch(uint32_t *frame, int *q, uint32_t *start_ms)
{
    if (sw_r == E->sw_w) return 0;
    *frame = E->sw_frame[sw_r % SWITCHES];
    *q = E->sw_q[sw_r % SWITCHES];
    *start_ms = E->sw_ms[sw_r % SWITCHES];
    sw_r++;
    return 1;
}

int engine_finished(void) { return E->done && audio_pending() == 0; }
uint32_t engine_buffered_bytes(void) { return E->wr - E->rd; }

void engine_stats(engine_stats_t *s)
{
    volatile uint32_t *src = (volatile uint32_t *)&E->stats;
    uint32_t *dst = (uint32_t *)s;
    for (unsigned i = 0; i < sizeof *s / 4; i++) dst[i] = src[i];
    s->err[sizeof s->err - 1] = 0;
}

/* ---- decoder side (COP) ----------------------------------------------------------------- */

static COP_DATA struct {
    uint32_t gen_seen;
    int16_t *chunk;
    int fill;                        /* frames already in chunk */
    int playing;                     /* a track is open in the player */
    uint32_t end_pos;                /* where the open track's bytes end (if known) */
} C;

/* The open track's bytes: up to the next mark, or the end of the stream. */
static int fifo_source(uint8_t *buf, int max)
{
    uint32_t rd = E->rd, wr = E->wr;
    uint32_t limit = wr;
    int bounded = 0;
    if (E->mark_r != E->mark_w) { limit = E->mark_pos[E->mark_r % MARKS]; bounded = 1; }
    else if (E->ended) bounded = 1;
    if (rd >= limit) return bounded ? 0 : -1;
    uint32_t n = limit - rd, at = rd % ENGINE_FIFO_BYTES, run = ENGINE_FIFO_BYTES - at;
    if (n > run) n = run;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) buf[i] = FIFO[at + i];
    E->rd = rd + n;
    return (int)n;
}

static void publish_stats(void)
{
    E->stats.dec_us = player_decode_us();
    E->stats.dec_ms = player_decoded_ms();
    E->stats.saturated = player_saturated();
    const uint32_t *p = player_profile();
    for (int i = 0; i < 4; i++) E->stats.prof[i] = p[i];
    const char *e = player_error();
    int i = 0;
    for (; e[i] && i < (int)sizeof E->stats.err - 1; i++) E->stats.err[i] = e[i];
    E->stats.err[i] = 0;
}

int engine_step(void)
{
    if (E->gen != C.gen_seen || E->gen_ack != E->gen) {      /* stop requested: drop everything */
        player_close();
        C.chunk = 0;
        C.fill = 0;
        C.playing = 0;
        C.gen_seen = E->gen;
        E->gen_ack = E->gen;
        return 0;
    }
    if (E->go != E->gen || E->done) return 0;
    int work = 0;
    while (audio_free_chunks() > 0) {
        if (!C.playing) {
            /* Next track: its mark must be at the read position. */
            if (E->mark_r != E->mark_w && E->mark_pos[E->mark_r % MARKS] == E->rd) {
                uint32_t i = E->mark_r;
                E->sw_frame[E->sw_w % SWITCHES] = E->committed + (uint32_t)C.fill;
                E->sw_q[E->sw_w % SWITCHES] = E->mark_q[i % MARKS];
                E->sw_ms[E->sw_w % SWITCHES] = E->mark_ms[i % MARKS];
                E->sw_w = E->sw_w + 1;
                E->mark_r = i + 1;
                player_open(fifo_source, E->mark_hold[i % MARKS]);
                C.playing = 1;
            } else if (E->ended && E->mark_r == E->mark_w && E->rd == E->wr) {
                if (C.fill) {                                    /* the last partial chunk */
                    if (!C.chunk) break;
                    for (int k = C.fill; k < AUDIO_CHUNK_FRAMES; k++) C.chunk[2 * k] = C.chunk[2 * k + 1] = 0;
                    audio_commit();
                    E->committed = E->committed + AUDIO_CHUNK_FRAMES;
                    C.chunk = 0;
                    C.fill = 0;
                }
                E->done = 1;
                return work;
            } else {
                return work;                                     /* waiting for the CPU to feed */
            }
        }
        if (!C.chunk) { C.chunk = audio_chunk(); C.fill = 0; if (!C.chunk) break; }
        int n = player_read(C.chunk + 2 * C.fill, AUDIO_CHUNK_FRAMES - C.fill);
        C.fill += n;
        if (n) work = 1;
        if (C.fill == AUDIO_CHUNK_FRAMES) {
            audio_commit();
            E->committed = E->committed + AUDIO_CHUNK_FRAMES;
            C.chunk = 0;
            C.fill = 0;
            publish_stats();
            if (E->gen != E->gen_ack) return work;
            continue;
        }
        if (player_stalled()) { publish_stats(); return work; }
        if (!player_is_open()) {                                 /* track over: skip to its end */
            publish_stats();
            if (E->mark_r != E->mark_w) E->rd = E->mark_pos[E->mark_r % MARKS];
            else if (E->ended) E->rd = E->wr;
            else { /* the CPU hasn't written the rest of this track yet: consume as it arrives */
                static COP_DATA uint8_t sink[256];
                if (fifo_source(sink, sizeof sink) != 0) return work;
            }
            C.playing = 0;
        }
    }
    return work;
}
