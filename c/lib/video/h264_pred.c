/* c/lib/video/h264_pred.c -- H.264 transforms, dequant and intra prediction.
 *
 * Implements the h264_pred.c part of the contract in h264_int.h:
 *
 *   h264_dequant_idct_add  4x4 inverse quantisation + integer IDCT + add
 *                          (spec 8.5.10 + 8.5.12.1), coef[] in ZIGZAG order,
 *                          coef[0] = DC.  DC and AC share the same scaling
 *                          rule (flat scaling, no scaling lists in baseline).
 *   h264_dc16_transform    4x4 inverse Hadamard for the 16 luma DC coeffs of
 *                          an Intra_16x16 MB (transform part of spec 8.5.11).
 *                          Pure, UNNORMALISED transform: no scaling, no shift.
 *   h264_dcdcm_transform   2x2 inverse Hadamard for chroma DC coeffs (spec
 *                          8.5.13 transform part).  Pure, unnormalised.
 *   h264_intra4x4          spec 8.3.1, all 9 modes
 *   h264_intra16x16        spec 8.3.3, all 4 modes
 *   h264_intra_chroma      spec 8.3.4, all 4 modes (chroma order: 0=DC,
 *                          1=horizontal, 2=vertical, 3=plane)
 *
 * CALLER WIRING CONTRACT for the DC Hadamards (no qp parameter here!):
 *  - Intra_16x16 luma DC (spec 8.5.11): the caller must scale the raw DC
 *    levels BEFORE calling h264_dc16_transform:
 *        qP >= 12:  f = c * LevelScale(qP%6, 0, 0) << (qP/6 - 2)
 *        qP <  12:  f = (c * LevelScale(qP%6, 0, 0) + (1 << (1 - qP/6)))
 *                       >> (2 - qP/6)
 *    The hadamard output g then REPLACES coef[0] of each 4x4 block and must
 *    NOT be scaled again -- i.e. it cannot be passed through
 *    h264_dequant_idct_add (which always scales coef[0]); the IDCT for I16
 *    blocks needs coef[0] taken verbatim.
 *  - Chroma DC (spec 8.5.13): the caller scales AFTER h264_dcdcm_transform:
 *        qPc >= 6:  d = (f * LevelScale(qPc%6, 0, 0) << (qPc/6)) >> 3
 *        qPc <  6:  d = (f * LevelScale(qPc%6, 0, 0)) >> 1
 *    Same caveat: the result is a final coef[0], not to be re-scaled.
 *
 * NEIGHBOUR LAYOUT for the intra functions (all samples already clipped to
 * 0..255 by reconstruction; we only read what the avail flags allow):
 *  - 4x4:    top[0..3] above samples; if avail_topright, top[4..7] are the
 *            top-right block's bottom row, otherwise we replicate top[3]
 *            (spec 8.3.1.1).  left[0..3].  topleft scalar.
 *  - 16x16:  top[0..15], left[0..15], topleft.
 *  - chroma: top[0..7], left[0..7], topleft.
 * The caller guarantees the mode is legal w.r.t. the avail flags (spec
 * Table 8-2); if not, we fall back to DC prediction with whatever is
 * available (deterministic, never out of bounds, never a crash).
 *
 * Pure C99, no allocation, no global mutable state.  '>>' on negative ints
 * is used with the spec's arithmetic-shift semantics (gcc/clang).
 */
#include "h264_int.h"

/* small helper: clip to sample range */
static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ------------------------------------------------------- shared tables -- */
/* zigzag scan index -> raster index for 4x4 blocks (spec 8.5.5, Fig 8-7). */
static const uint8_t zz4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* LevelScale v-table (spec Table 8-16), only the three columns the 4x4
 * position classes use:
 *   k=0: positions (i%2==0 && j%2==0)   -- includes DC
 *   k=1: positions (i%2==1 && j%2==1)
 *   k=2: the remaining (mixed parity) positions
 * Row index is qP % 6. */
