/* c/lib/video/h264_mb.c -- the macroblock layer: macroblock_layer(),
 * mb_pred(), sub_mb_pred() and residual(), for both entropy coders.
 *
 * CAVLC and CABAC differ in how every syntax element is spelled but not in
 * which elements appear or in what order, so this file has one structure with
 * a branch at each read rather than two decoders. That is deliberate: the two
 * paths then cannot drift, and a bug in the shared reconstruction shows up in
 * both -- an existing Baseline stream is a real regression test for a change
 * made for High profile.
 *
 * Neighbour-dependent context selection (CAVLC's nC, CABAC's ctxIdxInc) is
 * here because it reads mbinfo of the surrounding macroblocks; the arithmetic
 * decoder itself knows nothing about macroblocks.
 */
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"

/* ---------------------------------------------------------------- tables -- */
static const uint8_t zz4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
/* luma4x4BlkIdx <-> 4x4 grid position (spec 6.4.3): the 16 luma blocks of a MB
 * are NOT in raster order; they zigzag at both the 8x8 and the 4x4 level:
 * 0,1,4,5 / 2,3,6,7 / 8,9,12,13 / 10,11,14,15. */
static const uint8_t i4x[16] = { 0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3 };
static const uint8_t i4y[16] = { 0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3 };
static const uint8_t i4_inv[16] = { 0,1,4,5, 2,3,6,7, 8,9,12,13, 10,11,14,15 };
/* codeNum -> coded_block_pattern (spec Table 9-4) */
static const uint8_t cbp_intra_tab[48] = {
    47, 31, 15,  0, 23, 27, 29, 30,  7, 11, 13, 14, 39, 43, 45, 46,
    16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,  1,  2,  4,
     8, 17, 18, 20, 24,  6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};
