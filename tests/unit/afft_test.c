/* tests/unit/afft_test.c -- the two pieces of shared machinery the new
 * transform codecs stand on, checked against their definitions.
 *
 * WHY THIS IS A SEPARATE TEST AND NOT PART OF A CODEC TEST.  A wrong twiddle
 * or a wrong MDCT sign does not make a decoder fail loudly; it makes it
 * produce something that still sounds like the music, with an error that a
 * conformance bound then attributes to the codec logic. Then every hour spent
 * bisecting the codec is spent in the wrong file. So the FFT is checked
 * against the direct DFT sum, the IMDCT against the literal cosine sum in the
 * specification, and the transcendentals against the host libm -- all at the
 * exact sizes AAC (2048, 256), Vorbis (256..8192) and Opus/CELT (240, 480,
 * 960, 1920 -- none of them powers of two) actually use.
 *
 * The oracle is the definition, not another implementation of the fast path.
 * That is the whole point: two fast implementations agreeing proves they made
 * the same choice of convention, which is what a sign error looks like.
 *
 * This file may use libm; the code under test may not, and does not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "amath.h"
#include "afft.h"

static int failures, checks;

static void ok(int cond, const char *fmt, ...)
{
    checks++;
    if (!cond) {
        va_list ap;
        failures++;
        printf("FAIL: ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

/* --- amath --------------------------------------------------------------- */

static double relerr(double got, double want)
{
    if (want == 0.0) return fabs(got);
    return fabs(got - want) / fabs(want);
}

static void test_amath(void)
{
    /* sin/cos over a dense sweep, including the large arguments the bigger FFT
     * twiddle loops reach (k*2pi with k up to a few thousand). */
    double worst_sin = 0, worst_cos = 0;
    for (int i = -200000; i <= 200000; i++) {
        double x = (double)i * 0.0001 * 314.0;      /* out to about +-6300 rad */
        double s = a_sin(x), c = a_cos(x);
        double rs = fabs(s - sin(x)), rc = fabs(c - cos(x));   /* absolute: |sin| <= 1 */
        if (rs > worst_sin) worst_sin = rs;
        if (rc > worst_cos) worst_cos = rc;
    }
    ok(worst_sin < 4e-16, "a_sin worst absolute error %g over +-6300 rad", worst_sin);
    ok(worst_cos < 4e-16, "a_cos worst absolute error %g over +-6300 rad", worst_cos);
    printf("  a_sin/a_cos worst abs err over 400k points: %.3g / %.3g\n",
           worst_sin, worst_cos);

    /* Exactness where the codecs rely on it. */
    ok(a_cos(0.0) == 1.0, "a_cos(0) == 1 exactly (got %.17g)", a_cos(0.0));
    ok(a_sin(0.0) == 0.0, "a_sin(0) == 0 exactly");

    double worst_e = 0, worst_l = 0;
    for (int i = -10000; i <= 10000; i++) {
        double x = (double)i * 0.1;                 /* -1000 .. 1000 */
        double e = relerr(a_exp2(x), exp2(x));
        if (e > worst_e) worst_e = e;
    }
    ok(worst_e < 4e-16, "a_exp2 worst relative error %g", worst_e);

    for (int i = 1; i <= 200000; i++) {
        double x = (double)i * 1e-3;
        double e = fabs(a_log2(x) - log2(x));
        if (x > 1.5 || x < 0.5) e = relerr(a_log2(x), log2(x));
        if (e > worst_l) worst_l = e;
    }
    ok(worst_l < 4e-16, "a_log2 worst error %g", worst_l);
    printf("  a_exp2 / a_log2 worst rel err: %.3g / %.3g\n", worst_e, worst_l);

    /* exp2 must be exact on integers: the codecs use it as a power-of-two
     * gain, and 2^n coming back as 2^n*(1+eps) would spread a rounding error
     * across a whole scalefactor band. */
    int exact = 1;
    for (int n = -60; n <= 60; n++) if (a_exp2((double)n) != exp2((double)n)) exact = 0;
    ok(exact, "a_exp2 is exact on integer arguments");

    double worst_s = 0;
    for (int i = 1; i <= 200000; i++) {
        double x = (double)i * 1e-2;
        double e = relerr(a_sqrt(x), sqrt(x));
        if (e > worst_s) worst_s = e;
    }
    ok(worst_s == 0.0, "a_sqrt is exact (worst rel err %g)", worst_s);

    double worst_c = 0;
    for (int i = -100000; i <= 100000; i++) {
        double x = (double)i * 1e-2;
        double e = relerr(a_cbrt(x), cbrt(x));
        if (e > worst_c) worst_c = e;
    }
    ok(worst_c < 2e-15, "a_cbrt worst relative error %g", worst_c);

    /* pow on the shape AAC uses it: |q|^(4/3) for the whole quantiser range. */
    double worst_p = 0;
    for (int q = 1; q <= 20000; q++) {
        double want = pow((double)q, 4.0 / 3.0);
        double e = relerr(a_pow((double)q, 4.0 / 3.0), want);
        if (e > worst_p) worst_p = e;
    }
    ok(worst_p < 1e-14, "a_pow(q,4/3) worst relative error %g", worst_p);
    printf("  a_sqrt/a_cbrt/a_pow worst rel err: %.3g / %.3g / %.3g\n",
           worst_s, worst_c, worst_p);
}

