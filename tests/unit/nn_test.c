/* nn_test.c -- every kernel in c/lib/nn against a double-precision reference.
 *
 * THE REFERENCE IS WRITTEN HERE AND IT IS NOT A COPY. Each `ref_*` below is
 * the textbook definition evaluated in double, in the most obvious order, with
 * none of the things that make the real kernel fast: no four-way accumulator,
 * no loop reordering, no fused pass. That is the entire point. A test that
 * re-implemented the optimisation would agree with the optimisation's bugs,
 * which is how a "reference" ends up certifying the thing it was meant to
 * check -- this tree has the same argument in c/lib/gfx (an analytic predicate
 * rather than a second rasterizer) and in c/lib/video (ffmpeg, bit-exact).
 *
 * THE TOLERANCE IS DERIVED, NOT CHOSEN. A dot product of length k accumulated
 * in f32 has a relative error bounded by roughly k * 2^-24 in the worst case
 * and sqrt(k) * 2^-24 in practice. Every check below states the bound it is
 * asserting in terms of k rather than picking a round number that happens to
 * pass -- an epsilon fitted to the observed error measures nothing, because it
 * will also pass the next version whatever it does.
 *
 * WHAT WOULD SILENTLY PASS A WEAKER TEST, and is therefore checked explicitly:
 *   - a transposed matmul (A.B vs B.A) on a SQUARE shape;
 *   - a quantiser that divides by zero on an all-zero row;
 *   - a softmax with no max-shift, which is correct until a logit reaches 89;
 *   - RoPE's two conventions, which differ by a permutation and nothing else;
 *   - an rmsnorm that subtracts the mean (that is LayerNorm).
 * Each of those produces plausible numbers, so a "does it run" test sees none
 * of them.
 *
 *   make test-nn          this file
 *   make test-nn-negctl   three separate builds, each a DIFFERENT plausible
 *                         wrong algorithm: rmsnorm as LayerNorm, softmax
 *                         without the max shift, rope with huggingface's
 *                         pairing. Each must fail its own checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nn.h"

static int checks, failed;

static void ok(const char *what) { checks++; printf("ok  : %s\n", what); }
static void bad(const char *what, double got, double want, double bound)
{
    checks++; failed++;
    printf("FAIL: %s\n      got %.9g want %.9g (|err| %.3g > bound %.3g)\n",
           what, got, want, fabs(got - want), bound);
}
static void near(const char *what, double got, double want, double bound)
{
    if (fabs(got - want) <= bound) ok(what);
    else bad(what, got, want, bound);
}
static void eqi(const char *what, long got, long want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %ld want %ld\n", what, got, want); }
}

/* A deterministic generator, so a failure is reproducible and a bisect sees
 * the same numbers. Not rand(): its sequence is a libc detail and this test
 * compiles against two different libcs. */
static unsigned long g_seed = 88172645463325252UL;
static double urand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (double)((g_seed >> 11) & 0xFFFFFFFFUL) / 4294967296.0;
}
static float frand(void) { return (float)(urand() * 2.0 - 1.0); }

/* ------------------------------------------------------------ references -- */

static double ref_dot(const float *a, const float *b, int k)
{
    double s = 0.0;
    for (int i = 0; i < k; i++) s += (double)a[i] * (double)b[i];
    return s;
}

static void ref_matmul(double *c, const float *a, const float *b,
                       int m, int k, int n)
{
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            double s = 0.0;
            for (int p = 0; p < k; p++)
                s += (double)a[(size_t)i * k + p] * (double)b[(size_t)p * n + j];
            c[(size_t)i * n + j] = s;
        }
}

static void ref_rmsnorm(double *y, const float *x, const float *g,
                        int n, double eps)
{
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * (double)x[i];
    double inv = 1.0 / sqrt(ss / (double)n + eps);
    for (int i = 0; i < n; i++) y[i] = (g ? (double)g[i] : 1.0) * (double)x[i] * inv;
}

static void ref_softmax(double *y, const float *x, int n)
{
    double m = x[0];
    for (int i = 1; i < n; i++) if ((double)x[i] > m) m = x[i];
    double s = 0.0;
    for (int i = 0; i < n; i++) { y[i] = exp((double)x[i] - m); s += y[i]; }
    for (int i = 0; i < n; i++) y[i] /= s;
}