static const uint8_t cbp_inter_tab[48] = {
     0, 16,  1,  2,  4,  8, 32,  3,  5, 10, 12, 15, 47,  7, 11, 13,
    14,  6,  9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

/* Table 7-14: B macroblock partition layout and per-partition prediction.
 * nparts 0 marks B_Direct_16x16, which has no partitions of its own. */
typedef struct { uint8_t nparts, pw, ph, pred[2]; } bpart_t;
static const bpart_t b_mb[23] = {
    { 0, 4, 4, { 0, 0 } }, { 1, 4, 4, { 0, 0 } }, { 1, 4, 4, { 1, 1 } },
    { 1, 4, 4, { 2, 2 } }, { 2, 4, 2, { 0, 0 } }, { 2, 2, 4, { 0, 0 } },
    { 2, 4, 2, { 1, 1 } }, { 2, 2, 4, { 1, 1 } }, { 2, 4, 2, { 0, 1 } },
    { 2, 2, 4, { 0, 1 } }, { 2, 4, 2, { 1, 0 } }, { 2, 2, 4, { 1, 0 } },
    { 2, 4, 2, { 0, 2 } }, { 2, 2, 4, { 0, 2 } }, { 2, 4, 2, { 1, 2 } },
    { 2, 2, 4, { 1, 2 } }, { 2, 4, 2, { 2, 0 } }, { 2, 2, 4, { 2, 0 } },
    { 2, 4, 2, { 2, 1 } }, { 2, 2, 4, { 2, 1 } }, { 2, 4, 2, { 2, 2 } },
    { 2, 2, 4, { 2, 2 } }, { 4, 2, 2, { 0, 0 } }
};
/* Table 7-18: B sub_mb_type -> (sub-partitions, width, height, prediction).
 * pred 3 marks B_Direct_8x8. */
typedef struct { uint8_t nsub, pw, ph, pred; } bsub_t;
static const bsub_t b_sub[13] = {
    { 1, 2, 2, 3 }, { 1, 2, 2, 0 }, { 1, 2, 2, 1 }, { 1, 2, 2, 2 },
    { 2, 2, 1, 0 }, { 2, 1, 2, 0 }, { 2, 2, 1, 1 }, { 2, 1, 2, 1 },
    { 2, 2, 1, 2 }, { 2, 1, 2, 2 }, { 4, 1, 1, 0 }, { 4, 1, 1, 1 },
    { 4, 1, 1, 2 }
};

typedef struct { int px, py, pw, ph, kind, region, pred; } part_t;

/* --------------------------------------------------------------- helpers -- */
static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

/* Neighbouring macroblock address, or -1 when it is outside the picture or in
 * another slice. Every context derivation in this file starts here. */
static int nb_mb(h264dec *d, int addr, int dx, int dy)
{
    int mbx = addr % d->mbw + dx, mby = addr / d->mbw + dy;
    if (mbx < 0 || mby < 0 || mbx >= d->mbw || mby >= d->mbh) return -1;
    int n = mby * d->mbw + mbx;
    if (n > addr) return -1;
    if (h264_slice_of(d, n) != h264_slice_of(d, addr)) return -1;
    return n;
}

/* ================================================ CAVLC nC derivation ==== */
static int combine_nc(int na, int nb)
{
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

static int nb_nz(const mbinfo_t *m, int idx)
{
    if (m->type == MB_I_PCM) return 16;
    return m->nz[idx];
}

static int luma_nC(h264dec *d, int addr, int bx, int by)
{
    const mbinfo_t *mi = &d->mb[addr];
    int na = -1, nb = -1, n;
    if (bx > 0) na = nb_nz(mi, i4_inv[by * 4 + bx - 1]);
    else if ((n = nb_mb(d, addr, -1, 0)) >= 0)
        na = nb_nz(&d->mb[n], i4_inv[by * 4 + 3]);
    if (by > 0) nb = nb_nz(mi, i4_inv[(by - 1) * 4 + bx]);
    else if ((n = nb_mb(d, addr, 0, -1)) >= 0)
        nb = nb_nz(&d->mb[n], i4_inv[12 + bx]);
    return combine_nc(na, nb);
}

/* nC for the I16x16 luma DC block.  NOT the neighbours' DC TotalCoeff -- both
 * JM and ffmpeg read the ordinary 4x4 luma block counts adjacent to position
 * (0,0). Getting this wrong desyncs coeff_token whenever a neighbour's DC
 * count and its (3,0)/(0,3) block counts differ. */
static int i16dc_nC(h264dec *d, int addr) { return luma_nC(d, addr, 0, 0); }

static int chroma_nC(h264dec *d, int addr, int comp, int bx, int by)
{
    const mbinfo_t *mi = &d->mb[addr];
    int base = 16 + comp * 4, na = -1, nb = -1, n;
    if (bx > 0) na = nb_nz(mi, base + by * 2 + bx - 1);
    else if ((n = nb_mb(d, addr, -1, 0)) >= 0)
        na = nb_nz(&d->mb[n], base + by * 2 + 1);
    if (by > 0) nb = nb_nz(mi, base + (by - 1) * 2 + bx);
    else if ((n = nb_mb(d, addr, 0, -1)) >= 0)
        nb = nb_nz(&d->mb[n], base + 2 + bx);
    return combine_nc(na, nb);
}

/* ============================================ CABAC coded_block_flag ctx == */
/* 9.3.3.1.1.9. condTermFlagN is 1 when the neighbouring transform block exists
 * and has coefficients; an ABSENT neighbour counts as 1 for an intra
 * macroblock and 0 for an inter one, which is the asymmetry that makes this
 * worth writing out rather than folding into a "nz > 0" test. */
static int cbf_term(h264dec *d, int addr, int n, int cat, int idx, int comp)
{
    const mbinfo_t *cur = &d->mb[addr];
    if (n < 0) return h264_is_intra_type(cur->type) ? 1 : 0;
    const mbinfo_t *m = &d->mb[n];
    if (m->type == MB_I_PCM) return 1;
    switch (cat) {
    case 0: return (m->type == MB_I16x16 && m->nz_i16dc > 0) ? 1 : 0;
    case 3: return m->nz_cdc[comp] > 0 ? 1 : 0;
    case 4: return m->nz[16 + comp * 4 + idx] > 0 ? 1 : 0;
    default: return m->nz[idx] > 0 ? 1 : 0;      /* cat 1, 2 (and 5's blocks) */
    }
}

/* ctxIdxInc for a luma 4x4 block at grid (bx, by) of the current macroblock. */
static int cbf_inc_luma(h264dec *d, int addr, int cat, int bx, int by)
{
    int a, b, n;
    if (bx > 0) a = cbf_term(d, addr, addr, cat, i4_inv[by * 4 + bx - 1], 0);
    else { n = nb_mb(d, addr, -1, 0);
           a = cbf_term(d, addr, n, cat, i4_inv[by * 4 + 3], 0); }
    if (by > 0) b = cbf_term(d, addr, addr, cat, i4_inv[(by - 1) * 4 + bx], 0);
    else { n = nb_mb(d, addr, 0, -1);
           b = cbf_term(d, addr, n, cat, i4_inv[12 + bx], 0); }
    return a + 2 * b;
}

static int cbf_inc_chroma(h264dec *d, int addr, int comp, int bx, int by)
{
    int a, b, n;
    if (bx > 0) a = cbf_term(d, addr, addr, 4, by * 2 + bx - 1, comp);
    else { n = nb_mb(d, addr, -1, 0);
           a = cbf_term(d, addr, n, 4, by * 2 + 1, comp); }
    if (by > 0) b = cbf_term(d, addr, addr, 4, (by - 1) * 2 + bx, comp);
    else { n = nb_mb(d, addr, 0, -1);
           b = cbf_term(d, addr, n, 4, 2 + bx, comp); }
    return a + 2 * b;
}

static int cbf_inc_simple(h264dec *d, int addr, int cat, int comp)
{
    int na = nb_mb(d, addr, -1, 0), nb = nb_mb(d, addr, 0, -1);
    return cbf_term(d, addr, na, cat, 0, comp) +
           2 * cbf_term(d, addr, nb, cat, 0, comp);
}

/* ============================================== residual block dispatch == */
/* One coefficient block, whichever entropy coder the picture uses. `coef` is
 * filled in SCAN order and the return value is the number of nonzero
 * coefficients (which is what both nC and ctxIdxInc want), or < 0. */
static int read_block(h264dec *d, bs_t *bs, int cat, int max_coeff,
                      int nc, int cbf_inc, int coef[64])
{
    if (d->cabac) {
        int n = h264_cabac_residual(&d->cab, cat, max_coeff, cbf_inc, coef);
        if (n < 0 || d->cab.error) return -1;
        return n;
    }
    int tmp[16];
    int n = cavlc_decode(bs, nc, max_coeff, tmp);
    if (n < 0 || bs_error(bs)) return -1;
    for (int i = 0; i < max_coeff && i < 16; i++) coef[i] = tmp[i];
    return n;
}

/* ============================================== residual reconstruction == */
/* IDCT + add for blocks whose DC arrives FINAL (I16x16 luma via the scaled
 * Hadamard, chroma via the scaled 2x2 Hadamard): dc_val is placed verbatim at
 * raster position 0, ac[0..14] are the scan positions 1..15 (NULL = no AC
 * coded). Same integer arithmetic as h264_dequant_idct_add minus the DC
 * dequant, so results are bit-identical to the fused spec pipeline. */
static void idct_add_dc_ac(int dc_val, const int *ac, int qp, const int *ls,
                           uint8_t *dst, int stride)
{
    int d[16], f[16], g[16];
    int k = qp / 6;
    for (int i = 0; i < 16; i++) d[i] = 0;
    d[0] = dc_val;
    if (ac) {
        for (int i = 0; i < 15; i++) {
            int c = ac[i];
            if (!c) continue;
            int r = zz4[i + 1];
            if (c > 16383) c = 16383; else if (c < -16384) c = -16384;
            int p = c * ls[r];
            d[r] = k >= 4 ? p * (1 << (k - 4)) : (p + (1 << (3 - k))) >> (4 - k);
        }
    }
    for (int j = 0; j < 4; j++) {                 /* columns */
        int d0 = d[j], d1 = d[4 + j], d2 = d[8 + j], d3 = d[12 + j];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        f[j] = e0 + e3; f[4 + j] = e1 + e2; f[8 + j] = e1 - e2; f[12 + j] = e0 - e3;
    }
    for (int i = 0; i < 4; i++) {                 /* rows */
        int f0 = f[i * 4], f1 = f[i * 4 + 1], f2 = f[i * 4 + 2], f3 = f[i * 4 + 3];
        int e0 = f0 + f2, e1 = f0 - f2, e2 = (f1 >> 1) - f3, e3 = f1 + (f3 >> 1);
        g[i * 4] = e0 + e3; g[i * 4 + 1] = e1 + e2;
        g[i * 4 + 2] = e1 - e2; g[i * 4 + 3] = e0 - e3;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int v = dst[i * stride + j] + ((g[i * 4 + j] + 32) >> 6);
            dst[i * stride + j] = (uint8_t)clip3(0, 255, v);
        }
}

/* Which of the six 4x4 scaling lists a block uses: intra/inter x Y/Cb/Cr. */
static int ls_idx(int intra, int comp) { return (intra ? 0 : 3) + comp; }

static int residual_chroma(h264dec *d, bs_t *bs, int addr, int cbp_chroma,
                           int qpy, int intra)
{
    if (cbp_chroma <= 0) return H264_OK;
    if (cbp_chroma > 2) return H264_ERR_CORRUPT;
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    const pps_t *pps = d->cur_pps;
    int off_cb = pps->chroma_qp_index_offset;
    int off_cr = pps->has_second_chroma_offset ? pps->second_chroma_qp_offset
                                               : off_cb;

    /* Bitstream order (7.3.5.3): ALL chroma DC chains first (Cb then Cr),
     * THEN all AC blocks (Cb x4, Cr x4). */
    int dc[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    int qpc[2];
    for (int comp = 0; comp < 2; comp++) {
        qpc[comp] = h264_chroma_qp(qpy, comp ? off_cr : off_cb);
        int t[64];
        int tc = read_block(d, bs, 3, 4, -1,
                            d->cabac ? cbf_inc_simple(d, addr, 3, comp) : 0, t);
        if (tc < 0) return H264_ERR_CORRUPT;
        mi->nz_cdc[comp] = (uint8_t)tc;
        for (int i = 0; i < 4; i++) dc[comp][i] = t[i];
        h264_dcdcm_transform(dc[comp]);
        const int *ls = pps->ls4[ls_idx(intra, comp + 1)][qpc[comp] % 6];
        int k = qpc[comp] / 6;
        for (int i = 0; i < 4; i++) {
            /* 8.5.11.2: dcC = ((f * LevelScale4x4(qP%6,0,0)) << (qP/6)) >> 5.
             * That is exact once qP >= 6; below it the >> 5 is a FLOOR.
             * Rounding half up there instead is off by one whenever the
             * product is odd, which needs both a chroma qP under 6 and an odd
             * scale factor. Nothing in the generated matrix quantises that
             * finely; tests/fixtures/video does, in one macroblock of one
             * frame, and the error then rode the reference chain through the
             * rest of the stream. */
            int p = dc[comp][i] * ls[0];
            dc[comp][i] = k >= 5 ? p * (1 << (k - 5)) : (p >> (5 - k));
        }
    }
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        const int *ls = pps->ls4[ls_idx(intra, comp + 1)][qpc[comp] % 6];
        for (int b = 0; b < 4; b++) {
            int bx = b & 1, by = b >> 1;
            int ac[64], *acp = 0;
            if (cbp_chroma >= 2) {
                int tc = read_block(d, bs, 4, 15,
                                    chroma_nC(d, addr, comp, bx, by),
                                    d->cabac ? cbf_inc_chroma(d, addr, comp, bx, by) : 0,
                                    ac);
                if (tc < 0) return H264_ERR_CORRUPT;
                acp = ac;
                mi->nz[16 + comp * 4 + b] = (uint8_t)tc;
            }
            uint8_t *dst = plane + (long)(mby * 8 + by * 4) * d->stride_c
                                   + mbx * 8 + bx * 4;
            if (dc[comp][b] || acp)
                idct_add_dc_ac(dc[comp][b], acp, qpc[comp], ls, dst, d->stride_c);
        }
    }
    return H264_OK;
}

/* Luma residual of an inter or I_NxN macroblock: sixteen 4x4 blocks, or four
 * 8x8 ones. CAVLC codes an 8x8 block as four INTERLEAVED 4x4 blocks
 * (level8x8[4*i + i4x4] = level4x4[i]), CABAC as a single 64-coefficient
 * block -- the same transform either way. */
static int residual_luma(h264dec *d, bs_t *bs, int addr, int cbp_luma,
                         int qpy, int intra)
{
    mbinfo_t *mi = &d->mb[addr];
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    const pps_t *pps = d->cur_pps;
    const int *ls  = pps->ls4[ls_idx(intra, 0)][qpy % 6];
    const int *ls8 = pps->ls8[intra ? 0 : 1][qpy % 6];

    for (int i8 = 0; i8 < 4; i8++) {
        if (!(cbp_luma & (1 << i8))) continue;
        uint8_t *dst8 = d->cur->y
            + (long)(mby * 16 + (i8 >> 1) * 8) * d->stride_y + mbx * 16 + (i8 & 1) * 8;
        if (mi->transform8x8 && d->cabac) {
            int coef[64];
            int tc = read_block(d, bs, 5, 64, 0, -1, coef);
            if (tc < 0) return H264_ERR_CORRUPT;
            for (int k = 0; k < 4; k++) mi->nz[i8 * 4 + k] = (uint8_t)tc;
            h264_dequant_idct8_add(coef, qpy, ls8, dst8, d->stride_y);
        } else if (mi->transform8x8) {
            int coef[64];
            for (int i = 0; i < 64; i++) coef[i] = 0;
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, t[64];
                int bx = i4x[blk], by = i4y[blk];
                int tc = read_block(d, bs, 2, 16, luma_nC(d, addr, bx, by), 0, t);
                if (tc < 0) return H264_ERR_CORRUPT;
                mi->nz[blk] = (uint8_t)tc;
                for (int i = 0; i < 16; i++) coef[4 * i + i4] = t[i];
            }
            h264_dequant_idct8_add(coef, qpy, ls8, dst8, d->stride_y);
        } else {
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, coef[64];
                int bx = i4x[blk], by = i4y[blk];
                int tc = read_block(d, bs, 2, 16, luma_nC(d, addr, bx, by),
                                    d->cabac ? cbf_inc_luma(d, addr, 2, bx, by) : 0,
                                    coef);
                if (tc < 0) return H264_ERR_CORRUPT;
                mi->nz[blk] = (uint8_t)tc;
                if (tc) {
                    uint8_t *dst = d->cur->y
                        + (long)(mby * 16 + by * 4) * d->stride_y + mbx * 16 + bx * 4;
                    h264_dequant_idct_add(coef, qpy, ls, dst, d->stride_y);
                }
            }
        }
    }
    return H264_OK;
}

