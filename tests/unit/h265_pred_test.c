/* tests/unit/h265_pred_test.c -- transforms, dequantisation and intra
 * prediction as modules (spec 8.6.2-8.6.4 and 8.4.4.2).
 *
 * The interesting thing to test here is the transform MATRIX, because
 * h265_pred.c builds it instead of typing it out. The construction claims
 * three properties -- the even rows of the N-point matrix are the rows of the
 * N/2-point one, column N-1-n is +-column n by row parity, and the odd rows
 * are the four small tables -- and this file checks the result against 4- and
 * 8-point matrices typed independently from the spec, so a slip in the
 * construction shows up as a wrong number rather than as a wrong picture
 * thirty frames later.
 *
 * The intra tests are hand-computed cases rather than a re-implementation:
 * mode 26 copies the top row, mode 10 copies the left column, mode 2 walks
 * down the left column, mode 34 walks along the top row, and mode 18 is the
 * 135-degree diagonal that reads from BOTH edges -- which is the one that
 * catches a sign error in the negative-angle projection.
 *
 * EVERY sample test below runs at BOTH 8 and 10 bits. That is the point of
 * passing the bit depth into these functions instead of reading a global: the
 * reference values are recomputed from the spec's bit-depth-dependent shifts
 * (bdShift = 20 - BitDepth, the dequant bdShift = BitDepth + log2 - 5, the
 * strong-smoothing threshold 1 << (BitDepth - 5)), so a constant left at its
 * 8-bit value fails here rather than thirty frames into a 10-bit stream.
 * Sample magnitudes are scaled by SC() so the 10-bit run exercises the same
 * shapes at the same relative levels rather than living in the bottom quarter
 * of the range.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

const int16_t *h265_test_transform_matrix(void);

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* An 8-bit-shaped literal at the depth under test. */
#define SC(v) ((v) << (bd - 8))

static void fillp(uint16_t *p, int n, int v) { for (int i = 0; i < n; i++) p[i] = (uint16_t)v; }

/* ---- the 4- and 8-point matrices, typed from the spec's Table in 8.6.4.2 -- */
static const int ref_T4[4][4] = {
    { 64,  64,  64,  64 },
    { 83,  36, -36, -83 },
    { 64, -64, -64,  64 },
    { 36, -83,  83, -36 }
};
static const int ref_T8[8][8] = {
    { 64,  64,  64,  64,  64,  64,  64,  64 },
    { 89,  75,  50,  18, -18, -50, -75, -89 },
    { 83,  36, -36, -83, -83, -36,  36,  83 },
    { 75, -18, -89, -50,  50,  89,  18, -75 },
    { 64, -64, -64,  64,  64, -64, -64,  64 },
    { 50, -89,  18,  75, -75, -18,  89, -50 },
    { 36, -83,  83, -36, -36,  83, -83,  36 },
    { 18, -50,  75, -89,  89, -75,  50, -18 }
};

static void test_matrix(void)
{
    const int16_t *T = h265_test_transform_matrix();
#define T32(k, n) T[(k) * 32 + (n)]

    /* The N-point matrix is the 32-point one sub-sampled in frequency. */
    for (int k = 0; k < 4; k++)
        for (int n = 0; n < 4; n++)
            CHECK(T32(k * 8, n) == ref_T4[k][n],
                  "T4[%d][%d] = %d, want %d", k, n, T32(k * 8, n), ref_T4[k][n]);
    for (int k = 0; k < 8; k++)
        for (int n = 0; n < 8; n++)
            CHECK(T32(k * 4, n) == ref_T8[k][n],
                  "T8[%d][%d] = %d, want %d", k, n, T32(k * 4, n), ref_T8[k][n]);

    /* The nesting property the construction relies on, checked rather than
     * assumed: column N-1-n is +-column n according to row parity. */
    for (int N = 4; N <= 32; N <<= 1) {
        int step = 32 / N;
        for (int k = 0; k < N; k++)
            for (int n = 0; n < N / 2; n++) {
                int a = T32(k * step, n), b = T32(k * step, N - 1 - n);
                CHECK((k & 1) ? (b == -a) : (b == a),
                      "N=%d nesting at k=%d n=%d (%d vs %d)", N, k, n, a, b);
            }
    }
#undef T32
}

