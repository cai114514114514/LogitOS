/* tools/lmtrain.c -- the trainer for LOGITLM. HOST ONLY, and never linked into
 * anything that boots.
 *
 *     cc -O2 -o build/lmtrain tools/lmtrain.c -lm
 *     build/lmtrain --gradcheck
 *     build/lmtrain --corpus CLAUDE.md --out build/model.lm --steps 3000
 *
 * ---------------------------------------------------------------------------
 * WHY C AND NOT PYTHON. A trainer written against numpy/torch is a trainer
 * that does not run here: this repo vendors neither, the host is not
 * guaranteed to have either, and a build step that begins with `pip install`
 * is a build step that fails on somebody else's machine six months from now.
 * The whole model is 825k parameters and the corpus is 126 KB; that is a size
 * where scalar C with -O2 is not a compromise, it is simply enough. The
 * measured cost is printed by the trainer itself as tokens/s, so nobody has to
 * take this paragraph on trust.
 *
 * ---------------------------------------------------------------------------
 * EVERYTHING HERE IS DOUBLE, INCLUDING THE FORWARD PASS, AND THAT IS THE
 * GRADIENT CHECK'S DOING -- not caution. The check compares an analytic
 * gradient against (L(w+e) - L(w-e)) / 2e at e = 1e-4. In f32 the loss carries
 * a relative rounding error near 6e-8, so the numerator would be a difference
 * of two numbers that agree to within 1e-6 while the signal (2e * dL/dw) is of
 * order 2e-5 for a typical weight -- the check would be measuring f32 noise
 * and would pass or fail at random. In f64 the same numerator is exact to ~2e-16
 * relative and the check measures the gradient. Running the trainer itself in
 * f32 and the check in f64 would mean the check tests a SECOND implementation,
 * which is exactly the arrangement that lets a transposed gradient survive.
 *
 * The cost of that choice is real and is paid once: doubles halve the SSE2 lane
 * count, so the forward pass is roughly 2x slower than the f32 one would be.
 * The weights are rounded to f32 exactly once, in the writer, which is also
 * where a Q8 file quantises -- so what ships is f32 or int8 and what trains is
 * f64, and the difference between them is measured (see "q8 cost" below) rather
 * than assumed to be small.
 *
 * ---------------------------------------------------------------------------
 * THE FORMAT COMES FROM model.h AND THE QUANTISER FROM matmul.c, BY INCLUSION.
 * `struct lm_header` is not retyped here and `nn_quantize_q8` is not
 * reimplemented here. A writer that carries its own copy of a format is a
 * writer that drifts from the reader the first time somebody adds a field, and
 * a quantiser that carries its own copy of the rounding rule produces a file
 * whose weights differ from what the device would have produced from the same
 * floats -- silently, in the last bit, on every row. Including the real ones
 * costs a relative path and removes both failure modes. (`#include "nn.h"`
 * inside matmul.c resolves against matmul.c's own directory, which is why this
 * needs no -I.)
 *
 * ---------------------------------------------------------------------------
 * ARCHITECTURE, fixed and identical to c/lib/nn's: llama-shaped decoder-only,
 * NO biases anywhere. RMSNorm(eps 1e-5) -> attention -> residual -> RMSNorm ->
 * SwiGLU MLP -> residual. RoPE theta=10000 over INTERLEAVED pairs
 * (x[2i], x[2i+1]) -- the RoFormer/llama2.c convention, not huggingface's
 * split-half; nn_rope() implements the same one and the two are not
 * interchangeable. Causal attention, scale 1/sqrt(head_dim). Output head tied
 * to the token embedding by default. Byte-level vocabulary: 256 tokens, token
 * == byte, so there is no tokenizer file -- which matters because that would be
 * a second inode and this filesystem has about 33 left.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* OPENMP IS OPTIONAL AND THE FILE IS CORRECT WITHOUT IT. Every `#pragma omp`
 * below is ignored by a compiler invoked without -fopenmp, and `_OPENMP` is
 * the macro that compiler does not define -- so `cc -O2 tools/lmtrain.c -lm`
 * (what tests/nn.mk builds for --gradcheck, and what a host without OpenMP
 * gets) compiles to the single-threaded program this file used to be. The
 * threaded build is a different command line, not a different source file:
 *     cc -O3 -march=native -fopenmp -o build/lmtrain-fast tools/lmtrain.c -lm
 * The two agree on the gradient check, which is the only thing that would tell
 * them apart. */
#ifdef _OPENMP
#include <omp.h>
#endif

#include "../c/lib/nn/model.h"      /* struct lm_header, LM_MAGIC/VERSION/TIED */
#include "../c/lib/nn/matmul.c"     /* nn_quantize_q8, nn_dequantize_q8        */

/* The header is a 64-byte on-disk contract; a compiler that padded it would
 * produce files the loader cannot read, so say so at compile time rather than
 * discover it from a device that will not boot. */
typedef char lm_header_is_64[(sizeof(struct lm_header) == 64) ? 1 : -1];

#define RMS_EPS   1e-5
#define ROPE_BASE 10000.0

static void die(const char *msg)
{
    fprintf(stderr, "lmtrain: %s\n", msg);
    exit(1);
}

/* Wall clock in seconds. See the tok/s column's comment for why clock() is
 * the wrong one here. */
static double wall_s(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    return (double)time(NULL);
}

static void *xalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz);
    if (!p) die("out of memory");
    return p;
}

/* ------------------------------------------------------------------- rng --
 *
 * xorshift64* rather than rand(): rand()'s sequence is a property of the host
 * libc, so a run would not be reproducible from its seed on a different
 * machine -- and an initialisation nobody can reproduce makes a training curve
 * impossible to bisect. */
static unsigned long long g_rng = 1234;

static double urand(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    unsigned long long r = g_rng * 2685821657736338717ULL;
    /* 53 bits is exactly the f64 mantissa: taking more would set bits the
     * conversion then rounds away, biasing the low end. */
    return (double)(r >> 11) * (1.0 / 9007199254740992.0);
}

