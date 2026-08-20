/* lmshape_test.c -- the three things that had to be true before anyone builds
 * a 0.6B-class model on this stack, each measured against something that is
 * not the code under test.
 *
 *   cc -Ic/lib/nn -O2 -w -o build/lmshape_test tests/unit/lmshape_test.c \
 *      c/lib/nn/infer.c c/lib/nn/model.c c/lib/nn/tensor.c \
 *      c/lib/nn/matmul.c c/lib/nn/ops.c -lm && ./build/lmshape_test
 *
 * NEGATIVE CONTROL (must FAIL, and must fail in exactly one place):
 *   cc -Ic/lib/nn -O2 -w -DLMS_GQA_MODULO -o build/lmshape_neg ...same... && \
 *      ./build/lmshape_neg   # expected: non-zero exit, GQA rows red, MHA green
 *
 * ---------------------------------------------------------------------------
 * 1. GQA ABOVE 2:1. `model.h` has carried n_kv_heads since it was written and
 *    infer.c is BELIEVED to handle it; the only evidence was one 2:1 case in
 *    lm_infer_test.c. Qwen3-0.6B is 16 query heads over 8 kv heads. 16:8 and
 *    16:4 are checked here against a double reference written in this file.
 *
 *    THE REFERENCE AND THE IMPLEMENTATION AGREEING IS NOT THE WHOLE ANSWER,
 *    and saying so is the honest part: both write `kv = h / (nh/nkvh)`, so
 *    they confirm each other's ARITHMETIC and not the CONVENTION. The
 *    convention is llama's `repeat_kv`, which expands the kv dimension by
 *    n_rep so that kv head j serves query heads [j*n_rep, (j+1)*n_rep) -- a
 *    CONTIGUOUS block, i.e. exactly h / n_rep. The plausible alternative is
 *    the interleaved one, h % n_kv_heads, and -DLMS_GQA_MODULO puts it in the
 *    reference so the difference can be seen rather than argued.
 *
 *    IT MUST REDDEN EXACTLY TWO OF THE FOUR forward rows, and which two is
 *    the whole point: 16:8 and 16:4 fail (measured: worst |logit - ref| 8.63
 *    and 6.31 against a bound of 7.5e-4), while 16:16 and 16:1 stay green
 *    because at those two ratios the two mappings are PROVABLY the same
 *    function -- h/1 == h%16 for 16 heads over 16, and h/16 == h%1 == 0 for
 *    16 over 1. A control that reddened all four would mean the rows were
 *    failing for some other reason; a control that reddened none would mean
 *    this file never tested the grouped-query mapping at all.
 *
 * 2. QK-NORM, AND THE ORDER IT GOES IN. Qwen3 RMS-norms q and k per head
 *    between the projection and RoPE. Getting the order wrong is the same
 *    class of defect as the RoPE convention -- a model that runs and is
 *    subtly wrong -- and it is WORSE than that class in one specific way,
 *    which t_qknorm_order below measures:
 *
 *      RoPE is a rotation. It preserves the sum of squares of a head's slice,
 *      so the 1/sqrt(mean(x^2)) factor is the same in both orders. Only the
 *      elementwise gain distinguishes them -- and an untrained model's gain
 *      is all ones, at which the two orders are the SAME FUNCTION.
 *
 *    So the most natural test anybody would write (build a model, apply both
 *    orders, compare) passes on a correct implementation and on a wrong one.
 *    Measured below, head_dim 8, position 3:
 *
 *      uniform gain      worst |correct - swapped| 1.19e-07  (6.2e-08 of the
 *                        value scale)
 *      non-uniform gain  worst |correct - swapped| 0.1176    (4.4% of it)
 *
 *    The uniform figure is NOT zero and the first draft of this file asserted
 *    that it was, with memcmp, and failed: RoPE preserves the sum of squares
 *    only to f32 rounding (2.27970906 before, 2.27970897 after), so the two
 *    orders differ by one ulp of the norm factor. That is the trap in its
 *    exact form -- the orders disagree by the same amount everything else in
 *    the pass disagrees by, which is to say invisibly.
 *
 * 3. THE FORMAT AT THE TARGET SHAPE. Qwen3-0.6B's head_dim is 128 while
 *    dim/n_heads is 64, so `lm_expected_size` is checked against a byte count
 *    this file derives by hand from the shape, and every flag is checked to
 *    move the total by exactly the bytes it claims.
 *
 * TOLERANCES ARE DERIVED. An f32 dot product of length k carries a relative
 * error of order k*2^-24. The pass here is two layers, about 16 f32 stages
 * each, with k at most dim = 64 -- so the compounded relative bound is
 * 64 * 32 * 2^-24, written below as that product. Nothing here is an epsilon
 * chosen because it passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "infer.h"
#include "model.h"
#include "nn.h"

static int checks, failed;

static void ok_(const char *what) { checks++; printf("ok  : %s\n", what); }
static void eqi(const char *what, long got, long want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %ld want %ld\n", what, got, want); }
}
static void eqz(const char *what, unsigned long long got, unsigned long long want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %llu want %llu (diff %lld)\n",
                           what, got, want, (long long)got - (long long)want); }
}
static void near_(const char *what, double got, double want, double bound)
{
    checks++;
    if (fabs(got - want) <= bound) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %.9g want %.9g (|err| %.3g > %.3g)\n",
                           what, got, want, fabs(got - want), bound); }
}
static void gt_(const char *what, double got, double bound)
{
    checks++;
    if (got > bound) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %.9g, wanted > %.9g\n", what, got, bound); }
}

/* Deterministic and not rand(): rand()'s sequence is a libc detail and this
 * file is meant to build against mini-libc as well as glibc. */
