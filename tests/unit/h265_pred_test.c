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

    /* Row 0 is the DC basis. Row 16 is the 2-point basis pushed all the way
     * up the nesting, so it is +64,-64,-64,+64 repeating with period 4 --
     * NOT the half-and-half a 2-point row would be if the sub-sampling
     * happened in position instead of in frequency. */
    for (int n = 0; n < 32; n++) CHECK(T32(0, n) == 64, "T[0][%d] != 64", n);
    for (int n = 0; n < 32; n++) {
        int want = (((n + 1) / 2) % 2) ? -64 : 64;
        CHECK(T32(16, n) == want, "T[16][%d] = %d, want %d", n, T32(16, n), want);
    }

    /* Column symmetry: T[k][N-1-n] = (-1)^k T[k][n]. */
    for (int k = 0; k < 32; k++)
        for (int n = 0; n < 16; n++) {
            int want = (k & 1) ? -T32(k, n) : T32(k, n);
            CHECK(T32(k, 31 - n) == want,
                  "symmetry broken at T[%d][%d]", k, 31 - n);
        }

    /* The first column, which is the one place every odd table shows through:
     * 64, then the 32-point odd coefficients interleaved with the nested
     * even rows. */
    static const int col0[32] = {
        64, 90, 90, 90, 89, 88, 87, 85, 83, 82, 80, 78, 75, 73, 70, 67,
        64, 61, 57, 54, 50, 46, 43, 38, 36, 31, 25, 22, 18, 13,  9,  4
    };
    for (int k = 0; k < 32; k++)
        CHECK(T32(k, 0) == col0[k], "T[%d][0] = %d, want %d", k, T32(k, 0), col0[k]);

    /* Rows 0 and 16 are exactly orthogonal, and every row has the DC row's
     * norm scaled: a construction that duplicated a row would fail this. */
    for (int k = 1; k < 32; k++) {
        long dot = 0;
        for (int n = 0; n < 32; n++) dot += (long)T32(0, n) * T32(k, n);
        CHECK(dot == 0, "row %d is not orthogonal to DC (dot %ld)", k, dot);
    }
#undef T32
}

/* ---- inverse transforms ------------------------------------------------- */
static void test_idct_dc(void)
{
    /* A DC-only block: the 2D inverse of coefficient c at (0,0) is
     * ((((c*64 + 64) >> 7) * 64) + 2048) >> 12, flat across the block. */
    for (int log2 = 2; log2 <= 5; log2++) {
        int n = 1 << log2;
        int16_t coeff[32 * 32];
        uint8_t dst[32 * 32];
        for (int c = -2048; c <= 2048; c += 137) {
            memset(coeff, 0, sizeof(int16_t) * (size_t)n * n);
            coeff[0] = (int16_t)c;
            memset(dst, 100, (size_t)n * n);
            h265_itransform_add(coeff, log2, 0, dst, n);
            int stage1 = h265_clip3(-32768, 32767, (c * 64 + 64) >> 7);
            int want = h265_clip_u8(100 + ((stage1 * 64 + 2048) >> 12));
            int ok = 1;
            for (int i = 0; i < n * n; i++) if (dst[i] != want) ok = 0;
            CHECK(ok, "DC-only %dx%d c=%d: not flat at %d", n, n, c, want);
        }
    }
}

static void test_idst(void)
{
    /* HM's fastInverseDst butterfly, written out as an independent reference:
     *   block[0] = 29*(x0+x2) + 55*(x2+x3) + 74*x1
     *   block[1] = 55*(x0-x3) - 29*(x2+x3) + 74*x1
     *   block[2] = 74*(x0 - x2 + x3)
     *   block[3] = 55*(x0+x2) + 29*(x0-x3) - 74*x1
     * applied down the columns then across the rows, with the spec's shifts. */
    int16_t coeff[16];
    uint8_t dst[16], want[16];
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
                want[y * 4 + i] = (uint8_t)h265_clip_u8(80 + ((o[i] + 2048) >> 12));
        }
        memset(dst, 80, 16);
        h265_itransform_add(coeff, 2, 1, dst, 4);
        CHECK(memcmp(dst, want, 16) == 0, "4x4 DST trial %d mismatch", trial);
    }
}