/* ---- inverse transforms ------------------------------------------------- */
static void test_idct_dc(int bd)
{
    /* A DC-only block: the 2D inverse of coefficient c at (0,0) is
     * ((((c*64 + 64) >> 7) * 64) + (1 << (bdShift-1))) >> bdShift, flat across
     * the block, with bdShift = 20 - BitDepth. */
    int bdshift = 20 - bd, bdround = 1 << (bdshift - 1);
    for (int log2 = 2; log2 <= 5; log2++) {
        int n = 1 << log2;
        int16_t coeff[32 * 32];
        uint16_t dst[32 * 32];
        for (int c = -2048; c <= 2048; c += 137) {
            memset(coeff, 0, sizeof(int16_t) * (size_t)n * n);
            coeff[0] = (int16_t)c;
            fillp(dst, n * n, SC(100));
            h265_itransform_add(coeff, log2, 0, dst, n, bd);
            int stage1 = h265_clip3(-32768, 32767, (c * 64 + 64) >> 7);
            int want = h265_clip_pix(SC(100) + ((stage1 * 64 + bdround) >> bdshift), bd);
            int ok = 1;
            for (int i = 0; i < n * n; i++) if (dst[i] != want) ok = 0;
            CHECK(ok, "bd%d DC-only %dx%d c=%d: not flat at %d", bd, n, n, c, want);
        }
    }
}

static void test_idst(int bd)
{
    /* HM's fastInverseDst butterfly, written out as an independent reference:
     *   block[0] = 29*(x0+x2) + 55*(x2+x3) + 74*x1
     *   block[1] = 55*(x0-x3) - 29*(x2+x3) + 74*x1
     *   block[2] = 74*(x0 - x2 + x3)
     *   block[3] = 55*(x0+x2) + 29*(x0-x3) - 74*x1
     * applied down the columns then across the rows, with the spec's shifts. */
    int bdshift = 20 - bd, bdround = 1 << (bdshift - 1);
    int16_t coeff[16];
    uint16_t dst[16], want[16];
    unsigned seed = 7;
    for (int trial = 0; trial < 200; trial++) {
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245u + 12345u;
            coeff[i] = (int16_t)((int)((seed >> 9) & 1023) - 512);
        }
        int g[16];
        for (int x = 0; x < 4; x++) {
            int x0 = coeff[0 * 4 + x], x1 = coeff[1 * 4 + x];
            int x2 = coeff[2 * 4 + x], x3 = coeff[3 * 4 + x];
            int c0 = x0 + x2, c1 = x2 + x3, c2 = x0 - x3, c3 = 74 * x1;
            int o[4];
            o[0] = 29 * c0 + 55 * c1 + c3;
            o[1] = 55 * c2 - 29 * c1 + c3;
            o[2] = 74 * (x0 - x2 + x3);
            o[3] = 55 * c0 + 29 * c2 - c3;
            for (int i = 0; i < 4; i++)
                g[i * 4 + x] = h265_clip3(-32768, 32767, (o[i] + 64) >> 7);
        }
        for (int y = 0; y < 4; y++) {
            int x0 = g[y * 4 + 0], x1 = g[y * 4 + 1];
            int x2 = g[y * 4 + 2], x3 = g[y * 4 + 3];
            int c0 = x0 + x2, c1 = x2 + x3, c2 = x0 - x3, c3 = 74 * x1;
            int o[4];
            o[0] = 29 * c0 + 55 * c1 + c3;
            o[1] = 55 * c2 - 29 * c1 + c3;
            o[2] = 74 * (x0 - x2 + x3);
            o[3] = 55 * c0 + 29 * c2 - c3;
            for (int i = 0; i < 4; i++)
                want[y * 4 + i] = (uint16_t)h265_clip_pix(
                    SC(80) + ((o[i] + bdround) >> bdshift), bd);
        }
        fillp(dst, 16, SC(80));
        h265_itransform_add(coeff, 2, 1, dst, 4, bd);
        CHECK(memcmp(dst, want, sizeof dst) == 0,
              "bd%d 4x4 DST trial %d mismatch", bd, trial);
    }
}