static const int16_t vq[6][3] = {
    { 10, 16, 13 },
    { 11, 18, 14 },
    { 13, 20, 16 },
    { 14, 23, 18 },
    { 16, 25, 20 },
    { 18, 29, 23 }
};

/* position class for scaling: (even,even) -> 0, (odd,odd) -> 1, else 2 */
static int vclass(int i, int j)
{
    if ((i & 1) == 0 && (j & 1) == 0) return 0;
    if ((i & 1) && (j & 1)) return 1;
    return 2;
}

/* |level| clamp.  Conforming 8-bit streams never produce levels anywhere
 * near this (the reference decoder's own int32 arithmetic would overflow);
 * the clamp only keeps adversarial input from overflowing our int32 math:
 * after scaling |d| <= 16384*42*256 < 2^28, and two butterfly passes stay
 * well below 2^31. */
static int clamp_level(int c)
{
    if (c > 16383) return 16383;
    if (c < -16384) return -16384;
    return c;
}

/* -------------------------------------- h264_dequant_idct_add (8.5.10) -- */
void h264_dequant_idct_add(int coef[16], int qp, uint8_t *dst, int stride)
{
    int d[16], f[16], g[16];
    int i, j, m, qbits;

    /* qp is untrusted like everything else: spec range is 0..51. */
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    m = qp % 6;
    qbits = qp / 6;

    /* un-zigzag, dequantise into raster order: d = c * V(qP%6,i,j) << qP/6.
     * coef[] is in zigzag (scan) order, d[] in raster order.  Written as a
     * multiply by (1 << qbits) because left-shifting negative values is UB
     * in C99; the product is identical and stays within int32 (see
     * clamp_level). */
    for (i = 0; i < 16; i++)
        d[i] = 0;
    for (i = 0; i < 16; i++) {
        int r = zz4[i];                 /* raster position of scan index i */
        d[r] = clamp_level(coef[i]) * vq[m][vclass(r >> 2, r & 3)]
               * (1 << qbits);
    }

    /* 4x4 integer inverse transform (spec 8.5.12.1): the 1-D transform applied
     * to each COLUMN first, then to each ROW. The >>1 terms truncate, so the
     * 1-D stage is not linear and the order is part of the definition rather
     * than a free choice -- this matches ff_h264_idct_add, which is the
     * reference these tests are compared against. All intermediates int32. */
    for (j = 0; j < 4; j++) {
        int d0 = d[0 * 4 + j], d1 = d[1 * 4 + j];
        int d2 = d[2 * 4 + j], d3 = d[3 * 4 + j];
        int e0 = d0 + d2;
        int e1 = d0 - d2;
        int e2 = (d1 >> 1) - d3;
        int e3 = d1 + (d3 >> 1);
        f[0 * 4 + j] = e0 + e3;
        f[1 * 4 + j] = e1 + e2;
        f[2 * 4 + j] = e1 - e2;
        f[3 * 4 + j] = e0 - e3;
    }
    for (i = 0; i < 4; i++) {
        int f0 = f[i * 4 + 0], f1 = f[i * 4 + 1];
        int f2 = f[i * 4 + 2], f3 = f[i * 4 + 3];
        int e0 = f0 + f2;
        int e1 = f0 - f2;
        int e2 = (f1 >> 1) - f3;
        int e3 = f1 + (f3 >> 1);
        g[i * 4 + 0] = e0 + e3;
        g[i * 4 + 1] = e1 + e2;
        g[i * 4 + 2] = e1 - e2;
        g[i * 4 + 3] = e0 - e3;
    }

    /* final residual: r = (g + 32) >> 6, add prediction, clip (8.5.12.1) */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            dst[i * stride + j] = (uint8_t)clip255(
                dst[i * stride + j] + ((g[i * 4 + j] + 32) >> 6));
}

/* ------------------------------------------- DC inverse Hadamards ------- */
/* Clamp for hadamard inputs: caller-scaled DC values are far below this
 * (max ~ 16383*18<<8 < 2^27 for luma after the 8.5.11 scaling); the bound
 * keeps 16-term sums inside int32 for adversarial input. */
