/* c/lib/audio/mp3.c -- from-scratch MPEG-1/2/2.5 Layer III decoder. See mp3.h.
 *
 * The pipeline, in the order the standard defines it:
 *
 *   frame header + side info -> scalefactors -> Huffman -> requantisation ->
 *   stereo -> reordering (short blocks) -> alias reduction -> IMDCT + window +
 *   overlap-add -> frequency inversion -> polyphase synthesis -> PCM
 *
 * Two deliberate choices about arithmetic, both aimed at making the result
 * reproducible rather than merely fast:
 *
 *  - The IMDCT and the synthesis matrixing are written as the direct sums the
 *    standard states, not as a factored fast transform. A fast DCT is a
 *    rearrangement whose correctness is an argument; the direct form IS the
 *    definition, and tests/unit/mp3_units_test.c checks it against an
 *    independently written naive reference. The cost is real but bounded: the
 *    filter bank is ~2000 multiplies per 32 output samples.
 *
 *  - No libm. Every transcendental is a compile-time constant in
 *    mp3_tables.h, and the two run-time nonlinearities -- x^(4/3) and 2^(e/4)
 *    -- are computed here from scratch. That is not purity for its own sake:
 *    mini-libc has no double-precision libm, and if these tables were built by
 *    calling pow() the host and the guest would build slightly different ones
 *    and `make test-audio-os` could not compare their outputs.
 *
 * Every input byte is untrusted. Field values that index tables are range
 * checked at the point of use, not on the assumption that a valid frame header
 * implies a valid frame body.
 */

#include <stdlib.h>
#include <string.h>
#include "mp3.h"
#include "abits.h"
#include "mp3_tables.h"

#define SBLIMIT   32
#define SSLIMIT   18
#define NLINES    576
#define MAXRESV   4096       /* bit reservoir + one frame of main data */

/* 2^(1/4), 2^(1/2), 2^(3/4). Written as literals because they are constants,
 * and as their exact shortest round-tripping decimal so the host and target
 * compilers produce identical bits. */
static const double POW2Q[4] = {
    1.0, 1.189207115002721, 1.4142135623730951, 1.681792830507429
};

struct granule {
    int part2_3_length;
    int big_values;
    int global_gain;
    int scalefac_compress;
    int window_switching;
    int block_type;
    int mixed;
    int table_select[3];
    int subblock_gain[3];
    int region0_count, region1_count;
    int preflag;
    int scalefac_scale;
    int count1table;
};

struct mp3dec {
    /* Huffman decode trees, one per ISO table number that carries a codebook.
     * Node n has children at tree[2n] and tree[2n+1]: a positive value is a
     * child node index, a negative value is -(symbol+1), zero is "no such
     * branch" and cannot occur in a complete code. */
    int32_t *tree[34];

    double pow43[8207];        /* |is|^(4/3) for every value the format can code */

    /* stream state */
    int lsf;                   /* 1 for MPEG-2 and MPEG-2.5 (half sample rates) */
    int mpeg25;
    int sfreq;                 /* 0..8, row into the scalefactor band tables */
    int rate, channels, mode, mode_ext, ngr;

    /* bit reservoir: Layer III frames may reference main data that arrived in
     * earlier frames, so the last 511 bytes of main data are always kept. */
    uint8_t resv[MAXRESV];
    int     resv_len;

    /* per-granule working state */
    int32_t isv[2][NLINES];
    int     nonzero[2];
    double  xr[2][NLINES];
    int     scf[2][40];
    int     scf_save[2][23];   /* MPEG-1 scfsi: granule 0's long scalefactors */

    double  overlap[2][SBLIMIT][SSLIMIT];
    double  v[2][1024];
    int     vofs[2];

    float   out[MP3_MAX_SAMPLES * 2];
};

/* --- arithmetic without libm -------------------------------------------- */

/* 2^n for a modest integer n, by constructing the exponent field. */
static double exp2i(int n)
{
    if (n > 1023) n = 1023;
    if (n < -1022) return 0.0;          /* the decoder never needs denormals */
    union { uint64_t u; double d; } v;
    v.u = ((uint64_t)(n + 1023)) << 52;
    return v.d;
}

/* 2^(e/4). e is in quarter-decibel-ish units and is routinely negative, so the
 * split must floor rather than truncate: >> 2 on a negative int is an
 * arithmetic shift, which is exactly floor division by 4. */
static double pow2q(int e)
{
    if (e < -1200) return 0.0;
    if (e > 1200) e = 1200;
    int f = e >> 2;                                /* arithmetic shift = floor/4 */
    int r = e - (int)((unsigned)f << 2);           /* unsigned: f may be negative */
    return POW2Q[r & 3] * exp2i(f);
}

/* Cube root by Newton's method on doubles, seeded from an exponent split.
 * Deterministic and identical on host and target, which is the requirement
 * here -- a libm cbrt would be correctly rounded but would differ between the
 * two builds and change the CRC the guest test compares. */
