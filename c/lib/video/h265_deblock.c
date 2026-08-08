/* c/lib/video/h265_deblock.c -- the in-loop deblocking filter (8.7.2) and the
 * sample adaptive offset (8.7.3).
 *
 * Two filters where H.264 had one, and they run in this order over the whole
 * picture: every vertical edge first, then every horizontal edge, then SAO on
 * top of the result. The ordering is not a detail. Deblocking a horizontal
 * edge reads samples that the vertical pass has already rewritten, and SAO
 * reads the deblocked picture rather than the one it is writing -- which is
 * why sao_pic() works from a full copy. Doing SAO in place gives a picture
 * that is right almost everywhere and drifts along every edge classification
 * boundary.
 *
 * The filter kernels are exported so tests/unit/h265_deblock_test.c can drive
 * them directly: the strong/weak decision, the tC and beta clamps and the SAO
 * edge classifier are all small exactly-specified functions, and a whole-frame
 * test can only tell you that one of them is wrong.
 *
 * Boundary strength asks whether the two sides are predicted from the same
 * reference PICTURES -- not from the same list positions. The note in 8.7.2.4
 * is explicit about it, and weighted prediction puts one picture at several
 * indices on purpose, so the per-block state carries the referenced POC
 * alongside the ref_idx. That is the same trap the H.264 decoder documents,
 * and it survives into HEVC unchanged.
 */
#include <string.h>
#include <stdlib.h>
#include "h265.h"
#include "h265_int.h"

/* Table 8-12: beta' indexed by Q (0..51), tC' indexed by Q (0..53). */
static const uint8_t beta_tab[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24,
    26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
    58, 60, 62, 64
};
static const uint8_t tc_tab[54] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  8,  9, 10, 11, 13,
    14, 16, 18, 20, 22, 24
};

/* Table 8-10: qPi -> QpC for ChromaArrayType == 1 (4:2:0). */
int h265_chroma_qp(int qpi)
{
    static const uint8_t map[14] = { 29,30,31,32,33,33,34,34,35,35,36,36,37,37 };
    if (qpi < 30) return qpi;
    if (qpi > 43) return qpi - 6;
    return map[qpi - 30];
}

/* ===================== the filter kernels (8.7.2.5.3 / .7) ============== */
/* `base` addresses q0 of the first line. `dstep` steps from q0 towards q1
 * (across the edge), `lstep` steps to the next line along the edge. So a
 * vertical edge passes dstep = 1, lstep = stride, and a horizontal edge
 * passes dstep = stride, lstep = 1 -- one kernel for both directions. */
#define P(k, l) base[(long)(l) * lstep - (long)((k) + 1) * dstep]
#define Q(k, l) base[(long)(l) * lstep + (long)(k) * dstep]

static int strong_ok(const uint16_t *base, int dstep, int lstep, int l,
                     int beta, int tc, int dpq)
{
    int a = P(3, l) - P(0, l); if (a < 0) a = -a;
    int b = Q(0, l) - Q(3, l); if (b < 0) b = -b;
    int c = P(0, l) - Q(0, l); if (c < 0) c = -c;
    return (2 * dpq < (beta >> 2)) && (a + b < (beta >> 3)) &&
           (c < ((5 * tc + 1) >> 1));
}

/* One 4-line luma segment. no_p / no_q suppress writes on that side
 * (cu_transquant_bypass, or PCM with pcm_loop_filter_disabled). */
/* `beta` and `tc` arrive ALREADY scaled to the sample depth by the caller
 * (8.7.2.5.3: beta = beta' * (1 << (BitDepth - 8)), likewise tC), because they
 * are derived per edge from a QP the caller owns. `bd` is here only for the
 * Clip1Y at the end of the weak filter. */