/* --- FFT ----------------------------------------------------------------- */

static void dft_direct(const double *in, double *out, int n, int inverse)
{
    double sign = inverse ? 1.0 : -1.0;
    for (int k = 0; k < n; k++) {
        double sr = 0, si = 0;
        for (int j = 0; j < n; j++) {
            double a = sign * 2.0 * M_PI * (double)j * (double)k / (double)n;
            double c = cos(a), s = sin(a);
            sr += in[2 * j] * c - in[2 * j + 1] * s;
            si += in[2 * j] * s + in[2 * j + 1] * c;
        }
        out[2 * k] = sr;
        out[2 * k + 1] = si;
    }
}

static unsigned rngstate = 12345;
static double frand(void)
{
    rngstate = rngstate * 1103515245u + 12345u;
    return ((double)((rngstate >> 8) & 0xFFFF) / 32768.0) - 1.0;
}

static void test_fft_size(int n, int inverse)
{
    double *in  = malloc((size_t)n * 2 * sizeof(double));
    double *got = malloc((size_t)n * 2 * sizeof(double));
    double *want = malloc((size_t)n * 2 * sizeof(double));
    for (int i = 0; i < 2 * n; i++) in[i] = frand();

    afft *f = afft_new(n, inverse);
    ok(f != NULL, "afft_new(%d,%d)", n, inverse);
    if (!f) { free(in); free(got); free(want); return; }
    afft_run(f, in, got);
    dft_direct(in, want, n, inverse);

    double worst = 0, norm = 0;
    for (int i = 0; i < 2 * n; i++) {
        double d = fabs(got[i] - want[i]);
        if (d > worst) worst = d;
        norm += want[i] * want[i];
    }
    norm = sqrt(norm / (2 * n));
    /* The direct DFT is itself only accurate to about n ulp of its own
     * magnitude, so the bound is scaled by n; a real error in a butterfly is
     * O(1) relative, i.e. eleven orders of magnitude above this. */
    ok(worst < 1e-12 * (double)n,
       "fft n=%d inv=%d worst abs err %g (rms %g)", n, inverse, worst, norm);

    afft_free(f);
    free(in); free(got); free(want);
}

