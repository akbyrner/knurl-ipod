#include <stddef.h>
#include <string.h>
#include "mp3dec.h"
#include "mp3_tables.inc"

extern const uint32_t mp3_pow43[8207];  /* build/gen/pow43.c (tools/build_pow43.py) */
uint32_t platform_us(void);             /* for the per-stage profile */
#define PROF(stage) do { uint32_t t_ = platform_us(); d->prof[stage] += t_ - prof_t; prof_t = t_; } while (0)

/* Fixed-point constants, folded at compile time (no float at run time). */
#define QN(x, n) ((int32_t)((x) * (double)(1u << (n)) + ((x) < 0 ? -0.5 : 0.5)))
#define Q30(x) QN(x, 30)
#define Q29(x) QN(x, 29)
#define Q27(x) QN(x, 27)
static inline int32_t mul30(int32_t a, int32_t c) { return (int32_t)(((int64_t)a * c) >> 30); }
static inline int32_t mul29(int32_t a, int32_t c) { return (int32_t)(((int64_t)a * c) >> 29); }
static inline int32_t mul27(int32_t a, int32_t c) { return (int32_t)(((int64_t)a * c) >> 27); }

#define MAX_FREE_FORMAT_FRAME_SIZE 2304
#define MAX_FRAME_SYNC_MATCHES     10
#define MAX_BITRESERVOIR_BYTES     511
#define SHORT_BLOCK_TYPE           2
#define STOP_BLOCK_TYPE            3
#define HDR_SIZE                   4
#define HDR_IS_MONO(h)             (((h[3]) & 0xC0) == 0xC0)
#define HDR_IS_MS_STEREO(h)        (((h[3]) & 0xE0) == 0x60)
#define HDR_IS_FREE_FORMAT(h)      (((h[2]) & 0xF0) == 0)
#define HDR_IS_CRC(h)              (!((h[1]) & 1))
#define HDR_TEST_PADDING(h)        ((h[2]) & 0x2)
#define HDR_TEST_MPEG1(h)          ((h[1]) & 0x8)
#define HDR_TEST_NOT_MPEG25(h)     ((h[1]) & 0x10)
#define HDR_TEST_I_STEREO(h)       ((h[3]) & 0x10)
#define HDR_TEST_MS_STEREO(h)      ((h[3]) & 0x20)
#define HDR_GET_LAYER(h)           (((h[1]) >> 1) & 3)
#define HDR_GET_BITRATE(h)         ((h[2]) >> 4)
#define HDR_GET_SAMPLE_RATE(h)     (((h[2]) >> 2) & 3)
#define HDR_IS_FRAME_576(h)        ((h[1] & 14) == 2)
#define HDR_IS_LAYER_1(h)          ((h[1] & 6) == 6)
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

/* ---- bitstream ------------------------------------------------------------------------ */

static void bs_init(mp3dec_t *d, const uint8_t *data, int bytes)
{
    d->bs.buf = data;
    d->bs.pos = 0;
    d->bs.limit = bytes * 8;
}

static uint32_t get_bits(mp3dec_t *d, int n)
{
    uint32_t next, cache = 0, s = d->bs.pos & 7;
    int shl = n + (int)s;
    const uint8_t *p = d->bs.buf + (d->bs.pos >> 3);
    if ((d->bs.pos += n) > d->bs.limit) return 0;
    next = *p++ & (255 >> s);
    while ((shl -= 8) > 0) {
        cache |= next << shl;
        next = *p++;
    }
    return cache | (next >> -shl);
}

/* ---- frame header ----------------------------------------------------------------------- */

static int hdr_valid(const uint8_t *h)
{
    return h[0] == 0xff && ((h[1] & 0xF0) == 0xf0 || (h[1] & 0xFE) == 0xe2) &&
           HDR_GET_LAYER(h) != 0 && HDR_GET_BITRATE(h) != 15 && HDR_GET_SAMPLE_RATE(h) != 3;
}

static int hdr_compare(const uint8_t *h1, const uint8_t *h2)
{
    return hdr_valid(h2) && ((h1[1] ^ h2[1]) & 0xFE) == 0 && ((h1[2] ^ h2[2]) & 0x0C) == 0 &&
           !(HDR_IS_FREE_FORMAT(h1) ^ HDR_IS_FREE_FORMAT(h2));
}

static unsigned hdr_bitrate_kbps(const uint8_t *h)
{
    static const uint8_t halfrate[2][3][15] = {
        { { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 }, { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 }, { 0,16,24,28,32,40,48,56,64,72,80,88,96,112,128 } },
        { { 0,16,20,24,28,32,40,48,56,64,80,96,112,128,160 }, { 0,16,24,28,32,40,48,56,64,80,96,112,128,160,192 }, { 0,16,32,48,64,80,96,112,128,144,160,176,192,208,224 } },
    };
    return 2u * halfrate[!!HDR_TEST_MPEG1(h)][HDR_GET_LAYER(h) - 1][HDR_GET_BITRATE(h)];
}

static unsigned hdr_sample_rate_hz(const uint8_t *h)
{
    static const unsigned hz[3] = { 44100, 48000, 32000 };
    return hz[HDR_GET_SAMPLE_RATE(h)] >> (int)!HDR_TEST_MPEG1(h) >> (int)!HDR_TEST_NOT_MPEG25(h);
}

static unsigned hdr_frame_samples(const uint8_t *h)
{
    return HDR_IS_LAYER_1(h) ? 384 : (1152 >> (int)HDR_IS_FRAME_576(h));
}

static int hdr_frame_bytes(const uint8_t *h, int free_format_size)
{
    int frame_bytes = (int)(hdr_frame_samples(h) * hdr_bitrate_kbps(h) * 125 / hdr_sample_rate_hz(h));
    if (HDR_IS_LAYER_1(h)) frame_bytes &= ~3;
    return frame_bytes ? frame_bytes : free_format_size;
}

