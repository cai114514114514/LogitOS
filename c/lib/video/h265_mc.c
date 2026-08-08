/* c/lib/video/h265_mc.c -- inter prediction: fractional-sample interpolation
 * (8.5.3.3.3) and weighted sample prediction (8.5.3.3.4).
 *
 * HEVC widened H.264's 6-tap half-pel + bilinear quarter-pel into a real
 * separable filter bank: eight taps at three luma phases, four taps at seven
 * chroma phases, and -- the part that actually matters for bit-exactness --
 * an intermediate array kept at 14 bits rather than rounded back to 8 between
 * the two passes. Rounding the horizontal pass to samples before the vertical
 * one gives a picture that looks right and is wrong everywhere, which is
 * precisely the class of bug a tolerance-based test would pass.
 *
 * `ref` points at the visible (0,0) sample of a plane carrying H265_PAD
 * replicated border samples. Motion vectors that reach past that border are
 * handled by copying a clamped rectangle into the caller's scratch first:
 * the spec clamps every fetch to the picture, so the emulated block and the
 * padded read produce identical samples wherever both are legal.
 */
#include <string.h>
#include "h265.h"
#include "h265_int.h"

/* Table 8-11: luma interpolation filter, taps at offsets -3..+4. */
static const int8_t luma_filt[4][8] = {
    {  0,  0,  0, 64,  0,   0,  0,  0 },
    { -1,  4,-10, 58, 17,  -5,  1,  0 },
    { -1,  4,-11, 40, 40, -11,  4, -1 },
    {  0,  1, -5, 17, 58, -10,  4, -1 }
};
/* Table 8-13: chroma interpolation filter, taps at offsets -1..+2. */
static const int8_t chroma_filt[8][4] = {
    {  0, 64,  0,  0 },
    { -2, 58, 10, -2 },
    { -4, 54, 16, -2 },
    { -6, 46, 28, -4 },
    { -4, 36, 36, -4 },
    { -4, 28, 46, -6 },
    { -2, 16, 54, -4 },
    { -2, 10, 58, -2 }
};

/* Copy [x0, x0+w) x [y0, y0+h) out of a plane, clamping every coordinate to
 * the picture, into `dst`. This is the spec's Clip3 on xAi,j / yAi,j made
 * explicit for the rare vector that leaves the replicated border. */
static void emulate(uint16_t *dst, int dst_stride, const uint16_t *ref,
                    int ref_stride, int pw, int ph, int x0, int y0, int w, int h)
{
    for (int y = 0; y < h; y++) {
        int sy = h265_clip3(0, ph - 1, y0 + y);
        const uint16_t *row = ref + (long)sy * ref_stride;
        uint16_t *out = dst + (long)y * dst_stride;
        for (int x = 0; x < w; x++)
            out[x] = row[h265_clip3(0, pw - 1, x0 + x)];
    }
}

/* Does [x0,x0+w) x [y0,y0+h) sit inside the replicated border? */
static int inside_pad(int pw, int ph, int x0, int y0, int w, int h)
{
    return x0 >= -H265_PAD && y0 >= -H265_PAD &&
           x0 + w <= pw + H265_PAD && y0 + h <= ph + H265_PAD;
}

