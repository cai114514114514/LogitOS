/* c/lib/video/h264_cabac.c -- the H.264 arithmetic decoder, its context model
 * and residual_block_cabac.
 *
 * Baseline used CAVLC, so this is the piece that had to exist before a real
 * web video could be decoded at all: Main and High both mandate CABAC, and
 * every H.264 stream a browser is asked to play is High. The engine is the
 * spec's own (9.3.3.2), one bit of renormalisation at a time, rather than a
 * table-driven rewrite -- every step is checkable against the text, and this
 * decoder is graded on being bit-exact, not on being quick.
 *
 * H.265's engine next door has the same SHAPE and even shares the three state
 * tables, which is why it was worth reading. Nothing else transfers: the
 * context count, the initialisation constants, the ctxIdx assignment and the
 * binarisation of every syntax element are all different, and the residual
 * coding is a completely different algorithm (H.264 scans a whole block and
 * codes significance forwards, then levels backwards; H.265 works in 4x4
 * sub-blocks with four passes).
 *
 * The MACROBLOCK-layer syntax lives in h264.c, not here, because choosing a
 * ctxIdx is almost always a question about neighbouring macroblocks and that
 * state belongs to the orchestrator. This file takes a ctxIdx and returns a
 * bin; the one exception is residual_block_cabac, whose context bookkeeping is
 * internal to the block and would only get harder to read spread over a
 * function-call boundary.
 *
 * Every read past the end of the slice returns zero bits and counts an
 * overrun; 64 of those set the sticky error, which h264.c checks at
 * macroblock boundaries. Nothing here reads or writes outside its buffers.
 */
#include <string.h>
#include "h264.h"
#include "h264_int.h"
#include "h264_cabac_tables.h"

/* ---------------------------------------------------------------- tables -- */
/* Table 9-44: rangeTabLPS[pStateIdx][qCodIRangeIdx]. */
static const uint8_t range_tab_lps[64][4] = {
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

/* Table 9-45: transIdxLPS / transIdxMPS. */
static const uint8_t trans_idx_lps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};
static const uint8_t trans_idx_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};

/* Residual context bases (Table 9-11), frame-coded 4:2:0. ctxBlockCat 0..5:
 * 0 = Intra16x16 luma DC, 1 = Intra16x16 luma AC, 2 = luma 4x4,
 * 3 = chroma DC, 4 = chroma AC, 5 = luma 8x8. */
static const uint16_t cbf_base[6]  = {  85,  89,  93,  97, 101,    0 };
static const uint16_t sig_base[6]  = { 105, 120, 134, 149, 152,  402 };
static const uint16_t last_base[6] = { 166, 181, 195, 210, 213,  417 };
static const uint16_t abs_base[6]  = { 227, 237, 247, 257, 266,  426 };

/* Table 9-43: ctxIdxInc for significant_coeff_flag / last_significant_coeff_flag
 * in an 8x8 block. Unlike the 4x4 categories, where ctxIdxInc is simply the
 * scan position, the 8x8 block folds 63 positions onto 15 and 9 contexts. */
static const uint8_t sig_off_8x8[63] = {
    0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
    4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9,10, 9, 8, 7,
    7, 6,11,12,13,11, 6, 7, 8, 9,14,10, 9, 8, 6,11,
   12,13,11, 6, 9,14,10, 9,11,12,13,11,14,10,12
};
static const uint8_t last_off_8x8[63] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8
};

/* 9.3.3.1.3, coeff_abs_level_minus1. The node context is a compressed memory
 * of how many levels equal to 1 and how many greater than 1 have been decoded
 * so far in this block, walked backwards from the last significant
 * coefficient. 0..3 means "no level > 1 yet"; 4..7 means at least one. */
static const uint8_t abs_level1_ctx[8]     = { 1, 2, 3, 4, 0, 0, 0, 0 };
static const uint8_t abs_levelgt1_ctx[8]   = { 5, 5, 5, 5, 6, 7, 8, 9 };
/* Chroma DC caps the greater-than-1 context one lower (the "4 - (cat == 3)"
 * of 9.3.3.1.3), because its blocks only have four coefficients. */