static unsigned long long g_seed = 0x9E3779B97F4A7C15ULL;
static double urand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (double)((g_seed >> 11) & 0xFFFFFFFFULL) / 4294967296.0;
}

#define LMS_TOL (64.0 * 32.0 / 16777216.0)
#define LM_EPS  1e-5f              /* infer.c's LM_RMS_EPS, which is not
                                    * exported; a mismatch here would show up
                                    * as a reference disagreement, not as a
                                    * silent pass, because eps changes the
                                    * answer by more than LMS_TOL. */

/* ======================================================================== */
/* 2. QK-NORM                                                               */
/* ======================================================================== */

/* The reference: RMS-norm one head's slice, in double, written straight from
 * the definition. It shares no code with nn_rmsnorm. */
static void ref_rms(double *y, const double *x, const float *g, int n)
{
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (double)n + (double)LM_EPS;
    double inv = 1.0 / sqrt(ss);
    for (int i = 0; i < n; i++) y[i] = (double)g[i] * (x[i] * inv);
}

static void t_qknorm_ref(void)
{
    enum { NH = 5, HD = 8, N = NH * HD };
    float v[N], g[HD], base[HD];
    double x[N], r[N];
    for (int i = 0; i < HD; i++) g[i] = (float)(0.5 + 1.5 * urand());
    /* EVERY HEAD IS THE SAME VECTOR AT A DIFFERENT MAGNITUDE. That is the
     * whole construction: an RMS norm is scale-invariant, so a PER-HEAD norm
     * must return all five heads to the same values, and a norm over the
     * whole vector cannot -- it divides every head by one shared factor and
     * leaves the 1:2:3:4:5 spread in place. Random values per head would make
     * the two indistinguishable, which is what the first version of this
     * check did and why it failed with a spread of 3.94 against a correct
     * implementation. */
    for (int i = 0; i < HD; i++) base[i] = (float)(4.0 * urand() - 2.0);
    for (int h = 0; h < NH; h++)
        for (int i = 0; i < HD; i++) {
            v[h * HD + i] = base[i] * (float)(h + 1);
            x[h * HD + i] = (double)v[h * HD + i];
        }

    lm_qk_norm(v, g, NH, HD, LM_EPS);
    for (int h = 0; h < NH; h++) ref_rms(r + h * HD, x + h * HD, g, HD);

    double worst = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)v[i] - r[i]);
        if (e > worst) worst = e;
    }
    printf("      worst |lm_qk_norm - double ref| = %.4g over %d values\n", worst, N);
    /* Each output is g[i] * x[i] * inv: three f32 roundings over a sum of HD
     * terms, so HD * 2^-24 relative, against outputs of magnitude ~2. */
    near_("lm_qk_norm matches a double reference, per head", worst, 0.0,
          2.0 * HD / 16777216.0);

    /* PER HEAD, and this is the check that separates it from a whole-vector
     * norm: head h is head 0 scaled by (h+1), and a per-head norm divides
     * that factor straight back out, so every head's output must equal head
     * 0's. A norm over the whole vector divides all five by one factor and
     * leaves the spread untouched.
     *
     * THE BOUND IS NOT ZERO, AND THE REASON IS eps. RMS norm is
     * x/sqrt(mean(x^2) + eps), which is scale-invariant only when eps is 0:
     * scaling by (h+1) scales mean(x^2) by (h+1)^2 while eps stays put, so
     * head h comes back larger than head 0 by a factor of at most
     * sqrt((m+eps)/m) ~= 1 + eps/(2m). Both m and the output magnitude are
     * measured from this run's own data rather than assumed, and the f32
     * rounding term (HD*2^-24 relative, from the sum of squares) is added on
     * top. Fitting a tolerance to the observed spread instead would have
     * hidden exactly the effect this paragraph explains. */
    double m = 0.0, mag = 0.0;
    for (int i = 0; i < HD; i++) m += (double)base[i] * base[i];
    m /= (double)HD;
    for (int i = 0; i < N; i++) if (fabs((double)v[i]) > mag) mag = fabs((double)v[i]);
    double bound = mag * ((double)LM_EPS / (2.0 * m) + (double)HD / 16777216.0);

    double spread = 0.0;
    for (int h = 1; h < NH; h++)
        for (int i = 0; i < HD; i++) {
            double e = fabs((double)v[h * HD + i] - (double)v[i]);
            if (e > spread) spread = e;
        }
    printf("      head-to-head spread %.4g, eps+rounding bound %.4g "
           "(a whole-vector norm would leave ~%.4g)\n", spread, bound, mag * 0.8);
    near_("the norm is per head, not over the whole vector", spread, 0.0, bound);
}

