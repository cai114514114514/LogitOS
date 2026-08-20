/* lm_infer_test.c -- the forward pass against a second implementation of the
 * same arithmetic, written here, in double, and sharing nothing with it.
 *
 * THE REFERENCE IS THE WHOLE GATE, so what it must NOT be is worth stating
 * first. `ref_logits` below calls nothing in c/lib/nn and nothing in infer.c.
 * It has no KV cache at all: for a query at position `at` it recomputes every
 * key and value from every earlier token from scratch, through the whole
 * stack, every time. That is deliberately the slowest possible way to answer
 * the question, and it is the only way the cache itself gets checked -- a
 * reference that also carried a cache would agree with a cache-indexing bug.
 *
 * WHAT WOULD SILENTLY PASS A WEAKER TEST, and is therefore checked separately:
 *
 *   - the CAUSAL MASK. A pass that attended over the whole cache instead of
 *     0..pos would be correct on a fresh state (the rest of the cache is
 *     zeroed) and wrong on the second conversation. So the test writes
 *     garbage into positions past `pos` and demands the logits not move by a
 *     single bit.
 *   - the LAYER STRIDE of the cache. At n_layers = 1 the term `l * seq_len *
 *     kv_dim` is zero whatever it is, so the shape infer.h's own example uses
 *     cannot see a wrong stride. Every reference check below is run at
 *     n_layers = 2 as well.
 *   - GROUPED-QUERY attention. With n_kv_heads == n_heads the mapping from a
 *     query head to a kv head is the identity, so a missing `h / kv_mul`
 *     divides nothing. One config below has n_kv_heads = 1.
 *   - the NUCLEUS ITSELF. "It returns a token in range" is satisfied by
 *     returning the argmax always. The sampler is checked against the
 *     distribution it claims: 20,000 draws, frequencies within 5 standard
 *     errors, and a token outside the nucleus drawn exactly zero times.
 *   - Q8. Comparing a q8 model against an f32 model only bounds their
 *     difference. The q8 model is additionally compared against the double
 *     reference run on the DEQUANTISED weights, which is a tight check of the
 *     kernel path rather than a loose one of the format.
 *
 *   - a DECOUPLED head_dim, which is the shape the whole line is aimed at and
 *     the one every check above was blind to. Qwen3-0.6B is dim 1024 over 16
 *     query heads of head_dim 128, so q_dim = n_heads*head_dim is 2048 -- and
 *     `dim / n_heads` gives 64, half of it, WITH A ZERO REMAINDER. Every
 *     config in this file used to satisfy head_dim == dim/n_heads, so a
 *     derivation and a header field were indistinguishable and the suite
 *     could not tell an arena sized for one from an arena sized for the
 *     other. t_headdim below is the config that can.
 *
 * THE TOLERANCES ARE DERIVED. An f32 dot product of length k carries a
 * relative error of order k * 2^-24. The pass at these shapes is a chain of
 * about 16 such stages (three matvecs of length dim, one of length hidden,
 * two of length dim into hidden, the per-timestep attention dots, the output
 * head, plus one f32 rounding at each rmsnorm/softmax/swiglu), and the longest
 * single dot is max(dim, hidden, q_dim) -- which is `hidden = 16` at every
 * config this file had when the sentence was written and 2048 at the Qwen3
 * shape. So the compounded relative bound is 16 * max(dim,hidden,q_dim) *
 * 2^-24; `lm_tol()` computes it and returns 16*16*2^-24 -- the original
 * LM_TOL, bit for bit -- for every config that predates the head_dim work.
 * Nothing below is an epsilon chosen because it passed.
 *
 *   cc -Ic/lib/nn -O2 -w -o lmi tests/unit/lm_infer_test.c \
 *      c/lib/nn/infer.c c/lib/nn/model.c c/lib/nn/tensor.c \
 *      c/lib/nn/matmul.c c/lib/nn/ops.c c/lib/nn/quant4.c -lm && ./lmi
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "infer.h"
#include "model.h"
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

/* Deterministic, and not rand(): rand()'s sequence is a libc detail and this
 * file is meant to build against mini-libc as well as against glibc. */
static unsigned long long g_seed = 0x2545F4914F6CDD1DULL;
static double urand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (double)((g_seed >> 11) & 0xFFFFFFFFULL) / 4294967296.0;
}

/* The compounded f32 bound derived in the file header. Stated as the product
 * so the derivation is readable at the point of use.
 *
 * THE SECOND FACTOR IS THE LONGEST DOT, and it is 16 here only because
 * `hidden` was the longest dot in every config this file had. The
 * decoupled-head_dim configs added below break that -- at dim 1024 with
 * head_dim 128 the longest dot is wo's k = q_dim = 2048 -- so `lm_tol()`
 * computes it from the shape and this constant stays as the name of the
 * ORIGINAL derivation. See lm_tol; it returns exactly this value for every
 * config that existed before, so no bound in this file moved. */
#define LM_TOL (16.0 * 16.0 / 16777216.0)

/* ------------------------------------------------------------ the model --
 *
 * A fixture that is the FORMAT, not a struct that happens to feed lm_open: the
 * weights are generated once as f32, and the same weights are then written out
 * twice -- once as an f32 file and once as a q8 one -- so "the same underlying
 * weights" is true by construction rather than by two builders agreeing. */

/* `head_dim` is LAST so every initialiser written before it existed still
 * means what it meant -- C fills the rest with zero, and zero is model.h's own
 * "derive it from dim / n_heads". A field inserted in the middle would have
 * silently renumbered eight configs. */
struct cfg { int dim, n_layers, n_heads, n_kv_heads, hidden, vocab, seq_len, tied,
                 head_dim; };

/* The three numbers the whole of this file's shape arithmetic is made of, in
 * ONE place, because the fixture, the reference, the expected byte total and
 * the tolerance all need them and four copies of `head_dim ? head_dim : dim /
 * n_heads` is four places for the rule to be got wrong -- which is the rule
 * that was got wrong in infer.c. */
static int cfg_hd(const struct cfg *c)
{
    return c->head_dim ? c->head_dim : c->dim / c->n_heads;
}
static int cfg_qd(const struct cfg *c) { return c->n_heads    * cfg_hd(c); }
static int cfg_kv(const struct cfg *c) { return c->n_kv_heads * cfg_hd(c); }

/* The f32 bound of the file header, with its second factor computed from the
 * shape instead of pinned at 16.
 *
 * The stage count is UNCHANGED at 16: it counts the pass's f32 roundings and
 * every config in this file is one or two layers, which is what that 16 was
 * counted for. What moves is the longest single dot product, which the header
 * called "k at most hidden = 16" -- true of the shapes it was written for and
 * false of a decoupled head_dim, where wo's k is q_dim. The dots in a pass are
 * wq/wk/wv/w1/w3 (k = dim), the attention dot (k = head_dim), wo (k = q_dim),
 * w2 (k = hidden) and the head (k = dim), so the longest is
 * max(dim, hidden, q_dim) -- head_dim cannot exceed q_dim.
 *
 * For every config that existed before this change max(dim, hidden, q_dim) is
 * hidden = 16, so this returns 16 * 16 * 2^-24 = LM_TOL exactly. Checked, not
 * asserted in prose: t_layout's first check below compares the two. */
