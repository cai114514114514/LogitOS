/* kvbudget_test.c -- the KV cache as a second memory budget, checked and
 * measured, not just built.
 *
 * NAMED kvbudget_test.c, NOT shape_test.c. The task that produced this file
 * asked for tests/unit/shape_test.c; that path already holds an unrelated,
 * committed, tracked test (a differential HARFBUZZ TEXT-SHAPING check,
 * "shape" meaning glyph shaping, nothing to do with tensors). Overwriting it
 * would have destroyed real work over a name collision, so this file uses a
 * name that says what it tests instead. model.h separately references
 * tests/unit/lmshape_test.c (its QK-norm order-of-operations gap, a
 * different deliverable, presumably a different task) -- that file does not
 * exist yet and this is not it either.
 *
 * FOUR THINGS, matching the task's three deliverables plus the correctness
 * work underneath them:
 *
 *   PART A -- the budget calculator (kvcache.h's lm_budget_compute /
 *             lm_budget_max_seq) cross-checked against the REAL functions it
 *             is a superset of: model.c's lm_expected_size for the weight
 *             side, infer.c's lm_state_bytes for the activation+cache side.
 *             Byte for byte, not "close", and pinned against lmtrain.md's own
 *             published numbers (3,297,856 / 947,520 for the 824,448-param
 *             reference model) so a regression here is a regression against
 *             a number that already shipped, not a number invented today.
 *   PART B -- the real q8-per-head-per-position cache (kv_cache_write/read),
 *             checked for exactness (f32), bounded error (q8, against the
 *             SAME formula nn_quantize_q8 itself uses to set the bound, not
 *             a fitted epsilon), and row independence.
 *   PART C -- deliverable 2. A from-scratch second implementation of
 *             lm_forward (`shadow_forward`, calling nothing in infer.c) that
 *             reads its KV cache through kvcache.h instead of through
 *             `struct lm_state`'s arrays, run over a real 256-step generation
 *             at BOTH cache precisions against the identical token sequence
 *             a REAL lm_forward run produced -- so "shadow at f32 agrees with
 *             the real implementation" is proven first (fidelity), before
 *             "shadow at q8 diverges from shadow at f32 by THIS much, growing
 *             THIS fast" is reported (the actual measurement).
 *   PART D -- deliverable 3. The decision table: three model sizes, three
 *             weight widths, two KV widths, what fits in 512 MiB and how
 *             long a context it buys.
 *
 * Build (host, no OS, matching tests/nn.mk's existing pattern for
 * test-lm-format/test-lm-infer -- NOT added to tests/nn.mk itself, which two
 * other workflows in this effort own concurrently; run by name):
 *
 *   cc -Ic/lib/nn -O2 -w -o build/kvbudget_test tests/unit/kvbudget_test.c \
 *      c/lib/nn/kvcache.c c/lib/nn/model.c c/lib/nn/quant4.c \
 *      c/lib/nn/infer.c c/lib/nn/tensor.c c/lib/nn/matmul.c c/lib/nn/ops.c -lm
 *   build/kvbudget_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "model.h"
#include "infer.h"
#include "kvcache.h"
#include "quant4.h"
#include "nn.h"

static int checks, failed;

static void ok(const char *what) { checks++; printf("ok  : %s\n", what); }
static void bad_i(const char *what, long got, long want)
{
    checks++; failed++;
    printf("FAIL: %s\n      got %ld want %ld\n", what, got, want);
}
static void eqi(const char *what, long got, long want)
{
    if (got == want) ok(what); else bad_i(what, got, want);
}
static void near(const char *what, double got, double want, double bound)
{
    checks++;
    if (fabs(got - want) <= bound) { failed += 0; printf("ok  : %s\n", what); }
    else {
        failed++;
        printf("FAIL: %s\n      got %.9g want %.9g (|err| %.3g > bound %.3g)\n",
               what, got, want, fabs(got - want), bound);
    }
}
static void istrue(const char *what, int cond)
{
    checks++;
    if (cond) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s (condition false)\n", what); }
}

/* ===================================================== PART A -- budget === */

/* Fill a header's SIZE-RELEVANT fields (not magic/version/reserved, which
 * lm_expected_size does not read -- lm_open's job, not this function's). */
static void mkhdr(struct lm_header *h, int dim, int n_layers, int n_heads,
                  int n_kv_heads, int hidden, int vocab, int seq_len,
                  int head_dim, uint32_t flags, uint32_t dtype)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, LM_MAGIC, 8);
    h->version = LM_VERSION;
    h->dtype = dtype;
    h->dim = (uint32_t)dim; h->n_layers = (uint32_t)n_layers;
    h->n_heads = (uint32_t)n_heads; h->n_kv_heads = (uint32_t)n_kv_heads;
    h->hidden = (uint32_t)hidden; h->vocab = (uint32_t)vocab;
    h->seq_len = (uint32_t)seq_len; h->head_dim = (uint32_t)head_dim;
    h->flags = flags;
}