static double a_cbrt(double x)
{
    if (x <= 0.0) return 0.0;
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;   /* x = m * 2^e, m in [1,2) */
    v.u = (v.u & 0x000FFFFFFFFFFFFFull) | (1023ull << 52);
    double m = v.d;
    /* Fold the exponent remainder into the mantissa so the iteration always
     * sees an argument in [1, 8). */
    int q = e / 3, r = e - q * 3;
    if (r < 0) { r += 3; q -= 1; }
    double z = m * exp2i(r);
    double y = 1.5;                              /* mid-range seed for [1,8) */
    for (int i = 0; i < 12; i++)
        y = (2.0 * y + z / (y * y)) * (1.0 / 3.0);
    return y * exp2i(q);
}

/* --- CRC-16 over the protected part of a frame --------------------------- */
/* Polynomial x^16+x^15+x^2+1 (0x8005), initial value 0xFFFF, no reflection,
 * no final xor -- the CRC MPEG audio specifies. */
static uint16_t mp3_crc16(const uint8_t *p, long n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            c = (uint16_t)((c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1));
    }
    return c;
}

/* --- Huffman trees ------------------------------------------------------- */

static int build_tree(int32_t **out, const mp3_hufftab *t)
{
    if (!t->n) { *out = NULL; return AUDIO_OK; }
    /* A complete prefix code over n symbols is a full binary tree with exactly
     * n-1 internal nodes; 2n slots is generous and bounded. */
    int cap = 2 * t->n + 2;
    int32_t *tr = (int32_t *)calloc((size_t)cap * 2, sizeof(int32_t));
    if (!tr) return AUDIO_ERR_OOM;
    int next = 1;
    for (int s = 0; s < t->n; s++) {
        int len = t->len[s];
        uint32_t code = t->code[s];
        int cur = 0;
        for (int b = len - 1; b >= 0; b--) {
            int bit = (int)((code >> b) & 1u);
            int32_t *slot = &tr[cur * 2 + bit];
            if (b == 0) {
                if (*slot != 0) { free(tr); return AUDIO_ERR_CORRUPT; }
                *slot = -(s + 1);
            } else {
                if (*slot == 0) {
                    if (next >= cap) { free(tr); return AUDIO_ERR_CORRUPT; }
                    *slot = next++;
                } else if (*slot < 0) {
                    free(tr); return AUDIO_ERR_CORRUPT;   /* code is a prefix */
                }
                cur = *slot;
            }
        }
    }
    *out = tr;
    return AUDIO_OK;
}

/* Walk the tree. `end` bounds the read at the granule's part2_3 boundary so a
 * corrupt codeword cannot consume the next granule's bits. -1 on overrun. */
static int huff_sym(abits *b, const int32_t *tr, long end)
{
    int cur = 0;
    for (;;) {
        if (ab_pos(b) >= end || b->error) return -1;
        int bit = (int)ab_u1(b);
        int32_t nx = tr[cur * 2 + bit];
        if (nx < 0) return -nx - 1;
        if (nx == 0) return -1;
        cur = nx;
    }
}

/* --- frame header -------------------------------------------------------- */

static const int BITRATE1[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1
};
static const int BITRATE2[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1
};
static const int RATES[3][3] = {
    { 44100, 48000, 32000 },   /* MPEG-1   */
    { 22050, 24000, 16000 },   /* MPEG-2   */
    { 11025, 12000,  8000 }    /* MPEG-2.5 */
};

typedef struct {
    int version;      /* 0 = MPEG-2.5, 2 = MPEG-2, 3 = MPEG-1 */
    int lsf, mpeg25;
    int sfreq;        /* 0..8 index into the band tables */
    int rate, bitrate, channels, mode, mode_ext;
    int protect, padding;
    int frame_bytes, side_bytes;
} mp3hdr;

static int parse_header(const uint8_t *p, long len, mp3hdr *h)
{
    if (len < 4) return AUDIO_ERR_RANGE;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return AUDIO_ERR_CORRUPT;

    int version = (p[1] >> 3) & 3;
    int layer   = (p[1] >> 1) & 3;
    if (version == 1) return AUDIO_ERR_CORRUPT;      /* reserved */
    if (layer != 1) return AUDIO_ERR_UNSUPPORTED;    /* only Layer III */

    h->version = version;
    h->mpeg25 = (version == 0);
    h->lsf    = (version != 3);
    h->protect = !((p[1] >> 0) & 1);                 /* protection_bit is inverted */

    int br_idx = (p[2] >> 4) & 15;
    int sr_idx = (p[2] >> 2) & 3;
    if (sr_idx == 3) return AUDIO_ERR_CORRUPT;
    h->padding = (p[2] >> 1) & 1;

    h->mode     = (p[3] >> 6) & 3;
    h->mode_ext = (p[3] >> 4) & 3;
    h->channels = (h->mode == 3) ? 1 : 2;

    int emphasis = p[3] & 3;
    if (emphasis == 2) return AUDIO_ERR_CORRUPT;     /* reserved */

    h->bitrate = (version == 3) ? BITRATE1[br_idx] : BITRATE2[br_idx];
    if (h->bitrate < 0) return AUDIO_ERR_CORRUPT;
    if (h->bitrate == 0) return AUDIO_ERR_UNSUPPORTED;   /* free format */

    int rrow = (version == 3) ? 0 : (version == 2 ? 1 : 2);
    h->rate = RATES[rrow][sr_idx];
    h->sfreq = sr_idx + (version == 3 ? 0 : (version == 2 ? 3 : 6));

    long fb = (h->lsf ? 72000L : 144000L) * h->bitrate / h->rate + h->padding;
    if (fb < 24 || fb > 2048) return AUDIO_ERR_CORRUPT;
    h->frame_bytes = (int)fb;

    if (h->lsf) h->side_bytes = (h->channels == 1) ? 9 : 17;
    else        h->side_bytes = (h->channels == 1) ? 17 : 32;

    if (4 + (h->protect ? 2 : 0) + h->side_bytes > h->frame_bytes)
        return AUDIO_ERR_CORRUPT;
    return AUDIO_OK;
}

