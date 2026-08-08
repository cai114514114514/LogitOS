/* c/lib/video/h264.c -- orchestrator: Annex B scanning, NAL dispatch, the
 * slice loop, motion vector prediction, the B-slice direct modes, motion
 * compensation, and the public API.
 *
 * The decoder is a pull model: h264_decode() consumes NAL units until a
 * picture completes (signalled by the first slice of the NEXT picture) and
 * then hands back ONE picture -- but not necessarily the one just decoded.
 * With B slices, decode order is not display order, so a completed picture
 * goes into the DPB and the output process (h264_dpb.c) decides which picture
 * comes out. h264_flush() drains what is left at the end of the stream.
 *
 * The other files: h264_nal.c parses parameter sets and slice headers,
 * h264_dpb.c owns pictures and reference lists, h264_mb.c decodes macroblocks,
 * and h264_cavlc/h264_cabac are the two entropy coders. What stays here is
 * whatever needs to see both the macroblock grid and the reference lists.
 *
 * KNOWN DEFECT, pre-existing and not yet located. On streams that quantise
 * very finely -- macroblock QP around 5..11, or High profile with the JVT
 * scaling matrices, which quantise low frequencies finely for the same effect
 * -- a handful of samples come out one off from ffmpeg. It is sparse (single
 * digits of bytes per megabyte), it is always +-1, and it then rides the
 * reference chain until the next intra macroblock.
 *
 * What is already ruled out, so nobody repeats it: it is not the entropy
 * decoders (a plain Baseline CAVLC stream reproduces it), not the dequantiser
 * (all three scaling rules -- 4x4, chroma DC, Intra_16x16 DC -- were compared
 * exhaustively against ffmpeg's own formulation over the whole coefficient
 * range and agree on every value), not the scaling matrices (parsed and
 * checked in tests/unit/h264_cabac_test.c), and not the deblocking filter
 * (the difference is still there with the loop filter switched off on both
 * sides). And it is not new: the decoder as it stood before CABAC, B slices
 * and the 8x8 transform were added produces the identical wrong bytes, at the
 * identical positions, on the identical stream.
 *
 * Reproduce it with:
 *   ffmpeg -f lavfi -i testsrc2=rate=30:size=176x144 -t 0.5 -c:v libx264 \
 *     -profile:v baseline -x264-params \
 *     cabac=0:bframes=0:partitions=p8x8:aq-mode=1:aq-strength=1.8:qpmin=5 \
 *     -b:v 900k -f h264 lowqp.h264
 * then `make test-h264-diff` style byte counting against ffmpeg's decode.
 */
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "h264_int.h"

uint8_t *h264_nal_to_rbsp(const uint8_t *nal, int len, int *rbsp_len);

/* --------------------------------------------------------------- helpers -- */
static int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static int median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}
static int iabs(int v) { return v < 0 ? -v : v; }

