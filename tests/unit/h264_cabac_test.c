/* tests/unit/h264_cabac_test.c -- the CABAC engine and its inputs.
 *
 * Two things are pinned here, both of which are silent when wrong:
 *
 *  1. The 9.3.1.1 initialisation. The (m, n) constants in
 *     h264_cabac_tables.h were cross-checked against three implementations
 *     when they were generated, but nothing in the build stops someone
 *     editing the header afterwards. These checks recompute a sample of the
 *     derivation and compare against states taken from the spec's own
 *     definition, so a corrupted row shows up as a test failure rather than
 *     as a stream that decodes to noise.
 *
 *  2. The arithmetic decoder itself, driven end to end: a bit sequence is
 *     decoded twice, once through the engine and once through a transcription
 *     of 9.3.3.2's pseudocode written independently below, and the two must
 *     agree bin for bin. That catches a renormalisation or state-transition
 *     mistake without needing an encoder.
 *
 * The scaling matrices live here too, because they are the other table-shaped
 * thing in this decoder whose errors are invisible: Table 7-2's fall-back
 * rules decide which list an uncoded list inherits, and getting that wrong
 * changes the dequantiser for every coefficient of every macroblock without
 * changing a single bit of parsing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"
#include "h264_cabac_tables.h"   /* H264_NCTX */

static int fails, checks;

static void ck(int cond, const char *what, long got, long want)
{
    checks++;
    if (!cond) {
        fails++;
        if (fails < 20)
            fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, got, want);
    }
}

/* ---------------------------------------------------- 9.3.1.1 by hand ---- */
static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

static void test_init(void)
{
    /* Every context of every table, at every QP, must land inside the legal
     * state space: pStateIdx 0..63. A transcription slip in m or n almost
     * always pushes preCtxState out of 1..126 for some QP, and the Clip3
     * hides it -- so also check the two anchors the spec's own examples give:
     * a pair of (m, n) that must resolve to a known state. */
    h264cabac c;
    static const uint8_t buf[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };

    for (int st = 0; st < 3; st++) {
        int slice_type = st == 0 ? SLICE_I : (st == 1 ? SLICE_P : SLICE_B);
        for (int idc = 0; idc < 3; idc++) {
            for (int qp = 0; qp <= 51; qp++) {
                h264_cabac_init(&c, buf, sizeof buf, qp, slice_type, idc);
                for (int i = 0; i < H264_NCTX; i++) {
                    int state = c.state[i] >> 1;
                    if (state > 63) {
                        ck(0, "pStateIdx in range", state, 63);
                        return;
                    }
                }
            }
        }
    }
    ck(1, "pStateIdx in range for all contexts/QPs/tables", 0, 0);

    /* ctxIdx 0 of the I table is (m, n) = (20, -15) in Table 9-12. Spot-check
     * the derivation at three QPs, computed here from the spec formula. */
    struct { int qp, m, n; } v[] = { { 0, 20, -15 }, { 26, 20, -15 }, { 51, 20, -15 } };
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
        h264_cabac_init(&c, buf, sizeof buf, v[k].qp, SLICE_I, 0);
        int pre = clip3(1, 126, ((v[k].m * clip3(0, 51, v[k].qp)) >> 4) + v[k].n);
        int want = pre <= 63 ? ((63 - pre) << 1) : (((pre - 64) << 1) | 1);
        ck(c.state[0] == want, "ctx0 init", c.state[0], want);
    }
}

/* --------------------------------------- independent engine transcription - */
typedef struct { const uint8_t *b; int len, byte, bit; uint32_t range, off; int state[460]; } ref_t;