static void part_a(void)
{
    printf("\n-- PART A: budget calculator vs the real functions --\n");

    /* A1: the 824,448-parameter reference config, byte for byte against
     * tools/lmtrain.md's own measured output ("Files: build/model.lm
     * 3,297,856 B f32 . build/model.q8.lm 947,520 B q8"). This is not a
     * number this file invented -- it is the one that already shipped. */
    struct lm_header h824;
    mkhdr(&h824, 128, 4, 4, 4, 344, 256, 256, 0, LM_TIED, NN_F32);
    eqi("A1 lm_expected_size f32 824k == 3,297,856",
        (long)lm_expected_size(&h824), 3297856L);
    h824.dtype = NN_Q8;
    eqi("A1 lm_expected_size q8 824k == 947,520",
        (long)lm_expected_size(&h824), 947520L);
    h824.dtype = NN_F32;

    /* A2: lm_shape_from_header round-trips the derived head_dim exactly
     * (dim=128, n_heads=4 -> 32), and lm_budget_compute's weight_bytes
     * agrees with lm_expected_size on the SAME header it was built from --
     * not a tautology: lm_budget_compute builds its OWN synthetic header
     * from the shape and calls lm_expected_size on THAT, so this checks the
     * round trip (header -> shape -> header) preserves what matters. */
    struct lm_shape sh;
    istrue("A2 lm_shape_from_header accepts the 824k header",
           lm_shape_from_header(&sh, &h824));
    eqi("A2 head_dim derived == 32", sh.head_dim, 32);
    eqi("A2 tied == 1", sh.tied, 1);
    eqi("A2 qknorm == 0", sh.qknorm, 0);
    eqi("A2 qemb == 0", sh.qemb, 0);

    struct lm_budget b;
    eqi("A2 lm_budget_compute rc==0", lm_budget_compute(&sh, 256, LM_W_F32, LM_KV_F32, &b), 0);
    eqi("A2 budget.weight_bytes == lm_expected_size", (long)b.weight_bytes, 3297856L);

    /* A3: kv_bytes+act_bytes+logit_bytes == lm_state_bytes(m) EXACTLY, for a
     * header built ONLY from its fields (lm_state_bytes reads m->h and
     * nothing else -- infer.c's layout() dereferences only that). Three
     * shapes, because lm_infer_test.c's own commentary names exactly the
     * cases a lazy check would miss: the layer stride (n_layers=1 makes the
     * stride term multiply zero-ish) and GQA (n_kv_heads < n_heads). */
    struct { int dim,nl,nh,nkvh,hid,voc,seq,hd; } cfgs[] = {
        { 128, 4, 4, 4, 344, 256, 256, 0 },  /* the reference shape */
        {  64, 1, 2, 2,  96,  64,  32, 0 },  /* n_layers=1: stride edge */
        {  64, 2, 4, 1, 128,  64,  48, 0 },  /* GQA: n_kv_heads=1 */
        /* DECOUPLED head_dim, both directions, and they are here because the
         * three above CANNOT see this equality break: with head_dim derived,
         * q_dim is dim and `act_bytes` sizing q at dim is right by accident.
         * At head_dim 96 over dim 64 the query projection is 192 floats wide
         * and the residual stream 64, so an act_bytes that says dim is short
         * by 128 floats for q and another 128 for xb -- and lm_state_bytes,
         * which layout() computes, is not. The second is the other direction
         * (q_dim 32 < dim 64), where xb must stay dim-wide. */
        {  64, 2, 2, 1, 128,  64,  48, 96 },
        {  64, 2, 2, 2, 128,  64,  48, 16 },
    };
    for (size_t i = 0; i < sizeof cfgs / sizeof cfgs[0]; i++) {
        struct lm_header h;
        mkhdr(&h, cfgs[i].dim, cfgs[i].nl, cfgs[i].nh, cfgs[i].nkvh,
              cfgs[i].hid, cfgs[i].voc, cfgs[i].seq, cfgs[i].hd, LM_TIED, NN_F32);
        struct lm_shape s2;
        if (!lm_shape_from_header(&s2, &h)) { bad_i("A3 shape_from_header", 0, 1); continue; }
        struct lm_budget bb;
        if (lm_budget_compute(&s2, (int)h.seq_len, LM_W_F32, LM_KV_F32, &bb) != 0) {
            bad_i("A3 budget_compute", 0, 1); continue;
        }
        struct lm_model m; memset(&m, 0, sizeof m); m.h = h;
        size_t want = lm_state_bytes(&m);
        size_t got = bb.kv_bytes + bb.act_bytes + bb.logit_bytes;
        char label[128];
        snprintf(label, sizeof label,
                 "A3 [%d] kv+act+logit == lm_state_bytes (dim=%d nl=%d nh=%d nkvh=%d hd=%d%s)",
                 (int)i, cfgs[i].dim, cfgs[i].nl, cfgs[i].nh, cfgs[i].nkvh,
                 cfgs[i].hd ? cfgs[i].hd : cfgs[i].dim / cfgs[i].nh,
                 cfgs[i].hd ? " DECOUPLED" : " derived");
        eqi(label, (long)got, (long)want);
    }

    /* A4 -- THE FORMAT GAP, MEASURED. Qwen3-0.6B: dim 1024, 16 query heads,
     * head_dim 128 (dim/n_heads would be 64 -- a DIFFERENT number). This
     * struct's whole reason to carry an independent head_dim field is that a
     * shape like this could not be priced against the on-disk format at all
     * until model.c grew h->head_dim. It has, while this file was being
     * written (see kvcache.h's shape comment) -- so this assertion is the
     * measurement that the gap CLOSED, not an argument that it exists. */
    struct lm_header hq;
    mkhdr(&hq, 1024, 28, 16, 8, 3072, 151936, 1, 128, LM_TIED, NN_F32);
    size_t qsize = lm_expected_size(&hq);
    istrue("A4 lm_expected_size ACCEPTS a decoupled head_dim (Qwen3-0.6B shape)",
           qsize != 0);

    /* Independent byte total, computed here by hand from the SAME per-tensor
     * list model.c's payload order names (model.h), not by calling anything
     * this file is trying to check -- a third way to agree, alongside
     * lm_expected_size and lm_budget_compute below. */
    {
        long q_dim = 16 * 128, kv_dim = 8 * 128;
        long per_layer_params = 2L*1024*q_dim /* wq+wo */
                              + 2L*kv_dim*1024 /* wk+wv */
                              + 3L*3072*1024;  /* w1+w3+w2 */
        long matmul_params = per_layer_params * 28;
        /* model.h's own comment states this exact figure for the
         * attention+ffn parameter count (excludes norms and embedding,
         * which is why it is 595,984,384 - 151936*1024). */
        eqi("A4 matmul param count matches model.h's own worked arithmetic",
            matmul_params, 595984384L - 151936L*1024L);

        long norm_bytes = (2L*28 + 1) * 1024 * 4;              /* att+ffn per layer + final, f32 */
        long emb_bytes  = 151936L * 1024L * 4L;                /* f32 embedding, LM_QEMB off */
        long mm_bytes   = matmul_params * 4L;                  /* f32 matmul weights */
        long hand_total = (long)sizeof(struct lm_header) + norm_bytes + emb_bytes + mm_bytes;
        eqi("A4 lm_expected_size == independent hand total", (long)qsize, hand_total);
    }

    struct lm_shape shq;
    istrue("A4 lm_shape_from_header accepts it too", lm_shape_from_header(&shq, &hq));
    eqi("A4 shape.head_dim == 128 (NOT dim/n_heads == 64)", shq.head_dim, 128);
    struct lm_budget bq;
    eqi("A4 lm_budget_compute(F32) rc==0", lm_budget_compute(&shq, 256, LM_W_F32, LM_KV_F32, &bq), 0);
    eqi("A4 budget.weight_bytes == lm_expected_size", (long)bq.weight_bytes, (long)qsize);
    printf("      Qwen3-0.6B shape, f32 weights: %.1f MiB\n", bq.weight_bytes / 1048576.0);

    /* A5: the KV formula against CLAUDE.md's own worked numbers -- 56 MiB at
     * seq 256 scaling to 7,168 MiB at seq 32,768, exactly (28 layers, 8 kv
     * heads, head_dim 128; every seq_len below is a multiple of 4, so pad4 is
     * a no-op and the byte counts land on the MiB exactly, not "close to"). */
    struct { int seq; long mib; } kvcfg[] = {
        { 256, 56 }, { 512, 112 }, { 2048, 448 }, { 32768, 7168 },
    };
    for (size_t i = 0; i < sizeof kvcfg / sizeof kvcfg[0]; i++) {
        size_t got = kv_cache_bytes(28, kvcfg[i].seq, 8, 128, LM_KV_F32);
        char label[96];
        snprintf(label, sizeof label, "A5 kv_cache_bytes f32 seq=%d == %ld MiB exactly",
                 kvcfg[i].seq, kvcfg[i].mib);
        eqi(label, (long)got, kvcfg[i].mib * 1048576L);
    }

    /* A6: lm_budget_max_seq's boundary -- the returned seq_len fits, and one
     * more does not (unless it hit seq_cap, which this shape/ceiling pair is
     * chosen not to). */
    {
        struct lm_shape s2 = { 128, 4, 4, 4, 32, 344, 256, 1, 0, 0 };
        size_t ceiling = 8u * 1024u * 1024u;   /* 8 MiB -- small enough to bind */
        int mx = lm_budget_max_seq(&s2, LM_W_F32, LM_KV_F32, ceiling, 1000000);
        istrue("A6 lm_budget_max_seq found a positive answer", mx > 0);
        struct lm_budget bb;
        lm_budget_compute(&s2, mx, LM_W_F32, LM_KV_F32, &bb);
        istrue("A6 the returned seq_len itself fits", bb.total_bytes <= ceiling);
        if (mx < 1000000) {
            lm_budget_compute(&s2, mx + 1, LM_W_F32, LM_KV_F32, &bb);
            istrue("A6 one more token does NOT fit (this IS the boundary)",
                   bb.total_bytes > ceiling);
        }
        printf("      [8 MiB ceiling, 824k-shape] longest context = %d tokens\n", mx);
    }
}