void h265_mc_luma(int16_t *dst, int dst_stride,
                  const uint16_t *ref, int ref_stride, int pw, int ph,
                  int x, int y, int w, int h, int mvx, int mvy, uint16_t *emu,
                  int bd)
{
    int xi = x + (mvx >> 2), yi = y + (mvy >> 2);
    int xf = mvx & 3, yf = mvy & 3;
    /* 8.5.3.3.3. The intermediate is 14 bits WHATEVER the sample depth: a
     * full-pel sample is promoted by shift3 = 14 - BitDepth, and every
     * filtered sum is brought down by shift1 = BitDepth - 8. At 8 bits shift1
     * is 0, which is why the 8-bit code could omit it entirely and stay
     * correct -- and why omitting it is the first thing that breaks at 10. */
    int shift1 = bd - 8, shift3 = 14 - bd;

    const uint16_t *src = ref + (long)yi * ref_stride + xi;
    int sstride = ref_stride;
    if (!inside_pad(pw, ph, xi - 3, yi - 3, w + 8, h + 8)) {
        emulate(emu, 64 + 8, ref, ref_stride, pw, ph, xi - 3, yi - 3, w + 8, h + 8);
        src = emu + 3 * (64 + 8) + 3;
        sstride = 64 + 8;
    }

    if (!xf && !yf) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                dst[j * dst_stride + i] =
                    (int16_t)(src[(long)j * sstride + i] << shift3);
        return;
    }
    if (!yf) {
        const int8_t *f = luma_filt[xf];
        for (int j = 0; j < h; j++) {
            const uint16_t *r = src + (long)j * sstride;
            for (int i = 0; i < w; i++) {
                int s = 0;
                for (int k = 0; k < 8; k++) s += f[k] * r[i + k - 3];
                dst[j * dst_stride + i] = (int16_t)(s >> shift1);
            }
        }
        return;
    }
    if (!xf) {
        const int8_t *f = luma_filt[yf];
        for (int j = 0; j < h; j++) {
            const uint16_t *r = src + (long)j * sstride;
            for (int i = 0; i < w; i++) {
                int s = 0;
                for (int k = 0; k < 8; k++) s += f[k] * r[i + (long)(k - 3) * sstride];
                dst[j * dst_stride + i] = (int16_t)(s >> shift1);
            }
        }
        return;
    }
    /* separable: horizontal into a 14-bit intermediate, then vertical */
    int16_t tmp[(64 + 8) * 64];
    const int8_t *fx = luma_filt[xf], *fy = luma_filt[yf];
    for (int j = -3; j < h + 4; j++) {
        const uint16_t *r = src + (long)j * sstride;
        int16_t *o = tmp + (j + 3) * 64;
        for (int i = 0; i < w; i++) {
            int s = 0;
            for (int k = 0; k < 8; k++) s += fx[k] * r[i + k - 3];
            s >>= shift1;
#ifdef H265_CONTROL_NO_14BIT
            /* The negative control (make test-h265-units-control). Rounding
             * the horizontal pass back to 8-bit samples before the vertical
             * one is the textbook HEVC interpolation bug: the picture still
             * looks right, every block is within a couple of levels, and the
             * decoder is wrong on every half-pel diagonal in the stream. The
             * unit test must FAIL when this is defined; if it ever passes,
             * the test is measuring something other than what it claims. */
            s = h265_clip_pix((s + (1 << (shift3 - 1))) >> shift3, bd) << shift3;
#endif
            o[i] = (int16_t)s;
        }
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int s = 0;
            for (int k = 0; k < 8; k++) s += fy[k] * tmp[(j + k) * 64 + i];
            dst[j * dst_stride + i] = (int16_t)(s >> 6);
        }
}