static void t_qknorm_order(void)
{
    enum { NH = 4, HD = 8, N = NH * HD, POS = 3 };
    float base[N], g1[HD], gv[HD];
    for (int i = 0; i < N; i++) base[i] = (float)(2.0 * urand() - 1.0);
    for (int i = 0; i < HD; i++) { g1[i] = 1.0f; gv[i] = (float)(0.5 + 1.0 * urand()); }

    /* THE MECHANISM, measured first so the two results below are explained
     * rather than merely reported: RoPE preserves each head's sum of squares,
     * which is why the norm FACTOR cannot tell the orders apart. */
    {
        float t[N];
        memcpy(t, base, sizeof t);
        double before = 0.0, after = 0.0;
        for (int i = 0; i < HD; i++) before += (double)t[i] * t[i];
        for (int h = 0; h < NH; h++) nn_rope(t + h * HD, HD, POS, 10000.0f);
        for (int i = 0; i < HD; i++) after += (double)t[i] * t[i];
        printf("      head 0 sum of squares: %.9g before RoPE, %.9g after\n", before, after);
        near_("RoPE preserves a head's sum of squares (why the orders collide)",
              after, before, before * HD / 16777216.0);
    }

    float a[N], b[N];

    /* Uniform gain -- the trap. */
    memcpy(a, base, sizeof a);
    lm_qk_norm(a, g1, NH, HD, LM_EPS);
    for (int h = 0; h < NH; h++) nn_rope(a + h * HD, HD, POS, 10000.0f);

    memcpy(b, base, sizeof b);
    for (int h = 0; h < NH; h++) nn_rope(b + h * HD, HD, POS, 10000.0f);
    lm_qk_norm(b, g1, NH, HD, LM_EPS);

    /* THE FIRST VERSION OF THIS CHECK ASSERTED memcmp == 0 AND FAILED, and
     * the failure is worth more than the assertion was. In exact arithmetic
     * the two orders ARE the same function when the gain is uniform: RoPE is
     * linear, so rope(inv*x) = inv*rope(x), and RoPE preserves the sum of
     * squares, so the inv computed from rope(x) equals the one computed from
     * x. In f32 that second equality holds only to rounding -- the run above
     * prints a head's sum of squares as 3.11370169 before RoPE and 3.11370184
     * after, a relative move of 4.8e-8, which is 2^-24 to within a factor of
     * two. So the orders differ by ONE ULP OF THE NORM FACTOR and not by
     * zero.
     *
     * That does not weaken the trap, it sharpens it: the two orders agree to
     * f32 rounding, which is the same size as every other rounding difference
     * in the pass and therefore invisible to any tolerance a forward-pass
     * test could sensibly use. The number to compare it against is the
     * non-uniform case below. */
    double usame = 0.0, umag = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)a[i] - (double)b[i]);
        if (e > usame) usame = e;
        if (fabs((double)a[i]) > umag) umag = fabs((double)a[i]);
    }
    printf("      uniform gain: worst |correct - swapped| = %.4g "
           "over a value scale of %.4g  (= %.2g relative)\n",
           usame, umag, umag > 0 ? usame / umag : 0.0);
    /* HD*2^-24 relative: the sum of squares is HD f32 additions, and the
     * norm factor carries that error into every output. */
    near_("UNIFORM gain: the two orders agree to within f32 rounding "
          "(the trap -- no test can see the bug here)", usame, 0.0,
          umag * (double)HD / 16777216.0);

    /* Non-uniform gain -- where the order becomes visible. */
    memcpy(a, base, sizeof a);
    lm_qk_norm(a, gv, NH, HD, LM_EPS);
    for (int h = 0; h < NH; h++) nn_rope(a + h * HD, HD, POS, 10000.0f);

    memcpy(b, base, sizeof b);
    for (int h = 0; h < NH; h++) nn_rope(b + h * HD, HD, POS, 10000.0f);
    lm_qk_norm(b, gv, NH, HD, LM_EPS);

    double worst = 0.0, mag = 0.0;
    for (int i = 0; i < N; i++) {
        double e = fabs((double)a[i] - (double)b[i]);
        if (e > worst) worst = e;
        if (fabs((double)a[i]) > mag) mag = fabs((double)a[i]);
    }
    printf("      non-uniform gain: worst |correct - swapped| = %.4g "
           "over a value scale of %.4g\n", worst, mag);
    /* The separation must be far above rounding, or the check is measuring
     * noise. 1% of the value scale is four orders of magnitude above the
     * HD*2^-24 = 4.8e-7 relative that f32 rounding can produce. */
    gt_("NON-UNIFORM gain: the two orders separate far above f32 rounding",
        worst, 0.01 * mag);
}

/* ======================================================================== */
/* 1. + 3. a fixture, a reference forward pass, and the format               */
/* ======================================================================== */

struct cfg {
    int dim, n_layers, n_heads, n_kv_heads, head_dim, hidden, vocab, seq_len, tied;
};

/* model.h's payload order, stated here for the writer. Only the shapes that
 * this file's configs use: f32 weights, no LM_QKNORM in a file (infer.c does
 * not consume it yet -- see the report), no LM_QEMB in the forward-pass
 * configs. t_format below drives the flags through lm_expected_size directly,
 * which is where they can be checked without a forward pass. */
#define MAXT 64
struct fx {
    struct cfg c;
    int hd, q_dim, kv_dim;
    int n, rows[MAXT], cols[MAXT];
    float *W[MAXT];
};

#define T_EMB      0
#define T_LAY(l)   (1 + (l) * 9)
#define T_AN 0
#define T_WQ 1
#define T_WK 2
#define T_WV 3
#define T_WO 4
#define T_FN 5
#define T_W1 6
#define T_W3 7
#define T_W2 8
#define T_FINAL(c) (1 + (c)->n_layers * 9)

