/* tests/unit/h265_mc_test.c -- inter prediction as a module (spec 8.5.3.3).
 *
 * Three kinds of check, in increasing strength:
 *
 *  1. IMPULSE RESPONSE. Feeding a single non-zero sample through the filter
 *     reads the taps straight back out, so the eight luma taps at each of the
 *     four phases and the four chroma taps at each of the eight phases are
 *     recovered from the module rather than compared against a copy of the
 *     same array. A transposed or reversed table fails here.
 *  2. A REFERENCE IMPLEMENTATION written from the spec's own formulation --
 *     clamp every fetch with Clip3, sum the taps, apply shift1/shift2 -- run
 *     against the module's padded-plane-plus-emulation arrangement over
 *     random planes, every phase, every block size, and positions chosen to
 *     force each of the module's four paths and its edge emulation.
 *  3. INVARIANTS that hold whatever the taps are: every phase has DC gain 64,
 *     so a constant plane must come out as that constant << 6 at all sixteen
 *     luma phase pairs and all sixty-four chroma ones. This is the check that
 *     catches an intermediate rounded to 8 bits between the two passes, which
 *     is the classic HEVC MC bug and looks almost right.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* Table 8-11 and Table 8-13, typed from the spec. */
static const int ref_luma[4][8] = {
    {  0,  0,  0, 64,  0,   0,  0,  0 },
    { -1,  4,-10, 58, 17,  -5,  1,  0 },
    { -1,  4,-11, 40, 40, -11,  4, -1 },
    {  0,  1, -5, 17, 58, -10,  4, -1 }
};
static const int ref_chroma[8][4] = {
    {  0, 64,  0,  0 }, { -2, 58, 10, -2 }, { -4, 54, 16, -2 }, { -6, 46, 28, -4 },
    { -4, 36, 36, -4 }, { -4, 28, 46, -6 }, { -2, 16, 54, -4 }, { -2, 10, 58, -2 }
};

/* ---- a padded plane, exactly as h265.c builds one ----------------------- */
typedef struct { uint8_t *base, *vis; int stride, w, h; } plane_t;

static void plane_alloc(plane_t *p, int w, int h)
{
    p->w = w; p->h = h;
    p->stride = w + 2 * H265_PAD;
    p->base = malloc((size_t)p->stride * (h + 2 * H265_PAD));
    memset(p->base, 0, (size_t)p->stride * (h + 2 * H265_PAD));
    p->vis = p->base + (long)H265_PAD * p->stride + H265_PAD;
}
static void plane_pad(plane_t *p)
{
    for (int y = 0; y < p->h; y++) {
        uint8_t *row = p->vis + (long)y * p->stride;
        for (int i = 1; i <= H265_PAD; i++) { row[-i] = row[0]; row[p->w - 1 + i] = row[p->w - 1]; }
    }
    for (int i = 1; i <= H265_PAD; i++) {
        memcpy(p->vis - (long)i * p->stride - H265_PAD,
               p->vis - H265_PAD, (size_t)p->stride);
        memcpy(p->vis + (long)(p->h - 1 + i) * p->stride - H265_PAD,
               p->vis + (long)(p->h - 1) * p->stride - H265_PAD, (size_t)p->stride);
    }
}
static void plane_free(plane_t *p) { free(p->base); }

static int sample(const plane_t *p, int x, int y)
{
    x = h265_clip3(0, p->w - 1, x);
    y = h265_clip3(0, p->h - 1, y);
    return p->vis[(long)y * p->stride + x];
}

/* ---- the reference, straight from 8.5.3.3.3.1 / .2 ---------------------- */
static void ref_mc_luma(int16_t *dst, int ds, const plane_t *p,
                        int x, int y, int w, int h, int mvx, int mvy)
{
    int xi = x + (mvx >> 2), yi = y + (mvy >> 2);
    int xf = mvx & 3, yf = mvy & 3;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int v;
            if (!xf && !yf) {
                v = sample(p, xi + i, yi + j) << 6;
            } else if (!yf) {
                v = 0;
                for (int k = 0; k < 8; k++)
                    v += ref_luma[xf][k] * sample(p, xi + i + k - 3, yi + j);
            } else if (!xf) {
                v = 0;
                for (int k = 0; k < 8; k++)
                    v += ref_luma[yf][k] * sample(p, xi + i, yi + j + k - 3);
            } else {
                v = 0;
                for (int k = 0; k < 8; k++) {
                    int t = 0;
                    for (int m = 0; m < 8; m++)
                        t += ref_luma[xf][m] * sample(p, xi + i + m - 3, yi + j + k - 3);
                    v += ref_luma[yf][k] * t;
                }
                v >>= 6;
            }
            dst[j * ds + i] = (int16_t)v;
        }
}