static int clamp_dc(int c)
{
    if (c > (1 << 26) - 1) return (1 << 26) - 1;
    if (c < -(1 << 26)) return -(1 << 26);
    return c;
}

/* 4x4 inverse Hadamard over the 16 luma DC coefficients of an Intra_16x16
 * macroblock (transform part of spec 8.5.11).  Pure transform: no scaling,
 * no normalisation shift (the /4 normalisation lives in the caller's DC
 * scaling, see file header).  dc[] holds raster-order values in and out. */
void h264_dc16_transform(int dc[16])
{
    int f[16], g[16];
    int i, j;

    for (i = 0; i < 16; i++) f[i] = clamp_dc(dc[i]);

    /* columns first, then rows; 1-D kernel:
     *   m0 = c0+c2  m1 = c0-c2  m2 = c1-c3  m3 = c1+c3
     *   g0 = m0+m3  g1 = m1+m2  g2 = m1-m2  g3 = m0-m3
     * which is multiplication by W = [[1,1,1,1],[1,1,-1,-1],
     * [1,-1,-1,1],[1,-1,1,-1]]. */
    for (j = 0; j < 4; j++) {
        int c0 = f[0 * 4 + j], c1 = f[1 * 4 + j];
        int c2 = f[2 * 4 + j], c3 = f[3 * 4 + j];
        int m0 = c0 + c2, m1 = c0 - c2, m2 = c1 - c3, m3 = c1 + c3;
        g[0 * 4 + j] = m0 + m3;
        g[1 * 4 + j] = m1 + m2;
        g[2 * 4 + j] = m1 - m2;
        g[3 * 4 + j] = m0 - m3;
    }
    for (i = 0; i < 4; i++) {
        int c0 = g[i * 4 + 0], c1 = g[i * 4 + 1];
        int c2 = g[i * 4 + 2], c3 = g[i * 4 + 3];
        int m0 = c0 + c2, m1 = c0 - c2, m2 = c1 - c3, m3 = c1 + c3;
        dc[i * 4 + 0] = m0 + m3;
        dc[i * 4 + 1] = m1 + m2;
        dc[i * 4 + 2] = m1 - m2;
        dc[i * 4 + 3] = m0 - m3;
    }
}

/* 2x2 inverse Hadamard for the chroma DC coefficients (transform part of
 * spec 8.5.13).  Pure transform, no shift; raster order in and out:
 *   f00 = c00+c01+c10+c11   f01 = c00-c01+c10-c11
 *   f10 = c00+c01-c10-c11   f11 = c00-c01-c10+c11 */
void h264_dcdcm_transform(int dc[4])
{
    int c00 = clamp_dc(dc[0]), c01 = clamp_dc(dc[1]);
    int c10 = clamp_dc(dc[2]), c11 = clamp_dc(dc[3]);
    int s0 = c00 + c01, d0 = c00 - c01;
    int s1 = c10 + c11, d1 = c10 - c11;
    dc[0] = s0 + s1;
    dc[1] = d0 + d1;
    dc[2] = s0 - s1;
    dc[3] = d0 - d1;
}

/* -------------------------------------------------- intra prediction ---- */
/* DC mean helpers per spec availability rules. */
static int dc_mean_4(const uint8_t *top, const uint8_t *left,
                     int avail_top, int avail_left)
{
    int s = 0, i;
    if (avail_top && avail_left) {
        for (i = 0; i < 4; i++) s += top[i] + left[i];
        return (s + 4) >> 3;
    }
    if (avail_top) {
        for (i = 0; i < 4; i++) s += top[i];
        return (s + 2) >> 2;
    }
    if (avail_left) {
        for (i = 0; i < 4; i++) s += left[i];
        return (s + 2) >> 2;
    }
    return 128;
}