static double lm_tol(const struct cfg *c)
{
    int k = c->dim;
    if (c->hidden > k) k = c->hidden;
    if (cfg_qd(c) > k) k = cfg_qd(c);
    return 16.0 * (double)k / 16777216.0;
}

/* Tensor order is model.h's payload order and nothing else; the indices below
 * are how the reference finds a weight without a name table, which is the same
 * trade model.h makes and for the same reason. */
#define T_EMB          0
#define T_LAY(l)       (1 + (l) * 9)
#define T_AN 0
#define T_WQ 1
#define T_WK 2
#define T_WV 3
#define T_WO 4
#define T_FN 5
#define T_W1 6
#define T_W3 7
#define T_W2 8
#define T_FINAL(c)     (1 + (c)->n_layers * 9)
#define T_CLS(c)       (T_FINAL(c) + 1)

#define MAXT 64

struct fx {
    struct cfg c;
    int    n;
    int    rows[MAXT], cols[MAXT], quant[MAXT];
    float *W[MAXT];                 /* the f32 source of truth */
    float *Q[MAXT];                 /* the same, round-tripped through q8 */
};

static void fx_describe(struct fx *f, const struct cfg *c)
{
    /* model.h's payload shapes, which are stated in terms of q_dim and kv_dim
     * and NOT of dim: wq is [q_dim, dim] and wo is [dim, q_dim]. They collapse
     * to [dim, dim] exactly when head_dim is derived, which is why writing
     * them as `dim` was invisible for as long as it was. */
    int kv = cfg_kv(c), qd = cfg_qd(c);
    int i = 0;
    memset(f, 0, sizeof *f);
    f->c = *c;
#define ADD(r, co, q) do { f->rows[i] = (r); f->cols[i] = (co); f->quant[i] = (q); i++; } while (0)
    ADD(c->vocab, c->dim, 0);                       /* tok_emb, always f32 */
    for (int l = 0; l < c->n_layers; l++) {
        ADD(1, c->dim, 0);                          /* att_norm */
        ADD(qd, c->dim, 1);                         /* wq [q_dim, dim] */
        ADD(kv, c->dim, 1);                         /* wk */
        ADD(kv, c->dim, 1);                         /* wv */
        ADD(c->dim, qd, 1);                         /* wo [dim, q_dim] */
        ADD(1, c->dim, 0);                          /* ffn_norm */
        ADD(c->hidden, c->dim, 1);                  /* w1 */
        ADD(c->hidden, c->dim, 1);                  /* w3 */
        ADD(c->dim, c->hidden, 1);                  /* w2 */
    }
    ADD(1, c->dim, 0);                              /* final_norm */
    if (!c->tied) ADD(c->vocab, c->dim, 1);         /* wcls */
#undef ADD
    f->n = i;
}

static void fx_fill(struct fx *f)
{
    for (int i = 0; i < f->n; i++) {
        size_t ne = (size_t)f->rows[i] * f->cols[i];
        f->W[i] = (float *)malloc(ne * sizeof(float));
        f->Q[i] = (float *)malloc(ne * sizeof(float));
        /* A norm's gain sits near 1 and a weight near 0. That is not
         * cosmetic: gains near zero would collapse the residual stream and
         * make every logit tiny, which is the one input shape where a wrong
         * pass and a right one agree to any tolerance you like. */
        int isnorm = (f->rows[i] == 1);
        for (size_t j = 0; j < ne; j++)
            f->W[i][j] = isnorm ? (float)(0.8 + 0.4 * urand())
                                : (float)(urand() - 0.5);
        if (f->quant[i]) {
            /* The q8 round trip, done with the same quantiser the file writer
             * would use. This is FIXTURE code, not reference code -- the
             * reference never calls it, it only reads the result. */
            int8_t *q = (int8_t *)malloc(ne);
            float  *s = (float *)malloc((size_t)f->rows[i] * sizeof(float));
            nn_quantize_q8(q, s, f->W[i], f->rows[i], f->cols[i]);
            nn_dequantize_q8(f->Q[i], q, s, f->rows[i], f->cols[i]);
            free(q); free(s);
        } else {
            memcpy(f->Q[i], f->W[i], ne * sizeof(float));
        }
    }
}

static void fx_free(struct fx *f)
{
    for (int i = 0; i < f->n; i++) { free(f->W[i]); free(f->Q[i]); }
}

/* Serialise `src` into a LOGITLM blob at the given dtype. `src` is either
 * f->W (the true weights) or f->Q (the same weights after a q8 round trip),
 * which is how a q8 file and the f32 file it should agree with are built from
 * one array. */
static unsigned char *fx_blob(const struct fx *f, int dtype,
                              float * const *src, size_t *out_len)
{
    const struct cfg *c = &f->c;
    size_t len = sizeof(struct lm_header);
    for (int i = 0; i < f->n; i++) {
        size_t ne = (size_t)f->rows[i] * f->cols[i];
        if (dtype == NN_Q8 && f->quant[i]) len += ne + (size_t)f->rows[i] * sizeof(float);
        else                               len += ne * sizeof(float);
    }
    unsigned char *b = (unsigned char *)malloc(len);
    memset(b, 0, len);

    struct lm_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, LM_MAGIC, 8);
    h.version    = LM_VERSION;
    h.dtype      = (uint32_t)dtype;
    h.dim        = (uint32_t)c->dim;
    h.n_layers   = (uint32_t)c->n_layers;
    h.n_heads    = (uint32_t)c->n_heads;
    h.n_kv_heads = (uint32_t)c->n_kv_heads;
    h.hidden     = (uint32_t)c->hidden;
    h.vocab      = (uint32_t)c->vocab;
    h.seq_len    = (uint32_t)c->seq_len;
    h.flags      = c->tied ? LM_TIED : 0u;
    /* 0 when the config derives it, which is what every pre-existing config
     * does -- so the bytes this writes for them are unchanged. */
    h.head_dim   = (uint32_t)c->head_dim;
    memcpy(b, &h, sizeof h);

    size_t off = sizeof h;
    for (int i = 0; i < f->n; i++) {
        size_t ne = (size_t)f->rows[i] * f->cols[i];
        if (dtype == NN_Q8 && f->quant[i]) {
            /* model.h: "[rows*cols int8][rows f32 scales], in that order". */
            nn_quantize_q8((int8_t *)(b + off), (float *)(b + off + ne),
                           src[i], f->rows[i], f->cols[i]);
            off += ne + (size_t)f->rows[i] * sizeof(float);
        } else {
            memcpy(b + off, src[i], ne * sizeof(float));
            off += ne * sizeof(float);
        }
    }
    *out_len = len;
    return b;
}