/* ============================================== CABAC macroblock syntax == */
static int cab_intra_mb_type(h264dec *d, int addr, int base, int intra_slice)
{
    h264cabac *c = &d->cab;
    int st = base;
    if (intra_slice) {
        int ctx = 0, n;
        /* condTermFlagN: 1 unless the neighbour is I_NxN. In an I slice the
         * only other options are I16x16 and I_PCM. */
        if ((n = nb_mb(d, addr, -1, 0)) >= 0 &&
            (d->mb[n].type == MB_I16x16 || d->mb[n].type == MB_I_PCM)) ctx++;
        if ((n = nb_mb(d, addr, 0, -1)) >= 0 &&
            (d->mb[n].type == MB_I16x16 || d->mb[n].type == MB_I_PCM)) ctx++;
        if (!h264_cabac_decision(c, base + ctx)) return 0;      /* I_NxN */
        st = base + 2;
    } else {
        if (!h264_cabac_decision(c, base)) return 0;            /* I_NxN */
    }
    if (h264_cabac_terminate(c)) return 25;                     /* I_PCM */
    int t = 1;
    t += 12 * h264_cabac_decision(c, st + 1);
    if (h264_cabac_decision(c, st + 2))
        t += 4 + 4 * h264_cabac_decision(c, st + 2 + intra_slice);
    t += 2 * h264_cabac_decision(c, st + 3 + intra_slice);
    t += 1 * h264_cabac_decision(c, st + 3 + 2 * intra_slice);
    return t;
}

static int cab_mb_type_p(h264dec *d, int addr)
{
    h264cabac *c = &d->cab;
    if (!h264_cabac_decision(c, 14)) {
        if (!h264_cabac_decision(c, 15))
            return h264_cabac_decision(c, 16) ? 3 : 0;   /* P_8x8 : P_L0_16x16 */
        /* Table 9-34: the third bin is 1 for 16x8 and 0 for 8x16 -- the
         * opposite way round from the branch above, where 1 selects the LATER
         * mb_type. Reading it the intuitive way makes every 16x8 macroblock a
         * 8x16 one, which reads a different number of motion vectors and
         * therefore desynchronises the arithmetic decoder a few macroblocks
         * later, somewhere that looks nothing like the cause. */
        return h264_cabac_decision(c, 17) ? 1 : 2;       /* 16x8 : 8x16 */
    }
    return cab_intra_mb_type(d, addr, 17, 0) + 5;
}

/* 9.3.3.1.1.3. The bin0 context asks whether each neighbour was exactly
 * B_Skip or B_Direct_16x16 -- a B_8x8 whose sub_mb_types all happen to be
 * B_Direct_8x8 is neither, which is why mbinfo carries the mb_type answer
 * separately from the per-8x8 direct flags. */
static int cab_mb_type_b(h264dec *d, int addr)
{
    h264cabac *c = &d->cab;
    int ctx = 0, n;
    if ((n = nb_mb(d, addr, -1, 0)) >= 0 && !d->mb[n].bdirect16) ctx++;
    if ((n = nb_mb(d, addr, 0, -1)) >= 0 && !d->mb[n].bdirect16) ctx++;
    if (!h264_cabac_decision(c, 27 + ctx)) return 0;         /* B_Direct_16x16 */
    if (!h264_cabac_decision(c, 27 + 3))
        return 1 + h264_cabac_decision(c, 27 + 5);           /* B_L0/L1_16x16 */
    int bits = h264_cabac_decision(c, 27 + 4) << 3;
    bits |= h264_cabac_decision(c, 27 + 5) << 2;
    bits |= h264_cabac_decision(c, 27 + 5) << 1;
    bits |= h264_cabac_decision(c, 27 + 5);
    if (bits < 8) return bits + 3;
    if (bits == 13) return cab_intra_mb_type(d, addr, 32, 0) + 23;
    if (bits == 14) return 11;
    if (bits == 15) return 22;
    bits = (bits << 1) | h264_cabac_decision(c, 27 + 5);
    return bits - 4;
}

static int cab_sub_type_p(h264dec *d)
{
    h264cabac *c = &d->cab;
    if (h264_cabac_decision(c, 21)) return 0;
    if (!h264_cabac_decision(c, 22)) return 1;
    return h264_cabac_decision(c, 23) ? 2 : 3;
}

static int cab_sub_type_b(h264dec *d)
{
    h264cabac *c = &d->cab;
    if (!h264_cabac_decision(c, 36)) return 0;
    if (!h264_cabac_decision(c, 37)) return 1 + h264_cabac_decision(c, 39);
    int t = 3;
    if (h264_cabac_decision(c, 38)) {
        if (h264_cabac_decision(c, 39)) return 11 + h264_cabac_decision(c, 39);
        t += 4;
    }
    t += 2 * h264_cabac_decision(c, 39);
    t += h264_cabac_decision(c, 39);
    return t;
}

/* 9.3.3.1.1.6: a neighbour contributes only if it is available, is not a
 * direct/skip partition, uses this list, and its refIdx is above zero. */
static int cab_ref_term(h264dec *d, int addr, int gx, int gy, int list)
{
    if (gx < 0 || gy < 0 || gx >= d->mbw * 4 || gy >= d->mbh * 4) return 0;
    int n = nb_mb(d, addr, (gx >> 2) - addr % d->mbw, (gy >> 2) - addr / d->mbw);
    if (n < 0) return 0;
    const mbinfo_t *m = &d->mb[n];
    if (h264_is_intra_type(m->type)) return 0;
    int q = ((gy & 3) >> 1) * 2 + ((gx & 3) >> 1);
    if (m->type == MB_SKIP || (m->direct8x8 & (1 << q))) return 0;
    return m->ref_idx[list][q] > 0 ? 1 : 0;
}

static int cab_ref_idx(h264dec *d, int addr, int mbx, int mby,
                       int px, int py, int list, int max)
{
    h264cabac *c = &d->cab;
    int gx = mbx * 4 + px, gy = mby * 4 + py;
    int ctx = cab_ref_term(d, addr, gx - 1, gy, list)
            + 2 * cab_ref_term(d, addr, gx, gy - 1, list);
    int ref = 0;
    while (h264_cabac_decision(c, 54 + ctx)) {
        ref++;
        ctx = (ctx >> 2) + 4;
        if (ref > max || ref >= 32) return -1;
    }
    return ref;
}