/* 4x4 luma intra prediction, spec 8.3.1.  Writes exactly the 4x4 block. */
void h264_intra4x4(uint8_t *dst, int stride, int mode,
                   const uint8_t *top, const uint8_t *left, int topleft,
                   int avail_left, int avail_top, int avail_topright,
                   int avail_topleft)
{
    uint8_t t[8];         /* extended top row: t[4..7] = top-right or top[3] */
    int tl;               /* effective top-left sample */
    int x, y;

    /* Mode legality (spec Table 8-2): the caller guarantees it; on violation
     * fall back to DC with whatever is available -- deterministic, in range. */
    int need_top     = (mode == 0 || mode == 3 || mode == 4 ||
                        mode == 5 || mode == 6 || mode == 7);
    int need_left    = (mode == 1 || mode == 4 || mode == 5 ||
                        mode == 6 || mode == 8);
    int need_topleft = (mode == 4 || mode == 5 || mode == 6);
    if (mode < 0 || mode > 8 ||
        (need_top && !avail_top) || (need_left && !avail_left) ||
        (need_topleft && !avail_topleft)) {
        int dc = dc_mean_4(top, left, avail_top, avail_left);
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = (uint8_t)dc;
        return;
    }

    /* extended top row (spec 8.3.1.1: unavailable top-right replicates the
     * last available top sample) */
    if (avail_top) {
        for (x = 0; x < 4; x++) t[x] = top[x];
        if (avail_topright)
            for (x = 4; x < 8; x++) t[x] = top[x];
        else
            for (x = 4; x < 8; x++) t[x] = top[3];
    }
    /* effective top-left: only modes 4/5/6 use it and they required
     * avail_topleft above; the fallback keeps a deterministic value. */
    tl = avail_topleft ? topleft
         : (avail_top ? top[0] : (avail_left ? left[0] : 128));

    switch (mode) {
    case 0:                                     /* vertical (8.3.1.2.1) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = t[x];
        break;
    case 1:                                     /* horizontal (8.3.1.2.2) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = left[y];
        break;
    case 2: {                                   /* DC (8.3.1.2.3) */
        int dc = dc_mean_4(top, left, avail_top, avail_left);
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = (uint8_t)dc;
        break;
    }
    case 3:                                     /* down-left (8.3.1.2.4) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int v;
                if (x == 3 && y == 3)
                    v = (t[6] + 3 * t[7] + 2) >> 2;
                else
                    v = (t[x + y] + 2 * t[x + y + 1] + t[x + y + 2] + 2) >> 2;
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    case 4:                                     /* down-right (8.3.1.2.5) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int v;
                if (x > y) {
                    int p[3];                   /* t[x-y-2..x-y], t[-1] = tl */
                    p[0] = (x - y == 1) ? tl : t[x - y - 2];
                    p[1] = t[x - y - 1];
                    p[2] = t[x - y];
                    v = (p[0] + 2 * p[1] + p[2] + 2) >> 2;
                } else if (x < y) {
                    int p[3];                   /* l[y-x-2..y-x], l[-1] = tl */
                    p[0] = (y - x == 1) ? tl : left[y - x - 2];
                    p[1] = left[y - x - 1];
                    p[2] = left[y - x];
                    v = (p[0] + 2 * p[1] + p[2] + 2) >> 2;
                } else
                    v = (t[0] + 2 * tl + left[0] + 2) >> 2;
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    case 5: {                                   /* vertical-right (8.3.1.2.6) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int z = 2 * x - y, v;
                if (z == 0 || z == 2 || z == 4 || z == 6) {
                    int a = (x - (y >> 1) == 0) ? tl : t[x - (y >> 1) - 1];
                    v = (a + t[x - (y >> 1)] + 1) >> 1;
                } else if (z == 1 || z == 3 || z == 5) {
                    int k = x - (y >> 1);
                    int p0 = (k - 2 < 0) ? tl : t[k - 2];
                    int p1 = (k - 1 < 0) ? tl : t[k - 1];
                    v = (p0 + 2 * p1 + t[k] + 2) >> 2;
                } else if (z == -1)
                    v = (left[0] + 2 * tl + t[0] + 2) >> 2;
                else {                          /* z == -2 || z == -3 */
                    int p0 = (y - 3 < 0) ? tl : left[y - 3];
                    v = (left[y - 1] + 2 * left[y - 2] + p0 + 2) >> 2;
                }
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    }
    case 6: {                                   /* horizontal-down (8.3.1.2.7) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int z = 2 * y - x, v;
                if (z == 0 || z == 2 || z == 4 || z == 6) {
                    int a = (y - (x >> 1) == 0) ? tl : left[y - (x >> 1) - 1];
                    v = (a + left[y - (x >> 1)] + 1) >> 1;
                } else if (z == 1 || z == 3 || z == 5) {
                    int k = y - (x >> 1);
                    int p0 = (k - 2 < 0) ? tl : left[k - 2];
                    int p1 = (k - 1 < 0) ? tl : left[k - 1];
                    v = (p0 + 2 * p1 + left[k] + 2) >> 2;
                } else if (z == -1)
                    v = (t[0] + 2 * tl + left[0] + 2) >> 2;
                else {                          /* z == -2 || z == -3 */
                    int p0 = (x - 3 < 0) ? tl : t[x - 3];
                    v = (t[x - 1] + 2 * t[x - 2] + p0 + 2) >> 2;
                }
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    }
    case 7:                                     /* vertical-left (8.3.1.2.8) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int k = x + (y >> 1), v;
                /* no (3,3) special case here: the (t6+3t7+2)>>2 exception
                 * belongs to down-left (mode 3); VL's (3,3) is the generic
                 * odd-row tap F2(t4,t5,t6) (x264 predict_4x4_vl_c). */
                if ((y & 1) == 0)
                    v = (t[k] + t[k + 1] + 1) >> 1;
                else
                    v = (t[k] + 2 * t[k + 1] + t[k + 2] + 2) >> 2;
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    default: {                                  /* 8: horizontal-up (8.3.1.2.9) */
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int z = x + 2 * y, v;
                if (z == 0 || z == 2 || z == 4) {
                    int k = y + (x >> 1);
                    v = (left[k] + left[k + 1] + 1) >> 1;
                } else if (z == 1 || z == 3) {
                    int k = y + (x >> 1);
                    v = (left[k] + 2 * left[k + 1] + left[k + 2] + 2) >> 2;
                } else if (z == 5)
                    v = (left[2] + 3 * left[3] + 2) >> 2;
                else                            /* z > 5 */
                    v = left[3];
                dst[y * stride + x] = (uint8_t)v;
            }
        break;
    }
    }
}