static void ref_mc_chroma(int16_t *dst, int ds, const plane_t *p,
                          int x, int y, int w, int h, int mvx, int mvy)
{
    int xi = x + (mvx >> 3), yi = y + (mvy >> 3);
    int xf = mvx & 7, yf = mvy & 7;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int v;
            if (!xf && !yf) {
                v = sample(p, xi + i, yi + j) << 6;
            } else if (!yf) {
                v = 0;
                for (int k = 0; k < 4; k++)
                    v += ref_chroma[xf][k] * sample(p, xi + i + k - 1, yi + j);
            } else if (!xf) {
                v = 0;
                for (int k = 0; k < 4; k++)
                    v += ref_chroma[yf][k] * sample(p, xi + i, yi + j + k - 1);
            } else {
                v = 0;
                for (int k = 0; k < 4; k++) {
                    int t = 0;
                    for (int m = 0; m < 4; m++)
                        t += ref_chroma[xf][m] * sample(p, xi + i + m - 1, yi + j + k - 1);
                    v += ref_chroma[yf][k] * t;
                }
                v >>= 6;
            }
            dst[j * ds + i] = (int16_t)v;
        }
}

/* ---- 1. impulse response ------------------------------------------------ */
static void test_taps(void)
{
    plane_t p;
    plane_alloc(&p, 64, 64);
    /* An isolated non-zero sample far from every edge. */
    p.vis[32 * p.stride + 32] = 1;
    plane_pad(&p);

    int16_t dst[64 * 64];
    uint8_t emu[(64 + 8) * (64 + 8)];

    for (int f = 1; f < 4; f++) {
        /* Horizontal only. dst[0][i] picks out ref_luma[f][32 - (24 + i) + 3]
         * = the tap whose source offset lands on the impulse. */
        h265_mc_luma(dst, 64, p.vis, p.stride, p.w, p.h,
                     24, 32, 16, 1, f, 0, emu);
        for (int i = 0; i < 16; i++) {
            int k = 32 - (24 + i) + 3;
            int want = (k >= 0 && k < 8) ? ref_luma[f][k] : 0;
            CHECK(dst[i] == want, "luma h phase %d tap %d = %d, want %d",
                  f, k, dst[i], want);
        }
        /* Vertical only. */
        h265_mc_luma(dst, 64, p.vis, p.stride, p.w, p.h,
                     32, 24, 1, 16, 0, f, emu);
        for (int j = 0; j < 16; j++) {
            int k = 32 - (24 + j) + 3;
            int want = (k >= 0 && k < 8) ? ref_luma[f][k] : 0;
            CHECK(dst[j * 64] == want, "luma v phase %d tap %d = %d, want %d",
                  f, k, dst[j * 64], want);
        }
    }
    /* Phase 0 must be a pure copy scaled by 2^6. */
    h265_mc_luma(dst, 64, p.vis, p.stride, p.w, p.h, 24, 32, 16, 1, 0, 0, emu);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == ((24 + i == 32) ? 64 : 0), "luma phase 0 at %d", i);

    for (int f = 1; f < 8; f++) {
        h265_mc_chroma(dst, 64, p.vis, p.stride, p.w, p.h,
                       28, 32, 12, 1, f, 0, emu);
        for (int i = 0; i < 12; i++) {
            int k = 32 - (28 + i) + 1;
            int want = (k >= 0 && k < 4) ? ref_chroma[f][k] : 0;
            CHECK(dst[i] == want, "chroma h phase %d tap %d = %d, want %d",
                  f, k, dst[i], want);
        }
        h265_mc_chroma(dst, 64, p.vis, p.stride, p.w, p.h,
                       32, 28, 1, 12, 0, f, emu);
        for (int j = 0; j < 12; j++) {
            int k = 32 - (28 + j) + 1;
            int want = (k >= 0 && k < 4) ? ref_chroma[f][k] : 0;
            CHECK(dst[j * 64] == want, "chroma v phase %d tap %d = %d, want %d",
                  f, k, dst[j * 64], want);
        }
    }
    plane_free(&p);
}

/* ---- 2. against the reference implementation ---------------------------- */
static uint32_t seed = 99;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 9) % (unsigned)n); }