static void ref_rope(double *y, const float *x, int n, int pos, double theta)
{
    int half = n / 2;
    for (int i = 0; i < half; i++) {
        double freq = 1.0 / pow(theta, (double)(2 * i) / (double)n);
        double a = (double)pos * freq;
        double c = cos(a), s = sin(a);
        double x0 = x[2 * i], x1 = x[2 * i + 1];
        y[2 * i]     = x0 * c - x1 * s;
        y[2 * i + 1] = x0 * s + x1 * c;
    }
}

/* ----------------------------------------------------------------- tests -- */

static void t_shapes(void)
{
    printf("-- tensors: shape, ownership, and refusing what cannot be held\n");
    struct nn_tensor t;
    int d3[3] = { 2, 3, 4 };
    eqi("nn_new accepts a 3-D shape", nn_new(&t, NN_F32, 3, d3), 1);
    eqi("numel is the product", (long)nn_numel(&t), 24);
    eqi("bytes is numel * 4", (long)nn_bytes(&t), 96);
    eqi("a new tensor owns its data", t.own, 1);
    /* Zeroed on allocation, because an inference pass accumulates into
     * activations and an uninitialised one is a NaN that appears at layer 12
     * with nothing pointing back here. */
    eqi("a new tensor is zeroed", (long)(t.data[0] == 0.0f && t.data[23] == 0.0f), 1);
    nn_free(&t);
    eqi("nn_free zeroes the struct", t.ndim, 0);

    int dbad[2] = { 3, 0 };
    eqi("a zero dimension is refused", nn_new(&t, NN_F32, 2, dbad), 0);
    eqi("a refused shape leaves nothing behind", (long)(t.data == 0 && t.ndim == 0), 1);
    int dneg[2] = { -1, 4 };
    eqi("a negative dimension is refused", nn_new(&t, NN_F32, 2, dneg), 0);
    eqi("ndim above NN_MAXDIM is refused", nn_new(&t, NN_F32, NN_MAXDIM + 1, d3), 0);

    /* A wrapped tensor describes memory it does not own, which is what a
     * mapped weight file will be. nn_free must not touch it -- the check is
     * that the buffer survives, because a free() of a mapping is a fault the
     * test would not survive either. */
    float buf[6] = { 1, 2, 3, 4, 5, 6 };
    int d2[2] = { 2, 3 };
    nn_wrap_f32(&t, buf, 2, d2);
    eqi("a wrapped tensor does not own its data", t.own, 0);
    eqi("wrap does not copy", (long)(t.data == buf), 1);
    nn_free(&t);
    eqi("freeing a wrapped tensor leaves the buffer alone", (long)(buf[0] == 1.0f), 1);

    int dq[2] = { 4, 8 };
    eqi("a Q8 tensor allocates", nn_new(&t, NN_Q8, 2, dq), 1);
    /* 32 int8 values + one f32 scale per ROW. The scales are part of what the
     * tensor costs, so nn_bytes counts them -- the question a caller asks is
     * "will this fit", not "how big is the payload". */
    eqi("Q8 bytes counts the per-row scales", (long)nn_bytes(&t), 32 + 4 * 4);
    nn_free(&t);
}

static void t_matvec(void)
{
    printf("-- matvec f32 against a double dot product\n");
    enum { N = 37, K = 512 };
    static float w[N * K], x[K], y[N];
    for (int i = 0; i < N * K; i++) w[i] = frand();
    for (int i = 0; i < K; i++) x[i] = frand();
    nn_matvec_f32(y, w, x, N, K);

    /* THE BOUND. Values are in [-1,1], so a row's terms are too and the exact
     * sum is O(sqrt(K)) in magnitude. f32 has a 24-bit mantissa; a four-way
     * accumulation over K terms carries at most about (K/4) roundings per
     * partial sum, each of relative size 2^-24 against a partial sum bounded
     * by K. So an absolute bound of K * 2^-24 * 4 is generous and still tight
     * enough to catch a wrong element -- which would be O(1) off, not O(1e-4). */
    double bound = (double)K * 4.0 / 16777216.0;
    int worst_i = 0; double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double r = ref_dot(w + (size_t)i * K, x, K);
        double e = fabs((double)y[i] - r);
        if (e > worst) { worst = e; worst_i = i; }
    }
    near("every row is within K * 2^-24 * 4 of the exact dot product",
         worst, 0.0, bound);
    printf("      worst row %d, |err| %.3g, bound %.3g\n", worst_i, worst, bound);

    /* A zero-length or NULL call must do nothing rather than crash: the
     * shapes in a real model are computed, and a zero shows up as a bug
     * somewhere else, not here. */
    y[0] = 42.0f;
    nn_matvec_f32(y, w, x, 0, K);
    eqi("n = 0 writes nothing", (long)(y[0] == 42.0f), 1);
    nn_matvec_f32(y, 0, x, N, K);
    eqi("a NULL weight writes nothing", (long)(y[0] == 42.0f), 1);
}