static void fx_build(struct fx *f, const struct cfg *c)
{
    memset(f, 0, sizeof *f);
    f->c = *c;
    f->hd = c->head_dim ? c->head_dim : c->dim / c->n_heads;
    f->q_dim  = c->n_heads    * f->hd;
    f->kv_dim = c->n_kv_heads * f->hd;
    int i = 0;
#define ADD(r, co) do { f->rows[i] = (r); f->cols[i] = (co); i++; } while (0)
    ADD(c->vocab, c->dim);
    for (int l = 0; l < c->n_layers; l++) {
        ADD(1, c->dim);
        ADD(f->q_dim,  c->dim);
        ADD(f->kv_dim, c->dim);
        ADD(f->kv_dim, c->dim);
        ADD(c->dim,    f->q_dim);
        ADD(1, c->dim);
        ADD(c->hidden, c->dim);
        ADD(c->hidden, c->dim);
        ADD(c->dim,    c->hidden);
    }
    ADD(1, c->dim);
    if (!c->tied) ADD(c->vocab, c->dim);
#undef ADD
    f->n = i;
    for (int t = 0; t < f->n; t++) {
        size_t ne = (size_t)f->rows[t] * f->cols[t];
        f->W[t] = (float *)malloc(ne * sizeof(float));
        int isnorm = (f->rows[t] == 1);
        for (size_t j = 0; j < ne; j++)
            f->W[t][j] = isnorm ? (float)(0.8 + 0.4 * urand())
                                : (float)(urand() - 0.5);
    }
}

static void fx_free(struct fx *f) { for (int i = 0; i < f->n; i++) free(f->W[i]); }

static unsigned char *fx_blob(const struct fx *f, size_t *out_len)
{
    size_t len = sizeof(struct lm_header);
    for (int i = 0; i < f->n; i++)
        len += (size_t)f->rows[i] * f->cols[i] * sizeof(float);
    unsigned char *b = (unsigned char *)calloc(1, len);
    struct lm_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, LM_MAGIC, 8);
    h.version = LM_VERSION; h.dtype = NN_F32;
    h.dim = (uint32_t)f->c.dim; h.n_layers = (uint32_t)f->c.n_layers;
    h.n_heads = (uint32_t)f->c.n_heads; h.n_kv_heads = (uint32_t)f->c.n_kv_heads;
    h.hidden = (uint32_t)f->c.hidden; h.vocab = (uint32_t)f->c.vocab;
    h.seq_len = (uint32_t)f->c.seq_len;
    h.flags = f->c.tied ? LM_TIED : 0u;
    h.head_dim = (uint32_t)f->c.head_dim;
    memcpy(b, &h, sizeof h);
    size_t off = sizeof h;
    for (int i = 0; i < f->n; i++) {
        size_t ne = (size_t)f->rows[i] * f->cols[i];
        memcpy(b + off, f->W[i], ne * sizeof(float));
        off += ne * sizeof(float);
    }
    *out_len = len;
    return b;
}

/* --------------------------------------------------------- the reference --
 *
 * Double, straight-line, calls nothing under c/lib/nn. It recomputes every
 * position from scratch on every call and keeps no cache, so a cache-indexing
 * bug in infer.c has nothing here to agree with. */
#define R_T 8
#define R_D 64
#define R_H 64

static double rdot(const float *w, const double *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)w[i] * x[i];
    return s;
}

static void rrms(double *y, const double *x, const float *g, int n)
{
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (double)n + (double)LM_EPS;
    double inv = 1.0 / sqrt(ss);
    for (int i = 0; i < n; i++) y[i] = (double)g[i] * (x[i] * inv);
}

static void rrope(double *x, int n, int pos)
{
    for (int i = 0; i < n / 2; i++) {
        double fr = 1.0 / pow(10000.0, (double)(2 * i) / (double)n);
        double an = (double)pos * fr, c = cos(an), s = sin(an);
        double x0 = x[2 * i], x1 = x[2 * i + 1];
        x[2 * i]     = x0 * c - x1 * s;
        x[2 * i + 1] = x0 * s + x1 * c;
    }
}

/* Query head h reads kv head kvh_of(h). The whole subject of check 1. */
static int kvh_of(int h, int nh, int nkvh)
{
#ifdef LMS_GQA_MODULO
    /* NEGATIVE CONTROL: the interleaved mapping. Identical to the correct one
     * when nh == nkvh, which is what makes the MHA rows the control's own
     * control -- they must stay green. */
    (void)nh;
    return h % nkvh;
#else
    return h / (nh / nkvh);
#endif
}