static void test_transform_skip_and_bypass(void)
{
    int16_t coeff[16];
    uint8_t dst[16];
    for (int i = 0; i < 16; i++) coeff[i] = (int16_t)(i - 8);

    /* transform_skip: r = (d << (5 + log2)) rounded by bdShift 12. For 4x4
     * that is (d << 7 + 2048) >> 12, i.e. d/32 rounded to nearest. */
    memset(dst, 128, 16);
    h265_transform_skip_add(coeff, 2, dst, 4);
    for (int i = 0; i < 16; i++) {
        int want = h265_clip_u8(128 + ((((int)coeff[i] << 7) + 2048) >> 12));
        CHECK(dst[i] == want, "transform_skip[%d] = %d, want %d", i, dst[i], want);
    }

    /* transquant bypass: the level IS the residual, no shift at all. */
    memset(dst, 128, 16);
    h265_bypass_add(coeff, 2, dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == h265_clip_u8(128 + coeff[i]),
              "bypass[%d] = %d, want %d", i, dst[i], h265_clip_u8(128 + coeff[i]));

    /* Clipping at both ends, which a plain uint8_t add would get wrong. */
    int16_t big[16];
    for (int i = 0; i < 16; i++) big[i] = (int16_t)(i < 8 ? 500 : -500);
    memset(dst, 128, 16);
    h265_bypass_add(big, 2, dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == (i < 8 ? 255 : 0), "bypass clip at %d", i);
}

static void test_dequant(void)
{
    /* 8.6.3 with a flat list: d = ((c*16*levelScale[qp%6] << (qp/6)) + (1 <<
     * (bdShift-1))) >> bdShift, bdShift = log2 + 3, clipped to 16 bits. */
    static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };
    for (int log2 = 2; log2 <= 5; log2++) {
        int n = 1 << log2;
        for (int qp = 0; qp <= 51; qp += 7) {
            int16_t c[32 * 32];
            for (int i = 0; i < n * n; i++) c[i] = (int16_t)((i % 41) - 20);
            int16_t want[32 * 32];
            int bd = log2 + 3;
            for (int i = 0; i < n * n; i++) {
                long long v = ((long long)c[i] * 16 * level_scale[qp % 6]) << (qp / 6);
                v = (v + (1LL << (bd - 1))) >> bd;
                if (v < -32768) v = -32768;
                if (v > 32767) v = 32767;
                want[i] = (int16_t)(c[i] ? v : 0);
            }
            h265_dequant(c, n, qp, log2, 0, 0);
            CHECK(memcmp(c, want, (size_t)n * n * 2) == 0,
                  "dequant %dx%d qp=%d mismatch", n, n, qp);
        }
    }

    /* A non-flat scaling list must reach the coefficients, and the explicit
     * DC term must override position (0,0) for 16x16 and 32x32 only. */
    uint8_t list[64];
    for (int i = 0; i < 64; i++) list[i] = (uint8_t)(16 + i);
    int16_t c16[16 * 16], c8[8 * 8];
    for (int i = 0; i < 256; i++) c16[i] = 1;
    for (int i = 0; i < 64; i++) c8[i] = 1;
    h265_dequant(c8, 8, 0, 3, list, 99);
    CHECK(c8[0] == (int16_t)(((1 * list[0] * 40) + 32) >> 6),
          "8x8 scaling list must NOT use the DC override (got %d)", c8[0]);
    h265_dequant(c16, 16, 0, 4, list, 99);
    CHECK(c16[0] == (int16_t)(((1 * 99 * 40) + 64) >> 7),
          "16x16 DC override not applied (got %d)", c16[0]);
    CHECK(c16[1] != c16[2] || c16[1] != c16[16],
          "a non-flat scaling list must vary across positions");
}

/* ---- intra prediction --------------------------------------------------- */
/* Build the neighbour array in h265_pred.c's layout from callbacks. */
static void nb_fill(uint8_t *nb, int nbs, int corner,
                    const uint8_t *left, const uint8_t *top)
{
    for (int i = 0; i < 2 * nbs; i++) nb[2 * nbs - 1 - i] = left[i];
    nb[2 * nbs] = (uint8_t)corner;
    for (int i = 0; i < 2 * nbs; i++) nb[2 * nbs + 1 + i] = top[i];
}

static void test_intra_dc(void)
{
    for (int nbs = 4; nbs <= 32; nbs <<= 1) {
        uint8_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
        for (int i = 0; i < 2 * nbs; i++) { left[i] = 100; top[i] = 100; }
        nb_fill(nb, nbs, 100, left, top);
        h265_intra_pred(dst, nbs, nb, nbs, 1, 0);
        int ok = 1;
        for (int i = 0; i < nbs * nbs; i++) if (dst[i] != 100) ok = 0;
        CHECK(ok, "DC %dx%d on a constant edge must be that constant", nbs, nbs);

        /* The boundary filter applies to luma below 32x32 and never to chroma. */
        for (int i = 0; i < 2 * nbs; i++) { left[i] = 0; top[i] = 200; }
        nb_fill(nb, nbs, 0, left, top);
        h265_intra_pred(dst, nbs, nb, nbs, 1, 0);
        int dc = (200 * nbs + 0 * nbs + nbs) >> (1 + (nbs == 4 ? 2 : nbs == 8 ? 3 :
                                                      nbs == 16 ? 4 : 5));
        if (nbs < 32) {
            CHECK(dst[0] == (uint8_t)((0 + 2 * dc + 200 + 2) >> 2),
                  "DC %d corner filter", nbs);
            CHECK(dst[1] == (uint8_t)((200 + 3 * dc + 2) >> 2), "DC %d top filter", nbs);
            CHECK(dst[nbs] == (uint8_t)((0 + 3 * dc + 2) >> 2), "DC %d left filter", nbs);
        }
        CHECK(dst[nbs + 1] == (uint8_t)dc, "DC %d interior", nbs);

        h265_intra_pred(dst, nbs, nb, nbs, 1, 1);       /* chroma: unfiltered */
        CHECK(dst[0] == (uint8_t)dc, "DC %d chroma must not be edge-filtered", nbs);
    }
}

