/* tests/unit/h264_mc_test.c -- host unit test for c/lib/video/h264_mc.c.
 *
 * Strategy: build deterministic reference planes (visible area + H264_PAD
 * replicated-edge margin, all filled with an LCG pattern so the 6-tap filter
 * reads real data everywhere), then compare h264_mc_block() against a slow
 * REFERENCE implementation below that is written directly from the spec text
 * (ITU-T H.264 (2023) 8.4.2.2.1 / 8.4.2.2.2 / 8.4.3) and shares no code with
 * the module. Notably the reference derives the centre half-pel j from
 * UNCLIPPED VERTICAL intermediates filtered horizontally, while the module
 * uses horizontal intermediates filtered vertically -- the spec declares
 * both identical, so this cross-checks the trickiest position both ways.
 *
 * Build: gcc -std=c99 -O2 -Wall -Wextra -I c/lib/video \
 *          -o /tmp/h264_mc_test tests/unit/h264_mc_test.c c/lib/video/h264_mc.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "h264_int.h"   /* H264_PAD + prototypes under test */

static int g_checks, g_fails;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        if (g_fails <= 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); printf("\n"); } \
    } \
} while (0)

/* ------------------------------------------------------------ test data -- */
/* Deterministic pseudo-random fill (32-bit LCG), so every tap of the 6-tap
 * filter sees non-trivial data, including inside the pad margin. */
static uint32_t g_lcg = 0x12345678u;
static uint8_t nextrand(void)
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (uint8_t)(g_lcg >> 24);
}

#define LVW 64                 /* luma visible width/height */
#define LST (LVW + 2 * H264_PAD)
static uint8_t g_lbuf[LST * LST];

#define CVW 32                 /* chroma visible width/height */
#define CST (CVW + 2 * H264_PAD)
static uint8_t g_cbuf[CST * CST];

/* ------------------------------------------- slow spec reference (luma) -- */
/* Written straight from 8.4.2.2.1. PX addresses absolute plane coords; the
 * caller guarantees everything is inside the padded buffer. */
static int rclip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static int PX(const uint8_t *p, int st, int X, int Y)
{
    return p[Y * st + X];
}

/* b-type: horizontal 6-tap, clipped. Half position between (X,Y) & (X+1,Y). */
static int r_hhalf(const uint8_t *p, int st, int X, int Y)
{
    int b1 = PX(p, st, X - 2, Y) - 5 * PX(p, st, X - 1, Y)
           + 20 * PX(p, st, X, Y) + 20 * PX(p, st, X + 1, Y)
           - 5 * PX(p, st, X + 2, Y) + PX(p, st, X + 3, Y);
    return rclip((b1 + 16) >> 5);
}

/* h-type: vertical 6-tap, clipped. Half position between (X,Y) & (X,Y+1). */
static int r_vhalf(const uint8_t *p, int st, int X, int Y)
{
    int h1 = PX(p, st, X, Y - 2) - 5 * PX(p, st, X, Y - 1)
           + 20 * PX(p, st, X, Y) + 20 * PX(p, st, X, Y + 1)
           - 5 * PX(p, st, X, Y + 2) + PX(p, st, X, Y + 3);
    return rclip((h1 + 16) >> 5);
}

/* j-type: centre half. Reference direction: UNCLIPPED VERTICAL intermediates
 * at columns X-2..X+3 (each centred between rows Y and Y+1), combined
 * horizontally, then (j1 + 512) >> 10 and clip. The module goes the other
 * way (horizontal intermediates, vertical combine); the spec says the two
 * are identical, so agreement here is a real cross-check. */