static void ref_logits(const struct fx *f, const int *tok, int at, double *out)
{
    const struct cfg *c = &f->c;
    int D = c->dim, NH = c->n_heads, NKV = c->n_kv_heads;
    int HD = f->hd, KV = f->kv_dim, QD = f->q_dim, HI = c->hidden, VO = c->vocab;
    int T = at + 1;
    float * const *W = f->W;

    static double x[R_T][R_D], q[R_T][R_D], k[R_T][R_D], v[R_T][R_D];
    double xb[R_D], ao[R_D], hb[R_H], hb2[R_H], a[R_T];

    for (int t = 0; t < T; t++)
        for (int i = 0; i < D; i++) x[t][i] = (double)W[T_EMB][(size_t)tok[t] * D + i];

    for (int l = 0; l < c->n_layers; l++) {
        const float *an = W[T_LAY(l) + T_AN], *wq = W[T_LAY(l) + T_WQ];
        const float *wk = W[T_LAY(l) + T_WK], *wv = W[T_LAY(l) + T_WV];
        const float *wo = W[T_LAY(l) + T_WO], *fn = W[T_LAY(l) + T_FN];
        const float *w1 = W[T_LAY(l) + T_W1], *w3 = W[T_LAY(l) + T_W3];
        const float *w2 = W[T_LAY(l) + T_W2];

        for (int t = 0; t < T; t++) {
            rrms(xb, x[t], an, D);
            for (int i = 0; i < QD; i++) q[t][i] = rdot(wq + (size_t)i * D, xb, D);
            for (int i = 0; i < KV; i++) k[t][i] = rdot(wk + (size_t)i * D, xb, D);
            for (int i = 0; i < KV; i++) v[t][i] = rdot(wv + (size_t)i * D, xb, D);
            for (int h = 0; h < NH;  h++) rrope(q[t] + (size_t)h * HD, HD, t);
            for (int h = 0; h < NKV; h++) rrope(k[t] + (size_t)h * HD, HD, t);
        }
        for (int t = 0; t < T; t++) {
            for (int h = 0; h < NH; h++) {
                int off = kvh_of(h, NH, NKV) * HD;
                double mx = -1e300, sum = 0.0;
                for (int u = 0; u <= t; u++) {
                    double s = 0.0;
                    for (int i = 0; i < HD; i++) s += q[t][h * HD + i] * k[u][off + i];
                    a[u] = s / sqrt((double)HD);
                    if (a[u] > mx) mx = a[u];
                }
                for (int u = 0; u <= t; u++) { a[u] = exp(a[u] - mx); sum += a[u]; }
                for (int u = 0; u <= t; u++) a[u] /= sum;
                for (int i = 0; i < HD; i++) {
                    double s = 0.0;
                    for (int u = 0; u <= t; u++) s += a[u] * v[u][off + i];
                    ao[h * HD + i] = s;
                }
            }
            for (int i = 0; i < D; i++) x[t][i] += rdot(wo + (size_t)i * QD, ao, QD);
        }
        for (int t = 0; t < T; t++) {
            rrms(xb, x[t], fn, D);
            for (int i = 0; i < HI; i++) {
                hb[i]  = rdot(w1 + (size_t)i * D, xb, D);
                hb2[i] = rdot(w3 + (size_t)i * D, xb, D);
            }
            for (int i = 0; i < HI; i++) hb[i] = hb[i] / (1.0 + exp(-hb[i])) * hb2[i];
            for (int i = 0; i < D; i++) x[t][i] += rdot(w2 + (size_t)i * HI, hb, HI);
        }
    }
    rrms(xb, x[at], W[T_FINAL(c)], D);
    const float *cls = c->tied ? W[T_EMB] : W[T_FINAL(c) + 1];
    for (int i = 0; i < VO; i++) out[i] = rdot(cls + (size_t)i * D, xb, D);
}

static void t_forward(const char *name, struct cfg c, const int *tok, int nt)
{
    struct fx f;
    fx_build(&f, &c);
    size_t blen;
    unsigned char *blob = fx_blob(&f, &blen);

    struct lm_model m;
    struct lm_state s;
    char what[160];
    int rc = lm_open(&m, blob, blen);
    sprintf(what, "%s: lm_open", name);
    eqi(what, rc, 0);
    if (rc) { free(blob); fx_free(&f); return; }
    rc = lm_state_new(&s, &m);
    sprintf(what, "%s: lm_state_new", name);
    eqi(what, rc, 0);
    if (rc) { lm_close(&m); free(blob); fx_free(&f); return; }

    double ref[R_H];
    double worst = 0.0, scale = 1.0;
    for (int t = 0; t < nt; t++) {
        const float *lg = lm_forward(&m, &s, tok[t], t);
        if (!lg) {
            sprintf(what, "%s: lm_forward(pos %d) returned NULL", name, t);
            eqi(what, 0, 1);
            break;
        }
        ref_logits(&f, tok, t, ref);
        for (int i = 0; i < c.vocab; i++) {
            double e = fabs((double)lg[i] - ref[i]);
            if (e > worst) worst = e;
            if (fabs(ref[i]) > scale) scale = fabs(ref[i]);
        }
    }
    printf("      worst |logit - ref| = %.4g over a logit scale of %.4g\n", worst, scale);
    sprintf(what, "%s: logits match the double reference over %d positions", name, nt);
    near_(what, worst, 0.0, LMS_TOL * scale);

    lm_state_free(&s);
    lm_close(&m);
    free(blob);
    fx_free(&f);
}

/* ======================================================================== */
/* 3. the format at the target shape                                        */
/* ======================================================================== */

static void hdr(struct lm_header *h, uint32_t dim, uint32_t layers, uint32_t heads,
                uint32_t kvh, uint32_t hd, uint32_t hidden, uint32_t vocab,
                uint32_t seq, uint32_t dtype, uint32_t flags)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, LM_MAGIC, 8);
    h->version = LM_VERSION; h->dtype = dtype;
    h->dim = dim; h->n_layers = layers; h->n_heads = heads; h->n_kv_heads = kvh;
    h->hidden = hidden; h->vocab = vocab; h->seq_len = seq;
    h->flags = flags; h->head_dim = hd;
}

/* Qwen3-0.6B, and the byte total derived here BY HAND from the shape rather
 * than by calling the function under test. Written as a sum of named terms so
 * a disagreement says which tensor is wrong, not merely that the total is. */