static int hdr_padding(const uint8_t *h)
{
    return HDR_TEST_PADDING(h) ? (HDR_IS_LAYER_1(h) ? 4 : 1) : 0;
}

/* ---- side info and scale factors (MPEG-1) ------------------------------------------------ */

static int read_side_info(mp3dec_t *d, mp3_granule_t *gr, const uint8_t *hdr)
{
    unsigned tables, scfsi;
    int main_data_begin, part_23_sum = 0;
    int sr_idx = HDR_GET_SAMPLE_RATE(hdr) + 5;      /* rows 5..7 of the band tables are MPEG-1 */
    int gr_count = HDR_IS_MONO(hdr) ? 1 : 2;

    gr_count *= 2;
    main_data_begin = (int)get_bits(d, 9);
    scfsi = get_bits(d, 7 + gr_count);

    do {
        if (HDR_IS_MONO(hdr)) scfsi <<= 4;
        gr->part_23_length = (uint16_t)get_bits(d, 12);
        part_23_sum += gr->part_23_length;
        gr->big_values = (uint16_t)get_bits(d, 9);
        if (gr->big_values > 288) return -1;
        gr->global_gain = (uint8_t)get_bits(d, 8);
        gr->scalefac_compress = (uint16_t)get_bits(d, 4);
        gr->sfbtab = scf_long[sr_idx];
        gr->n_long_sfb = 22;
        gr->n_short_sfb = 0;
        if (get_bits(d, 1)) {
            gr->block_type = (uint8_t)get_bits(d, 2);
            if (!gr->block_type) return -1;
            gr->mixed_block_flag = (uint8_t)get_bits(d, 1);
            gr->region_count[0] = 7;
            gr->region_count[1] = 255;
            if (gr->block_type == SHORT_BLOCK_TYPE) {
                scfsi &= 0x0F0F;
                if (!gr->mixed_block_flag) {
                    gr->region_count[0] = 8;
                    gr->sfbtab = scf_short[sr_idx];
                    gr->n_long_sfb = 0;
                    gr->n_short_sfb = 39;
                } else {
                    gr->sfbtab = scf_mixed[sr_idx];
                    gr->n_long_sfb = 8;
                    gr->n_short_sfb = 30;
                }
            }
            tables = get_bits(d, 10);
            tables <<= 5;
            gr->subblock_gain[0] = (uint8_t)get_bits(d, 3);
            gr->subblock_gain[1] = (uint8_t)get_bits(d, 3);
            gr->subblock_gain[2] = (uint8_t)get_bits(d, 3);
        } else {
            gr->block_type = 0;
            gr->mixed_block_flag = 0;
            tables = get_bits(d, 15);
            gr->region_count[0] = (uint8_t)get_bits(d, 4);
            gr->region_count[1] = (uint8_t)get_bits(d, 3);
            gr->region_count[2] = 255;
        }
        gr->table_select[0] = (uint8_t)(tables >> 10);
        gr->table_select[1] = (uint8_t)((tables >> 5) & 31);
        gr->table_select[2] = (uint8_t)(tables & 31);
        gr->preflag = (uint8_t)get_bits(d, 1);
        gr->scalefac_scale = (uint8_t)get_bits(d, 1);
        gr->count1_table = (uint8_t)get_bits(d, 1);
        gr->scfsi = (uint8_t)((scfsi >> 12) & 15);
        scfsi <<= 4;
        gr++;
    } while (--gr_count);

    if (part_23_sum + d->bs.pos > d->bs.limit + main_data_begin * 8) return -1;
    return main_data_begin;
}

static void read_scalefactors(mp3dec_t *d, uint8_t *scf, uint8_t *ist_pos, const uint8_t *scf_size,
                              const uint8_t *scf_count, int scfsi)
{
    for (int i = 0; i < 4 && scf_count[i]; i++, scfsi *= 2) {
        int cnt = scf_count[i];
        if (scfsi & 8) {
            memcpy(scf, ist_pos, (size_t)cnt);
        } else {
            int bits = scf_size[i];
            if (!bits) {
                memset(scf, 0, (size_t)cnt);
                memset(ist_pos, 0, (size_t)cnt);
            } else {
                int max_scf = (scfsi < 0) ? (1 << bits) - 1 : -1;
                for (int k = 0; k < cnt; k++) {
                    int s = (int)get_bits(d, bits);
                    ist_pos[k] = (uint8_t)(s == max_scf ? -1 : s);
                    scf[k] = (uint8_t)s;
                }
            }
        }
        ist_pos += cnt;
        scf += cnt;
    }
    scf[0] = scf[1] = scf[2] = 0;
}

/* 2^(r/4), Q30 */
static const int32_t frac4[4] = { Q30(1.0), Q30(1.18920711500272), Q30(1.41421356237310), Q30(1.68179283050743) };

/* Each band's gain 2^(e4/4) as a Q30 factor and a power-of-two shift (relative to the
 * pow43 table's scale), so dequantising a value is one multiply and one shift. */