static double nrand(void)
{
    /* Box-Muller. The log's argument is taken from (0,1] by construction --
     * urand() can return 0 and log(0) is -inf, which becomes a NaN weight that
     * poisons the whole run without an error anywhere. */
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* ------------------------------------------------------- the parameter map --
 *
 * ONE FLAT ARRAY AND A TABLE OVER IT, rather than a struct of named pointers.
 * Five things walk every parameter -- init, the gradient clip, AdamW, the
 * gradient check's per-tensor report, and the file writer -- and with named
 * pointers each of them is a list that can silently omit a tensor. Here the
 * table is the list, the order IS model.h's payload order, and the writer is a
 * loop rather than a transcription. */
enum { PK_NORM = 0, PK_EMB = 1, PK_MAT = 2 };

struct pdesc {
    char   name[24];
    size_t off;                 /* into the flat parameter array */
    int    rows, cols;
    int    kind;
};

/* Per-layer tensor slots, in model.h's order. */
enum { L_AN = 0, L_WQ, L_WK, L_WV, L_WO, L_FN, L_W1, L_W3, L_W2, L_PER };

struct cfg {
    int dim, n_layers, n_heads, n_kv_heads, head_dim, kv_dim, hidden, vocab, seq_len;
    int tied;
};

#define T_TOK          0
#define T_L(l, i)      (1 + (l) * L_PER + (i))
#define T_FINAL(c)     (1 + (c)->n_layers * L_PER)
#define T_WCLS(c)      (1 + (c)->n_layers * L_PER + 1)
#define N_TENS(c)      (1 + (c)->n_layers * L_PER + 1 + ((c)->tied ? 0 : 1))

static int cfg_derive(struct cfg *c)
{
    if (c->dim <= 0 || c->n_layers <= 0 || c->n_heads <= 0 || c->n_kv_heads <= 0 ||
        c->hidden <= 0 || c->vocab <= 0 || c->seq_len <= 0) return 0;
    if (c->dim % c->n_heads) return 0;
    if (c->n_heads % c->n_kv_heads) return 0;
    c->head_dim = c->dim / c->n_heads;
    if (c->head_dim % 2) return 0;          /* RoPE rotates pairs */
    c->kv_dim = c->n_kv_heads * c->head_dim;
    return 1;
}

static void desc_add(struct pdesc *d, int *n, size_t *off, const char *name,
                     int rows, int cols, int kind)
{
    struct pdesc *p = &d[*n];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->off = *off; p->rows = rows; p->cols = cols; p->kind = kind;
    *off += (size_t)rows * cols;
    (*n)++;
}

static int build_desc(const struct cfg *c, struct pdesc *d, size_t *n_param)
{
    char nm[24];
    int n = 0; size_t off = 0;
    desc_add(d, &n, &off, "tok_emb", c->vocab, c->dim, PK_EMB);
    for (int l = 0; l < c->n_layers; l++) {
        snprintf(nm, sizeof nm, "L%d.att_norm", l); desc_add(d, &n, &off, nm, 1, c->dim, PK_NORM);
        snprintf(nm, sizeof nm, "L%d.wq", l);       desc_add(d, &n, &off, nm, c->dim, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.wk", l);       desc_add(d, &n, &off, nm, c->kv_dim, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.wv", l);       desc_add(d, &n, &off, nm, c->kv_dim, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.wo", l);       desc_add(d, &n, &off, nm, c->dim, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.ffn_norm", l); desc_add(d, &n, &off, nm, 1, c->dim, PK_NORM);
        snprintf(nm, sizeof nm, "L%d.w1", l);       desc_add(d, &n, &off, nm, c->hidden, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.w3", l);       desc_add(d, &n, &off, nm, c->hidden, c->dim, PK_MAT);
        snprintf(nm, sizeof nm, "L%d.w2", l);       desc_add(d, &n, &off, nm, c->dim, c->hidden, PK_MAT);
    }
    desc_add(d, &n, &off, "final_norm", 1, c->dim, PK_NORM);
    if (!c->tied) desc_add(d, &n, &off, "wcls", c->vocab, c->dim, PK_MAT);
    *n_param = off;
    return n;
}

/* --------------------------------------------------------------- kernels --
 *
 * Three shapes of linear algebra and nothing else. Every one keeps its inner
 * loop contiguous along k so -O2 can vectorise it; the i-then-j orderings that
 * would walk a column instead are the difference between a cache line per
 * element and a cache line per sixteen. */

/* EVERY ONE OF THE THREE IS BLOCKED FOUR TOKENS AT A TIME, and the reason is
 * memory traffic and not instruction count. The natural writing of each loop
 * puts `t` outermost, which streams the whole weight matrix once PER TOKEN:
 * at dim=256 that is a 512 KB matrix pulled 256 times per call, ~134 MB, for
 * 16.8 MFLOP of work -- 8 bytes moved per flop, which is a bandwidth test
 * with a matmul attached. Holding four token rows at once divides every one of
 * those passes by four and costs four accumulators instead of one, and four
 * independent accumulator chains is also exactly what the FMA latency wants.
 * MEASURED BOTH WAYS, because the answer depends on the flags and the smaller
 * of the two numbers is the one that would have talked somebody out of doing
 * it. dim=256/L=6/seq=256, one thread, tok/s, against the same file with these
 * four loops reverted:
 *     cc -O2                          304 -> 349   (+15%)
 *     -O3 -march=native -fassoc...    500 -> 802   (+60%)
 * At -O2 the inner loop is four SCALAR FMAs and is compute-bound, so cutting
 * memory traffic buys little. Vectorised it is bandwidth-bound and blocking is
 * most of the win. The full ladder is in tools/lmtrain.md.
 *
 * WHAT IT COSTS: the four-accumulator dot product of the old lin_fwd summed
 * (s0+s1)+(s2+s3) over j-strided partials; these sum straight along j. Both
 * are correct and neither is more so -- but they round differently, so a model
 * trained before this change and one trained after are not bit-identical from
 * the same seed. Nothing downstream depends on that (the gates compare the
 * TRAINER to the ENGINE on ONE set of weights, not two training runs to each
 * other); it is recorded because "same seed, same bytes" is a property people
 * assume without checking. */
#define LB 4                        /* tokens per block. 8 spills on AVX2. */

/* Y[T,n] = X[T,k] . W[n,k]^T   (W is stored out-features x in-features, which
 * is the layout LOGITLM defines and the layout nn_matvec_f32 consumes). */
static void lin_fwd(double *y, const double *x, const double *w, int T, int n, int k)
{
    int t = 0;
    for (; t + LB - 1 < T; t += LB) {
        const double *x0 = x + (size_t)t * k, *x1 = x0 + k, *x2 = x1 + k, *x3 = x2 + k;
        double *y0 = y + (size_t)t * n, *y1 = y0 + n, *y2 = y1 + n, *y3 = y2 + n;
        for (int i = 0; i < n; i++) {
            const double *wr = w + (size_t)i * k;
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            for (int j = 0; j < k; j++) {
                double wv = wr[j];
                s0 += wv * x0[j]; s1 += wv * x1[j];
                s2 += wv * x2[j]; s3 += wv * x3[j];
            }
            y0[i] = s0; y1[i] = s1; y2[i] = s2; y3[i] = s3;
        }
    }
    for (; t < T; t++) {
        const double *xr = x + (size_t)t * k;
        double *yr = y + (size_t)t * n;
        for (int i = 0; i < n; i++) {
            const double *wr = w + (size_t)i * k;
            double s = 0;
            for (int j = 0; j < k; j++) s += wr[j] * xr[j];
            yr[i] = s;
        }
    }
}

/* dX += dY . W  and  dW += dY^T . X. Both accumulate: a layer's input grad
 * arrives from more than one consumer (the MLP's gate and up projections share
 * their input), so an assigning version would silently drop one of them. The
 * caller zeroes dX.
 *
 * TWO SEPARATE LOOP NESTS, where this used to be one fused pass over W's rows.
 * Fusing them looks like a saving -- one trip over the matrix instead of two --
 * and is the opposite: the fused version has BOTH W and dW resident at once
 * (1 MB at dim=256, over this machine's 2 MB L2 once the activations are
 * counted) and repeats that per token. Split, each nest keeps one matrix warm
 * and four token rows (8 KB) in L1. */
static void lin_bwd(double *dx, double *dw, const double *dy, const double *x,
                    const double *w, int T, int n, int k)
{
    int t = 0;
    for (; t + LB - 1 < T; t += LB) {
        const double *d0 = dy + (size_t)t * n, *d1 = d0 + n, *d2 = d1 + n, *d3 = d2 + n;
        double *o0 = dx + (size_t)t * k, *o1 = o0 + k, *o2 = o1 + k, *o3 = o2 + k;
        for (int i = 0; i < n; i++) {
            double a0 = d0[i], a1 = d1[i], a2 = d2[i], a3 = d3[i];
            const double *wr = w + (size_t)i * k;
            for (int j = 0; j < k; j++) {
                double wv = wr[j];
                o0[j] += a0 * wv; o1[j] += a1 * wv;
                o2[j] += a2 * wv; o3[j] += a3 * wv;
            }
        }
    }
    for (; t < T; t++) {
        const double *dyr = dy + (size_t)t * n;
        double *dxr = dx + (size_t)t * k;
        for (int i = 0; i < n; i++) {
            double d = dyr[i];
            const double *wr = w + (size_t)i * k;
            for (int j = 0; j < k; j++) dxr[j] += d * wr[j];
        }
    }
    t = 0;
    for (; t + LB - 1 < T; t += LB) {
        const double *x0 = x + (size_t)t * k, *x1 = x0 + k, *x2 = x1 + k, *x3 = x2 + k;
        const double *d0 = dy + (size_t)t * n, *d1 = d0 + n, *d2 = d1 + n, *d3 = d2 + n;
        for (int i = 0; i < n; i++) {
            double a0 = d0[i], a1 = d1[i], a2 = d2[i], a3 = d3[i];
            double *dwr = dw + (size_t)i * k;
            for (int j = 0; j < k; j++)
                dwr[j] += a0 * x0[j] + a1 * x1[j] + a2 * x2[j] + a3 * x3[j];
        }
    }
    for (; t < T; t++) {
        const double *xr = x + (size_t)t * k;
        const double *dyr = dy + (size_t)t * n;
        for (int i = 0; i < n; i++) {
            double d = dyr[i];
            double *dwr = dw + (size_t)i * k;
            for (int j = 0; j < k; j++) dwr[j] += d * xr[j];
        }
    }
}

/* RMS norm. Returns the 1/rms factor, which the backward pass needs and which
 * is cheaper to keep than to recompute (it costs one double per token per norm
 * against a second pass over the whole vector). */
static double rms_fwd(double *y, const double *x, const double *g, int n)
{
    double ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    double r = 1.0 / sqrt(ss / (double)n + RMS_EPS);
    for (int i = 0; i < n; i++) y[i] = g[i] * x[i] * r;
    return r;
}

/* y_i = g_i x_i r,  r = (mean(x^2)+eps)^(-1/2),  so dr/dx_k = -r^3 x_k / n and
 *   dL/dx_k = r g_k dy_k - (r^3 x_k / n) * sum_i dy_i g_i x_i.
 * The second term is the whole reason RMSNorm's backward is not elementwise;
 * dropping it leaves a gradient that still points downhill (the first term
 * dominates) and trains to a worse place -- which is the failure the gradient
 * check exists to catch, because nothing else would. */
static void rms_bwd(double *dx, double *dg, const double *dy, const double *x,
                    const double *g, double r, int n)
{
    double S = 0;
    for (int i = 0; i < n; i++) S += dy[i] * g[i] * x[i];
    double c = r * r * r * S / (double)n;
#ifdef LMTRAIN_NO_RMS_JACOBIAN
    /* NEGATIVE CONTROL: treat r as a constant, i.e. forget that the norm's
     * scale depends on its own input. This is the most plausible wrong
     * RMSNorm backward there is -- it is what you get by differentiating the
     * line `y = g*x*r` and stopping -- and it still points downhill, so the
     * loss still falls and no test that watches a loss curve can see it. */
    c = 0.0;
#endif
    for (int i = 0; i < n; i++) {
        dg[i] += dy[i] * x[i] * r;
        dx[i] += r * g[i] * dy[i] - c * x[i];
    }
}

/* RoPE, interleaved pairs, computed exactly as nn_rope does -- 1/pow(theta,
 * 2i/n) and not a recurrence, so the two agree bit for bit at every position
 * rather than drifting apart at long context.
 *
 * THE COS AND SIN ARE TABULATED, and the table is not an approximation of the
 * formula -- it holds the values the formula produces, so every rotation is
 * the SAME double it was when this was pow/cos/sin per pair. That is the whole
 * requirement: nothing may change in the arithmetic, only in how often it is
 * done. The rotation depends on (pos, i, head_dim, theta) and on nothing that
 * varies within a run, so computing it inside the loop recomputed one of
 * seq_len*head_dim/2 = 2,048 fixed values 3.1 million times per sequence here
 * (2 calls x 6 layers x 256 positions x (8+8) heads x 16 pairs, forward and
 * backward). Measured against -DLMTRAIN_NO_ROPE_TABLE below, which is the
 * control, at dim=256/L=6/seq=256 on one thread: 802 -> 835 tok/s (+4.1%)
 * with the vectorising flags, 349 -> 355 (+1.7%) at plain -O2. Small, and
 * kept because it is free -- but the honest reading is that 1,536 double
 * transcendentals against 823k MACs is a 1:536 ratio and behaves like one.
 * (This is the same arithmetic the DEVICE's nn_rope still does per token. It
 * is not fixed there because c/lib/nn is not this line's file to edit -- and
 * on the device the ratio is the same, so the same few percent is what it
 * would be worth there too, not the large win the shape of the code
 * suggests.)
 * (This is the same arithmetic the DEVICE's nn_rope still does per token --
 * 1,536 double transcendentals against 823k MACs. It is not fixed there
 * because c/lib/nn is not this line's file to edit.)
 *
 * The lookup is guarded rather than trusted: a position or a width the table
 * was not built for falls back to the formula, so a caller the init did not
 * anticipate is slow and correct rather than fast and reading someone else's
 * row. */
static double *g_rope_cs = NULL;      /* [pos][half][2], cos then sin */
static int g_rope_n = 0, g_rope_pos = 0;

static void rope_init(int n, int maxpos)
{
    free(g_rope_cs);
    g_rope_cs = xalloc((size_t)maxpos * (n / 2) * 2, sizeof(double));
    g_rope_n = n; g_rope_pos = maxpos;
    int half = n / 2;
    for (int p = 0; p < maxpos; p++)
        for (int i = 0; i < half; i++) {
            double freq = 1.0 / pow(ROPE_BASE, (double)(2 * i) / (double)n);
            double a = (double)p * freq;
            g_rope_cs[((size_t)p * half + i) * 2 + 0] = cos(a);
            g_rope_cs[((size_t)p * half + i) * 2 + 1] = sin(a);
        }
}

static const double *rope_row(int n, int pos)
{
#ifdef LMTRAIN_NO_ROPE_TABLE
    /* The switch that makes the table's cost a measurement instead of a
     * belief: -DLMTRAIN_NO_ROPE_TABLE forces every rotation back through
     * pow/cos/sin and changes nothing else, so the difference in tok/s is the
     * table's whole contribution and the difference in the loss column is
     * zero (it must be -- the fallback computes the same doubles). */
    (void)n; (void)pos; return NULL;
#endif
    if (g_rope_cs && n == g_rope_n && pos >= 0 && pos < g_rope_pos)
        return g_rope_cs + (size_t)pos * (n / 2) * 2;
    return NULL;
}

static void rope_fwd(double *x, int n, int pos)
{
    int half = n / 2;
    const double *cs = rope_row(n, pos);
    for (int i = 0; i < half; i++) {
        double c, s;
        if (cs) { c = cs[2 * i]; s = cs[2 * i + 1]; }
        else {
            double freq = 1.0 / pow(ROPE_BASE, (double)(2 * i) / (double)n);
            double a = (double)pos * freq; c = cos(a); s = sin(a);
        }
        double x0 = x[2 * i], x1 = x[2 * i + 1];
        x[2 * i]     = x0 * c - x1 * s;
        x[2 * i + 1] = x0 * s + x1 * c;
    }
}

/* A rotation's transpose is the rotation by -a. */
static void rope_bwd(double *d, int n, int pos)
{
    int half = n / 2;
    const double *cs = rope_row(n, pos);
    for (int i = 0; i < half; i++) {
        double c, s;
        if (cs) { c = cs[2 * i]; s = cs[2 * i + 1]; }
        else {
            double freq = 1.0 / pow(ROPE_BASE, (double)(2 * i) / (double)n);
            double a = (double)pos * freq; c = cos(a); s = sin(a);
        }
        double d0 = d[2 * i], d1 = d[2 * i + 1];
        d[2 * i]     =  d0 * c + d1 * s;
        d[2 * i + 1] = -d0 * s + d1 * c;
    }
}

/* -------------------------------------------------------------- activations --
 *
 * Sized once for seq_len and reused for every step and every shorter sequence,
 * so the training loop allocates nothing. The strides are computed from the
 * RUNTIME T, not from the capacity, because the sampler runs the same forward
 * over a growing prefix. */
struct act {
    int cap;
    double *xin;                    /* [L+1][cap][dim] residual entering layer l */
    double *n1, *q, *ao, *res1, *n2;/* [L][cap][dim] */
    double *k, *v;                  /* [L][cap][kv_dim] */
    double *r1, *r2;                /* [L][cap] */
    double *att;                    /* [L][cap][heads][cap] softmax rows */
    double *gg, *uu, *hh;           /* [L][cap][hidden] */
    double *nf;                     /* [cap][dim] */
    double *rf;                     /* [cap] */
    double *logits, *probs;         /* [cap][vocab] */
    double *dx, *dres, *dxb, *dtmp; /* [cap][dim] */
    double *dq, *dao;               /* [cap][dim] */
    double *dk, *dv;                /* [cap][kv_dim] */
    double *dgg, *duu, *dhh;        /* [cap][hidden] */
    double *dlogits;                /* [cap][vocab] */
    double *dp;                     /* [cap] one attention row's dp */
    double *arena;                  /* the ONE allocation */
    size_t arena_n;
};

static double *carve(double **cur, size_t n) { double *p = *cur; *cur += n; return p; }

static void act_new(struct act *a, const struct cfg *c)
{
    size_t L = c->n_layers, T = c->seq_len, D = c->dim, H = c->hidden;
    size_t K = c->kv_dim, NH = c->n_heads, V = c->vocab;
    /* ONE allocation, carved -- which is infer.h's arrangement and is here for
     * its second reason rather than its first: thirty separate mallocs need a
     * thirty-line free() that is a LIST, and a list that falls one entry behind
     * act_new leaks silently. The size below is asserted against the carving,
     * so the two cannot disagree. */
    size_t need = (L + 1) * T * D          /* xin                          */
                + 5 * L * T * D            /* n1 q ao res1 n2              */
                + 2 * L * T * K            /* k v                          */
                + 2 * L * T                /* r1 r2                        */
                + L * T * NH * T           /* att                          */
                + 3 * L * T * H            /* gg uu hh                     */
                + T * D + T                /* nf rf                        */
                + 2 * T * V                /* logits probs                 */
                + 6 * T * D                /* dx dres dxb dtmp dq dao      */
                + 2 * T * K                /* dk dv                        */
                + 3 * T * H                /* dgg duu dhh                  */
                + T * V + T;               /* dlogits dp                   */
    memset(a, 0, sizeof *a);
    a->cap = (int)T;
    a->arena_n = need;
    a->arena = xalloc(need, sizeof(double));
    double *p = a->arena;
    a->xin  = carve(&p, (L + 1) * T * D);
    a->n1   = carve(&p, L * T * D);
    a->q    = carve(&p, L * T * D);
    a->ao   = carve(&p, L * T * D);
    a->res1 = carve(&p, L * T * D);
    a->n2   = carve(&p, L * T * D);
    a->k    = carve(&p, L * T * K);
    a->v    = carve(&p, L * T * K);
    a->r1   = carve(&p, L * T);
    a->r2   = carve(&p, L * T);
    a->att  = carve(&p, L * T * NH * T);
    a->gg   = carve(&p, L * T * H);
    a->uu   = carve(&p, L * T * H);
    a->hh   = carve(&p, L * T * H);
    a->nf   = carve(&p, T * D);
    a->rf   = carve(&p, T);
    a->logits = carve(&p, T * V);
    a->probs  = carve(&p, T * V);
    a->dx   = carve(&p, T * D);
    a->dres = carve(&p, T * D);
    a->dxb  = carve(&p, T * D);
    a->dtmp = carve(&p, T * D);
    a->dq   = carve(&p, T * D);
    a->dao  = carve(&p, T * D);
    a->dk   = carve(&p, T * K);
    a->dv   = carve(&p, T * K);
    a->dgg  = carve(&p, T * H);
    a->duu  = carve(&p, T * H);
    a->dhh  = carve(&p, T * H);
    a->dlogits = carve(&p, T * V);
    a->dp   = carve(&p, T);
    if ((size_t)(p - a->arena) != need)
        die("activation arena size disagrees with the carving");
}

static void act_free(struct act *a)
{
    free(a->arena);
    memset(a, 0, sizeof *a);
}

/* ---------------------------------------------------------------- forward --
 *
 * Returns the SUMMED negative log-likelihood over the T predicted tokens, or 0
 * when tgt is NULL (the sampler, which wants logits and no loss). Every
 * intermediate the backward pass needs is left in `a`. */
static double lm_forward(const struct cfg *c, const struct pdesc *d, const double *P,
                         struct act *a, const unsigned char *tok,
                         const unsigned char *tgt, int T)
{
    const int D = c->dim, H = c->hidden, K = c->kv_dim, hd = c->head_dim;
    const int nh = c->n_heads, nkv = c->n_kv_heads, nrep = nh / nkv, cap = a->cap;
    const double scale = 1.0 / sqrt((double)hd);
    const double *emb = P + d[T_TOK].off;

    for (int t = 0; t < T; t++)
        memcpy(a->xin + (size_t)t * D, emb + (size_t)tok[t] * D, (size_t)D * sizeof(double));

    for (int l = 0; l < c->n_layers; l++) {
        double *xin  = a->xin  + (size_t)l * cap * D;
        double *xout = a->xin  + (size_t)(l + 1) * cap * D;
        double *n1   = a->n1   + (size_t)l * cap * D;
        double *q    = a->q    + (size_t)l * cap * D;
        double *kk   = a->k    + (size_t)l * cap * K;
        double *vv   = a->v    + (size_t)l * cap * K;
        double *ao   = a->ao   + (size_t)l * cap * D;
        double *res1 = a->res1 + (size_t)l * cap * D;
        double *n2   = a->n2   + (size_t)l * cap * D;
        double *r1   = a->r1   + (size_t)l * cap;
        double *r2   = a->r2   + (size_t)l * cap;
        double *att  = a->att  + (size_t)l * cap * nh * cap;
        double *gg   = a->gg   + (size_t)l * cap * H;
        double *uu   = a->uu   + (size_t)l * cap * H;
        double *hh   = a->hh   + (size_t)l * cap * H;
        const double *an = P + d[T_L(l, L_AN)].off, *fn = P + d[T_L(l, L_FN)].off;
        const double *wq = P + d[T_L(l, L_WQ)].off, *wk = P + d[T_L(l, L_WK)].off;
        const double *wv = P + d[T_L(l, L_WV)].off, *wo = P + d[T_L(l, L_WO)].off;
        const double *w1 = P + d[T_L(l, L_W1)].off, *w3 = P + d[T_L(l, L_W3)].off;
        const double *w2 = P + d[T_L(l, L_W2)].off;

        for (int t = 0; t < T; t++)
            r1[t] = rms_fwd(n1 + (size_t)t * D, xin + (size_t)t * D, an, D);

        lin_fwd(q,  n1, wq, T, D, D);
        lin_fwd(kk, n1, wk, T, K, D);
        lin_fwd(vv, n1, wv, T, K, D);

        for (int t = 0; t < T; t++) {
            for (int h = 0; h < nh;  h++) rope_fwd(q  + (size_t)t * D + h * hd, hd, t);
            for (int h = 0; h < nkv; h++) rope_fwd(kk + (size_t)t * K + h * hd, hd, t);
        }

        for (int t = 0; t < T; t++) {
            for (int h = 0; h < nh; h++) {
                const double *qv = q + (size_t)t * D + h * hd;
                int kvh = h / nrep;
                double *p = att + ((size_t)t * nh + h) * cap;
                /* CAUSAL: j runs to t inclusive and no further. A mask applied
                 * as -inf after computing every score would cost T^2/2 wasted
                 * dot products and would then have to be undone in the
                 * backward pass; not computing them is both. */
                double mx = -1e300;
                for (int j = 0; j <= t; j++) {
                    const double *kv = kk + (size_t)j * K + kvh * hd;
                    double s = 0;
                    for (int e = 0; e < hd; e++) s += qv[e] * kv[e];
                    s *= scale;
                    p[j] = s;
                    if (s > mx) mx = s;
                }
                double sum = 0;
                for (int j = 0; j <= t; j++) { p[j] = exp(p[j] - mx); sum += p[j]; }
                double inv = 1.0 / sum;
                for (int j = 0; j <= t; j++) p[j] *= inv;

                double *out = ao + (size_t)t * D + h * hd;
                for (int e = 0; e < hd; e++) out[e] = 0;
                for (int j = 0; j <= t; j++) {
                    const double *vvv = vv + (size_t)j * K + kvh * hd;
                    double w = p[j];
                    for (int e = 0; e < hd; e++) out[e] += w * vvv[e];
                }
            }
        }

        lin_fwd(a->dtmp, ao, wo, T, D, D);      /* dtmp is scratch here */
        for (size_t i = 0; i < (size_t)T * D; i++) res1[i] = xin[i] + a->dtmp[i];

        for (int t = 0; t < T; t++)
            r2[t] = rms_fwd(n2 + (size_t)t * D, res1 + (size_t)t * D, fn, D);

        lin_fwd(gg, n2, w1, T, H, D);
        lin_fwd(uu, n2, w3, T, H, D);
        for (size_t i = 0; i < (size_t)T * H; i++) {
            double z = gg[i];
            hh[i] = (z / (1.0 + exp(-z))) * uu[i];
        }
        lin_fwd(a->dtmp, hh, w2, T, D, H);
        for (size_t i = 0; i < (size_t)T * D; i++) xout[i] = res1[i] + a->dtmp[i];
    }

    const double *xf = a->xin + (size_t)c->n_layers * cap * D;
    const double *fnorm = P + d[T_FINAL(c)].off;
    const double *wcls  = P + d[c->tied ? T_TOK : T_WCLS(c)].off;
    for (int t = 0; t < T; t++)
        a->rf[t] = rms_fwd(a->nf + (size_t)t * D, xf + (size_t)t * D, fnorm, D);
    lin_fwd(a->logits, a->nf, wcls, T, c->vocab, D);

    if (!tgt) return 0.0;

    double loss = 0;
    for (int t = 0; t < T; t++) {
        double *lg = a->logits + (size_t)t * c->vocab;
        double *pr = a->probs  + (size_t)t * c->vocab;
        double mx = lg[0];
        for (int i = 1; i < c->vocab; i++) if (lg[i] > mx) mx = lg[i];
        double sum = 0;
        for (int i = 0; i < c->vocab; i++) { pr[i] = exp(lg[i] - mx); sum += pr[i]; }
        double inv = 1.0 / sum;
        for (int i = 0; i < c->vocab; i++) pr[i] *= inv;
        /* log(sum) + mx - lg[target] instead of -log(pr[target]): identical in
         * exact arithmetic and it does not lose the low bits of a probability
         * that underflowed to zero, which is what a confident model produces
         * on the tokens it is most sure about. */
        loss += log(sum) + mx - lg[tgt[t]];
    }
    return loss;
}

/* --------------------------------------------------------------- backward --
 *
 * `gscale` multiplies dlogits, so the caller decides what the loss is averaged
 * over (all tokens of all sequences in the step) without this function knowing
 * about batching. Gradients ACCUMULATE into G, so a batch is a loop. */
static void lm_backward(const struct cfg *c, const struct pdesc *d, const double *P,
                        double *G, struct act *a, const unsigned char *tok,
                        const unsigned char *tgt, int T, double gscale)
{
    const int D = c->dim, H = c->hidden, K = c->kv_dim, hd = c->head_dim;
    const int nh = c->n_heads, nkv = c->n_kv_heads, nrep = nh / nkv, cap = a->cap;
    const double scale = 1.0 / sqrt((double)hd);

    for (int t = 0; t < T; t++) {
        const double *pr = a->probs + (size_t)t * c->vocab;
        double *dl = a->dlogits + (size_t)t * c->vocab;
        for (int i = 0; i < c->vocab; i++) dl[i] = pr[i] * gscale;
        dl[tgt[t]] -= gscale;
    }

    const double *xf = a->xin + (size_t)c->n_layers * cap * D;
    const double *fnorm = P + d[T_FINAL(c)].off;
    int wcls_t = c->tied ? T_TOK : T_WCLS(c);
    const double *wcls = P + d[wcls_t].off;
    double *dwcls = G + d[wcls_t].off;      /* == dtok_emb when tied, on purpose */
    double *dfn = G + d[T_FINAL(c)].off;

    memset(a->dtmp, 0, (size_t)T * D * sizeof(double));   /* d(nf) */
    lin_bwd(a->dtmp, dwcls, a->dlogits, a->nf, wcls, T, c->vocab, D);
    memset(a->dx, 0, (size_t)T * D * sizeof(double));
    for (int t = 0; t < T; t++)
        rms_bwd(a->dx + (size_t)t * D, dfn, a->dtmp + (size_t)t * D,
                xf + (size_t)t * D, fnorm, a->rf[t], D);

    for (int l = c->n_layers - 1; l >= 0; l--) {
        double *xin  = a->xin  + (size_t)l * cap * D;
        double *n1   = a->n1   + (size_t)l * cap * D;
        double *q    = a->q    + (size_t)l * cap * D;
        double *kk   = a->k    + (size_t)l * cap * K;
        double *vv   = a->v    + (size_t)l * cap * K;
        double *ao   = a->ao   + (size_t)l * cap * D;
        double *res1 = a->res1 + (size_t)l * cap * D;
        double *n2   = a->n2   + (size_t)l * cap * D;
        double *r1   = a->r1   + (size_t)l * cap;
        double *r2   = a->r2   + (size_t)l * cap;
        double *att  = a->att  + (size_t)l * cap * nh * cap;
        double *gg   = a->gg   + (size_t)l * cap * H;
        double *uu   = a->uu   + (size_t)l * cap * H;
        double *hh   = a->hh   + (size_t)l * cap * H;
        const double *an = P + d[T_L(l, L_AN)].off, *fn = P + d[T_L(l, L_FN)].off;
        const double *wq = P + d[T_L(l, L_WQ)].off, *wk = P + d[T_L(l, L_WK)].off;
        const double *wv = P + d[T_L(l, L_WV)].off, *wo = P + d[T_L(l, L_WO)].off;
        const double *w1 = P + d[T_L(l, L_W1)].off, *w3 = P + d[T_L(l, L_W3)].off;
        const double *w2 = P + d[T_L(l, L_W2)].off;
        double *dan = G + d[T_L(l, L_AN)].off, *dfn2 = G + d[T_L(l, L_FN)].off;
        double *dwq = G + d[T_L(l, L_WQ)].off, *dwk = G + d[T_L(l, L_WK)].off;
        double *dwv = G + d[T_L(l, L_WV)].off, *dwo = G + d[T_L(l, L_WO)].off;
        double *dw1 = G + d[T_L(l, L_W1)].off, *dw3 = G + d[T_L(l, L_W3)].off;
        double *dw2 = G + d[T_L(l, L_W2)].off;

        /* xout = res1 + w2(hh): the residual sends the same gradient to both. */
        memcpy(a->dres, a->dx, (size_t)T * D * sizeof(double));

        memset(a->dhh, 0, (size_t)T * H * sizeof(double));
        lin_bwd(a->dhh, dw2, a->dx, hh, w2, T, D, H);
        for (size_t i = 0; i < (size_t)T * H; i++) {
            double z = gg[i], sg = 1.0 / (1.0 + exp(-z));
            /* d/dz [z*sigmoid(z)] = sigmoid + z*sigmoid*(1-sigmoid) */
            a->dgg[i] = a->dhh[i] * uu[i] * (sg * (1.0 + z * (1.0 - sg)));
            a->duu[i] = a->dhh[i] * (z * sg);
        }
        memset(a->dtmp, 0, (size_t)T * D * sizeof(double));   /* d(n2) */
        lin_bwd(a->dtmp, dw1, a->dgg, n2, w1, T, H, D);
        lin_bwd(a->dtmp, dw3, a->duu, n2, w3, T, H, D);
        for (int t = 0; t < T; t++)
            rms_bwd(a->dres + (size_t)t * D, dfn2, a->dtmp + (size_t)t * D,
                    res1 + (size_t)t * D, fn, r2[t], D);

        /* res1 = xin + wo(ao) */
        memcpy(a->dxb, a->dres, (size_t)T * D * sizeof(double));
        memset(a->dao, 0, (size_t)T * D * sizeof(double));
        lin_bwd(a->dao, dwo, a->dres, ao, wo, T, D, D);

        memset(a->dq, 0, (size_t)T * D * sizeof(double));
        memset(a->dk, 0, (size_t)T * K * sizeof(double));
        memset(a->dv, 0, (size_t)T * K * sizeof(double));
        for (int t = 0; t < T; t++) {
            for (int h = 0; h < nh; h++) {
                int kvh = h / nrep;
                const double *p  = att + ((size_t)t * nh + h) * cap;
                const double *go = a->dao + (size_t)t * D + h * hd;
                const double *qv = q + (size_t)t * D + h * hd;
                double *dqv = a->dq + (size_t)t * D + h * hd;
                double dot = 0;
                for (int j = 0; j <= t; j++) {
                    const double *vvv = vv + (size_t)j * K + kvh * hd;
                    double *dvv = a->dv + (size_t)j * K + kvh * hd;
                    double s = 0, w = p[j];
                    for (int e = 0; e < hd; e++) { s += go[e] * vvv[e]; dvv[e] += w * go[e]; }
                    a->dp[j] = s;
                    dot += w * s;
                }
                for (int j = 0; j <= t; j++) {
                    /* softmax jacobian: p_j (dp_j - sum_m p_m dp_m) */
                    double dsj = p[j] * (a->dp[j] - dot) * scale;
                    const double *kv = kk + (size_t)j * K + kvh * hd;
                    double *dkv = a->dk + (size_t)j * K + kvh * hd;
                    for (int e = 0; e < hd; e++) {
                        dqv[e] += dsj * kv[e];
                        dkv[e] += dsj * qv[e];
                    }
                }
            }
        }

        for (int t = 0; t < T; t++) {
            for (int h = 0; h < nh;  h++) rope_bwd(a->dq + (size_t)t * D + h * hd, hd, t);
            for (int h = 0; h < nkv; h++) rope_bwd(a->dk + (size_t)t * K + h * hd, hd, t);
        }

        memset(a->dtmp, 0, (size_t)T * D * sizeof(double));   /* d(n1) */
        lin_bwd(a->dtmp, dwq, a->dq, n1, wq, T, D, D);
        lin_bwd(a->dtmp, dwk, a->dk, n1, wk, T, K, D);
        lin_bwd(a->dtmp, dwv, a->dv, n1, wv, T, K, D);
#ifdef LMTRAIN_TRANSPOSE_DWQ
        /* NEGATIVE CONTROL: dWq transposed, and nothing else touched. Wq is
         * square so this compiles, runs, and produces a gradient of exactly
         * the right magnitude pointing somewhere else -- the loss still falls,
         * just slower and to a worse place. This is the failure the per-tensor
         * report exists for: a single global worst-case would be dominated by
         * the eleven tensors that are still right. */
        for (int r0 = 0; r0 < D; r0++)
            for (int c0 = r0 + 1; c0 < D; c0++) {
                double t0 = dwq[(size_t)r0 * D + c0];
                dwq[(size_t)r0 * D + c0] = dwq[(size_t)c0 * D + r0];
                dwq[(size_t)c0 * D + r0] = t0;
            }
#endif
        for (int t = 0; t < T; t++)
            rms_bwd(a->dxb + (size_t)t * D, dan, a->dtmp + (size_t)t * D,
                    xin + (size_t)t * D, an, r1[t], D);

        memcpy(a->dx, a->dxb, (size_t)T * D * sizeof(double));
    }

    double *demb = G + d[T_TOK].off;
    for (int t = 0; t < T; t++) {
        double *row = demb + (size_t)tok[t] * D;
        const double *g = a->dx + (size_t)t * D;
        for (int i = 0; i < D; i++) row[i] += g[i];
    }
}

/* ------------------------------------------------------------- gradcheck --
 *
 * THE GATE THAT DECIDES WHETHER THIS IS A TRAINER OR A RANDOM-NUMBER GENERATOR
 * THAT IMPROVES SLIGHTLY. Central differences in f64 against every analytic
 * gradient, reported PER TENSOR: one global worst-case hides the single matrix
 * whose gradient is transposed, and a transposed gradient still makes the loss
 * fall -- slower, and to a worse place, with nothing anywhere saying why.
 *
 * THE THRESHOLD IS DERIVED, NOT FITTED. A central difference at step e carries
 *   rounding   ~ eps_mach * |L| / (2e)  = 2.2e-16 * 5.5 / 2e-4 ~ 6e-12
 *   truncation ~ e^2/6 * |L'''|         = 1e-8/6 * O(1)        ~ 2e-9
 * so the numeric gradient is good to about 2e-9 ABSOLUTE. Relative error is
 * therefore only meaningful where the gradient itself is larger than that by a
 * wide margin, which is why the denominator has a floor of 1e-4: below it a
 * correct gradient would report a relative error of 2e-9/1e-4 = 2e-5, still
 * comfortably under the 1e-4 bar, while a WRONG gradient of any size at all
 * moves the numerator by its own magnitude and fails. A floor tuned to the
 * observed error would measure nothing; this one is arithmetic. */
#define GC_EPS   1e-4
#define GC_FLOOR 1e-4
#define GC_BAR   1e-4

struct gc_case { const char *label; struct cfg c; };

static int gradcheck_one(const struct gc_case *gc)
{
    struct cfg c = gc->c;
    if (!cfg_derive(&c)) { fprintf(stderr, "gradcheck: bad config\n"); return 1; }
    rope_init(c.head_dim, c.seq_len);

    struct pdesc *d = xalloc(N_TENS(&c), sizeof *d);
    size_t np = 0;
    int nt = build_desc(&c, d, &np);

    double *P = xalloc(np, sizeof(double));
    double *G = xalloc(np, sizeof(double));
    struct act a; act_new(&a, &c);

    /* Init away from every special point: gains are not exactly 1 (a gain of 1
     * makes dg = dy*x*r, which is correct for the wrong reason if the gain
     * gradient is dropped) and no weight is zero. */
    for (int i = 0; i < nt; i++) {
        double *p = P + d[i].off;
        size_t n = (size_t)d[i].rows * d[i].cols;
        if (d[i].kind == PK_NORM) for (size_t j = 0; j < n; j++) p[j] = 1.0 + 0.3 * nrand();
        else                      for (size_t j = 0; j < n; j++) p[j] = 0.5 * nrand();
    }

    int T = c.seq_len;
    unsigned char *tok = xalloc(T + 1, 1), *tgt = xalloc(T + 1, 1);
    for (int t = 0; t < T; t++) tok[t] = (unsigned char)(urand() * c.vocab);
    for (int t = 0; t < T; t++) tgt[t] = (unsigned char)(urand() * c.vocab);

    double gscale = 1.0 / (double)T;
    lm_forward(&c, d, P, &a, tok, tgt, T);
    lm_backward(&c, d, P, G, &a, tok, tgt, T, gscale);

    printf("gradcheck %-10s dim=%d L=%d heads=%d kv=%d hidden=%d vocab=%d seq=%d tied=%d "
           "(%zu params)\n", gc->label, c.dim, c.n_layers, c.n_heads, c.n_kv_heads,
           c.hidden, c.vocab, c.seq_len, c.tied, np);
    printf("  %-14s %6s  %12s  %6s  %14s  %14s\n",
           "tensor", "n", "worst rel", "at", "analytic", "numeric");

    int bad = 0;
    double worst_all = 0;
    for (int i = 0; i < nt; i++) {
        size_t n = (size_t)d[i].rows * d[i].cols;
        double worst = 0, wa = 0, wn = 0; size_t wat = 0;
        for (size_t j = 0; j < n; j++) {
            double *w = P + d[i].off + j;
            double save = *w;
            *w = save + GC_EPS; double lp = lm_forward(&c, d, P, &a, tok, tgt, T) * gscale;
            *w = save - GC_EPS; double lm = lm_forward(&c, d, P, &a, tok, tgt, T) * gscale;
            *w = save;
            double num = (lp - lm) / (2.0 * GC_EPS);
            double ana = G[d[i].off + j];
            double den = fabs(ana) + fabs(num);
            if (den < GC_FLOOR) den = GC_FLOOR;
            double rel = fabs(ana - num) / den;
            if (rel > worst) { worst = rel; wa = ana; wn = num; wat = j; }
        }
        if (worst > worst_all) worst_all = worst;
        int fail = worst > GC_BAR;
        if (fail) bad++;
        printf("  %-14s %6zu  %12.3e  %6zu  %14.6e  %14.6e%s\n",
               d[i].name, n, worst, wat, wa, wn, fail ? "   <-- FAIL" : "");
    }
    printf("  WORST %.3e over %zu params -- %s (bar %.0e)\n\n",
           worst_all, np, bad ? "FAIL" : "PASS", (double)GC_BAR);

    free(tok); free(tgt); act_free(&a); free(P); free(G); free(d);
    return bad ? 1 : 0;
}

static int run_gradcheck(void)
{
    /* Three configurations, because three distinct wirings can each be wrong
     * alone. TIED is what ships and makes tok_emb receive gradient from two
     * places at once; UNTIED is the only one in which a missing wcls gradient
     * is visible at all; GQA (n_kv_heads < n_heads) is the only one in which
     * the query-head -> kv-head mapping does anything. */
    struct gc_case cases[] = {
        { "tied",   { 8, 1, 2, 2, 0, 0, 16, 8, 4, 1 } },
        { "untied", { 8, 1, 2, 2, 0, 0, 16, 8, 4, 0 } },
        { "gqa",    { 8, 2, 4, 2, 0, 0, 16, 8, 4, 1 } },
    };
    g_rng = 20260820;
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        bad += gradcheck_one(&cases[i]);
    printf("gradcheck: %s\n", bad ? "FAILED" : "all configurations pass");
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------- the writer --
 *
 * Sizes computed here rather than taken from lm_expected_size(), because
 * model.c is not linked into a host tool -- but the number is checked against
 * the bytes actually written, so a disagreement between this arithmetic and
 * this loop cannot ship. The cross-check against the READER is somebody else's
 * gate (tests/unit/lm_format_test.c), which is the right place for it: a
 * writer that validates itself proves nothing about the reader. */
static size_t tensor_disk_bytes(const struct pdesc *p, int dtype)
{
    size_t n = (size_t)p->rows * p->cols;
    if (p->kind == PK_MAT && dtype == NN_Q8)
        return n + (size_t)p->rows * sizeof(float);   /* int8 body, then f32 scales */
    return n * sizeof(float);
}

static size_t file_expected_bytes(const struct cfg *c, const struct pdesc *d,
                                  int nt, int dtype)
{
    size_t s = sizeof(struct lm_header);
    for (int i = 0; i < nt; i++) s += tensor_disk_bytes(&d[i], dtype);
    (void)c;
    return s;
}

static int write_model(const char *path, const struct cfg *c, const struct pdesc *d,
                       int nt, const double *P, int dtype, size_t *out_bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "lmtrain: cannot open %s for writing\n", path); return 0; }

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
    /* reserved[] stays zero: model.h says a reader must REFUSE a non-zero one,
     * so writing anything there is writing a file no loader will open. */
    if (fwrite(&h, 1, sizeof h, f) != sizeof h) { fclose(f); return 0; }

    size_t maxn = 0, maxrows = 0;
    for (int i = 0; i < nt; i++) {
        size_t n = (size_t)d[i].rows * d[i].cols;
        if (n > maxn) maxn = n;
        if ((size_t)d[i].rows > maxrows) maxrows = d[i].rows;
    }
    float  *fbuf = xalloc(maxn, sizeof(float));
    int8_t *qbuf = xalloc(maxn, 1);
    float  *sbuf = xalloc(maxrows, sizeof(float));

    for (int i = 0; i < nt; i++) {
        const double *p = P + d[i].off;
        size_t n = (size_t)d[i].rows * d[i].cols;
        for (size_t j = 0; j < n; j++) fbuf[j] = (float)p[j];
        if (d[i].kind == PK_MAT && dtype == NN_Q8) {
            nn_quantize_q8(qbuf, sbuf, fbuf, d[i].rows, d[i].cols);
            if (fwrite(qbuf, 1, n, f) != n) goto werr;
            if (fwrite(sbuf, sizeof(float), d[i].rows, f) != (size_t)d[i].rows) goto werr;
        } else {
            if (fwrite(fbuf, sizeof(float), n, f) != n) goto werr;
        }
    }

    long pos = ftell(f);
    fclose(f);
    free(fbuf); free(qbuf); free(sbuf);
    size_t want = file_expected_bytes(c, d, nt, dtype);
    if (pos < 0 || (size_t)pos != want) {
        fprintf(stderr, "lmtrain: wrote %ld bytes, format says %zu -- refusing to "
                        "claim this file is valid\n", pos, want);
        return 0;
    }
    if (out_bytes) *out_bytes = want;
    return 1;
werr:
    fclose(f); free(fbuf); free(qbuf); free(sbuf);
    fprintf(stderr, "lmtrain: short write on %s\n", path);
    return 0;
}

/* --------------------------------------------------------------- sampling --
 *
 * Greedy, and it re-runs the FULL forward over the whole prefix for every
 * token, which is O(n^2) where a KV cache is O(n). That is deliberate: a
 * cached single-token path is a SECOND implementation of the forward pass, and
 * a second implementation that disagrees with the trained one produces
 * plausible nonsense with no error anywhere -- the exact failure this file
 * warns about for RoPE. The device has that path (c/lib/nn/infer.c) and
 * proving it matches is that line's gate, not this one's. At 200 tokens the
 * quadratic cost is a few seconds, once, after a run measured in minutes. */
static void sample_greedy(const struct cfg *c, const struct pdesc *d, const double *P,
                          struct act *a, const char *prompt, int ngen)
{
    int cap = c->seq_len;
    unsigned char *ctx = xalloc(cap, 1);
    int n = 0;
    for (const char *s = prompt; *s && n < cap - 1; s++) ctx[n++] = (unsigned char)*s;
    if (n == 0) ctx[n++] = '\n';

    fputs(prompt, stdout);
    for (int i = 0; i < ngen; i++) {
        if (n >= cap) {
            /* Slide by half. Positions restart from 0 for the surviving half,
             * which is what a sliding window does and what it costs; the
             * default prompt+length never reaches here. */
            memmove(ctx, ctx + cap / 2, (size_t)(cap - cap / 2));
            n = cap - cap / 2;
        }
        lm_forward(c, d, P, a, ctx, NULL, n);
        const double *lg = a->logits + (size_t)(n - 1) * c->vocab;
        int best = 0;
        for (int j = 1; j < c->vocab; j++) if (lg[j] > lg[best]) best = j;
        ctx[n++] = (unsigned char)best;
        /* Bytes >= 0x20 pass through so a UTF-8 sequence the model learned
         * from the corpus arrives intact; only C0 controls are folded. */
        int b = best;
        putchar((b == '\n' || b == '\t' || b >= 32) ? b : '.');
    }
    putchar('\n');
    free(ctx);
}

/* ------------------------------------------------------------------ main --*/

/* -------------------------------------------------------- held-out bits/byte --
 *
 * THE NUMBER THE WHOLE LINE IS JUDGED ON, and the reason it is not `eval_loss`
 * below. eval_loss samples a handful of random windows: that is the right
 * instrument for a curve you watch during a run, and the wrong one for a claim
 * against a compressor, because gzip is not handed eight random windows -- it
 * is handed every byte of the held-out region exactly once. So is this.
 *
 * THE COVERAGE RULE. A forward pass over data[s .. s+T) predicts
 * data[s+1 .. s+T], and position p in it has p+1 bytes of context. Scoring a
 * whole window and then starting the next one where it ended would charge the
 * model for bytes it had to predict from one or two bytes of context, over and
 * over -- a real cost, but one gzip does not pay repeatedly and one that says
 * more about the window layout than about the model. So windows OVERLAP by
 * T-stride and only the last `stride` positions of each are scored:
 *
 *   window 0 at lo         scores targets lo+1 .. lo+T          (context 1..T)
 *   window m at lo+m*strd  scores targets ..+T-strd+1 .. ..+T   (context >= T-strd+1)
 *
 * Consecutive windows abut exactly, so EVERY byte of [lo+1, hi) is scored
 * exactly once and none is scored twice. At the default T=256/stride=128 the
 * scored bytes carry between 129 and 256 bytes of context. That is the
 * model's whole memory and it is worth saying out loud next to a gzip number:
 * gzip's window is 32 KB and xz's is 64 MB, so the transformer is playing a
 * much harder game and beating it anyway is the finding.
 *
 * The first byte of the region has no context at all and is charged 8 bits --
 * what a uniform byte model would pay -- so the total is over all `hi - lo`
 * bytes and can be put beside `gzip -9 | wc -c` without an asterisk.
 *
 * The window loop is the parallel one because windows are independent; each
 * worker needs its own activation arena, which is why this takes an array. */
static double eval_bpb(const struct cfg *c, const struct pdesc *d, const double *P,
                       struct act **acts, int W,
                       const unsigned char *data, size_t lo, size_t hi,
                       int stride, double *out_bytes)
{
    int T = c->seq_len;
    if (hi <= lo + 1) { if (out_bytes) *out_bytes = 0; return 0.0; }
    size_t N = hi - lo;
    if (stride <= 0 || stride > T) stride = T / 2;

    /* Enumerate the windows first, serially, so the parallel loop is a flat
     * indexed range and the reduction order does not depend on scheduling. */
    size_t nw = 1;
    if (N > (size_t)T) nw += (N - (size_t)T + (size_t)stride - 1) / (size_t)stride;
    size_t *ws = xalloc(nw, sizeof(size_t));
    int *wskip = xalloc(nw, sizeof(int));
    int *wlen  = xalloc(nw, sizeof(int));
    size_t covered = lo + 1;                 /* first target not yet scored */
    size_t k = 0;
    for (size_t m = 0; m < nw; m++) {
        size_t s = lo + m * (size_t)stride;
        if (s + (size_t)T > hi) s = (hi >= lo + (size_t)T) ? hi - (size_t)T : lo;
        int len = (int)((hi - s < (size_t)T) ? (hi - s) : (size_t)T);
        if (len < 2) continue;
        /* score positions p with s+p+1 >= covered */
        long p0 = (long)covered - (long)s - 1;
        if (p0 < 0) p0 = 0;
        if (p0 >= len) continue;             /* the tail window added nothing */
        ws[k] = s; wskip[k] = (int)p0; wlen[k] = len;
        /* Position p scores target s+p+1, so the last one this window can
         * reach is min(s+len, hi-1) and the next unscored index is one past
         * it. Writing `covered = s + len` here is off by one and the symptom
         * is not a crash -- it is a byte scored twice at every window seam and
         * a bits/byte that is quietly wrong by a fraction of a percent. */
        size_t last = s + (size_t)len; if (last > hi - 1) last = hi - 1;
        covered = last + 1;
        k++;
        if (covered >= hi) break;
    }
    nw = k;

    double *bits = xalloc(nw ? nw : 1, sizeof(double));
    long long *cnt = xalloc(nw ? nw : 1, sizeof(long long));
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1) num_threads(W)
#endif
    for (long i = 0; i < (long)nw; i++) {
#ifdef _OPENMP
        struct act *a = acts[omp_get_thread_num() % W];
#else
        struct act *a = acts[0];
#endif
        int len = wlen[i];
        const unsigned char *tok = data + ws[i];
        lm_forward(c, d, P, a, tok, NULL, len);
        double b = 0; long long n = 0;
        for (int p = wskip[i]; p < len - 1 + 1 && ws[i] + (size_t)p + 1 < hi; p++) {
            const double *lg = a->logits + (size_t)p * c->vocab;
            double mx = lg[0];
            for (int v = 1; v < c->vocab; v++) if (lg[v] > mx) mx = lg[v];
            double sum = 0;
            for (int v = 0; v < c->vocab; v++) sum += exp(lg[v] - mx);
            b += (log(sum) + mx - lg[tok[p + 1]]);
            n++;
        }
        bits[i] = b; cnt[i] = n;
    }
    double tot = 0; long long ntok = 0;
    for (size_t m = 0; m < nw; m++) { tot += bits[m]; ntok += cnt[m]; }
    free(ws); free(wskip); free(wlen); free(bits); free(cnt);
    if (out_bytes) *out_bytes = (double)(ntok + 1);   /* +1: the unscored first byte */
    /* nats -> bits, plus 8 bits for that first byte, over the whole region. */
    return (tot / 0.6931471805599453 + 8.0) / (double)(ntok + 1);
}