/* mvd, UEG3 with uCoff 9 (9.3.2.3). The bin0 context is chosen by the SUM of
 * the two neighbours' absolute mvd for this component, bucketed at 3 and 33 --
 * so the mvd of every partition has to be remembered, not just its vector. */
static int cab_mvd(h264dec *d, int addr, int gx, int gy, int list, int comp)
{
    h264cabac *c = &d->cab;
    int base = comp ? 47 : 40;
    int amvd = h264_mvd_ctx(d, addr, gx - 1, gy, list, comp)
             + h264_mvd_ctx(d, addr, gx, gy - 1, list, comp);
    int inc = (amvd > 2) + (amvd > 32);
    if (!h264_cabac_decision(c, base + inc)) return 0;
    int v = 1, ctx = base + 3;
    while (v < 9 && h264_cabac_decision(c, ctx)) {
        if (v < 4) ctx++;
        v++;
    }
    if (v >= 9) {
        int k = 3, n = 0;
        while (h264_cabac_bypass(c) && n < 24) { v += 1 << k; k++; n++; }
        while (k--) v += h264_cabac_bypass(c) << k;
    }
    return h264_cabac_bypass(c) ? -v : v;
}

static int cab_cbp_luma(h264dec *d, int addr)
{
    h264cabac *c = &d->cab;
    int cbp = 0, na = nb_mb(d, addr, -1, 0), nb = nb_mb(d, addr, 0, -1);
    /* condTermFlagN is 1 when the neighbouring 8x8 block has NO coefficients;
     * an unavailable or I_PCM neighbour contributes 0. */
    for (int b = 0; b < 4; b++) {
        int a, t;
        if (b & 1) a = !((cbp >> (b - 1)) & 1);
        else if (na < 0 || d->mb[na].type == MB_I_PCM) a = 0;
        else a = !((d->mb[na].cbp >> (b + 1)) & 1);
        if (b & 2) t = !((cbp >> (b - 2)) & 1);
        else if (nb < 0 || d->mb[nb].type == MB_I_PCM) t = 0;
        else t = !((d->mb[nb].cbp >> (b + 2)) & 1);
        cbp |= h264_cabac_decision(c, 73 + a + 2 * t) << b;
    }
    return cbp;
}

static int cab_cbp_chroma(h264dec *d, int addr)
{
    h264cabac *c = &d->cab;
    int na = nb_mb(d, addr, -1, 0), nb = nb_mb(d, addr, 0, -1);
    int ca = na < 0 ? 0 : (d->mb[na].cbp >> 4) & 3;
    int cb = nb < 0 ? 0 : (d->mb[nb].cbp >> 4) & 3;
    int ctx = (ca > 0) + 2 * (cb > 0);
    if (!h264_cabac_decision(c, 77 + ctx)) return 0;
    ctx = 4 + (ca == 2) + 2 * (cb == 2);
    return 1 + h264_cabac_decision(c, 77 + ctx);
}

static int cab_qp_delta(h264dec *d)
{
    h264cabac *c = &d->cab;
    if (!h264_cabac_decision(c, 60 + (d->last_qp_delta != 0))) return 0;
    int val = 1, ctx = 2;
    while (h264_cabac_decision(c, 60 + ctx)) {
        ctx = 3;
        if (++val > 102) return 0x7fffffff;      /* corrupt: caller rejects */
    }
    return (val & 1) ? (val + 1) >> 1 : -((val + 1) >> 1);
}

static int cab_transform8x8(h264dec *d, int addr)
{
    int ctx = 0, n;
    if ((n = nb_mb(d, addr, -1, 0)) >= 0 && d->mb[n].transform8x8) ctx++;
    if ((n = nb_mb(d, addr, 0, -1)) >= 0 && d->mb[n].transform8x8) ctx++;
    return h264_cabac_decision(&d->cab, 399 + ctx);
}

static int cab_chroma_pred_mode(h264dec *d, int addr)
{
    h264cabac *c = &d->cab;
    int ctx = 0, n;
    if ((n = nb_mb(d, addr, -1, 0)) >= 0 && d->mb[n].chroma_mode != 0) ctx++;
    if ((n = nb_mb(d, addr, 0, -1)) >= 0 && d->mb[n].chroma_mode != 0) ctx++;
    if (!h264_cabac_decision(c, 64 + ctx)) return 0;
    if (!h264_cabac_decision(c, 64 + 3)) return 1;
    if (!h264_cabac_decision(c, 64 + 3)) return 2;
    return 3;
}

/* ================================================= intra prediction ====== */
/* Per-block availability flags (6.4.11.4 / 8.3.1.1), at 4x4 or 8x8 scale. */
static void ixx_avail(h264dec *d, int cur, int mbx, int mby, int bx, int by,
                      int n, int *al, int *at, int *atl, int *atr)
{
    *al = bx > 0 ? 1 : h264_intra_avail(d, cur, mbx - 1, mby);
    *at = by > 0 ? 1 : h264_intra_avail(d, cur, mbx, mby - 1);
    *atl = (bx > 0 && by > 0) ? 1
         : h264_intra_avail(d, cur, mbx - (bx == 0), mby - (by == 0));
    if (by > 0) {
        /* Inside the macroblock the above-right block is available only if it
         * has already been DECODED, and 4x4 blocks are coded in Z order:
         *      0  1  4  5
         *      2  3  6  7
         *      8  9 12 13
         *     10 11 14 15
         * For a block at (bx, by>0) the above-right is (bx+1, by-1), which is
         * earlier except when bx == 1 and by is odd (blocks 3 and 11, whose
         * above-right sits in the next 8x8 quadrant) or bx == 3 (the next
         * macroblock). At 8x8 scale (n == 2) the same reasoning leaves only
         * block 3 without one. */
        *atr = n == 2 ? (bx == 0) : ((bx != 3) && !(bx == 1 && (by & 1)));
    } else {
        *atr = bx < n - 1 ? h264_intra_avail(d, cur, mbx, mby - 1)
                          : h264_intra_avail(d, cur, mbx + 1, mby - 1);
    }
}

/* Predicted intra mode (8.3.1.1 / 8.3.2.1). An unavailable neighbour is -1, a
 * neighbour not coded I_NxN counts as DC(2), and if EITHER side is -1 the
 * prediction is DC(2) -- NOT min() with the other side substituted. */
static int pred_intra_mode(h264dec *d, int addr, int bx, int by)
{
    const mbinfo_t *mi = &d->mb[addr];
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    int ma = -1, mbm = -1;
    if (bx > 0) ma = mi->i4mode[i4_inv[by * 4 + bx - 1]];
    else if (h264_intra_avail(d, addr, mbx - 1, mby)) {
        const mbinfo_t *l = &d->mb[addr - 1];
        ma = l->type == MB_I4x4 ? l->i4mode[i4_inv[by * 4 + 3]] : 2;
    }
    if (by > 0) mbm = mi->i4mode[i4_inv[(by - 1) * 4 + bx]];
    else if (h264_intra_avail(d, addr, mbx, mby - 1)) {
        const mbinfo_t *t = &d->mb[addr - d->mbw];
        mbm = t->type == MB_I4x4 ? t->i4mode[i4_inv[12 + bx]] : 2;
    }
    int mn = ma < mbm ? ma : mbm;
    return mn < 0 ? 2 : mn;
}

static int read_intra_mode(h264dec *d, bs_t *bs, int pred)
{
    if (d->cabac) {
        if (h264_cabac_decision(&d->cab, 68)) return pred;
        int m = h264_cabac_decision(&d->cab, 69);
        m += h264_cabac_decision(&d->cab, 69) << 1;
        m += h264_cabac_decision(&d->cab, 69) << 2;
        return m + (m >= pred);
    }
    if (bs_u1(bs)) return pred;
    int rem = (int)bs_u(bs, 3);
    return rem < pred ? rem : rem + 1;
}