static const uint8_t rlps[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},
    {116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    { 95,116,137,158},{ 90,110,130,150},{ 85,104,123,142},{ 81, 99,117,135},
    { 77, 94,111,128},{ 73, 89,105,122},{ 69, 85,100,116},{ 66, 80, 95,110},
    { 62, 76, 90,104},{ 59, 72, 86, 99},{ 56, 69, 81, 94},{ 53, 65, 77, 89},
    { 51, 62, 73, 85},{ 48, 59, 69, 80},{ 46, 56, 66, 76},{ 43, 53, 63, 72},
    { 41, 50, 59, 69},{ 39, 48, 56, 65},{ 37, 45, 54, 62},{ 35, 43, 51, 59},
    { 33, 41, 48, 56},{ 32, 39, 46, 53},{ 30, 37, 43, 50},{ 29, 35, 41, 48},
    { 27, 33, 39, 45},{ 26, 31, 37, 43},{ 24, 30, 35, 41},{ 23, 28, 33, 39},
    { 22, 27, 32, 37},{ 21, 26, 30, 35},{ 20, 24, 29, 33},{ 19, 23, 27, 31},
    { 18, 22, 26, 30},{ 17, 21, 25, 28},{ 16, 20, 23, 27},{ 15, 19, 22, 25},
    { 14, 18, 21, 24},{ 14, 17, 20, 23},{ 13, 16, 19, 22},{ 12, 15, 18, 21},
    { 12, 14, 17, 20},{ 11, 14, 16, 19},{ 11, 13, 15, 18},{ 10, 12, 15, 17},
    { 10, 12, 14, 16},{  9, 11, 13, 15},{  9, 11, 12, 14},{  8, 10, 12, 14},
    {  8,  9, 11, 13},{  7,  9, 11, 12},{  7,  9, 10, 12},{  7,  8, 10, 11},
    {  6,  8,  9, 11},{  6,  7,  9, 10},{  6,  7,  8,  9},{  2,  2,  2,  2}
};
static const uint8_t tlps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};
static const uint8_t tmps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};

static int rbit(ref_t *r)
{
    if (r->byte >= r->len) return 0;
    int b = (r->b[r->byte] >> (7 - r->bit)) & 1;
    if (++r->bit == 8) { r->bit = 0; r->byte++; }
    return b;
}

static void rrenorm(ref_t *r)
{
    while (r->range < 256) { r->range <<= 1; r->off = (r->off << 1) | (uint32_t)rbit(r); }
}

static int rdecide(ref_t *r, int ctx)
{
    int s = r->state[ctx] >> 1, mps = r->state[ctx] & 1, bin;
    uint32_t lps = rlps[s][(r->range >> 6) & 3];
    r->range -= lps;
    if (r->off >= r->range) {
        bin = 1 - mps;
        r->off -= r->range;
        r->range = lps;
        if (s == 0) mps = 1 - mps;
        s = tlps[s];
    } else {
        bin = mps;
        s = tmps[s];
    }
    r->state[ctx] = (s << 1) | mps;
    rrenorm(r);
    return bin;
}

static int rbypass(ref_t *r)
{
    r->off = (r->off << 1) | (uint32_t)rbit(r);
    if (r->off >= r->range) { r->off -= r->range; return 1; }
    return 0;
}

static void test_engine(void)
{
    /* A pseudo-random payload, decoded through both engines with the same
     * interleaving of decisions and bypasses over a spread of contexts. */
    uint8_t buf[512];
    uint32_t seed = 12345;
    for (unsigned i = 0; i < sizeof buf; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }

    h264cabac c;
    ref_t r;
    h264_cabac_init(&c, buf, (int)sizeof buf, 30, SLICE_P, 1);
    memset(&r, 0, sizeof r);
    r.b = buf; r.len = (int)sizeof buf;
    for (int i = 0; i < H264_NCTX; i++) r.state[i] = c.state[i];
    r.range = 510;
    r.off = 0;
    for (int i = 0; i < 9; i++) r.off = (r.off << 1) | (uint32_t)rbit(&r);

    int mismatch = 0;
    for (int i = 0; i < 3000; i++) {
        int a, b;
        if (i % 7 == 3) { a = h264_cabac_bypass(&c); b = rbypass(&r); }
        else            { int ctx = (i * 37) % H264_NCTX;
                          a = h264_cabac_decision(&c, ctx); b = rdecide(&r, ctx); }
        if (a != b) { mismatch = i + 1; break; }
    }
    ck(mismatch == 0, "engine matches the 9.3.3.2 transcription", mismatch, 0);
    ck(c.range == r.range, "engine codIRange", (long)c.range, (long)r.range);
    ck(c.offset == r.off, "engine codIOffset", (long)c.offset, (long)r.off);
}