static void t_format(void)
{
    const uint64_t DIM = 1024, NL = 28, NH = 16, NKV = 8, HD = 128;
    const uint64_t HI = 3072, VO = 151936, SEQ = 512;
    const uint64_t QD = NH * HD, KVD = NKV * HD;

    /* f32 everywhere, tied, no qknorm. */
    uint64_t attn = (QD * DIM + KVD * DIM + KVD * DIM + DIM * QD);
    uint64_t ffn  = 3 * HI * DIM;
    uint64_t norms = 2 * DIM;                          /* att_norm + ffn_norm */
    uint64_t per_layer = (attn + ffn + norms) * 4;
    uint64_t base = 64 + VO * DIM * 4 + NL * per_layer + DIM * 4;

    struct lm_header h;
    hdr(&h, (uint32_t)DIM, (uint32_t)NL, (uint32_t)NH, (uint32_t)NKV, (uint32_t)HD,
        (uint32_t)HI, (uint32_t)VO, (uint32_t)SEQ, NN_F32, LM_TIED);
    eqz("qwen3-0.6b f32 tied: lm_expected_size == the hand-derived total",
        (unsigned long long)lm_expected_size(&h), (unsigned long long)base);

    /* head_dim 0 must NOT give the same answer -- it is a different model
     * (64-wide heads), and the whole reason the field exists is that the old
     * format could not tell them apart. */
    struct lm_header h0 = h;
    h0.head_dim = 0;
    gt_("head_dim 0 sizes a DIFFERENT model (the derived 64-wide one)",
        (double)llabs((long long)lm_expected_size(&h) - (long long)lm_expected_size(&h0)),
        0.0);

    /* BYTE COMPATIBILITY. A header with head_dim 0 must size to exactly what
     * the pre-head_dim formula gave, which is stated here in its own terms:
     * wq and wo were [dim,dim], kv was n_kv_heads*(dim/n_heads). */
    {
        uint64_t hd0 = DIM / NH, kvd0 = NKV * hd0;
        uint64_t a0 = DIM * DIM + kvd0 * DIM + kvd0 * DIM + DIM * DIM;
        uint64_t pl0 = (a0 + ffn + norms) * 4;
        uint64_t old = 64 + VO * DIM * 4 + NL * pl0 + DIM * 4;
        eqz("head_dim 0 reproduces the pre-head_dim byte count exactly",
            (unsigned long long)lm_expected_size(&h0), (unsigned long long)old);
    }

    /* Each flag must move the total by exactly what it claims and nothing
     * else. Checked as a DIFFERENCE, so it cannot be satisfied by a formula
     * that is wrong in the same way in both terms. */
    struct lm_header hq = h;
    hq.flags |= LM_QKNORM;
    eqz("LM_QKNORM adds exactly 2 * head_dim * 4 bytes per layer",
        (unsigned long long)(lm_expected_size(&hq) - lm_expected_size(&h)),
        (unsigned long long)(NL * 2 * HD * 4));

    struct lm_header h8 = h, h8e = h;
    h8.dtype = NN_Q8;
    h8e.dtype = NN_Q8; h8e.flags |= LM_QEMB;
    /* f32 embedding -> q8 embedding: vocab*dim*4 becomes vocab*dim + vocab*4. */
    eqz("LM_QEMB shrinks the embedding from vocab*dim*4 to vocab*(dim+4)",
        (unsigned long long)(lm_expected_size(&h8) - lm_expected_size(&h8e)),
        (unsigned long long)(VO * DIM * 4 - (VO * DIM + VO * 4)));

    printf("      qwen3-0.6b: f32 %.1f MiB, q8 %.1f MiB, q8+qemb %.1f MiB\n",
           lm_expected_size(&h) / 1048576.0,
           lm_expected_size(&h8) / 1048576.0,
           lm_expected_size(&h8e) / 1048576.0);

    /* An unknown flag is a refusal, not something to ignore: it changes the
     * payload length, so ignoring it walks a layout that is not there. */
    struct lm_header hb = h;
    hb.flags |= 0x40000000u;
    eqz("an unknown flag is refused (size 0), not ignored",
        (unsigned long long)lm_expected_size(&hb), 0ULL);

    /* n_heads * head_dim must not be allowed to wrap a uint32. 16 heads of
     * 0x20000000 is exactly 2^33, which is 0 in 32 bits and would size every
     * attention tensor at nothing. */
    struct lm_header hw = h;
    hw.head_dim = 0x20000000u;
    eqz("n_heads * head_dim wrapping 32 bits is refused",
        (unsigned long long)lm_expected_size(&hw), 0ULL);
}

/* An explicit head_dim opens, and infer.c REFUSES to run it -- loudly, with a
 * NULL, rather than attending over the wrong stride. That refusal is the
 * current state of the hookup and is pinned here so that whoever wires
 * infer.c sees this check turn from "refuses" into a real forward pass. */
static void t_headdim_open(void)
{
    /* dim 8, 2 query heads of 6, 1 kv head: q_dim 12, kv_dim 6, and dim/n_heads
     * would say 4. Deliberately a shape the derived rule gets WRONG rather
     * than one it cannot express -- the derived answer here is plausible (4
     * divides 8 exactly), which is the case a reader would not question. */
    struct cfg c;
    c.dim = 8; c.n_layers = 1; c.n_heads = 2; c.n_kv_heads = 1; c.head_dim = 6;
    c.hidden = 16; c.vocab = 8; c.seq_len = 8; c.tied = 1;
    struct fx f;
    fx_build(&f, &c);
    size_t blen;
    unsigned char *blob = fx_blob(&f, &blen);
    struct lm_model m;
    eqi("explicit head_dim: lm_open accepts it", lm_open(&m, blob, blen), 0);
    eqi("explicit head_dim: q_dim = n_heads * head_dim", m.q_dim, 12);
    eqi("explicit head_dim: kv_dim = n_kv_heads * head_dim", m.kv_dim, 6);
    eqi("explicit head_dim: wq is [q_dim, dim]", m.layer[0].wq.dim[0], 12);
    eqi("explicit head_dim: wo is [dim, q_dim]", m.layer[0].wo.dim[1], 12);
    struct lm_state s;
    if (lm_state_new(&s, &m) == 0) {
        eqi("explicit head_dim: lm_forward REFUSES until infer.c derives hd "
            "from the header", (long)(lm_forward(&m, &s, 0, 0) == NULL), 1);
        lm_state_free(&s);
    } else {
        ok_("explicit head_dim: lm_state_new refuses it (also a loud refusal)");
    }
    lm_close(&m);
    free(blob);
    fx_free(&f);
}