static void decode_scalefactors(mp3dec_t *d, const uint8_t *hdr, uint8_t *ist_pos, const mp3_granule_t *gr)
{
    const uint8_t *scf_partition = scf_partitions[!!gr->n_short_sfb + !gr->n_long_sfb];
    uint8_t scf_size[4], iscf[40];
    int i, scf_shift = gr->scalefac_scale + 1;
    int part = scfc_decode[gr->scalefac_compress];
    scf_size[1] = scf_size[0] = (uint8_t)(part >> 2);
    scf_size[3] = scf_size[2] = (uint8_t)(part & 3);
    read_scalefactors(d, iscf, ist_pos, scf_size, scf_partition, gr->scfsi);

    if (gr->n_short_sfb) {
        int sh = 3 - scf_shift;
        for (i = 0; i < gr->n_short_sfb; i += 3) {
            iscf[gr->n_long_sfb + i + 0] = (uint8_t)(iscf[gr->n_long_sfb + i + 0] + (gr->subblock_gain[0] << sh));
            iscf[gr->n_long_sfb + i + 1] = (uint8_t)(iscf[gr->n_long_sfb + i + 1] + (gr->subblock_gain[1] << sh));
            iscf[gr->n_long_sfb + i + 2] = (uint8_t)(iscf[gr->n_long_sfb + i + 2] + (gr->subblock_gain[2] << sh));
        }
    } else if (gr->preflag) {
        for (i = 0; i < 10; i++) iscf[11 + i] = (uint8_t)(iscf[11 + i] + preamp[i]);
    }

    /* minimp3's scale: output = 0.5 x the ISO value (so the synthesis window is in 16-bit
     * units); M/S folds its 1/sqrt(2) in here as another -2 quarter steps. */
    int gain_exp = gr->global_gain - 4 - 210 - (HDR_IS_MS_STEREO(hdr) ? 2 : 0);
    for (i = 0; i < (int)(gr->n_long_sfb + gr->n_short_sfb); i++) {
        int e4 = gain_exp - (iscf[i] << scf_shift);
        d->scf_mul[i] = frac4[e4 & 3];
        d->scf_sh[i] = (int8_t)((e4 >> 2) - 26 + MP3_FRAC);     /* pow43 mantissa is 2^26-scaled */
    }
}

/* |x|^(4/3) * band gain, in MP3_FRAC fixed point. */
static inline int32_t dequant(mp3dec_t *d, unsigned x, int32_t mul, int sh)
{
    uint32_t p = mp3_pow43[x];
    int s = (int)(p & 31) + sh;
    int32_t t = (int32_t)(((int64_t)(p >> 5) * mul) >> 30);   /* < 2^27.8 */
    if (s >= 0) {
        if (s > 3) { d->saturated++; return 0x7fffffff; }
        return t << s;
    }
    if (s < -30) return 0;
    return (t + (1 << (-s - 1))) >> -s;
}

/* ---- Huffman decoding (minimp3's table walk) ------------------------------------------- */

static void huffman(mp3dec_t *d, int32_t *dst, const mp3_granule_t *gr_info, int layer3gr_limit)
{
#define PEEK_BITS(n)  (bs_cache >> (32 - (n)))
#define FLUSH_BITS(n) { bs_cache <<= (n); bs_sh += (n); }
#define CHECK_BITS    while (bs_sh >= 0) { bs_cache |= (uint32_t)*bs_next_ptr++ << bs_sh; bs_sh -= 8; }
#define BSPOS         ((int)(bs_next_ptr - d->bs.buf) * 8 - 24 + bs_sh)
    int32_t mul = 0;
    int sh = 0, band = 0;
    int ireg = 0, big_val_cnt = gr_info->big_values;
    const uint8_t *sfb = gr_info->sfbtab;
    const uint8_t *bs_next_ptr = d->bs.buf + d->bs.pos / 8;
    uint32_t bs_cache = (((bs_next_ptr[0] * 256u + bs_next_ptr[1]) * 256u + bs_next_ptr[2]) * 256u + bs_next_ptr[3]) << (d->bs.pos & 7);
    int pairs_to_decode, np, bs_sh = (d->bs.pos & 7) - 8;
    bs_next_ptr += 4;

    while (big_val_cnt > 0) {
        int tab_num = gr_info->table_select[ireg];
        int sfb_cnt = gr_info->region_count[ireg++];
        const int16_t *codebook = huff_tabs + huff_tabindex[tab_num];
        int linbits = huff_linbits[tab_num];
        do {
            np = *sfb++ / 2;
            pairs_to_decode = MIN(big_val_cnt, np);
            mul = d->scf_mul[band];
            sh = d->scf_sh[band++];
            do {
                int w = 5;
                int leaf = codebook[PEEK_BITS(w)];
                while (leaf < 0) {
                    FLUSH_BITS(w);
                    w = leaf & 7;
                    leaf = codebook[PEEK_BITS(w) - (leaf >> 3)];
                }
                FLUSH_BITS(leaf >> 8);
                for (int j = 0; j < 2; j++, dst++, leaf >>= 4) {
                    int lsb = leaf & 0x0F;
                    if (lsb == 15 && linbits) {
                        lsb += (int)PEEK_BITS(linbits);
                        FLUSH_BITS(linbits);
                        CHECK_BITS;
                    }
                    if (lsb) {
                        int32_t v = dequant(d, (unsigned)lsb, mul, sh);
                        *dst = (int32_t)bs_cache < 0 ? -v : v;
                        FLUSH_BITS(1);
                    } else {
                        *dst = 0;
                    }
                }
                CHECK_BITS;
            } while (--pairs_to_decode);
        } while ((big_val_cnt -= np) > 0 && --sfb_cnt >= 0);
    }

    int32_t one = band ? dequant(d, 1, mul, sh) : 0;    /* +-1 in the band big values ended in */
    for (np = 1 - big_val_cnt;; dst += 4) {
        const uint8_t *codebook_count1 = gr_info->count1_table ? huff_tab33 : huff_tab32;
        int leaf = codebook_count1[PEEK_BITS(4)];
        if (!(leaf & 8))
            leaf = codebook_count1[(leaf >> 3) + (bs_cache << 4 >> (32 - (leaf & 3)))];
        FLUSH_BITS(leaf & 7);
        if (BSPOS > layer3gr_limit) break;
#define RELOAD_SCALEFACTOR  if (!--np) { np = *sfb++ / 2; if (!np) break; one = dequant(d, 1, d->scf_mul[band], d->scf_sh[band]); band++; }
#define DEQ_COUNT1(s) if (leaf & (128 >> (s))) { dst[s] = ((int32_t)bs_cache < 0) ? -one : one; FLUSH_BITS(1) }
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(0);
        DEQ_COUNT1(1);
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(2);
        DEQ_COUNT1(3);
        CHECK_BITS;
    }
    d->bs.pos = layer3gr_limit;
#undef RELOAD_SCALEFACTOR
#undef DEQ_COUNT1
}

/* ---- stereo ----------------------------------------------------------------------------- */