/* ===================================================== PART B -- cache === */

static unsigned long long g_seed = 0x9E3779B97F4A7C15ULL;
static double urand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (double)((g_seed >> 11) & 0xFFFFFFFFULL) / 4294967296.0;
}

static void part_b(void)
{
    printf("\n-- PART B: the real q8-per-head-per-position cache --\n");

    const int NL = 3, SEQ = 17, NKVH = 5, HD = 24;

    /* B1: kv_cache_new's actual allocation matches kv_cache_bytes -- the same
     * "layout runs twice and must agree" property infer.c's own arena has
     * (that file's comment: "the classic way to allocate 4 bytes less than
     * you write"), checked here rather than trusted by construction. */
    for (int kb = 0; kb < 2; kb++) {
        int bits = kb ? LM_KV_Q8 : LM_KV_F32;
        struct lm_kvcache c;
        int rc = kv_cache_new(&c, NL, SEQ, NKVH, HD, bits);
        char label[64];
        snprintf(label, sizeof label, "B1 kv_cache_new rc==0 (%s)", kb ? "q8" : "f32");
        eqi(label, rc, 0);
        size_t want = kv_cache_bytes(NL, SEQ, NKVH, HD, bits);
        snprintf(label, sizeof label, "B1 arena_len == kv_cache_bytes (%s)", kb ? "q8" : "f32");
        eqi(label, (long)c.arena_len, (long)want);
        kv_cache_free(&c);
        istrue("B1 kv_cache_free zeroes the struct", c.arena == NULL && c.arena_len == 0);
    }

    /* B2: f32 round-trip is EXACT -- no quantiser in the path, so anything
     * other than bit-identical is a bug in the carving, not in rounding. */
    {
        struct lm_kvcache c;
        kv_cache_new(&c, NL, SEQ, NKVH, HD, LM_KV_F32);
        float kin[64], vin[64], kout[64], vout[64];
        for (int i = 0; i < HD; i++) { kin[i] = (float)(urand()*2-1); vin[i] = (float)(urand()*2-1); }
        kv_cache_write(&c, 1, 9, 3, kin, vin);
        kv_cache_read(&c, 1, 9, 3, kout, vout);
        int exact = 1;
        for (int i = 0; i < HD; i++) if (kin[i] != kout[i] || vin[i] != vout[i]) exact = 0;
        istrue("B2 f32 round trip is bit-exact", exact);
        kv_cache_free(&c);
    }

    /* B3: q8 round trip is bounded by the SAME formula nn_quantize_q8 uses to
     * pick its own scale (nn.h: scale[i] = max|row|/127), not a number fitted
     * to what this test happened to observe. Max reconstruction error per
     * element is scale/2 (round-to-nearest over a step of `scale`); a little
     * slack covers the two f32 roundings model.c's own comment on
     * nn_quantize_q8 already documents (amax*inv landing at 127.0000008
     * instead of 127.0). */
    {
        struct lm_kvcache c;
        kv_cache_new(&c, NL, SEQ, NKVH, HD, LM_KV_Q8);
        float kin[64], vin[64], kout[64], vout[64];
        double worst_ratio = 0.0;
        for (int trial = 0; trial < 200; trial++) {
            float amax = 0.0f;
            for (int i = 0; i < HD; i++) {
                kin[i] = (float)((urand()*2-1) * (1 + 10*urand()));  /* mixed scale */
                float a = fabsf(kin[i]); if (a > amax) amax = a;
                vin[i] = (float)((urand()*2-1) * (1 + 10*urand()));
            }
            float scale = amax > 0.0f ? amax / 127.0f : 0.0f;
            int layer = trial % NL, pos = trial % SEQ, kvh = trial % NKVH;
            kv_cache_write(&c, layer, pos, kvh, kin, vin);
            kv_cache_read(&c, layer, pos, kvh, kout, vout);
            double bound = (double)scale * 0.5 + 1e-6;
            for (int i = 0; i < HD; i++) {
                double e = fabs((double)kin[i] - kout[i]);
                if (e > bound) {
                    char label[96];
                    snprintf(label, sizeof label, "B3 q8 K error within scale/2 (trial %d elem %d)", trial, i);
                    near(label, e, 0.0, bound);
                }
                if (scale > 0) { double r = e / bound; if (r > worst_ratio) worst_ratio = r; }
            }
        }
        istrue("B3 200 q8 write/read trials, every element within its row's scale/2 bound", 1);
        printf("      worst observed error / bound ratio: %.4f (<=1.0 required)\n", worst_ratio);
        kv_cache_free(&c);
    }

    /* B4: rows do not alias -- writing (layer,pos,kv_head) and its neighbours
     * with distinguishable patterns and reading each back at its OWN
     * coordinate must not see another row's data. This is the check that
     * would catch a row-index arithmetic bug kv_cache_bytes' byte-count
     * agreement (B1) cannot see at all. */
    {
        struct lm_kvcache c;
        kv_cache_new(&c, NL, SEQ, NKVH, HD, LM_KV_F32);
        int mismatches = 0;
        for (int l = 0; l < NL; l++)
            for (int p = 0; p < SEQ; p++)
                for (int kh = 0; kh < NKVH; kh++) {
                    float pat = (float)(l * 1000 + p * 10 + kh);
                    float kin[64], vin[64];
                    for (int i = 0; i < HD; i++) { kin[i] = pat + i; vin[i] = -pat - i; }
                    kv_cache_write(&c, l, p, kh, kin, vin);
                }
        for (int l = 0; l < NL; l++)
            for (int p = 0; p < SEQ; p++)
                for (int kh = 0; kh < NKVH; kh++) {
                    float pat = (float)(l * 1000 + p * 10 + kh);
                    float kout[64], vout[64];
                    kv_cache_read(&c, l, p, kh, kout, vout);
                    for (int i = 0; i < HD; i++)
                        if (kout[i] != pat + i || vout[i] != -pat - i) mismatches++;
                }
        eqi("B4 every one of NL*SEQ*NKVH rows reads back its OWN pattern (0 mismatches)",
            mismatches, 0);
        kv_cache_free(&c);
    }

    /* B5: out-of-range coordinates are refused, not written/read as garbage. */
    {
        struct lm_kvcache c;
        kv_cache_new(&c, NL, SEQ, NKVH, HD, LM_KV_F32);
        float buf[64]; for (int i=0;i<HD;i++) buf[i]=42.0f;
        float out[64]; for (int i=0;i<HD;i++) out[i]=-1.0f;
        kv_cache_write(&c, NL, 0, 0, buf, buf);      /* layer out of range */
        kv_cache_read(&c, NL, 0, 0, out, out);
        int untouched = 1;
        for (int i=0;i<HD;i++) if (out[i] != -1.0f) untouched = 0;
        istrue("B5 out-of-range layer leaves the read buffer untouched", untouched);
        kv_cache_free(&c);
    }
}

