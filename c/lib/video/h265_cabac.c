/* c/lib/video/h265_cabac.c -- the HEVC arithmetic decoder, its context model,
 * and residual_coding().
 *
 * H.264 baseline used CAVLC, so none of this existed next door: CABAC is
 * entirely new work. The engine is the spec's own (9.3.4.3) rather than a
 * table-driven rewrite -- DecodeDecision, DecodeBypass and DecodeTerminate,
 * one bit of renormalisation at a time. It is not the fastest way to do it,
 * but every step is checkable against the text, and this decoder is graded on
 * being bit-exact, not on being quick.
 *
 * residual_coding (7.3.8.11) lives here too. It is 90% entropy decoding: the
 * last-significant-coefficient position, the 4x4 sub-block scan, and four
 * passes of flags per sub-block whose contexts depend on what the previous
 * pass and the previous sub-block did. Splitting it from the engine would put
 * the context bookkeeping on the far side of a function-call boundary for no
 * benefit.
 *
 * Every read past the end of the substream returns zero bits and increments
 * `overrun`; 64 bits of that sets the sticky error. Callers check at slice
 * boundaries. Nothing here reads or writes outside its buffers.
 */
#include <string.h>
#include "h265.h"
#include "h265_int.h"

/* --------------------------------------------------------------- tables -- */
#define CNU 154

/* Table 9-5..9-37, flattened into the CTX_* layout of h265_int.h.
 * Row 0 = initType 0 (I), row 1 = initType 1, row 2 = initType 2. */
static const uint8_t init_values[3][H265_NCTX] = {
{
    /* sao_merge */            153,
    /* sao_type_idx */         200,
    /* split_cu_flag */        139, 141, 157,
    /* cu_transquant_bypass */ 154,
    /* cu_skip_flag */         CNU, CNU, CNU,
    /* pred_mode_flag */       CNU,
    /* part_mode */            184, CNU, CNU, CNU,
    /* prev_intra_luma */      184,
    /* intra_chroma_pred */    63,
    /* rqt_root_cbf */         CNU,
    /* merge_flag */           CNU,
    /* merge_idx */            CNU,
    /* inter_pred_idc */       CNU, CNU, CNU, CNU, CNU,
    /* ref_idx */              CNU, CNU,
    /* mvp_flag */             CNU,
    /* abs_mvd_greater0 */     CNU,
    /* abs_mvd_greater1 */     CNU,
    /* split_transform */      153, 138, 138,
    /* cbf_luma */             111, 141,
    /* cbf_chroma */           94, 138, 182, 154,
    /* transform_skip */       139, 139,
    /* cu_qp_delta_abs */      154, 154, 154,
    /* last_sig_x_prefix */    110, 110, 124, 125, 140, 153, 125, 127, 140,
                               109, 111, 143, 127, 111,  79, 108, 123,  63,
    /* last_sig_y_prefix */    110, 110, 124, 125, 140, 153, 125, 127, 140,
                               109, 111, 143, 127, 111,  79, 108, 123,  63,
    /* coded_sub_block */      91, 171, 134, 141,
    /* sig_coeff_flag */       111, 111, 125, 110, 110,  94, 124, 108, 124,
                               107, 125, 141, 179, 153, 125, 107, 125, 141,
                               179, 153, 125, 107, 125, 141, 179, 153, 125,
                               140, 139, 182, 182, 152, 136, 152, 136, 153,
                               136, 139, 111, 136, 139, 111,
    /* greater1 */             140,  92, 137, 138, 140, 152, 138, 139, 153,
                                74, 149,  92, 139, 107, 122, 152, 140, 179,
                               166, 182, 140, 227, 122, 197,
    /* greater2 */             138, 153, 136, 167, 152, 152
},
{
    153,
    185,
    107, 139, 126,
    154,
    197, 185, 201,
    149,
    154, 139, 154, 154,
    154,
    152,
    79,
    110,
    122,
    95, 79, 63, 31, 31,
    153, 153,
    168,
    140,
    198,
    124, 138, 94,
    153, 111,
    149, 107, 167, 154,
    139, 139,
    154, 154, 154,
    125, 110,  94, 110,  95,  79, 125, 111, 110,
     78, 110, 111, 111,  95,  94, 108, 123, 108,
    125, 110,  94, 110,  95,  79, 125, 111, 110,
     78, 110, 111, 111,  95,  94, 108, 123, 108,
    121, 140, 61, 154,
    155, 154, 139, 153, 139, 123, 123,  63, 153,
    166, 183, 140, 136, 153, 154, 166, 183, 140,
    136, 153, 154, 166, 183, 140, 136, 153, 154,
    170, 153, 123, 123, 107, 121, 107, 121, 167,
    151, 183, 140, 151, 183, 140,
    154, 196, 196, 167, 154, 152, 167, 182, 182,
    134, 149, 136, 153, 121, 136, 137, 169, 194,
    166, 167, 154, 167, 137, 182,
    107, 167,  91, 122, 107, 167
},
{
    153,
    160,
    107, 139, 126,
    154,
    197, 185, 201,
    134,
    154, 139, 154, 154,
    183,
    152,
    79,
    154,
    137,
    95, 79, 63, 31, 31,
    153, 153,
    168,
    169,
    198,
    224, 167, 122,
    153, 111,
    149,  92, 167, 154,
    139, 139,
    154, 154, 154,
    125, 110, 124, 110,  95,  94, 125, 111, 111,
     79, 125, 126, 111, 111,  79, 108, 123,  93,
    125, 110, 124, 110,  95,  94, 125, 111, 111,
     79, 125, 126, 111, 111,  79, 108, 123,  93,
    121, 140, 61, 154,
    170, 154, 139, 153, 139, 123, 123,  63, 124,
    166, 183, 140, 136, 153, 154, 166, 183, 140,
    136, 153, 154, 166, 183, 140, 136, 153, 154,
    170, 153, 138, 138, 122, 121, 122, 121, 167,
    151, 183, 140, 151, 183, 140,
    154, 196, 167, 167, 154, 152, 167, 182, 182,
    134, 149, 136, 153, 121, 136, 137, 169, 194,
    166, 167, 154, 167, 137, 182,
    107, 167,  91, 107, 107, 167
}
};