/* ----------------------------------------------------------- reference --
 *
 * Everything below is double, straight-line, and calls nothing under c/lib.
 * It is the textbook definition in the most obvious order, with no cache and
 * no reuse of anything the implementation does. */

/* The reference's static buffers. FOUR bounds, not one, because a decoupled
 * head_dim makes the widths genuinely different: the residual stream is `dim`
 * wide, the query projection is `q_dim = n_heads*head_dim` wide, and the key
 * and value projections are `kv_dim` wide. They were one constant (R_D 32)
 * while all three were equal.
 *
 * Sized for the largest config in main(): dim 1024, q_dim 2048, kv_dim 1024,
 * hidden 256. R_H is also the LOGIT row (`out` below and every `ref[R_H]` at a
 * call site), so it bounds vocab too -- 32 at that config. Total .bss here is
 * 16*(1024+2048+1024+1024)*8 = 655,360 B, which is why the reference is
 * static: 640 KiB is not a stack frame. */
#define R_T  16     /* positions */
#define R_D  1024   /* dim */
#define R_QD 2048   /* q_dim  = n_heads    * head_dim */
#define R_KV 1024   /* kv_dim = n_kv_heads * head_dim */
#define R_H  256    /* hidden, and the logit row -- so also max vocab */

static double rdot(const float *w, const double *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)w[i] * x[i];
    return s;
}

static void rrms(double *y, const double *x, const float *g, int n)
{
    /* The architecture's eps is 1e-5 and it reaches the f32 pass as a float,
     * so the reference adds the same value a float 1e-5 widens to -- the two
     * differ by 1.2e-14 relative, which is far under LM_TOL but is free to get
     * exactly right and confusing to leave approximate. */
    const double eps = (double)1.0e-5f;
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (double)n + eps;
    double inv = 1.0 / sqrt(ss);
    for (int i = 0; i < n; i++) y[i] = (double)g[i] * (x[i] * inv);
}

static void rrope(double *x, int n, int pos)
{
    /* Interleaved pairs, theta 10000 -- the convention nn.h names. */
    for (int i = 0; i < n / 2; i++) {
        double f = 1.0 / pow(10000.0, (double)(2 * i) / (double)n);
        double a = (double)pos * f, c = cos(a), s = sin(a);
        double x0 = x[2 * i], x1 = x[2 * i + 1];
        x[2 * i]     = x0 * c - x1 * s;
        x[2 * i + 1] = x0 * s + x1 * c;
    }
}

static void ref_logits(const struct fx *f, float * const *W,
                       const int *tok, int at, double *out)
{
    const struct cfg *c = &f->c;
    int D = c->dim, NH = c->n_heads, NKV = c->n_kv_heads;
    /* HD comes from the config's own rule, not from D / NH. Deriving it here
     * would make the reference agree with the bug it exists to catch -- the
     * derivation IS the bug, and a reference that repeats it is not an
     * independent second implementation, it is the same one twice. */
    int HD = cfg_hd(c), QD = cfg_qd(c), KV = cfg_kv(c);
    int HI = c->hidden, VO = c->vocab;
    int mul = NH / NKV, T = at + 1;

    static double x[R_T][R_D], q[R_T][R_QD], k[R_T][R_KV], v[R_T][R_KV];
    double xb[R_D], ao[R_QD], hb[R_H], hb2[R_H], a[R_T];

    for (int t = 0; t < T; t++)
        for (int i = 0; i < D; i++)
            x[t][i] = (double)W[T_EMB][(size_t)tok[t] * D + i];

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
                int off = (h / mul) * HD;
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
            /* wo is [dim, q_dim]: dim output rows, each a dot of length q_dim
             * over the attention output. It reads `dim, dim` exactly when
             * head_dim is derived. */
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
    const float *cls = c->tied ? W[T_EMB] : W[T_CLS(c)];
    for (int i = 0; i < VO; i++) out[i] = rdot(cls + (size_t)i * D, xb, D);
}

/* ------------------------------------------------------------- the tests -- */

/* lm_state_bytes is documented as a pure function of the header, so this
 * builds an lm_model that lm_open never touched and asks it. The total is
 * spelled out as the sum of infer.h's own buffer list, each rounded up to a
 * multiple of four floats -- an independent statement of the layout, not a
 * second call to the function under test. */
static size_t expect_bytes(const struct cfg *c)
{
    int kv = cfg_kv(c), qd = cfg_qd(c);
    /* xb is max(dim, q_dim) and NOT dim, because two producers of different
     * widths write it -- infer.h says so at the field. This line and infer.c's
     * `xbw` are the two independent statements of that; if they disagree, the
     * check below is where it shows. */
    int xbw = qd > c->dim ? qd : c->dim;
    size_t n = 0;
#define PAD(v) n += (((size_t)(v) + 3u) & ~(size_t)3u)
    PAD(c->dim); PAD(xbw); PAD(c->dim);               /* x, xb, xb2   */
    PAD(c->hidden); PAD(c->hidden);                   /* hb, hb2      */
    PAD(qd); PAD(kv); PAD(kv);                        /* q, k, v      */
    PAD(c->n_heads * c->seq_len);                     /* att          */
    PAD(c->vocab);                                    /* logits       */
    PAD(c->n_layers * c->seq_len * kv);               /* kcache       */
    PAD(c->n_layers * c->seq_len * kv);               /* vcache       */
#undef PAD
    return n * sizeof(float);
}

/* Fill an lm_model that lm_open never touched, from a cfg. `m->head_dim` and
 * friends stay ZERO, which is the not-yet-opened case lm_state_bytes is
 * documented to serve -- and, deliberately, the case where infer.c's agreement
 * guard has nothing to compare against and must not fire. */
static void mk_unopened(struct lm_model *m, const struct cfg *c)
{
    memset(m, 0, sizeof *m);
    m->h.dim = (uint32_t)c->dim;             m->h.n_layers = (uint32_t)c->n_layers;
    m->h.n_heads = (uint32_t)c->n_heads;     m->h.n_kv_heads = (uint32_t)c->n_kv_heads;
    m->h.hidden = (uint32_t)c->hidden;       m->h.vocab = (uint32_t)c->vocab;
    m->h.seq_len = (uint32_t)c->seq_len;     m->h.head_dim = (uint32_t)c->head_dim;
}