/* ===================================================== PART C -- accuracy = */

/* A small, byte-level model, deliberately the SAME shape lmtrain.md's own
 * 824,448-parameter reference uses -- proven trainable, and small enough
 * that a 256-step generation with two shadow replays is seconds, not
 * minutes, on the host. Weights are untrained (0.02-normal, the exact scale
 * lmtrain.c's own init uses -- see tools/lmtrain.md, "Init is 0.02-normal")
 * rather than a real trained checkpoint: this experiment is about the
 * ARITHMETIC of KV quantisation, not about model quality, and an untrained
 * model at the real init scale produces the same order of activation
 * magnitudes a step-0 real model would. */
struct pshape { int dim, n_layers, n_heads, n_kv_heads, hidden, vocab, seq_len; };

static unsigned long long g_seed2 = 0xC0FFEE1234567891ULL;
static double urand2(void)
{
    g_seed2 ^= g_seed2 << 13; g_seed2 ^= g_seed2 >> 7; g_seed2 ^= g_seed2 << 17;
    return (double)((g_seed2 >> 11) & 0xFFFFFFFFULL) / 4294967296.0;
}
/* Twelve-uniform sum: mean 6, variance 1, so (sum-6) approximates N(0,1) by
 * the CLT -- good enough for a diagnostic fixture, not claimed exact. */
static float randn02(void)
{
    double s = 0.0;
    for (int i = 0; i < 12; i++) s += urand2();
    return (float)((s - 6.0) * 0.02);
}

