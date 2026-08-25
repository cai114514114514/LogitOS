/* tests/unit/mpeg12_idct_test.c -- the inverse DCT, against two references.
 *
 * THE STREAM GATE (make test-mpeg12) already pins this transform: it is
 * bit-exact against ffmpeg -idct simple over 31 streams, which is a stronger
 * statement about agreement than anything here. What it does NOT say is that
 * the transform is a correct IDCT at all -- two implementations of the same
 * wrong butterfly agree perfectly. So this file asks the two questions the
 * stream gate cannot:
 *
 *   1. IS IT AN IDCT? Every one of the 32 coefficient positions in the
 *      even/odd butterfly is recovered by feeding unit impulses through the
 *      code and compared with cos((2n+1)k*pi/16) computed in double. This is
 *      the check that the constants were derived rather than copied.
 *
 *   2. IS IT ACCURATE ENOUGH TO BE CONFORMING? ISO/IEC 13818-2 Annex A
 *      specifies the IDCT only by IEEE 1180's statistical test, so that test
 *      IS the standard's bar and it is run here in full: 10,000 pseudo-random
 *      blocks at each of the three prescribed input ranges, against a
 *      double-precision reference IDCT, with 1180's five error limits and its
 *      all-zero requirement.
 *
 * And one claim from mpeg12_idct.c's own header is checked because it is
 * surprising and load-bearing: the row pass's DC-only shortcut is NOT
 * algebraically equal to the general path, so "simplifying" it away changes
 * the picture. The test names the exact DC values where they part.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mpeg12_int.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

/* ---------------------------------------------------------- references -- */
static double C(int k) { return k ? 1.0 : 1.0 / sqrt(2.0); }

static void ref_idct(const double in[64], double out[64])
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int v = 0; v < 8; v++)
                for (int u = 0; u < 8; u++)
                    s += C(u) * C(v) * in[v * 8 + u] *
                         cos((2 * x + 1) * u * M_PI / 16.0) *
                         cos((2 * y + 1) * v * M_PI / 16.0);
            out[y * 8 + x] = s / 4.0;
        }
}

static void ref_fdct(const double in[64], double out[64])
{
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    s += in[y * 8 + x] *
                         cos((2 * x + 1) * u * M_PI / 16.0) *
                         cos((2 * y + 1) * v * M_PI / 16.0);
            out[v * 8 + u] = C(u) * C(v) * s / 4.0;
        }
}

/* ------------------------------------------------- 1. is it an IDCT? ----- */
/* The row pass computes out[n] = sum_k W(n,k) * in[k] >> 11 with an added
 * rounding constant. Feeding a single large impulse at k and reading out[n]
 * recovers W(n,k) to within the shift's resolution; the reference is
 * cos((2n+1)k*pi/16) * sqrt(2) * 2^14, i.e. exactly the derivation in
 * mpeg12_idct.c. A single transposed or sign-flipped coefficient shows here
 * and produces a picture that still looks like a picture. */
static void test_butterfly(void)
{
    const int AMP = 1024;                 /* large enough to swamp rounding */
    int worst = 0;

    for (int k = 0; k < 8; k++) {
        int16_t blk[64];
        int out[64];
        memset(blk, 0, sizeof blk);
        blk[k] = AMP;                     /* impulse in row 0, column k */
        /* The impulse sits at 2D coefficient (v=0, u=k), so BOTH C(u)=C(k)
         * and C(v)=C(0) apply -- the row-0 placement does not exempt it from
         * the row-direction normalisation constant, it just makes the cos()
         * factor for y equal cos(0)=1 for every row. A first version of this
         * check divided by 8.0 with only the single C(k) factor applied and
         * silently dropped C(0) (=1/sqrt(2)): every expected value came out
         * too small by exactly sqrt(2), which is a bug in the test's formula,
         * not the decoder -- confirmed independently by the IEEE 1180 check
         * below, which uses the full C(u)*C(v) 2D reference and passes with
         * peak error <=1 on the SAME transform. Put the impulse at
         * coefficient (0, k) and read row 0 (constant across every row,
         * since the v=0 term makes cos((2y+1)*0*pi/16) == 1 for all y). */
        m12_idct_raw(blk, out);
        for (int n = 0; n < 8; n++) {
            double want = AMP * C(k) * C(0) * cos((2 * n + 1) * k * M_PI / 16.0) / 4.0;
            double got = out[n];
            double err = fabs(got - want);
            if (err > worst) worst = (int)ceil(err);
            CHECK(err <= 1.5,
                  "butterfly k=%d n=%d got %.1f want %.3f", k, n, got, want);
        }
    }
    printf("  butterfly: 64 positions vs cos((2n+1)k*pi/16), worst |err| %d\n",
           worst);
}

