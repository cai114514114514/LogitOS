/* c/lib/video/h264_deblock.c -- in-loop deblocking filter (ITU-T H.264, 8.7).
 *
 * Post-pass over a finished frame, applied once per macroblock in raster
 * order: for each MB first the four vertical edges (left boundary, then the
 * internal edges at x = 4, 8, 12) and then the four horizontal edges (top
 * boundary, then y = 4, 8, 12). Luma edges are 16 samples long (four
 * independent 4-sample segments, each with its own boundary strength bS);
 * 4:2:0 chroma edges are 8 samples long and are only filtered at the 8x8
 * chroma block boundaries (the luma edges at x/y = 0 and 8).
 *
 * Boundary strength derivation (8.7.2.x):
 *   - MB boundary edge, either side intra .............. bS = 4
 *   - internal edge of an intra MB ..................... bS = 3
 *   - either adjacent 4x4 block has nonzero coeffs ..... bS = 2
 *   - different reference index, or |dmv| >= 4 quarter-pel
 *     (one whole pixel) in either component ............ bS = 1
 *   - otherwise ........................................ bS = 0 (no filtering)
 * Per the spec (derivation of bS for chroma edges), a chroma edge reuses the
 * bS of the luma edge at the same position: chroma sample (x, y) maps to the
 * luma edge segment containing luma sample (2x, 2y), so chroma pixel pair
 * (2i, 2i+1) is filtered with the luma bS of segment i. The chroma nonzero
 * counts in mbinfo are therefore not consulted here.
 *
 * All tables are file-scope constants, there is no mutable global state and
 * no allocation. Every value taken from the caller's mbinfo (qp, mv, ref)
 * is treated as untrusted and clipped/used in bounds-safe form only; the
 * pixel addressing itself never leaves the mbw*mbh macroblock grid, so any
 * plane with at least mbw*16 x mbh*16 luma samples at the given strides is
 * safe. Anything inconsistent (bad geometry, idc == 1, malformed slice
 * list) degrades to a safe no-op rather than an out-of-bounds access.
 */
#include "h264_int.h"

/* --------------------------------------------------- spec tables -------- */
/* Table 8-16: filter thresholds alpha, indexed by indexA 0..51. */
static const uint8_t alpha_tab[52] = {
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     4, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 20, 22, 25, 28,
    32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144,
   162, 182, 203, 226, 255, 255
};

/* Table 8-16: filter thresholds beta, indexed by indexB 0..51. */
static const uint8_t beta_tab[52] = {
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 6, 6, 7, 7, 8, 8,
     9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18
};

/* Table 8-17: tc0 thresholds, [indexA 0..51][bS 1..3]; column 0 is a
 * placeholder (bS = 0 edges are never filtered). */
static const uint8_t tc0_tab[52][4] = {
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1},
    {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 1},
    {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 2},
    {0, 1, 1, 2}, {0, 1, 1, 2}, {0, 1, 1, 2}, {0, 1, 2, 3},
    {0, 1, 2, 3}, {0, 2, 2, 3}, {0, 2, 2, 4}, {0, 2, 3, 4},
    {0, 2, 3, 4}, {0, 3, 3, 5}, {0, 3, 4, 6}, {0, 3, 4, 6},
    {0, 4, 5, 7}, {0, 4, 5, 8}, {0, 4, 6, 9}, {0, 5, 7, 10},
    {0, 6, 8, 11}, {0, 6, 8, 13}, {0, 7, 10, 14}, {0, 8, 11, 16},
    {0, 9, 12, 18}, {0, 10, 13, 20}, {0, 11, 15, 23}, {0, 13, 17, 25}
};

/* Table 8-15 (spec 8.5.8): chroma QP as a function of the clipped luma-based
 * qPC (identity below 30). */
static const uint8_t chroma_qp_tab[52] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36,
    37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