static void chroma_intra_pred(h264dec *d, int addr, int mode)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    int al = h264_intra_avail(d, addr, mbx - 1, mby);
    int at = h264_intra_avail(d, addr, mbx, mby - 1);
    int atl = h264_intra_avail(d, addr, mbx - 1, mby - 1);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *plane = comp ? d->cur->v : d->cur->u;
        int s = d->stride_c, px = mbx * 8, py = mby * 8;
        uint8_t topc[8], leftc[8];
        int tl = atl ? plane[(long)(py - 1) * s + px - 1] : 0;
        if (at) memcpy(topc, plane + (long)(py - 1) * s + px, 8);
        else memset(topc, 0, 8);
        if (al) for (int i = 0; i < 8; i++) leftc[i] = plane[(long)(py + i) * s + px - 1];
        else memset(leftc, 0, 8);
        h264_intra_chroma(plane + (long)py * s + px, s, mode, topc, leftc, tl, al, at);
    }
}

/* I_NxN: sixteen 4x4 blocks or four 8x8 ones. Prediction and residual are
 * interleaved per block because each block predicts from the RECONSTRUCTED
 * samples of the ones before it. */
static int decode_i_nxn(h264dec *d, bs_t *bs, int addr, int *qpyp, int chroma_mode)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    const pps_t *pps = d->cur_pps;
    int n = mi->transform8x8 ? 2 : 4;          /* blocks per side */
    int bw = mi->transform8x8 ? 8 : 4;         /* block size in samples */

    for (int b = 0; b < n * n; b++) {
        int bx = mi->transform8x8 ? (b & 1) * 2 : i4x[b];
        int by = mi->transform8x8 ? (b >> 1) * 2 : i4y[b];
        int mode = read_intra_mode(d, bs, pred_intra_mode(d, addr, bx, by));
        if (!d->cabac && bs_error(bs)) return H264_ERR_CORRUPT;
        if (mi->transform8x8) {
            for (int k = 0; k < 4; k++)
                mi->i4mode[i4_inv[(by + (k >> 1)) * 4 + bx + (k & 1)]] = (uint8_t)mode;
        } else {
            mi->i4mode[b] = (uint8_t)mode;
        }
    }
    if (d->cabac) chroma_mode = cab_chroma_pred_mode(d, addr);
    else {
        uint32_t cm = bs_ue(bs);
        if (bs_error(bs) || cm > 3) return H264_ERR_CORRUPT;
        chroma_mode = (int)cm;
    }
    mi->chroma_mode = (uint8_t)chroma_mode;

    /* coded_block_pattern + qp */
    int cbp;
    if (d->cabac) {
        cbp = cab_cbp_luma(d, addr) | (cab_cbp_chroma(d, addr) << 4);
    } else {
        uint32_t codeNum = bs_ue(bs);
        if (bs_error(bs) || codeNum > 47) return H264_ERR_CORRUPT;
        cbp = cbp_intra_tab[codeNum];
    }
    mi->cbp = (uint8_t)cbp;
    int cbp_luma = cbp & 15, cbp_chroma = (cbp >> 4) & 3;
    int qpy = *qpyp;
    if (cbp_luma || cbp_chroma) {
        int delta;
        if (d->cabac) { delta = cab_qp_delta(d); d->last_qp_delta = delta; }
        else { delta = (int)bs_se(bs); if (bs_error(bs)) return H264_ERR_CORRUPT; }
        if (delta < -87 || delta > 77) return H264_ERR_CORRUPT;
        qpy = ((qpy + delta) % 52 + 52) % 52;
        *qpyp = qpy;
    } else if (d->cabac) {
        d->last_qp_delta = 0;
    }
    mi->qp = (int8_t)qpy;

    /* Luma: predict + residual per block, in coding order. */
    const int *ls  = pps->ls4[ls_idx(1, 0)][qpy % 6];
    const int *ls8 = pps->ls8[0][qpy % 6];
    uint8_t topbuf[16], leftbuf[8];
    for (int b = 0; b < n * n; b++) {
        int bx = mi->transform8x8 ? (b & 1) : i4x[b];
        int by = mi->transform8x8 ? (b >> 1) : i4y[b];
        int al, at, atl, atr, tl;
        ixx_avail(d, addr, mbx, mby, bx, by, n, &al, &at, &atl, &atr);
        int px = mbx * 16 + bx * bw, py = mby * 16 + by * bw;
        const uint8_t *yp = d->cur->y;
        int s = d->stride_y;
        memset(topbuf, 0, sizeof topbuf);
        memset(leftbuf, 0, sizeof leftbuf);
        if (at) {
            const uint8_t *t = yp + (long)(py - 1) * s + px;
            memcpy(topbuf, t, (size_t)bw);
            for (int i = bw; i < 2 * bw; i++) topbuf[i] = atr ? t[i] : t[bw - 1];
        }
        if (al)
            for (int i = 0; i < bw; i++) leftbuf[i] = yp[(long)(py + i) * s + px - 1];
        tl = atl ? yp[(long)(py - 1) * s + px - 1] : 0;

        uint8_t *dst = d->cur->y + (long)py * s + px;
        int mode = mi->i4mode[mi->transform8x8
                              ? i4_inv[(by * 2) * 4 + bx * 2] : b];
        if (mi->transform8x8)
            h264_intra8x8(dst, s, mode, topbuf, leftbuf, tl, al, at, atr, atl);
        else
            h264_intra4x4(dst, s, mode, topbuf, leftbuf, tl, al, at, atr, atl);

        if (!(cbp_luma & (1 << (mi->transform8x8 ? b : (b >> 2))))) continue;
        if (mi->transform8x8) {
            int coef[64];
            if (d->cabac) {
                int tc = read_block(d, bs, 5, 64, 0, -1, coef);
                if (tc < 0) return H264_ERR_CORRUPT;
                for (int k = 0; k < 4; k++) mi->nz[b * 4 + k] = (uint8_t)tc;
            } else {
                for (int i = 0; i < 64; i++) coef[i] = 0;
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = b * 4 + i4, t[64];
                    int qx = i4x[blk], qy = i4y[blk];
                    int tc = read_block(d, bs, 2, 16, luma_nC(d, addr, qx, qy), 0, t);
                    if (tc < 0) return H264_ERR_CORRUPT;
                    mi->nz[blk] = (uint8_t)tc;
                    for (int i = 0; i < 16; i++) coef[4 * i + i4] = t[i];
                }
            }
            h264_dequant_idct8_add(coef, qpy, ls8, dst, s);
        } else {
            int coef[64];
            int tc = read_block(d, bs, 2, 16, luma_nC(d, addr, bx, by),
                                d->cabac ? cbf_inc_luma(d, addr, 2, bx, by) : 0,
                                coef);
            if (tc < 0) return H264_ERR_CORRUPT;
            mi->nz[b] = (uint8_t)tc;
            if (tc) h264_dequant_idct_add(coef, qpy, ls, dst, s);
        }
    }

    chroma_intra_pred(d, addr, chroma_mode);
    return residual_chroma(d, bs, addr, cbp_chroma, qpy, 1);
}