static const uint8_t abs_levelgt1_ctx_dc[8]= { 5, 5, 5, 5, 6, 7, 8, 8 };
static const uint8_t abs_level_trans[2][8] = {
    { 1, 2, 3, 3, 4, 5, 6, 7 },      /* after a level == 1 */
    { 4, 4, 4, 4, 5, 6, 7, 7 }       /* after a level > 1 */
};

/* ============================================================== engine === */
static int cabac_bit(h264cabac *c)
{
    if (c->bytepos >= c->len) {
        if (++c->overrun > 64) c->error = 1;
        return 0;
    }
    int b = (c->buf[c->bytepos] >> (7 - c->bitpos)) & 1;
    if (++c->bitpos == 8) { c->bitpos = 0; c->bytepos++; }
    return b;
}

static void renorm(h264cabac *c)
{
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    }
}

static int clip3i(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

void h264_cabac_init(h264cabac *c, const uint8_t *buf, int len,
                     int slice_qp, int slice_type, int cabac_init_idc)
{
    memset(c, 0, sizeof *c);
    c->buf = buf;
    c->len = len < 0 ? 0 : len;

    /* 9.3.1.1: (m, n) -> preCtxState -> (pStateIdx, valMPS), stored packed. */
    const int8_t (*tab)[2] = (slice_type == SLICE_I)
        ? h264_cabac_init_I
        : h264_cabac_init_PB[clip3i(0, 2, cabac_init_idc)];
    int qp = clip3i(0, 51, slice_qp);
    for (int i = 0; i < H264_NCTX; i++) {
        int pre = clip3i(1, 126, ((tab[i][0] * qp) >> 4) + tab[i][1]);
        c->state[i] = (uint8_t)(pre <= 63 ? ((63 - pre) << 1)
                                          : (((pre - 64) << 1) | 1));
    }

    /* 9.3.1.2: the engine starts with a 9-bit window. The caller has already
     * consumed cabac_alignment_one_bit and handed us a byte-aligned buffer. */
    c->range = 510;
    c->offset = 0;
    for (int i = 0; i < 9; i++)
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
}

/* Restart the arithmetic decoder at a byte position, keeping the context
 * model. This is the I_PCM case of 9.3.1.2: the samples are read straight out
 * of the bitstream and the engine picks up after them. */
void h264_cabac_restart(h264cabac *c, int bytepos)
{
    c->bytepos = bytepos < 0 ? 0 : bytepos;
    c->bitpos = 0;
    c->range = 510;
    c->offset = 0;
    for (int i = 0; i < 9; i++)
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
}

int h264_cabac_decision(h264cabac *c, int ctx_idx)
{
    if (ctx_idx < 0 || ctx_idx >= H264_NCTX) { c->error = 1; return 0; }
    uint8_t s = c->state[ctx_idx];
    int state = s >> 1, mps = s & 1;
    uint32_t lps = range_tab_lps[state][(c->range >> 6) & 3];
    int bin;

    c->range -= lps;
    if (c->offset >= c->range) {
        bin = !mps;
        c->offset -= c->range;
        c->range = lps;
        if (state == 0) mps = !mps;
        state = trans_idx_lps[state];
    } else {
        bin = mps;
        state = trans_idx_mps[state];
    }
    c->state[ctx_idx] = (uint8_t)((state << 1) | mps);
    renorm(c);
    return bin;
}

int h264_cabac_bypass(h264cabac *c)
{
    c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    if (c->offset >= c->range) { c->offset -= c->range; return 1; }
    return 0;
}

int h264_cabac_terminate(h264cabac *c)
{
    c->range -= 2;
    if (c->offset >= c->range) return 1;      /* no renormalisation: we are done */
    renorm(c);
    return 0;
}

/* UEGk (9.3.2.3): a truncated-unary prefix of at most u_coff bins over the
 * contexts [ctx, ctx + ctx_inc_max], then an Exp-Golomb order-k bypass suffix,
 * then an optional bypass sign. Used for mvd (k = 3, u_coff = 9, signed) --
 * coeff_abs_level_minus1 has its own context walk and is coded inline below. */
int h264_cabac_ueg(h264cabac *c, int ctx, int ctx_inc_max, int k,
                   int u_coff, int sign)
{
    int val = 0;
    while (val < u_coff &&
           h264_cabac_decision(c, ctx + (val < ctx_inc_max ? val : ctx_inc_max)))
        val++;
    if (val == u_coff) {
        /* Exp-Golomb order k, bypass coded. The exponent is bounded so a
         * corrupt stream cannot spin here. */
        int nbits = 0;
        while (h264_cabac_bypass(c) && nbits < 24) { val += 1 << k; k++; nbits++; }
        while (k--) val += h264_cabac_bypass(c) << k;
    }
    if (sign && val && h264_cabac_bypass(c)) val = -val;
    return val;
}

/* ==================================================== residual_block ===== */
int h264_cabac_residual(h264cabac *c, int cat, int max_coeff, int cbf_inc,
                        int coef[64])
{
    if (cat < 0 || cat > 5 || max_coeff < 1 || max_coeff > 64) {
        c->error = 1;
        return -1;
    }
    for (int i = 0; i < max_coeff; i++) coef[i] = 0;

    /* coded_block_flag (7.3.5.3.3). A 4:2:0 8x8 luma block does not carry one
     * -- the coded_block_pattern bit already said the block is coded -- and
     * the caller signals that with cbf_inc < 0. */
    if (cbf_inc >= 0) {
        if (!h264_cabac_decision(c, cbf_base[cat] + cbf_inc)) return 0;
    }

    /* significance map: forwards over the scan, stopping at the last
     * significant coefficient. The final position needs no flag -- if the
     * scan reaches it without a "last", it is significant by construction. */
    int index[64], n = 0;
    int is8 = (max_coeff == 64);
    int sbase = sig_base[cat], lbase = last_base[cat];
    for (int i = 0; i < max_coeff - 1; i++) {
        int sinc = is8 ? sig_off_8x8[i] : i;
        if (h264_cabac_decision(c, sbase + sinc)) {
            index[n++] = i;
            int linc = is8 ? last_off_8x8[i] : i;
            if (h264_cabac_decision(c, lbase + linc)) goto levels;
        }
        if (c->error) return -1;
    }
    index[n++] = max_coeff - 1;

levels:
    if (n < 1 || n > max_coeff) { c->error = 1; return -1; }
    {
        /* Levels are coded BACKWARDS, from the last significant coefficient to
         * the first, and the context walk depends on that order: the run of
         * trailing +-1s is what the "number of decoded levels equal to 1"
         * counter is measuring. */
        const uint8_t *gt1 = (cat == 3) ? abs_levelgt1_ctx_dc : abs_levelgt1_ctx;
        int abase = abs_base[cat], node = 0;
        for (int i = n - 1; i >= 0; i--) {
            int level;
            if (!h264_cabac_decision(c, abase + abs_level1_ctx[node])) {
                level = 1;
                node = abs_level_trans[0][node];
            } else {
                level = 2;
                int ctx = abase + gt1[node];
                node = abs_level_trans[1][node];
                while (level < 15 && h264_cabac_decision(c, ctx)) level++;
                if (level >= 15) {
                    /* UEG0 suffix, uCoff = 14. The prefix length is bounded so
                     * corrupt input cannot make this loop forever or overflow. */
                    int j = 0;
                    while (h264_cabac_bypass(c) && j < 24) j++;
                    level = 1;
                    while (j--) level += level + h264_cabac_bypass(c);
                    level += 14;
                }
            }
            if (h264_cabac_bypass(c)) level = -level;
            /* index[] is a SCAN position, and coefficients are handed back in
             * scan order -- the same convention cavlc_decode uses, so the
             * transforms un-scan and do not care which entropy coder produced
             * the block. For an 8x8 block that scan is the 8x8 zigzag, which is
             * not the 4x4 one applied four times. */
            coef[index[i]] = level;
            if (c->error) return -1;
        }
    }
    return n;
}