/* Table 9-52: rangeTabLps[pStateIdx][qRangeIdx]. */
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

/* ============================================================== engine === */
static int cabac_bit(cabac_t *c)
{
    if (c->bytepos >= c->len) {
        if (++c->overrun > 64) c->error = 1;
        return 0;
    }
    int b = (c->buf[c->bytepos] >> (7 - c->bitpos)) & 1;
    if (++c->bitpos == 8) { c->bitpos = 0; c->bytepos++; }
    return b;
}

static void renorm(cabac_t *c)
{
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    }
}

void h265_cabac_init_ctx(cabac_t *c, int init_type, int qp)
{
    int q = h265_clip3(0, 51, qp);
    const uint8_t *iv = init_values[init_type];
    for (int i = 0; i < H265_NCTX; i++) {
        int m = (iv[i] >> 4) * 5 - 45;
        int n = ((iv[i] & 15) << 3) - 16;
        int pre = h265_clip3(1, 126, ((m * q) >> 4) + n);
        int mps = pre <= 63 ? 0 : 1;
        int state = mps ? pre - 64 : 63 - pre;
        c->state[i] = (uint8_t)((state << 1) | mps);
    }
}

int h265_cabac_start(cabac_t *c, const uint8_t *buf, int len, int bytepos)
{
    if (bytepos < 0 || bytepos > len) return H265_ERR_CORRUPT;
    c->buf = buf; c->len = len;
    c->bytepos = bytepos; c->bitpos = 0;
    c->overrun = 0; c->error = 0;
    c->range = 510;
    c->offset = 0;
    for (int i = 0; i < 9; i++)
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    /* ivlOffset must be < ivlCurrRange; 510/511 would decode nothing sane. */
    if (c->offset >= 510) return H265_ERR_CORRUPT;
    return H265_OK;
}

int h265_cabac_decision(cabac_t *c, int ctx)
{
    uint8_t s = c->state[ctx];
    int state = s >> 1, mps = s & 1;
    uint32_t lps = range_tab_lps[state][(c->range >> 6) & 3];
    int bin;

    c->range -= lps;
    if (c->offset >= c->range) {
        bin = 1 - mps;
        c->offset -= c->range;
        c->range = lps;
        if (state == 0) mps = 1 - mps;
        state = trans_idx_lps[state];
    } else {
        bin = mps;
        state = trans_idx_mps[state];
    }
    c->state[ctx] = (uint8_t)((state << 1) | mps);
    renorm(c);
    return bin;
}

int h265_cabac_bypass(cabac_t *c)
{
    c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    if (c->offset >= c->range) { c->offset -= c->range; return 1; }
    return 0;
}