static int decode_i16(h264dec *d, bs_t *bs, int addr, int *qpyp, int t16)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    const pps_t *pps = d->cur_pps;
    mi->type = MB_I16x16;
    int mode = t16 & 3;
    int cbpcode = t16 >> 2;             /* 0..5: chroma = %3, luma15 = >= 3 */
    int cbp_luma = cbpcode >= 3 ? 15 : 0;
    int cbp_chroma = cbpcode % 3;
    mi->intra16_mode = (uint8_t)mode;
    mi->cbp = (uint8_t)((cbp_chroma << 4) | cbp_luma);

    int chroma_mode;
    if (d->cabac) chroma_mode = cab_chroma_pred_mode(d, addr);
    else {
        uint32_t cm = bs_ue(bs);
        if (bs_error(bs) || cm > 3) return H264_ERR_CORRUPT;
        chroma_mode = (int)cm;
    }
    mi->chroma_mode = (uint8_t)chroma_mode;

    int qpy = *qpyp, delta;
    if (d->cabac) { delta = cab_qp_delta(d); d->last_qp_delta = delta; }
    else { delta = (int)bs_se(bs); if (bs_error(bs)) return H264_ERR_CORRUPT; }
    if (delta < -87 || delta > 77) return H264_ERR_CORRUPT;
    qpy = ((qpy + delta) % 52 + 52) % 52;
    *qpyp = qpy;
    mi->qp = (int8_t)qpy;

    /* 16x16 luma prediction */
    int al = h264_intra_avail(d, addr, mbx - 1, mby);
    int at = h264_intra_avail(d, addr, mbx, mby - 1);
    int atl = h264_intra_avail(d, addr, mbx - 1, mby - 1);
    int s = d->stride_y, px = mbx * 16, py = mby * 16;
    uint8_t top16[16], left16[16];
    int tl = atl ? d->cur->y[(long)(py - 1) * s + px - 1] : 0;
    if (at) memcpy(top16, d->cur->y + (long)(py - 1) * s + px, 16);
    else memset(top16, 0, 16);
    if (al) for (int i = 0; i < 16; i++) left16[i] = d->cur->y[(long)(py + i) * s + px - 1];
    else memset(left16, 0, 16);
    h264_intra16x16(d->cur->y + (long)py * s + px, s, mode, top16, left16, tl, al, at);

    /* Luma DC chain (8.5.10): Hadamard first, THEN scale. Scaling the levels
     * before the transform is equivalent while the shift is exact, and is off
     * by a rounding step once qP drops below 12 -- and with a scaling matrix
     * it is not even equivalent, because the per-coefficient factors differ. */
    const int *ls = pps->ls4[ls_idx(1, 0)][qpy % 6];
    int dc[16];
    {
        int coef[64];
        int tc = read_block(d, bs, 0, 16, i16dc_nC(d, addr),
                            d->cabac ? cbf_inc_simple(d, addr, 0, 0) : 0, coef);
        if (tc < 0) return H264_ERR_CORRUPT;
        mi->nz_i16dc = (uint8_t)tc;
        for (int i = 0; i < 16; i++) dc[zz4[i]] = coef[i];
        h264_dc16_transform(dc);
        int k = qpy / 6;
        for (int i = 0; i < 16; i++) {
            int p = dc[i] * ls[0];
            dc[i] = k >= 6 ? p * (1 << (k - 6)) : (p + (1 << (5 - k))) >> (6 - k);
        }
    }
    /* AC blocks */
    for (int blk = 0; blk < 16; blk++) {
        int bx = i4x[blk], by = i4y[blk];
        int ac[64], *acp = 0;
        if (cbp_luma) {
            int tc = read_block(d, bs, 1, 15, luma_nC(d, addr, bx, by),
                                d->cabac ? cbf_inc_luma(d, addr, 1, bx, by) : 0,
                                ac);
            if (tc < 0) return H264_ERR_CORRUPT;
            acp = ac;
            mi->nz[blk] = (uint8_t)tc;
        }
        if (dc[by * 4 + bx] || acp) {
            uint8_t *dst = d->cur->y + (long)(py + by * 4) * s + px + bx * 4;
            idct_add_dc_ac(dc[by * 4 + bx], acp, qpy, ls, dst, s);
        }
    }

    chroma_intra_pred(d, addr, chroma_mode);
    return residual_chroma(d, bs, addr, cbp_chroma, qpy, 1);
}

static int decode_ipcm(h264dec *d, bs_t *bs, int addr)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    mi->type = MB_I_PCM;
    mi->qp = 0;                                 /* spec: PCM blocks use qP 0 */
    mi->cbp = 47;
    d->last_qp_delta = 0;                       /* codes no mb_qp_delta */

    if (d->cabac) {
        /* 7.3.5: the samples sit byte aligned in the RAW bitstream, not in the
         * arithmetic code. This engine tracks a true bit position, so the
         * alignment is just the next byte boundary; a decoder that reads ahead
         * in words has to unwind its lookahead here instead, which is where
         * I_PCM under CABAC usually breaks. */
        int pos = d->cab.bytepos + (d->cab.bitpos ? 1 : 0);
        if (d->cab.len - pos < 384) return H264_ERR_CORRUPT;
        const uint8_t *p = d->cab.buf + pos;
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                d->cur->y[(long)(mby * 16 + i) * d->stride_y + mbx * 16 + j] = *p++;
        for (int comp = 0; comp < 2; comp++) {
            uint8_t *plane = comp ? d->cur->v : d->cur->u;
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 8; j++)
                    plane[(long)(mby * 8 + i) * d->stride_c + mbx * 8 + j] = *p++;
        }
        /* 9.3.1.2: the ENGINE restarts after the samples. The context model
         * does not -- it carries on across the macroblock. */
        h264_cabac_restart(&d->cab, pos + 384);
    } else {
        bs_align(bs);
        if (bs_error(bs) || bs_bits_left(bs) < (256 + 64 + 64) * 8)
            return H264_ERR_CORRUPT;
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++)
                d->cur->y[(long)(mby * 16 + i) * d->stride_y + mbx * 16 + j] =
                    (uint8_t)bs_u(bs, 8);
        for (int comp = 0; comp < 2; comp++) {
            uint8_t *plane = comp ? d->cur->v : d->cur->u;
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 8; j++)
                    plane[(long)(mby * 8 + i) * d->stride_c + mbx * 8 + j] =
                        (uint8_t)bs_u(bs, 8);
        }
    }
    for (int i = 0; i < 24; i++) mi->nz[i] = 16;
    mi->nz_i16dc = 16;
    mi->nz_cdc[0] = mi->nz_cdc[1] = 16;
    return H264_OK;
}

/* ============================================================= inter MBs == */
/* P macroblock partition layout (Table 7-13) plus its sub_mb_types. */
static int p_parts(h264dec *d, bs_t *bs, int mb_type, part_t *parts)
{
    if (mb_type == 0) { parts[0] = (part_t){ 0, 0, 4, 4, 0, 0, 0 }; return 1; }
    if (mb_type == 1) {
        parts[0] = (part_t){ 0, 0, 4, 2, 1, 0, 0 };
        parts[1] = (part_t){ 0, 2, 4, 2, 1, 1, 0 };
        return 2;
    }
    if (mb_type == 2) {
        parts[0] = (part_t){ 0, 0, 2, 4, 2, 0, 0 };
        parts[1] = (part_t){ 2, 0, 2, 4, 2, 1, 0 };
        return 2;
    }
    int n = 0;
    for (int r = 0; r < 4; r++) {
        int st;
        if (d->cabac) st = cab_sub_type_p(d);
        else {
            uint32_t v = bs_ue(bs);
            if (bs_error(bs) || v > 3) return -1;
            st = (int)v;
        }
        int px = (r & 1) * 2, py = (r >> 1) * 2;
        static const uint8_t sw[4] = { 2, 2, 1, 1 }, sh[4] = { 2, 1, 2, 1 };
        static const uint8_t sn[4] = { 1, 2, 2, 4 };
        for (int k = 0; k < sn[st]; k++) {
            int ox = (st == 2 || st == 3) ? (k & 1) : 0;
            int oy = (st == 1) ? k : (st == 3 ? (k >> 1) : 0);
            parts[n++] = (part_t){ px + ox, py + oy, sw[st], sh[st], 0, r, 0 };
        }
    }
    return n;
}

/* The 8x8 quadrant a partition lives in -- the index mi->ref_idx[] is stored
 * and read back under. */
static int part_quadrant(const part_t *pt)
{
    return (pt->py >> 1) * 2 + (pt->px >> 1);
}

/* Read one reference index for a partition, or 0 when the list has a single
 * entry and the element is therefore not coded. */
static int read_ref_idx(h264dec *d, bs_t *bs, int addr, int list, const part_t *pt)
{
    int n = d->n_rl[list];
    if (n <= 1) return 0;
    if (d->cabac) {
        int mbx = addr % d->mbw, mby = addr / d->mbw;
        return cab_ref_idx(d, addr, mbx, mby, pt->px, pt->py, list, n - 1);
    }
    int v = (int)bs_te(bs, (uint32_t)n - 1);
    if (bs_error(bs)) return -1;
    return v;
}