static double eval_loss(const struct cfg *c, const struct pdesc *d, const double *P,
                        struct act *a, const unsigned char *data, size_t lo, size_t hi,
                        int nwin, unsigned long long seed)
{
    /* A FIXED seed, so the validation windows are the same set at every step:
     * a val loss measured on fresh random windows each time moves for two
     * reasons at once and cannot be read as a curve. */
    unsigned long long save = g_rng;
    g_rng = seed;
    int T = c->seq_len;
    double tot = 0; long ntok = 0;
    for (int i = 0; i < nwin; i++) {
        size_t span = hi - lo;
        if (span < (size_t)T + 1) break;
        size_t s = lo + (size_t)(urand() * (double)(span - T - 1));
        tot += lm_forward(c, d, P, a, data + s, data + s + 1, T);
        ntok += T;
    }
    g_rng = save;
    return ntok ? tot / (double)ntok : 0.0;
}

static const char *usage_text =
"usage: lmtrain [options]\n"
"  --gradcheck            run the finite-difference gate and exit\n"
"  --corpus PATH          training text (default CLAUDE.md)\n"
"  --out PATH             LOGITLM output (default build/model.lm)\n"
"  --also-q8 PATH         additionally write a Q8 file from the same weights\n"
"  --dtype f32|q8         dtype of the matmul weights in --out (default f32)\n"
"  --steps N              (3000)   --batch N (1)     --seq N (256)\n"
"  --threads N            data-parallel workers (default: all cores, capped\n"
"                         at --batch; 1 without -fopenmp)\n"
"  --val-tail N           hold out exactly the LAST N bytes (default: 10%%)\n"
"  --bpb-stride N         held-out bits/byte window stride (128)\n"
"  --no-bpb               skip the full held-out bits/byte pass\n"
"  --dim N (128)  --layers N (4)  --heads N (4)  --kv-heads N (4)\n"
"  --hidden N (344)       --untied  (do not tie the output head)\n"
"  --lr F (1e-3)  --warmup N (100)  --min-lr-frac F (0.1)\n"
"  --wd F (0.1)   --clip F (1.0)    --seed N (1234)\n"
"  --every N (100)  --val-every N (500)  --val-batches N (8)\n"
"  --sample-len N (200)   --prompt STR\n";