static void midside_stereo(int32_t *left, int n)
{
    int32_t *right = left + 576;
    for (int i = 0; i < n; i++) {
        int32_t a = left[i], b = right[i];
        left[i] = a + b;
        right[i] = a - b;
    }
}

static void intensity_stereo_band(int32_t *left, int n, int32_t kl, int32_t kr)
{
    for (int i = 0; i < n; i++) {
        left[i + 576] = mul30(left[i], kr);
        left[i] = mul30(left[i], kl);
    }
}

static void stereo_top_band(const int32_t *right, const uint8_t *sfb, int nbands, int max_band[3])
{
    max_band[0] = max_band[1] = max_band[2] = -1;
    for (int i = 0; i < nbands; i++) {
        for (int k = 0; k < sfb[i]; k += 2) {
            if (right[k] != 0 || right[k + 1] != 0) {
                max_band[i % 3] = i;
                break;
            }
        }
        right += sfb[i];
    }
}

static void stereo_process(int32_t *left, const uint8_t *ist_pos, const uint8_t *sfb, const uint8_t *hdr, int max_band[3])
{
    static const int32_t pan[7 * 2] = {
        Q30(0), Q30(1), Q30(0.21132487), Q30(0.78867513), Q30(0.36602540), Q30(0.63397460),
        Q30(0.5), Q30(0.5), Q30(0.63397460), Q30(0.36602540), Q30(0.78867513), Q30(0.21132487), Q30(1), Q30(0)
    };
    for (unsigned i = 0; sfb[i]; i++) {
        unsigned ipos = ist_pos[i];
        if ((int)i > max_band[i % 3] && ipos < 7) {
            int32_t kl = pan[2 * ipos], kr = pan[2 * ipos + 1];
            if (HDR_TEST_MS_STEREO(hdr)) {
                kl = mul30(kl, Q30(1.41421356));
                kr = mul30(kr, Q30(1.41421356));
            }
            intensity_stereo_band(left, sfb[i], kl, kr);
        } else if (HDR_TEST_MS_STEREO(hdr)) {
            midside_stereo(left, sfb[i]);
        }
        left += sfb[i];
    }
}

static void intensity_stereo(int32_t *left, uint8_t *ist_pos, const mp3_granule_t *gr, const uint8_t *hdr)
{
    int max_band[3], n_sfb = gr->n_long_sfb + gr->n_short_sfb;
    int max_blocks = gr->n_short_sfb ? 3 : 1;
    stereo_top_band(left + 576, gr->sfbtab, n_sfb, max_band);
    if (gr->n_long_sfb)
        max_band[0] = max_band[1] = max_band[2] = MAX(MAX(max_band[0], max_band[1]), max_band[2]);
    for (int i = 0; i < max_blocks; i++) {
        int itop = n_sfb - max_blocks + i;
        int prev = itop - max_blocks;
        ist_pos[itop] = (uint8_t)(max_band[i] >= prev ? 3 : ist_pos[prev]);
    }
    stereo_process(left, ist_pos, gr->sfbtab, hdr, max_band);
}

/* ---- reorder, alias reduction, IMDCT ---------------------------------------------------- */

static void reorder(int32_t *grbuf, int32_t *scratch, const uint8_t *sfb)
{
    int32_t *src = grbuf, *dst = scratch;
    for (int len; 0 != (len = *sfb); sfb += 3, src += 2 * len) {
        for (int i = 0; i < len; i++, src++) {
            *dst++ = src[0 * len];
            *dst++ = src[1 * len];
            *dst++ = src[2 * len];
        }
    }
    memcpy(grbuf, scratch, (size_t)(dst - scratch) * sizeof(int32_t));
}

static void antialias(int32_t *grbuf, int nbands)
{
    static const int32_t aa[2][8] = {
        { Q30(0.85749293), Q30(0.88174200), Q30(0.94962865), Q30(0.98331459), Q30(0.99551782), Q30(0.99916056), Q30(0.99989920), Q30(0.99999316) },
        { Q30(0.51449576), Q30(0.47173197), Q30(0.31337745), Q30(0.18191320), Q30(0.09457419), Q30(0.04096558), Q30(0.01419856), Q30(0.00369997) }
    };
    for (; nbands > 0; nbands--, grbuf += 18) {
        for (int i = 0; i < 8; i++) {
            int32_t u = grbuf[18 + i], dn = grbuf[17 - i];
            grbuf[18 + i] = (int32_t)(((int64_t)u * aa[0][i] - (int64_t)dn * aa[1][i]) >> 30);
            grbuf[17 - i] = (int32_t)(((int64_t)u * aa[1][i] + (int64_t)dn * aa[0][i]) >> 30);
        }
    }
}

static void dct3_9(int32_t *y)
{
    int32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, t0, t2, t4;
    s0 = y[0]; s2 = y[2]; s4 = y[4]; s6 = y[6]; s8 = y[8];
    t0 = s0 + (s6 >> 1);
    s0 -= s6;
    t4 = mul30(s4 + s2, Q30(0.93969262));
    t2 = mul30(s8 + s2, Q30(0.76604444));
    s6 = mul30(s4 - s8, Q30(0.17364818));
    s4 += s8 - s2;

    s2 = s0 - (s4 >> 1);
    y[4] = s4 + s0;
    s8 = t0 - t2 + s6;
    s0 = t0 - t4 + t2;
    s4 = t0 + t4 - s6;

    s1 = y[1]; s3 = y[3]; s5 = y[5]; s7 = y[7];
    s3 = mul30(s3, Q30(0.86602540));
    t0 = mul30(s5 + s1, Q30(0.98480775));
    t4 = mul30(s5 - s7, Q30(0.34202014));
    t2 = mul30(s1 + s7, Q30(0.64278761));
    s1 = mul30(s1 - s5 - s7, Q30(0.86602540));

    s5 = t0 - s3 - t2;
    s7 = t4 - s3 - t0;
    s3 = t4 + s3 - t2;

    y[0] = s4 - s7;
    y[1] = s2 + s1;
    y[2] = s0 - s3;
    y[3] = s8 + s5;
    y[5] = s8 - s5;
    y[6] = s0 + s3;
    y[7] = s2 - s1;
    y[8] = s4 + s7;
}