/* LM_QEMB: the loader must give a quantised embedding AND a quantised
 * classifier from the one table, and lm_embed_row must reconstruct a row
 * without the caller branching on the flag. */
static void t_qemb(void)
{
    const int VO = 12, DIM = 8;
    /* Build the table, quantise it, and lay out a whole model by hand. Only
     * the embedding matters here, so the rest is one minimal layer. */
    struct cfg c = { DIM, 1, 2, 2, 0, 16, VO, 8, 1 };
    struct fx f;
    fx_build(&f, &c);

    /* Re-serialise with a q8 embedding and q8 weights. */
    size_t elen = (size_t)VO * DIM + (size_t)VO * sizeof(float);
    size_t len = sizeof(struct lm_header) + elen;
    for (int i = 1; i < f.n; i++) {
        size_t ne = (size_t)f.rows[i] * f.cols[i];
        len += (f.rows[i] == 1) ? ne * sizeof(float)
                                : ne + (size_t)f.rows[i] * sizeof(float);
    }
    unsigned char *b = (unsigned char *)calloc(1, len);
    struct lm_header h;
    hdr(&h, DIM, 1, 2, 2, 0, 16, VO, 8, NN_Q8, LM_TIED | LM_QEMB);
    memcpy(b, &h, sizeof h);
    size_t off = sizeof h;
    for (int i = 0; i < f.n; i++) {
        size_t ne = (size_t)f.rows[i] * f.cols[i];
        if (f.rows[i] == 1) { memcpy(b + off, f.W[i], ne * sizeof(float)); off += ne * sizeof(float); }
        else {
            nn_quantize_q8((int8_t *)(b + off), (float *)(b + off + ne),
                           f.W[i], f.rows[i], f.cols[i]);
            off += ne + (size_t)f.rows[i] * sizeof(float);
        }
    }
    eqz("qemb: the hand-laid blob is lm_expected_size bytes",
        (unsigned long long)len, (unsigned long long)lm_expected_size(&h));

    struct lm_model m;
    eqi("qemb: lm_open", lm_open(&m, b, len), 0);
    eqi("qemb: tok_emb (the f32 fast path) is NULL", (long)(m.tok_emb == NULL), 1);
    eqi("qemb: m.emb is q8", m.emb.dtype, NN_Q8);
    eqi("qemb: the tied classifier is the SAME quantised tensor",
        (long)(m.wcls.dtype == NN_Q8 && m.wcls.q == m.emb.q), 1);

    float row[16];
    double worst = 0.0, tol = 0.0;
    int rcbad = 0;
    for (int t = 0; t < VO; t++) {
        if (lm_embed_row(&m, t, row) != 0) { rcbad++; continue; }
        float s = m.emb.scale[t];
        if (0.5 * s > tol) tol = 0.5 * s;
        for (int i = 0; i < DIM; i++) {
            double e = fabs((double)row[i] - (double)f.W[T_EMB][(size_t)t * DIM + i]);
            if (e > worst) worst = e;
        }
    }
    /* DERIVED: nn_quantize_q8 rounds to the nearest multiple of
     * max|row|/127, so no element can be more than half a step out. */
    eqi("qemb: lm_embed_row returned 0 for every token", rcbad, 0);
    printf("      qemb: worst |dequantised - original| = %.4g, half a q8 step = %.4g\n",
           worst, tol);
    near_("qemb: lm_embed_row reconstructs every row to within half a q8 step",
          worst, 0.0, tol);
    eqi("qemb: lm_embed_row refuses a token out of range",
        (long)(lm_embed_row(&m, VO, row) < 0), 1);

    lm_close(&m);
    free(b);
    fx_free(&f);
}

/* A file that carries LM_QKNORM: the two gains must land BETWEEN wv and wo,
 * which is the one thing about the flag that a size check cannot see -- two
 * vectors of the right total length in the wrong place add up to exactly the
 * same byte count. So the payload is written with each tensor tagged by its
 * own value and the descriptors are read back by identity, not by shape.
 *
 * lm_forward is NOT run here. infer.c does not consume q_norm/k_norm yet (the
 * hookup is two lines and is named in this workflow's report); until it does,
 * a forward-pass assertion would be pinning the ABSENCE of the feature, and
 * would have to be rewritten the day it lands. What is pinned instead is the
 * part that is finished: the file layout and the loader. */