long mp3_id3_len(const uint8_t *data, long len)
{
    if (!data || len < 10) return 0;
    if (memcmp(data, "ID3", 3) != 0) return 0;
    /* Syncsafe: seven bits per byte, so no byte can look like a frame sync. */
    if ((data[6] | data[7] | data[8] | data[9]) & 0x80) return 0;
    long sz = ((long)data[6] << 21) | ((long)data[7] << 14) |
              ((long)data[8] << 7)  | (long)data[9];
    long total = 10 + sz;
    if (data[5] & 0x10) total += 10;              /* footer present */
    return (total > 0 && total <= len) ? total : 0;
}

int mp3_is_info_frame(const uint8_t *data, long len)
{
    mp3hdr h;
    if (parse_header(data, len, &h) != AUDIO_OK) return 0;
    if (h.frame_bytes > len) return 0;
    long o = 4 + (h.protect ? 2 : 0) + h.side_bytes;
    if (o + 4 <= (long)h.frame_bytes &&
        (memcmp(data + o, "Xing", 4) == 0 || memcmp(data + o, "Info", 4) == 0))
        return 1;
    if (36 + 4 <= (long)h.frame_bytes && memcmp(data + 36, "VBRI", 4) == 0)
        return 1;
    return 0;
}

/* --- side info ----------------------------------------------------------- */