static void test_transform_skip_and_bypass(int bd)
{
    int bdshift = 20 - bd, bdround = 1 << (bdshift - 1);
    int16_t coeff[16];
    uint16_t dst[16];
    for (int i = 0; i < 16; i++) coeff[i] = (int16_t)(i - 8);

    /* transform_skip: r = (d << (5 + log2)) rounded by bdShift = 20-BitDepth.
     * For 4x4 that is (d*128 + round) >> bdShift. */
    fillp(dst, 16, SC(128));
    h265_transform_skip_add(coeff, 2, dst, 4, bd);
    for (int i = 0; i < 16; i++) {
        /* * 128 rather than << 7: coeff[i] is signed and shifting a negative
         * value left is undefined, which UBSan reports. */
        int want = h265_clip_pix(SC(128) + (((int)coeff[i] * 128 + bdround) >> bdshift), bd);
        CHECK(dst[i] == want, "bd%d transform_skip[%d] = %d, want %d",
              bd, i, dst[i], want);
    }

    /* transquant bypass: the level IS the residual, no shift at all. */
    fillp(dst, 16, SC(128));
    h265_bypass_add(coeff, 2, dst, 4, bd);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == h265_clip_pix(SC(128) + coeff[i], bd),
              "bd%d bypass[%d] = %d, want %d", bd, i, dst[i],
              h265_clip_pix(SC(128) + coeff[i], bd));

    /* Clipping at BOTH ends, against the depth's own maximum. A clamp left at
     * 255 passes the 8-bit half of this and fails the 10-bit half, which is
     * exactly the bug this file exists to catch. */
    int16_t big[16];
    for (int i = 0; i < 16; i++) big[i] = (int16_t)(i < 8 ? (1 << bd) : -(1 << bd));
    fillp(dst, 16, SC(128));
    h265_bypass_add(big, 2, dst, 4, bd);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == (i < 8 ? (1 << bd) - 1 : 0), "bd%d bypass clip at %d (got %d)",
              bd, i, dst[i]);
}

static void test_dequant(int bd)
{
    /* 8.6.3 with a flat list: d = ((c*16*levelScale[qp%6] << (qp/6)) + (1 <<
     * (bdShift-1))) >> bdShift, bdShift = BitDepth + log2 - 5, clipped to
     * 16 bits. */
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    for (int log2 = 2; log2 <= 5; log2++) {
        int n = 1 << log2;
        for (int qp = 0; qp <= 51; qp += 7) {
            int16_t c[32 * 32];
            for (int i = 0; i < n * n; i++) c[i] = (int16_t)((i % 41) - 20);
            int16_t want[32 * 32];
            int sh = bd + log2 - 5;
            for (int i = 0; i < n * n; i++) {
                long long v = (long long)c[i] * 16 * level_scale[qp % 6] *
                              (1LL << (qp / 6));   /* not <<: c[i] is signed */
                v = (v + (1LL << (sh - 1))) >> sh;
                if (v < -32768) v = -32768;
                if (v > 32767) v = 32767;
                want[i] = (int16_t)(c[i] ? v : 0);
            }
            h265_dequant(c, n, qp, log2, 0, 0, bd);
            CHECK(memcmp(c, want, (size_t)n * n * 2) == 0,
                  "bd%d dequant %dx%d qp=%d mismatch", bd, n, n, qp);
        }
    }

    /* A non-flat scaling list must reach the coefficients, and the explicit
     * DC term must override position (0,0) for 16x16 and 32x32 only. */
    uint8_t list[64];
    for (int i = 0; i < 64; i++) list[i] = (uint8_t)(16 + i);
    int16_t c16[16 * 16], c8[8 * 8];
    /* Coefficient 64, not 1. The dequant right shift grows with the bit depth
     * (bdShift = BitDepth + log2 - 5), so a unit coefficient quantises two
     * ADJACENT scaling-list entries to the same output at 10 bits and the
     * "must vary across positions" assertion below becomes vacuous -- it
     * passed only because 8 bits happened to keep them apart. */
    for (int i = 0; i < 256; i++) c16[i] = 64;
    for (int i = 0; i < 64; i++) c8[i] = 64;
    int sh8 = bd + 3 - 5, sh16 = bd + 4 - 5;
    h265_dequant(c8, 8, 0, 3, list, 99, bd);
    CHECK(c8[0] == (int16_t)(((64 * list[0] * 40) + (1 << (sh8 - 1))) >> sh8),
          "bd%d 8x8 scaling list must NOT use the DC override (got %d)", bd, c8[0]);
    h265_dequant(c16, 16, 0, 4, list, 99, bd);
    CHECK(c16[0] == (int16_t)(((64 * 99 * 40) + (1 << (sh16 - 1))) >> sh16),
          "bd%d 16x16 DC override not applied (got %d)", bd, c16[0]);
    CHECK(c16[1] != c16[2] || c16[1] != c16[16],
          "bd%d a non-flat scaling list must vary across positions", bd);
}