void h265_deblock_luma4(uint16_t *base, int dstep, int lstep,
                        int beta, int tc, int no_p, int no_q, int bd)
{
    if (tc <= 0 && beta <= 0) return;
    int dp0 = abs(P(2, 0) - 2 * P(1, 0) + P(0, 0));
    int dp3 = abs(P(2, 3) - 2 * P(1, 3) + P(0, 3));
    int dq0 = abs(Q(2, 0) - 2 * Q(1, 0) + Q(0, 0));
    int dq3 = abs(Q(2, 3) - 2 * Q(1, 3) + Q(0, 3));
    int dpq0 = dp0 + dq0, dpq3 = dp3 + dq3;
    int dp = dp0 + dp3, dq = dq0 + dq3;
    int d = dpq0 + dpq3;
    if (d >= beta) return;

    int strong = strong_ok(base, dstep, lstep, 0, beta, tc, dpq0) &&
                 strong_ok(base, dstep, lstep, 3, beta, tc, dpq3);
    int dep = (dp < ((beta + (beta >> 1)) >> 3));
    int deq = (dq < ((beta + (beta >> 1)) >> 3));

    for (int l = 0; l < 4; l++) {
        int p0 = P(0, l), p1 = P(1, l), p2 = P(2, l), p3 = P(3, l);
        int q0 = Q(0, l), q1 = Q(1, l), q2 = Q(2, l), q3 = Q(3, l);
        if (strong) {
            if (!no_p) {
                P(0, l) = (uint16_t)h265_clip3(p0 - 2 * tc, p0 + 2 * tc,
                    (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                P(1, l) = (uint16_t)h265_clip3(p1 - 2 * tc, p1 + 2 * tc,
                    (p2 + p1 + p0 + q0 + 2) >> 2);
                P(2, l) = (uint16_t)h265_clip3(p2 - 2 * tc, p2 + 2 * tc,
                    (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            }
            if (!no_q) {
                Q(0, l) = (uint16_t)h265_clip3(q0 - 2 * tc, q0 + 2 * tc,
                    (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                Q(1, l) = (uint16_t)h265_clip3(q1 - 2 * tc, q1 + 2 * tc,
                    (p0 + q0 + q1 + q2 + 2) >> 2);
                Q(2, l) = (uint16_t)h265_clip3(q2 - 2 * tc, q2 + 2 * tc,
                    (p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3);
            }
        } else {
            int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (abs(delta) >= tc * 10) continue;
            delta = h265_clip3(-tc, tc, delta);
            if (!no_p) P(0, l) = (uint16_t)h265_clip_pix(p0 + delta, bd);
            if (!no_q) Q(0, l) = (uint16_t)h265_clip_pix(q0 - delta, bd);
            if (dep && !no_p) {
                int dpv = h265_clip3(-(tc >> 1), tc >> 1,
                                     (((p2 + p0 + 1) >> 1) - p1 + delta) >> 1);
                P(1, l) = (uint16_t)h265_clip_pix(p1 + dpv, bd);
            }
            if (deq && !no_q) {
                int dqv = h265_clip3(-(tc >> 1), tc >> 1,
                                     (((q2 + q0 + 1) >> 1) - q1 - delta) >> 1);
                Q(1, l) = (uint16_t)h265_clip_pix(q1 + dqv, bd);
            }
        }
    }
}

/* Chroma, 8.7.2.5.5: no strong/weak decision at all, one sample each side. */
void h265_deblock_chroma_n(uint16_t *base, int dstep, int lstep, int n,
                           int tc, int no_p, int no_q, int bd)
{
    for (int l = 0; l < n; l++) {
        int p0 = P(0, l), p1 = P(1, l), q0 = Q(0, l), q1 = Q(1, l);
        /* * 4 rather than << 2: q0 - p0 is signed and often negative, and a
         * left shift of a negative value is undefined in C. */
        int delta = h265_clip3(-tc, tc, (((q0 - p0) * 4 + p1 - q1 + 4) >> 3));
        if (!no_p) P(0, l) = (uint16_t)h265_clip_pix(p0 + delta, bd);
        if (!no_q) Q(0, l) = (uint16_t)h265_clip_pix(q0 - delta, bd);
    }
}
#undef P
#undef Q

/* ======================== boundary strength (8.7.2.4) =================== */
static int same_pic_set(const bi_t *a, const bi_t *b)
{
    /* Do the two blocks reference the same SET of pictures? Compared by POC,
     * because that is what identifies a picture; list position must not
     * enter into it. */
    int na = (a->pred_flag & 1) + ((a->pred_flag >> 1) & 1);
    int nb = (b->pred_flag & 1) + ((b->pred_flag >> 1) & 1);
    if (na != nb) return 0;
    int ap[2], bp[2], ai = 0, bi2 = 0;
    for (int l = 0; l < 2; l++) {
        if (a->pred_flag & (1 << l)) ap[ai++] = a->refpoc[l];
        if (b->pred_flag & (1 << l)) bp[bi2++] = b->refpoc[l];
    }
    if (na == 1) return ap[0] == bp[0];
    return (ap[0] == bp[0] && ap[1] == bp[1]) ||
           (ap[0] == bp[1] && ap[1] == bp[0]);
}

static int mv_far(const int16_t a[2], const int16_t b[2])
{
    return abs(a[0] - b[0]) >= 4 || abs(a[1] - b[1]) >= 4;
}

int h265_bs(const bi_t *p, const bi_t *q, int tu_edge)
{
    if (p->pred_mode == MODE_INTRA || q->pred_mode == MODE_INTRA) return 2;
    if (tu_edge && (p->cbf_luma || q->cbf_luma)) return 1;
    if (!same_pic_set(p, q)) return 1;

    int np = (p->pred_flag & 1) + ((p->pred_flag >> 1) & 1);
    if (np == 1) {
        int lp = (p->pred_flag & 1) ? 0 : 1;
        int lq = (q->pred_flag & 1) ? 0 : 1;
        return mv_far(p->mv[lp], q->mv[lq]) ? 1 : 0;
    }
    /* Two vectors each. When the two referenced pictures are DIFFERENT the
     * pairing is forced; when they are the same picture either pairing is
     * allowed, and bS is 0 if either one is close enough. */
    if (p->refpoc[0] != p->refpoc[1]) {
        /* match q's lists to p's by picture */
        int q0 = (q->refpoc[0] == p->refpoc[0]) ? 0 : 1;
        int q1 = 1 - q0;
        return (mv_far(p->mv[0], q->mv[q0]) || mv_far(p->mv[1], q->mv[q1])) ? 1 : 0;
    }
    int direct = mv_far(p->mv[0], q->mv[0]) || mv_far(p->mv[1], q->mv[1]);
    int cross  = mv_far(p->mv[0], q->mv[1]) || mv_far(p->mv[1], q->mv[0]);
    return (direct && cross) ? 1 : 0;
}

/* ====================== the picture-level deblocking pass =============== */
/* Is the edge between the CTBs containing (xp,yp) and (xq,yq) allowed to be
 * filtered? Slice and tile boundaries can switch it off. */
static int edge_allowed(h265dec *d, int xp, int yp, int xq, int yq)
{
    const bi_t *bp = h265_bi(d, xp, yp), *bq = h265_bi(d, xq, yq);
    if (bp->slice_idx != bq->slice_idx) {
        /* The flag of the slice containing q governs its own left/top edge. */
        int ctb = (yq >> d->cur_sps->log2_ctb) * d->cur_sps->ctb_width +
                  (xq >> d->cur_sps->log2_ctb);
        if (!d->ctb_filt_across_slice[ctb]) return 0;
        int ctbp = (yp >> d->cur_sps->log2_ctb) * d->cur_sps->ctb_width +
                   (xp >> d->cur_sps->log2_ctb);
        if (!d->ctb_filt_across_slice[ctbp] && bp->slice_idx > bq->slice_idx)
            return 0;
    }
    if (bp->tile_idx != bq->tile_idx && !d->cur_pps->loop_filter_across_tiles)
        return 0;
    return 1;
}

static void deblock_dir(h265dec *d, int vertical)
{
    const sps_t *sps = d->cur_sps;
    const pps_t *pps = d->cur_pps;
    pic_t *pic = d->cur;
    int W = sps->width, H = sps->height;
    int bdy = sps->bit_depth_luma, bdc = sps->bit_depth_chroma;
    int qpbd_c = 6 * (bdc - 8);

    /* Luma edges sit on the 8x8 grid; chroma edges on the 16x16 luma grid. */
    for (int y = 0; y < H; y += 8) {
        for (int x = 0; x < W; x += 8) {
            int ex = vertical ? x : x, ey = vertical ? y : y;
            if (vertical && ex == 0) continue;
            if (!vertical && ey == 0) continue;
            const bi_t *bq = h265_bi(d, ex, ey);
            if (!(bq->bnd & (vertical ? 1 : 2))) continue;
            int xp = vertical ? ex - 1 : ex, yp = vertical ? ey : ey - 1;
            int ctb = (ey >> sps->log2_ctb) * sps->ctb_width + (ex >> sps->log2_ctb);
            if (d->ctb_deblock_disabled[ctb]) continue;
            if (!edge_allowed(d, xp, yp, ex, ey)) continue;

            /* A TU edge is one the transform tree drew; a PU-only edge gets
             * bS from the motion comparison alone. Both were recorded as bnd
             * bits during decode, with bit 2/3 marking the TU case. */
            int tu_edge = (bq->bnd & (vertical ? 4 : 8)) != 0;

            /* Two 4-line segments per 8-sample edge, each with its own
             * neighbour on the p side. */
            for (int seg = 0; seg < 2; seg++) {
                int sx = vertical ? ex : ex + seg * 4;
                int sy = vertical ? ey + seg * 4 : ey;
                if (sx >= W || sy >= H) continue;
                const bi_t *q2 = h265_bi(d, sx, sy);
                const bi_t *p2 = h265_bi(d, vertical ? sx - 1 : sx,
                                            vertical ? sy : sy - 1);
                int bs = h265_bs(p2, q2, tu_edge);
                if (bs == 0) continue;

                int qpl = (q2->qp + p2->qp + 1) >> 1;
                int qb = h265_clip3(0, 51, qpl + d->ctb_beta[ctb]);
                /* 8.7.2.5.3: the tabulated beta'/tC' are 8-bit values; both
                 * scale by 1 << (BitDepth - 8). Leaving them unscaled at 10
                 * bits makes the filter roughly four times too timid --
                 * a picture that looks fine and is wrong on every edge. */
                int beta = beta_tab[qb] << (bdy - 8);
                int qt = h265_clip3(0, 53, qpl + 2 * (bs - 1) + d->ctb_tc[ctb]);
                int tc = tc_tab[qt] << (bdy - 8);
                int no_p = p2->bypass, no_q = q2->bypass;

                uint16_t *base = pic->y + (long)sy * pic->stride_y + sx;
                if (tc > 0 || beta > 0)
                    h265_deblock_luma4(base, vertical ? 1 : pic->stride_y,
                                       vertical ? pic->stride_y : 1,
                                       beta, tc, no_p, no_q, bdy);
            }

            /* Chroma: bS must be 2, and only every other edge of the luma
             * grid carries a chroma edge in 4:2:0. */
            if (vertical ? (ex & 15) : (ey & 15)) continue;
            for (int seg = 0; seg < 2; seg++) {
                int sx = vertical ? ex : ex + seg * 4;
                int sy = vertical ? ey + seg * 4 : ey;
                if (sx >= W || sy >= H) continue;
                const bi_t *q2 = h265_bi(d, sx, sy);
                const bi_t *p2 = h265_bi(d, vertical ? sx - 1 : sx,
                                            vertical ? sy : sy - 1);
                if (h265_bs(p2, q2, tu_edge) != 2) continue;
                int qpl = (q2->qp + p2->qp + 1) >> 1;
                int no_p = p2->bypass, no_q = q2->bypass;
                for (int c = 0; c < 2; c++) {
                    int off = c ? pps->cr_qp_offset : pps->cb_qp_offset;
                    /* qPi may be NEGATIVE once QpBdOffsetC is non-zero, and it
                     * must stay negative through Table 8-10 -- clamping it to
                     * 0 here and only clamping again after the +2*(bS-1) term
                     * gives a different tC index. */
                    int qpc = h265_chroma_qp(h265_clip3(-qpbd_c, 57, qpl + off));
                    int qt = h265_clip3(0, 53, qpc + 2 + d->ctb_tc[ctb]);
                    int tc = tc_tab[qt] << (bdc - 8);
                    if (tc <= 0) continue;
                    uint16_t *pl = c ? pic->v : pic->u;
                    uint16_t *base = pl + (long)(sy / 2) * pic->stride_c + sx / 2;
                    h265_deblock_chroma_n(base, vertical ? 1 : pic->stride_c,
                                          vertical ? pic->stride_c : 1, 2,
                                          tc, no_p, no_q, bdc);
                }
            }
        }
    }
}

void h265_deblock_pic(h265dec *d)
{
    deblock_dir(d, 1);      /* every vertical edge first (8.7.2) */
    deblock_dir(d, 0);      /* then every horizontal one */
}

/* ============================== SAO (8.7.3) ============================= */
static const int8_t eo_h[4][2] = { {-1, 1}, { 0, 0}, {-1, 1}, { 1,-1} };
static const int8_t eo_v[4][2] = { { 0, 0}, {-1, 1}, {-1, 1}, {-1, 1} };

static int sgn(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

/* Exported for the unit test: classify one sample against its two neighbours
 * and return the index into SaoOffsetVal (0 means "leave it alone"). */
int h265_sao_edge_idx(int cur, int a, int b)
{
    int e = 2 + sgn(cur - a) + sgn(cur - b);
    if (e <= 2) e = (e == 2) ? 0 : e + 1;
    return e;
}

/* 8.7.3.2: the band index is sample >> (BitDepth - 5) -- there are always 32
 * bands, so the shift grows with the depth. A hardcoded >> 3 puts a 10-bit
 * sample in a band four times too high, which only misbehaves where the band
 * offsets are actually non-zero and is therefore content-dependent. */
int h265_sao_band_idx(int sample, int band_position, int bd)
{
    int k = (sample >> (bd - 5)) - band_position;
    if (k < 0) k += 32;
    return (k < 4) ? k + 1 : 0;
}

/* Is (x,y) usable as an SAO neighbour of (cx,cy)? Component coordinates; the
 * shift converts to luma for the slice/tile lookups. */
static int sao_nb_ok(h265dec *d, int cx, int cy, int x, int y, int shift, int cw, int ch)
{
    if (x < 0 || y < 0 || x >= cw || y >= ch) return 0;
    const bi_t *bc = h265_bi(d, cx << shift, cy << shift);
    const bi_t *bn = h265_bi(d, x << shift, y << shift);
    if (bn->slice_idx != bc->slice_idx) {
        const sps_t *sps = d->cur_sps;
        int ctb = ((cy << shift) >> sps->log2_ctb) * sps->ctb_width +
                  ((cx << shift) >> sps->log2_ctb);
        if (!d->ctb_filt_across_slice[ctb]) return 0;
    }
    if (bn->tile_idx != bc->tile_idx && !d->cur_pps->loop_filter_across_tiles)
        return 0;
    return 1;
}

void h265_sao_pic(h265dec *d)
{
    const sps_t *sps = d->cur_sps;
    pic_t *pic = d->cur;
    if (!d->sao || !d->sao_src) return;

    /* SAO reads the DEBLOCKED picture and writes a different one. Working in
     * place would feed each sample's own correction into its neighbour's
     * edge classification. */
    int ysz = pic->stride_y * (sps->height + 2 * H265_PAD);
    int csz = pic->stride_c * (sps->height / 2 + 2 * H265_PAD);
    uint16_t *src_y = d->sao_src;
    uint16_t *src_u = src_y + ysz;
    uint16_t *src_v = src_u + csz;
    memcpy(src_y, pic->y - (long)H265_PAD * pic->stride_y - H265_PAD,
           (size_t)ysz * sizeof *src_y);
    memcpy(src_u, pic->u - (long)H265_PAD * pic->stride_c - H265_PAD,
           (size_t)csz * sizeof *src_u);
    memcpy(src_v, pic->v - (long)H265_PAD * pic->stride_c - H265_PAD,
           (size_t)csz * sizeof *src_v);
    src_y += (long)H265_PAD * pic->stride_y + H265_PAD;
    src_u += (long)H265_PAD * pic->stride_c + H265_PAD;
    src_v += (long)H265_PAD * pic->stride_c + H265_PAD;

    for (int cb = 0; cb < sps->ctb_count; cb++) {
        int rx = cb % sps->ctb_width, ry = cb / sps->ctb_width;
        const sao_t *s = &d->sao[cb];
        for (int c = 0; c < 3; c++) {
            if (s->type[c] == SAO_NOT_APPLIED) continue;
            int shift = c ? 1 : 0;
            int cw = c ? sps->width / 2 : sps->width;
            int ch = c ? sps->height / 2 : sps->height;
            uint16_t *dst = c == 0 ? pic->y : c == 1 ? pic->u : pic->v;
            const uint16_t *src = c == 0 ? src_y : c == 1 ? src_u : src_v;
            int stride = c ? pic->stride_c : pic->stride_y;
            int bd = c ? sps->bit_depth_chroma : sps->bit_depth_luma;

            int x0 = (rx << sps->log2_ctb) >> shift;
            int y0 = (ry << sps->log2_ctb) >> shift;
            int x1 = x0 + (sps->ctb_size >> shift), y1 = y0 + (sps->ctb_size >> shift);
            if (x1 > cw) x1 = cw;
            if (y1 > ch) y1 = ch;

            /* SaoOffsetVal = offset << (BitDepth - Min(BitDepth, 10)). That
             * shift is 0 at 8 bits AND at 10, so Main 10 needs no scaling
             * here; it only starts to bite at 12. Written out rather than
             * assumed, because "it happens to be zero" is worth one line. */
            int sao_shift = bd - (bd < 10 ? bd : 10);
            /* Multiply rather than shift: SAO offsets are signed and routinely
             * negative (the last two edge offsets always are), and a left
             * shift of a negative value is undefined in C even when the shift
             * count is zero -- which it is at both 8 and 10 bits, so this
             * never produced a wrong sample and only ever showed up under
             * UBSan. Same reasoning, and the same fix, as the `* 4` in
             * h265_deblock_chroma_n above. */
            int sao_mul = 1 << sao_shift;
            int off[5];
            off[0] = 0;
            for (int i = 0; i < 4; i++) off[i + 1] = s->off[c][i] * sao_mul;

            if (s->type[c] == SAO_BAND) {
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        const bi_t *b = h265_bi(d, x << shift, y << shift);
                        if (b->bypass) continue;
                        int v = src[(long)y * stride + x];
                        int k = h265_sao_band_idx(v, s->clsx[c], bd);
                        if (k) dst[(long)y * stride + x] =
                            (uint16_t)h265_clip_pix(v + off[k], bd);
                    }
            } else {
                int cls = s->clsx[c];
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        const bi_t *b = h265_bi(d, x << shift, y << shift);
                        if (b->bypass) continue;
                        int ax = x + eo_h[cls][0], ay = y + eo_v[cls][0];
                        int bx = x + eo_h[cls][1], by = y + eo_v[cls][1];
                        if (!sao_nb_ok(d, x, y, ax, ay, shift, cw, ch)) continue;
                        if (!sao_nb_ok(d, x, y, bx, by, shift, cw, ch)) continue;
                        int v = src[(long)y * stride + x];
                        int k = h265_sao_edge_idx(v, src[(long)ay * stride + ax],
                                                     src[(long)by * stride + bx]);
                        if (k) dst[(long)y * stride + x] =
                            (uint16_t)h265_clip_pix(v + off[k], bd);
                    }
            }
        }
    }
}