/* qP 30..51 -> qP_C (spec Table 8-15) */
static const int8_t chroma_qp_map[22] = {
    29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36,
    37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

int h264_chroma_qp(int qpy, int offset)
{
    int qpi = clip3(0, 51, qpy + offset);
    return qpi < 30 ? qpi : chroma_qp_map[qpi - 30];
}

int h264_is_intra_type(int t)
{
    return t == MB_I4x4 || t == MB_I16x16 || t == MB_I_PCM;
}

/* slice index of a macroblock address (slice_first_mb is raster-ordered) */
int h264_slice_of(const h264dec *d, int addr)
{
    int s = 0;
    for (int i = 1; i < d->n_slices; i++) {
        if (addr >= d->slice_first_mb[i]) s = i;
        else break;
    }
    return s;
}

/* Is MB (mbx, mby) available as an intra-prediction/mode neighbour of the MB
 * at cur_addr?  constrained_intra_pred hides inter MBs. */
int h264_intra_avail(h264dec *d, int cur_addr, int mbx, int mby)
{
    if (mbx < 0 || mby < 0 || mbx >= d->mbw || mby >= d->mbh) return 0;
    int addr = mby * d->mbw + mbx;
    if (addr > cur_addr) return 0;
    if (h264_slice_of(d, addr) != h264_slice_of(d, cur_addr)) return 0;
    if (d->cur_pps->constrained_intra_pred &&
        !h264_is_intra_type(d->mb[addr].type))
        return 0;
    return 1;
}

/* ============================================================ neighbours == */
typedef struct { int avail; int ref; int mvx, mvy; } nb_t;

/* Inter-prediction neighbour of the 4x4 block at global grid (gx, gy), for one
 * reference list.
 *
 * `avail` answers "is mbAddrN available" in the spec's sense -- inside the
 * picture, same slice, already decoded -- and NOT "does it have a motion
 * vector". An INTRA neighbour is available; 6.4.11.7 just gives it refIdx -1
 * and mv (0, 0). The two are not interchangeable, because two rules test
 * availability itself rather than refIdx:
 *   - P_Skip (8.4.1.1) forces mv (0,0) when A or B is UNAVAILABLE. Treating an
 *     intra neighbour as unavailable pins skipped macroblocks next to intra
 *     ones at zero motion instead of letting them predict.
 *   - the median (8.4.1.3.1) substitutes A for B and C only when B and C are
 *     both unavailable.
 * Everywhere else the two cases coincide, since an unavailable neighbour also
 * contributes refIdx -1 and mv (0, 0). */
static nb_t get_nb(h264dec *d, int cur_addr, int gx, int gy, int list)
{
    nb_t r = { 0, -1, 0, 0 };
    if (gx < 0 || gy < 0 || gx >= d->mbw * 4 || gy >= d->mbh * 4) return r;
    int addr = (gy >> 2) * d->mbw + (gx >> 2);
    /* A macroblock that comes LATER in decoding order is not a neighbour
     * (6.4.9): it has not been decoded, so it has no motion vector to predict
     * from. This matters for the C neighbour, which sits at (px + pw, py - 1)
     * and lands in the macroblock to the RIGHT whenever the partition touches
     * its own macroblock's right edge. Those must fall back to D. */
    if (addr > cur_addr) return r;
    if (addr == cur_addr &&
        !(d->mb_mv_done & (uint16_t)(1u << ((gy & 3) * 4 + (gx & 3)))))
        return r;                              /* same MB, not decoded yet */
    if (h264_slice_of(d, addr) != h264_slice_of(d, cur_addr)) return r;
    const mbinfo_t *m = &d->mb[addr];
    r.avail = 1;
    if (h264_is_intra_type(m->type)) return r; /* available, but refIdx -1 */
    r.ref = m->ref_idx[list][((gy & 3) >> 1) * 2 + ((gx & 3) >> 1)];
    r.mvx = m->mv[list][(gy & 3) * 4 + (gx & 3)][0];
    r.mvy = m->mv[list][(gy & 3) * 4 + (gx & 3)][1];
    return r;
}

/* |mvd| a neighbouring 4x4 block contributes to the CABAC mvd context. An
 * absent, intra or direct neighbour contributes nothing (9.3.3.1.1.7). */
int h264_mvd_ctx(h264dec *d, int addr, int gx, int gy, int list, int comp)
{
    if (gx < 0 || gy < 0 || gx >= d->mbw * 4 || gy >= d->mbh * 4) return 0;
    int n = (gy >> 2) * d->mbw + (gx >> 2);
    if (n > addr) return 0;
    if (n == addr &&
        !(d->mb_mv_done & (uint16_t)(1u << ((gy & 3) * 4 + (gx & 3)))))
        return 0;
    if (h264_slice_of(d, n) != h264_slice_of(d, addr)) return 0;
    const mbinfo_t *m = &d->mb[n];
    if (h264_is_intra_type(m->type) || m->type == MB_SKIP) return 0;
    int q = ((gy & 3) >> 1) * 2 + ((gx & 3) >> 1);
    if (m->direct8x8 & (1 << q)) return 0;
    if (m->ref_idx[list][q] < 0) return 0;
    return iabs(m->mvd[list][(gy & 3) * 4 + (gx & 3)][comp]);
}

/* ======================================================== MV prediction == */
void h264_mv_pred(h264dec *d, int cur, int mbx, int mby,
                  int px, int py, int pw, int ph, int list, int ref,
                  int dir_kind, int *outx, int *outy)
{
    (void)ph;   /* only C steps by the partition size, and it steps in x */
    int gx = mbx * 4 + px, gy = mby * 4 + py;
    /* 6.4.11.7 locates the three neighbours from the partition's TOP-LEFT
     * corner (x, y), and only C steps to the right by the partition width:
     *   A = (x - 1, y)   B = (x, y - 1)   C = (x + predPartWidth, y - 1)
     *   D = (x - 1, y - 1), substituted for C when C is unavailable.
     * Reading B at the partition's own right edge instead picks a different
     * 4x4 block whenever the row above is partitioned more finely. The bit
     * count is unaffected (mvd is read either way), so a wrong mvp never
     * desynchronises the stream; it just silently shifts the reconstructed
     * block and feeds the error onward through every neighbour after it. */
    nb_t A = get_nb(d, cur, gx - 1, gy, list);
    nb_t B = get_nb(d, cur, gx, gy - 1, list);
    nb_t C = get_nb(d, cur, gx + pw, gy - 1, list);
    if (!C.avail) C = get_nb(d, cur, gx - 1, gy - 1, list);   /* D substitution */

    if (dir_kind == 1) {              /* 16x8: upper -> B, lower -> A */
        if (py == 0 && B.avail && B.ref == ref) { *outx = B.mvx; *outy = B.mvy; return; }
        if (py != 0 && A.avail && A.ref == ref) { *outx = A.mvx; *outy = A.mvy; return; }
    } else if (dir_kind == 2) {       /* 8x16: left -> A, right -> C */
        if (px == 0 && A.avail && A.ref == ref) { *outx = A.mvx; *outy = A.mvy; return; }
        if (px != 0 && C.avail && C.ref == ref) { *outx = C.mvx; *outy = C.mvy; return; }
    }

    if (A.avail && !B.avail && !C.avail) { *outx = A.mvx; *outy = A.mvy; return; }
    int ma = A.avail && A.ref == ref;
    int mb = B.avail && B.ref == ref;
    int mc = C.avail && C.ref == ref;
    if (ma + mb + mc == 1) {
        if (ma)      { *outx = A.mvx; *outy = A.mvy; }
        else if (mb) { *outx = B.mvx; *outy = B.mvy; }
        else         { *outx = C.mvx; *outy = C.mvy; }
        return;
    }
    *outx = median3(A.avail ? A.mvx : 0, B.avail ? B.mvx : 0, C.avail ? C.mvx : 0);
    *outy = median3(A.avail ? A.mvy : 0, B.avail ? B.mvy : 0, C.avail ? C.mvy : 0);
}

/* P_Skip motion vector (8.4.1.1).
 *
 * The zero case is an OR over four conditions, not an AND over two: the vector
 * is zero if EITHER neighbour is missing, or if EITHER of them is itself a
 * zero-motion reference-0 block. Requiring both leaves a skipped macroblock
 * predicting from the median when the spec says it must not move at all -- a
 * small displacement, so the picture stays recognisable, which is what makes
 * it survive a casual look.
 *
 * When it is not zero, the answer is the ORDINARY 16x16 prediction with
 * refIdx 0, including that derivation's own special cases. A bare median of
 * the three is not the same function. */
void h264_mv_pred_skip(h264dec *d, int cur, int mbx, int mby, int *outx, int *outy)
{
    nb_t A = get_nb(d, cur, mbx * 4 - 1, mby * 4, 0);
    nb_t B = get_nb(d, cur, mbx * 4, mby * 4 - 1, 0);
    if (!A.avail || !B.avail ||
        (A.ref == 0 && A.mvx == 0 && A.mvy == 0) ||
        (B.ref == 0 && B.mvx == 0 && B.mvy == 0)) {
        *outx = 0; *outy = 0;
        return;
    }
    h264_mv_pred(d, cur, mbx, mby, 0, 0, 4, 4, 0, 0, 0, outx, outy);
}

/* ========================================================= direct modes == */
/* MinPositive (8.4.1.2.2): prefer the smaller NON-NEGATIVE index; if only one
 * is non-negative that one wins; if neither, the result stays negative. */
static int min_positive(int a, int b)
{
    if (a >= 0 && b >= 0) return a < b ? a : b;
    return a > b ? a : b;
}

/* The lowest index in RefPicList0 that references the picture the colocated
 * block used (8.4.1.2.3). Matched by picture order count, because a DPB slot
 * is not a stable identity across pictures. */
static int map_col_to_list0(h264dec *d, int32_t poc)
{
    for (int i = 0; i < d->n_rl[0]; i++)
        if (d->rl[0][i] && d->rl[0][i]->poc == poc) return i;
    return 0;
}

void h264_direct_motion(h264dec *d, slice_t *sl, int addr)
{
    const sps_t *sps = d->cur_sps;
    pic_t *colpic = d->n_rl[1] > 0 ? d->rl[1][0] : 0;
    const colmb_t *col = (colpic && colpic->col) ? &colpic->col[addr] : 0;
    int step = sps->direct_8x8_inference ? 2 : 1;
    /* With direct_8x8_inference the whole 8x8 takes the motion of the
     * colocated block at its OUTER CORNER -- not its top-left, and not an
     * average. The corners in raster 4x4 order are 0, 3, 12, 15. */
    static const uint8_t corner[4] = { 0, 3, 12, 15 };

    memset(d->dir_mv, 0, sizeof d->dir_mv);
    for (int l = 0; l < 2; l++)
        for (int b = 0; b < 16; b++) d->dir_ref[l][b] = -1;
    if (d->n_rl[0] < 1 || d->n_rl[1] < 1) return;

    if (sl->direct_spatial_mv_pred) {
        /* --- spatial (8.4.1.2.2) --- */
        int mbx = addr % d->mbw, mby = addr / d->mbw;
        int ref[2], mv[2][2];
        uint16_t saved = d->mb_mv_done;
        d->mb_mv_done = 0;             /* the neighbours are the WHOLE MB's */
        for (int l = 0; l < 2; l++) {
            nb_t A = get_nb(d, addr, mbx * 4 - 1, mby * 4, l);
            nb_t B = get_nb(d, addr, mbx * 4, mby * 4 - 1, l);
            nb_t C = get_nb(d, addr, mbx * 4 + 4, mby * 4 - 1, l);
            if (!C.avail) C = get_nb(d, addr, mbx * 4 - 1, mby * 4 - 1, l);
            ref[l] = min_positive(min_positive(A.avail ? A.ref : -1,
                                               B.avail ? B.ref : -1),
                                  C.avail ? C.ref : -1);
        }
        int zero_pred = 0;
        if (ref[0] < 0 && ref[1] < 0) { ref[0] = ref[1] = 0; zero_pred = 1; }
        for (int l = 0; l < 2; l++) {
            mv[l][0] = mv[l][1] = 0;
            if (ref[l] >= 0)
                h264_mv_pred(d, addr, mbx, mby, 0, 0, 4, 4, l, ref[l], 0,
                             &mv[l][0], &mv[l][1]);
        }
        d->mb_mv_done = saved;

        for (int by = 0; by < 4; by += step)
            for (int bx = 0; bx < 4; bx += step) {
                int ci = step == 2 ? corner[(by >> 1) * 2 + (bx >> 1)] : by * 4 + bx;
                /* colZeroFlag (8.4.1.2.2): only when the colocated PICTURE is
                 * short-term, the colocated block's own reference INDEX was 0,
                 * and its vector is within one quarter-pel of zero in both
                 * components. All three, or the block keeps the predicted
                 * vector. */
                int colzero = 0;
                if (col && colpic->reference == 1 && !col->intra &&
                    (col->ref_zero & (1u << ci)) &&
                    col->mv[ci][0] >= -1 && col->mv[ci][0] <= 1 &&
                    col->mv[ci][1] >= -1 && col->mv[ci][1] <= 1)
                    colzero = 1;
                for (int k = 0; k < step * step; k++) {
                    int b = (by + (k / step)) * 4 + bx + (k % step);
                    for (int l = 0; l < 2; l++) {
                        d->dir_ref[l][b] = (int8_t)ref[l];
                        int zero = zero_pred || ref[l] < 0 ||
                                   (ref[l] == 0 && colzero);
                        d->dir_mv[l][b][0] = (int16_t)(zero ? 0 : mv[l][0]);
                        d->dir_mv[l][b][1] = (int16_t)(zero ? 0 : mv[l][1]);
                    }
                }
            }
        return;
    }

    /* --- temporal (8.4.1.2.3) --- */
    for (int by = 0; by < 4; by += step)
        for (int bx = 0; bx < 4; bx += step) {
            int ci = step == 2 ? corner[(by >> 1) * 2 + (bx >> 1)] : by * 4 + bx;
            int r0 = 0, mvcx = 0, mvcy = 0, lt = 0;
            if (col && !col->intra && col->ref_poc[ci] != COL_NOREF) {
                r0 = map_col_to_list0(d, col->ref_poc[ci]);
                mvcx = col->mv[ci][0];
                mvcy = col->mv[ci][1];
                lt = (col->ref_lt >> ci) & 1;
            }
            int mv0x, mv0y, mv1x, mv1y;
            pic_t *p0 = d->rl[0][r0];
            int td = (p0 && colpic) ? clip3(-128, 127, colpic->poc - p0->poc) : 0;
            if (lt || td == 0 || !p0 || p0->reference == 2) {
                mv0x = mvcx; mv0y = mvcy; mv1x = 0; mv1y = 0;
            } else {
                int tb = clip3(-128, 127, d->cur->poc - p0->poc);
                int tx = (16384 + iabs(td) / 2) / td;
                int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
                mv0x = (dsf * mvcx + 128) >> 8;
                mv0y = (dsf * mvcy + 128) >> 8;
                mv1x = mv0x - mvcx;
                mv1y = mv0y - mvcy;
            }
            for (int k = 0; k < step * step; k++) {
                int b = (by + (k / step)) * 4 + bx + (k % step);
                d->dir_ref[0][b] = (int8_t)r0;
                d->dir_ref[1][b] = 0;
                d->dir_mv[0][b][0] = (int16_t)clip3(-8192, 8191, mv0x);
                d->dir_mv[0][b][1] = (int16_t)clip3(-8192, 8191, mv0y);
                d->dir_mv[1][b][0] = (int16_t)clip3(-8192, 8191, mv1x);
                d->dir_mv[1][b][1] = (int16_t)clip3(-8192, 8191, mv1y);
            }
        }
}

/* ==================================================================== MC == */
/* Motion-compensate one block from a reference plane. Reads inside the
 * replicated H264_PAD border go straight to the plane; anything beyond is
 * rebuilt through a clamped edge-emulation scratch so no access ever leaves
 * the allocation. */
static void mc_plane(h264dec *d, uint8_t *dst, int dstride,
                     const uint8_t *plane, int stride, int pw, int ph,
                     int is_luma, int x, int y, int w, int h, int mvx, int mvy)
{
    /* Split each component into a floored integer offset and a fraction.
     * Negative motion vectors are routine, and both `>>` and `<<` on negative
     * ints are implementation-defined or undefined -- UBSan flags the shift on
     * essentially every P frame. */
    int bits = is_luma ? 2 : 3;
    int unit = 1 << bits;
    int qx = mvx / unit, fx = mvx - qx * unit;
    int qy = mvy / unit, fy = mvy - qy * unit;
    if (fx < 0) { fx += unit; qx--; }
    if (fy < 0) { fy += unit; qy--; }
    int m = is_luma ? 2 : 0;                  /* filter taps before the block */
    int t = is_luma ? 3 : 1;                  /* filter taps after the block */
    if (x + qx >= m - H264_PAD && y + qy >= m - H264_PAD &&
        x + qx + w + t <= pw + H264_PAD && y + qy + h + t <= ph + H264_PAD) {
        h264_mc_block(dst, dstride, plane, stride, x, y, w, h, mvx, mvy, is_luma);
        return;
    }
    if (is_luma) {
        int sw = w + 6, sh = h + 6;           /* [-2 .. w+3] both axes */
        uint8_t *s = d->emu_luma;
        for (int r = 0; r < sh; r++) {
            const uint8_t *row = plane + (long)clip3(0, ph - 1, y + qy - 2 + r) * stride;
            for (int c = 0; c < sw; c++)
                s[r * sw + c] = row[clip3(0, pw - 1, x + qx - 2 + c)];
        }
        h264_mc_block(dst, dstride, s, sw, 2, 2, w, h, fx, fy, 1);
    } else {
        int sw = w + 1, sh = h + 1;           /* bilinear: [0 .. w] x [0 .. h] */
        uint8_t *s = d->emu_chroma;
        for (int r = 0; r < sh; r++) {
            const uint8_t *row = plane + (long)clip3(0, ph - 1, y + qy + r) * stride;
            for (int c = 0; c < sw; c++)
                s[r * sw + c] = row[clip3(0, pw - 1, x + qx + c)];
        }
        h264_mc_block(dst, dstride, s, sw, 0, 0, w, h, fx, fy, 0);
    }
}

/* Single-list prediction of one partition into an arbitrary destination. */
static void mc_one(h264dec *d, int list, int ref_idx, int x, int y, int w, int h,
                   int mvx, int mvy, uint8_t *dy, int sy,
                   uint8_t *du, uint8_t *dv, int sc)
{
    const pic_t *ref = d->rl[list][ref_idx];
    mc_plane(d, dy, sy, ref->y, ref->stride_y,
             d->mbw * 16, d->mbh * 16, 1, x, y, w, h, mvx, mvy);
    mc_plane(d, du, sc, ref->u, ref->stride_c,
             d->mbw * 8, d->mbh * 8, 0, x / 2, y / 2, w / 2, h / 2, mvx, mvy);
    mc_plane(d, dv, sc, ref->v, ref->stride_c,
             d->mbw * 8, d->mbh * 8, 0, x / 2, y / 2, w / 2, h / 2, mvx, mvy);
}

void h264_inter_pred(h264dec *d, slice_t *sl, int addr,
                     int px, int py, int pw4, int ph4, int pred)
{
    const mbinfo_t *mi = &d->mb[addr];
    int mbx = addr % d->mbw, mby = addr / d->mbw;
    int q = (py >> 1) * 2 + (px >> 1);
    int x = mbx * 16 + px * 4, y = mby * 16 + py * 4;
    int w = pw4 * 4, h = ph4 * 4;
    uint8_t *dy = d->cur->y + (long)y * d->stride_y + x;
    uint8_t *du = d->cur->u + (long)(y / 2) * d->stride_c + x / 2;
    uint8_t *dv = d->cur->v + (long)(y / 2) * d->stride_c + x / 2;
    int r0 = mi->ref_idx[0][q], r1 = mi->ref_idx[1][q];

    if (pred == 2 && r0 >= 0 && r1 >= 0) {
        /* Both lists: interpolate each into scratch, then combine. The two
         * predictions are averaged (or weighted) at FULL precision of the
         * 8-bit interpolated samples -- the spec has no higher-precision
         * intermediate here, so the scratch buffers lose nothing. */
        for (int l = 0; l < 2; l++)
            mc_one(d, l, l ? r1 : r0, x, y, w, h,
                   mi->mv[l][py * 4 + px][0], mi->mv[l][py * 4 + px][1],
                   d->bip_y[l], 16, d->bip_u[l], d->bip_v[l], 8);
        if (d->use_weight == 0) {
            h264_mc_bi_avg(dy, d->stride_y, d->bip_y[0], 16, d->bip_y[1], 16, w, h);
            h264_mc_bi_avg(du, d->stride_c, d->bip_u[0], 8, d->bip_u[1], 8, w / 2, h / 2);
            h264_mc_bi_avg(dv, d->stride_c, d->bip_v[0], 8, d->bip_v[1], 8, w / 2, h / 2);
            return;
        }
        int lw, cw, w0, w1, cw0[2], cw1[2], o0 = 0, o1 = 0, co0[2] = {0,0}, co1[2] = {0,0};
        if (d->use_weight == 2) {                    /* implicit: POC derived */
            lw = cw = 5;
            w0 = d->impl_w[r0][r1][0]; w1 = d->impl_w[r0][r1][1];
            cw0[0] = cw0[1] = w0; cw1[0] = cw1[1] = w1;
        } else {                                     /* explicit */
            lw = sl->luma_log2_weight_denom;
            cw = sl->chroma_log2_weight_denom;
            w0 = sl->wp_luma_w[0][r0]; w1 = sl->wp_luma_w[1][r1];
            o0 = sl->wp_luma_o[0][r0]; o1 = sl->wp_luma_o[1][r1];
            for (int c = 0; c < 2; c++) {
                cw0[c] = sl->wp_chroma_w[0][r0][c]; cw1[c] = sl->wp_chroma_w[1][r1][c];
                co0[c] = sl->wp_chroma_o[0][r0][c]; co1[c] = sl->wp_chroma_o[1][r1][c];
            }
        }
        h264_mc_bi_weight(dy, d->stride_y, d->bip_y[0], 16, d->bip_y[1], 16,
                          w, h, lw, w0, w1, o0, o1);
        h264_mc_bi_weight(du, d->stride_c, d->bip_u[0], 8, d->bip_u[1], 8,
                          w / 2, h / 2, cw, cw0[0], cw1[0], co0[0], co1[0]);
        h264_mc_bi_weight(dv, d->stride_c, d->bip_v[0], 8, d->bip_v[1], 8,
                          w / 2, h / 2, cw, cw0[1], cw1[1], co0[1], co1[1]);
        return;
    }

    int l = (pred == 1 || (pred == 2 && r0 < 0)) ? 1 : 0;
    int ri = l ? r1 : r0;
    if (ri < 0 || !d->rl[l][ri]) return;
    mc_one(d, l, ri, x, y, w, h,
           mi->mv[l][py * 4 + px][0], mi->mv[l][py * 4 + px][1],
           dy, d->stride_y, du, dv, d->stride_c);
    /* Explicit weights apply to single-list prediction too; implicit weights
     * do NOT -- 8.4.2.3 only defines them for the bi-predicted case, so a
     * uni-predicted block in an implicitly weighted B slice is unweighted. */
    if (d->use_weight == 1) {
        h264_mc_weight(dy, d->stride_y, w, h, sl->luma_log2_weight_denom,
                       sl->wp_luma_w[l][ri], sl->wp_luma_o[l][ri]);
        h264_mc_weight(du, d->stride_c, w / 2, h / 2, sl->chroma_log2_weight_denom,
                       sl->wp_chroma_w[l][ri][0], sl->wp_chroma_o[l][ri][0]);
        h264_mc_weight(dv, d->stride_c, w / 2, h / 2, sl->chroma_log2_weight_denom,
                       sl->wp_chroma_w[l][ri][1], sl->wp_chroma_o[l][ri][1]);
    }
}

/* ------------------------------------------------------- new/finish frame */
static int new_picture(h264dec *d, const slice_t *sl, int nal_ref_idc, int nal_type)
{
    const sps_t *sps = d->cur_sps;
    if (d->mbw != sps->mb_width || d->mbh != sps->mb_height) {
        for (int i = 0; i < MAX_DPB; i++) h264_release_pic(d, i);
        d->mbw = sps->mb_width;
        d->mbh = sps->mb_height;
        d->stride_y = d->mbw * 16 + 2 * H264_PAD;
        d->stride_c = d->mbw * 8 + 2 * H264_PAD;
        int cw = sps->crop_flag ? (sps->crop[0] + sps->crop[1]) * 2 : 0;
        int ch = sps->crop_flag ? (sps->crop[2] + sps->crop[3]) * 2 : 0;
        d->width = d->mbw * 16 - cw;
        d->height = d->mbh * 16 - ch;
        if (d->width <= 0 || d->height <= 0) return H264_ERR_CORRUPT;
    }
    h264_set_reorder_depth(d);
    int rc = h264_alloc_picture(d);
    if (rc) return rc;
    d->cur->poc = h264_compute_poc(d, sl, nal_ref_idc, nal_type);
    d->cur->pts = d->next_pts;
    d->cur->frame_num = sl->frame_num;
    d->cur->needed_for_output = 1;
    d->cur->output_seq = d->output_seq++;
    d->n_slices = 0;
    d->cur_idr = (nal_type == 5);
    d->cur_ref = (nal_ref_idc != 0);
    return H264_OK;
}

static void finish_picture(h264dec *d)
{
    pic_t *cur = d->cur;
    if (!cur) return;
    if (d->n_slices > 0 && d->last_slice.disable_deblocking_filter_idc != 1)
        h264_deblock_frame(cur->y, cur->u, cur->v, d->stride_y, d->stride_c,
                           d->mbw, d->mbh, d->mb, &d->last_slice,
                           d->cur_sps, d->cur_pps,
                           d->slice_first_mb, d->n_slices);
    h264_border_pad(d, cur);
    /* Freeze the colocated motion BEFORE marking: an MMCO in this picture's
     * own header can unreference the pictures it points at. */
    h264_store_colocated(d);
    h264_mark_refs(d);
    free(d->mb); d->mb = 0;
    d->cur = 0;
    d->n_slices = 0;
}

/* ============================================================= slices ==== */
static int decode_slice(h264dec *d, bs_t *bs, slice_t *sl,
                        int nal_ref_idc, int nal_type)
{
    int total = d->mbw * d->mbh;
    if (sl->first_mb_in_slice == 0) {
        if (d->cur) return H264_ERR_CORRUPT;     /* boundary handled by caller */
        if (!d->cur_sps) return H264_ERR_CORRUPT;
        int rc = new_picture(d, sl, nal_ref_idc, nal_type);
        if (rc) return rc;
        total = d->mbw * d->mbh;
    } else {
        if (!d->cur) return H264_ERR_CORRUPT;
        if (sl->first_mb_in_slice >= total) return H264_ERR_CORRUPT;
        if (d->cur_sps->mb_width != d->mbw || d->cur_sps->mb_height != d->mbh)
            return H264_ERR_CORRUPT;
    }
    if (d->n_slices < 64) d->slice_first_mb[d->n_slices] = sl->first_mb_in_slice;
    d->n_slices++;
    d->last_slice = *sl;

    int rc = h264_build_lists(d, sl);
    if (rc) return rc;
    d->use_weight = 0;
    if (sl->slice_type == SLICE_P && d->cur_pps->weighted_pred) d->use_weight = 1;
    else if (sl->slice_type == SLICE_B) {
        if (d->cur_pps->weighted_bipred_idc == 1) d->use_weight = 1;
        else if (d->cur_pps->weighted_bipred_idc == 2) {
            d->use_weight = 2;
            h264_calc_implicit_weights(d);
        }
    }

    int qpy = ((26 + d->cur_pps->pic_init_qp + sl->slice_qp_delta) % 52 + 52) % 52;
    d->cabac = d->cur_pps->entropy_cabac;
    if (d->cabac) {
        /* 7.3.4: cabac_alignment_one_bit pads to a byte boundary, then the
         * arithmetic decoder owns the rest of the slice. */
        bs_align(bs);
        int off = bs->bitpos >> 3;
        if (off > bs->len) return H264_ERR_CORRUPT;
        h264_cabac_init(&d->cab, bs->data + off, bs->len - off,
                        qpy, sl->slice_type, sl->cabac_init_idc);
        d->last_qp_delta = 0;
    }

    int addr = sl->first_mb_in_slice;
    while (addr < total) {
        int skip = 0;
        if (sl->slice_type != SLICE_I) {
            if (!d->cabac) {
                uint32_t run = bs_ue(bs);
                if (bs_error(bs)) return H264_ERR_CORRUPT;
                if (run > (uint32_t)(total - addr)) return H264_ERR_CORRUPT;
                while (run--) {
                    rc = h264_decode_skip_mb(d, sl, addr, qpy);
                    if (rc) return rc;
                    addr++;
                }
                if (addr >= total) break;
                if (!bs_more_rbsp_data(bs)) break;
            } else {
                /* mb_skip_flag, ctxIdxOffset 11 for P and 24 for B. The
                 * neighbour test is "available and NOT skipped". */
                int ctx = 0, mbx = addr % d->mbw, mby = addr / d->mbw;
                if (mbx > 0) {
                    int n = addr - 1;
                    if (h264_slice_of(d, n) == h264_slice_of(d, addr) &&
                        !d->mb[n].skip) ctx++;
                }
                if (mby > 0) {
                    int n = addr - d->mbw;
                    if (h264_slice_of(d, n) == h264_slice_of(d, addr) &&
                        !d->mb[n].skip) ctx++;
                }
                if (sl->slice_type == SLICE_B) ctx += 13;
                skip = h264_cabac_decision(&d->cab, 11 + ctx);
            }
        }
        if (skip) {
            rc = h264_decode_skip_mb(d, sl, addr, qpy);
            if (rc) return rc;
        } else {
            if (!d->cabac && !bs_more_rbsp_data(bs)) break;
            rc = h264_decode_mb(d, bs, sl, addr, &qpy);
            if (rc) return rc;
        }
        addr++;
        if (d->cabac) {
            if (d->cab.error) return H264_ERR_CORRUPT;
            if (h264_cabac_terminate(&d->cab)) break;   /* end_of_slice_flag */
        } else if (!bs_more_rbsp_data(bs)) {
            break;
        }
    }
    return H264_OK;
}

/* ==================================================== NAL dispatch / API == */
static int find_start_code(const uint8_t *p, int n)
{
    for (int i = 0; i + 2 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return -1;
}

h264dec *h264_open(void)
{
    h264dec *d = (h264dec *)malloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->max_long_term_idx = -1;
    d->pending_free = -1;
    d->next_pts = H264_NOPTS;
    return d;
}

void h264_close(h264dec *d)
{
    if (!d) return;
    for (int i = 0; i < MAX_DPB; i++) h264_release_pic(d, i);
    free(d->mb);
    free(d);
}

/* Release the picture the caller was shown last time. Deferred to here
 * because the API promises the planes stay valid until the next call. */
static void reap(h264dec *d)
{
    if (d->pending_free >= 0) {
        h264_release_pic(d, d->pending_free);
        d->pending_free = -1;
    }
}

int h264_decode(h264dec *d, const uint8_t *data, int len,
                h264frame *out, int *got_frame)
{
    return h264_decode_pts(d, data, len, H264_NOPTS, out, got_frame);
}

int h264_decode_pts(h264dec *d, const uint8_t *data, int len, int64_t pts,
                    h264frame *out, int *got_frame)
{
    if (!d || !data || !got_frame || !out || len < 0) return H264_ERR_CORRUPT;
    *got_frame = 0;
    reap(d);
    /* Applies to the next picture that STARTS, not to whatever comes out of
     * this call: with reordering those are different pictures, and the whole
     * point of carrying the value is that the decoder knows which is which. */
    d->next_pts = pts;

    /* Hand back anything the reorder buffer is already holding past its
     * depth. This consumes no input, so a picture that unblocks several
     * outputs emits them over successive calls. */
    if (d->drain || h264_pending_output(d) > d->num_reorder) {
        if (h264_bump_one(d, out)) { *got_frame = 1; return 0; }
        d->drain = 0;
    }

    int pos = 0;
    while (pos < len) {
        int sc = find_start_code(data + pos, len - pos);
        if (sc < 0) return len;                  /* trailing garbage: eat it */
        int nal = pos + sc + 3;
        if (nal >= len) return len;
        int next = find_start_code(data + nal, len - nal);
        int nend = next < 0 ? len : nal + next;
        /* Annex B: zero bytes immediately before a start code prefix are
         * leading_zero_8bits of the NEXT NAL, not trailing content of this
         * one -- leave them out or more_rbsp_data() sees phantom bits. */
        while (nend > nal + 1 && data[nend - 1] == 0) nend--;

        uint8_t hdr = data[nal];
        if (hdr & 0x80) return H264_ERR_CORRUPT; /* forbidden_zero_bit */
        int nri = (hdr >> 5) & 3, ntype = hdr & 31;

        if (ntype == 7 || ntype == 8 || ntype == 1 || ntype == 5) {
            int rbsp_len = 0;
            uint8_t *rbsp = h264_nal_to_rbsp(data + nal, nend - nal, &rbsp_len);
            if (!rbsp) return H264_ERR_OOM;
            bs_t bs;
            bs_init(&bs, rbsp, rbsp_len);
            int rc = H264_OK;
            if (ntype == 7) {
                rc = h264_parse_sps(d, &bs);
            } else if (ntype == 8) {
                rc = h264_parse_pps(d, &bs);
            } else {
                slice_t sl;
                rc = h264_parse_slice_header(d, &bs, nri, ntype, &sl);
                if (rc == H264_OK) {
                    if (sl.first_mb_in_slice == 0 && d->cur) {
                        /* First slice of the NEXT picture: the current one is
                         * complete. Finish it and leave this NAL unconsumed. */
                        finish_picture(d);
                        free(rbsp);
                        /* An IDR restarts the POC numbering, so every picture
                         * still held back has to come out BEFORE it -- their
                         * counts are not comparable with the new ones. */
                        if (ntype == 5) d->drain = 1;
                        if (d->drain || h264_pending_output(d) > d->num_reorder) {
                            if (h264_bump_one(d, out)) {
                                *got_frame = 1;
                                return pos + sc;
                            }
                            d->drain = 0;
                        }
                        return pos + sc;
                    }
                    rc = decode_slice(d, &bs, &sl, nri, ntype);
                }
            }
            free(rbsp);
            if (rc) return rc;
        } else if (ntype >= 2 && ntype <= 4) {
            return H264_ERR_UNSUPPORTED;         /* data partitioning */
        }
        /* 6 (SEI), 9 (AUD), everything else: skipped */
        pos = nend;
    }
    return len;
}

int h264_flush(h264dec *d, h264frame *out)
{
    if (!d || !out) return H264_ERR_CORRUPT;
    reap(d);
    if (d->cur) finish_picture(d);
    if (h264_bump_one(d, out)) return 1;
    return 0;
}

int h264_stream_info(h264dec *d, int *w, int *h, double *fps)
{
    if (!d) return H264_ERR_CORRUPT;
    for (int i = 0; i < 32; i++) {
        if (!d->sps[i].present) continue;
        const sps_t *s = &d->sps[i];
        int cw = s->crop_flag ? (s->crop[0] + s->crop[1]) * 2 : 0;
        int ch = s->crop_flag ? (s->crop[2] + s->crop[3]) * 2 : 0;
        if (w) *w = s->mb_width * 16 - cw;
        if (h) *h = s->mb_height * 16 - ch;
        if (fps)
            *fps = s->vui_timing && s->num_units_in_tick
                 ? (double)s->time_scale / (2.0 * s->num_units_in_tick) : 0.0;
        return H264_OK;
    }
    return H264_ERR_CORRUPT;
}