static void t_matmul(void)
{
    printf("-- matmul f32, including the transpose a square shape would hide\n");
    enum { M = 7, K = 11, N = 13 };
    static float a[M * K], b[K * N], c[M * N];
    static double r[M * N];
    for (int i = 0; i < M * K; i++) a[i] = frand();
    for (int i = 0; i < K * N; i++) b[i] = frand();
    nn_matmul_f32(c, a, b, M, K, N);
    ref_matmul(r, a, b, M, K, N);
    double bound = (double)K * 4.0 / 16777216.0;
    double worst = 0.0;
    for (int i = 0; i < M * N; i++) {
        double e = fabs((double)c[i] - r[i]);
        if (e > worst) worst = e;
    }
    near("C[m,n] = A[m,k].B[k,n] elementwise", worst, 0.0, bound);

    /* NON-SQUARE ON PURPOSE. With M == K == N a transposed implementation
     * produces a matrix of the right shape full of wrong numbers, and an
     * elementwise comparison against a reference that shares the bug passes.
     * Here the shapes 7x11 and 11x13 make a transpose a size error, and the
     * check below makes it an explicit one rather than a segfault. */
    eqi("the shapes are deliberately distinct", (long)(M != K && K != N && M != N), 1);

    /* The i-k-j ordering skips a zero row of A. That is an optimisation, and
     * an optimisation that changes the answer is a bug -- so a matrix with
     * zeros in it has to give the same result as the reference, which does
     * not skip anything. */
    for (int i = 0; i < K; i++) a[(size_t)3 * K + i] = 0.0f;
    nn_matmul_f32(c, a, b, M, K, N);
    ref_matmul(r, a, b, M, K, N);
    worst = 0.0;
    for (int i = 0; i < M * N; i++) {
        double e = fabs((double)c[i] - r[i]);
        if (e > worst) worst = e;
    }
    near("the zero-skip in the inner loop does not change the answer",
         worst, 0.0, bound);
}

static void t_quant(void)
{
    printf("-- q8: per-row scales, the all-zero row, and the error it costs\n");
    enum { N = 16, K = 256 };
    static float w[N * K], back[N * K], x[K];
    static int8_t q[N * K];
    static float scale[N];

    /* Rows of DELIBERATELY different magnitude -- 1, 1e-3, 1e-6, ... -- which
     * is the case per-row quantisation exists for. Under a single per-tensor
     * scale the small rows would quantise to all zeros and the model would
     * lose those output channels entirely, silently. */
    for (int i = 0; i < N; i++) {
        float mag = 1.0f;
        for (int e = 0; e < i; e++) mag *= 0.5f;
        for (int j = 0; j < K; j++) w[(size_t)i * K + j] = frand() * mag;
    }
    /* One row entirely zero: the division-by-zero case. */
    for (int j = 0; j < K; j++) w[(size_t)5 * K + j] = 0.0f;

    nn_quantize_q8(q, scale, w, N, K);
    eqi("an all-zero row gets a zero scale", (long)(scale[5] == 0.0f), 1);
    nn_dequantize_q8(back, q, scale, N, K);
    for (int j = 0; j < K; j++)
        if (back[(size_t)5 * K + j] != 0.0f) { bad("the zero row reconstructs exactly",
                                                   back[(size_t)5 * K + j], 0.0, 0.0); break; }
    ok("the zero row reconstructs exactly");

    /* No value may leave the int8 range, and -128 in particular must never
     * appear: the reconstruction is symmetric around zero and -128 has no
     * positive partner, so a single one biases a whole output channel. */
    int over = 0;
    for (int i = 0; i < N * K; i++) if (q[i] == -128) over++;
    eqi("no weight quantises to -128", over, 0);

    /* THE ERROR IS BOUNDED BY HALF A STEP, per row, by construction. This is
     * the property that makes q8 usable at all, and it is checked per row
     * rather than globally -- a global check would be satisfied by the largest
     * row alone and would say nothing about the small ones, which are exactly
     * the ones a per-tensor scale destroys. */
    int worst_row = -1; double worst_ratio = 0.0;
    for (int i = 0; i < N; i++) {
        if (scale[i] == 0.0f) continue;
        double half = (double)scale[i] * 0.5;
        for (int j = 0; j < K; j++) {
            double e = fabs((double)back[(size_t)i * K + j] - (double)w[(size_t)i * K + j]);
            double ratio = e / half;
            if (ratio > worst_ratio) { worst_ratio = ratio; worst_row = i; }
        }
    }
    /* 1.0 plus a hair for the f32 rounding of the reconstruction itself. */
    near("every weight reconstructs within half a quantisation step",
         worst_ratio, 0.0, 1.0 + 1e-5);
    printf("      worst row %d at %.4f of a half-step\n", worst_row, worst_ratio);

    /* And the matvec built on it agrees with the DEQUANTISED weights -- not
     * with the originals. That distinction is the whole honesty of the check:
     * q8 introduces error in the weights, once, offline; what the kernel must
     * not do is introduce any MORE. Comparing against the originals would fold
     * the two together and let a broken kernel hide inside the quantisation
     * error it is allowed to have. */
    for (int i = 0; i < K; i++) x[i] = frand();
    static float y[N];
    nn_matvec_q8(y, q, scale, x, N, K);
    double bound = (double)K * 8.0 / 16777216.0;
    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double r = ref_dot(back + (size_t)i * K, x, K);
        double e = fabs((double)y[i] - r);
        /* Scaled by the row's magnitude: an absolute bound would be vacuous
         * for the rows that are 2^-15 smaller than the first. */
        double mag = scale[i] > 0.0f ? (double)scale[i] * 127.0 : 1.0;
        if (e / mag > worst) worst = e / mag;
    }
    near("matvec_q8 matches an exact dot of the dequantised weights",
         worst, 0.0, bound);
}