int main(int argc, char **argv)
{
    const char *corpus = "CLAUDE.md", *outpath = "build/model.lm", *q8path = NULL;
    const char *prompt = "The kernel ";
    int steps = 3000, batch = 1, every = 100, val_every = 500, val_batches = 8;
    int sample_len = 200, dtype = NN_F32;
    double lr0 = 1e-3, wd = 0.1, clip = 1.0, min_lr_frac = 0.1;
    int warmup = 100;
    int nthreads = 0, bpb_stride = 128, do_bpb = 1;
    long long val_tail = 0;
    unsigned long long seed = 1234;
    struct cfg c;
    memset(&c, 0, sizeof c);
    c.dim = 128; c.n_layers = 4; c.n_heads = 4; c.n_kv_heads = 4;
    c.hidden = 344; c.vocab = 256; c.seq_len = 256; c.tied = 1;

    for (int i = 1; i < argc; i++) {
        const char *A = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (die("missing argument"), ""))
        if      (!strcmp(A, "--gradcheck"))   return run_gradcheck();
        else if (!strcmp(A, "--corpus"))      corpus = NEXT();
        else if (!strcmp(A, "--out"))         outpath = NEXT();
        else if (!strcmp(A, "--also-q8"))     q8path = NEXT();
        else if (!strcmp(A, "--dtype")) {
            const char *v = NEXT();
            if (!strcmp(v, "f32")) dtype = NN_F32;
            else if (!strcmp(v, "q8")) dtype = NN_Q8;
            else die("--dtype must be f32 or q8");
        }
        else if (!strcmp(A, "--steps"))       steps = atoi(NEXT());
        else if (!strcmp(A, "--batch"))       batch = atoi(NEXT());
        else if (!strcmp(A, "--threads"))     nthreads = atoi(NEXT());
        else if (!strcmp(A, "--val-tail"))    val_tail = strtoll(NEXT(), NULL, 10);
        else if (!strcmp(A, "--bpb-stride"))  bpb_stride = atoi(NEXT());
        else if (!strcmp(A, "--no-bpb"))      do_bpb = 0;
        else if (!strcmp(A, "--seq"))         c.seq_len = atoi(NEXT());
        else if (!strcmp(A, "--dim"))         c.dim = atoi(NEXT());
        else if (!strcmp(A, "--layers"))      c.n_layers = atoi(NEXT());
        else if (!strcmp(A, "--heads"))       c.n_heads = atoi(NEXT());
        else if (!strcmp(A, "--kv-heads"))    c.n_kv_heads = atoi(NEXT());
        else if (!strcmp(A, "--hidden"))      c.hidden = atoi(NEXT());
        else if (!strcmp(A, "--untied"))      c.tied = 0;
        else if (!strcmp(A, "--lr"))          lr0 = atof(NEXT());
        else if (!strcmp(A, "--warmup"))      warmup = atoi(NEXT());
        else if (!strcmp(A, "--min-lr-frac")) min_lr_frac = atof(NEXT());
        else if (!strcmp(A, "--wd"))          wd = atof(NEXT());
        else if (!strcmp(A, "--clip"))        clip = atof(NEXT());
        else if (!strcmp(A, "--seed"))        seed = (unsigned long long)strtoull(NEXT(), NULL, 10);
        else if (!strcmp(A, "--every"))       every = atoi(NEXT());
        else if (!strcmp(A, "--val-every"))   val_every = atoi(NEXT());
        else if (!strcmp(A, "--val-batches")) val_batches = atoi(NEXT());
        else if (!strcmp(A, "--sample-len"))  sample_len = atoi(NEXT());
        else if (!strcmp(A, "--prompt"))      prompt = NEXT();
        else if (!strcmp(A, "-h") || !strcmp(A, "--help")) { fputs(usage_text, stdout); return 0; }
        else { fprintf(stderr, "lmtrain: unknown option %s\n\n%s", A, usage_text); return 2; }
        #undef NEXT
    }
    if (!cfg_derive(&c)) die("inconsistent config (dim%heads, heads%kv_heads, or odd head_dim)");
    if (c.vocab != 256) die("byte-level vocabulary is 256 by construction");
    /* atoi/atof return 0 for text they cannot parse, so `--steps thirty` would
     * otherwise train for no steps and write an untrained model that opens
     * fine. Refuse instead: a run that silently does nothing is worse than one
     * that stops. */
    if (steps <= 0)  die("--steps must be positive (a non-numeric value parses as 0)");
    if (batch <= 0)  die("--batch must be positive");
    if (lr0 <= 0)    die("--lr must be positive");
    if (warmup < 0)  die("--warmup cannot be negative");
    if (val_batches < 0 || sample_len < 0) die("--val-batches/--sample-len cannot be negative");
    if (every <= 0) every = 1;
    if (val_every <= 0) val_every = 1;
    g_rng = seed ? seed : 1;
    rope_init(c.head_dim, c.seq_len);

    /* WORKERS ARE CAPPED AT THE BATCH because the parallelism is over
     * sequences and nothing else: a 25th worker on a batch of 24 would sit
     * idle holding a 60 MB activation arena and a full-size gradient array.
     * Parallelising INSIDE a sequence (splitting a matmul across cores) is the
     * other option and was not taken -- the layers are 256-1024 wide, so each
     * one is a few hundred microseconds of work and the barrier at every one
     * of the ~50 per step would cost more than it saves. Batch-parallel has
     * exactly one synchronisation point per step. */