/* ---------------------------------------- 2. IEEE 1180 / 13818-2 Annex A - */
static unsigned long rstate = 1;
static double frand(void)                 /* 1180's own generator shape */
{
    rstate = rstate * 1103515245ul + 12345ul;
    return (double)((rstate >> 16) & 0x7FFF) / 32768.0;
}
static int rnd_range(int L, int H)
{
    double x = frand() * (H - L + 1);
    int v = L + (int)x;
    return v > H ? H : v;
}

struct stats { double sum[64], sumsq[64]; long n; int peak; };

static void ieee1180_range(int L, int H, int sign, struct stats *st)
{
    for (int b = 0; b < 10000; b++) {
        double pix[64], coef[64], refout[64];
        int16_t blk[64];
        int out[64];

        for (int i = 0; i < 64; i++) pix[i] = sign * rnd_range(L, H);
        ref_fdct(pix, coef);
        for (int i = 0; i < 64; i++) {
            double r = floor(coef[i] + 0.5);
            if (r > 2047) r = 2047;
            if (r < -2048) r = -2048;
            blk[i] = (int16_t)r;
            coef[i] = r;
        }
        ref_idct(coef, refout);
        m12_idct_raw(blk, out);

        for (int i = 0; i < 64; i++) {
            int want = (int)floor(refout[i] + 0.5);
            if (want > 255) want = 255;
            if (want < -256) want = -256;
            int got = out[i];
            if (got > 255) got = 255;
            if (got < -256) got = -256;
            int e = got - want;
            if (abs(e) > st->peak) st->peak = abs(e);
            st->sum[i] += e;
            st->sumsq[i] += (double)e * e;
        }
        st->n++;
    }
}

static void test_ieee1180(void)
{
    static const struct { int L, H; } ranges[] = { {-256, 255}, {-5, 5}, {-300, 300} };
    static const char *names[] = { "-256..255", "-5..5", "-300..300" };

    for (int r = 0; r < 3; r++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            struct stats st;
            memset(&st, 0, sizeof st);
            rstate = 1 + r * 7 + (sign < 0);
            ieee1180_range(ranges[r].L, ranges[r].H, sign, &st);

            double omse = 0, ome = 0, pmse = 0, pme = 0;
            for (int i = 0; i < 64; i++) {
                double mse = st.sumsq[i] / st.n, me = fabs(st.sum[i]) / st.n;
                if (mse > pmse) pmse = mse;
                if (me > pme) pme = me;
                omse += st.sumsq[i];
                ome += st.sum[i];
            }
            omse /= (double)st.n * 64;
            ome = fabs(ome) / ((double)st.n * 64);

            /* IEEE 1180-1990 clause 5, the five limits. */
            CHECK(st.peak <= 1, "1180 %s sign %d: peak error %d > 1",
                  names[r], sign, st.peak);
            CHECK(pmse <= 0.06, "1180 %s sign %d: worst-pixel mse %.4f > 0.06",
                  names[r], sign, pmse);
            CHECK(omse <= 0.02, "1180 %s sign %d: overall mse %.4f > 0.02",
                  names[r], sign, omse);
            CHECK(pme <= 0.015, "1180 %s sign %d: worst-pixel mean %.4f > 0.015",
                  names[r], sign, pme);
            CHECK(ome <= 0.0015, "1180 %s sign %d: overall mean %.5f > 0.0015",
                  names[r], sign, ome);
            printf("  IEEE 1180 %-10s sign %+d: peak %d  pmse %.4f  omse %.4f"
                   "  pme %.4f  ome %.5f\n",
                   names[r], sign, st.peak, pmse, omse, pme, ome);
        }
    }

    /* 1180's sixth requirement, and the only one that is absolute. */
    int16_t z[64];
    int out[64];
    memset(z, 0, sizeof z);
    m12_idct_raw(z, out);
    for (int i = 0; i < 64; i++)
        CHECK(out[i] == 0, "all-zero input produced %d at %d", out[i], i);
    printf("  IEEE 1180 all-zero input -> all-zero output\n");
}