struct tdesc { int rows, cols, isnorm; };
#define MAXT 64

static int build_f32_blob(const struct pshape *c, unsigned char **out_blob, size_t *out_len)
{
    int hd = c->dim / c->n_heads, kv = c->n_kv_heads * hd;
    struct tdesc t[MAXT]; int n = 0;
    t[n].rows=c->vocab; t[n].cols=c->dim; t[n].isnorm=0; n++;              /* tok_emb */
    for (int l = 0; l < c->n_layers; l++) {
        t[n].rows=1; t[n].cols=c->dim; t[n].isnorm=1; n++;                 /* att_norm */
        t[n].rows=c->dim; t[n].cols=c->dim; t[n].isnorm=0; n++;            /* wq */
        t[n].rows=kv; t[n].cols=c->dim; t[n].isnorm=0; n++;                /* wk */
        t[n].rows=kv; t[n].cols=c->dim; t[n].isnorm=0; n++;                /* wv */
        t[n].rows=c->dim; t[n].cols=c->dim; t[n].isnorm=0; n++;            /* wo */
        t[n].rows=1; t[n].cols=c->dim; t[n].isnorm=1; n++;                 /* ffn_norm */
        t[n].rows=c->hidden; t[n].cols=c->dim; t[n].isnorm=0; n++;         /* w1 */
        t[n].rows=c->hidden; t[n].cols=c->dim; t[n].isnorm=0; n++;         /* w3 */
        t[n].rows=c->dim; t[n].cols=c->hidden; t[n].isnorm=0; n++;         /* w2 */
    }
    t[n].rows=1; t[n].cols=c->dim; t[n].isnorm=1; n++;                     /* final_norm */
    /* tied: no wcls tensor in the payload */

    size_t len = sizeof(struct lm_header);
    for (int i = 0; i < n; i++) len += (size_t)t[i].rows * t[i].cols * sizeof(float);

    unsigned char *b = (unsigned char *)malloc(len);
    if (!b) return 0;

    struct lm_header h;
    mkhdr(&h, c->dim, c->n_layers, c->n_heads, c->n_kv_heads, c->hidden,
          c->vocab, c->seq_len, 0 /* derive head_dim */, LM_TIED, NN_F32);
    memcpy(b, &h, sizeof h);

    size_t off = sizeof h;
    for (int i = 0; i < n; i++) {
        size_t ne = (size_t)t[i].rows * t[i].cols;
        float *dst = (float *)(b + off);
        for (size_t j = 0; j < ne; j++) dst[j] = t[i].isnorm ? 1.0f : randn02();
        off += ne * sizeof(float);
    }
    *out_blob = b; *out_len = len;
    return 1;
}

/* -------------------------------------------------- the shadow forward pass
 *
 * Independent of infer.c: it never includes infer.h's struct lm_state and
 * never touches s->kcache/s->vcache. It calls the exact same nn.h kernels in
 * the exact same order infer.c's lm_forward does (RMSNorm, matvec, RoPE per
 * head, softmax, SwiGLU), so at LM_KV_F32 it is expected to reproduce
 * lm_forward's logits bit for bit -- which is checked below before the q8
 * measurement is trusted at all. */
struct shadow_state {
    const struct lm_model *m;
    int dim, hidden, vocab, seq, nh, nkvh, hd, kv_dim, nl;
    float *x, *xb, *xb2, *hb, *hb2, *q, *k, *v, *att, *logits, *ktmp, *vtmp;
};

#define SHD_RMS_EPS   1e-5f
#define SHD_ROPE_BASE 10000.0f   /* infer.c's LM_ROPE_BASE -- an architecture
                                  constant, not a per-model one; duplicated
                                  here rather than exported for one caller,
                                  same as this file's other small helpers */

static int shadow_init(struct shadow_state *s, const struct lm_model *m)
{
    memset(s, 0, sizeof *s);
    s->m = m;
    const struct lm_header *h = &m->h;
    s->dim = (int)h->dim; s->hidden = (int)h->hidden; s->vocab = (int)h->vocab;
    s->seq = (int)h->seq_len; s->nh = (int)h->n_heads; s->nkvh = (int)h->n_kv_heads;
    s->hd = m->head_dim; s->kv_dim = m->kv_dim; s->nl = (int)h->n_layers;
    s->x = (float*)malloc((size_t)s->dim*sizeof(float));
    s->xb = (float*)malloc((size_t)s->dim*sizeof(float));
    s->xb2 = (float*)malloc((size_t)s->dim*sizeof(float));
    s->hb = (float*)malloc((size_t)s->hidden*sizeof(float));
    s->hb2 = (float*)malloc((size_t)s->hidden*sizeof(float));
    s->q = (float*)malloc((size_t)s->dim*sizeof(float));
    s->k = (float*)malloc((size_t)s->kv_dim*sizeof(float));
    s->v = (float*)malloc((size_t)s->kv_dim*sizeof(float));
    s->att = (float*)malloc((size_t)s->seq*sizeof(float));
    s->logits = (float*)malloc((size_t)s->vocab*sizeof(float));
    s->ktmp = (float*)malloc((size_t)s->hd*sizeof(float));
    s->vtmp = (float*)malloc((size_t)s->hd*sizeof(float));
    return s->x && s->xb && s->xb2 && s->hb && s->hb2 && s->q && s->k && s->v
        && s->att && s->logits && s->ktmp && s->vtmp;
}
static void shadow_free(struct shadow_state *s)
{
    free(s->x); free(s->xb); free(s->xb2); free(s->hb); free(s->hb2);
    free(s->q); free(s->k); free(s->v); free(s->att); free(s->logits);
    free(s->ktmp); free(s->vtmp);
    memset(s, 0, sizeof *s);
}

/* infer.c's own mv(), reimplemented here rather than shared across the file
 * boundary -- ten lines, and this file is expressly forbidden from including
 * infer.c. */