static int r_jhalf(const uint8_t *p, int st, int X, int Y)
{
    int v1[6], i, j1;
    for (i = 0; i < 6; i++) {
        int col = X - 2 + i;
        v1[i] = PX(p, st, col, Y - 2) - 5 * PX(p, st, col, Y - 1)
              + 20 * PX(p, st, col, Y) + 20 * PX(p, st, col, Y + 1)
              - 5 * PX(p, st, col, Y + 2) + PX(p, st, col, Y + 3);
    }
    j1 = v1[0] - 5 * v1[1] + 20 * v1[2] + 20 * v1[3] - 5 * v1[4] + v1[5];
    return rclip((j1 + 512) >> 10);
}

/* Prediction sample at integer anchor (X,Y), quarter-pel fraction (fx,fy). */
static int ref_luma_px(const uint8_t *p, int st, int X, int Y, int fx, int fy)
{
    switch (fy * 4 + fx) {
    case 0:  return PX(p, st, X, Y);
    case 1:  return (PX(p, st, X, Y) + r_hhalf(p, st, X, Y) + 1) >> 1;
    case 2:  return r_hhalf(p, st, X, Y);
    case 3:  return (r_hhalf(p, st, X, Y) + PX(p, st, X + 1, Y) + 1) >> 1;
    case 4:  return (PX(p, st, X, Y) + r_vhalf(p, st, X, Y) + 1) >> 1;
    case 5:  return (r_hhalf(p, st, X, Y) + r_vhalf(p, st, X, Y) + 1) >> 1;
    case 6:  return (r_hhalf(p, st, X, Y) + r_jhalf(p, st, X, Y) + 1) >> 1;
    case 7:  return (r_hhalf(p, st, X, Y) + r_vhalf(p, st, X + 1, Y) + 1) >> 1;
    case 8:  return r_vhalf(p, st, X, Y);
    case 9:  return (r_vhalf(p, st, X, Y) + r_jhalf(p, st, X, Y) + 1) >> 1;
    case 10: return r_jhalf(p, st, X, Y);
    case 11: return (r_jhalf(p, st, X, Y) + r_vhalf(p, st, X + 1, Y) + 1) >> 1;
    case 12: return (r_vhalf(p, st, X, Y) + PX(p, st, X, Y + 1) + 1) >> 1;
    case 13: return (r_hhalf(p, st, X, Y + 1) + r_vhalf(p, st, X, Y) + 1) >> 1;
    case 14: return (r_jhalf(p, st, X, Y) + r_hhalf(p, st, X, Y + 1) + 1) >> 1;
    default: return (r_hhalf(p, st, X, Y + 1) + r_vhalf(p, st, X + 1, Y) + 1) >> 1;
    }
}

/* ----------------------------------------- slow spec reference (chroma) -- */
/* 8.4.2.2.2: 1/8-pel bilinear, no clipping in the spec formula. */
static int ref_chroma_px(const uint8_t *p, int st, int X, int Y, int xf, int yf)
{
    int A = PX(p, st, X, Y),     B = PX(p, st, X + 1, Y);
    int C = PX(p, st, X, Y + 1), D = PX(p, st, X + 1, Y + 1);
    return ((8 - xf) * (8 - yf) * A + xf * (8 - yf) * B
          + (8 - xf) * yf * C + xf * yf * D + 32) >> 6;
}

/* ----------------------------------------- slow spec reference (weight) -- */
/* 8.4.3, written in the algebraically identical doubled form to stay an
 * independent implementation: (a + 2^(k-1)) >> k == (2a + 2^k) >> (k+1). */
static int ref_weight(int v, int log2w, int wgt, int off)
{
    int t;
    if (log2w == 0)
        t = v * wgt + off;
    else
        t = ((2 * v * wgt + (1 << log2w)) >> (log2w + 1)) + off;
    return rclip(t);
}