/* ------------------------------------------------------------- helpers --- */
static int clip3(int lo, int hi, int v)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint8_t clip1(int v)              /* Clip1: to the 0..255 sample range */
{
    return (uint8_t)clip3(0, 255, v);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static int is_intra_mb(int type)
{
    return type == MB_I4x4 || type == MB_I16x16 || type == MB_I_PCM;
}

/* Luma QP of a macroblock, defensively clamped into the legal 0..51 range. */
static int mb_qp(const mbinfo_t *m)
{
    return clip3(0, 51, m->qp);
}

/* 8x8 sub-block region index (for ref_idx[4]) of 4x4 block column/row. */
static int region_of(int bcol, int brow)
{
    return (brow >> 1) * 2 + (bcol >> 1);
}

/* --------------------------------------------------- bS derivation ------- */
/* Derive the four segment boundary strengths of one luma edge of the current
 * MB (spec derivation of the content dependent boundary filtering strength).
 * `edge` is 0..3 for the edge at offset 4*edge. For the boundary edges
 * (edge == 0) `mn` is the left/top neighbour MB and the adjacent blocks are
 * its right-most/bottom-most column/row; for internal edges `mn` is NULL and
 * both sides live in `mc`. */
static void luma_bs_vert(const mbinfo_t *mc, const mbinfo_t *mn,
                         int edge, uint8_t bs[4])
{
    if (edge == 0 && (is_intra_mb(mc->type) || is_intra_mb(mn->type))) {
        bs[0] = bs[1] = bs[2] = bs[3] = 4;    /* MB edge, either side intra */
        return;
    }
    if (edge != 0 && is_intra_mb(mc->type)) {
        bs[0] = bs[1] = bs[2] = bs[3] = 3;    /* internal edge of intra MB */
        return;
    }
    for (int i = 0; i < 4; i++) {             /* i: 4x4 block row, 0..3 */
        int bp, bq;                           /* 4x4 block indices, p/q side */
        const mbinfo_t *mp, *mq;
        if (edge == 0) {
            mp = mn; bp = i * 4 + 3;          /* neighbour's right column */
            mq = mc; bq = i * 4 + 0;          /* current MB's left column */
        } else {
            mp = mc; bp = i * 4 + (edge - 1);
            mq = mc; bq = i * 4 + edge;
        }
        if (mp->nz[bp] > 0 || mq->nz[bq] > 0) {
            bs[i] = 2;
        } else if (mp->ref_idx[region_of(bp & 3, bp >> 2)] !=
                   mq->ref_idx[region_of(bq & 3, bq >> 2)] ||
                   iabs(mp->mv[bp][0] - mq->mv[bq][0]) >= 4 ||
                   iabs(mp->mv[bp][1] - mq->mv[bq][1]) >= 4) {
            bs[i] = 1;
        } else {
            bs[i] = 0;
        }
    }
}

static void luma_bs_horz(const mbinfo_t *mc, const mbinfo_t *mn,
                         int edge, uint8_t bs[4])
{
    if (edge == 0 && (is_intra_mb(mc->type) || is_intra_mb(mn->type))) {
        bs[0] = bs[1] = bs[2] = bs[3] = 4;
        return;
    }
    if (edge != 0 && is_intra_mb(mc->type)) {
        bs[0] = bs[1] = bs[2] = bs[3] = 3;
        return;
    }
    for (int i = 0; i < 4; i++) {             /* i: 4x4 block column, 0..3 */
        int bp, bq;
        const mbinfo_t *mp, *mq;
        if (edge == 0) {
            mp = mn; bp = 3 * 4 + i;          /* neighbour's bottom row */
            mq = mc; bq = 0 * 4 + i;          /* current MB's top row */
        } else {
            mp = mc; bp = (edge - 1) * 4 + i;
            mq = mc; bq = edge * 4 + i;
        }
        if (mp->nz[bp] > 0 || mq->nz[bq] > 0) {
            bs[i] = 2;
        } else if (mp->ref_idx[region_of(bp & 3, bp >> 2)] !=
                   mq->ref_idx[region_of(bq & 3, bq >> 2)] ||
                   iabs(mp->mv[bp][0] - mq->mv[bq][0]) >= 4 ||
                   iabs(mp->mv[bp][1] - mq->mv[bq][1]) >= 4) {
            bs[i] = 1;
        } else {
            bs[i] = 0;
        }
    }
}

/* ------------------------------------------------- sample filtering ------ */
/* Filter one luma edge (16 samples, 4 segments of 4). `pix` addresses sample
 * q0 of the first segment; `es` is the stride along the edge, `as` the stride
 * across it (so p0 is pix[-as], p1 pix[-2*as], ...). bS < 4 uses the normal
 * filter (8.7.2.4), bS == 4 the strong filter (8.7.2.5). */
static void filter_luma_edge(uint8_t *pix, int es, int as, const uint8_t bs[4],
                             int qp, int a_off, int b_off)
{
    int indexA = clip3(0, 51, qp + a_off);
    int indexB = clip3(0, 51, qp + b_off);
    int alpha = alpha_tab[indexA];
    int beta = beta_tab[indexB];

    if (alpha == 0 || beta == 0)
        return;                       /* no sample can satisfy the conditions */

    for (int i = 0; i < 4; i++) {
        int strength = bs[i];
        if (strength == 0)
            continue;
        for (int d = 0; d < 4; d++) {
            uint8_t *s = pix + (4 * i + d) * es;
            int p0 = s[-as], p1 = s[-2 * as], p2 = s[-3 * as];
            int q0 = s[0],   q1 = s[as],      q2 = s[2 * as];

            /* the same initial test gates every bS: only edges that look
             * like blocking artifacts are touched */
            if (iabs(p0 - q0) >= alpha ||
                iabs(p1 - p0) >= beta ||
                iabs(q1 - q0) >= beta)
                continue;

            if (strength < 4) {
                int tc0 = tc0_tab[indexA][strength];
                int tc = tc0;
                int avg = (p0 + q0 + 1) >> 1;
                int delta;

                if (iabs(p2 - p0) < beta) {   /* ap < beta: filter p1 too */
                    if (tc0)
                        s[-2 * as] = clip1(p1 + clip3(-tc0, tc0,
                                                      ((p2 + avg) >> 1) - p1));
                    tc++;
                }
                if (iabs(q2 - q0) < beta) {   /* aq < beta: filter q1 too */
                    if (tc0)
                        s[as] = clip1(q1 + clip3(-tc0, tc0,
                                                 ((q2 + avg) >> 1) - q1));
                    tc++;
                }
                delta = clip3(-tc, tc,
                              (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
                s[-as] = clip1(p0 + delta);
                s[0]   = clip1(q0 - delta);
            } else {
                int p3 = s[-4 * as], q3 = s[3 * as];
                int ap = iabs(p2 - p0), aq = iabs(q2 - q0);

                if (ap < beta && iabs(p0 - q0) < ((alpha >> 2) + 2)) {
                    /* small edge on the p side: smooth three samples */
                    s[-as]     = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                    s[-2 * as] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                    s[-3 * as] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
                } else {
                    s[-as] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
                }
                if (aq < beta && iabs(p0 - q0) < ((alpha >> 2) + 2)) {
                    s[0]       = (uint8_t)((q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3);
                    s[as]      = (uint8_t)((q2 + q1 + q0 + p0 + 2) >> 2);
                    s[2 * as]  = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
                } else {
                    s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
                }
            }
        }
    }
}

/* Filter one 4:2:0 chroma edge (8 samples). Per the spec's chroma bS rule,
 * sample pair (2i, 2i+1) reuses the luma bS of segment i. bS < 4 filters one
 * sample per side with tc = tc0 + 1 (8.7.2.4 chroma rule); bS == 4 uses the
 * chroma simplification of 8.7.2.5 (single averaged sample per side). */
static void filter_chroma_edge(uint8_t *pix, int es, int as, const uint8_t bs[4],
                               int qpc, int a_off, int b_off)
{
    int indexA = clip3(0, 51, qpc + a_off);
    int indexB = clip3(0, 51, qpc + b_off);
    int alpha = alpha_tab[indexA];
    int beta = beta_tab[indexB];

    if (alpha == 0 || beta == 0)
        return;

    for (int i = 0; i < 4; i++) {
        int strength = bs[i];
        if (strength == 0)
            continue;
        for (int d = 0; d < 2; d++) {
            uint8_t *s = pix + (2 * i + d) * es;
            int p0 = s[-as], p1 = s[-2 * as];
            int q0 = s[0],   q1 = s[as];

            if (iabs(p0 - q0) >= alpha ||
                iabs(p1 - p0) >= beta ||
                iabs(q1 - q0) >= beta)
                continue;

            if (strength < 4) {
                int tc = tc0_tab[indexA][strength] + 1;   /* chroma: tc0 + 1 */
                int delta = clip3(-tc, tc,
                                  (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
                s[-as] = clip1(p0 + delta);
                s[0]   = clip1(q0 - delta);
            } else {
                s[-as] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
                s[0]   = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

/* ------------------------------------------------- slice edge rules ------ */
/* Map a raster-order MB address to its slice index by binary search over the
 * ascending slice_first_mb[] list. */
static int slice_of(const int *slice_first_mb, int n_slices, int addr)
{
    int lo = 0, hi = n_slices - 1, idx = 0;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (slice_first_mb[mid] <= addr) {
            idx = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return idx;
}

/* With disable_deblocking_filter_idc == 2, edges between macroblocks of
 * different slices are not filtered. A malformed slice list (not ascending,
 * out of range, or not covering MB 0) disables skipping rather than risking
 * a wrong decision on untrusted input. */
static int slice_skip_ok(const int *slice_first_mb, int n_slices, int n_mb)
{
    if (!slice_first_mb || n_slices <= 0)
        return 0;
    if (slice_first_mb[0] != 0)
        return 0;
    for (int i = 1; i < n_slices; i++)
        if (slice_first_mb[i] <= slice_first_mb[i - 1] ||
            slice_first_mb[i] >= n_mb)
            return 0;
    return 1;
}

/* ------------------------------------------------------------- frame ----- */
void h264_deblock_frame(uint8_t *y, uint8_t *u, uint8_t *v,
                        int stride_y, int stride_c, int mbw, int mbh,
                        const mbinfo_t *mb, const slice_t *sl,
                        const sps_t *sps, const pps_t *pps,
                        const int *slice_first_mb, int n_slices)
{
    int n_mb, a_off, b_off, c_off_cb, c_off_cr, check_slices;
    (void)sps;                      /* baseline frames only: nothing to read */

    /* defensive geometry validation: everything below assumes a full MB grid
     * inside each plane; refuse anything we cannot prove safe */
    if (!y || !u || !v || !mb || !sl || !pps)
        return;
    if (mbw <= 0 || mbh <= 0)
        return;
    n_mb = mbw * mbh;
    if (n_mb / mbw != mbh)          /* address overflow */
        return;
    if (stride_y < mbw * 16 || stride_c < mbw * 8)
        return;
    if (sl->disable_deblocking_filter_idc == 1)
        return;                     /* filter fully disabled (contract says the
                                       caller skips us; be safe anyway) */

    a_off = sl->slice_alpha_c0_offset;      /* already FilterOffsetA (x2) */
    b_off = sl->slice_beta_offset;
    c_off_cb = pps->chroma_qp_index_offset;
    c_off_cr = pps->has_second_chroma_offset ? pps->second_chroma_qp_offset
                                             : pps->chroma_qp_index_offset;
    check_slices = sl->disable_deblocking_filter_idc == 2 &&
                   slice_skip_ok(slice_first_mb, n_slices, n_mb);

    for (int mby = 0; mby < mbh; mby++) {
        for (int mbx = 0; mbx < mbw; mbx++) {
            int addr = mby * mbw + mbx;
            const mbinfo_t *mc = mb + addr;
            uint8_t *yq = y + (size_t)mby * 16 * stride_y + (size_t)mbx * 16;
            uint8_t *cb = u + (size_t)mby * 8 * stride_c + (size_t)mbx * 8;
            uint8_t *cr = v + (size_t)mby * 8 * stride_c + (size_t)mbx * 8;
            int qp_cur = mb_qp(mc);
            int qpc_cb_cur = chroma_qp_tab[clip3(0, 51, qp_cur + c_off_cb)];
            int qpc_cr_cur = chroma_qp_tab[clip3(0, 51, qp_cur + c_off_cr)];
            uint8_t bs[4];

            /* ---- four vertical edges: left boundary, then x = 4, 8, 12 -- */
            for (int e = 0; e < 4; e++) {
                int qp_e, qpc_cb, qpc_cr;
                if (e == 0) {
                    const mbinfo_t *mn;
                    if (mbx == 0)
                        continue;                 /* picture boundary */
                    if (check_slices &&
                        slice_of(slice_first_mb, n_slices, addr) !=
                        slice_of(slice_first_mb, n_slices, addr - 1))
                        continue;                 /* slice boundary, idc == 2 */
                    mn = mc - 1;
                    qp_e = (qp_cur + mb_qp(mn) + 1) >> 1;
                    qpc_cb = (qpc_cb_cur +
                              chroma_qp_tab[clip3(0, 51, mb_qp(mn) + c_off_cb)] + 1) >> 1;
                    qpc_cr = (qpc_cr_cur +
                              chroma_qp_tab[clip3(0, 51, mb_qp(mn) + c_off_cr)] + 1) >> 1;
                    luma_bs_vert(mc, mn, 0, bs);
                } else {
                    qp_e = qp_cur;
                    qpc_cb = qpc_cb_cur;
                    qpc_cr = qpc_cr_cur;
                    luma_bs_vert(mc, NULL, e, bs);
                }
                if (bs[0] == 0 && bs[1] == 0 && bs[2] == 0 && bs[3] == 0)
                    continue;
                filter_luma_edge(yq + 4 * e, stride_y, 1, bs, qp_e, a_off, b_off);
                if (e == 0 || e == 2) {
                    /* 4:2:0 chroma edges sit at luma x = 0 and x = 8 only */
                    filter_chroma_edge(cb + 2 * e, stride_c, 1, bs,
                                       qpc_cb, a_off, b_off);
                    filter_chroma_edge(cr + 2 * e, stride_c, 1, bs,
                                       qpc_cr, a_off, b_off);
                }
            }

            /* ------ four horizontal edges: top boundary, then y = 4, 8, 12  */
            for (int e = 0; e < 4; e++) {
                int qp_e, qpc_cb, qpc_cr;
                if (e == 0) {
                    const mbinfo_t *mn;
                    if (mby == 0)
                        continue;
                    if (check_slices &&
                        slice_of(slice_first_mb, n_slices, addr) !=
                        slice_of(slice_first_mb, n_slices, addr - mbw))
                        continue;
                    mn = mc - mbw;
                    qp_e = (qp_cur + mb_qp(mn) + 1) >> 1;
                    qpc_cb = (qpc_cb_cur +
                              chroma_qp_tab[clip3(0, 51, mb_qp(mn) + c_off_cb)] + 1) >> 1;
                    qpc_cr = (qpc_cr_cur +
                              chroma_qp_tab[clip3(0, 51, mb_qp(mn) + c_off_cr)] + 1) >> 1;
                    luma_bs_horz(mc, mn, 0, bs);
                } else {
                    qp_e = qp_cur;
                    qpc_cb = qpc_cb_cur;
                    qpc_cr = qpc_cr_cur;
                    luma_bs_horz(mc, NULL, e, bs);
                }
                if (bs[0] == 0 && bs[1] == 0 && bs[2] == 0 && bs[3] == 0)
                    continue;
                filter_luma_edge(yq + 4 * e * stride_y, 1, stride_y, bs,
                                 qp_e, a_off, b_off);
                if (e == 0 || e == 2) {
                    filter_chroma_edge(cb + 2 * e * stride_c, 1, stride_c, bs,
                                       qpc_cb, a_off, b_off);
                    filter_chroma_edge(cr + 2 * e * stride_c, 1, stride_c, bs,
                                       qpc_cr, a_off, b_off);
                }
            }
        }
    }
}