static void imdct36(int32_t *grbuf, int32_t *overlap, const int32_t *window, int nbands)
{
    static const int32_t twid9[18] = {
        Q30(0.73727734), Q30(0.79335334), Q30(0.84339145), Q30(0.88701083), Q30(0.92387953), Q30(0.95371695),
        Q30(0.97629601), Q30(0.99144486), Q30(0.99904822), Q30(0.67559021), Q30(0.60876143), Q30(0.53729961),
        Q30(0.46174861), Q30(0.38268343), Q30(0.30070580), Q30(0.21643961), Q30(0.13052619), Q30(0.04361938)
    };
    for (int j = 0; j < nbands; j++, grbuf += 18, overlap += 9) {
        int32_t co[9], si[9];
        co[0] = -grbuf[0];
        si[0] = grbuf[17];
        for (int i = 0; i < 4; i++) {
            si[8 - 2 * i] = grbuf[4 * i + 1] - grbuf[4 * i + 2];
            co[1 + 2 * i] = grbuf[4 * i + 1] + grbuf[4 * i + 2];
            si[7 - 2 * i] = grbuf[4 * i + 4] - grbuf[4 * i + 3];
            co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
        }
        dct3_9(co);
        dct3_9(si);
        si[1] = -si[1];
        si[3] = -si[3];
        si[5] = -si[5];
        si[7] = -si[7];
        for (int i = 0; i < 9; i++) {
            int32_t ovl = overlap[i];
            int32_t sum = (int32_t)(((int64_t)co[i] * twid9[9 + i] + (int64_t)si[i] * twid9[0 + i]) >> 30);
            overlap[i] = (int32_t)(((int64_t)co[i] * twid9[0 + i] - (int64_t)si[i] * twid9[9 + i]) >> 30);
            grbuf[i] = (int32_t)(((int64_t)ovl * window[0 + i] - (int64_t)sum * window[9 + i]) >> 30);
            grbuf[17 - i] = (int32_t)(((int64_t)ovl * window[9 + i] + (int64_t)sum * window[0 + i]) >> 30);
        }
    }
}

static void idct3(int32_t x0, int32_t x1, int32_t x2, int32_t *dst)
{
    int32_t m1 = mul30(x1, Q30(0.86602540));
    int32_t a1 = x0 - (x2 >> 1);
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

static void imdct12(const int32_t *x, int32_t *dst, int32_t *overlap)
{
    static const int32_t twid3[6] = { Q30(0.79335334), Q30(0.92387953), Q30(0.99144486), Q30(0.60876143), Q30(0.38268343), Q30(0.13052619) };
    int32_t co[3], si[3];
    idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];
    for (int i = 0; i < 3; i++) {
        int32_t ovl = overlap[i];
        int32_t sum = (int32_t)(((int64_t)co[i] * twid3[3 + i] + (int64_t)si[i] * twid3[0 + i]) >> 30);
        overlap[i] = (int32_t)(((int64_t)co[i] * twid3[0 + i] - (int64_t)si[i] * twid3[3 + i]) >> 30);
        dst[i] = (int32_t)(((int64_t)ovl * twid3[2 - i] - (int64_t)sum * twid3[5 - i]) >> 30);
        dst[5 - i] = (int32_t)(((int64_t)ovl * twid3[5 - i] + (int64_t)sum * twid3[2 - i]) >> 30);
    }
}

static void imdct_short(int32_t *grbuf, int32_t *overlap, int nbands)
{
    for (; nbands > 0; nbands--, overlap += 9, grbuf += 18) {
        int32_t tmp[18];
        memcpy(tmp, grbuf, sizeof tmp);
        memcpy(grbuf, overlap, 6 * sizeof(int32_t));
        imdct12(tmp, grbuf + 6, overlap + 6);
        imdct12(tmp + 1, grbuf + 12, overlap + 6);
        imdct12(tmp + 2, overlap, overlap + 6);
    }
}

static void change_sign(int32_t *grbuf)
{
    grbuf += 18;
    for (int b = 0; b < 32; b += 2, grbuf += 36)
        for (int i = 1; i < 18; i += 2)
            grbuf[i] = -grbuf[i];
}

static void imdct_gr(int32_t *grbuf, int32_t *overlap, unsigned block_type, unsigned n_long_bands)
{
    static const int32_t mdct_window[2][18] = {
        { Q30(0.99904822), Q30(0.99144486), Q30(0.97629601), Q30(0.95371695), Q30(0.92387953), Q30(0.88701083),
          Q30(0.84339145), Q30(0.79335334), Q30(0.73727734), Q30(0.04361938), Q30(0.13052619), Q30(0.21643961),
          Q30(0.30070580), Q30(0.38268343), Q30(0.46174861), Q30(0.53729961), Q30(0.60876143), Q30(0.67559021) },
        { Q30(1), Q30(1), Q30(1), Q30(1), Q30(1), Q30(1), Q30(0.99144486), Q30(0.92387953), Q30(0.79335334),
          0, 0, 0, 0, 0, 0, Q30(0.13052619), Q30(0.38268343), Q30(0.60876143) }
    };
    if (n_long_bands) {
        imdct36(grbuf, overlap, mdct_window[0], (int)n_long_bands);
        grbuf += 18 * n_long_bands;
        overlap += 9 * n_long_bands;
    }
    if (block_type == SHORT_BLOCK_TYPE)
        imdct_short(grbuf, overlap, 32 - (int)n_long_bands);
    else
        imdct36(grbuf, overlap, mdct_window[block_type == STOP_BLOCK_TYPE], 32 - (int)n_long_bands);
}

/* ---- bit reservoir ---------------------------------------------------------------------- */