/* ------------------------------------------------------------ test bodies -- */
static void test_luma(void)
{
    static const int sizes[3] = { 4, 8, 16 };
    static const int orig[2][2] = { { 0, 0 }, { 40, 24 } };
    static const int qxs[4] = { -16, -3, 7, 19 };
    static const int qys[3] = { -13, 0, 11 };
    const uint8_t *ref = g_lbuf + H264_PAD * LST + H264_PAD;
    uint8_t got[16 * 16];
    int oi, si, sj, xi, yi, fx, fy;

    for (oi = 0; oi < 2; oi++)
    for (si = 0; si < 3; si++)
    for (sj = 0; sj < 3; sj++)
    for (xi = 0; xi < 4; xi++)
    for (yi = 0; yi < 3; yi++)
    for (fy = 0; fy < 4; fy++)
    for (fx = 0; fx < 4; fx++) {
        int x = orig[oi][0], y = orig[oi][1];
        int w = sizes[si], h = sizes[sj];
        int mvx = qxs[xi] * 4 + fx, mvy = qys[yi] * 4 + fy;
        int X = x + qxs[xi], Y = y + qys[yi];
        int r, c;

        memset(got, 0xAA, sizeof(got));
        h264_mc_block(got, 16, ref, LST, x, y, w, h, mvx, mvy, 1);

        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++) {
                int want = ref_luma_px(ref, LST, X + c, Y + r, fx, fy);
                CHECK(got[r * 16 + c] == want,
                      "luma x=%d y=%d w=%d h=%d mv=(%d,%d) px=(%d,%d): got %d want %d",
                      x, y, w, h, mvx, mvy, c, r, got[r * 16 + c], want);
                if (g_fails > 20) return;
            }
        /* dst rows beyond w must be untouched (stride honoured) */
        for (r = 0; r < h; r++)
            for (c = w; c < 16; c++)
                CHECK(got[r * 16 + c] == 0xAA, "luma dst overwrite at w=%d", w);
    }
}

static void test_chroma(void)
{
    static const int qis[3] = { -8, 0, 5 };
    const uint8_t *ref = g_cbuf + H264_PAD * CST + H264_PAD;
    uint8_t got[16 * 16];
    int xi, yi, xf, yf;

    /* Full (xf,yf) 8x8 grid on an 8x8 block, several integer offsets. */
    for (xi = 0; xi < 3; xi++)
    for (yi = 0; yi < 3; yi++)
    for (yf = 0; yf < 8; yf++)
    for (xf = 0; xf < 8; xf++) {
        int x = 8, y = 4, w = 8, h = 8;
        int mvx = qis[xi] * 8 + xf, mvy = qis[yi] * 8 + yf;
        int X = x + qis[xi], Y = y + qis[yi];
        int r, c;

        memset(got, 0xAA, sizeof(got));
        h264_mc_block(got, 16, ref, CST, x, y, w, h, mvx, mvy, 0);

        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++) {
                int want = ref_chroma_px(ref, CST, X + c, Y + r, xf, yf);
                CHECK(got[r * 16 + c] == want,
                      "chroma mv=(%d,%d) px=(%d,%d): got %d want %d",
                      mvx, mvy, c, r, got[r * 16 + c], want);
                if (g_fails > 20) return;
            }
    }

    /* Other block geometries incl. negative-MV corner cases. */
    {
        static const int geo[][6] = {
            /* x, y, w, h, mvx, mvy */
            { 0, 0, 4, 4, -63, -57 },   /* far into the top-left pad */
            { 0, 0, 16, 16, 39, 41 },
            { 4, 12, 4, 16, -31, 7 },
            { 12, 0, 16, 4, 63, -25 },
            { 0, 8, 8, 16, 0, 0 },     /* integer position */
        };
        unsigned gi;
        for (gi = 0; gi < sizeof(geo) / sizeof(geo[0]); gi++) {
            int x = geo[gi][0], y = geo[gi][1], w = geo[gi][2], h = geo[gi][3];
            int mvx = geo[gi][4], mvy = geo[gi][5];
            int qx = mvx / 8, qy = mvy / 8, xf, yf, r, c;
            xf = mvx - qx * 8; if (xf < 0) { xf += 8; qx--; }
            yf = mvy - qy * 8; if (yf < 0) { yf += 8; qy--; }

            memset(got, 0xAA, sizeof(got));
            h264_mc_block(got, 16, ref, CST, x, y, w, h, mvx, mvy, 0);
            for (r = 0; r < h; r++)
                for (c = 0; c < w; c++) {
                    int want = ref_chroma_px(ref, CST, x + qx + c, y + qy + r,
                                             xf, yf);
                    CHECK(got[r * 16 + c] == want,
                          "chroma geo %u px=(%d,%d): got %d want %d",
                          gi, c, r, got[r * 16 + c], want);
                }
        }
    }
}