static void t_layout(void)
{
    /* lm_tol is the file header's derivation with its second factor computed
     * rather than pinned. Asserted rather than claimed in the comment, because
     * "no bound moved" is exactly the kind of statement that is cheap to write
     * and expensive to be wrong about. */
    struct cfg ref_shape = { 8, 1, 2, 2, 16, 8, 8, 1, 0 };
    near("lm_tol at the pre-existing shapes IS the old LM_TOL",
         lm_tol(&ref_shape), LM_TOL, 0.0);

    /* Every buffer of the reference shape is already a multiple of four
     * floats, so it cannot see the rounding. The second config is chosen so
     * that hidden (10), att (2*5) and vocab (6) all need padding and the two
     * caches do not -- if lm_state_bytes and the carve disagree about the
     * rounding, this is where it shows.
     *
     * The last two are DECOUPLED head_dims and they are the point of this
     * function now. `d` has q_dim 12 against a dim of 8, so `q` and `xb` are
     * both WIDER than the residual stream -- that is the direction that used
     * to overrun. `e` has q_dim 4 against a dim of 8, the other direction,
     * where `q` shrinks but `xb` must stay `dim` wide because nn_rmsnorm still
     * fills it: a max() written as a min(), or as plain q_dim, passes `d` and
     * fails `e`. */
    struct cfg a = { 8, 1, 2, 2, 16, 8, 8, 1, 0 };
    struct cfg b = { 8, 1, 2, 2, 10, 6, 5, 1, 0 };
    struct cfg d = { 8, 2, 2, 1, 16, 8, 8, 1, 6 };
    struct cfg e = { 8, 2, 2, 2, 16, 8, 8, 1, 2 };
    struct cfg *cs[4] = { &a, &b, &d, &e };
    for (int i = 0; i < 4; i++) {
        struct lm_model m;
        mk_unopened(&m, cs[i]);
        char what[128];
        /* The [head_dim] tag is what test-lm-infer-negctl counts. Only checks
         * that MUST redden when the derivation is restored carry it, so the
         * control can assert "these and nothing else" by grep rather than by a
         * number somebody keeps in their head. */
        sprintf(what, "lm_state_bytes matches infer.h's buffer list (cfg %d%s)",
                i, cs[i]->head_dim ? ", [head_dim] decoupled" : "");
        eqi(what, (long)lm_state_bytes(&m), (long)expect_bytes(cs[i]));
    }

    /* A header that cannot be honoured must be refused rather than sized. A
     * dim that is not a multiple of n_heads has no head_dim, and returning
     * some number for it would allocate an arena the forward pass then indexes
     * off the end of. */
    struct lm_model bad_m;
    memset(&bad_m, 0, sizeof bad_m);
    bad_m.h.dim = 9; bad_m.h.n_layers = 1; bad_m.h.n_heads = 2;
    bad_m.h.n_kv_heads = 2; bad_m.h.hidden = 16; bad_m.h.vocab = 8;
    bad_m.h.seq_len = 8;
    eqi("lm_state_bytes refuses dim % n_heads != 0", (long)lm_state_bytes(&bad_m), 0);
    eqi("...and says WHY: LM_STATE_E_HEADER", lm_state_why(&bad_m), LM_STATE_E_HEADER);
    /* THE SAME HEADER WITH head_dim SET IS FINE, and this is the half of the
     * rule the old check got backwards. dim 9 over 2 heads has no derived head
     * size -- but a file that STATES head_dim 4 has described a perfectly
     * well-formed shape (q_dim 8, kv_dim 8) over a 9-wide residual stream, and
     * model.h's rule is that divisibility is required only in the derived
     * case. Refusing it would make the field unusable for exactly the models
     * it was added for. */
    bad_m.h.head_dim = 4;
    eqi("[head_dim] an explicit head_dim lifts the dim % n_heads requirement",
        (long)(lm_state_bytes(&bad_m) != 0), 1);
    bad_m.h.head_dim = 0;

    memset(&bad_m, 0, sizeof bad_m);
    eqi("lm_state_bytes refuses an all-zero header", (long)lm_state_bytes(&bad_m), 0);
    eqi("...and says WHY: LM_STATE_E_HEADER", lm_state_why(&bad_m), LM_STATE_E_HEADER);
    eqi("lm_state_why(NULL) is LM_STATE_E_ARG", lm_state_why(NULL), LM_STATE_E_ARG);

    /* lm_state_why and lm_state_bytes must agree about WHETHER, or a caller
     * that branches on one and logs the other reports a refusal for a header
     * that sized fine. */
    struct lm_model okm;
    mk_unopened(&okm, &a);
    eqi("lm_state_why is OK exactly when lm_state_bytes is nonzero",
        (long)(lm_state_why(&okm) == LM_STATE_OK && lm_state_bytes(&okm) != 0), 1);

    /* THE OVERFLOW ARM. An explicit head_dim is an untrusted u32 off a disk:
     * 16 heads of 0x20000000 is 0x2.0000.0000, which wraps to ZERO in 32 bits
     * -- so the old `size_t q_dim = n_heads * hd` on a 32-bit-ish consumer
     * would size every attention buffer at nothing and then index it. The
     * refusal is E_SIZE and not E_HEADER because the header is self-consistent;
     * what does not fit is this machine. */
    struct lm_model big;
    mk_unopened(&big, &a);
    /* SIXTEEN heads, not cfg a's two, and the number matters: 2 * 0x20000000
     * is 0x40000000, which fits an int perfectly well and is refused by
     * nothing -- the first version of this check used cfg a's n_heads and
     * "passed" by sizing an 85,899,346,208-byte arena. It is the PRODUCT that
     * has to leave the range, and 16 * 0x20000000 is 0x2_0000_0000: zero in 32
     * bits, and above INT_MAX in 64.
     *
     * dim is raised to 32 with it so that 32 % 16 == 0. That is not cosmetic:
     * with cfg a's dim of 8 the DERIVED rule refuses this header on
     * divisibility, so the "is it refused" check would pass under the negative
     * control -- refused, yes, for a reason that has nothing to do with the
     * size. Only the WHY check would have reddened, and a control that reddens
     * half a pair reads as a control that is half wrong. */
    big.h.dim = 32; big.h.n_heads = 16; big.h.n_kv_heads = 16;
    big.h.head_dim = 0x20000000u;
    eqi("[head_dim] a head geometry that does not fit an int is refused",
        (long)lm_state_bytes(&big), 0);
    eqi("[head_dim] ...and says WHY: LM_STATE_E_SIZE",
        lm_state_why(&big), LM_STATE_E_SIZE);
}

/* Open a model and its state, run the reference alongside it, and report the
 * worst logit disagreement over `nt` positions. */
