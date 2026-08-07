/* tests/unit/h265_deblock_test.c -- the deblocking kernels, the boundary
 * strength derivation, and the SAO classifiers, as modules (spec 8.7).
 *
 * The kernels take a base pointer plus two strides -- one across the edge and
 * one along it -- so the same code filters vertical and horizontal edges.
 * That is worth a test on its own: transposing the buffer and swapping the
 * strides must give the transpose of the result, and an implementation that
 * had the two mixed up would still produce a plausible picture.
 *
 * Boundary strength gets the attention it does because of the note in
 * 8.7.2.4: the question is whether the two sides reference the same
 * PICTURES, not whether they use the same list indices. Weighted prediction
 * puts one picture at several indices deliberately, and bi-prediction from a
 * single picture twice makes both list pairings legal. Those two cases are
 * tested explicitly, because a decoder that compares ref_idx passes every
 * ordinary stream and then over-filters exactly the frames that use them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

void h265_deblock_luma4(uint8_t *base, int dstep, int lstep,
                        int beta, int tc, int no_p, int no_q);
void h265_deblock_chroma_n(uint8_t *base, int dstep, int lstep, int n,
                           int tc, int no_p, int no_q);
int  h265_bs(const bi_t *p, const bi_t *q, int tu_edge);
int  h265_sao_edge_idx(int cur, int a, int b);
int  h265_sao_band_idx(int sample, int band_position);
int  h265_chroma_qp(int qpi);

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* An 8-wide by 4-tall test edge: base points at q0 of line 0. */
#define STRIDE 16
static void mk(uint8_t *buf, const int *pv, const int *qv)
{
    memset(buf, 0, STRIDE * 8);
    for (int l = 0; l < 4; l++)
        for (int k = 0; k < 4; k++) {
            buf[l * STRIDE + 4 - 1 - k] = (uint8_t)pv[k];   /* p3..p0 at 0..3 */
            buf[l * STRIDE + 4 + k] = (uint8_t)qv[k];
        }
}
#define BASE(buf) ((buf) + 4)

static void test_flat(void)
{
    uint8_t buf[STRIDE * 8], ref[STRIDE * 8];
    int pv[4] = { 90, 90, 90, 90 }, qv[4] = { 90, 90, 90, 90 };
    mk(buf, pv, qv);
    memcpy(ref, buf, sizeof buf);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, 64, 24, 0, 0);
    CHECK(memcmp(buf, ref, sizeof buf) == 0,
          "a constant edge must survive the strong filter untouched");

    h265_deblock_chroma_n(BASE(buf), 1, STRIDE, 4, 24, 0, 0);
    CHECK(memcmp(buf, ref, sizeof buf) == 0,
          "a constant edge must survive the chroma filter untouched");
}

static void test_no_filter_when_d_ge_beta(void)
{
    uint8_t buf[STRIDE * 8], ref[STRIDE * 8];
    /* A strong ripple on both sides makes d large; with beta small the whole
     * segment is left alone. */
    int pv[4] = { 10, 200, 10, 200 }, qv[4] = { 10, 200, 10, 200 };
    mk(buf, pv, qv);
    memcpy(ref, buf, sizeof buf);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, 6, 24, 0, 0);
    CHECK(memcmp(buf, ref, sizeof buf) == 0, "d >= beta must skip the segment");
}