static void t_rmsnorm(void)
{
    printf("-- rmsnorm, and the mean it must NOT subtract\n");
    enum { N = 1024 };
    static float x[N], g[N], y[N];
    static double r[N];
    /* A DELIBERATE NON-ZERO MEAN. This is the check that separates RMS norm
     * from LayerNorm: with a zero-mean input the two agree, so a test built on
     * random symmetric data would pass against either and catch nothing. Every
     * value here is shifted by +3, so an implementation that subtracts the
     * mean is off by a mile. */
    for (int i = 0; i < N; i++) { x[i] = frand() + 3.0f; g[i] = frand(); }
    nn_rmsnorm(y, x, g, N, 1e-5f);
    ref_rmsnorm(r, x, g, N, 1e-5);
    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)y[i] - r[i]);
        if (e > worst) worst = e;
    }
    /* The output is O(1) in magnitude (it is normalised), so the bound is a
     * few f32 ulps and not a function of N -- the N-dependence lives in the
     * sum of squares, which is computed in double precisely so that it does
     * not reach the output. That is what the negative control removes. */
    near("rmsnorm matches the double reference on a mean-3 input",
         worst, 0.0, 1e-5);
    printf("      worst |err| %.3g on a deliberately non-zero-mean input\n", worst);

    /* A NULL gain means gain 1, checked against its OWN reference rather than
     * by dividing the gained result by g -- g is random and can be arbitrarily
     * close to zero, so that division is a test that fails on the data instead
     * of on the code. */
    nn_rmsnorm(y, x, 0, N, 1e-5f);
    ref_rmsnorm(r, x, 0, N, 1e-5);
    worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)y[i] - r[i]);
        if (e > worst) worst = e;
    }
    near("a NULL gain is the identity gain", worst, 0.0, 1e-5);
}