static void t_forward(const char *name, struct cfg c, const int *tok, int nt)
{
    struct fx f;
    fx_describe(&f, &c);
    fx_fill(&f);

    size_t blen;
    unsigned char *blob = fx_blob(&f, NN_F32, f.W, &blen);
    struct lm_model m;
    int rc = lm_open(&m, blob, blen);
    char what[128];
    sprintf(what, "%s: lm_open", name);
    eqi(what, rc, 0);
    if (rc) { free(blob); fx_free(&f); return; }

    struct lm_state s;
    rc = lm_state_new(&s, &m);
    sprintf(what, "%s: lm_state_new", name);
    eqi(what, rc, 0);
    if (rc) { lm_close(&m); free(blob); fx_free(&f); return; }

    /* The one allocation, and the number that was promised for it. */
    sprintf(what, "%s: arena_len == lm_state_bytes", name);
    eqi(what, (long)s.arena_len, (long)lm_state_bytes(&m));

    double ref[R_H];
    double worst = 0.0, scale = 1.0;
    int ran = 0;
    for (int t = 0; t < nt; t++) {
        const float *lg = lm_forward(&m, &s, tok[t], t);
        if (!lg) { sprintf(what, "%s: lm_forward(pos %d) returned NULL", name, t);
                   eqi(what, 0, 1); break; }
        ran++;
        ref_logits(&f, f.W, tok, t, ref);
        for (int i = 0; i < c.vocab; i++) {
            double e = fabs((double)lg[i] - ref[i]);
            if (e > worst) worst = e;
            if (fabs(ref[i]) > scale) scale = fabs(ref[i]);
        }
    }
    sprintf(what, "%s: logits match the double reference over %d positions", name, nt);
    printf("      worst |logit - ref| = %.4g over a logit scale of %.4g "
           "(bound %.4g)\n", worst, scale, lm_tol(&c) * scale);
    /* A run that stopped early leaves `worst` at 0 and would PASS this check
     * on no evidence at all -- so the incomplete case is failed explicitly.
     * That is not hypothetical: a refused shape returns NULL from the very
     * first lm_forward, which is exactly what the negative control below
     * produces, and "0 px wrong and failing" is CLAUDE.md's own name for a
     * zero printed in the words of the best outcome. */
    if (ran != nt) {
        char inc[192];
        sprintf(inc, "%s: all %d positions ran (only %d did, so the reference "
                     "check below has no evidence)", name, nt, ran);
        eqi(inc, ran, nt);
        /* FAILED, not evaluated. `worst` is still 0 because no comparison ever
         * ran, and `near(0, 0, bound)` would report the best possible outcome
         * for a pass that did not happen -- CLAUDE.md's "0 px wrong and
         * failing", printed in the words of the best. */
        eqi(what, 0, 1);
    } else {
        near(what, worst, 0.0, lm_tol(&c) * scale);
    }

    lm_state_free(&s);
    sprintf(what, "%s: lm_state_free zeroes the state", name);
    eqi(what, (long)(s.arena == NULL && s.arena_len == 0 && s.pos == 0), 1);
    lm_close(&m);
    free(blob);
    fx_free(&f);
}

/* The causal mask, and it is the only check here that does not need a
 * reference: identical inputs must give identical bits, so the assertion is
 * memcmp and not a tolerance. */
static void t_mask(void)
{
    struct cfg c = { 8, 2, 2, 2, 16, 8, 8, 1 };
    int tok[3] = { 5, 2, 7 };
    struct fx f;
    fx_describe(&f, &c); fx_fill(&f);
    size_t blen; unsigned char *blob = fx_blob(&f, NN_F32, f.W, &blen);
    struct lm_model m; struct lm_state s;
    /* Checked rather than assumed. A test that walks on through a failed
     * constructor reports a SIGSEGV instead of a failed check, and a crash
     * with no output is the hardest kind of failure to read. */
    if (lm_open(&m, blob, blen) || lm_state_new(&s, &m)) {
        eqi("mask: model and state build", 0, 1);
        free(blob); fx_free(&f); return;
    }

    float clean[R_H];
    for (int t = 0; t < 3; t++) {
        const float *lg = lm_forward(&m, &s, tok[t], t);
        if (t == 2) memcpy(clean, lg, (size_t)c.vocab * sizeof(float));
    }

    lm_state_reset(&s);
    eqi("lm_state_reset puts pos back to 0", s.pos, 0);
    for (int t = 0; t < 2; t++) lm_forward(&m, &s, tok[t], t);

    /* Positions 3..7 of every layer's cache, filled with values a real key
     * never takes. If attention ran to seq_len instead of to pos, or if the
     * layer stride were wrong, this moves the logits. */
    int kv = cfg_kv(&c);
    for (int l = 0; l < c.n_layers; l++)
        for (int t = 3; t < c.seq_len; t++)
            for (int i = 0; i < kv; i++) {
                size_t o = ((size_t)l * c.seq_len + t) * kv + i;
                s.kcache[o] = 1e6f + (float)i;
                s.vcache[o] = -1e6f - (float)i;
            }

    const float *lg = lm_forward(&m, &s, tok[2], 2);
    eqi("garbage past pos: lm_forward still returns logits", (long)(lg != NULL), 1);
    eqi("garbage in cache positions 3..7 does not move the logits at pos 2",
        lg ? memcmp(clean, lg, (size_t)c.vocab * sizeof(float)) : 1, 0);

    lm_state_free(&s); lm_close(&m); free(blob); fx_free(&f);
}

static void t_pos(void)
{
    struct cfg c = { 8, 1, 2, 2, 16, 8, 8, 1 };
    struct fx f;
    fx_describe(&f, &c); fx_fill(&f);
    size_t blen; unsigned char *blob = fx_blob(&f, NN_F32, f.W, &blen);
    struct lm_model m; struct lm_state s;
    if (lm_open(&m, blob, blen) || lm_state_new(&s, &m)) {
        eqi("pos: model and state build", 0, 1);
        free(blob); fx_free(&f); return;
    }

    eqi("lm_forward at pos 0 on a fresh state", (long)(lm_forward(&m, &s, 1, 0) != NULL), 1);
    eqi("...advances pos to 1", s.pos, 1);
    eqi("lm_forward refuses a pos behind the state",  (long)(lm_forward(&m, &s, 1, 0) == NULL), 1);
    eqi("lm_forward refuses a pos ahead of the state", (long)(lm_forward(&m, &s, 1, 2) == NULL), 1);
    eqi("a refused pos does not advance the state", s.pos, 1);
    eqi("lm_forward accepts the state's own pos", (long)(lm_forward(&m, &s, 3, 1) != NULL), 1);
    eqi("lm_forward refuses a token outside the vocabulary",
        (long)(lm_forward(&m, &s, c.vocab, 2) == NULL), 1);
    eqi("lm_forward refuses a negative token", (long)(lm_forward(&m, &s, -1, 2) == NULL), 1);

    /* Past the end of the cache. Without this the last write would land one
     * row beyond the arena, which is the classic off-by-one that a seq_len of
     * 256 hides for a long time. */
    for (int t = 2; t < c.seq_len; t++) lm_forward(&m, &s, 2, t);
    eqi("the cache fills to seq_len", s.pos, c.seq_len);
    eqi("lm_forward refuses pos == seq_len",
        (long)(lm_forward(&m, &s, 2, c.seq_len) == NULL), 1);

    /* A state built for one model must not be driven by another. */
    struct lm_model m2; lm_open(&m2, blob, blen);
    eqi("lm_forward refuses a model the state was not built for",
        (long)(lm_forward(&m2, &s, 2, 0) == NULL), 1);
    lm_close(&m2);

    lm_state_free(&s); lm_close(&m); free(blob); fx_free(&f);
}