static void test_strong(void)
{
    /* A clean step, flat on both sides: dpq is 0, |p3-p0| and |q0-q3| are 0,
     * and |p0-q0| is small enough, so the strong filter runs. Every output is
     * hand-computed from 8.7.2.5.7. */
    uint8_t buf[STRIDE * 8];
    int pv[4] = { 100, 100, 100, 100 };     /* p0 p1 p2 p3 */
    int qv[4] = { 108, 108, 108, 108 };
    int beta = 64, tc = 24;
    mk(buf, pv, qv);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, beta, tc, 0, 0);

    int p0 = 100, p1 = 100, p2 = 100, p3 = 100;
    int q0 = 108, q1 = 108, q2 = 108, q3 = 108;
    int wp0 = h265_clip3(p0 - 2 * tc, p0 + 2 * tc, (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
    int wp1 = h265_clip3(p1 - 2 * tc, p1 + 2 * tc, (p2 + p1 + p0 + q0 + 2) >> 2);
    int wp2 = h265_clip3(p2 - 2 * tc, p2 + 2 * tc, (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
    int wq0 = h265_clip3(q0 - 2 * tc, q0 + 2 * tc, (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
    int wq1 = h265_clip3(q1 - 2 * tc, q1 + 2 * tc, (p0 + q0 + q1 + q2 + 2) >> 2);
    int wq2 = h265_clip3(q2 - 2 * tc, q2 + 2 * tc, (p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3);
    for (int l = 0; l < 4; l++) {
        const uint8_t *r = buf + l * STRIDE;
        CHECK(r[3] == wp0 && r[2] == wp1 && r[1] == wp2,
              "strong p side line %d: %d %d %d, want %d %d %d",
              l, r[3], r[2], r[1], wp0, wp1, wp2);
        CHECK(r[4] == wq0 && r[5] == wq1 && r[6] == wq2,
              "strong q side line %d: %d %d %d, want %d %d %d",
              l, r[4], r[5], r[6], wq0, wq1, wq2);
        CHECK(r[0] == 100 && r[7] == 108, "strong must not touch p3/q3");
    }

    /* The same step with tc = 1 must be clamped to +-2 around the original. */
    mk(buf, pv, qv);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, beta, 1, 0, 0);
    for (int l = 0; l < 4; l++) {
        const uint8_t *r = buf + l * STRIDE;
        for (int k = 1; k <= 3; k++)
            CHECK(abs(r[4 - k] - 100) <= 2, "tc clamp on p%d", k - 1);
        for (int k = 0; k <= 2; k++)
            CHECK(abs(r[4 + k] - 108) <= 2, "tc clamp on q%d", k);
    }
}

static void test_weak(void)
{
    /* A step too large for the strong decision (|p0-q0| >= (5*tc+1)>>1) but
     * with d < beta, so the weak filter runs. */
    uint8_t buf[STRIDE * 8];
    int pv[4] = { 100, 100, 100, 100 };
    int qv[4] = { 130, 130, 130, 130 };
    int beta = 64, tc = 4;
    mk(buf, pv, qv);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, beta, tc, 0, 0);

    int p0 = 100, p1 = 100, p2 = 100, q0 = 130, q1 = 130, q2 = 130;
    int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
    CHECK(abs(delta) < tc * 10, "the weak filter's own gate should let this through");
    delta = h265_clip3(-tc, tc, delta);
    int dp = h265_clip3(-(tc >> 1), tc >> 1, (((p2 + p0 + 1) >> 1) - p1 + delta) >> 1);
    int dq = h265_clip3(-(tc >> 1), tc >> 1, (((q2 + q0 + 1) >> 1) - q1 - delta) >> 1);
    for (int l = 0; l < 4; l++) {
        const uint8_t *r = buf + l * STRIDE;
        CHECK(r[3] == h265_clip_u8(p0 + delta), "weak p0 line %d = %d", l, r[3]);
        CHECK(r[4] == h265_clip_u8(q0 - delta), "weak q0 line %d = %d", l, r[4]);
        CHECK(r[2] == h265_clip_u8(p1 + dp), "weak p1 line %d", l);
        CHECK(r[5] == h265_clip_u8(q1 + dq), "weak q1 line %d", l);
        CHECK(r[1] == 100 && r[6] == 130, "weak must not touch p2/q2");
    }

    /* |delta| >= tc*10 aborts the whole line. tc = 1 gives a threshold of 10
     * while delta is about 17, so nothing may change. */
    uint8_t ref[STRIDE * 8];
    mk(buf, pv, qv);
    memcpy(ref, buf, sizeof buf);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, beta, 1, 0, 0);
    CHECK(memcmp(buf, ref, sizeof buf) == 0,
          "|delta| >= 10*tc must leave the line untouched");
}

static void test_sides_suppressed(void)
{
    uint8_t buf[STRIDE * 8], ref[STRIDE * 8];
    int pv[4] = { 100, 100, 100, 100 }, qv[4] = { 108, 108, 108, 108 };
    mk(buf, pv, qv);
    memcpy(ref, buf, sizeof buf);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, 64, 24, 1, 0);
    for (int l = 0; l < 4; l++) {
        CHECK(memcmp(buf + l * STRIDE, ref + l * STRIDE, 4) == 0,
              "no_p must leave the whole p side alone (line %d)", l);
        CHECK(memcmp(buf + l * STRIDE + 4, ref + l * STRIDE + 4, 4) != 0,
              "... and must still filter the q side (line %d)", l);
    }
    mk(buf, pv, qv);
    h265_deblock_luma4(BASE(buf), 1, STRIDE, 64, 24, 0, 1);
    for (int l = 0; l < 4; l++)
        CHECK(memcmp(buf + l * STRIDE + 4, ref + l * STRIDE + 4, 4) == 0,
              "no_q must leave the whole q side alone (line %d)", l);
}

static void test_direction_symmetry(void)
{
    /* The same edge filtered as a vertical one and as a horizontal one on the
     * transposed buffer must give transposed results. */
    uint8_t a[STRIDE * 8], b[STRIDE * 8];
    unsigned s = 3;
    for (int i = 0; i < STRIDE * 8; i++) {
        s = s * 1103515245u + 12345u;
        a[i] = (uint8_t)((s >> 16) & 63);
    }
    for (int l = 0; l < 4; l++)
        for (int k = 0; k < 8; k++) a[l * STRIDE + k] = (uint8_t)(100 + (k >= 4 ? 9 : 0) + (l & 1));
    memset(b, 0, sizeof b);
    for (int l = 0; l < 4; l++)
        for (int k = 0; k < 8; k++) b[k * STRIDE + l] = a[l * STRIDE + k];

    h265_deblock_luma4(a + 4, 1, STRIDE, 64, 10, 0, 0);
    h265_deblock_luma4(b + 4 * STRIDE, STRIDE, 1, 64, 10, 0, 0);
    int ok = 1;
    for (int l = 0; l < 4; l++)
        for (int k = 0; k < 8; k++)
            if (b[k * STRIDE + l] != a[l * STRIDE + k]) ok = 0;
    CHECK(ok, "the two stride arguments must be a true transpose of each other");
}

static void test_chroma(void)
{
    uint8_t buf[STRIDE * 8];
    int pv[4] = { 80, 90, 0, 0 }, qv[4] = { 120, 110, 0, 0 };
    int tc = 6;
    mk(buf, pv, qv);
    h265_deblock_chroma_n(BASE(buf), 1, STRIDE, 4, tc, 0, 0);
    int p0 = 80, p1 = 90, q0 = 120, q1 = 110;
    int delta = h265_clip3(-tc, tc, ((((q0 - p0) << 2) + p1 - q1 + 4) >> 3));
    for (int l = 0; l < 4; l++) {
        CHECK(buf[l * STRIDE + 3] == h265_clip_u8(p0 + delta), "chroma p0 line %d", l);
        CHECK(buf[l * STRIDE + 4] == h265_clip_u8(q0 - delta), "chroma q0 line %d", l);
        CHECK(buf[l * STRIDE + 2] == 90 && buf[l * STRIDE + 5] == 110,
              "chroma must touch only p0 and q0");
    }

    /* Table 8-10, the 4:2:0 chroma QP mapping. */
    for (int q = 0; q < 30; q++) CHECK(h265_chroma_qp(q) == q, "chroma qp identity at %d", q);
    static const int want[14] = { 29,30,31,32,33,33,34,34,35,35,36,36,37,37 };
    for (int q = 30; q <= 43; q++)
        CHECK(h265_chroma_qp(q) == want[q - 30], "chroma qp table at %d", q);
    for (int q = 44; q <= 57; q++)
        CHECK(h265_chroma_qp(q) == q - 6, "chroma qp minus 6 at %d", q);
}

/* ---- boundary strength -------------------------------------------------- */
static bi_t inter1(int poc, int mvx, int mvy)
{
    bi_t b;
    memset(&b, 0, sizeof b);
    b.pred_mode = MODE_INTER;
    b.pred_flag = 1;
    b.refpoc[0] = poc;
    b.mv[0][0] = (int16_t)mvx;
    b.mv[0][1] = (int16_t)mvy;
    return b;
}
static bi_t inter2(int poc0, int mv0x, int mv0y, int poc1, int mv1x, int mv1y)
{
    bi_t b;
    memset(&b, 0, sizeof b);
    b.pred_mode = MODE_INTER;
    b.pred_flag = 3;
    b.refpoc[0] = poc0; b.mv[0][0] = (int16_t)mv0x; b.mv[0][1] = (int16_t)mv0y;
    b.refpoc[1] = poc1; b.mv[1][0] = (int16_t)mv1x; b.mv[1][1] = (int16_t)mv1y;
    return b;
}

static void test_bs(void)
{
    bi_t intra;
    memset(&intra, 0, sizeof intra);
    intra.pred_mode = MODE_INTRA;
    bi_t a = inter1(4, 0, 0);
    CHECK(h265_bs(&intra, &a, 0) == 2, "an intra side always gives bS 2");
    CHECK(h265_bs(&a, &intra, 0) == 2, "... on either side");

    bi_t b = inter1(4, 0, 0);
    CHECK(h265_bs(&a, &b, 0) == 0, "identical prediction, no coefficients: bS 0");
    b.cbf_luma = 1;
    CHECK(h265_bs(&a, &b, 1) == 1, "a coded transform edge gives bS 1");
    CHECK(h265_bs(&a, &b, 0) == 0, "... but only when it IS a transform edge");
    b.cbf_luma = 0;

    CHECK(h265_bs(&a, &b, 0) == 0, "mv difference of 0");
    b = inter1(4, 3, 0);
    CHECK(h265_bs(&a, &b, 0) == 0, "mv difference of 3 quarter-pels is under the bar");
    b = inter1(4, 4, 0);
    CHECK(h265_bs(&a, &b, 0) == 1, "mv difference of 4 quarter-pels reaches it");
    b = inter1(4, 0, -4);
    CHECK(h265_bs(&a, &b, 0) == 1, "... in either component");
    b = inter1(6, 0, 0);
    CHECK(h265_bs(&a, &b, 0) == 1, "a different reference picture gives bS 1");

    /* Different NUMBER of vectors. */
    bi_t c = inter2(4, 0, 0, 6, 0, 0);
    CHECK(h265_bs(&a, &c, 0) == 1, "one vector versus two gives bS 1");

    /* THE list-position trap: the same picture reached through different list
     * positions is the same picture. A decoder comparing ref_idx would call
     * this a difference and over-filter. */
    bi_t d0 = inter1(4, 1, 1), d1 = inter1(4, 1, 1);
    d0.ref_idx[0] = 0; d1.ref_idx[0] = 3;      /* weighted prediction does this */
    CHECK(h265_bs(&d0, &d1, 0) == 0,
          "the same picture at different list indices must not raise bS");

    /* Two vectors, two different pictures, matched by picture rather than by
     * list -- p uses (poc4 in L0, poc6 in L1) and q uses (poc6 in L0, poc4 in
     * L1) with the SAME vectors per picture. */
    bi_t e0 = inter2(4, 8, 0, 6, -8, 0);
    bi_t e1 = inter2(6, -8, 0, 4, 8, 0);
    CHECK(h265_bs(&e0, &e1, 0) == 0,
          "two references matched by picture, not by list, must give bS 0");
    bi_t e2 = inter2(6, -8, 0, 4, 12, 0);
    CHECK(h265_bs(&e0, &e2, 0) == 1, "... and a real difference still shows");

    /* Bi-prediction from ONE picture twice: either pairing may be used, so
     * bS is 0 if either matches. */
    bi_t f0 = inter2(4, 8, 0, 4, -8, 0);
    bi_t f1 = inter2(4, -8, 0, 4, 8, 0);
    CHECK(h265_bs(&f0, &f1, 0) == 0,
          "one picture used twice: the crossed pairing counts too");
    bi_t f2 = inter2(4, 40, 0, 4, -40, 0);
    CHECK(h265_bs(&f0, &f2, 0) == 1, "... unless neither pairing is close");
}

/* ---- SAO ---------------------------------------------------------------- */
static void test_sao_edge(void)
{
    /* 8.7.3.2: edgeIdx = 2 + Sign(c-a) + Sign(c-b), then 0->1, 1->2, 2->0.
     * A local minimum gets category 1, a valley edge 2, flat 0, a step 3, a
     * local maximum 4. All nine sign pairs, by hand. */
    CHECK(h265_sao_edge_idx(10, 20, 20) == 1, "local minimum -> 1");
    CHECK(h265_sao_edge_idx(10, 20, 10) == 2, "one lower neighbour -> 2");
    CHECK(h265_sao_edge_idx(10, 10, 20) == 2, "the other way round -> 2");
    CHECK(h265_sao_edge_idx(10, 10, 10) == 0, "flat -> 0, i.e. unchanged");
    CHECK(h265_sao_edge_idx(10, 5, 10) == 3, "one higher neighbour -> 3");
    CHECK(h265_sao_edge_idx(10, 10, 5) == 3, "the other way round -> 3");
    CHECK(h265_sao_edge_idx(10, 5, 5) == 4, "local maximum -> 4");
    CHECK(h265_sao_edge_idx(10, 5, 20) == 0, "a monotone slope -> 0");
    CHECK(h265_sao_edge_idx(10, 20, 5) == 0, "... in either direction");
}

static void test_sao_band(void)
{
    /* Four consecutive bands of 8 starting at sao_band_position, wrapping at
     * 32. Anything outside gets index 0 and is left alone. */
    for (int pos = 0; pos < 32; pos++)
        for (int v = 0; v < 256; v++) {
            int band = v >> 3;
            int k = band - pos;
            if (k < 0) k += 32;
            int want = (k < 4) ? k + 1 : 0;
            int got = h265_sao_band_idx(v, pos);
            if (got != want) {
                CHECK(0, "band idx pos=%d v=%d: got %d want %d", pos, v, got, want);
                return;
            }
        }
    checks++;
    /* The wrap itself, spelled out: position 30 covers bands 30, 31, 0, 1. */
    CHECK(h265_sao_band_idx(30 * 8, 30) == 1, "band wrap start");
    CHECK(h265_sao_band_idx(31 * 8, 30) == 2, "band wrap +1");
    CHECK(h265_sao_band_idx(0, 30) == 3, "band wrap around to 0");
    CHECK(h265_sao_band_idx(1 * 8, 30) == 4, "band wrap around to 1");
    CHECK(h265_sao_band_idx(2 * 8, 30) == 0, "just past the wrapped run");
}

int main(void)
{
    test_flat();
    test_no_filter_when_d_ge_beta();
    test_strong();
    test_weak();
    test_sides_suppressed();
    test_direction_symmetry();
    test_chroma();
    test_bs();
    test_sao_edge();
    test_sao_band();
    printf("h265_deblock_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