static void t_softmax(void)
{
    printf("-- softmax: the max-shift, and what it is worth\n");
    enum { N = 64 };
    static float x[N], keep[N];
    static double r[N];
    for (int i = 0; i < N; i++) x[i] = frand() * 4.0f;
    memcpy(keep, x, sizeof x);
    ref_softmax(r, x, N);
    nn_softmax(x, N);
    double worst = 0.0, sum = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)x[i] - r[i]);
        if (e > worst) worst = e;
        sum += x[i];
    }
    near("softmax matches the double reference", worst, 0.0, 1e-6);
    near("softmax sums to 1", sum, 1.0, 1e-5);

    /* THE CASE THE SHIFT EXISTS FOR. exp(120) is +inf in f32 and in double it
     * is 1.3e52 -- finite, but the ratio is what matters and an unshifted f32
     * implementation returns NaN for every element. Attention logits reach
     * this range on real models, so this is not a synthetic edge. */
    static float big[N];
    for (int i = 0; i < N; i++) big[i] = (float)(i) * 3.0f;   /* up to 189 */
    memcpy(keep, big, sizeof big);
    ref_softmax(r, big, N);
    nn_softmax(keep, N);
    int nan = 0; sum = 0.0;
    for (int i = 0; i < N; i++) { if (keep[i] != keep[i]) nan++; sum += keep[i]; }
    eqi("large logits produce no NaN", nan, 0);
    near("and still sum to 1", sum, 1.0, 1e-5);
    /* AND THE DISTRIBUTION IS RIGHT, not merely finite. "No NaN" alone would
     * be satisfied by returning a uniform row, which is what a clamped
     * exponential produces and is exactly as wrong as a NaN while looking
     * like a working attention. Compared against the same double reference,
     * at logits an unshifted f32 implementation cannot represent at all.
     *
     * (An earlier version of this check asserted the top logit took
     * "essentially all the mass". It does not: the top two differ by 3, so
     * the maximum is 1 - e^-3 - ... = 0.9502, and the bound was the test's
     * arithmetic being wrong rather than the kernel's. A derived constant
     * that nobody derived is how a test ends up measuring its author.) */
    worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)keep[i] - r[i]);
        if (e > worst) worst = e;
    }
    near("and match the reference distribution at logits up to 189",
         worst, 0.0, 1e-6);
}

static void t_silu_swiglu(void)
{
    printf("-- silu and the swiglu pairing\n");
    enum { N = 33 };
    static float a[N], b[N], out[N], sa[N];
    for (int i = 0; i < N; i++) { a[i] = frand() * 6.0f; b[i] = frand(); }
    memcpy(sa, a, sizeof a);
    nn_silu(sa, N);
    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double v = (double)a[i];
        double r = v / (1.0 + exp(-v));
        double e = fabs((double)sa[i] - r);
        if (e > worst) worst = e;
    }
    near("silu(x) = x * sigmoid(x)", worst, 0.0, 1e-6);

    nn_swiglu(out, a, b, N);
    worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)out[i] - (double)sa[i] * (double)b[i]);
        if (e > worst) worst = e;
    }
    /* The fused form must equal silu-then-multiply exactly enough that fusing
     * it was free. If it did not, the fusion would be a silent change to the
     * model's arithmetic rather than a speed-up. */
    near("swiglu(a,b) == silu(a) * b", worst, 0.0, 1e-6);
}

static void t_rope(void)
{
    printf("-- rope: the interleaved convention, and that it is a ROTATION\n");
    enum { N = 64 };
    static float x[N], y[N];
    static double r[N];
    for (int i = 0; i < N; i++) x[i] = frand();
    memcpy(y, x, sizeof x);
    nn_rope(y, N, 7, 10000.0f);
    ref_rope(r, x, N, 7, 10000.0);
    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)y[i] - r[i]);
        if (e > worst) worst = e;
    }
    near("rope matches the double reference at pos 7", worst, 0.0, 1e-6);

    /* A ROTATION PRESERVES LENGTH, pairwise. This is the property that catches
     * a sign error in one of the four terms -- which the elementwise check
     * above also catches, but only because the reference shares the sign
     * convention. This one is independent of the reference entirely: it is
     * true of any correct rotary embedding, whichever convention it uses. */
    worst = 0.0;
    for (int i = 0; i < N / 2; i++) {
        double before = (double)x[2*i] * x[2*i] + (double)x[2*i+1] * x[2*i+1];
        double after  = (double)y[2*i] * y[2*i] + (double)y[2*i+1] * y[2*i+1];
        double e = fabs(after - before);
        if (e > worst) worst = e;
    }
    near("every pair keeps its length (it is a rotation)", worst, 0.0, 1e-6);

    /* Position 0 is the identity: cos(0)=1, sin(0)=0 for every frequency. A
     * model that applied rope at the wrong position would still pass the
     * reference check above (the reference would be asked the same wrong
     * position); this pins the one position whose answer is known without a
     * reference at all. */
    memcpy(y, x, sizeof x);
    nn_rope(y, N, 0, 10000.0f);
    int same = 1;
    for (int i = 0; i < N; i++) if (y[i] != x[i]) { same = 0; break; }
    eqi("rope at position 0 is the identity", same, 1);
}

int main(void)
{
    t_shapes();
    t_matvec();
    t_matmul();
    t_quant();
    t_rmsnorm();
    t_softmax();
    t_silu_swiglu();
    t_rope();

    printf("\nnn_test: %d checks, %d failures\n", checks, failed);
    if (failed) return 1;
    printf("nn_test: ALL PASS\n");
    return 0;
}