static void t_greedy(void)
{
    /* The maximum at index 0 and at index n-1. A loop that starts at 1 with
     * best = 0 gets the first for free and can still miss the last; a loop
     * that starts at 0 with best = -1 and `>=` gets the last and misses ties.
     * A middle-index test sees neither. */
    float a[5] = { 9.0f, 1.0f, 2.0f, 3.0f, 4.0f };
    float b[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 9.0f };
    float c[5] = { 1.0f, 9.0f, 2.0f, 3.0f, 4.0f };
    float one[1] = { -3.0f };
    float tie[4] = { 2.0f, 5.0f, 5.0f, 1.0f };
    eqi("greedy finds a maximum at index 0",   lm_sample_greedy(a, 5), 0);
    eqi("greedy finds a maximum at index n-1", lm_sample_greedy(b, 5), 4);
    eqi("greedy finds a maximum in the middle", lm_sample_greedy(c, 5), 1);
    eqi("greedy on n == 1",                    lm_sample_greedy(one, 1), 0);
    eqi("greedy breaks a tie toward the first", lm_sample_greedy(tie, 4), 1);
    eqi("greedy refuses n <= 0",               lm_sample_greedy(a, 0), -1);
    eqi("greedy refuses a NULL row",           lm_sample_greedy(NULL, 5), -1);
}

static void t_topp(void)
{
    /* log p, so a temperature of 1 makes the softmax return p exactly. */
    const double p[4] = { 0.50, 0.25, 0.15, 0.10 };
    float base[4], work[4];
    for (int i = 0; i < 4; i++) base[i] = (float)log(p[i]);

    memcpy(work, base, sizeof work);
    unsigned long long rng = 12345;
    eqi("topp at temp 0 is greedy", lm_sample_topp(work, 4, 0.0f, 0.9f, &rng),
        lm_sample_greedy(base, 4));
    memcpy(work, base, sizeof work);
    eqi("topp at temp 0 leaves the logits alone",
        memcmp(work, base, sizeof work), 0);

    /* Reproducible from a seed: the same seed must give the same sequence, and
     * a different seed must not (otherwise "reproducible" is satisfied by a
     * sampler that ignores the rng entirely). */
    int seqA[16], seqB[16], seqC[16];
    unsigned long long r1 = 99, r2 = 99, r3 = 100;
    for (int i = 0; i < 16; i++) {
        memcpy(work, base, sizeof work); seqA[i] = lm_sample_topp(work, 4, 1.0f, 1.0f, &r1);
        memcpy(work, base, sizeof work); seqB[i] = lm_sample_topp(work, 4, 1.0f, 1.0f, &r2);
        memcpy(work, base, sizeof work); seqC[i] = lm_sample_topp(work, 4, 1.0f, 1.0f, &r3);
    }
    eqi("the same seed gives the same 16 draws", memcmp(seqA, seqB, sizeof seqA), 0);
    eqi("a different seed gives different draws",
        (long)(memcmp(seqA, seqC, sizeof seqA) != 0), 1);

    /* The distribution itself. 20,000 draws at topp = 1: the standard error of
     * a frequency at p = 0.5 is sqrt(0.25/20000) = 0.00354, so the bound is
     * five of those. A sampler that returned the argmax always, or that drew
     * uniformly, misses every one of these by a mile. */
    const int N = 20000;
    int cnt[4] = { 0, 0, 0, 0 };
    unsigned long long r = 0xDEADBEEF;
    for (int i = 0; i < N; i++) {
        memcpy(work, base, sizeof work);
        int t = lm_sample_topp(work, 4, 1.0f, 1.0f, &r);
        if (t < 0 || t > 3) { eqi("topp returned a token out of range", t, 0); break; }
        cnt[t]++;
    }
    for (int i = 0; i < 4; i++) {
        char what[96];
        double se = sqrt(p[i] * (1.0 - p[i]) / (double)N);
        sprintf(what, "topp=1 draws token %d at its own probability (%.2f)", i, p[i]);
        near(what, (double)cnt[i] / N, p[i], 5.0 * se);
    }

    /* TEMPERATURE ACTUALLY DOES SOMETHING. Everything above runs at temp 1 or
     * temp 0, and both are satisfied by an implementation that ignores the
     * parameter -- a mutation that deletes the divide passes every check in
     * this function without it. At temperature T the distribution is
     * p_i^(1/T) renormalised, so T = 0.5 squares it: the four probabilities
     * become [.25 .0625 .0225 .01] / .345. */
    {
        double q[4], sum = 0.0;
        for (int i = 0; i < 4; i++) { q[i] = p[i] * p[i]; sum += q[i]; }
        int c3[4] = { 0, 0, 0, 0 };
        unsigned long long rr = 0xC0FFEE;
        for (int i = 0; i < N; i++) {
            memcpy(work, base, sizeof work);
            c3[lm_sample_topp(work, 4, 0.5f, 1.0f, &rr)]++;
        }
        for (int i = 0; i < 4; i++) {
            char what[96];
            double e = q[i] / sum;
            double se = sqrt(e * (1.0 - e) / (double)N);
            sprintf(what, "temp=0.5 squares the distribution at token %d (%.4f)", i, e);
            near(what, (double)c3[i] / N, e, 5.0 * se);
        }
    }

    /* The nucleus. 0.50 + 0.25 = 0.75 < 0.8, so the smallest prefix with mass
     * >= 0.8 is the top THREE; token 3 must never be drawn, and the other
     * three must appear at p/0.90. */
    int c2[4] = { 0, 0, 0, 0 };
    r = 0x1234567;
    for (int i = 0; i < N; i++) {
        memcpy(work, base, sizeof work);
        c2[lm_sample_topp(work, 4, 1.0f, 0.8f, &r)]++;
    }
    eqi("topp=0.8 never draws the token outside the nucleus", c2[3], 0);
    for (int i = 0; i < 3; i++) {
        char what[96];
        double q = p[i] / 0.90;
        double se = sqrt(q * (1.0 - q) / (double)N);
        sprintf(what, "topp=0.8 renormalises token %d to %.4f", i, q);
        near(what, (double)c2[i] / N, q, 5.0 * se);
    }

    /* A nucleus so tight only the top token fits is greedy, by every draw. */
    r = 7; int allmax = 1;
    for (int i = 0; i < 64; i++) {
        memcpy(work, base, sizeof work);
        if (lm_sample_topp(work, 4, 1.0f, 0.0001f, &r) != 0) allmax = 0;
    }
    eqi("a nucleus of one is greedy on every draw", allmax, 1);
    eqi("topp refuses n <= 0", lm_sample_topp(work, 0, 1.0f, 0.9f, &r), -1);
    eqi("topp refuses a NULL rng when it needs one",
        lm_sample_topp(work, 4, 1.0f, 0.9f, NULL), -1);
}