/* ---- intra prediction --------------------------------------------------- */
/* Build the neighbour array in h265_pred.c's layout from callbacks. */
static void nb_fill(uint16_t *nb, int nbs, int corner,
                    const uint16_t *left, const uint16_t *top)
{
    for (int i = 0; i < 2 * nbs; i++) nb[2 * nbs - 1 - i] = left[i];
    nb[2 * nbs] = (uint16_t)corner;
    for (int i = 0; i < 2 * nbs; i++) nb[2 * nbs + 1 + i] = top[i];
}

static void test_intra_dc(int bd)
{
    for (int nbs = 4; nbs <= 32; nbs <<= 1) {
        uint16_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
        for (int i = 0; i < 2 * nbs; i++) { left[i] = SC(100); top[i] = SC(100); }
        nb_fill(nb, nbs, SC(100), left, top);
        h265_intra_pred(dst, nbs, nb, nbs, 1, 0, bd);
        int ok = 1;
        for (int i = 0; i < nbs * nbs; i++) if (dst[i] != SC(100)) ok = 0;
        CHECK(ok, "bd%d DC %dx%d on a constant edge must be that constant",
              bd, nbs, nbs);

        /* The boundary filter applies to luma below 32x32 and never to chroma. */
        for (int i = 0; i < 2 * nbs; i++) { left[i] = 0; top[i] = SC(200); }
        nb_fill(nb, nbs, 0, left, top);
        h265_intra_pred(dst, nbs, nb, nbs, 1, 0, bd);
        int log2 = nbs == 4 ? 2 : nbs == 8 ? 3 : nbs == 16 ? 4 : 5;
        int dc = (SC(200) * nbs + 0 * nbs + nbs) >> (1 + log2);
        if (nbs < 32) {
            CHECK(dst[0] == (uint16_t)((0 + 2 * dc + SC(200) + 2) >> 2),
                  "bd%d DC %d corner filter", bd, nbs);
            CHECK(dst[1] == (uint16_t)((SC(200) + 3 * dc + 2) >> 2),
                  "bd%d DC %d top filter", bd, nbs);
            CHECK(dst[nbs] == (uint16_t)((0 + 3 * dc + 2) >> 2),
                  "bd%d DC %d left filter", bd, nbs);
        }
        CHECK(dst[nbs + 1] == (uint16_t)dc, "bd%d DC %d interior", bd, nbs);

        h265_intra_pred(dst, nbs, nb, nbs, 1, 1, bd);   /* chroma: unfiltered */
        CHECK(dst[0] == (uint16_t)dc, "bd%d DC %d chroma must not be edge-filtered",
              bd, nbs);
    }
}