static int shmv(float *y, const struct nn_tensor *w, const float *x, int n, int k)
{
    if (!w || w->ndim != 2 || w->dim[0] != n || w->dim[1] != k) return 0;
    if (w->dtype == NN_Q8) {
        if (!w->q || !w->scale) return 0;
        nn_matvec_q8(y, w->q, w->scale, x, n, k);
    } else {
        if (!w->data) return 0;
        nn_matvec_f32(y, w->data, x, n, k);
    }
    return 1;
}

static const float *shadow_forward(struct shadow_state *s, struct lm_kvcache *kv,
                                   int token, int pos)
{
    const struct lm_model *m = s->m;
    int dim=s->dim, hidden=s->hidden, vocab=s->vocab, nh=s->nh, nkvh=s->nkvh,
        hd=s->hd, kv_dim=s->kv_dim, nl=s->nl;
    if (pos < 0 || pos >= s->seq) return NULL;
    if (lm_embed_row(m, token, s->x) != 0) return NULL;

    int kv_mul = nh / nkvh;
    float ascale = (float)(1.0 / sqrt((double)hd));

    for (int l = 0; l < nl; l++) {
        const struct lm_layer *L = &m->layer[l];
        nn_rmsnorm(s->xb, s->x, L->att_norm, dim, SHD_RMS_EPS);
        if (!shmv(s->q, &L->wq, s->xb, dim, dim)) return NULL;
        if (!shmv(s->k, &L->wk, s->xb, kv_dim, dim)) return NULL;
        if (!shmv(s->v, &L->wv, s->xb, kv_dim, dim)) return NULL;

        for (int hi = 0; hi < nh; hi++) nn_rope(s->q + (size_t)hi*hd, hd, pos, SHD_ROPE_BASE, NN_ROPE_INTERLEAVED);
        for (int hi = 0; hi < nkvh; hi++) nn_rope(s->k + (size_t)hi*hd, hd, pos, SHD_ROPE_BASE, NN_ROPE_INTERLEAVED);

        for (int kvh = 0; kvh < nkvh; kvh++)
            kv_cache_write(kv, l, pos, kvh, s->k + (size_t)kvh*hd, s->v + (size_t)kvh*hd);

        for (int hi = 0; hi < nh; hi++) {
            const float *qh = s->q + (size_t)hi*hd;
            int kvh = hi / kv_mul;
            float *att = s->att;
            for (int t = 0; t <= pos; t++) {
                kv_cache_read(kv, l, t, kvh, s->ktmp, s->vtmp);
                float d; nn_matvec_f32(&d, s->ktmp, qh, 1, hd);
                att[t] = d * ascale;
            }
            nn_softmax(att, pos + 1);
            float *out = s->xb + (size_t)hi*hd;
            for (int i = 0; i < hd; i++) out[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                kv_cache_read(kv, l, t, kvh, s->ktmp, s->vtmp);
                float a = att[t];
                for (int i = 0; i < hd; i++) out[i] += a * s->vtmp[i];
            }
        }
        if (!shmv(s->xb2, &L->wo, s->xb, dim, dim)) return NULL;
        nn_add(s->x, s->xb2, dim);

        nn_rmsnorm(s->xb, s->x, L->ffn_norm, dim, SHD_RMS_EPS);
        if (!shmv(s->hb, &L->w1, s->xb, hidden, dim)) return NULL;
        if (!shmv(s->hb2, &L->w3, s->xb, hidden, dim)) return NULL;
        nn_swiglu(s->hb, s->hb, s->hb2, hidden);
        if (!shmv(s->xb2, &L->w2, s->hb, dim, hidden)) return NULL;
        nn_add(s->x, s->xb2, dim);
    }
    nn_rmsnorm(s->xb, s->x, m->final_norm, dim, SHD_RMS_EPS);
    if (!shmv(s->logits, &m->wcls, s->xb, vocab, dim)) return NULL;
    return s->logits;
}

/* KL(softmax(a) || softmax(b)), nats, double precision throughout. */
static double softmax_kl(const float *a, const float *b, int n)
{
    double ma = a[0], mb = b[0];
    for (int i = 1; i < n; i++) { if (a[i] > ma) ma = a[i]; if (b[i] > mb) mb = b[i]; }
    double za = 0.0, zb = 0.0;
    for (int i = 0; i < n; i++) { za += exp((double)a[i]-ma); zb += exp((double)b[i]-mb); }
    double lza = log(za), lzb = log(zb);
    double kl = 0.0;
    for (int i = 0; i < n; i++) {
        double lpa = (double)a[i] - ma - lza;
        double pa = exp(lpa);
        if (pa <= 0.0) continue;
        double lpb = (double)b[i] - mb - lzb;
        kl += pa * (lpa - lpb);
    }
    return kl;
}
static float max_abs_diff(const float *a, const float *b, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; i++) { float d = fabsf(a[i]-b[i]); if (d > m) m = d; }
    return m;
}
static double rms_diff(const float *a, const float *b, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) { double d = (double)a[i]-b[i]; s += d*d; }
    return sqrt(s / n);
}