static void save_reservoir(mp3dec_t *d)
{
    int pos = (d->bs.pos + 7) / 8;
    int remains = d->bs.limit / 8 - pos;
    if (remains > MAX_BITRESERVOIR_BYTES) {
        pos += remains - MAX_BITRESERVOIR_BYTES;
        remains = MAX_BITRESERVOIR_BYTES;
    }
    if (remains > 0) memcpy(d->reserv_buf, d->maindata + pos, (size_t)remains);   /* separate buffers */
    d->reserv = remains > 0 ? remains : 0;
}

static int restore_reservoir(mp3dec_t *d, const uint8_t *frame, int pos_bits, int limit_bits, int main_data_begin)
{
    int frame_bytes = (limit_bits - pos_bits) / 8;
    int bytes_have = MIN(d->reserv, main_data_begin);
    memcpy(d->maindata, d->reserv_buf + MAX(0, d->reserv - main_data_begin), (size_t)bytes_have);
    memcpy(d->maindata + bytes_have, frame + pos_bits / 8, (size_t)frame_bytes);
    bs_init(d, d->maindata, bytes_have + frame_bytes);
    return d->reserv >= main_data_begin;
}

static void l3_decode(mp3dec_t *d, mp3_granule_t *gr_info, int nch)
{
    uint32_t prof_t = platform_us();
    for (int ch = 0; ch < nch; ch++) {
        int layer3gr_limit = d->bs.pos + gr_info[ch].part_23_length;
        decode_scalefactors(d, d->header, d->ist_pos[ch], gr_info + ch);
        huffman(d, d->grbuf[ch], gr_info + ch, layer3gr_limit);
    }
    PROF(0);
    if (HDR_TEST_I_STEREO(d->header))
        intensity_stereo(d->grbuf[0], d->ist_pos[1], gr_info, d->header);
    else if (HDR_IS_MS_STEREO(d->header))
        midside_stereo(d->grbuf[0], 576);

    for (int ch = 0; ch < nch; ch++, gr_info++) {
        int aa_bands = 31;
        int n_long_bands = gr_info->mixed_block_flag ? 2 : 0;
        if (gr_info->n_short_sfb) {
            aa_bands = n_long_bands - 1;
            reorder(d->grbuf[ch] + n_long_bands * 18, d->syn[0], gr_info->sfbtab + gr_info->n_long_sfb);
        }
        antialias(d->grbuf[ch], aa_bands);
        imdct_gr(d->grbuf[ch], d->mdct_overlap[ch], gr_info->block_type, (unsigned)n_long_bands);
        change_sign(d->grbuf[ch]);
    }
    PROF(1);
}

/* ---- polyphase synthesis ---------------------------------------------------------------- */