#ifdef _OPENMP
    if (nthreads <= 0) nthreads = omp_get_max_threads();
#else
    if (nthreads > 1)
        fprintf(stderr, "lmtrain: built without OpenMP -- --threads %d ignored, "
                        "running on 1 core\n", nthreads);
    nthreads = 1;
#endif
    if (nthreads > batch) nthreads = batch;
    if (nthreads < 1) nthreads = 1;

    /* ------------------------------------------------------------ corpus */
    FILE *cf = fopen(corpus, "rb");
    if (!cf) { fprintf(stderr, "lmtrain: cannot open corpus %s\n", corpus); return 1; }
    fseek(cf, 0, SEEK_END); long clen = ftell(cf); fseek(cf, 0, SEEK_SET);
    if (clen <= c.seq_len + 1) { fclose(cf); die("corpus shorter than one window"); }
    unsigned char *data = xalloc((size_t)clen, 1);
    if (fread(data, 1, (size_t)clen, cf) != (size_t)clen) { fclose(cf); die("short read on corpus"); }
    fclose(cf);

    /* THE SPLIT IS A CONTIGUOUS TAIL, and `--val-tail N` names it in bytes
     * because a fraction cannot. The corpus this trains on is built by
     * tools/lmcorpus.py, which moves one whole subsystem (c/fs, 284,271 B) to
     * the end precisely so the held-out set is code from a part of the tree
     * the model never saw -- not a random slice with training data on both
     * sides of it, which a model that had merely interpolated between its
     * neighbours would score well on. Passing the exact byte count keeps the
     * trainer's boundary and the corpus builder's boundary the same number:
     * `--val-tail 284271`, and the manifest says where that starts.
     * Without it, 90/10, which is what a plain text file gets. */
    size_t train_hi = (size_t)((double)clen * 0.9);
    if (val_tail > 0) {
        if (val_tail >= clen) die("--val-tail is the whole corpus");
        train_hi = (size_t)clen - (size_t)val_tail;
    }
    size_t val_lo = train_hi;
    int have_val = 1;
    if (train_hi < (size_t)c.seq_len + 2 || (size_t)clen - val_lo < (size_t)c.seq_len + 2) {
        /* NOT silently folded into "validate on the training set": a val loss
         * measured on data the model trained on is the train loss under a
         * different name, and printing it in the val column would make
         * memorisation look like generalisation. */
        train_hi = (size_t)clen; val_lo = 0; have_val = 0;
    }

    /* ------------------------------------------------------------ params */
    struct pdesc *d = xalloc(N_TENS(&c), sizeof *d);
    size_t np = 0;
    int nt = build_desc(&c, d, &np);
    double *P = xalloc(np, sizeof(double));
    double *G = xalloc(np, sizeof(double));
    double *M = xalloc(np, sizeof(double));
    double *V = xalloc(np, sizeof(double));

    /* GPT-2's 0.02 normal for everything that multiplies, gains at 1. That
     * number is not a preference, it is checkable: RMSNorm hands the head a
     * vector of unit RMS per component, so a row of `dim` weights at sd 0.02
     * produces logits of sd sqrt(128)*0.02 = 0.226, and for iid logits of
     * spread s the expected loss is ln(V) + s^2/2 = 5.5452 + 0.0255 = 5.571
     * nats. Measured at step 0: 5.6031, which is that number plus the sample
     * noise of 256 tokens. An init or a loss that is wrong lands somewhere
     * else entirely -- a factor-of-two error in the logit scale, a missing
     * max-shift, a target off by one -- so the first printed loss is the
     * cheapest test in this file and it is printed before any update. */
    for (int i = 0; i < nt; i++) {
        double *p = P + d[i].off;
        size_t n = (size_t)d[i].rows * d[i].cols;
        if (d[i].kind == PK_NORM) for (size_t j = 0; j < n; j++) p[j] = 1.0;
        else                      for (size_t j = 0; j < n; j++) p[j] = 0.02 * nrand();
    }

    /* One activation arena AND one gradient array per worker. The gradient
     * array is the expensive half (np doubles, 35 MB at 4.4M params) and the
     * alternative -- one shared G with an atomic add per element -- is not a
     * trade-off, it is slower AND non-deterministic: the sum order would
     * depend on which core finished first, so two runs of the same seed would
     * differ in the last bits and a bisect would have nothing to hold on to.
     * Private arrays summed in worker order give a run that is reproducible
     * from (seed, threads). Not from seed alone: floating-point addition is
     * not associative, so `--threads 8` and `--threads 24` are different runs.
     * That is stated rather than hidden, and it is why the thread count is
     * printed in the header line below. */
    struct act *wa = xalloc(nthreads, sizeof *wa);
    struct act **wap = xalloc(nthreads, sizeof *wap);
    double **wG = xalloc(nthreads, sizeof *wG);
    for (int w = 0; w < nthreads; w++) {
        act_new(&wa[w], &c);
        wap[w] = &wa[w];
        wG[w] = xalloc(np, sizeof(double));
    }
    struct act a = wa[0];       /* worker 0's arena, for the serial paths */

    if (have_val)
        printf("lmtrain: corpus %s (%ld bytes, train 0..%zu, val %zu..%ld = %zu B)\n",
               corpus, clen, train_hi, val_lo, clen, (size_t)clen - val_lo);
    else
        printf("lmtrain: corpus %s (%ld bytes, no held-out split -- too short; "
               "the val column will read 0)\n", corpus, clen);
    printf("lmtrain: dim=%d layers=%d heads=%d kv_heads=%d head_dim=%d hidden=%d "
           "vocab=%d seq=%d tied=%d\n", c.dim, c.n_layers, c.n_heads, c.n_kv_heads,
           c.head_dim, c.hidden, c.vocab, c.seq_len, c.tied);
    printf("lmtrain: %zu parameters (%.2f MB f32), %d steps x %d seq x %d batch = %.0f tokens\n",
           np, (double)np * 4.0 / 1048576.0, steps, c.seq_len, batch,
           (double)steps * c.seq_len * batch);
    printf("lmtrain: ln(vocab) = %.4f is what step 0 must print\n", log((double)c.vocab));
    printf("lmtrain: %d worker(s), %.0f MB of activations + %.0f MB of gradients\n",
           nthreads, (double)nthreads * a.arena_n * 8.0 / 1048576.0,
           (double)nthreads * np * 8.0 / 1048576.0);
    printf("%8s %10s %10s %10s %10s %8s %10s\n",
           "step", "loss", "ema", "val", "lr", "gnorm", "tok/s");
    fflush(stdout);

    /* ------------------------------------------------------------- train */
    const double b1 = 0.9, b2 = 0.95, aeps = 1e-8;
    double gscale = 1.0 / ((double)batch * c.seq_len);
    double t_mark = wall_s();
    long tok_mark = 0;
    double last_val = 0;
    /* ONE sequence's loss is an unbiased estimate of the model's loss with the
     * variance of 256 samples drawn from one 256-byte window, and adjacent
     * windows of prose differ enough that the raw column swings +-0.3 while
     * the model only improves. Both columns are printed rather than the smooth
     * one alone: an EMA lags, so a divergence shows in `loss` first, and a
     * curve nobody can read is a curve nobody checks. Half-life 20 steps. */
    double ema = 0; int ema_seen = 0;
    const double ema_a = 1.0 - pow(0.5, 1.0 / 20.0);
    /* Counted rather than assumed: if the clip fires on every step then the
     * update length is `clip`, not `lr * adam_step`, and the whole schedule
     * above is decoration. That is a thing to know about a run, and it is
     * invisible from the loss curve.
     *
     * COUNTING IT WAS NOT ENOUGH, and the previous run is the evidence: it
     * reported "fired on 2980 of 3000 steps (99.3%)" and there was nothing in
     * the output to say whether that meant the norm was 1.01 or 40. The norm
     * itself is a column now, and the whole distribution is kept -- a clip
     * that fires on 99% of steps at a norm of 1.1 is scaling the gradient by
     * 0.9 and Adam divides that back out; one that fires at a norm of 40 is
     * throwing away 97% of the update and the learning rate printed beside it
     * is fiction. The two are indistinguishable from a hit count. */
    long clip_hits = 0;
    double *gn_hist = xalloc(steps, sizeof(double));
    size_t *boff = xalloc(batch, sizeof(size_t));
    double *bloss = xalloc(batch, sizeof(double));
    double *gnpart = xalloc(nthreads, sizeof(double));

    for (int step = 0; step < steps; step++) {
        /* Linear warmup then cosine to min_lr_frac. Warmup matters more here
         * than the decay does: Adam's first steps take a full-size step in a
         * direction estimated from one gradient, and at lr 1e-3 that is enough
         * to push the residual stream somewhere the norms take hundreds of
         * steps to recover from. */
        double lr;
        if (step < warmup) lr = lr0 * (double)(step + 1) / (double)warmup;
        else {
            double prog = (double)(step - warmup) / (double)(steps > warmup ? steps - warmup : 1);
            if (prog > 1.0) prog = 1.0;
            lr = lr0 * (min_lr_frac + (1.0 - min_lr_frac) * 0.5 * (1.0 + cos(3.14159265358979 * prog)));
        }

        /* THE WINDOW OFFSETS ARE DRAWN SERIALLY, before any worker starts, so
         * the RNG stream is the same sequence of draws whatever --threads
         * says. Drawing inside the parallel loop would make the DATA a
         * function of the thread count, and then two runs that disagree could
         * not be compared at all -- not even loosely, because they would have
         * seen different text. */
        for (int b = 0; b < batch; b++)
            boff[b] = (size_t)(urand() * (double)(train_hi - c.seq_len - 1));

#ifdef _OPENMP
#pragma omp parallel for schedule(static) num_threads(nthreads)
#endif
        for (int b = 0; b < batch; b++) {
#ifdef _OPENMP
            int w = omp_get_thread_num();
#else
            int w = 0;
#endif
            const unsigned char *seq = data + boff[b];
            bloss[b] = lm_forward(&c, d, P, &wa[w], seq, seq + 1, c.seq_len);
            lm_backward(&c, d, P, wG[w], &wa[w], seq, seq + 1, c.seq_len, gscale);
        }
        double loss = 0;
        for (int b = 0; b < batch; b++) loss += bloss[b];   /* fixed order */
        loss *= gscale;
        if (!ema_seen) { ema = loss; ema_seen = 1; } else ema += ema_a * (loss - ema);

        /* Global-norm clip. Per-tensor clipping would change the DIRECTION of
         * the update, not just its length, which is a different optimiser.
         *
         * The per-worker gradients are summed here, in worker order, in the
         * same pass that computes the norm -- and each worker's array is
         * zeroed as it is read, while the cache line is already in hand. The
         * separate memset it replaces was a whole extra sweep over
         * threads*np*8 bytes (840 MB at 24 workers and 4.4M params), which at
         * this machine's memory bandwidth is about as long as it takes to
         * write them in the first place. */
        double gn = 0;
#ifdef _OPENMP
#pragma omp parallel num_threads(nthreads)
#endif
        {
            int tid = 0, nt2 = 1;
#ifdef _OPENMP
            tid = omp_get_thread_num(); nt2 = omp_get_num_threads();
#endif
            /* THE RANGE IS SPLIT BY HAND AND THE PARTIALS ARE SUMMED IN THREAD
             * ORDER, where this was `reduction(+:gn)` and that was a bug --
             * caught by running the same command twice and diffing the two
             * .lm files, which is a check worth keeping in the habit. OpenMP's
             * reduction combines partials in whatever order the threads
             * finish, so `gn` differed in its last bits between two runs of
             * the identical command line; `gmul = clip/gn` then differed, and
             * the weights differed. Most of it hid: the writer rounds to f32,
             * which absorbs a 1-ulp f64 difference nearly every time, so the
             * two 3.3 MB files first differed at byte 2,700,081 and were
             * otherwise equal -- a reproducibility failure that looks exactly
             * like a fluke until you try to bisect one. */
            long lo2 = (long)((np * (size_t)tid) / (size_t)nt2);
            long hi2 = (long)((np * (size_t)(tid + 1)) / (size_t)nt2);
            double s2 = 0;
            for (long jj = lo2; jj < hi2; jj++) {
                double s = 0;
                for (int w = 0; w < nthreads; w++) { s += wG[w][jj]; wG[w][jj] = 0.0; }
                G[jj] = s;
                s2 += s * s;
            }
            gnpart[tid] = s2;
        }
        for (int w = 0; w < nthreads; w++) gn += gnpart[w];
        gn = sqrt(gn);
        gn_hist[step] = gn;
        double gmul = 1.0;
        if (clip > 0 && gn > clip) { gmul = clip / gn; clip_hits++; }

        double bc1 = 1.0 - pow(b1, (double)(step + 1));
        double bc2 = 1.0 - pow(b2, (double)(step + 1));
        /* Parallel over TENSORS rather than over the flat range, because the
         * decay coefficient is a property of the tensor and hoisting the test
         * out of the inner loop is what makes this one memory pass instead of
         * a branch per parameter. Tensors are wildly unequal in size (the
         * embedding is 65k, a norm gain is 256), hence dynamic scheduling. */
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
#endif
        for (int i = 0; i < nt; i++) {
            size_t off = d[i].off, n = (size_t)d[i].rows * d[i].cols;
            /* Decoupled weight decay on everything EXCEPT the norm gains. A
             * decayed gain shrinks toward zero and an RMSNorm gain has no
             * reason to be small; the embedding IS decayed because with
             * LM_TIED it is also the output matrix. */
            double dec = (d[i].kind == PK_NORM) ? 0.0 : wd;
            for (size_t j = off; j < off + n; j++) {
                double g = G[j] * gmul;
                M[j] = b1 * M[j] + (1.0 - b1) * g;
                V[j] = b2 * V[j] + (1.0 - b2) * g * g;
                double mh = M[j] / bc1, vh = V[j] / bc2;
                P[j] -= lr * (mh / (sqrt(vh) + aeps) + dec * P[j]);
            }
        }

        tok_mark += (long)batch * c.seq_len;
        int last = (step == steps - 1);
        if (step % every == 0 || last) {
            if (have_val && (step % val_every == 0 || last))
                last_val = eval_loss(&c, d, P, &a, data, val_lo, (size_t)clen,
                                     val_batches, 987654321ULL);
            /* WALL CLOCK, NOT clock(). clock() is CPU time on glibc, so with
             * 24 workers it counts 24 seconds per second and the tok/s column
             * would read the SINGLE-THREADED rate no matter how many cores
             * were running -- a speedup measurement that cannot see a speedup.
             * CLOCK_MONOTONIC where there is one; time() (whole seconds, so a
             * short --every reads 0) is the fallback and says so. */
            double now = wall_s();
            double dt = now - t_mark;
            double tps = dt > 0 ? (double)tok_mark / dt : 0;
            printf("%8d %10.4f %10.4f %10.4f %10.2e %8.2f %10.0f\n",
                   step, loss, ema, last_val, lr, gn, tps);
            fflush(stdout);
            t_mark = now; tok_mark = 0;
        }
    }

    {
        /* The distribution, not just the hit count -- see the clip_hits
         * comment. Sorted copy; steps is at most a few tens of thousands. */
        double *sorted = xalloc(steps, sizeof(double));
        memcpy(sorted, gn_hist, (size_t)steps * sizeof(double));
        for (int i = 1; i < steps; i++) {          /* insertion sort: O(n^2)
            * but n is the step count and this runs once, after a run measured
            * in minutes. qsort would need a comparator and a cast. */
            double v = sorted[i]; int j = i - 1;
            while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
            sorted[j + 1] = v;
        }
        printf("lmtrain: gradient clip (%.2f) fired on %ld of %d steps (%.1f%%)\n",
               clip, clip_hits, steps, steps ? 100.0 * (double)clip_hits / steps : 0.0);
        printf("lmtrain: grad norm  min %.3f  p10 %.3f  median %.3f  p90 %.3f  max %.3f\n",
               sorted[0], sorted[steps / 10], sorted[steps / 2],
               sorted[(steps * 9) / 10], sorted[steps - 1]);
        if (clip > 0 && sorted[steps / 2] > clip)
            printf("lmtrain: NOTE -- the median gradient norm is %.2fx the clip, so the\n"
                   "  typical update is `clip`-sized and the lr schedule above is scaled\n"
                   "  by %.3f on a typical step. Raise --clip or lower --lr.\n",
                   sorted[steps / 2] / clip, clip / sorted[steps / 2]);
        free(sorted);
    }

    /* ---------------------------------------------------------- report q8 */
    double val_f32 = have_val
        ? eval_loss(&c, d, P, &a, data, val_lo, (size_t)clen, val_batches, 987654321ULL) : 0;

    /* What Q8 COSTS, measured rather than asserted: round-trip every matmul
     * weight through the exact quantiser the writer uses and re-evaluate. The
     * number that matters is the loss, not the weight error -- a large relative
     * error on a near-zero weight is free and a small one on a large weight is
     * not, and only the loss knows the difference. */
    double *Pq = xalloc(np, sizeof(double));
    memcpy(Pq, P, np * sizeof(double));
    {
        size_t maxn = 0, maxrows = 0;
        for (int i = 0; i < nt; i++) {
            size_t n = (size_t)d[i].rows * d[i].cols;
            if (n > maxn) maxn = n;
            if ((size_t)d[i].rows > maxrows) maxrows = d[i].rows;
        }
        float *fb = xalloc(maxn, sizeof(float)), *sb = xalloc(maxrows, sizeof(float));
        int8_t *qb = xalloc(maxn, 1);
        for (int i = 0; i < nt; i++) {
            if (d[i].kind != PK_MAT) continue;
            size_t n = (size_t)d[i].rows * d[i].cols;
            for (size_t j = 0; j < n; j++) fb[j] = (float)Pq[d[i].off + j];
            nn_quantize_q8(qb, sb, fb, d[i].rows, d[i].cols);
            nn_dequantize_q8(fb, qb, sb, d[i].rows, d[i].cols);
            for (size_t j = 0; j < n; j++) Pq[d[i].off + j] = (double)fb[j];
        }
        free(fb); free(sb); free(qb);
    }
    double val_q8 = have_val
        ? eval_loss(&c, d, Pq, &a, data, val_lo, (size_t)clen, val_batches, 987654321ULL) : 0;
    free(Pq);

    /* ------------------------------------------------------------- write */
    size_t nb = 0;
    if (!write_model(outpath, &c, d, nt, P, dtype, &nb)) return 1;
    printf("\nwrote %s  %zu bytes  dtype=%s\n", outpath, nb, dtype == NN_Q8 ? "q8" : "f32");
    if (q8path) {
        size_t nb8 = 0;
        if (!write_model(q8path, &c, d, nt, P, NN_Q8, &nb8)) return 1;
        /* The ratio is only a claim about Q8 when the other file is f32; with
         * --dtype q8 both files are the same thing and "1.00x smaller" would
         * be a measurement of nothing dressed as one. */
        if (dtype == NN_Q8)
            printf("wrote %s  %zu bytes  dtype=q8  (same dtype as --out)\n", q8path, nb8);
        else
            printf("wrote %s  %zu bytes  dtype=q8  (%.2fx smaller than the f32 file)\n",
                   q8path, nb8, (double)nb / (double)nb8);
    }
    printf("val loss  f32 %.4f   q8 %.4f   (q8 costs %+.4f nats)\n",
           val_f32, val_q8, val_q8 - val_f32);

    /* ------------------------------------------------- the number that counts */
    if (have_val && do_bpb) {
        double nb = 0;
        double t0 = wall_s();
        double bpb = eval_bpb(&c, d, P, wap, nthreads, data, val_lo, (size_t)clen,
                              bpb_stride, &nb);
        printf("\nHELD-OUT bits/byte  %.4f   over %.0f bytes [%zu, %ld)"
               "  (stride %d, context %d-%d B)\n",
               bpb, nb, val_lo, clen, bpb_stride,
               c.seq_len - bpb_stride + 1, c.seq_len);
        printf("  = %.0f bytes if it were a compressor; %.2f s to measure\n",
               bpb * nb / 8.0, wall_s() - t0);
        printf("  compare:  gzip -9 and xz -9 on EXACTLY those bytes --\n");
        printf("    tail -c +%zu %s > /tmp/heldout.bin\n", val_lo + 1, corpus);
        printf("    gzip -9 -c /tmp/heldout.bin | wc -c   # x8/%0.f = bits/byte\n", nb);
    }

    /* -------------------------------------------------------- the sample */
    if (sample_len > 0) {
        printf("\n--- greedy sample, %d tokens, prompt %s ---\n", sample_len, prompt);
        sample_greedy(&c, d, P, &a, prompt, sample_len);
        printf("--- end ---\n");
    }

    /* `a` is a shallow copy of wa[0] and shares its arena, so it must NOT be
     * freed here as well -- the workers own everything. */
    for (int w = 0; w < nthreads; w++) { act_free(&wa[w]); free(wG[w]); }
    free(wa); free(wap); free(wG);
    free(gn_hist); free(boff); free(bloss); free(gnpart);
    free(P); free(G); free(M); free(V); free(d); free(data);
    return 0;
}