static void test_intra_planar(int bd)
{
    /* On a plane that is already bilinear in the edges, planar reproduces it.
     * Take left[y] = 50 + y, top[x] = 50 + x with matching far edges. */
    int nbs = 8;
    uint16_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
    for (int i = 0; i < 2 * nbs; i++) {
        left[i] = (uint16_t)SC(50 + i); top[i] = (uint16_t)SC(50 + i);
    }
    nb_fill(nb, nbs, SC(49), left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 0, 0, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++) {
            int want = ((nbs - 1 - x) * SC(50 + y) + (x + 1) * SC(50 + nbs) +
                        (nbs - 1 - y) * SC(50 + x) + (y + 1) * SC(50 + nbs) + nbs) >> 4;
            CHECK(dst[y * nbs + x] == want, "bd%d planar (%d,%d) = %d want %d",
                  bd, x, y, dst[y * nbs + x], want);
        }
}

static void test_intra_angular(int bd)
{
    int nbs = 8;
    uint16_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
    for (int i = 0; i < 2 * nbs; i++) {
        left[i] = (uint16_t)SC(10 + i); top[i] = (uint16_t)SC(80 + i);
    }

    /* Mode 26 is pure vertical: every row is the top row (chroma, so the
     * mode-26 edge filter that only luma gets does not interfere). */
    nb_fill(nb, nbs, SC(5), left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 26, 1, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x], "bd%d mode 26 (%d,%d)", bd, x, y);

    /* ... and for luma the first COLUMN is filtered by 8.4.4.2.6. */
    h265_intra_pred(dst, nbs, nb, nbs, 26, 0, bd);
    for (int y = 0; y < nbs; y++)
        CHECK(dst[y * nbs] == h265_clip_pix(top[0] + ((left[y] - SC(5)) >> 1), bd),
              "bd%d mode 26 luma column filter at y=%d", bd, y);
    for (int y = 0; y < nbs; y++)
        for (int x = 1; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x], "bd%d mode 26 luma interior (%d,%d)",
                  bd, x, y);

    /* Mode 10 is pure horizontal, mirrored. */
    h265_intra_pred(dst, nbs, nb, nbs, 10, 1, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == left[y], "bd%d mode 10 (%d,%d)", bd, x, y);
    h265_intra_pred(dst, nbs, nb, nbs, 10, 0, bd);
    for (int x = 0; x < nbs; x++)
        CHECK(dst[x] == h265_clip_pix(left[0] + ((top[x] - SC(5)) >> 1), bd),
              "bd%d mode 10 luma row filter at x=%d", bd, x);

    /* Mode 2 (angle +32, horizontal family): pred[x][y] = p[-1][x+y+1]. */
    h265_intra_pred(dst, nbs, nb, nbs, 2, 0, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == left[x + y + 1], "bd%d mode 2 (%d,%d)", bd, x, y);

    /* Mode 34 (angle +32, vertical family): pred[x][y] = p[x+y+1][-1]. */
    h265_intra_pred(dst, nbs, nb, nbs, 34, 0, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x + y + 1], "bd%d mode 34 (%d,%d)", bd, x, y);

    /* Mode 18 (angle -32): the 135-degree diagonal, which is the only one
     * that reads from both edges through the invAngle projection. */
    h265_intra_pred(dst, nbs, nb, nbs, 18, 0, bd);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++) {
            int want = (x > y) ? top[x - y - 1] : (x == y ? SC(5) : left[y - x - 1]);
            CHECK(dst[y * nbs + x] == want, "bd%d mode 18 (%d,%d) = %d want %d",
                  bd, x, y, dst[y * nbs + x], want);
        }

    /* A fractional angle must interpolate. Mode 3 has intraPredAngle 26, so
     * for x = 0 the projection is iIdx 0, iFact 26 and the sample is
     * ((32-26)*ref[y+1] + 26*ref[y+2] + 16) >> 5 -- with ref[k] = p[-1][k-1].
     * A square-wave left edge makes every such value land strictly between
     * the two it was mixed from, which a nearest-sample copy cannot produce.
     * The earlier version of this check used a unit ramp, where every
     * interpolation lands back on a grid value and the assertion was
     * vacuous. */
    for (int i = 0; i < 2 * nbs; i++) left[i] = (uint16_t)((i & 1) ? SC(64) : 0);
    nb_fill(nb, nbs, SC(5), left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 3, 0, bd);
    for (int y = 0; y < nbs; y++) {
        int want = (6 * left[y] + 26 * left[y + 1] + 16) >> 5;
        CHECK(dst[y * nbs] == want, "bd%d mode 3 (0,%d) = %d, want %d",
              bd, y, dst[y * nbs], want);
        CHECK(want != left[y] && want != left[y + 1],
              "bd%d mode 3 (0,%d) must be strictly between its two taps", bd, y);
    }
}