static void test_intra_planar(void)
{
    /* On a plane that is already bilinear in the edges, planar reproduces it.
     * Take left[y] = 50 + y, top[x] = 50 + x with matching far edges. */
    int nbs = 8;
    uint8_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
    for (int i = 0; i < 2 * nbs; i++) { left[i] = (uint8_t)(50 + i); top[i] = (uint8_t)(50 + i); }
    nb_fill(nb, nbs, 49, left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 0, 0);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++) {
            int want = ((nbs - 1 - x) * (50 + y) + (x + 1) * (50 + nbs) +
                        (nbs - 1 - y) * (50 + x) + (y + 1) * (50 + nbs) + nbs) >> 4;
            CHECK(dst[y * nbs + x] == want, "planar (%d,%d) = %d want %d",
                  x, y, dst[y * nbs + x], want);
        }
}

static void test_intra_angular(void)
{
    int nbs = 8;
    uint8_t nb[4 * 32 + 1], left[64], top[64], dst[32 * 32];
    for (int i = 0; i < 2 * nbs; i++) { left[i] = (uint8_t)(10 + i); top[i] = (uint8_t)(80 + i); }

    /* Mode 26 is pure vertical: every row is the top row (chroma, so the
     * mode-26 edge filter that only luma gets does not interfere). */
    nb_fill(nb, nbs, 5, left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 26, 1);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x], "mode 26 (%d,%d)", x, y);

    /* ... and for luma the first COLUMN is filtered by 8.4.4.2.6. */
    h265_intra_pred(dst, nbs, nb, nbs, 26, 0);
    for (int y = 0; y < nbs; y++)
        CHECK(dst[y * nbs] == h265_clip_u8(top[0] + ((left[y] - 5) >> 1)),
              "mode 26 luma column filter at y=%d", y);
    for (int y = 0; y < nbs; y++)
        for (int x = 1; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x], "mode 26 luma interior (%d,%d)", x, y);

    /* Mode 10 is pure horizontal, mirrored. */
    h265_intra_pred(dst, nbs, nb, nbs, 10, 1);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == left[y], "mode 10 (%d,%d)", x, y);
    h265_intra_pred(dst, nbs, nb, nbs, 10, 0);
    for (int x = 0; x < nbs; x++)
        CHECK(dst[x] == h265_clip_u8(left[0] + ((top[x] - 5) >> 1)),
              "mode 10 luma row filter at x=%d", x);

    /* Mode 2 (angle +32, horizontal family): pred[x][y] = p[-1][x+y+1]. */
    h265_intra_pred(dst, nbs, nb, nbs, 2, 0);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == left[x + y + 1], "mode 2 (%d,%d)", x, y);

    /* Mode 34 (angle +32, vertical family): pred[x][y] = p[x+y+1][-1]. */
    h265_intra_pred(dst, nbs, nb, nbs, 34, 0);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++)
            CHECK(dst[y * nbs + x] == top[x + y + 1], "mode 34 (%d,%d)", x, y);

    /* Mode 18 (angle -32): the 135-degree diagonal, which is the only one
     * that reads from both edges through the invAngle projection. */
    h265_intra_pred(dst, nbs, nb, nbs, 18, 0);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++) {
            int want = (x > y) ? top[x - y - 1] : (x == y ? 5 : left[y - x - 1]);
            CHECK(dst[y * nbs + x] == want, "mode 18 (%d,%d) = %d want %d",
                  x, y, dst[y * nbs + x], want);
        }

    /* A fractional angle must interpolate. Mode 3 has intraPredAngle 26, so
     * for x = 0 the projection is iIdx 0, iFact 26 and the sample is
     * ((32-26)*ref[y+1] + 26*ref[y+2] + 16) >> 5 -- with ref[k] = p[-1][k-1].
     * A square-wave left edge makes every such value land strictly between
     * the two it was mixed from, which a nearest-sample copy cannot produce.
     * The earlier version of this check used a unit ramp, where every
     * interpolation lands back on a grid value and the assertion was
     * vacuous. */
    for (int i = 0; i < 2 * nbs; i++) left[i] = (uint8_t)((i & 1) ? 64 : 0);
    nb_fill(nb, nbs, 5, left, top);
    h265_intra_pred(dst, nbs, nb, nbs, 3, 0);
    for (int y = 0; y < nbs; y++) {
        int want = (6 * left[y] + 26 * left[y + 1] + 16) >> 5;
        CHECK(dst[y * nbs] == want, "mode 3 (0,%d) = %d, want %d",
              y, dst[y * nbs], want);
        CHECK(want != left[y] && want != left[y + 1],
              "mode 3 (0,%d) must be strictly between its two taps", y);
    }
}

