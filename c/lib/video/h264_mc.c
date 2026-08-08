/* c/lib/video/h264_mc.c -- quarter-pel motion compensation interpolation.
 *
 * Implements the two leaf functions declared in h264_int.h:
 *
 *   h264_mc_block()  inter prediction of one WxH block from a reference
 *                    plane. Luma: 1/4-pel positions per ITU-T H.264 (2023)
 *                    8.4.2.2.1 -- half-pel via the 6-tap (1,-5,20,20,-5,1)/32
 *                    filter, quarter-pel via upward-rounded averaging of the
 *                    two nearest integer/half-pel samples. Chroma: 1/8-pel
 *                    bilinear per 8.4.2.2.2.
 *   h264_mc_weight() explicit weighted prediction for P slices (8.4.3),
 *                    applied in place to a predicted block.
 *
 * Reference planes carry H264_PAD (32) replicated edge pixels on all four
 * sides (h264.c maintains them), so any sample an H.264 MV can reach is in
 * bounds and this file reads WITHOUT coordinate clamping -- that is the
 * contract. Everything else is still treated as untrusted: null pointers,
 * bad block sizes and out-of-range weighting parameters are sanitised
 * instead of crashing. Pure C99, no allocation, no global state.
 *
 * Luma sub-pel geometry (Figure 8-4): G,H,M,N below are integer samples,
 * lowercase are sub-pel; (fx,fy) = (mvx&3, mvy&3) with floor semantics:
 *
 *        fx:  0   1   2   3
 *   fy=0      G   a   b   c   H        b: horiz 6-tap (clipped)
 *   fy=1      d   e   f   g            h: vert  6-tap (clipped)
 *   fy=2      h   i   j   k   m        j: centre, 6-tap over UNCLIPPED horiz
 *   fy=3      n   p   q   r   s        intermediates, (j1+512)>>10, clipped
 *             M
 *
 *   a=(G+b+1)>>1  c=(H+b+1)>>1  d=(G+h+1)>>1  n=(M+h+1)>>1
 *   e=(b+h+1)>>1  g=(b+m+1)>>1  p=(h+s+1)>>1  r=(s+m+1)>>1   (diagonal pairs)
 *   f=(b+j+1)>>1  i=(h+j+1)>>1  k=(j+m+1)>>1  q=(j+s+1)>>1
 */

#include <stdint.h>
#include "h264_int.h"