static int read_side_info(mp3dec *d, const mp3hdr *h, const uint8_t *si,
                          struct granule gr[2][2], int *main_data_begin,
                          int scfsi[2][4])
{
    abits b;
    ab_init(&b, si, h->side_bytes);
    int nch = h->channels;

    if (h->lsf) {
        *main_data_begin = (int)ab_u(&b, 8);
        ab_u(&b, nch == 1 ? 1 : 2);              /* private_bits */
        for (int c = 0; c < 2; c++) for (int g = 0; g < 4; g++) scfsi[c][g] = 0;
    } else {
        *main_data_begin = (int)ab_u(&b, 9);
        ab_u(&b, nch == 1 ? 5 : 3);              /* private_bits */
        for (int c = 0; c < nch; c++)
            for (int g = 0; g < 4; g++) scfsi[c][g] = (int)ab_u1(&b);
    }

    int ngr = h->lsf ? 1 : 2;
    for (int g = 0; g < ngr; g++) {
        for (int c = 0; c < nch; c++) {
            struct granule *G = &gr[g][c];
            G->part2_3_length   = (int)ab_u(&b, 12);
            G->big_values       = (int)ab_u(&b, 9);
            G->global_gain      = (int)ab_u(&b, 8);
            G->scalefac_compress = (int)ab_u(&b, h->lsf ? 9 : 4);
            G->window_switching = (int)ab_u1(&b);
            if (G->window_switching) {
                G->block_type = (int)ab_u(&b, 2);
                G->mixed      = (int)ab_u1(&b);
                for (int i = 0; i < 2; i++) G->table_select[i] = (int)ab_u(&b, 5);
                G->table_select[2] = 0;
                for (int i = 0; i < 3; i++) G->subblock_gain[i] = (int)ab_u(&b, 3);
                /* Region counts are not transmitted when the window switches;
                 * the standard fixes the first region at the boundary that
                 * region1Start below encodes. */
                G->region0_count = (G->block_type == 2 && !G->mixed) ? 8 : 7;
                G->region1_count = 20 - G->region0_count;
                if (G->block_type == 0) return AUDIO_ERR_CORRUPT;  /* reserved here */
            } else {
                G->block_type = 0;
                G->mixed = 0;
                for (int i = 0; i < 3; i++) G->table_select[i] = (int)ab_u(&b, 5);
                G->region0_count = (int)ab_u(&b, 4);
                G->region1_count = (int)ab_u(&b, 3);
                for (int i = 0; i < 3; i++) G->subblock_gain[i] = 0;
            }
            G->preflag        = h->lsf ? 0 : (int)ab_u1(&b);
            G->scalefac_scale = (int)ab_u1(&b);
            G->count1table    = (int)ab_u1(&b);
            if (G->big_values > 288) return AUDIO_ERR_CORRUPT;  /* 2*big_values <= 576 */
        }
    }
    (void)d;
    return ab_error(&b) ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- scalefactors -------------------------------------------------------- */

/* MPEG-1: two lengths chosen by scalefac_compress, plus scfsi sharing between
 * the two granules of a frame (long blocks only). */
static int scalefactors_1(mp3dec *d, abits *b, struct granule *G,
                          int ch, int gr, const int scfsi[4])
{
    int *scf = d->scf[ch];
    int slen1 = mp3_slen1[G->scalefac_compress];
    int slen2 = mp3_slen2[G->scalefac_compress];
    memset(scf, 0, sizeof(d->scf[0]));
    int p = 0;

    if (G->block_type == 2) {
        if (G->mixed) {
            for (int sfb = 0; sfb < 8; sfb++) scf[p++] = (int)ab_u(b, slen1);
            for (int sfb = 3; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) scf[p++] = (int)ab_u(b, slen1);
        } else {
            for (int sfb = 0; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) scf[p++] = (int)ab_u(b, slen1);
        }
        for (int sfb = 6; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++) scf[p++] = (int)ab_u(b, slen2);
        /* sfb 12 short carries no scalefactor and stays zero. */
    } else {
        static const int GRP[4][2] = { { 0, 6 }, { 6, 11 }, { 11, 16 }, { 16, 21 } };
        for (int g = 0; g < 4; g++) {
            int slen = (g < 2) ? slen1 : slen2;
            for (int sfb = GRP[g][0]; sfb < GRP[g][1]; sfb++) {
                if (gr == 1 && scfsi[g]) scf[sfb] = d->scf_save[ch][sfb];
                else scf[sfb] = (int)ab_u(b, slen);
            }
        }
        for (int sfb = 0; sfb < 22; sfb++) d->scf_save[ch][sfb] = scf[sfb];
    }
    return ab_error(b) ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* MPEG-2/2.5: scalefac_compress is a 9-bit index that selects both a partition
 * of the bands into four groups and a field width for each group. The mapping
 * is the one ISO 13818-3 2.4.3.2 tabulates; it is generated here rather than
 * stored because it is a handful of nested ranges, not a table of magic. */
static void lsf_slen(int comp, int i_stereo, unsigned *slen, int *cls, int *preflag)
{
    *preflag = 0;
    if (i_stereo) {
        int n = comp >> 1;
        if (n < 180)      { int i = n / 36, j = (n % 36) / 6, k = n % 6;
                            *slen = (unsigned)(i | (j << 3) | (k << 6)); *cls = 3; }
        else if (n < 244) { n -= 180; int i = n / 16, j = (n % 16) / 4, k = n % 4;
                            *slen = (unsigned)(i | (j << 3) | (k << 6)); *cls = 4; }
        else if (n < 256) { n -= 244; int i = n / 3, j = n % 3;
                            *slen = (unsigned)(i | (j << 3)); *cls = 5; }
        else              { *slen = 0; *cls = 5; }
    } else {
        int n = comp;
        if (n < 400)      { int i = n / 80, j = (n % 80) / 16, k = (n % 16) / 4, l = n % 4;
                            *slen = (unsigned)(i | (j << 3) | (k << 6) | (l << 9)); *cls = 0; }
        else if (n < 500) { n -= 400; int i = n / 20, j = (n % 20) / 4, k = n % 4;
                            *slen = (unsigned)(i | (j << 3) | (k << 6)); *cls = 1; }
        else if (n < 512) { n -= 500; int i = n / 3, j = n % 3;
                            *slen = (unsigned)(i | (j << 3)); *cls = 2; *preflag = 1; }
        else              { *slen = 0; *cls = 2; *preflag = 1; }
    }
}

static int scalefactors_2(mp3dec *d, abits *b, struct granule *G, int ch, int i_stereo)
{
    int *scf = d->scf[ch];
    memset(scf, 0, sizeof(d->scf[0]));

    unsigned slen; int cls, pre;
    lsf_slen(G->scalefac_compress, i_stereo, &slen, &cls, &pre);
    G->preflag = pre;

    int blk = 0;
    if (G->block_type == 2) { blk = 1; if (G->mixed) blk = 2; }
    const uint8_t *nr = mp3_lsf_nr[blk][cls];

    int p = 0;
    for (int i = 0; i < 4; i++) {
        int num = (int)(slen & 7);
        slen >>= 3;
        for (int j = 0; j < (int)nr[i]; j++) {
            if (p >= 40) return AUDIO_ERR_CORRUPT;
            scf[p++] = num ? (int)ab_u(b, num) : 0;
        }
    }
    return ab_error(b) ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

/* --- Huffman spectrum ---------------------------------------------------- */

static int decode_spectrum(mp3dec *d, abits *b, struct granule *G, int ch, long end)
{
    int32_t *is = d->isv[ch];
    memset(is, 0, sizeof(d->isv[0]));
    const int16_t *sl = mp3_sfb_long[d->sfreq];

    /* Region boundaries. When the window switches, region0_count is not
     * transmitted and the standard fixes it -- but "fixed" means fixed in
     * scalefactor bands, not in lines. A short block's region0 is the first
     * three short bands over three windows, which is 36 lines at every
     * sampling rate except the 8 kHz row where the bands are twice as wide; a
     * start or stop block is a LONG block, so its region0 is the first eight
     * long bands, which is 36 lines only at 44.1 kHz and 54 at the MPEG-2
     * rates. Using 36 everywhere costs nothing on MPEG-1 and silently
     * misdecodes every window-switched MPEG-2 frame. */
    int region1, region2 = NLINES;
    if (G->window_switching) {
        if (G->block_type == 2) region1 = (d->sfreq == 8) ? 72 : 36;
        else                    region1 = sl[8];
    } else {
        int r0 = G->region0_count + 1;
        int r1 = r0 + G->region1_count + 1;
        if (r0 > 22) r0 = 22;
        if (r1 > 22) r1 = 22;
        region1 = sl[r0];
        region2 = sl[r1];
    }
    if (region1 > NLINES) region1 = NLINES;
    if (region2 > NLINES) region2 = NLINES;
    if (region2 < region1) region2 = region1;

    int nbig = G->big_values * 2;
    if (nbig > NLINES) nbig = NLINES;

    int i = 0;
    for (int region = 0; region < 3 && i < nbig; region++) {
        int stop = (region == 0) ? region1 : (region == 1) ? region2 : nbig;
        if (stop > nbig) stop = nbig;
        int ts = G->table_select[region];
        if (ts > 31) return AUDIO_ERR_CORRUPT;
        int book = mp3_ht_book[ts];
        int linbits = mp3_ht_linbits[ts];
        if (book == 0) {                       /* table 0: this region is all zeros */
            i = stop;
            continue;
        }
        const int32_t *tr = d->tree[book];
        int dim = mp3_huff[book].dim;
        if (!tr || !dim) return AUDIO_ERR_CORRUPT;

        while (i < stop) {
            int s = huff_sym(b, tr, end);
            if (s < 0) return AUDIO_OK;        /* ran out of bits: rest stays zero */
            int x = s / dim, y = s % dim;
            if (x == 15 && linbits) x += (int)ab_u(b, linbits);
            if (x && ab_u1(b)) x = -x;
            if (y == 15 && linbits) y += (int)ab_u(b, linbits);
            if (y && ab_u1(b)) y = -y;
            if (b->error || ab_pos(b) > end) return AUDIO_OK;
            is[i++] = x;
            is[i++] = y;
        }
    }

    /* count1: quadruples of -1/0/+1 until the granule's bits run out. */
    const int32_t *tr = d->tree[G->count1table ? 33 : 32];
    if (!tr) return AUDIO_ERR_CORRUPT;
    while (i <= NLINES - 4 && ab_pos(b) < end) {
        long save = ab_pos(b);
        int s = huff_sym(b, tr, end);
        if (s < 0) break;
        int q[4] = { (s >> 3) & 1, (s >> 2) & 1, (s >> 1) & 1, s & 1 };
        for (int k = 0; k < 4; k++)
            if (q[k] && ab_u1(b)) q[k] = -q[k];
        if (b->error || ab_pos(b) > end) {
            /* The quadruple did not fit. The standard's own note says a
             * decoder must discard it rather than emit a half-read one. */
            ab_seek(b, save);
            break;
        }
        for (int k = 0; k < 4; k++) is[i++] = q[k];
    }

    d->nonzero[ch] = 0;
    for (int k = NLINES - 1; k >= 0; k--)
        if (is[k]) { d->nonzero[ch] = k + 1; break; }
    return AUDIO_OK;
}

/* --- requantisation ------------------------------------------------------ */

static double dequant(mp3dec *d, int32_t v, double gain)
{
    int a = v < 0 ? -v : v;
    if (a > 8206) a = 8206;              /* the format cannot code more */
    double m = d->pow43[a] * gain;
    return v < 0 ? -m : m;
}

static void requantize(mp3dec *d, struct granule *G, int ch)
{
    const int16_t *sl = mp3_sfb_long[d->sfreq];
    const int16_t *ss = mp3_sfb_short[d->sfreq];
    const int32_t *is = d->isv[ch];
    const int *scf = d->scf[ch];
    double *xr = d->xr[ch];
    memset(xr, 0, sizeof(d->xr[0]));

    int shift = 1 + G->scalefac_scale;   /* scalefac multiplier, in quarter units */
    int gg = G->global_gain - 210;
    int p = 0;

    if (G->block_type == 2) {
        int sfb = 0;
        if (G->mixed) {
            int nlong = d->lsf ? 6 : 8;
            for (sfb = 0; sfb < nlong; sfb++) {
                double gain = pow2q(gg - (scf[p] << shift));
                for (int i = sl[sfb]; i < sl[sfb + 1] && i < NLINES; i++)
                    xr[i] = dequant(d, is[i], gain);
                p++;
            }
            sfb = 3;
        }
        for (; sfb < 13; sfb++) {
            int w0 = ss[sfb], w1 = ss[sfb + 1];
            int width = (w1 - w0) / 3;
            for (int w = 0; w < 3; w++) {
                double gain = pow2q(gg - 8 * G->subblock_gain[w] - (scf[p] << shift));
                for (int k = 0; k < width; k++) {
                    int idx = w0 + w * width + k;
                    if (idx < NLINES) xr[idx] = dequant(d, is[idx], gain);
                }
                p++;
            }
        }
    } else {
        for (int sfb = 0; sfb < 22; sfb++) {
            int sf = scf[sfb] + (G->preflag ? mp3_pretab[sfb] : 0);
            double gain = pow2q(gg - (sf << shift));
            for (int i = sl[sfb]; i < sl[sfb + 1] && i < NLINES; i++)
                xr[i] = dequant(d, is[i], gain);
        }
    }
}

/* Short blocks arrive ordered by scalefactor band then window; the filter bank
 * wants them ordered by subband. */
static void reorder(mp3dec *d, struct granule *G, int ch)
{
    if (G->block_type != 2) return;
    const int16_t *ss = mp3_sfb_short[d->sfreq];
    double *xr = d->xr[ch];
    double tmp[NLINES];

    int start_sfb = G->mixed ? 3 : 0;
    int begin = ss[start_sfb];
    memcpy(tmp, xr, sizeof(tmp));

    for (int sfb = start_sfb; sfb < 13; sfb++) {
        int w0 = ss[sfb], w1 = ss[sfb + 1];
        int width = (w1 - w0) / 3;
        if (width <= 0) continue;
        int line0 = w0 / 3;
        for (int w = 0; w < 3; w++) {
            for (int k = 0; k < width; k++) {
                int L = line0 + k;
                int dst = (L / 6) * 18 + w * 6 + (L % 6);
                int src = w0 + w * width + k;
                if (dst < NLINES && src < NLINES && dst >= begin)
                    xr[dst] = tmp[src];
            }
        }
    }
}

/* --- stereo -------------------------------------------------------------- */

static void stereo(mp3dec *d, struct granule gr[2][2], int g, int intensity_scale)
{
    int ms = (d->mode == 1) && (d->mode_ext & 2);
    int is = (d->mode == 1) && (d->mode_ext & 1);
    if (!ms && !is) return;

    double *L = d->xr[0], *R = d->xr[1];
    const int16_t *sl = mp3_sfb_long[d->sfreq];
    const int16_t *ss = mp3_sfb_short[d->sfreq];
    struct granule *G1 = &gr[g][1];

    /* Intensity stereo starts where the second channel's spectrum ends. */
    int is_start = NLINES;
    if (is) {
        is_start = d->nonzero[1];
        if (is_start > NLINES) is_start = NLINES;
    }

    if (ms) {
        static const double INV_SQRT2 = 0.7071067811865476;
        for (int i = 0; i < is_start; i++) {
            double m = L[i], s = R[i];
            L[i] = (m + s) * INV_SQRT2;
            R[i] = (m - s) * INV_SQRT2;
        }
    }

    if (!is) return;

    /* Above the boundary the right channel carries no spectrum; the left one
     * is panned by a position coded in the right channel's scalefactors. */
    const int *scf = d->scf[1];
    if (G1->block_type == 2) {
        for (int sfb = 0; sfb < 13; sfb++) {
            int w0 = ss[sfb], w1 = ss[sfb + 1];
            if (w1 <= is_start) continue;
            int width = (w1 - w0) / 3;
            for (int w = 0; w < 3; w++) {
                int pos = scf[sfb * 3 + w - (G1->mixed ? 1 : 0)];
                if (pos < 0 || pos > 15) continue;
                double t1, t2;
                if (d->lsf) {
                    if (pos & 1) { t1 = mp3_lsf_is1[intensity_scale][pos]; t2 = 1.0; }
                    else         { t1 = 1.0; t2 = mp3_lsf_is2[intensity_scale][pos]; }
                } else {
                    if (pos == 7) continue;
                    t1 = mp3_is_t1[pos]; t2 = mp3_is_t2[pos];
                }
                for (int k = 0; k < width; k++) {
                    int L2 = (w0 / 3) + k;
                    int idx = (L2 / 6) * 18 + w * 6 + (L2 % 6);
                    if (idx >= NLINES) continue;
                    double v = L[idx];
                    L[idx] = v * t1;
                    R[idx] = v * t2;
                }
            }
        }
    } else {
        for (int sfb = 0; sfb < 22; sfb++) {
            if (sl[sfb + 1] <= is_start) continue;
            int pos = scf[sfb < 21 ? sfb : 20];
            if (pos < 0 || pos > 15) continue;
            double t1, t2;
            if (d->lsf) {
                if (pos & 1) { t1 = mp3_lsf_is1[intensity_scale][pos]; t2 = 1.0; }
                else         { t1 = 1.0; t2 = mp3_lsf_is2[intensity_scale][pos]; }
            } else {
                if (pos == 7) continue;
                t1 = mp3_is_t1[pos]; t2 = mp3_is_t2[pos];
            }
            for (int i = sl[sfb]; i < sl[sfb + 1] && i < NLINES; i++) {
                double v = L[i];
                L[i] = v * t1;
                R[i] = v * t2;
            }
        }
    }
}

/* --- alias reduction ----------------------------------------------------- */

static void antialias(mp3dec *d, struct granule *G, int ch)
{
    double *xr = d->xr[ch];
#if AUDIO_SABOTAGE == 1
    /* NEGATIVE CONTROL (see `make test-audio-negctl`). Alias reduction is the
     * one stage whose omission still produces plausible-sounding audio, so it
     * is the honest thing to disable: it proves the conformance gate is
     * measuring the decoder and not merely observing that two programs both
     * produce sound. This block exists only under -DAUDIO_SABOTAGE=1, which no
     * shipping build defines. */
    (void)xr; (void)G;
    return;
#endif
    int nb;
    if (G->block_type == 2) nb = G->mixed ? 1 : 0;   /* only the long part, if any */
    else nb = SBLIMIT - 1;

    for (int sb = 0; sb < nb; sb++) {
        double *p = xr + sb * 18 + 18;               /* boundary between sb and sb+1 */
        for (int i = 0; i < 8; i++) {
            double a = p[-1 - i], b = p[i];
            p[-1 - i] = a * mp3_aa_cs[i] - b * mp3_aa_ca[i];
            p[i]      = b * mp3_aa_cs[i] + a * mp3_aa_ca[i];
        }
    }
}

/* --- IMDCT, windowing, overlap ------------------------------------------- */

/* 36-point IMDCT, direct form: x[i] = sum_k X[k] cos(pi/72 (2i+1+18)(2k+1)).
 * The cosine argument is always an integer multiple of pi/72 taken mod 144,
 * so mp3_cos72 represents it exactly. */
static void imdct36(const double *in, double *out)
{
    for (int i = 0; i < 36; i++) {
        double s = 0.0;
        int a = 2 * i + 1 + 18;
        for (int k = 0; k < 18; k++) {
            int m = (a * (2 * k + 1)) % 144;
            s += in[k] * mp3_cos72[m];
        }
        out[i] = s;
    }
}

/* 12-point IMDCT: x[i] = sum_k X[k] cos(pi/24 (2i+1+6)(2k+1)). */
static void imdct12(const double *in, double *out)
{
    for (int i = 0; i < 12; i++) {
        double s = 0.0;
        int a = 2 * i + 1 + 6;
        for (int k = 0; k < 6; k++) {
            int m = (a * (2 * k + 1)) % 48;
            s += in[k] * mp3_cos24[m];
        }
        out[i] = s;
    }
}

static void hybrid(mp3dec *d, struct granule *G, int ch, double sb[SBLIMIT][SSLIMIT])
{
    const double *xr = d->xr[ch];
    for (int s = 0; s < SBLIMIT; s++) {
        double raw[36], win[36];
        int bt = G->block_type;
        if (bt == 2 && G->mixed && s < 2) bt = 0;    /* mixed: two long subbands */

        if (bt == 2) {
            for (int i = 0; i < 36; i++) win[i] = 0.0;
            for (int w = 0; w < 3; w++) {
                double sub[12];
                imdct12(xr + s * 18 + w * 6, sub);
                for (int i = 0; i < 12; i++)
                    win[6 + w * 6 + i] += sub[i] * mp3_win[2][i];
            }
        } else {
            imdct36(xr + s * 18, raw);
            for (int i = 0; i < 36; i++) win[i] = raw[i] * mp3_win[bt][i];
        }

        for (int i = 0; i < 18; i++) {
            sb[s][i] = win[i] + d->overlap[ch][s][i];
            d->overlap[ch][s][i] = win[18 + i];
        }
        /* Frequency inversion: every second sample of every odd subband. */
        if (s & 1)
            for (int i = 1; i < 18; i += 2) sb[s][i] = -sb[s][i];
    }
}

/* --- polyphase synthesis ------------------------------------------------- */

/* The ISO decoder flow, written out: matrix 32 subband samples into 64 V
 * values, build a 512-tap window input from the last 16 V vectors, apply D,
 * and fold down to 32 PCM samples. */
static void synth(mp3dec *d, int ch, const double *S, float *out, int stride)
{
    double *V = d->v[ch];
    int ofs = (d->vofs[ch] - 64) & 1023;
    d->vofs[ch] = ofs;

    for (int i = 0; i < 64; i++) {
        double s = 0.0;
        for (int k = 0; k < 32; k++) {
            int m = ((16 + i) * (2 * k + 1)) & 127;
            s += S[k] * mp3_cos64[m];
        }
        V[(ofs + i) & 1023] = s;
    }

    double U[512];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 32; j++) {
            U[i * 64 + j]      = V[(ofs + i * 128 + j) & 1023];
            U[i * 64 + 32 + j] = V[(ofs + i * 128 + 96 + j) & 1023];
        }
    }

    for (int j = 0; j < 32; j++) {
        double s = 0.0;
        for (int i = 0; i < 16; i++) {
            int idx = j + 32 * i;
            s += U[idx] * MP3_DWIN(idx);
        }
        out[j * stride] = (float)s;
    }
}

/* --- decoder ------------------------------------------------------------- */

mp3dec *mp3_open(void)
{
    mp3dec *d = (mp3dec *)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    for (int t = 0; t < 34; t++) {
        if (build_tree(&d->tree[t], &mp3_huff[t]) != AUDIO_OK) {
            mp3_close(d);
            return NULL;
        }
    }
    for (int i = 0; i < 8207; i++)
        d->pow43[i] = (double)i * a_cbrt((double)i);
    return d;
}

void mp3_close(mp3dec *d)
{
    if (!d) return;
    for (int t = 0; t < 34; t++) free(d->tree[t]);
    free(d);
}

/* Decode one granule of one channel into d->xr[ch]. */
static int granule_spectrum(mp3dec *d, abits *b, struct granule gr[2][2],
                            int g, int ch, const int scfsi[2][4], int i_stereo)
{
    struct granule *G = &gr[g][ch];
    long start = ab_pos(b);
    long end = start + G->part2_3_length;
    if (end > b->len * 8) return AUDIO_ERR_CORRUPT;

    int rc;
    if (d->lsf) rc = scalefactors_2(d, b, G, ch, i_stereo && ch == 1);
    else        rc = scalefactors_1(d, b, G, ch, g, scfsi[ch]);
    if (rc != AUDIO_OK) return rc;

    rc = decode_spectrum(d, b, G, ch, end);
    if (rc != AUDIO_OK) return rc;

    ab_seek(b, end);
    requantize(d, G, ch);
    reorder(d, G, ch);
    return AUDIO_OK;
}

int mp3_decode(mp3dec *d, const uint8_t *data, long len, mp3frame *out, int *got)
{
    if (!d || !data || !out || !got) return AUDIO_ERR_RANGE;
    *got = 0;
    if (len < 4) return 0;

    mp3hdr h;
    int rc = parse_header(data, len, &h);
    if (rc == AUDIO_ERR_RANGE) return 0;
    if (rc != AUDIO_OK) return rc;
    if (h.frame_bytes > len) return 0;                 /* need more bytes */

    long off = 4;
    if (h.protect) {
        /* The CRC covers the last two header bytes and the whole side info. */
        uint16_t want = (uint16_t)((data[4] << 8) | data[5]);
        uint8_t tmp[2 + 32];
        tmp[0] = data[2]; tmp[1] = data[3];
        memcpy(tmp + 2, data + 6, (size_t)h.side_bytes);
        if (mp3_crc16(tmp, 2 + h.side_bytes) != want)
            return AUDIO_ERR_CORRUPT;
        off = 6;
    }

    d->lsf = h.lsf; d->mpeg25 = h.mpeg25; d->sfreq = h.sfreq;
    d->rate = h.rate; d->channels = h.channels;
    d->mode = h.mode; d->mode_ext = h.mode_ext;
    d->ngr = h.lsf ? 1 : 2;

    struct granule gr[2][2];
    memset(gr, 0, sizeof(gr));
    int main_data_begin = 0, scfsi[2][4];
    rc = read_side_info(d, &h, data + off, gr, &main_data_begin, scfsi);
    if (rc != AUDIO_OK) return rc;
    off += h.side_bytes;

    long md_len = h.frame_bytes - off;
    if (md_len < 0) return AUDIO_ERR_CORRUPT;

    /* Bit reservoir. main_data_begin counts backwards from the end of the
     * main data seen so far, which is why the decoder must keep history and
     * why the first frames of a stream legitimately produce nothing. */
    if (main_data_begin > d->resv_len) {
        if (d->resv_len + md_len <= MAXRESV) {
            memcpy(d->resv + d->resv_len, data + off, (size_t)md_len);
            d->resv_len += (int)md_len;
        } else {
            int keep = 511;
            if (md_len >= MAXRESV) { d->resv_len = 0; }
            else {
                if (keep > d->resv_len) keep = d->resv_len;
                memmove(d->resv, d->resv + d->resv_len - keep, (size_t)keep);
                memcpy(d->resv + keep, data + off, (size_t)md_len);
                d->resv_len = keep + (int)md_len;
            }
        }
        return h.frame_bytes;                          /* consumed, no output yet */
    }

    /* Assemble: main_data_begin bytes of history followed by this frame's. */
    uint8_t work[MAXRESV];
    int hist = main_data_begin;
    if (hist + md_len > MAXRESV) return AUDIO_ERR_CORRUPT;
    memcpy(work, d->resv + d->resv_len - hist, (size_t)hist);
    memcpy(work + hist, data + off, (size_t)md_len);
    long work_len = hist + md_len;

    /* Update the reservoir for the next frame before decoding, so an error
     * below does not desynchronise the history. */
    {
        int keep = 511;
        int total = d->resv_len + (int)md_len;
        if (total <= MAXRESV) {
            memcpy(d->resv + d->resv_len, data + off, (size_t)md_len);
            d->resv_len = total;
        } else {
            if (keep > d->resv_len) keep = d->resv_len;
            memmove(d->resv, d->resv + d->resv_len - keep, (size_t)keep);
            memcpy(d->resv + keep, data + off, (size_t)md_len);
            d->resv_len = keep + (int)md_len;
        }
        if (d->resv_len > 1024) {
            memmove(d->resv, d->resv + d->resv_len - 1024, 1024);
            d->resv_len = 1024;
        }
    }

    abits b;
    ab_init(&b, work, work_len);

    int nch = h.channels;
    int i_stereo = (h.mode == 1) && (h.mode_ext & 1);
    int intensity_scale = h.lsf ? (h.mode_ext >> 1) & 1 : 0;
    int nsamples = 0;

    for (int g = 0; g < d->ngr; g++) {
        for (int c = 0; c < nch; c++) {
            rc = granule_spectrum(d, &b, gr, g, c, scfsi, i_stereo);
            if (rc != AUDIO_OK) return rc;
        }
        stereo(d, gr, g, intensity_scale);
        for (int c = 0; c < nch; c++) {
            antialias(d, &gr[g][c], c);
            double sb[SBLIMIT][SSLIMIT];
            hybrid(d, &gr[g][c], c, sb);
            for (int s = 0; s < SSLIMIT; s++) {
                double S[SBLIMIT];
                for (int k = 0; k < SBLIMIT; k++) S[k] = sb[k][s];
                synth(d, c, S, d->out + (nsamples + s * 32) * nch + c, nch);
            }
        }
        nsamples += SSLIMIT * 32;
    }

    out->rate = h.rate;
    out->channels = nch;
    out->nsamples = nsamples;
    out->bitrate_kbps = h.bitrate;
    out->pcm = d->out;
    *got = 1;
    return h.frame_bytes;
}