static void test_intra_filter(int bd)
{
    uint16_t nb[4 * 32 + 1], ref[4 * 32 + 1], left[64], top[64];
    size_t es = sizeof nb[0];

    /* 4x4 is never filtered, DC is never filtered, chroma is never filtered
     * (4:2:0), and the mode-distance threshold gates the rest. */
    for (int i = 0; i < 64; i++) {
        left[i] = (uint16_t)SC(i * 3); top[i] = (uint16_t)SC(255 - i * 3);
    }
    nb_fill(nb, 4, SC(128), left, top);
    memcpy(ref, nb, 17 * es);
    h265_intra_filter(nb, 4, 0, 0, 0, bd);
    CHECK(memcmp(nb, ref, 17 * es) == 0, "bd%d 4x4 must never be smoothed", bd);

    nb_fill(nb, 8, SC(128), left, top);
    memcpy(ref, nb, 33 * es);
    h265_intra_filter(nb, 8, 1, 0, 0, bd);
    CHECK(memcmp(nb, ref, 33 * es) == 0, "bd%d DC must never be smoothed", bd);
    h265_intra_filter(nb, 8, 0, 1, 0, bd);
    CHECK(memcmp(nb, ref, 33 * es) == 0, "bd%d 4:2:0 chroma must never be smoothed", bd);
    h265_intra_filter(nb, 8, 26, 0, 0, bd);
    CHECK(memcmp(nb, ref, 33 * es) == 0, "bd%d 8x8 mode 26 is inside the threshold", bd);
    h265_intra_filter(nb, 8, 18, 0, 0, bd);
    CHECK(memcmp(nb, ref, 33 * es) != 0, "bd%d 8x8 mode 18 must be smoothed", bd);

    /* The filter is [1,2,1]/4 across the whole run with the ends held. */
    nb_fill(nb, 8, SC(128), left, top);
    memcpy(ref, nb, 33 * es);
    h265_intra_filter(nb, 8, 0, 0, 0, bd);
    CHECK(nb[0] == ref[0] && nb[32] == ref[32],
          "bd%d the run's ends are not filtered", bd);
    for (int i = 1; i < 32; i++)
        CHECK(nb[i] == (uint16_t)((ref[i - 1] + 2 * ref[i] + ref[i + 1] + 2) >> 2),
              "bd%d smoothing at %d", bd, i);

    /* Strong intra smoothing: 32x32 luma, edges near-linear, so the whole run
     * becomes an exact linear ramp between the three corner samples. The
     * flatness threshold is 1 << (BitDepth - 5) -- 8 at 8 bits, 32 at 10 --
     * and the scaled ramp below stays inside it at both depths. */
    for (int i = 0; i < 64; i++) {
        left[i] = (uint16_t)SC(100 + i); top[i] = (uint16_t)SC(100 + i);
    }
    nb_fill(nb, 32, SC(100), left, top);
    h265_intra_filter(nb, 32, 0, 0, 1, bd);
    int corner = SC(100), bl = left[63], tr = top[63];
    CHECK(nb[0] == bl && nb[64] == corner && nb[128] == tr,
          "bd%d strong smoothing must keep the three anchors", bd);
    for (int y = 0; y < 63; y++)
        CHECK(nb[63 - y] == (uint16_t)(((63 - y) * corner + (y + 1) * bl + 32) >> 6),
              "bd%d strong smoothing left at y=%d", bd, y);
    for (int x = 0; x < 63; x++)
        CHECK(nb[65 + x] == (uint16_t)(((63 - x) * corner + (x + 1) * tr + 32) >> 6),
              "bd%d strong smoothing top at x=%d", bd, x);

    /* With a step in the middle of the edge the flatness test must FAIL and
     * the ordinary [1,2,1] filter must be used instead. */
    for (int i = 0; i < 64; i++) {
        left[i] = (uint16_t)(i < 32 ? SC(40) : SC(200)); top[i] = (uint16_t)SC(120);
    }
    nb_fill(nb, 32, SC(120), left, top);
    memcpy(ref, nb, 129 * es);
    h265_intra_filter(nb, 32, 0, 0, 1, bd);
    CHECK(nb[1] == (uint16_t)((ref[0] + 2 * ref[1] + ref[2] + 2) >> 2),
          "bd%d a non-flat edge must fall back to [1,2,1]", bd);
}