/* mb_pred / sub_mb_pred for a P or B macroblock, then MC and residual. */
static int decode_inter_mb(h264dec *d, bs_t *bs, slice_t *sl, int addr,
                           int *qpyp, part_t *parts, int npart, int nregion,
                           int is_b, int direct_mask, int all_8x8)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    const sps_t *sps = d->cur_sps;
    mi->type = MB_INTER;
    mi->direct8x8 = (uint8_t)direct_mask;

    /* Direct motion is derived for the whole macroblock at once -- spatial
     * direct picks its reference indices from the macroblock's neighbours, so
     * they cannot be derived per sub-macroblock -- and then applied only to
     * the 8x8s that asked for it. */
    if (direct_mask) {
        h264_direct_motion(d, sl, addr);
        for (int q = 0; q < 4; q++) {
            if (!(direct_mask & (1 << q))) continue;
            int b0 = (q >> 1) * 8 + (q & 1) * 2;
            for (int l = 0; l < 2; l++) {
                mi->ref_idx[l][q] = d->dir_ref[l][b0];
                mi->ref_pic[l][q] = mi->ref_idx[l][q] < 0 ? -1
                    : (int8_t)(d->rl[l][mi->ref_idx[l][q]] - d->pics);
            }
            for (int by = (q >> 1) * 2; by < (q >> 1) * 2 + 2; by++)
                for (int bx = (q & 1) * 2; bx < (q & 1) * 2 + 2; bx++)
                    for (int l = 0; l < 2; l++) {
                        mi->mv[l][by * 4 + bx][0] = d->dir_mv[l][by * 4 + bx][0];
                        mi->mv[l][by * 4 + bx][1] = d->dir_mv[l][by * 4 + bx][1];
                    }
        }
    }

    /* --- ref_idx, list-major then partition order (7.3.5.1/7.3.5.2) ---
     *
     * Each region's index is published into mbinfo AS SOON as it is read, not
     * after the loop: 9.3.3.1.1.6 picks the ref_idx context from the blocks to
     * the left and above, and for the second partition of a 16x8 (or the
     * fourth 8x8) those blocks are in THIS macroblock. Deferring the write
     * leaves them reading the -1 the macroblock was initialised with, which
     * flips ctxIdxInc and desynchronises the arithmetic decoder -- so the
     * damage is not a wrong reference, it is the rest of the slice. */
    for (int l = 0; l < (is_b ? 2 : 1); l++) {
        for (int r = 0; r < nregion; r++) {
            /* The region's prediction comes from the first partition that
             * lives in it; every partition of a region shares its ref_idx. */
            const part_t *pt = 0;
            for (int p = 0; p < npart; p++)
                if (parts[p].region == r) { pt = &parts[p]; break; }
            if (!pt || pt->pred == 3) continue;      /* direct: nothing coded */
            if (!(pt->pred == l || pt->pred == 2)) continue;
            int v = read_ref_idx(d, bs, addr, l, pt);
            if (v < 0 || v >= d->n_rl[l]) return H264_ERR_CORRUPT;
            for (int q = 0; q < 4; q++) {
                int qr;
                if (nregion == 1)      qr = 0;
                else if (nregion == 2) qr = (parts[0].ph == 2) ? (q >> 1) : (q & 1);
                else                   qr = q;
                if (qr != r || (direct_mask & (1 << q))) continue;
                mi->ref_idx[l][q] = (int8_t)v;
                mi->ref_pic[l][q] = (int8_t)(d->rl[l][v] - d->pics);
            }
        }
    }

    /* --- mvd per (sub-)partition, mvp per 8.4.1.3, list-major --- */
    for (int l = 0; l < (is_b ? 2 : 1); l++) {
        /* 6.4.11.7's "not yet decoded" is about the PARTITION order, and both
         * lists walk that order from the start -- so the mask resets per list,
         * not once per macroblock. Leaving it set from the L0 pass makes every
         * L1 prediction read vectors that have not been written. */
        d->mb_mv_done = 0;
        for (int p = 0; p < npart; p++) {
            part_t *pt = &parts[p];
            int q = part_quadrant(pt);
            if (pt->pred == 3) {
                /* A direct partition IS decoded, at its own place in partition
                 * order -- it is a neighbour for what comes after it and not
                 * for what came before, exactly like a coded one. */
                for (int by = pt->py; by < pt->py + pt->ph; by++)
                    for (int bx = pt->px; bx < pt->px + pt->pw; bx++)
                        d->mb_mv_done |= (uint16_t)(1u << (by * 4 + bx));
                continue;
            }
            int uses = (pt->pred == l || pt->pred == 2);
            int mvx = 0, mvy = 0;
            if (uses) {
                int ref = mi->ref_idx[l][q];
                h264_mv_pred(d, addr, mbx, mby, pt->px, pt->py, pt->pw, pt->ph,
                             l, ref, pt->kind, &mvx, &mvy);
                int dx, dy;
                if (d->cabac) {
                    dx = cab_mvd(d, addr, mbx * 4 + pt->px, mby * 4 + pt->py, l, 0);
                    dy = cab_mvd(d, addr, mbx * 4 + pt->px, mby * 4 + pt->py, l, 1);
                    if (d->cab.error) return H264_ERR_CORRUPT;
                } else {
                    dx = (int)bs_se(bs);
                    dy = (int)bs_se(bs);
                    if (bs_error(bs)) return H264_ERR_CORRUPT;
                }
                mvx = clip3(-8192, 8191, mvx + dx);
                mvy = clip3(-8192, 8191, mvy + dy);
                for (int by = pt->py; by < pt->py + pt->ph; by++)
                    for (int bx = pt->px; bx < pt->px + pt->pw; bx++) {
                        mi->mvd[l][by * 4 + bx][0] = (int16_t)dx;
                        mi->mvd[l][by * 4 + bx][1] = (int16_t)dy;
                    }
            }
            for (int by = pt->py; by < pt->py + pt->ph; by++)
                for (int bx = pt->px; bx < pt->px + pt->pw; bx++) {
                    mi->mv[l][by * 4 + bx][0] = (int16_t)mvx;
                    mi->mv[l][by * 4 + bx][1] = (int16_t)mvy;
                    d->mb_mv_done |= (uint16_t)(1u << (by * 4 + bx));
                }
        }
    }

    /* --- coded_block_pattern, transform_size_8x8_flag, qp --- */
    int cbp;
    if (d->cabac) cbp = cab_cbp_luma(d, addr) | (cab_cbp_chroma(d, addr) << 4);
    else {
        uint32_t codeNum = bs_ue(bs);
        if (bs_error(bs) || codeNum > 47) return H264_ERR_CORRUPT;
        cbp = cbp_inter_tab[codeNum];
    }
    mi->cbp = (uint8_t)cbp;
    int cbp_luma = cbp & 15, cbp_chroma = (cbp >> 4) & 3;

    /* 7.3.5: the flag is only present when there is luma residual to transform
     * AND no partition is smaller than 8x8 -- a 4x8 partition cannot carry an
     * 8x8 transform. A direct partition needs direct_8x8_inference too, since
     * without it the direct motion itself is derived per 4x4; the caller has
     * already folded that into all_8x8. */
    if (d->cur_pps->transform_8x8 && cbp_luma && all_8x8) {
        if (d->cabac) mi->transform8x8 = (uint8_t)cab_transform8x8(d, addr);
        else mi->transform8x8 = (uint8_t)bs_u1(bs);
    }

    int qpy = *qpyp;
    if (cbp_luma || cbp_chroma) {
        int delta;
        if (d->cabac) { delta = cab_qp_delta(d); d->last_qp_delta = delta; }
        else { delta = (int)bs_se(bs); if (bs_error(bs)) return H264_ERR_CORRUPT; }
        if (delta < -87 || delta > 77) return H264_ERR_CORRUPT;
        qpy = ((qpy + delta) % 52 + 52) % 52;
        *qpyp = qpy;
    } else if (d->cabac) {
        d->last_qp_delta = 0;
    }
    mi->qp = (int8_t)qpy;

    /* --- motion compensation, then residual on top --- */
    for (int p = 0; p < npart; p++) {
        part_t *pt = &parts[p];
        if (pt->pred != 3) {
            h264_inter_pred(d, sl, addr, pt->px, pt->py, pt->pw, pt->ph, pt->pred);
            continue;
        }
        /* Direct partitions are compensated block by block: the reference
         * indices are shared across the partition but each block may carry its
         * own vector (8x8 granularity when direct_8x8_inference is set, 4x4
         * when it is not). */
        int q = part_quadrant(pt);
        int step = sps->direct_8x8_inference ? 2 : 1;
        int l0 = mi->ref_idx[0][q] >= 0, l1 = mi->ref_idx[1][q] >= 0;
        for (int by = pt->py; by < pt->py + pt->ph; by += step)
            for (int bx = pt->px; bx < pt->px + pt->pw; bx += step)
                h264_inter_pred(d, sl, addr, bx, by, step, step,
                                l0 && l1 ? 2 : (l0 ? 0 : 1));
    }

    int rc = residual_luma(d, bs, addr, cbp_luma, qpy, 0);
    if (rc) return rc;
    return residual_chroma(d, bs, addr, cbp_chroma, qpy, 0);
}