static void test_weight(void)
{
    static const int log2ws[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const int wgts[9] = { 0, 1, -1, 2, 32, 64, 127, -128, -64 };
    static const int offs[7] = { 0, 1, -1, 255, -255, 128, -128 };
    static const int pix[8] = { 0, 1, 127, 128, 254, 255, 3, 200 };
    uint8_t blk[8 * 8];
    int li, wi, oi, pi, pj;

    for (li = 0; li < 8; li++)
    for (wi = 0; wi < 9; wi++)
    for (oi = 0; oi < 7; oi++) {
        /* 8 pixels on the diagonal of a 4x4-ish layout: use w=4,h=2 so each
         * distinct pixel value lands exactly once per row. */
        for (pj = 0; pj < 8; pj++) blk[pj] = (uint8_t)pix[pj];
        h264_mc_weight(blk, 4, 4, 2,
                       log2ws[li], wgts[wi], offs[oi]);
        for (pi = 0; pi < 8; pi++) {
            int want = ref_weight(pix[pi], log2ws[li], wgts[wi], offs[oi]);
            CHECK(blk[pi] == want,
                  "weight log2w=%d w=%d o=%d v=%d: got %d want %d",
                  log2ws[li], wgts[wi], offs[oi], pix[pi], blk[pi], want);
        }
    }

    /* Rounding-boundary spot checks with explicit expected values.
     * Geometry w=4,h=2,stride=8; columns 4..7 are sentinels that must stay
     * untouched. */
    {
        uint8_t b1[16] = { 1, 3, 255, 0, 0xEE, 0xEE, 0xEE, 0xEE,
                           5, 7, 9, 11, 0xEE, 0xEE, 0xEE, 0xEE };
        /* log2w=1, w=1, o=0: v -> (2v+2)>>2 = (v+1)>>1 */
        h264_mc_weight(b1, 8, 4, 2, 1, 1, 0);
        CHECK(b1[0] == 1 && b1[1] == 2 && b1[2] == 128 && b1[3] == 0 &&
              b1[8] == 3 && b1[9] == 4 && b1[10] == 5 && b1[11] == 6 &&
              b1[4] == 0xEE && b1[15] == 0xEE,
              "weight rounding boundary (v+1)>>1: %d %d %d %d",
              b1[0], b1[1], b1[2], b1[3]);
    }
    {
        uint8_t b2[16] = { 10, 250, 100, 55, 0xEE, 0xEE, 0xEE, 0xEE,
                           10, 250, 100, 55, 0xEE, 0xEE, 0xEE, 0xEE };
        /* log2w=0, w=2, o=300: clip(2v+300) -- offset inside the clip */
        h264_mc_weight(b2, 8, 4, 2, 0, 2, 300);
        CHECK(b2[0] == 255 && b2[1] == 255 && b2[2] == 255 && b2[3] == 255 &&
              b2[8] == 255 && b2[11] == 255 &&
              b2[4] == 0xEE && b2[15] == 0xEE,
              "weight log2w=0 high clip: %d %d %d %d",
              b2[0], b2[1], b2[2], b2[3]);
    }
    {
        uint8_t b3[16] = { 10, 250, 100, 55, 0xEE, 0xEE, 0xEE, 0xEE,
                           10, 250, 100, 55, 0xEE, 0xEE, 0xEE, 0xEE };
        /* log2w=0, w=-1, o=20: clip(20-v) */
        h264_mc_weight(b3, 8, 4, 2, 0, -1, 20);
        CHECK(b3[0] == 10 && b3[1] == 0 && b3[2] == 0 && b3[3] == 0 &&
              b3[8] == 10 && b3[9] == 0 &&
              b3[4] == 0xEE && b3[15] == 0xEE,
              "weight log2w=0 negative weight: %d %d %d %d",
              b3[0], b3[1], b3[2], b3[3]);
    }
    {
        /* stride honoured: a 2x2 block in a 4-wide buffer, sentinel cols */
        uint8_t b4[8] = { 9, 9, 0xEE, 0xEE, 9, 9, 0xEE, 0xEE };
        h264_mc_weight(b4, 4, 2, 2, 0, 1, 1);
        CHECK(b4[0] == 10 && b4[1] == 10 && b4[4] == 10 && b4[5] == 10 &&
              b4[2] == 0xEE && b4[3] == 0xEE && b4[6] == 0xEE && b4[7] == 0xEE,
              "weight stride/sentinel");
    }
}

static void test_robust(void)
{
    uint8_t buf[16 * 16];
    const uint8_t *ref = g_lbuf + H264_PAD * LST + H264_PAD;

    /* Bad sizes / nulls must be silent no-ops, never a crash. */
    memset(buf, 0x55, sizeof(buf));
    h264_mc_block(buf, 16, ref, LST, 0, 0, 5, 8, 0, 0, 1);
    h264_mc_block(buf, 16, ref, LST, 0, 0, 8, 0, 0, 0, 1);
    h264_mc_block(buf, 16, ref, LST, 0, 0, -4, 4, 0, 0, 1);
    h264_mc_block(NULL, 16, ref, LST, 0, 0, 8, 8, 0, 0, 1);
    h264_mc_block(buf, 16, NULL, LST, 0, 0, 8, 8, 0, 0, 1);
    h264_mc_weight(buf, 16, 3, 8, 2, 1, 0);
    h264_mc_weight(NULL, 16, 8, 8, 2, 1, 0);
    CHECK(buf[0] == 0x55, "robust: buf written by rejected calls");

    /* Out-of-range weighting parameters are clamped into the spec range
     * (still bounded arithmetic, no crash); content changes are fine here. */
    h264_mc_weight(buf, 16, 8, 8, -3, 1, 0);
    h264_mc_weight(buf, 16, 8, 8, 42, 1, 0);
    g_checks++;

    /* Largest MVs that still stay inside the pad must not crash and must
     * still match the filter output at the pad boundary. */
    {
        uint8_t want[16 * 16];
        int r, c;
        /* X = 0 + (-116)/4 = -29: taps reach -31, one pixel inside the pad. */
        h264_mc_block(buf, 16, ref, LST, 0, 0, 16, 16, -116, -116, 1);
        for (r = 0; r < 16; r++)
            for (c = 0; c < 16; c++)
                want[r * 16 + c] = (uint8_t)ref_luma_px(ref, LST,
                                                        -29 + c, -29 + r, 0, 0);
        CHECK(memcmp(buf, want, sizeof(buf)) == 0, "robust: pad-edge MV mismatch");
        /* X = 48 + 29 = 77: taps reach 77+16+2 = 95, the last padded pixel. */
        h264_mc_block(buf, 16, ref, LST, 48, 48, 16, 16, 119, 119, 1);
        g_checks++;
    }
}

int main(void)
{
    unsigned i;
    for (i = 0; i < sizeof(g_lbuf); i++) g_lbuf[i] = nextrand();
    for (i = 0; i < sizeof(g_cbuf); i++) g_cbuf[i] = nextrand();

    test_luma();
    test_chroma();
    test_weight();
    test_robust();

    printf("h264_mc_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