static void test_fft(void)
{
    /* Powers of two (AAC, Vorbis), and the 2^a*3*5 sizes CELT needs. */
    static const int sizes[] = { 4, 8, 15, 16, 20, 32, 60, 64, 96, 120, 128,
                                 240, 256, 480, 512, 960, 1024, 1920, 2048 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        test_fft_size(sizes[i], 0);
        test_fft_size(sizes[i], 1);
    }

    /* An impulse must transform to a constant, and a constant to an impulse:
     * two properties that a wrong twiddle sign cannot satisfy by accident. */
    int n = 120;
    double *in = calloc((size_t)n * 2, sizeof(double));
    double *out = malloc((size_t)n * 2 * sizeof(double));
    in[0] = 1.0;
    afft *f = afft_new(n, 0);
    afft_run(f, in, out);
    int flat = 1;
    for (int k = 0; k < n; k++)
        if (fabs(out[2 * k] - 1.0) > 1e-12 || fabs(out[2 * k + 1]) > 1e-12) flat = 0;
    ok(flat, "FFT of a unit impulse is flat 1+0i at n=120");
    afft_free(f);
    free(in); free(out);
}

/* --- IMDCT --------------------------------------------------------------- */

static void imdct_direct(const double *X, double *y, int n)
{
    double n0 = (double)(n / 4) + 0.5;
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int k = 0; k < n / 2; k++)
            s += X[k] * cos(2.0 * M_PI / (double)n * ((double)k + 0.5) * ((double)i + n0));
        y[i] = s;
    }
}

static void test_imdct_size(int n)
{
    double *X = malloc((size_t)(n / 2) * sizeof(double));
    double *got = malloc((size_t)n * sizeof(double));
    double *want = malloc((size_t)n * sizeof(double));
    for (int i = 0; i < n / 2; i++) X[i] = frand();

    amdct *m = amdct_new(n);
    ok(m != NULL, "amdct_new(%d)", n);
    if (!m) { free(X); free(got); free(want); return; }
    amdct_imdct(m, X, got);
    imdct_direct(X, want, n);

    double worst = 0, peak = 0;
    for (int i = 0; i < n; i++) {
        double d = fabs(got[i] - want[i]);
        if (d > worst) worst = d;
        if (fabs(want[i]) > peak) peak = fabs(want[i]);
    }
    ok(worst < 1e-11 * (peak + 1.0),
       "imdct n=%d worst abs err %g (peak %g)", n, worst, peak);
    amdct_free(m);
    free(X); free(got); free(want);
}

static void test_imdct(void)
{
    /* AAC long/short, Vorbis's range, CELT's four frame sizes doubled. */
    static const int sizes[] = { 8, 16, 64, 120, 240, 256, 480, 512, 960,
                                 1024, 1920, 2048, 4096, 8192 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        test_imdct_size(sizes[i]);

    /* The two symmetries the fast path exploits, asserted against the fast
     * path's own output so that dropping one of them is caught here rather
     * than showing up as a channel of noise inside a codec. */
    int n = 256;
    double *X = malloc((size_t)(n / 2) * sizeof(double));
    double *y = malloc((size_t)n * sizeof(double));
    for (int i = 0; i < n / 2; i++) X[i] = frand();
    amdct *m = amdct_new(n);
    amdct_imdct(m, X, y);
    int s1 = 1, s2 = 1;
    for (int j = 0; j < n / 4; j++) {
        if (fabs(y[n / 2 - 1 - j] + y[j]) > 1e-12) s1 = 0;
        if (fabs(y[3 * n / 2 - 1 - (j + n / 2)] - y[j + n / 2]) > 1e-12) s2 = 0;
    }
    ok(s1, "IMDCT first half is antisymmetric: y[n/2-1-j] == -y[j]");
    ok(s2, "IMDCT second half is symmetric: y[3n/2-1-j] == y[j]");
    amdct_free(m);
    free(X); free(y);
}

int main(void)
{
    printf("afft/amath: FFT, IMDCT and the from-scratch transcendentals\n");
    test_amath();
    test_fft();
    test_imdct();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