/* ------------------------------------------------------------- skip ----- */
int h264_decode_skip_mb(h264dec *d, slice_t *sl, int addr, int qpy)
{
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    mbinfo_t *mi = &d->mb[addr];
    memset(mi, 0, sizeof *mi);
    mi->type = MB_SKIP;
    mi->qp = (int8_t)qpy;
    mi->skip = 1;
    /* A skipped macroblock codes no mb_qp_delta, and 9.3.3.1.1.5 asks whether
     * the PREVIOUS macroblock in decoding order had a nonzero one -- so it has
     * to answer "no" for the next macroblock, not leave the last coded value
     * standing. */
    d->last_qp_delta = 0;
    for (int l = 0; l < 2; l++)
        for (int q = 0; q < 4; q++) { mi->ref_idx[l][q] = -1; mi->ref_pic[l][q] = -1; }

    if (sl->slice_type == SLICE_B) {
        /* B_Skip is B_Direct_16x16 without a residual. */
        mi->direct8x8 = 0x0F;
        mi->bdirect16 = 1;
        h264_direct_motion(d, sl, addr);
        for (int q = 0; q < 4; q++)
            for (int l = 0; l < 2; l++) {
                int b = (q >> 1) * 8 + (q & 1) * 2;
                mi->ref_idx[l][q] = d->dir_ref[l][b];
                mi->ref_pic[l][q] = mi->ref_idx[l][q] < 0 ? -1
                    : (int8_t)(d->rl[l][mi->ref_idx[l][q]] - d->pics);
            }
        for (int b = 0; b < 16; b++)
            for (int l = 0; l < 2; l++) {
                mi->mv[l][b][0] = d->dir_mv[l][b][0];
                mi->mv[l][b][1] = d->dir_mv[l][b][1];
            }
        int step = d->cur_sps->direct_8x8_inference ? 2 : 1;
        for (int by = 0; by < 4; by += step)
            for (int bx = 0; bx < 4; bx += step) {
                int q = (by >> 1) * 2 + (bx >> 1);
                int l0 = mi->ref_idx[0][q] >= 0, l1 = mi->ref_idx[1][q] >= 0;
                h264_inter_pred(d, sl, addr, bx, by, step, step,
                                l0 && l1 ? 2 : (l0 ? 0 : 1));
            }
        return H264_OK;
    }

    if (d->n_rl[0] < 1 || !d->rl[0][0]) return H264_ERR_CORRUPT;
    for (int q = 0; q < 4; q++) {                /* P_Skip: refIdxL0 = 0 */
        mi->ref_idx[0][q] = 0;
        mi->ref_pic[0][q] = (int8_t)(d->rl[0][0] - d->pics);
    }
    int mvx, mvy;
    d->mb_mv_done = 0;
    h264_mv_pred_skip(d, addr, mbx, mby, &mvx, &mvy);
    for (int i = 0; i < 16; i++) {
        mi->mv[0][i][0] = (int16_t)mvx;
        mi->mv[0][i][1] = (int16_t)mvy;
    }
    h264_inter_pred(d, sl, addr, 0, 0, 4, 4, 0);
    return H264_OK;
}

/* ================================================= macroblock_layer ====== */
int h264_decode_mb(h264dec *d, bs_t *bs, slice_t *sl, int addr, int *qpyp)
{
    mbinfo_t *mi = &d->mb[addr];
    memset(mi, 0, sizeof *mi);
    for (int l = 0; l < 2; l++)
        for (int q = 0; q < 4; q++) { mi->ref_idx[l][q] = -1; mi->ref_pic[l][q] = -1; }

    int st = sl->slice_type, mb_type;
    if (d->cabac) {
        if (st == SLICE_I)      mb_type = cab_intra_mb_type(d, addr, 3, 1);
        else if (st == SLICE_P) mb_type = cab_mb_type_p(d, addr);
        else                    mb_type = cab_mb_type_b(d, addr);
        if (d->cab.error) return H264_ERR_CORRUPT;
    } else {
        uint32_t v = bs_ue(bs);
        if (bs_error(bs) || v > 48) return H264_ERR_CORRUPT;
        mb_type = (int)v;
    }

    /* --- P --- */
    if (st == SLICE_P && mb_type <= 4) {
        part_t parts[16];
        /* P_8x8ref0 (mb_type 4) is P_8x8 with every ref_idx inferred to 0; it
         * has no CABAC binarisation, which is why only CAVLC can produce it. */
        int npart = p_parts(d, bs, mb_type == 4 ? 3 : mb_type, parts);
        if (npart < 0) return H264_ERR_CORRUPT;
        int nregion = mb_type == 0 ? 1 : (mb_type <= 2 ? 2 : 4);
        int all8 = 1;
        for (int p = 0; p < npart; p++)
            if (parts[p].pw < 2 || parts[p].ph < 2) all8 = 0;
        if (mb_type == 4) {
            int saved[2] = { d->n_rl[0], d->n_rl[1] };
            d->n_rl[0] = 1;                      /* forces every ref_idx to 0 */
            int rc = decode_inter_mb(d, bs, sl, addr, qpyp, parts, npart,
                                     nregion, 0, 0, all8);
            d->n_rl[0] = saved[0]; d->n_rl[1] = saved[1];
            return rc;
        }
        return decode_inter_mb(d, bs, sl, addr, qpyp, parts, npart, nregion,
                               0, 0, all8);
    }

    /* --- B --- */
    if (st == SLICE_B && mb_type <= 22) {
        const bpart_t *bp = &b_mb[mb_type];
        part_t parts[16];
        int npart = 0, nregion, direct_mask = 0, all8 = 1;
        if (mb_type == 0) {                      /* B_Direct_16x16 */
            direct_mask = 0x0F;
            nregion = 0;
            mi->bdirect16 = 1;
            all8 = d->cur_sps->direct_8x8_inference;
            parts[npart++] = (part_t){ 0, 0, 4, 4, 0, 0, 3 };
        } else if (mb_type == 22) {              /* B_8x8 */
            nregion = 4;
            for (int r = 0; r < 4; r++) {
                int t;
                if (d->cabac) t = cab_sub_type_b(d);
                else {
                    uint32_t v = bs_ue(bs);
                    if (bs_error(bs) || v > 12) return H264_ERR_CORRUPT;
                    t = (int)v;
                }
                const bsub_t *bs8 = &b_sub[t];
                int px = (r & 1) * 2, py = (r >> 1) * 2;
                if (bs8->pred == 3) {
                    direct_mask |= 1 << r;
                    if (!d->cur_sps->direct_8x8_inference) all8 = 0;
                    parts[npart++] = (part_t){ px, py, 2, 2, 0, r, 3 };
                    continue;
                }
                if (bs8->pw < 2 || bs8->ph < 2) all8 = 0;
                for (int k = 0; k < bs8->nsub; k++) {
                    int ox = (bs8->pw == 1) ? (k & 1) : 0;
                    int oy = (bs8->ph == 1) ? (bs8->nsub == 4 ? (k >> 1) : k) : 0;
                    parts[npart++] = (part_t){ px + ox, py + oy, bs8->pw,
                                               bs8->ph, 0, r, bs8->pred };
                }
            }
        } else {
            nregion = bp->nparts;
            for (int p = 0; p < bp->nparts; p++) {
                int px = (bp->pw == 2) ? p * 2 : 0;
                int py = (bp->ph == 2) ? p * 2 : 0;
                parts[npart++] = (part_t){ px, py, bp->pw, bp->ph,
                                           bp->pw == 4 && bp->ph == 2 ? 1
                                         : (bp->pw == 2 && bp->ph == 4 ? 2 : 0),
                                           p, bp->pred[p] };
            }
        }
        return decode_inter_mb(d, bs, sl, addr, qpyp, parts, npart, nregion,
                               1, direct_mask, all8);
    }

    /* --- intra, in any slice type --- */
    int t = mb_type - (st == SLICE_P ? 5 : (st == SLICE_B ? 23 : 0));
    if (t < 0 || t > 25) return H264_ERR_CORRUPT;
    if (t == 25) return decode_ipcm(d, bs, addr);
    if (t == 0) {
        mi->type = MB_I4x4;
        if (d->cur_pps->transform_8x8) {
            if (d->cabac) mi->transform8x8 = (uint8_t)cab_transform8x8(d, addr);
            else mi->transform8x8 = (uint8_t)bs_u1(bs);
        }
        return decode_i_nxn(d, bs, addr, qpyp, 0);
    }
    return decode_i16(d, bs, addr, qpyp, t - 1);
}