/* plane prediction core (spec 8.3.3.4 / 8.3.4.4).  size is 16 or 8. */
static void plane_pred(uint8_t *dst, int stride, int size,
                       const uint8_t *top, const uint8_t *left, int topleft)
{
    int h = size >> 1;          /* 8 or 4 */
    int H = 0, V = 0, a, b, c, x, y;

    for (x = 0; x < h; x++) {
        int hi = top[h + x];
        int lo = (h - 2 - x >= 0) ? top[h - 2 - x] : topleft;
        H += (x + 1) * (hi - lo);
    }
    for (y = 0; y < h; y++) {
        int hi = left[h + y];
        int lo = (h - 2 - y >= 0) ? left[h - 2 - y] : topleft;
        V += (y + 1) * (hi - lo);
    }
    a = 16 * (left[size - 1] + top[size - 1]);
    if (size == 16) {
        b = (5 * H + 32) >> 6;
        c = (5 * V + 32) >> 6;
    } else {
        b = (17 * H + 16) >> 5;
        c = (17 * V + 16) >> 5;
    }
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            dst[y * stride + x] = (uint8_t)clip255(
                (a + b * (x - (h - 1)) + c * (y - (h - 1)) + 16) >> 5);
}

/* 16x16 luma intra prediction, spec 8.3.3.  Modes: 0=vertical, 1=horizontal,
 * 2=DC, 3=plane. */