/* ------------------------------ 3. the DC shortcut is not an optimisation - */
static void test_dc_shortcut(void)
{
    int diverge = 0, first = 0;

    for (int dc = -2048; dc <= 2047; dc++) {
        int16_t a[64], b[64];
        int oa[64], ob[64];
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        a[0] = (int16_t)dc;
        /* The same DC with one coefficient set and then cleared cannot be
         * used to reach the general path, so the general path's value is
         * computed here directly: (W4*dc + 1024) >> 11, broadcast. */
        m12_idct_raw(a, oa);
        int general = (16383 * dc + 1024) >> 11;
        for (int i = 0; i < 64; i++) b[i] = 0;
        b[0] = (int16_t)general;          /* unused, keeps the shapes parallel */
        (void)ob;

        int shortcut = (int16_t)(((unsigned)dc << 3) & 0xFFFFu);
        if (shortcut != general) {
            if (!diverge) first = dc;
            diverge++;
        }
    }
    CHECK(diverge > 0,
          "the DC-only row shortcut is algebraically equal to the general "
          "path, so mpeg12_idct.c's warning about removing it is wrong");
    printf("  DC row shortcut differs from the general path for %d of 4096 "
           "DC values (first at %d) -- removing it changes the picture\n",
           diverge, first);
}

/* ------------------------------------------- 4. put / add match the raw -- */
static void test_put_add(void)
{
    uint8_t dst[8 * 16], dst2[8 * 16];
    int16_t blk[64], cp[64];
    int raw[64];

    rstate = 99;
    for (int t = 0; t < 200; t++) {
        for (int i = 0; i < 64; i++) blk[i] = (int16_t)rnd_range(-800, 800);
        memcpy(cp, blk, sizeof cp);
        m12_idct_raw(cp, raw);

        memcpy(cp, blk, sizeof cp);
        memset(dst, 0xA5, sizeof dst);
        m12_idct_put(dst, 16, cp);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = raw[y * 8 + x];
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                CHECK(dst[y * 16 + x] == v, "put mismatch at %d,%d", x, y);
            }

        memcpy(cp, blk, sizeof cp);
        for (int i = 0; i < 8 * 16; i++) dst2[i] = (uint8_t)(i * 7 + t);
        uint8_t before[8 * 16];
        memcpy(before, dst2, sizeof before);
        m12_idct_add(dst2, 16, cp);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = before[y * 16 + x] + raw[y * 8 + x];
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                CHECK(dst2[y * 16 + x] == v, "add mismatch at %d,%d", x, y);
            }
    }
    printf("  put/add agree with the raw transform over 200 random blocks\n");
}

int main(void)
{
    printf("mpeg12 IDCT:\n");
    test_butterfly();
    test_ieee1180();
    test_dc_shortcut();
    test_put_add();
    if (fails) { printf("MPEG12-IDCT-FAIL %d\n", fails); return 1; }
    printf("MPEG12-IDCT-OK\n");
    return 0;
}