static void t_qknorm_file(void)
{
    const uint32_t DIM = 8, NL = 2, NH = 2, KVH = 1, HD = 4, HI = 16, VO = 6;
    struct lm_header h;
    hdr(&h, DIM, NL, NH, KVH, 0, HI, VO, 8, NN_F32, LM_TIED | LM_QKNORM);
    size_t len = lm_expected_size(&h);
    eqi("qknorm file: the header is sizeable", (long)(len > 0), 1);
    unsigned char *b = (unsigned char *)calloc(1, len);
    memcpy(b, &h, sizeof h);

    /* Walk model.h's payload order and write a distinct constant into each
     * tensor: 1000*layer + a per-tensor tag. A misplaced q_norm then reads as
     * a wrong TAG, not as a plausible number. */
    float *p = (float *)(b + sizeof h);
    for (uint32_t i = 0; i < VO * DIM; i++) *p++ = 1.0f;              /* tok_emb */
    for (uint32_t l = 0; l < NL; l++) {
        for (uint32_t i = 0; i < DIM; i++)     *p++ = 1000.0f * l + 1;  /* att_norm */
        for (uint32_t i = 0; i < DIM * DIM; i++) *p++ = 1000.0f * l + 2; /* wq */
        for (uint32_t i = 0; i < HD * KVH * DIM; i++) *p++ = 1000.0f * l + 3; /* wk */
        for (uint32_t i = 0; i < HD * KVH * DIM; i++) *p++ = 1000.0f * l + 4; /* wv */
        for (uint32_t i = 0; i < HD; i++)      *p++ = 1000.0f * l + 5;  /* q_norm */
        for (uint32_t i = 0; i < HD; i++)      *p++ = 1000.0f * l + 6;  /* k_norm */
        for (uint32_t i = 0; i < DIM * DIM; i++) *p++ = 1000.0f * l + 7; /* wo */
        for (uint32_t i = 0; i < DIM; i++)     *p++ = 1000.0f * l + 8;  /* ffn_norm */
        for (uint32_t i = 0; i < HI * DIM; i++) *p++ = 1000.0f * l + 9;  /* w1 */
        for (uint32_t i = 0; i < HI * DIM; i++) *p++ = 1000.0f * l + 10; /* w3 */
        for (uint32_t i = 0; i < DIM * HI; i++) *p++ = 1000.0f * l + 11; /* w2 */
    }
    for (uint32_t i = 0; i < DIM; i++) *p++ = 99.0f;                  /* final_norm */
    eqz("qknorm file: the writer landed exactly on the end of the blob",
        (unsigned long long)((unsigned char *)p - b), (unsigned long long)len);

    struct lm_model m;
    eqi("qknorm file: lm_open", lm_open(&m, b, len), 0);
    int okq = 1, okk = 1, okwo = 1;
    for (uint32_t l = 0; l < NL; l++) {
        const struct lm_layer *L = &m.layer[l];
        if (!L->q_norm || L->q_norm[0] != 1000.0f * l + 5 ||
            L->q_norm[HD - 1] != 1000.0f * l + 5) okq = 0;
        if (!L->k_norm || L->k_norm[0] != 1000.0f * l + 6 ||
            L->k_norm[HD - 1] != 1000.0f * l + 6) okk = 0;
        /* wo AFTER the gains, which is the offset a misplaced pair breaks. */
        if (L->wo.data[0] != 1000.0f * l + 7) okwo = 0;
    }
    eqi("qknorm file: q_norm points at the q gain, in every layer", okq, 1);
    eqi("qknorm file: k_norm points at the k gain, in every layer", okk, 1);
    eqi("qknorm file: wo still starts where it should (the gains sit before it)",
        okwo, 1);
    eqi("qknorm file: final_norm is the last tensor", (long)(m.final_norm[0] == 99.0f), 1);
    lm_close(&m);

    /* And without the flag, the same shape must leave both NULL -- so a
     * caller can ask the MODEL whether the file carried QK-norm rather than
     * re-reading the header. */
    struct lm_header h2;
    hdr(&h2, DIM, NL, NH, KVH, 0, HI, VO, 8, NN_F32, LM_TIED);
    size_t len2 = lm_expected_size(&h2);
    unsigned char *b2 = (unsigned char *)calloc(1, len2);
    memcpy(b2, &h2, sizeof h2);
    struct lm_model m2;
    eqi("no-qknorm file: lm_open", lm_open(&m2, b2, len2), 0);
    eqi("no-qknorm file: q_norm and k_norm are NULL",
        (long)(m2.layer[0].q_norm == NULL && m2.layer[0].k_norm == NULL), 1);
    eqz("no-qknorm file: it is exactly 2*head_dim*4*n_layers bytes smaller",
        (unsigned long long)(len - len2), (unsigned long long)(2 * HD * 4 * NL));
    lm_close(&m2);
    free(b); free(b2);
}

/* ======================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    eqi("struct lm_header is still 64 bytes", (long)sizeof(struct lm_header), 64);

#ifdef LMS_GQA_MODULO
    printf("\n*** -DLMS_GQA_MODULO: the reference maps query head h to kv head\n"
           "*** h %% n_kv_heads. The MHA rows MUST stay green and the GQA rows\n"
           "*** MUST redden; anything else means this file is not measuring the\n"
           "*** grouped-query mapping at all.\n\n");
#endif

    printf("--- QK-norm ---\n");
    t_qknorm_ref();
    t_qknorm_order();

    printf("\n--- GQA above 2:1 ---\n");
    int tok[4] = { 3, 1, 0, 7 };
    /* The control for the control: nh == nkvh, where both mappings are the
     * identity. This row must be green in EVERY build. */
    struct cfg mha = { 64, 2, 16, 16, 0, 32, 8, 8, 1 };
    t_forward("MHA 16:16", mha, tok, 4);
    struct cfg g8 = { 64, 2, 16, 8, 0, 32, 8, 8, 1 };
    t_forward("GQA 16:8 (Qwen3-0.6B's ratio)", g8, tok, 4);
    struct cfg g4 = { 64, 2, 16, 4, 0, 32, 8, 8, 1 };
    t_forward("GQA 16:4", g4, tok, 4);
    struct cfg g1 = { 64, 2, 16, 1, 0, 32, 8, 8, 1 };
    t_forward("GQA 16:1 (multi-query)", g1, tok, 4);

    printf("\n--- the format at the target shape ---\n");
    t_format();
    t_qknorm_file();
    t_headdim_open();
    t_qemb();

    printf("\nlmshape_test: %d checks, %d failures\n", checks, failed);
    if (failed) return 1;
    printf("lmshape_test: ALL PASS\n");
    return 0;
}