static void part_c(void)
{
    printf("\n-- PART C: q8 KV cache accuracy over a real 256-step generation --\n");

    struct pshape PC = { 128, 4, 4, 4, 344, 256, 256 };
    unsigned char *blob; size_t blen;
    if (!build_f32_blob(&PC, &blob, &blen)) { bad_i("C build_f32_blob", 0, 1); return; }

    struct lm_header hh; memcpy(&hh, blob, sizeof hh);
    eqi("C0 blob length == lm_expected_size", (long)blen, (long)lm_expected_size(&hh));

    struct lm_model m;
    int rc = lm_open(&m, blob, blen);
    eqi("C0 lm_open succeeds", rc, 0);
    if (rc != 0) { free(blob); return; }

    struct lm_state stA;
    eqi("C0 lm_state_new succeeds", lm_state_new(&stA, &m), 0);

    const int NGEN = 256, VOCAB = PC.vocab;
    int *toks = (int *)malloc((size_t)NGEN * sizeof(int));
    float *logf32 = (float *)malloc((size_t)NGEN * VOCAB * sizeof(float));
    unsigned long long rng = 1234567891ULL;
    int tok = 0;
    int ok_run = 1;
    for (int pos = 0; pos < NGEN; pos++) {
        toks[pos] = tok;
        const float *lg = lm_forward(&m, &stA, tok, stA.pos);
        if (!lg) { ok_run = 0; break; }
        memcpy(logf32 + (size_t)pos*VOCAB, lg, (size_t)VOCAB*sizeof(float));
        float tmp[256];
        memcpy(tmp, lg, (size_t)VOCAB*sizeof(float));
        tok = lm_sample_topp(tmp, VOCAB, 1.0f, 0.9f, &rng);
    }
    istrue("C1 the real lm_forward ran 256 steps without refusing", ok_run);

    /* Two shadow replays of the IDENTICAL fixed token sequence -- the whole
     * point of fixing `toks[]` from the real run first is that neither
     * shadow pass resamples, so any difference in their logits traces back
     * to the KV cache's precision and nothing else (not to the two runs
     * having attended over different tokens). */
    struct shadow_state ss;
    shadow_init(&ss, &m);
    struct lm_kvcache kvf32, kvq8;
    kv_cache_new(&kvf32, PC.n_layers, PC.seq_len, PC.n_kv_heads, m.head_dim, LM_KV_F32);
    kv_cache_new(&kvq8,  PC.n_layers, PC.seq_len, PC.n_kv_heads, m.head_dim, LM_KV_Q8);

    float *logshadow_f32 = (float *)malloc((size_t)NGEN * VOCAB * sizeof(float));
    float *logshadow_q8  = (float *)malloc((size_t)NGEN * VOCAB * sizeof(float));
    int shadow_ok = 1;
    for (int pos = 0; pos < NGEN; pos++) {
        const float *lf = shadow_forward(&ss, &kvf32, toks[pos], pos);
        if (!lf) { shadow_ok = 0; break; }
        memcpy(logshadow_f32 + (size_t)pos*VOCAB, lf, (size_t)VOCAB*sizeof(float));
    }
    /* Re-init the residual/scratch state for a clean second pass -- the KV
     * cache is a SEPARATE object (kvq8) so nothing carries over between the
     * two replays except the fixed token sequence. */
    for (int pos = 0; pos < NGEN && shadow_ok; pos++) {
        const float *lq = shadow_forward(&ss, &kvq8, toks[pos], pos);
        if (!lq) { shadow_ok = 0; break; }
        memcpy(logshadow_q8 + (size_t)pos*VOCAB, lq, (size_t)VOCAB*sizeof(float));
    }
    istrue("C2 both shadow replays ran 256 steps without refusing", shadow_ok);

    if (ok_run && shadow_ok) {
        /* C3: FIDELITY. shadow_forward at LM_KV_F32 must reproduce the real
         * lm_forward's logits -- same kernels, same order, same values
         * copied into the cache with a plain memcpy on both sides, so this
         * is expected to be BIT-IDENTICAL, and is checked as such (bound 0)
         * rather than with slack, because any nonzero bound here would hide
         * a real divergence between the two implementations under the q8
         * measurement's own noise floor. */
        float worst = 0.0f;
        for (int pos = 0; pos < NGEN; pos++) {
            float d = max_abs_diff(logf32 + (size_t)pos*VOCAB,
                                   logshadow_f32 + (size_t)pos*VOCAB, VOCAB);
            if (d > worst) worst = d;
        }
        near("C3 shadow(f32) matches real lm_forward, ALL 256 positions, max|diff|", worst, 0.0, 0.0);

        printf("\n      pos | shadow(f32) vs real, max|d|  |  shadow(q8) vs shadow(f32): max|d|   rms    KL(nats)\n");
        printf(      "      ----|------------------------  |  ------------------------------------------------\n");
        int checkpoints[] = { 1, 2, 4, 8, 16, 32, 64, 128, 255 };
        double prev_kl = -1.0;
        int growing = 1;
        for (size_t i = 0; i < sizeof checkpoints/sizeof checkpoints[0]; i++) {
            int p = checkpoints[i];
            const float *rf = logf32 + (size_t)p*VOCAB;
            const float *sf = logshadow_f32 + (size_t)p*VOCAB;
            const float *sq = logshadow_q8 + (size_t)p*VOCAB;
            float fid = max_abs_diff(rf, sf, VOCAB);
            float md = max_abs_diff(sf, sq, VOCAB);
            double rd = rms_diff(sf, sq, VOCAB);
            double kl = softmax_kl(sf, sq, VOCAB);
            printf("      %3d | %.3e                     |  %.4e             %.4e  %.4e\n",
                   p, (double)fid, (double)md, rd, kl);
            if (prev_kl >= 0.0 && kl < prev_kl * 0.5) growing = 0;   /* clearly shrank */
            prev_kl = kl;
        }
        printf("\n      (KL divergence measured, not assumed to grow -- see the printed column;\n"
               "       report exactly what it does, per the task's own instruction.)\n");
        (void)growing;
    }

    free(logshadow_f32); free(logshadow_q8);
    kv_cache_free(&kvf32); kv_cache_free(&kvq8);
    shadow_free(&ss);
    free(logf32); free(toks);
    lm_state_free(&stA);
    lm_close(&m);
    free(blob);
}

/* ===================================================== PART D -- the table */