/* Clip1Y: clip to the 8-bit sample range. */
static int clip1y(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* Split a motion vector component into floor(v / 2^bits) and the remaining
 * fraction in [0, 2^bits). C99 integer division truncates toward zero, which
 * is wrong for the negative MVs that are routine in inter coding, so do the
 * floor adjustment explicitly (also keeps us off implementation-defined
 * right-shift-of-negative behaviour). */
static void mv_split(int v, int bits, int *base, int *frac)
{
    int q = v / (1 << bits);
    int r = v - q * (1 << bits);
    if (r < 0) { r += 1 << bits; q -= 1; }
    *base = q;
    *frac = r;
}

/* 6-tap (1,-5,20,20,-5,1) filter, unclipped intermediate.
 * half_h: taps p[-2..3] horizontally, centred between p[0] and p[1].
 * half_v: taps p[-2s..3s] vertically, centred between p[0] and p[s].
 * Worst case magnitude is 255*52 = 13260 -- comfortable in an int. */
static int half_h(const uint8_t *p)
{
    return p[-2] - 5 * p[-1] + 20 * p[0] + 20 * p[1] - 5 * p[2] + p[3];
}

static int half_v(const uint8_t *p, int s)
{
    return p[-2 * s] - 5 * p[-s] + 20 * p[0] + 20 * p[s]
         - 5 * p[2 * s] + p[3 * s];
}

/* w,h come from the inter partition geometry: {4,8,16} per the contract.
 * 2 is also accepted for 4:2:0 chroma of a 4x4 luma partition (a harmless
 * superset); anything else is corrupt -- do nothing rather than write out
 * of bounds. */
static int valid_size(int v)
{
    return v == 2 || v == 4 || v == 8 || v == 16;
}

/* ---------------------------------------------------------------- luma -- */
/* Luma block prediction. src = ref + (y+qy)*stride + (x+qx), the integer-pel
 * anchor; (fx,fy) in [0,3]x[0,3] is the quarter-pel sub-position. */
static void mc_luma(uint8_t *dst, int dst_stride,
                    const uint8_t *src, int ref_stride,
                    int w, int h, int fx, int fy)
{
    int r, c;

    /* (0,0): integer position -- plain copy. */
    if (fx == 0 && fy == 0) {
        for (r = 0; r < h; r++) {
            const uint8_t *s = src + r * ref_stride;
            uint8_t *d = dst + r * dst_stride;
            for (c = 0; c < w; c++) d[c] = s[c];
        }
        return;
    }

    /* fy == 0, fx in {1,2,3}: horizontal 6-tap row (b), plus the quarter
     * averages a=(G+b+1)>>1 and c=(b+H+1)>>1. */
    if (fy == 0) {
        for (r = 0; r < h; r++) {
            const uint8_t *s = src + r * ref_stride;
            uint8_t *d = dst + r * dst_stride;
            for (c = 0; c < w; c++) {
                int b = clip1y((half_h(s + c) + 16) >> 5);
                if (fx == 2)      d[c] = (uint8_t)b;
                else if (fx == 1) d[c] = (uint8_t)((s[c] + b + 1) >> 1);
                else              d[c] = (uint8_t)((b + s[c + 1] + 1) >> 1);
            }
        }
        return;
    }

    /* fx == 0, fy in {1,2,3}: vertical 6-tap row (h), plus d=(G+h+1)>>1
     * and n=(h+M+1)>>1. */
    if (fx == 0) {
        for (r = 0; r < h; r++) {
            const uint8_t *s = src + r * ref_stride;
            uint8_t *d = dst + r * dst_stride;
            for (c = 0; c < w; c++) {
                int hv = clip1y((half_v(s + c, ref_stride) + 16) >> 5);
                if (fy == 2)      d[c] = (uint8_t)hv;
                else if (fy == 1) d[c] = (uint8_t)((s[c] + hv + 1) >> 1);
                else d[c] = (uint8_t)((hv + s[c + ref_stride] + 1) >> 1);
            }
        }
        return;
    }

    /* General case fx,fy in {1,2,3}: build the half-pel grids the 9
     * quarter/centre positions pick from.
     *   hb[r][c]  horizontal half at row r, between cols c and c+1.
     *             Rows 0..h: quarter row 3 also needs the half of the NEXT
     *             integer row (spec label s).
     *   vh[r][c]  vertical half at col c, between rows r and r+1.
     *             Cols 0..w: quarter col 3 also needs the half of the next
     *             integer column (spec label m).
     *   jh[r][c]  centre half: 6-tap over the UNCLIPPED horizontal
     *             intermediates of rows r-2..r+3 (spec: j1 = cc - 5*dd +
     *             20*h1 + 20*m1 - 5*ee + ff may equally be built from the
     *             horizontal intermediates; both directions are identical),
     *             then Clip1Y((j1 + 512) >> 10). */
    {
        int hb[17][16], vh[16][17], jh[16][16];

        for (r = 0; r <= h; r++)
            for (c = 0; c < w; c++)
                hb[r][c] = clip1y((half_h(src + r * ref_stride + c) + 16) >> 5);

        for (r = 0; r < h; r++)
            for (c = 0; c <= w; c++)
                vh[r][c] = clip1y((half_v(src + r * ref_stride + c,
                                          ref_stride) + 16) >> 5);

        for (r = 0; r < h; r++) {
            for (c = 0; c < w; c++) {
                const uint8_t *p = src + r * ref_stride + c;
                int j1 = half_h(p - 2 * ref_stride)
                       - 5 * half_h(p - ref_stride)
                       + 20 * half_h(p)
                       + 20 * half_h(p + ref_stride)
                       - 5 * half_h(p + 2 * ref_stride)
                       + half_h(p + 3 * ref_stride);
                jh[r][c] = clip1y((j1 + 512) >> 10);
            }
        }

        for (r = 0; r < h; r++) {
            uint8_t *d = dst + r * dst_stride;
            int sel = (fy << 2) | fx;
            for (c = 0; c < w; c++) {
                int v;
                switch (sel) {
                /* e,g,p,r: upward-rounded average of the two diagonal
                 * half-pel samples. */
                case (1 << 2) | 1: v = (hb[r][c] + vh[r][c] + 1) >> 1; break;
                case (1 << 2) | 3: v = (hb[r][c] + vh[r][c + 1] + 1) >> 1; break;
                case (3 << 2) | 1: v = (hb[r + 1][c] + vh[r][c] + 1) >> 1; break;
                case (3 << 2) | 3: v = (hb[r + 1][c] + vh[r][c + 1] + 1) >> 1; break;
                /* f,i,k,q: average of the two nearest half-pel samples
                 * along the quarter direction (centre j involved). */
                case (1 << 2) | 2: v = (hb[r][c] + jh[r][c] + 1) >> 1; break;
                case (2 << 2) | 1: v = (vh[r][c] + jh[r][c] + 1) >> 1; break;
                case (2 << 2) | 3: v = (jh[r][c] + vh[r][c + 1] + 1) >> 1; break;
                case (3 << 2) | 2: v = (jh[r][c] + hb[r + 1][c] + 1) >> 1; break;
                /* j itself. */
                default:           v = jh[r][c]; break; /* (2,2) */
                }
                d[c] = (uint8_t)v;
            }
        }
    }
}

/* --------------------------------------------------------------- chroma -- */
/* Chroma block prediction (spec 8.4.2.2.2): 1/8-pel bilinear. (xf,yf) in
 * [0,7]x[0,7]; A,B,C,D are the four surrounding integer samples. The
 * weights sum to 64, so with the +32 rounding the result is always inside
 * [0,255] -- the spec applies no clip here. */
static void mc_chroma(uint8_t *dst, int dst_stride,
                      const uint8_t *src, int ref_stride,
                      int w, int h, int xf, int yf)
{
    int r, c;
    int wa = (8 - xf) * (8 - yf), wb = xf * (8 - yf);
    int wc = (8 - xf) * yf,       wd = xf * yf;

    for (r = 0; r < h; r++) {
        const uint8_t *s = src + r * ref_stride;
        uint8_t *d = dst + r * dst_stride;
        for (c = 0; c < w; c++) {
            d[c] = (uint8_t)((wa * s[c] + wb * s[c + 1]
                            + wc * s[c + ref_stride]
                            + wd * s[c + ref_stride + 1] + 32) >> 6);
        }
    }
}

/* --------------------------------------------------------------- public -- */
void h264_mc_block(uint8_t *dst, int dst_stride,
                   const uint8_t *ref, int ref_stride,
                   int x, int y, int w, int h, int mvx, int mvy, int is_luma)
{
    int qx, qy, fx, fy;
    const uint8_t *src;

    if (!dst || !ref) return;
    if (!valid_size(w) || !valid_size(h)) return;
    if (dst_stride < w || ref_stride < w + 1) return;  /* nonsense geometry */

    if (is_luma) {
        /* Quarter-pel units: floor(mv/4) integer offset, mv%4 fraction. */
        mv_split(mvx, 2, &qx, &fx);
        mv_split(mvy, 2, &qy, &fy);
        src = ref + (y + qy) * ref_stride + (x + qx);
        mc_luma(dst, dst_stride, src, ref_stride, w, h, fx, fy);
    } else {
        /* Chroma: the same MV in 1/8-chroma-pel units (4:2:0). x and y are
         * already chroma-plane coordinates supplied by the caller. */
        mv_split(mvx, 3, &qx, &fx);
        mv_split(mvy, 3, &qy, &fy);
        src = ref + (y + qy) * ref_stride + (x + qx);
        mc_chroma(dst, dst_stride, src, ref_stride, w, h, fx, fy);
    }
}

/* ------------------------------------------------------ bi-prediction --- */
/* 8.4.2.3.1, the default: the rounded average of the two single-list
 * predictions. Both inputs are already interpolated blocks, so this is the
 * only place the two lists meet. */
void h264_mc_bi_avg(uint8_t *dst, int dst_stride,
                    const uint8_t *a, int as, const uint8_t *b, int bs,
                    int w, int h)
{
    int r, c;
    if (!dst || !a || !b) return;
    if (!valid_size(w) || !valid_size(h)) return;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            dst[r * dst_stride + c] =
                (uint8_t)((a[r * as + c] + b[r * bs + c] + 1) >> 1);
}

/* 8.4.2.3.2 for the bi-predicted case. Explicit and implicit weighting differ
 * only in where w0/w1/o0/o1 came from -- implicit derives them from the
 * picture order counts and always uses logWD 5 with zero offsets -- so both
 * land here.
 *
 * The shift is logWD + 1, not logWD: the two weights each carry a full
 * denominator and the sum carries two. Using the single-list shift here is a
 * factor-of-two error that a decoder still renders as a picture, just a
 * blown-out one, and only on bi-predicted blocks. */
void h264_mc_bi_weight(uint8_t *dst, int dst_stride,
                       const uint8_t *a, int as, const uint8_t *b, int bs,
                       int w, int h, int log2w,
                       int w0, int w1, int o0, int o1)
{
    int r, c;
    if (!dst || !a || !b) return;
    if (!valid_size(w) || !valid_size(h)) return;
    if (log2w < 0) log2w = 0; else if (log2w > 7) log2w = 7;
    if (w0 < -128) w0 = -128; else if (w0 > 128) w0 = 128;
    if (w1 < -128) w1 = -128; else if (w1 > 128) w1 = 128;
    if (o0 < -255) o0 = -255; else if (o0 > 255) o0 = 255;
    if (o1 < -255) o1 = -255; else if (o1 > 255) o1 = 255;

    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            int v = ((a[r * as + c] * w0 + b[r * bs + c] * w1
                      + (1 << log2w)) >> (log2w + 1)) + ((o0 + o1 + 1) >> 1);
            dst[r * dst_stride + c] = (uint8_t)clip1y(v);
        }
}