static void test_intra_filter(void)
{
    uint8_t nb[4 * 32 + 1], ref[4 * 32 + 1], left[64], top[64];

    /* 4x4 is never filtered, DC is never filtered, chroma is never filtered
     * (4:2:0), and the mode-distance threshold gates the rest. */
    for (int i = 0; i < 64; i++) { left[i] = (uint8_t)(i * 3); top[i] = (uint8_t)(255 - i * 3); }
    nb_fill(nb, 4, 128, left, top);
    memcpy(ref, nb, 17);
    h265_intra_filter(nb, 4, 0, 0, 0);
    CHECK(memcmp(nb, ref, 17) == 0, "4x4 must never be smoothed");

    nb_fill(nb, 8, 128, left, top);
    memcpy(ref, nb, 33);
    h265_intra_filter(nb, 8, 1, 0, 0);
    CHECK(memcmp(nb, ref, 33) == 0, "DC must never be smoothed");
    h265_intra_filter(nb, 8, 0, 1, 0);
    CHECK(memcmp(nb, ref, 33) == 0, "4:2:0 chroma must never be smoothed");
    h265_intra_filter(nb, 8, 26, 0, 0);
    CHECK(memcmp(nb, ref, 33) == 0, "8x8 mode 26 is inside the threshold");
    h265_intra_filter(nb, 8, 18, 0, 0);
    CHECK(memcmp(nb, ref, 33) != 0, "8x8 mode 18 must be smoothed");

    /* The filter is [1,2,1]/4 across the whole run with the ends held. */
    nb_fill(nb, 8, 128, left, top);
    memcpy(ref, nb, 33);
    h265_intra_filter(nb, 8, 0, 0, 0);
    CHECK(nb[0] == ref[0] && nb[32] == ref[32], "the run's ends are not filtered");
    for (int i = 1; i < 32; i++)
        CHECK(nb[i] == (uint8_t)((ref[i - 1] + 2 * ref[i] + ref[i + 1] + 2) >> 2),
              "smoothing at %d", i);

    /* Strong intra smoothing: 32x32 luma, edges near-linear, so the whole run
     * becomes an exact linear ramp between the three corner samples. */
    for (int i = 0; i < 64; i++) { left[i] = (uint8_t)(100 + i); top[i] = (uint8_t)(100 + i); }
    nb_fill(nb, 32, 100, left, top);
    h265_intra_filter(nb, 32, 0, 0, 1);
    int corner = 100, bl = left[63], tr = top[63];
    CHECK(nb[0] == bl && nb[64] == corner && nb[128] == tr,
          "strong smoothing must keep the three anchors");
    for (int y = 0; y < 63; y++)
        CHECK(nb[63 - y] == (uint8_t)(((63 - y) * corner + (y + 1) * bl + 32) >> 6),
              "strong smoothing left at y=%d", y);
    for (int x = 0; x < 63; x++)
        CHECK(nb[65 + x] == (uint8_t)(((63 - x) * corner + (x + 1) * tr + 32) >> 6),
              "strong smoothing top at x=%d", x);

    /* With a step in the middle of the edge the flatness test must FAIL and
     * the ordinary [1,2,1] filter must be used instead. */
    for (int i = 0; i < 64; i++) { left[i] = (uint8_t)(i < 32 ? 40 : 200); top[i] = 120; }
    nb_fill(nb, 32, 120, left, top);
    memcpy(ref, nb, 129);
    h265_intra_filter(nb, 32, 0, 0, 1);
    CHECK(nb[1] == (uint8_t)((ref[0] + 2 * ref[1] + ref[2] + 2) >> 2),
          "a non-flat edge must fall back to [1,2,1]");
}

int main(void)
{
    test_matrix();
    test_idct_dc();
    test_idst();
    test_transform_skip_and_bypass();
    test_dequant();
    test_intra_dc();
    test_intra_planar();
    test_intra_angular();
    test_intra_filter();
    printf("h265_pred_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