/* ------------------------------------------------- the decoupled head_dim --
 *
 * THE ONE PROPERTY THAT WAS FALSE AND NOTHING CAUGHT: `lm_state_bytes` is
 * documented as "exactly how many bytes lm_state_new will ask for", and at a
 * decoupled head_dim it was not -- it reported an arena sized from
 * dim/n_heads while the forward pass wrote n_heads*head_dim. Every config in
 * this file made those two numbers equal, so the claim was untested at the one
 * shape that can distinguish them.
 *
 * Two equalities are asserted here and they are NOT the same statement:
 *
 *   - arena_len == lm_state_bytes. True BY CONSTRUCTION (layout() runs twice),
 *     so it stays green even under the negative control, and it is here to say
 *     so: it is the claim infer.c's own header makes, and a check that cannot
 *     redden is a check that must be shown not to be the evidence.
 *   - lm_state_bytes == expect_bytes. INDEPENDENT -- expect_bytes is written
 *     from infer.h's field list in this file and calls nothing under test.
 *     This is the one that was false, and it lives in t_layout above where the
 *     four configs are.
 *
 * The geometry is Qwen3-0.6B's, cut where cutting is free: dim 1024, 16 query
 * heads over 8 kv heads (GQA 2:1), head_dim 128 -- so q_dim 2048 against a
 * 1024-wide residual stream, and kv_dim 1024. n_layers is 1 rather than 28
 * because the weights are 28 MB a layer at this width and the double reference
 * recomputes every earlier position from scratch; the LAYER STRIDE at a
 * decoupled kv_dim is covered instead by cfg `d` in main(), which is eight
 * floats wide and two layers deep. */
static void t_headdim(void)
{
    struct cfg c = { 1024, 1, 16, 8, 256, 32, 4, 1, 128 };
    int tok[2] = { 7, 19 };

    eqi("[head_dim] the fixture's q_dim is 2048, not dim", cfg_qd(&c), 2048);
    eqi("[head_dim] the fixture's kv_dim is 1024", cfg_kv(&c), 1024);
    eqi("[head_dim] dim / n_heads would have said 64", c.dim / c.n_heads, 64);

    /* The sizing question answered BEFORE anything is allocated, which is what
     * infer.h says lm_state_bytes is for. */
    struct lm_model unopened;
    mk_unopened(&unopened, &c);
    size_t predicted = lm_state_bytes(&unopened);
    eqi("[head_dim] lm_state_bytes on an unopened header matches the buffer list",
        (long)predicted, (long)expect_bytes(&c));

    struct fx f;
    fx_describe(&f, &c);
    fx_fill(&f);
    size_t blen;
    unsigned char *blob = fx_blob(&f, NN_F32, f.W, &blen);

    struct lm_model m;
    int rc = lm_open(&m, blob, blen);
    eqi("[head_dim] lm_open accepts a file with head_dim 128 over dim 1024", rc, 0);
    if (rc) { free(blob); fx_free(&f); return; }

    /* The loader's geometry, which is the one the WEIGHT TENSORS were shaped
     * by. infer.c's guard compares its own derivation against these three. */
    eqi("[head_dim] lm_open reports head_dim 128", m.head_dim, 128);
    eqi("[head_dim] lm_open reports q_dim 2048",   m.q_dim,    2048);
    eqi("[head_dim] lm_open reports kv_dim 1024",  m.kv_dim,   1024);

    struct lm_state s;
    rc = lm_state_new(&s, &m);
    eqi("[head_dim] lm_state_new on the opened model", rc, LM_STATE_OK);
    if (rc) { lm_close(&m); free(blob); fx_free(&f); return; }

    /* THE EQUALITY. Both directions: what was promised before opening, what is
     * promised now, and what was actually allocated. */
    eqi("[head_dim] arena_len == lm_state_bytes", (long)s.arena_len,
        (long)lm_state_bytes(&m));
    eqi("[head_dim] ...and == what the unopened header predicted",
        (long)s.arena_len, (long)predicted);
    printf("      arena %zu B = %.2f KiB at dim 1024 / 16 heads / head_dim 128 "
           "(the derivation would have said %zu B)\n",
           s.arena_len, (double)s.arena_len / 1024.0,
           expect_bytes(&(struct cfg){ 1024, 1, 16, 8, 256, 32, 4, 1, 0 }));

    /* THE GUARD. Two independently maintained numbers, one equality -- so a
     * model whose loader geometry does not match what this header says is
     * REFUSED rather than sized for one shape and written by the other. The
     * only way to build that disagreement is to corrupt one side by hand,
     * which is the point: it is a BUG in one of the two, not a bad file, and
     * before the fix it was silent. */
    struct lm_model skew = m;
    skew.head_dim = 64; skew.q_dim = 1024; skew.kv_dim = 512;
    eqi("[head_dim] a loader/sizer geometry disagreement sizes nothing",
        (long)lm_state_bytes(&skew), 0);
    eqi("[head_dim] ...and says WHY: LM_STATE_E_GEOM",
        lm_state_why(&skew), LM_STATE_E_GEOM);
    struct lm_state junk;
    int jrc = lm_state_new(&junk, &skew);
    eqi("[head_dim] ...and lm_state_new returns the same code",
        jrc, LM_STATE_E_GEOM);
    /* Under the negative control this call SUCCEEDS -- that is the failure
     * being demonstrated -- so the arena it allocated is released rather than
     * leaked out of a suite that is expected to be run under ASan one day. */
    if (jrc == LM_STATE_OK) lm_state_free(&junk);

    /* THE FORWARD PASS, against the double reference. Two positions: pos 0 and
     * pos 1, so RoPE runs at a nonzero position and attention runs over more
     * than one timestep. */
    double ref[R_H];
    double worst = 0.0, scale = 1.0;
    int ran = 0;
    for (int t = 0; t < 2; t++) {
        const float *lg = lm_forward(&m, &s, tok[t], t);
        if (!lg) break;
        ran++;
        ref_logits(&f, f.W, tok, t, ref);
        for (int i = 0; i < c.vocab; i++) {
            double e = fabs((double)lg[i] - ref[i]);
            if (e > worst) worst = e;
            if (fabs(ref[i]) > scale) scale = fabs(ref[i]);
        }
    }
    eqi("[head_dim] lm_forward runs at dim 1024 / 16 heads / head_dim 128", ran, 2);
    printf("      worst |logit - ref| = %.4g over a logit scale of %.4g "
           "(bound %.4g)\n", worst, scale, lm_tol(&c) * scale);
    if (ran == 2)
        near("[head_dim] logits match the double reference at the decoupled shape",
             worst, 0.0, lm_tol(&c) * scale);
    else
        /* Not `near(worst=0)`, which would PASS on a run that never happened. */
        eqi("[head_dim] logits match the double reference at the decoupled shape "
            "(NOT RUN -- lm_forward refused the shape)", 0, 1);

    lm_state_free(&s);
    lm_close(&m);
    free(blob);
    fx_free(&f);
}