void h264_mc_weight(uint8_t *dst, int stride, int w, int h,
                    int log2w, int weight, int offset)
{
    int r, c;

    if (!dst) return;
    if (!valid_size(w) || !valid_size(h)) return;
    if (stride < w) return;

    /* Sanitise into the spec ranges (8.4.3): log2WD in 0..7, offset in
     * -255..255. Valid streams never leave these, so clamping only fires on
     * corrupt input -- where it keeps the arithmetic small and in bounds
     * instead of overflowing.
     *
     * The weight bound is 128, not 127. -128..127 is the range of the CODED
     * syntax element, but a weight can also be INFERRED: when
     * luma_weight_l0_flag is 0 the weight is 2^log2WD, which is 128 for
     * log2WD == 7 -- a value the bitstream cannot express and the clamp must
     * not touch. Clamping it to 127 turns "no weighting" into a multiply by
     * 127/128, i.e. every predicted luma sample one too low across whole
     * macroblocks, and only for luma, since the chroma denominator has to
     * reach 7 as well before its default leaves the coded range. */
    if (log2w < 0) log2w = 0; else if (log2w > 7) log2w = 7;
    if (weight < -128) weight = -128; else if (weight > 128) weight = 128;
    if (offset < -255) offset = -255; else if (offset > 255) offset = 255;

    for (r = 0; r < h; r++) {
        uint8_t *d = dst + r * stride;
        for (c = 0; c < w; c++) {
            int v = d[c];
            /* Spec 8.4.3: with log2WD == 0 there is no shift and no
             * rounding term; otherwise round-then-shift, add the offset,
             * and clip once at the very end. */
            if (log2w == 0)
                v = v * weight + offset;
            else
                v = ((v * weight + (1 << (log2w - 1))) >> log2w) + offset;
            d[c] = (uint8_t)clip1y(v);
        }
    }
}