static void test_vs_reference(void)
{
    plane_t p;
    plane_alloc(&p, 96, 80);
    for (int y = 0; y < p.h; y++)
        for (int x = 0; x < p.w; x++)
            p.vis[(long)y * p.stride + x] = (uint8_t)rnd(256);
    plane_pad(&p);

    int16_t got[64 * 64], want[64 * 64];
    uint8_t emu[(64 + 8) * (64 + 8)];
    static const int sizes[] = { 4, 8, 12, 16, 24, 32, 64 };

    int bad = 0;
    for (int si = 0; si < 7 && !bad; si++) {
        for (int sj = 0; sj < 7 && !bad; sj++) {
            int w = sizes[si], h = sizes[sj];
            if (w > 64 || h > 64) continue;
            for (int t = 0; t < 24 && !bad; t++) {
                int x = rnd(p.w), y = rnd(p.h);
                int mvx, mvy;
                if (t < 16) {              /* every phase pair, near the middle */
                    mvx = (t & 3) + 4 * (rnd(9) - 4);
                    mvy = ((t >> 2) & 3) + 4 * (rnd(9) - 4);
                } else if (t < 20) {       /* over an edge, still inside the pad */
                    mvx = rnd(200) - 100; mvy = rnd(200) - 100;
                } else {                   /* far outside: the emulation path */
                    mvx = (rnd(4000) - 2000) * 4 + rnd(4);
                    mvy = (rnd(4000) - 2000) * 4 + rnd(4);
                }
                h265_mc_luma(got, 64, p.vis, p.stride, p.w, p.h, x, y, w, h, mvx, mvy, emu);
                ref_mc_luma(want, 64, &p, x, y, w, h, mvx, mvy);
                for (int j = 0; j < h && !bad; j++)
                    for (int i = 0; i < w; i++)
                        if (got[j * 64 + i] != want[j * 64 + i]) {
                            CHECK(0, "luma %dx%d at (%d,%d) mv (%d,%d): "
                                  "sample (%d,%d) = %d, want %d",
                                  w, h, x, y, mvx, mvy, i, j,
                                  got[j * 64 + i], want[j * 64 + i]);
                            bad = 1;
                            break;
                        }
            }
        }
    }
    CHECK(!bad, "luma interpolation matched the reference on every case");
    plane_free(&p);

    plane_alloc(&p, 48, 40);
    for (int y = 0; y < p.h; y++)
        for (int x = 0; x < p.w; x++)
            p.vis[(long)y * p.stride + x] = (uint8_t)rnd(256);
    plane_pad(&p);
    bad = 0;
    static const int csizes[] = { 2, 4, 6, 8, 12, 16, 32 };
    for (int si = 0; si < 7 && !bad; si++) {
        for (int sj = 0; sj < 7 && !bad; sj++) {
            int w = csizes[si], h = csizes[sj];
            for (int t = 0; t < 24 && !bad; t++) {
                int x = rnd(p.w), y = rnd(p.h);
                int mvx, mvy;
                if (t < 16) { mvx = (t & 3) * 2 + rnd(2) + 8 * (rnd(9) - 4);
                              mvy = ((t >> 2) & 3) * 2 + rnd(2) + 8 * (rnd(9) - 4); }
                else if (t < 20) { mvx = rnd(200) - 100; mvy = rnd(200) - 100; }
                else { mvx = (rnd(2000) - 1000) * 8 + rnd(8);
                       mvy = (rnd(2000) - 1000) * 8 + rnd(8); }
                h265_mc_chroma(got, 64, p.vis, p.stride, p.w, p.h, x, y, w, h, mvx, mvy, emu);
                ref_mc_chroma(want, 64, &p, x, y, w, h, mvx, mvy);
                for (int j = 0; j < h && !bad; j++)
                    for (int i = 0; i < w; i++)
                        if (got[j * 64 + i] != want[j * 64 + i]) {
                            CHECK(0, "chroma %dx%d at (%d,%d) mv (%d,%d): "
                                  "sample (%d,%d) = %d, want %d",
                                  w, h, x, y, mvx, mvy, i, j,
                                  got[j * 64 + i], want[j * 64 + i]);
                            bad = 1;
                            break;
                        }
            }
        }
    }
    CHECK(!bad, "chroma interpolation matched the reference on every case");
    plane_free(&p);
}