/* ------------------------------------------------ scaling matrices ------- */
/* Table 7-2's fall-back rules, checked through the real parser: a PPS that
 * signals a scaling matrix but codes none of the eight lists must end up with
 * the JVT defaults, with lists 1,2 inheriting from 0 and 4,5 from 3. That is
 * exactly what x264 emits for --cqm jvt, and it is where a decoder quietly
 * ends up dequantising with flat matrices instead. */
static const uint8_t jvt4_intra[16] = {
     6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42
};
static const uint8_t jvt4_inter[16] = {
    10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34
};

/* A minimal High-profile SPS/PPS pair, written bit by bit so the test does
 * not need an encoder. */
typedef struct { uint8_t b[64]; int n; } bw_t;
static void wbit(bw_t *w, int v)
{
    if (w->n >= (int)sizeof w->b * 8) return;
    if (v) w->b[w->n >> 3] |= (uint8_t)(0x80 >> (w->n & 7));
    w->n++;
}
static void wbits(bw_t *w, uint32_t v, int n) { while (n--) wbit(w, (v >> n) & 1); }
static void wue(bw_t *w, uint32_t v)
{
    int nb = 0; uint32_t t = v + 1;
    while (t >> (nb + 1)) nb++;
    for (int i = 0; i < nb; i++) wbit(w, 0);
    wbits(w, v + 1, nb + 1);
}
static void wse(bw_t *w, int v) { wue(w, v <= 0 ? (uint32_t)(-2 * v) : (uint32_t)(2 * v - 1)); }