void h265_mc_chroma(int16_t *dst, int dst_stride,
                    const uint16_t *ref, int ref_stride, int pw, int ph,
                    int x, int y, int w, int h, int mvx, int mvy, uint16_t *emu,
                    int bd)
{
    int xi = x + (mvx >> 3), yi = y + (mvy >> 3);
    int xf = mvx & 7, yf = mvy & 7;
    int shift1 = bd - 8, shift3 = 14 - bd;

    const uint16_t *src = ref + (long)yi * ref_stride + xi;
    int sstride = ref_stride;
    if (!inside_pad(pw, ph, xi - 1, yi - 1, w + 4, h + 4)) {
        emulate(emu, 32 + 4, ref, ref_stride, pw, ph, xi - 1, yi - 1, w + 4, h + 4);
        src = emu + 1 * (32 + 4) + 1;
        sstride = 32 + 4;
    }

    if (!xf && !yf) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                dst[j * dst_stride + i] =
                    (int16_t)(src[(long)j * sstride + i] << shift3);
        return;
    }
    if (!yf) {
        const int8_t *f = chroma_filt[xf];
        for (int j = 0; j < h; j++) {
            const uint16_t *r = src + (long)j * sstride;
            for (int i = 0; i < w; i++) {
                int s = 0;
                for (int k = 0; k < 4; k++) s += f[k] * r[i + k - 1];
                dst[j * dst_stride + i] = (int16_t)(s >> shift1);
            }
        }
        return;
    }
    if (!xf) {
        const int8_t *f = chroma_filt[yf];
        for (int j = 0; j < h; j++) {
            const uint16_t *r = src + (long)j * sstride;
            for (int i = 0; i < w; i++) {
                int s = 0;
                for (int k = 0; k < 4; k++) s += f[k] * r[i + (long)(k - 1) * sstride];
                dst[j * dst_stride + i] = (int16_t)(s >> shift1);
            }
        }
        return;
    }
    int16_t tmp[(32 + 4) * 32];
    const int8_t *fx = chroma_filt[xf], *fy = chroma_filt[yf];
    for (int j = -1; j < h + 2; j++) {
        const uint16_t *r = src + (long)j * sstride;
        int16_t *o = tmp + (j + 1) * 32;
        for (int i = 0; i < w; i++) {
            int s = 0;
            for (int k = 0; k < 4; k++) s += fx[k] * r[i + k - 1];
            s >>= shift1;
            o[i] = (int16_t)s;
        }
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int s = 0;
            for (int k = 0; k < 4; k++) s += fy[k] * tmp[(j + k) * 32 + i];
            dst[j * dst_stride + i] = (int16_t)(s >> 6);
        }
}

/* ================================================= weighted prediction === */
/* 8.5.3.3.4.2/.3. Every shift here carries the bit depth:
 *   shift1 = 14 - BitDepth   (uni, and the weighted log2Wd base)
 *   shift2 = 15 - BitDepth   (bi)
 * and the CODED offsets are in units of the sample depth via
 *   WpOffsetBdShift = BitDepth - 8,
 * so o -> o << WpOffsetBdShift. Forgetting that last one leaves weighted
 * prediction shifted by a factor of four at 10 bits -- which looks like a
 * brightness bug on fades and nothing at all anywhere else. */
void h265_pred_uni(uint16_t *dst, int dst_stride, const int16_t *src, int src_stride,
                   int w, int h, int bd)
{
    int shift1 = 14 - bd;
    int off1 = 1 << (shift1 - 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * dst_stride + x] = (uint16_t)h265_clip_pix(
                (src[y * src_stride + x] + off1) >> shift1, bd);
}

void h265_pred_bi(uint16_t *dst, int dst_stride, const int16_t *s0, int s0_stride,
                  const int16_t *s1, int s1_stride, int w, int h, int bd)
{
    int shift2 = 15 - bd;
    int off2 = 1 << (shift2 - 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * dst_stride + x] = (uint16_t)h265_clip_pix(
                (s0[y * s0_stride + x] + s1[y * s1_stride + x] + off2) >> shift2, bd);
}

void h265_pred_uni_w(uint16_t *dst, int dst_stride, const int16_t *src, int src_stride,
                     int w, int h, int denom, int weight, int offset, int bd)
{
    int log2wd = denom + (14 - bd);
    int o = offset * (1 << (bd - 8));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = src[y * src_stride + x];
            int r;
            if (log2wd >= 1)
                r = ((v * weight + (1 << (log2wd - 1))) >> log2wd) + o;
            else
                r = v * weight + o;
            dst[y * dst_stride + x] = (uint16_t)h265_clip_pix(r, bd);
        }
}

void h265_pred_bi_w(uint16_t *dst, int dst_stride, const int16_t *s0, int s0_stride,
                    const int16_t *s1, int s1_stride, int w, int h,
                    int denom, int w0, int o0, int w1, int o1, int bd)
{
    int log2wd = denom + (14 - bd);
    int wo0 = o0 * (1 << (bd - 8)), wo1 = o1 * (1 << (bd - 8));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = s0[y * s0_stride + x] * w0 + s1[y * s1_stride + x] * w1 +
                    ((wo0 + wo1 + 1) << log2wd);
            dst[y * dst_stride + x] = (uint16_t)h265_clip_pix(v >> (log2wd + 1), bd);
        }
}