uint32_t h265_cabac_bypass_n(cabac_t *c, int n)
{
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | (uint32_t)h265_cabac_bypass(c);
    return v;
}

int h265_cabac_terminate(cabac_t *c)
{
    c->range -= 2;
    if (c->offset >= c->range) return 1;
    renorm(c);
    return 0;
}

int h265_cabac_pos(const cabac_t *c)
{
    return c->bytepos + (c->bitpos ? 1 : 0);
}

/* ========================================================= scan orders === */
/* ScanOrder[log2BlockSize][scanIdx][sPos][sComp], built once (6.5.3/4/5).
 * Only 4x4 (for coefficients within a sub-block) and the sub-block grids up
 * to 8x8 (a 32x32 TB has 8x8 sub-blocks) are needed. */
static uint8_t scan_x[4][3][64], scan_y[4][3][64];
static int scan_built;

static void build_scans(void)
{
    for (int l = 0; l < 4; l++) {
        int n = 1 << l;
        /* 6.5.3 up-right diagonal */
        {
            int i = 0, x = 0, y = 0, stop = 0;
            while (!stop) {
                while (y >= 0) {
                    if (x < n && y < n) {
                        scan_x[l][0][i] = (uint8_t)x;
                        scan_y[l][0][i] = (uint8_t)y;
                        i++;
                    }
                    y--; x++;
                }
                y = x; x = 0;
                if (i >= n * n) stop = 1;
            }
        }
        /* 6.5.4 horizontal */
        {
            int i = 0;
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    scan_x[l][1][i] = (uint8_t)x;
                    scan_y[l][1][i] = (uint8_t)y;
                    i++;
                }
        }
        /* 6.5.5 vertical */
        {
            int i = 0;
            for (int x = 0; x < n; x++)
                for (int y = 0; y < n; y++) {
                    scan_x[l][2][i] = (uint8_t)x;
                    scan_y[l][2][i] = (uint8_t)y;
                    i++;
                }
        }
    }
    scan_built = 1;
}

/* 9.3.4.2.5 ctxIdxMap for 4x4 blocks. */
static const uint8_t sig_ctx_map4[16] = {
    0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8
};

/* ==================================================== residual_coding ==== */
/* Decodes one transform block's coefficients into `out` (raster order inside
 * the block, zero-filled). Returns the number of significant coefficients, or
 * < 0 on corrupt input.
 *
 * `pred_mode_intra` is IntraPredModeY/C of the covering block (only consulted
 * for the mode-dependent scan of 4x4 and 8x8 intra blocks); `bypass` is
 * cu_transquant_bypass_flag; `*ts` receives transform_skip_flag. */