/* The strong-smoothing threshold, isolated. At 10 bits the qualifying window
 * is four times wider, so an edge whose curvature is 20 (in 10-bit units) is
 * flat enough at 10 bits and would NOT be at 8. A threshold left at 1 << 3
 * takes the [1,2,1] branch here and the reconstruction silently diverges.
 * This is the check that fails if that constant is not scaled. */
static void test_strong_smoothing_threshold(void)
{
    uint16_t nb[4 * 32 + 1], plain[4 * 32 + 1], left[64], top[64];
    size_t es = sizeof nb[0];
    int bd = 10;

    /* corner + tr - 2*top[31] = 20, i.e. inside 32 but outside 8. */
    int corner = 400;
    for (int i = 0; i < 64; i++) { left[i] = 400; top[i] = 400; }
    top[31] = 390; top[63] = 400;         /* a = 400 + 400 - 780 = 20 */
    left[31] = 390; left[63] = 400;       /* b = 20 as well */

    nb_fill(nb, 32, corner, left, top);
    h265_intra_filter(nb, 32, 0, 0, 1, bd);

    /* What the [1,2,1] fallback would have produced, for comparison. */
    nb_fill(plain, 32, corner, left, top);
    h265_intra_filter(plain, 32, 0, 0, 0, bd);   /* strong off -> always [1,2,1] */

    CHECK(memcmp(nb, plain, 129 * es) != 0,
          "10-bit strong smoothing must engage at curvature 20 "
          "(threshold is 1 << (BitDepth-5) = 32, not 8)");
    /* And the result must be the exact bi-linear ramp. */
    int bl = left[63], tr = top[63];
    for (int x = 0; x < 63; x++)
        CHECK(nb[65 + x] == (uint16_t)(((63 - x) * corner + (x + 1) * tr + 32) >> 6),
              "10-bit strong ramp top at x=%d", x);
    for (int y = 0; y < 63; y++)
        CHECK(nb[63 - y] == (uint16_t)(((63 - y) * corner + (y + 1) * bl + 32) >> 6),
              "10-bit strong ramp left at y=%d", y);
}

int main(void)
{
    test_matrix();
    for (int i = 0; i < 2; i++) {
        int bd = i ? 10 : 8;
        test_idct_dc(bd);
        test_idst(bd);
        test_transform_skip_and_bypass(bd);
        test_dequant(bd);
        test_intra_dc(bd);
        test_intra_planar(bd);
        test_intra_angular(bd);
        test_intra_filter(bd);
    }
    test_strong_smoothing_threshold();
    printf("h265_pred_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