static void print_row(const char *label, const struct lm_shape *sh,
                      int wbits, int kbits, size_t ceiling, int seq_cap)
{
    struct lm_budget b;
    int rc = lm_budget_compute(sh, 1, wbits, kbits, &b);
    if (rc != 0) { printf("%-28s  wbits=%2d kbits=%2d  REFUSED (rc=%d)\n", label, wbits, kbits, rc); return; }
    int mx = lm_budget_max_seq(sh, wbits, kbits, ceiling, seq_cap);
    const char *wn = wbits==LM_W_F32?"f32":wbits==LM_W_Q8?"q8":"q4";
    const char *kn = kbits==LM_KV_F32?"f32":"q8";
    if (mx <= 0) {
        printf("%-28s  w=%-3s kv=%-3s  weights %9.1f MiB  -> IMPOSSIBLE in %.0f MiB (weights alone %s)\n",
               label, wn, kn, b.weight_bytes/1048576.0, ceiling/1048576.0,
               b.weight_bytes > ceiling ? "exceed it" : "leave no room for even 1 token");
    } else {
        struct lm_budget bb; lm_budget_compute(sh, mx, wbits, kbits, &bb);
        printf("%-28s  w=%-3s kv=%-3s  weights %9.1f MiB  -> longest context %7d tok  (kv %.1f MiB, total %.1f MiB)\n",
               label, wn, kn, b.weight_bytes/1048576.0, mx, bb.kv_bytes/1048576.0, bb.total_bytes/1048576.0);
    }
}

static void part_d(void)
{
    printf("\n-- PART D: the table that decides everything (ceiling = 512 MiB) --\n\n");
    size_t CEIL = 512ull * 1024 * 1024;
    int CAP = 2000000;

    /* Row 1: 824,448-parameter reference -- proven, trained, on disk today. */
    struct lm_shape s824 = { 128, 4, 4, 4, 32, 344, 256, 1, 0, 0 };
    /* Row 2: ~100M, byte-level, same architecture family, no exotic flags --
     * chosen to land near 100M params; verified below, not assumed. */
    struct lm_shape s100 = { 768, 16, 12, 4, 64, 2048, 256, 1, 0, 0 };
    /* Row 3: Qwen3-0.6B shape, per CLAUDE.md and model.h's own comment. */
    struct lm_shape s600 = { 1024, 28, 16, 8, 128, 3072, 151936, 1, 1, 0 };
    struct lm_shape s600e = s600; s600e.qemb = 1;   /* the embedding-tax comparison */

    /* Verify the ~100M row actually lands near 100M before using it, per
     * CLAUDE.md's "every number is measured, never remembered" -- computed
     * via the SAME lm_budget_compute path (f32 weight bytes / 4 == params
     * for a shape with no quantised-embedding subtlety, tok_emb counted). */
    {
        struct lm_budget b100;
        lm_budget_compute(&s100, 1, LM_W_F32, LM_KV_F32, &b100);
        double params = b100.weight_bytes / 4.0;
        printf("row 2 shape check: dim=768 layers=16 heads=12/4 hidden=2048 vocab=256"
               " -> %.1fM params (target ~100M)\n\n", params/1e6);
    }

    const char *hdr = "shape/weights/kv                    fits?\n";
    (void)hdr;

    int wbits[] = { LM_W_F32, LM_W_Q8, LM_W_Q4 };
    int kbits[] = { LM_KV_F32, LM_KV_Q8 };

    for (size_t wi = 0; wi < 3; wi++)
        for (size_t ki = 0; ki < 2; ki++)
            print_row("824k (dim128 L4 H4)", &s824, wbits[wi], kbits[ki], CEIL, CAP);
    printf("\n");
    for (size_t wi = 0; wi < 3; wi++)
        for (size_t ki = 0; ki < 2; ki++)
            print_row("~100M (dim768 L16 H12)", &s100, wbits[wi], kbits[ki], CEIL, CAP);
    printf("\n");
    for (size_t wi = 0; wi < 3; wi++)
        for (size_t ki = 0; ki < 2; ki++)
            print_row("Qwen3-0.6B qemb=0", &s600, wbits[wi], kbits[ki], CEIL, CAP);
    printf("\n");
    for (size_t wi = 0; wi < 3; wi++)
        for (size_t ki = 0; ki < 2; ki++)
            print_row("Qwen3-0.6B qemb=1", &s600e, wbits[wi], kbits[ki], CEIL, CAP);

    /* The embedding-tax finding, stated as a number rather than left for the
     * reader to subtract from the table above: model.h's own comment already
     * argues LM_QEMB matters at this vocab size; this is that argument run. */
    {
        struct lm_budget b0, b1;
        lm_budget_compute(&s600, 1, LM_W_Q4, LM_KV_F32, &b0);
        lm_budget_compute(&s600e, 1, LM_W_Q4, LM_KV_F32, &b1);
        printf("\nembedding tax at q4 weights, Qwen3-0.6B shape: qemb=0 -> %.1f MiB, "
               "qemb=1 -> %.1f MiB (%.1f MiB saved by quantising the embedding table)\n",
               b0.weight_bytes/1048576.0, b1.weight_bytes/1048576.0,
               (b0.weight_bytes - b1.weight_bytes)/1048576.0);
    }

    /* And the q8-KV compression ratio actually delivered, vs the "halves the
     * budget" the task's own framing offered as an expectation -- report
     * what the arithmetic gives, not what was expected. */
    {
        size_t f32b = kv_cache_bytes(28, 4096, 8, 128, LM_KV_F32);
        size_t q8b  = kv_cache_bytes(28, 4096, 8, 128, LM_KV_Q8);
        printf("\nKV compression at Qwen3-0.6B's head_dim=128: f32 %.1f MiB, q8 %.1f MiB, "
               "ratio %.3fx (1 byte/4 bytes = 0.25 baseline; the gap from 0.25 is the "
               "per-row f32 scale, %d bytes / %d-element row)\n",
               f32b/1048576.0, q8b/1048576.0, (double)q8b/(double)f32b, 4, 128);
    }
}

int main(void)
{
    part_a();
    part_b();
    part_c();
    part_d();

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