int h265_residual_coding(h265dec *d, int log2_size, int c_idx,
                         int pred_mode_intra, int is_intra, int bypass,
                         int16_t *out, int *ts)
{
    cabac_t *c = &d->cabac;
    const pps_t *pps = d->cur_pps;
    const sps_t *sps = d->cur_sps;
    int n = 1 << log2_size;

    if (!scan_built) build_scans();
    memset(out, 0, (size_t)n * n * sizeof(int16_t));
    *ts = 0;

    if (pps->transform_skip_enabled && !bypass && log2_size == 2)
        *ts = h265_cabac_decision(c, CTX_TRANSFORM_SKIP + (c_idx ? 1 : 0));

    /* --- last significant coefficient position (7.3.8.11 + 9.3.4.2.3) --- */
    int ctx_off, ctx_shift;
    if (c_idx == 0) {
        ctx_off = 3 * (log2_size - 2) + ((log2_size - 1) >> 2);
        ctx_shift = (log2_size + 1) >> 2;
    } else {
        ctx_off = 15;
        ctx_shift = log2_size - 2;
    }
    int cmax = (log2_size << 1) - 1;
    int last_x = 0, last_y = 0;
    while (last_x < cmax &&
           h265_cabac_decision(c, CTX_LAST_X + ctx_off + (last_x >> ctx_shift)))
        last_x++;
    while (last_y < cmax &&
           h265_cabac_decision(c, CTX_LAST_Y + ctx_off + (last_y >> ctx_shift)))
        last_y++;
    if (last_x > 3) {
        int nb = (last_x >> 1) - 1;
        int suf = (int)h265_cabac_bypass_n(c, nb);
        last_x = (1 << nb) * (2 + (last_x & 1)) + suf;
    }
    if (last_y > 3) {
        int nb = (last_y >> 1) - 1;
        int suf = (int)h265_cabac_bypass_n(c, nb);
        last_y = (1 << nb) * (2 + (last_y & 1)) + suf;
    }

    /* --- scan selection (7.4.9.11) --- */
    int scan_idx = 0;
    if (is_intra && (log2_size == 2 || (log2_size == 3 && c_idx == 0))) {
        if (pred_mode_intra >= 6 && pred_mode_intra <= 14) scan_idx = 2;
        else if (pred_mode_intra >= 22 && pred_mode_intra <= 30) scan_idx = 1;
    }
    if (scan_idx == 2) { int t = last_x; last_x = last_y; last_y = t; }
    if (last_x >= n || last_y >= n) { c->error = 1; return H265_ERR_CORRUPT; }

    /* --- locate the last sub-block and scan position --- */
    int sb_log2 = log2_size - 2;              /* sub-blocks per side, log2 */
    int nsb = 1 << (sb_log2 * 2);
    int last_sb = nsb - 1, last_pos = 16;
    for (;;) {
        if (last_pos == 0) { last_pos = 16; last_sb--; }
        last_pos--;
        if (last_sb < 0) { c->error = 1; return H265_ERR_CORRUPT; }
        int xs = scan_x[sb_log2][scan_idx][last_sb];
        int ys = scan_y[sb_log2][scan_idx][last_sb];
        int xc = (xs << 2) + scan_x[2][scan_idx][last_pos];
        int yc = (ys << 2) + scan_y[2][scan_idx][last_pos];
        if (xc == last_x && yc == last_y) break;
    }

    uint8_t csbf[64];
    memset(csbf, 0, sizeof csbf);
    int greater1_ctx = 1;      /* HM's c1: persists across sub-blocks */
    int nsig_total = 0;

    for (int i = last_sb; i >= 0; i--) {
        int xs = scan_x[sb_log2][scan_idx][i];
        int ys = scan_y[sb_log2][scan_idx][i];
        int infer_dc = 0;

        if (i < last_sb && i > 0) {
            int cs = 0;
            if (xs < (1 << sb_log2) - 1) cs += csbf[(ys << sb_log2) + xs + 1];
            if (ys < (1 << sb_log2) - 1) cs += csbf[((ys + 1) << sb_log2) + xs];
            int inc = (cs > 1 ? 1 : cs) + (c_idx ? 2 : 0);
            csbf[(ys << sb_log2) + xs] =
                (uint8_t)h265_cabac_decision(c, CTX_SUBBLOCK + inc);
            infer_dc = 1;
        } else {
            csbf[(ys << sb_log2) + xs] = 1;   /* first and last are implied */
        }
        if (!csbf[(ys << sb_log2) + xs]) continue;

        /* --- sig_coeff_flag pass --- */
        int sig[16];
        memset(sig, 0, sizeof sig);
        int start = (i == last_sb) ? last_pos - 1 : 15;
        if (i == last_sb) sig[last_pos] = 1;

        /* precompute the neighbouring sub-block pattern for 9.3.4.2.5 */
        int prev_csbf = 0;
        if (xs < (1 << sb_log2) - 1) prev_csbf |= csbf[(ys << sb_log2) + xs + 1];
        if (ys < (1 << sb_log2) - 1) prev_csbf |= csbf[((ys + 1) << sb_log2) + xs] << 1;

        for (int k = start; k >= 0; k--) {
            /* 7.3.8.11: the DC flag of an explicitly-coded sub-block is not
             * sent when nothing above it in the scan was significant -- the
             * sub-block was signalled non-empty, so something has to be. */
            if (k == 0 && infer_dc) { sig[0] = 1; break; }
            int xp = scan_x[2][scan_idx][k], yp = scan_y[2][scan_idx][k];
            int xc = (xs << 2) + xp, yc = (ys << 2) + yp;
            int sig_ctx;
            if (log2_size == 2) {
                sig_ctx = sig_ctx_map4[(yc << 2) + xc];
            } else if (xc + yc == 0) {
                sig_ctx = 0;
            } else {
                switch (prev_csbf) {
                case 0:  sig_ctx = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0; break;
                case 1:  sig_ctx = (yp == 0) ? 2 : (yp == 1) ? 1 : 0; break;
                case 2:  sig_ctx = (xp == 0) ? 2 : (xp == 1) ? 1 : 0; break;
                default: sig_ctx = 2; break;
                }
                if (c_idx == 0) {
                    if (xs + ys > 0) sig_ctx += 3;
                    if (log2_size == 3) sig_ctx += (scan_idx == 0) ? 9 : 15;
                    else sig_ctx += 21;
                } else {
                    if (log2_size == 3) sig_ctx += 9;
                    else sig_ctx += 12;
                }
            }
            int inc = (c_idx == 0) ? sig_ctx : 27 + sig_ctx;
            sig[k] = h265_cabac_decision(c, CTX_SIG + inc);
            if (sig[k]) infer_dc = 0;
        }

        /* --- collect the significant positions in scan order (high to low) */
        int pos[16], nsig = 0;
        for (int k = 15; k >= 0; k--) if (sig[k]) pos[nsig++] = k;
        if (nsig == 0) continue;
        nsig_total += nsig;

        int first_sig = pos[nsig - 1], last_sig = pos[0];
        int sign_hidden = (last_sig - first_sig > 3) && !bypass;

        /* --- coeff_abs_level_greater1_flag (9.3.4.2.6) --- */
        int ctx_set = (i > 0 && c_idx == 0) ? 2 : 0;
        if (greater1_ctx == 0) ctx_set++;
        greater1_ctx = 1;
        int g1[16];
        memset(g1, 0, sizeof g1);
        int ng1 = nsig < 8 ? nsig : 8;
        int first_g1 = -1;
        for (int k = 0; k < ng1; k++) {
            int inc = (ctx_set << 2) + greater1_ctx + (c_idx ? 16 : 0);
            g1[k] = h265_cabac_decision(c, CTX_GREATER1 + inc);
            if (g1[k]) {
                greater1_ctx = 0;
                if (first_g1 < 0) first_g1 = k;
            } else if (greater1_ctx > 0 && greater1_ctx < 3) {
                greater1_ctx++;
            }
        }

        /* --- coeff_abs_level_greater2_flag --- */
        int g2 = 0;
        if (first_g1 >= 0)
            g2 = h265_cabac_decision(c, CTX_GREATER2 + ctx_set + (c_idx ? 4 : 0));

        /* --- signs --- */
        int nsigns = nsig;
        if (pps->sign_data_hiding && sign_hidden) nsigns--;
        uint32_t signs = h265_cabac_bypass_n(c, nsigns);

        /* --- remaining levels (9.3.3.11: the Rice parameter adapts to the
         * level just decoded, and resets with every sub-block) --- */
        int rice = 0;
        int sum_abs = 0;
        int levels[16];
        for (int k = 0; k < nsig; k++) {
            int base = 1;
            if (k < ng1) base += g1[k];
            if (k == first_g1) base += g2;
            int need = (k < 8) ? ((k == first_g1) ? 3 : 2) : 1;
            int rem = 0;
            if (base == need) {
                int prefix = 0;
                while (prefix < 32 && h265_cabac_bypass(c)) prefix++;
                if (prefix >= 32) { c->error = 1; return H265_ERR_CORRUPT; }
                if (prefix < 3) {
                    rem = (prefix << rice) + (int)h265_cabac_bypass_n(c, rice);
                } else {
                    int pm3 = prefix - 3;
                    if (pm3 + rice > 30) { c->error = 1; return H265_ERR_CORRUPT; }
                    rem = (((1 << pm3) + 3 - 1) << rice) +
                          (int)h265_cabac_bypass_n(c, pm3 + rice);
                }
                if (base + rem > (3 << rice) && rice < 4) rice++;
            }
            levels[k] = base + rem;
            sum_abs += levels[k];
        }

        /* --- place them, applying the signs (and the hidden one) --- */
        int si = 0;
        for (int k = 0; k < nsig; k++) {
            int kk = pos[k];
            int xc = (xs << 2) + scan_x[2][scan_idx][kk];
            int yc = (ys << 2) + scan_y[2][scan_idx][kk];
            int neg;
            if (pps->sign_data_hiding && sign_hidden && kk == first_sig) {
                neg = sum_abs & 1;
            } else {
                neg = (int)((signs >> (nsigns - 1 - si)) & 1);
                si++;
            }
            int v = neg ? -levels[k] : levels[k];
            out[yc * n + xc] = (int16_t)h265_clip3(-32768, 32767, v);
        }
        if (c->error) return H265_ERR_CORRUPT;
    }
    (void)sps;
    return c->error ? H265_ERR_CORRUPT : nsig_total;
}