/* ---- 3. the DC-gain invariant ------------------------------------------- */
static void test_constant_plane(void)
{
    plane_t p;
    plane_alloc(&p, 40, 40);
    memset(p.base, 0, (size_t)p.stride * (p.h + 2 * H265_PAD));
    for (int y = 0; y < p.h; y++)
        memset(p.vis + (long)y * p.stride, 173, (size_t)p.w);
    plane_pad(&p);

    int16_t dst[64 * 64];
    uint8_t emu[(64 + 8) * (64 + 8)];
    for (int fy = 0; fy < 4; fy++)
        for (int fx = 0; fx < 4; fx++) {
            h265_mc_luma(dst, 64, p.vis, p.stride, p.w, p.h, 8, 8, 16, 16, fx, fy, emu);
            int ok = 1;
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16; i++) if (dst[j * 64 + i] != 173 * 64) ok = 0;
            CHECK(ok, "luma phase (%d,%d) on a constant plane: got %d, want %d",
                  fx, fy, dst[0], 173 * 64);
        }
    for (int fy = 0; fy < 8; fy++)
        for (int fx = 0; fx < 8; fx++) {
            h265_mc_chroma(dst, 64, p.vis, p.stride, p.w, p.h, 8, 8, 8, 8, fx, fy, emu);
            int ok = 1;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) if (dst[j * 64 + i] != 173 * 64) ok = 0;
            CHECK(ok, "chroma phase (%d,%d) on a constant plane: got %d",
                  fx, fy, dst[0]);
        }
    plane_free(&p);
}

/* ---- weighted sample prediction (8.5.3.3.4) ----------------------------- */
static void test_weighting(void)
{
    int16_t s0[16], s1[16];
    uint8_t dst[16];
    for (int i = 0; i < 16; i++) { s0[i] = (int16_t)(i * 700 - 3000); s1[i] = (int16_t)(8000 - i * 500); }

    /* default uni: (v + 32) >> 6 */
    h265_pred_uni(dst, 4, s0, 4, 4, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == h265_clip_u8((s0[i] + 32) >> 6), "uni[%d]", i);

    /* default bi: (v0 + v1 + 64) >> 7 */
    h265_pred_bi(dst, 4, s0, 4, s1, 4, 4, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == h265_clip_u8((s0[i] + s1[i] + 64) >> 7), "bi[%d]", i);

    /* explicit uni: log2Wd = denom + 6, always >= 1 for 8-bit */
    for (int denom = 0; denom <= 7; denom++) {
        int wgt = (1 << denom) + 3, off = -7;
        h265_pred_uni_w(dst, 4, s0, 4, 4, 4, denom, wgt, off);
        int log2wd = denom + 6;
        for (int i = 0; i < 16; i++) {
            int want = h265_clip_u8(((s0[i] * wgt + (1 << (log2wd - 1))) >> log2wd) + off);
            CHECK(dst[i] == want, "uni_w denom=%d [%d] = %d want %d",
                  denom, i, dst[i], want);
        }
    }

    /* explicit bi */
    for (int denom = 0; denom <= 7; denom++) {
        int w0 = (1 << denom) - 2, o0 = 5, w1 = (1 << denom) + 1, o1 = -4;
        h265_pred_bi_w(dst, 4, s0, 4, s1, 4, 4, 4, denom, w0, o0, w1, o1);
        int log2wd = denom + 6;
        for (int i = 0; i < 16; i++) {
            int v = s0[i] * w0 + s1[i] * w1 + ((o0 + o1 + 1) << log2wd);
            CHECK(dst[i] == h265_clip_u8(v >> (log2wd + 1)),
                  "bi_w denom=%d [%d]", denom, i);
        }
    }

    /* A weight of 1<<denom with offset 0 must reproduce the default path --
     * the identity that says the two formulas are the same arithmetic. */
    for (int denom = 0; denom <= 7; denom++) {
        uint8_t a[16], b[16];
        h265_pred_uni(a, 4, s0, 4, 4, 4);
        h265_pred_uni_w(b, 4, s0, 4, 4, 4, denom, 1 << denom, 0);
        CHECK(memcmp(a, b, 16) == 0, "unit weight at denom %d must equal default uni", denom);
        h265_pred_bi(a, 4, s0, 4, s1, 4, 4, 4);
        h265_pred_bi_w(b, 4, s0, 4, s1, 4, 4, 4, denom, 1 << denom, 0, 1 << denom, 0);
        CHECK(memcmp(a, b, 16) == 0, "unit weight at denom %d must equal default bi", denom);
    }
}

int main(void)
{
    test_taps();
    test_constant_plane();
    test_weighting();
    test_vs_reference();
    printf("h265_mc_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