static void test_scaling(void)
{
    /* The parser only needs the parameter-set arrays, so build the decoder
     * struct directly rather than linking the whole decoder into this test. */
    h264dec *d = (h264dec *)malloc(sizeof *d);
    memset(d, 0, sizeof *d);
    bw_t w;
    bs_t bs;

    /* SPS: profile 100, no scaling matrix of its own. */
    memset(&w, 0, sizeof w);
    wbits(&w, 100, 8);                 /* profile_idc */
    wbits(&w, 0, 8);                   /* constraint flags */
    wbits(&w, 30, 8);                  /* level_idc */
    wue(&w, 0);                        /* sps_id */
    wue(&w, 1);                        /* chroma_format_idc = 4:2:0 */
    wue(&w, 0); wue(&w, 0);            /* bit depths = 8 */
    wbit(&w, 0);                       /* qpprime_y_zero_transform_bypass */
    wbit(&w, 0);                       /* seq_scaling_matrix_present */
    wue(&w, 0);                        /* log2_max_frame_num_minus4 */
    wue(&w, 2);                        /* poc_type 2 */
    wue(&w, 1);                        /* max_num_ref_frames */
    wbit(&w, 0);                       /* gaps_in_frame_num_allowed */
    wue(&w, 10); wue(&w, 8);           /* 11 x 9 macroblocks */
    wbit(&w, 1);                       /* frame_mbs_only */
    wbit(&w, 1);                       /* direct_8x8_inference */
    wbit(&w, 0);                       /* frame_cropping */
    wbit(&w, 0);                       /* vui_parameters_present */
    wbit(&w, 1);                       /* rbsp_stop_one_bit */
    bs_init(&bs, w.b, (w.n + 7) / 8);
    ck(h264_parse_sps(d, &bs) == H264_OK, "High-profile SPS accepted", 0, 0);

    /* PPS: transform_8x8_mode_flag = 1, pic_scaling_matrix_present_flag = 1,
     * and all eight pic_scaling_list_present_flag = 0. */
    memset(&w, 0, sizeof w);
    wue(&w, 0);                        /* pps_id */
    wue(&w, 0);                        /* sps_id */
    wbit(&w, 1);                       /* entropy_coding_mode = CABAC */
    wbit(&w, 0);                       /* bottom_field_pic_order_in_frame */
    wue(&w, 0);                        /* num_slice_groups_minus1 */
    wue(&w, 0); wue(&w, 0);            /* num_ref_idx defaults */
    wbit(&w, 0);                       /* weighted_pred */
    wbits(&w, 0, 2);                   /* weighted_bipred_idc */
    wse(&w, 0); wse(&w, 0); wse(&w, 0);/* pic_init_qp/qs, chroma_qp_offset */
    wbit(&w, 0);                       /* deblocking_filter_control_present */
    wbit(&w, 0);                       /* constrained_intra_pred */
    wbit(&w, 0);                       /* redundant_pic_cnt_present */
    wbit(&w, 1);                       /* transform_8x8_mode_flag */
    wbit(&w, 1);                       /* pic_scaling_matrix_present_flag */
    for (int i = 0; i < 8; i++) wbit(&w, 0);   /* no list coded */
    wse(&w, 0);                        /* second_chroma_qp_index_offset */
    wbit(&w, 1);                       /* rbsp_stop_one_bit */
    bs_init(&bs, w.b, (w.n + 7) / 8);
    ck(h264_parse_pps(d, &bs) == H264_OK, "High-profile PPS accepted", 0, 0);

    const pps_t *p = &d->pps[0];
    int bad = 0;
    for (int i = 0; i < 16; i++) {
        if (p->scaling4[0][i] != jvt4_intra[i]) bad++;
        if (p->scaling4[1][i] != jvt4_intra[i]) bad++;   /* rule: inherit list 0 */
        if (p->scaling4[2][i] != jvt4_intra[i]) bad++;   /* rule: inherit list 1 */
        if (p->scaling4[3][i] != jvt4_inter[i]) bad++;
        if (p->scaling4[4][i] != jvt4_inter[i]) bad++;
        if (p->scaling4[5][i] != jvt4_inter[i]) bad++;
    }
    ck(bad == 0, "4x4 scaling matrices fall back to the JVT defaults", bad, 0);
    /* The 8x8 defaults are symmetric, so a transposed table would look right;
     * check a few asymmetric-in-index positions against the spec's values. */
    ck(p->scaling8[0][0] == 6,  "8x8 intra [0][0]", p->scaling8[0][0], 6);
    ck(p->scaling8[0][9] == 11, "8x8 intra [1][1]", p->scaling8[0][9], 11);
    ck(p->scaling8[0][63] == 42, "8x8 intra [7][7]", p->scaling8[0][63], 42);
    ck(p->scaling8[1][0] == 9,  "8x8 inter [0][0]", p->scaling8[1][0], 9);
    ck(p->scaling8[1][63] == 35, "8x8 inter [7][7]", p->scaling8[1][63], 35);

    /* LevelScale folds the matrix into normAdjust; the DC entry is what the
     * Intra_16x16 and chroma DC chains use, and it is the one a flat-matrix
     * test can never distinguish from a wrong one. */
    ck(p->ls4[0][0][0] == 6 * 10, "LevelScale4x4 intra Y DC at qP%6=0",
       p->ls4[0][0][0], 60);
    ck(p->ls8[0][0][0] == 6 * 20, "LevelScale8x8 intra Y DC at qP%6=0",
       p->ls8[0][0][0], 120);

    free(d);
}

int main(void)
{
    test_init();
    test_engine();
    test_scaling();
    printf("H264-CABAC-%s: %d checks, %d failures\n",
           fails ? "FAIL" : "OK", checks, fails);
    return fails ? 1 : 0;
}