static void t_q8(void)
{
    struct cfg c = { 8, 2, 2, 2, 16, 8, 8, 1 };
    int tok[4] = { 4, 0, 6, 1 };
    struct fx f;
    fx_describe(&f, &c); fx_fill(&f);

    size_t l32, l8;
    unsigned char *b32 = fx_blob(&f, NN_F32, f.W, &l32);
    unsigned char *b8  = fx_blob(&f, NN_Q8,  f.W, &l8);

    struct lm_model m32, m8;
    int r1 = lm_open(&m32, b32, l32), r2 = lm_open(&m8, b8, l8);
    eqi("q8: both files open", (long)(r1 == 0 && r2 == 0), 1);
    if (r1 || r2) { free(b32); free(b8); fx_free(&f); return; }

    struct lm_state s32, s8;
    if (lm_state_new(&s32, &m32) || lm_state_new(&s8, &m8)) {
        eqi("q8: both states build", 0, 1);
        lm_close(&m32); lm_close(&m8); free(b32); free(b8); fx_free(&f); return;
    }

    double refW[R_H], refQ[R_H];
    double d_kernel = 0.0;   /* q8 model vs the double reference on the
                              * DEQUANTISED weights -- a tight check that the
                              * quantised kernel path computes what it claims */
    double d_quant  = 0.0;   /* the double reference on W vs on Q -- the cost
                              * of the format itself, with no f32 in it */
    double d_models = 0.0;   /* what a caller actually sees */
    double scale = 1.0;

    for (int t = 0; t < 4; t++) {
        const float *g32 = lm_forward(&m32, &s32, tok[t], t);
        const float *g8  = lm_forward(&m8,  &s8,  tok[t], t);
        if (!g32 || !g8) { eqi("q8: both models ran", 0, 1); break; }
        ref_logits(&f, f.W, tok, t, refW);
        ref_logits(&f, f.Q, tok, t, refQ);
        for (int i = 0; i < c.vocab; i++) {
            double a = fabs((double)g8[i] - refQ[i]);
            double b = fabs(refW[i] - refQ[i]);
            double e = fabs((double)g8[i] - (double)g32[i]);
            if (a > d_kernel) d_kernel = a;
            if (b > d_quant)  d_quant  = b;
            if (e > d_models) d_models = e;
            if (fabs(refW[i]) > scale) scale = fabs(refW[i]);
        }
    }

    printf("      q8 cost: |q8 - f32 logits| = %.4g, |ref(W) - ref(Wq)| = %.4g,\n"
           "               |q8 - ref(Wq)| = %.4g, logit scale %.4g\n",
           d_models, d_quant, d_kernel, scale);

    /* The q8 kernel is held to the SAME f32 bound as the f32 one, because
     * against the dequantised weights it is doing the same arithmetic in a
     * different order -- nn_matvec_q8 accumulates q.x and scales once, which
     * differs from summing (q*scale).x only in rounding. A loose bound here
     * would let a genuinely wrong quantised path hide inside "quantisation
     * error". */
    near("q8 logits match the double reference on the dequantised weights",
         d_kernel, 0.0, lm_tol(&c) * scale);

    /* And the number a caller sees is bounded by the two above rather than by
     * a figure chosen to pass: |q8 - f32| <= |ref(W) - ref(Wq)| + both models'
     * own f32 error. */
    near("q8 and f32 differ by the quantisation error and no more",
         d_models, 0.0, d_quant + 2.0 * lm_tol(&c) * scale);

    lm_state_free(&s32); lm_state_free(&s8);
    lm_close(&m32); lm_close(&m8);
    free(b32); free(b8); fx_free(&f);
}

int main(void)
{
    /* Line buffered even when piped: this suite drives an implementation that
     * can fault, and a fully-buffered stdout loses every check that passed
     * before the fault -- which is the difference between "it crashed after
     * the mask check" and "it crashed". */
    setvbuf(stdout, NULL, _IOLBF, 0);

    eqi("struct lm_header is 64 bytes", (long)sizeof(struct lm_header), 64);

    t_layout();

    /* infer.h's own example shape, one layer. */
    int tokA[4] = { 3, 1, 0, 7 };
    struct cfg a = { 8, 1, 2, 2, 16, 8, 8, 1 };
    t_forward("1 layer, MHA", a, tokA, 4);

    /* Two layers, because at one the cache's layer stride is multiplied by
     * zero and any value for it passes. */
    struct cfg b = { 8, 2, 2, 2, 16, 8, 8, 1 };
    t_forward("2 layers, MHA", b, tokA, 4);

    /* Grouped-query: two query heads over one kv head, so h / kv_mul stops
     * being the identity and kv_dim stops being dim. */
    struct cfg g = { 8, 2, 2, 1, 16, 8, 8, 1 };
    t_forward("2 layers, GQA 2:1", g, tokA, 4);

    /* An untied output head, so wcls is a tensor of its own rather than a view
     * of the embedding -- a different code path in the loader and in the last
     * matvec. */
    struct cfg u = { 8, 1, 2, 2, 16, 8, 8, 0 };
    t_forward("1 layer, untied head", u, tokA, 4);

    /* DECOUPLED head_dim at a shape small enough to run everything the big one
     * cannot afford: two layers (so the cache's layer stride is multiplied by
     * a NONZERO l at a kv_dim that is not dim/n_heads) and GQA 2:1. head_dim 6
     * over dim 8 puts q_dim at 12, WIDER than the residual stream -- the
     * direction that overran the arena. */
    struct cfg dq = { 8, 2, 2, 1, 16, 8, 8, 1, 6 };
    t_forward("2 layers, GQA 2:1, [head_dim] 6 over dim 8 (q_dim 12)", dq, tokA, 4);

    /* And the OTHER direction, which a max() written as a plain q_dim would
     * pass the first config and fail here: head_dim 2 puts q_dim at 4, NARROWER
     * than dim, and `xb` must still be dim wide because nn_rmsnorm fills it. */
    struct cfg ds = { 8, 2, 2, 2, 16, 8, 8, 1, 2 };
    t_forward("2 layers, [head_dim] 2 over dim 8 (q_dim 4, narrower)", ds, tokA, 4);

    t_headdim();

    t_mask();
    t_pos();
    t_greedy();
    t_topp();
    t_q8();

    printf("\nlm_infer_test: %d checks, %d failures\n", checks, failed);
    if (failed) return 1;
    printf("lm_infer_test: ALL PASS\n");
    return 0;
}