static void dct_ii(int32_t *grbuf, int n)
{
    static const int32_t sec[24] = {
        Q27(10.19000816), Q27(0.50060302), Q27(0.50241929), Q27(3.40760851), Q27(0.50547093), Q27(0.52249861),
        Q27(2.05778098), Q27(0.51544732), Q27(0.56694406), Q27(1.48416460), Q27(0.53104258), Q27(0.64682180),
        Q27(1.16943991), Q27(0.55310392), Q27(0.78815460), Q27(0.97256821), Q27(0.58293498), Q27(1.06067765),
        Q27(0.83934963), Q27(0.62250412), Q27(1.72244716), Q27(0.74453628), Q27(0.67480832), Q27(5.10114861)
    };
    for (int k = 0; k < n; k++) {
        int32_t t[4][8], *x, *y = grbuf + k;
        int i;
        for (x = t[0], i = 0; i < 8; i++, x++) {
            int32_t x0 = y[i * 18], x1 = y[(15 - i) * 18], x2 = y[(16 + i) * 18], x3 = y[(31 - i) * 18];
            int32_t t0 = x0 + x3, t1 = x1 + x2;
            int32_t t2 = mul27(x1 - x2, sec[3 * i + 0]);
            int32_t t3 = mul27(x0 - x3, sec[3 * i + 1]);
            x[0] = t0 + t1;
            x[8] = mul27(t0 - t1, sec[3 * i + 2]);
            x[16] = t3 + t2;
            x[24] = mul27(t3 - t2, sec[3 * i + 2]);
        }
        for (x = t[0], i = 0; i < 4; i++, x += 8) {
            int32_t x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
            xt = x0 - x7; x0 += x7;
            x7 = x1 - x6; x1 += x6;
            x6 = x2 - x5; x2 += x5;
            x5 = x3 - x4; x3 += x4;
            x4 = x0 - x3; x0 += x3;
            x3 = x1 - x2; x1 += x2;
            x[0] = x0 + x1;
            x[4] = mul29(x0 - x1, Q29(0.70710677));
            x5 = x5 + x6;
            x6 = mul29(x6 + x7, Q29(0.70710677));
            x7 = x7 + xt;
            x3 = mul29(x3 + x4, Q29(0.70710677));
            x5 -= mul29(x7, Q29(0.198912367));      /* rotate by PI/8 */
            x7 += mul29(x5, Q29(0.382683432));
            x5 -= mul29(x7, Q29(0.198912367));
            x0 = xt - x6; xt += x6;
            x[1] = mul29(xt + x7, Q29(0.50979561));
            x[2] = mul29(x4 + x3, Q29(0.54119611));
            x[3] = mul29(x0 - x5, Q29(0.60134488));
            x[5] = mul29(x0 + x5, Q29(0.89997619));
            x[6] = mul29(x4 - x3, Q29(1.30656302));
            x[7] = mul29(xt - x7, Q29(2.56291556));
        }
        for (i = 0; i < 7; i++, y += 4 * 18) {
            y[0 * 18] = t[0][i];
            y[1 * 18] = t[2][i] + t[3][i] + t[3][i + 1];
            y[2 * 18] = t[1][i] + t[1][i + 1];
            y[3 * 18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
        }
        y[0 * 18] = t[0][7];
        y[1 * 18] = t[2][7] + t[3][7];
        y[2 * 18] = t[1][7];
        y[3 * 18] = t[3][7];
    }
}

static inline int16_t scale_pcm(int64_t a)
{
    int64_t s = (a + ((int64_t)1 << (MP3_FRAC - 1))) >> MP3_FRAC;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static inline int16_t clip16(int32_t s)
{
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

void mp3_synth_lane(const int32_t *z, const int32_t *w, int32_t out[2]);   /* synth_arm.S */

static void synth_pair(int16_t *pcm, int nch, const int32_t *z)
{
    int64_t a;
    a  = (int64_t)(z[14 * 64] - z[0]) * 29;
    a += (int64_t)(z[1 * 64] + z[13 * 64]) * 213;
    a += (int64_t)(z[12 * 64] - z[2 * 64]) * 459;
    a += (int64_t)(z[3 * 64] + z[11 * 64]) * 2037;
    a += (int64_t)(z[10 * 64] - z[4 * 64]) * 5153;
    a += (int64_t)(z[5 * 64] + z[9 * 64]) * 6574;
    a += (int64_t)(z[8 * 64] - z[6 * 64]) * 37489;
    a += (int64_t)z[7 * 64] * 75038;
    pcm[0] = scale_pcm(a);

    z += 2;
    a  = (int64_t)z[14 * 64] * 104;
    a += (int64_t)z[12 * 64] * 1567;
    a += (int64_t)z[10 * 64] * 9727;
    a += (int64_t)z[8 * 64] * 64019;
    a += (int64_t)z[6 * 64] * -9975;
    a += (int64_t)z[4 * 64] * -45;
    a += (int64_t)z[2 * 64] * 146;
    a += (int64_t)z[0 * 64] * -5;
    pcm[16 * nch] = scale_pcm(a);
}

static void synth(int32_t *xl, int16_t *dstl, int nch, int32_t *lins)
{
    int32_t *xr = xl + 576 * (nch - 1);
    int16_t *dstr = dstl + (nch - 1);
    int32_t *zlin = lins + 15 * 64;
    const int32_t *w = synth_win;

    zlin[4 * 15]     = xl[18 * 16];
    zlin[4 * 15 + 1] = xr[18 * 16];
    zlin[4 * 15 + 2] = xl[0];
    zlin[4 * 15 + 3] = xr[0];
    zlin[4 * 31]     = xl[1 + 18 * 16];
    zlin[4 * 31 + 1] = xr[1 + 18 * 16];
    zlin[4 * 31 + 2] = xl[1];
    zlin[4 * 31 + 3] = xr[1];
    synth_pair(dstr, nch, lins + 4 * 15 + 1);
    synth_pair(dstr + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
    synth_pair(dstl, nch, lins + 4 * 15);
    synth_pair(dstl + 32 * nch, nch, lins + 4 * 15 + 64);

    for (int i = 14; i >= 0; i--, w += 16) {
        zlin[4 * i]          = xl[18 * (31 - i)];
        zlin[4 * i + 1]      = xr[18 * (31 - i)];
        zlin[4 * i + 2]      = xl[1 + 18 * (31 - i)];
        zlin[4 * i + 3]      = xr[1 + 18 * (31 - i)];
        zlin[4 * (i + 16)]   = xl[1 + 18 * (1 + i)];
        zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
        zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
        zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];
        /* One lane (L/R x two output slots) at a time, so the two 64-bit sums stay in
         * registers: 16 multiply-accumulates per output pair. Same sums, same order as
         * minimp3's lane-parallel version, so the result is bit-identical. On ARM this is
         * hand-written (synth_arm.S): GCC spilled it to the stack. */
        const int32_t *z = zlin + 4 * i;
        int32_t acc_a[4], acc_b[4];
        for (int j = 0; j < 4; j++) {
#if defined(__arm__) && !defined(MP3_NO_ASM)
            int32_t ab[2];
            mp3_synth_lane(z + j, w, ab);
            acc_a[j] = ab[0];
            acc_b[j] = ab[1];
#else
            const int32_t *zj = z + j;
            int64_t a = 0, b = 0;
#define TAP(k, sgn) { int32_t w0 = w[2 * (k)], w1 = w[2 * (k) + 1]; int32_t vz = zj[-(k) * 64], vy = zj[-(15 - (k)) * 64]; \
                      b += (int64_t)vz * w1 + (int64_t)vy * w0; \
                      if (sgn > 0) a += (int64_t)vz * w0 - (int64_t)vy * w1; else a += (int64_t)vy * w1 - (int64_t)vz * w0; }
            TAP(0, 1) TAP(1, -1) TAP(2, 1) TAP(3, -1) TAP(4, 1) TAP(5, -1) TAP(6, 1) TAP(7, -1)
#undef TAP
            acc_a[j] = (int32_t)((a + (1 << (MP3_FRAC - 1))) >> MP3_FRAC);
            acc_b[j] = (int32_t)((b + (1 << (MP3_FRAC - 1))) >> MP3_FRAC);
#endif
        }
        dstr[(15 - i) * nch] = clip16(acc_a[1]);
        dstr[(17 + i) * nch] = clip16(acc_b[1]);
        dstl[(15 - i) * nch] = clip16(acc_a[0]);
        dstl[(17 + i) * nch] = clip16(acc_b[0]);
        dstr[(47 - i) * nch] = clip16(acc_a[3]);
        dstr[(49 + i) * nch] = clip16(acc_b[3]);
        dstl[(47 - i) * nch] = clip16(acc_a[2]);
        dstl[(49 + i) * nch] = clip16(acc_b[2]);
    }
}

static void synth_granule(mp3dec_t *d, int32_t *qmf_state, int32_t *grbuf, int nbands, int nch, int16_t *pcm, int32_t *lins)
{
    uint32_t prof_t = platform_us();
    for (int i = 0; i < nch; i++) dct_ii(grbuf + 576 * i, nbands);
    PROF(2);
    memcpy(lins, qmf_state, sizeof(int32_t) * 15 * 64);
    memset(lins + 15 * 64, 0, sizeof(int32_t) * 64 * (size_t)nbands);
    for (int i = 0; i < nbands; i += 2) synth(grbuf + i, pcm + 32 * nch * i, nch, lins + i * 64);
    if (nch == 1) {
        for (int i = 0; i < 15 * 64; i += 2) qmf_state[i] = lins[nbands * 64 + i];
    } else {
        memcpy(qmf_state, lins + nbands * 64, sizeof(int32_t) * 15 * 64);
    }
    PROF(3);
}

static int hdr_is_null_or_tag(const uint8_t *h)
{
    return (h[0] == 'T' && h[1] == 'A' && h[2] == 'G') || (h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0);
}

/* ---- frame sync ------------------------------------------------------------------------- */


static int match_frame(const uint8_t *hdr, int mp3_bytes, int frame_bytes)
{
    int i = 0;
    for (int nmatch = 0; nmatch < MAX_FRAME_SYNC_MATCHES; nmatch++) {
        i += hdr_frame_bytes(hdr + i, frame_bytes) + hdr_padding(hdr + i);
        if (i + HDR_SIZE > mp3_bytes) return nmatch > 0;
        if (hdr_is_null_or_tag(hdr + i)) return nmatch > 0;
        if (!hdr_compare(hdr, hdr + i)) return 0;
    }
    return 1;
}

static int find_frame(const uint8_t *mp3, int mp3_bytes, int *free_format_bytes, int *ptr_frame_bytes)
{
    for (int i = 0; i < mp3_bytes - HDR_SIZE; i++, mp3++) {
        if (hdr_valid(mp3)) {
            int frame_bytes = hdr_frame_bytes(mp3, *free_format_bytes);
            int frame_and_padding = frame_bytes + hdr_padding(mp3);
            for (int k = HDR_SIZE; !frame_bytes && k < MAX_FREE_FORMAT_FRAME_SIZE && i + 2 * k < mp3_bytes - HDR_SIZE; k++) {
                if (hdr_compare(mp3, mp3 + k)) {
                    int fb = k - hdr_padding(mp3);
                    int nextfb = fb + hdr_padding(mp3 + k);
                    if (i + k + nextfb + HDR_SIZE > mp3_bytes || !hdr_compare(mp3, mp3 + k + nextfb)) continue;
                    frame_and_padding = k;
                    frame_bytes = fb;
                    *free_format_bytes = fb;
                }
            }
            if ((frame_bytes && i + frame_and_padding <= mp3_bytes && match_frame(mp3, mp3_bytes - i, frame_bytes)) ||
                (!i && frame_and_padding == mp3_bytes)) {
                *ptr_frame_bytes = frame_and_padding;
                return i;
            }
            *free_format_bytes = 0;
        }
    }
    *ptr_frame_bytes = 0;
    return mp3_bytes;
}

/* ---- public ----------------------------------------------------------------------------- */

void mp3_init(mp3dec_t *d)
{
    d->header[0] = 0;
}

int mp3_decode_frame(mp3dec_t *d, const uint8_t *mp3, int mp3_bytes, int16_t *pcm, mp3_info_t *info)
{
    int i = 0, frame_size = 0, success = 1;
    memset(d->ist_pos, 0, sizeof d->ist_pos);
    if (mp3_bytes > 4 && d->header[0] == 0xff && hdr_compare(d->header, mp3)) {
        frame_size = hdr_frame_bytes(mp3, d->free_format_bytes) + hdr_padding(mp3);
        /* Same stream as the last frame: accept it if it's complete and followed by another
         * frame, an ID3v1 tag, or the end of the data (minimp3 would drop the last frame of a
         * file that ends in an ID3v1 tag, as the server's files do). */
        if (frame_size > mp3_bytes) {
            frame_size = 0;
        } else if (mp3_bytes - frame_size >= HDR_SIZE &&
                   !hdr_compare(mp3, mp3 + frame_size) && !hdr_is_null_or_tag(mp3 + frame_size)) {
            frame_size = 0;
        }
    }
    if (!frame_size) {
        uint32_t sat = d->saturated;
        memset(d, 0, offsetof(mp3dec_t, bs));          /* lost sync: start clean */
        d->saturated = sat;
        i = find_frame(mp3, mp3_bytes, &d->free_format_bytes, &frame_size);
        if (!frame_size || i + frame_size > mp3_bytes) {
            info->frame_bytes = i;
            return 0;
        }
    }
    const uint8_t *hdr = mp3 + i;
    memcpy(d->header, hdr, HDR_SIZE);
    info->frame_bytes = i + frame_size;
    info->frame_offset = i;
    info->channels = HDR_IS_MONO(hdr) ? 1 : 2;
    info->hz = (int)hdr_sample_rate_hz(hdr);
    info->bitrate_kbps = (int)hdr_bitrate_kbps(hdr);
    if (HDR_GET_LAYER(hdr) != 1 || !HDR_TEST_MPEG1(hdr)) return 0;   /* only MPEG-1 Layer III */

    bs_init(d, hdr + HDR_SIZE, frame_size - HDR_SIZE);
    if (HDR_IS_CRC(hdr)) get_bits(d, 16);
    int main_data_begin = read_side_info(d, d->gr_info, hdr);
    if (main_data_begin < 0 || d->bs.pos > d->bs.limit) {
        mp3_init(d);
        return 0;
    }
    success = restore_reservoir(d, hdr + HDR_SIZE, d->bs.pos, d->bs.limit, main_data_begin);
    if (success) {
        for (int igr = 0; igr < 2; igr++, pcm += 576 * info->channels) {
            memset(d->grbuf, 0, sizeof d->grbuf);
            l3_decode(d, d->gr_info + igr * info->channels, info->channels);
            synth_granule(d, d->qmf_state, d->grbuf[0], 18, info->channels, pcm, d->syn[0]);
        }
    }
    save_reservoir(d);
    return success * MP3_FRAME_SAMPLES;
}