void h264_intra16x16(uint8_t *dst, int stride, int mode,
                     const uint8_t *top, const uint8_t *left, int topleft,
                     int avail_left, int avail_top)
{
    int x, y;

    if (mode < 0 || mode > 3 ||
        ((mode == 0 || mode == 3) && !avail_top) ||
        ((mode == 1 || mode == 3) && !avail_left))
        mode = 2;                               /* illegal: fall back to DC */

    switch (mode) {
    case 0:                                     /* vertical (8.3.3.1) */
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                dst[y * stride + x] = top[x];
        break;
    case 1:                                     /* horizontal (8.3.3.2) */
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                dst[y * stride + x] = left[y];
        break;
    case 2: {                                   /* DC (8.3.3.3) */
        int dc, s = 0;
        if (avail_top && avail_left) {
            for (x = 0; x < 16; x++) s += top[x] + left[x];
            dc = (s + 16) >> 5;
        } else if (avail_top) {
            for (x = 0; x < 16; x++) s += top[x];
            dc = (s + 8) >> 4;
        } else if (avail_left) {
            for (x = 0; x < 16; x++) s += left[x];
            dc = (s + 8) >> 4;
        } else
            dc = 128;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                dst[y * stride + x] = (uint8_t)dc;
        break;
    }
    default:                                    /* plane (8.3.3.4) */
        plane_pred(dst, stride, 16, top, left, topleft);
        break;
    }
}

/* chroma 8x8 intra prediction, spec 8.3.4.  Mode order differs from luma
 * 16x16: 0=DC, 1=horizontal, 2=vertical, 3=plane. */
void h264_intra_chroma(uint8_t *dst, int stride, int mode,
                       const uint8_t *top, const uint8_t *left, int topleft,
                       int avail_left, int avail_top)
{
    int x, y;

    if (mode < 0 || mode > 3 ||
        (mode == 2 && !avail_top) || (mode == 1 && !avail_left) ||
        (mode == 3 && (!avail_top || !avail_left)))
        mode = 0;                               /* illegal: fall back to DC */

    switch (mode) {
    case 0:                                     /* DC (8.3.4.1) */
        /* The 8x8 block is four 4x4 quadrants, and the spec does NOT treat them
         * alike. Only the two quadrants on the diagonal average both edges; the
         * other two prefer the edge they touch and use it ALONE even when both
         * are available:
         *
         *   (0,0)  top[0..3] + left[0..3]      averaged, (s + 4) >> 3
         *   (1,0)  top[4..7] if top available, else left[0..3]
         *   (0,1)  left[4..7] if left available, else top[0..3]
         *   (1,1)  top[4..7] + left[4..7]      averaged
         *
         * Averaging both edges everywhere is close enough to look right and
         * wrong in nearly every macroblock that has both neighbours, which is
         * almost all of them. */
        for (y = 0; y < 8; y += 4)
            for (x = 0; x < 8; x += 4) {
                int dc, s = 0, i;
                int on_diag = (x == 0) == (y == 0);   /* quadrants (0,0) and (1,1) */
                /* Which edge this quadrant asks for FIRST. top[x..x+3] and
                 * left[y..y+3] are already the runs each quadrant touches. */
                int top_first = on_diag ? avail_top : (x != 0);

                if (on_diag && avail_top && avail_left) {
                    for (i = 0; i < 4; i++) s += top[x + i] + left[y + i];
                    dc = (s + 4) >> 3;
                } else if (top_first ? avail_top : !avail_left && avail_top) {
                    for (i = 0; i < 4; i++) s += top[x + i];
                    dc = (s + 2) >> 2;
                } else if (avail_left) {
                    for (i = 0; i < 4; i++) s += left[y + i];
                    dc = (s + 2) >> 2;
                } else
                    dc = 128;
                for (int yy = 0; yy < 4; yy++)
                    for (int xx = 0; xx < 4; xx++)
                        dst[(y + yy) * stride + x + xx] = (uint8_t)dc;
            }
        break;
    case 1:                                     /* horizontal (8.3.4.2) */
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                dst[y * stride + x] = left[y];
        break;
    case 2:                                     /* vertical (8.3.4.3) */
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                dst[y * stride + x] = top[x];
        break;
    default:                                    /* plane (8.3.4.4) */
        plane_pred(dst, stride, 8, top, left, topleft);
        break;
    }
}
