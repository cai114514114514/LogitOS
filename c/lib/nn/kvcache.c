/* kvcache.c -- see kvcache.h for what this file is and why it does not touch
 * infer.c. Two independent pieces: the budget arithmetic (pure, no
 * allocation) and the real q8-per-head-per-position cache (one allocation,
 * carved the same way infer.c's arena is).
 *
 *   cc -Ic/lib/nn -O2 -w -c c/lib/nn/kvcache.c
 *   (linked into tests/unit/kvbudget_test.c together with model.c, quant4.c,
 *    tensor.c, matmul.c, ops.c -- see that file's header for the exact
 *    command)
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "kvcache.h"
#include "nn.h"
#include "quant4.h"   /* q4_bytes() -- the real block layout, not a guess;
                       see kvcache.h's LM_W_Q4 comment */

/* --------------------------------------------------------- overflow-safe --
 *
 * model.c has its own copy of exactly this (struct sz / sz_val / sz_mul /
 * sz_add) and it is `static`, so it cannot be called across the file
 * boundary without exporting it for one caller. Fifteen lines duplicated
 * here is cheaper and safer than widening model.c's interface for that --
 * and every number this arithmetic touches can come from a caller exploring
 * a shape nobody has validated yet (that is the whole point of a calculator
 * that runs before the format does), so it gets the same "refuse, do not
 * wrap" treatment model.c gives an untrusted on-disk header. */
struct sz { size_t v; int ok; };
static struct sz szv(size_t v) { struct sz s; s.v = v; s.ok = 1; return s; }
static struct sz szbad(void)   { struct sz s; s.v = 0; s.ok = 0; return s; }
static struct sz szmul(struct sz a, size_t b)
{
    if (!a.ok) return a;
    if (a.v != 0 && b > (size_t)-1 / a.v) return szbad();
    return szv(a.v * b);
}
static struct sz szadd(struct sz a, struct sz b)
{
    if (!a.ok || !b.ok) return szbad();
    if (b.v > (size_t)-1 - a.v) return szbad();
    return szv(a.v + b.v);
}

/* infer.c's take() pads every sub-buffer to a multiple of 4 floats (16-byte
 * SSE2 alignment); replicated here, not called, because the two files must
 * not share a translation unit (infer.c is another workflow's). The point of
 * matching it exactly rather than approximately is kvbudget_test.c's strongest
 * check: with kv_bits = LM_KV_F32, lm_budget_compute's
 * kv_bytes+act_bytes+logit_bytes must equal lm_state_bytes(m) BYTE FOR BYTE
 * for a real header, not "close" -- a real gate comparing two independent
 * implementations of the same arena layout, neither one edited to agree with
 * the other. */
static size_t pad4(size_t nfloat) { return (nfloat + 3u) & ~(size_t)3u; }

/* A norm vector -- ALWAYS f32 regardless of weight_bits, model.h's rule. */
static struct sz vec_bytes(uint32_t n) { return szmul(szv(n), sizeof(float)); }

/* Bytes a [rows,cols] matrix costs, for the ONE weight width the real format
 * cannot name yet (LM_W_Q4 -- see kvcache.h). `qbits` is 32 (plain f32, the
 * embedding when LM_QEMB is off) or 4 (quant4.h's real block scheme,
 * Q4_BLOCK weights/scale, Q4_SYM mode -- the same mode/block a weight
 * quantiser would use, per quant4_test.c). This is the ONLY place this file
 * still computes a matrix's bytes by hand; F32 and Q8 go through
 * lm_expected_size itself (below), because that function already has this
 * arithmetic and calling it is how a change to model.c's payload order
 * cannot silently leave this file computing something else. */
static struct sz sz_matq(uint32_t rows, uint32_t cols, int qbits)
{
    if (qbits == 32)
        return szmul(szmul(szv(rows), cols), 4);
    if (rows > (uint32_t)INT_MAX || cols > (uint32_t)INT_MAX) return szbad();
    size_t b = q4_bytes((int)rows, (int)cols, Q4_BLOCK, Q4_SYM);
    if (!b) return szbad();       /* q4_bytes refuses rows<=0 or cols<=0;
                                   shape_ok already required both positive,
                                   so this only fires on an INT_MAX-adjacent
                                   shape nobody has actually tried yet */
    return szv(b);
}

/* ---------------------------------------------------------------- shape -- */

int lm_shape_from_header(struct lm_shape *out, const struct lm_header *h)
{
    if (out) memset(out, 0, sizeof *out);
    if (!out || !h) return 0;
    /* Reuses model.c's own consistency check rather than a second copy of
     * it: lm_expected_size returns 0 on exactly the header shapes this
     * function must also refuse (zero dims, dim % n_heads, n_heads %
     * n_kv_heads, a field too large to become an int) -- the loader that
     * actually opens files IS the reference for "is this header sane". */
    if (!lm_expected_size(h)) return 0;

    out->dim        = (int)h->dim;
    out->n_layers    = (int)h->n_layers;
    out->n_heads     = (int)h->n_heads;
    out->n_kv_heads  = (int)h->n_kv_heads;
    /* h->head_dim == 0 means "derive it", model.h's own rule -- and
     * lm_expected_size just proved dim % n_heads == 0 in that branch (it is
     * one of the refusals it makes), so the division below is exact. A
     * nonzero h->head_dim is taken as-is, unchecked against dim/n_heads,
     * because the whole point of the field is that the two need not agree
     * (Qwen3-0.6B: head_dim 128, dim/n_heads 64). */
    out->head_dim    = h->head_dim ? (int)h->head_dim
                                    : (int)(h->dim / h->n_heads);
    out->hidden      = (int)h->hidden;
    out->vocab       = (int)h->vocab;
    out->tied        = (h->flags & LM_TIED)   ? 1 : 0;
    out->qknorm      = (h->flags & LM_QKNORM) ? 1 : 0;
    out->qemb        = (h->flags & LM_QEMB)   ? 1 : 0;
    return 1;
}

static int shape_ok(const struct lm_shape *sh)
{
    if (!sh) return 0;
    if (sh->dim <= 0 || sh->n_layers <= 0 || sh->n_heads <= 0 ||
        sh->n_kv_heads <= 0 || sh->head_dim <= 0 || sh->hidden <= 0 ||
        sh->vocab <= 0)
        return 0;
    /* GQA divisibility -- the same requirement infer.c's kv_mul = nh/nkvh
     * needs to be an integer. Note there is deliberately NO dim % n_heads
     * check here: that is a fact about the current on-disk format's
     * head_dim-by-division rule, and this struct exists precisely to price
     * shapes that rule does not hold for (kvcache.h's Qwen3 example). */
    if (sh->n_heads % sh->n_kv_heads) return 0;
    return 1;
}

/* ------------------------------------------------------------- budget --- */

int lm_budget_compute(const struct lm_shape *sh, int seq_len,
                      int weight_bits, int kv_bits, struct lm_budget *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!out) return -5;
    if (!shape_ok(sh)) return -1;
    if (seq_len <= 0) return -2;
    if (weight_bits != LM_W_F32 && weight_bits != LM_W_Q8 && weight_bits != LM_W_Q4)
        return -3;
    if (kv_bits != LM_KV_F32 && kv_bits != LM_KV_Q8)
        return -4;

    uint32_t dim    = (uint32_t)sh->dim;
    uint32_t hidden  = (uint32_t)sh->hidden;
    uint32_t vocab   = (uint32_t)sh->vocab;
    uint32_t hd      = (uint32_t)sh->head_dim;
    /* Same overflow discipline model.c's lm_geom uses for these two products
     * (its own comment: "16 heads of 0x20000000 is a product that wraps to
     * ZERO in 32 bits, sizing every attention tensor at nothing"). Needed
     * here even on the lm_expected_size path below, because the ACTIVATIONS
     * section further down (k/v/att scratch, which is not part of the file)
     * uses kvrows too and must not silently disagree with what
     * lm_expected_size derived internally for the same shape. */
    uint64_t qrows64  = (uint64_t)sh->n_heads    * hd;
    uint64_t kvrows64 = (uint64_t)sh->n_kv_heads * hd;
    if (qrows64 > (uint64_t)INT_MAX || kvrows64 > (uint64_t)INT_MAX) return -6;
    uint32_t qrows  = (uint32_t)qrows64;   /* wq/wo's Q-side width, n_heads*hd */
    uint32_t kvrows = (uint32_t)kvrows64;  /* == kv_dim, n_kv_heads*hd */

    /* -------------------------------------------------------- weights -- */
    struct sz wtotal;
    if (weight_bits == LM_W_F32 || weight_bits == LM_W_Q8) {
        /* Build a real header and ask model.c's own lm_expected_size --
         * not a second copy of its payload-order arithmetic. This is what
         * makes the "format gap" claim in kvcache.h checkable rather than
         * argued: if this header (head_dim set explicitly to a value that
         * disagrees with dim/n_heads, e.g. Qwen3-0.6B) did NOT get accepted,
         * lm_expected_size would return 0 and this function would refuse --
         * see kvbudget_test.c's format-gap section for that assertion run. */
        struct lm_header h;
        memset(&h, 0, sizeof h);
        h.dtype      = (weight_bits == LM_W_F32) ? NN_F32 : NN_Q8;
        h.dim        = dim;
        h.n_layers   = (uint32_t)sh->n_layers;
        h.n_heads    = (uint32_t)sh->n_heads;
        h.n_kv_heads = (uint32_t)sh->n_kv_heads;
        h.hidden     = hidden;
        h.vocab      = vocab;
        h.seq_len    = 1;     /* not part of the file's byte size at all
                               (lm_expected_size never reads it into the
                               total); must be nonzero only to pass ITS
                               own "no zero dims" refusal */
        h.head_dim   = hd;
        h.flags      = (sh->tied   ? LM_TIED   : 0u) |
                       (sh->qknorm ? LM_QKNORM : 0u) |
                       (sh->qemb   ? LM_QEMB   : 0u);
        size_t got = lm_expected_size(&h);
        if (!got) return -6;
        wtotal = szv(got);
    } else {
        /* LM_W_Q4: model.h's `dtype` cannot name this yet (its own comment:
         * "a q4 file is REFUSED rather than mis-sized until the quantiser
         * that defines the block layout exists") -- so this walks the SAME
         * payload order model.c's lm_expected_size does, by hand, but every
         * matmul tensor's bytes come from quant4.h's real `q4_bytes()`
         * rather than a formula invented here. */
        int embq = sh->qemb ? 4 : 32;
        wtotal = sz_matq(vocab, dim, embq);                        /* tok_emb */

        struct sz per_layer = vec_bytes(dim);                       /* att_norm */
        per_layer = szadd(per_layer, sz_matq(qrows, dim, 4));         /* wq [qrows,dim] */
        per_layer = szadd(per_layer, sz_matq(kvrows, dim, 4));        /* wk [kvrows,dim] */
        per_layer = szadd(per_layer, sz_matq(kvrows, dim, 4));        /* wv */
        if (sh->qknorm) {
            per_layer = szadd(per_layer, vec_bytes(hd));               /* q_norm */
            per_layer = szadd(per_layer, vec_bytes(hd));               /* k_norm */
        }
        per_layer = szadd(per_layer, sz_matq(dim, qrows, 4));         /* wo [dim,qrows] */
        per_layer = szadd(per_layer, vec_bytes(dim));                 /* ffn_norm */
        per_layer = szadd(per_layer, sz_matq(hidden, dim, 4));        /* w1 */
        per_layer = szadd(per_layer, sz_matq(hidden, dim, 4));        /* w3 */
        per_layer = szadd(per_layer, sz_matq(dim, hidden, 4));        /* w2 */

        wtotal = szadd(wtotal, szmul(per_layer, (size_t)sh->n_layers));
        wtotal = szadd(wtotal, vec_bytes(dim));                       /* final_norm */
        if (!sh->tied)
            wtotal = szadd(wtotal, sz_matq(vocab, dim, 4));            /* wcls */
    }
    if (!wtotal.ok) return -6;
    out->weight_bytes = wtotal.v;

    /* -------------------------------------------------------------- kv -- */
    size_t kvb = kv_cache_bytes(sh->n_layers, seq_len, sh->n_kv_heads,
                                sh->head_dim, kv_bits);
    if (!kvb) return -7;
    out->kv_bytes = kvb;

    /* ------------------------------------------------------ activations -- *
     * Mirrors infer.c's layout() exactly, buffer for buffer, minus kc/vc
     * (kv_bytes above already counts those) -- see pad4's comment for why
     * exactness here is the point, not a nicety. */
    /* xb is max(dim, qrows) and q is qrows -- NOT dim, which is what these two
     * lines said until infer.c's layout() was taught the decoupled head_dim.
     * The two collapse to dim exactly when head_dim is derived, which is why
     * "mirrors infer.c's layout() exactly" was true of every shape this
     * function had been asked about and false of the one it was written for.
     * The number is not small: at Qwen3-0.6B's 1024/16/128, q alone is 2048
     * floats against dim's 1024. */
    size_t xbw = qrows > dim ? (size_t)qrows : (size_t)dim;
    struct sz act = szmul(szv(pad4((size_t)dim)), 4);          /* x   */
    act = szadd(act, szmul(szv(pad4(xbw)), 4));                 /* xb  */
    act = szadd(act, szmul(szv(pad4((size_t)dim)), 4));         /* xb2 */
    act = szadd(act, szmul(szv(pad4((size_t)hidden)), 4));      /* hb  */
    act = szadd(act, szmul(szv(pad4((size_t)hidden)), 4));      /* hb2 */
    act = szadd(act, szmul(szv(pad4((size_t)qrows)), 4));       /* q   */
    act = szadd(act, szmul(szv(pad4((size_t)kvrows)), 4));      /* k   */
    act = szadd(act, szmul(szv(pad4((size_t)kvrows)), 4));      /* v   */
    struct sz att_elems = szmul(szv((size_t)sh->n_heads), (size_t)seq_len);
    if (!att_elems.ok) return -8;
    act = szadd(act, szmul(szv(pad4(att_elems.v)), 4));         /* att */
    if (!act.ok) return -9;
    out->act_bytes = act.v;

    struct sz logit = szmul(szv(pad4((size_t)vocab)), 4);
    if (!logit.ok) return -10;
    out->logit_bytes = logit.v;

    struct sz gtotal = szadd(szadd(szv(out->weight_bytes), szv(out->kv_bytes)),
                              szadd(szv(out->act_bytes), szv(out->logit_bytes)));
    if (!gtotal.ok) return -11;
    out->total_bytes = gtotal.v;
    return 0;
}

int lm_budget_max_seq(const struct lm_shape *sh, int weight_bits, int kv_bits,
                      size_t ceiling_bytes, int seq_cap)
{
    if (!shape_ok(sh) || seq_cap <= 0) return 0;

    struct lm_budget b;
    if (lm_budget_compute(sh, 1, weight_bits, kv_bits, &b) != 0) return 0;
    if (b.total_bytes > ceiling_bytes) return 0;      /* not even 1 token fits */

    if (lm_budget_compute(sh, seq_cap, weight_bits, kv_bits, &b) == 0 &&
        b.total_bytes <= ceiling_bytes)
        return seq_cap;                                /* the whole cap fits */

    /* Binary search for the LARGEST seq_len that fits. Valid because
     * total_bytes is nondecreasing in seq_len: weight_bytes does not depend
     * on it, and kv_bytes/act_bytes's att term are both linear in it. At
     * most ~log2(seq_cap) calls to lm_budget_compute, so a cap in the
     * millions is still under 25 evaluations. */
    int lo = 1, hi = seq_cap;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;   /* bias up: converge on the fit */
        if (lm_budget_compute(sh, mid, weight_bits, kv_bits, &b) == 0 &&
            b.total_bytes <= ceiling_bytes)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* --------------------------------------------------------------- cache -- */

size_t kv_cache_bytes(int n_layers, int seq_len, int n_kv_heads, int head_dim,
                      int kv_bits)
{
    if (n_layers <= 0 || seq_len <= 0 || n_kv_heads <= 0 || head_dim <= 0)
        return 0;
    if (kv_bits != LM_KV_F32 && kv_bits != LM_KV_Q8) return 0;

    size_t kv_dim = (size_t)n_kv_heads * (size_t)head_dim;
    struct sz elems = szmul(szmul(szv((size_t)n_layers), (size_t)seq_len), kv_dim);
    if (!elems.ok) return 0;

    if (kv_bits == LM_KV_F32) {
        /* K and V are each ONE buffer of the whole cache, padded once as a
         * whole -- matching infer.c's kc/vc, which are each a single take()
         * of n_layers*seq_len*kv_dim floats, not padded per row. */
        struct sz padded = szv(pad4(elems.v));
        struct sz one = szmul(padded, 4);
        struct sz both = szmul(one, 2);
        return both.ok ? both.v : 0;
    }

    /* Q8: int8 payload (1 B/elem, no padding -- matching model.c's own
     * wrap_mat, which places a Q8 tensor's f32 scale row immediately after
     * the int8 payload with no alignment padding either; x86_64 tolerates
     * the resulting unaligned float access and this file does not diverge
     * from a convention the mapped-weight path already ships) plus one f32
     * scale per (layer, pos, kv_head) row. */
    struct sz rows = szmul(szmul(szv((size_t)n_layers), (size_t)seq_len),
                           (size_t)n_kv_heads);
    if (!rows.ok) return 0;
    struct sz kside = szadd(elems, szmul(rows, sizeof(float)));
    struct sz both = szmul(kside, 2);
    return both.ok ? both.v : 0;
}

int kv_cache_new(struct lm_kvcache *c, int n_layers, int seq_len,
                 int n_kv_heads, int head_dim, int kv_bits)
{
    if (!c) return -1;
    memset(c, 0, sizeof *c);
    size_t need = kv_cache_bytes(n_layers, seq_len, n_kv_heads, head_dim, kv_bits);
    if (!need) return -2;

    void *arena = malloc(need);
    if (!arena) return -3;
    /* Zeroed for the diagnostic reason infer.c zeroes its arena (see
     * infer.c's comment at lm_state_new): a caller that reads a
     * (layer,pos,kv_head) row it never wrote gets the same wrong answer
     * every run instead of one that depends on what the heap last held.
     * This module has no causal-mask guarantee of its own (it does not track
     * `pos`; that bookkeeping is the caller's, same as infer.c's `s->pos`),
     * so the zeroing is the only thing standing between a caller bug and
     * silently plausible-looking garbage. */
    memset(arena, 0, need);

    size_t kv_dim = (size_t)n_kv_heads * (size_t)head_dim;
    size_t elems  = (size_t)n_layers * (size_t)seq_len * kv_dim;

    c->dtype      = kv_bits;
    c->n_layers    = n_layers;
    c->seq_len     = seq_len;
    c->n_kv_heads  = n_kv_heads;
    c->head_dim    = head_dim;
    c->arena       = arena;
    c->arena_len   = need;

    if (kv_bits == LM_KV_F32) {
        size_t padded = pad4(elems);
        c->kf = (float *)arena;
        c->vf = c->kf + padded;
    } else {
        size_t rows = (size_t)n_layers * (size_t)seq_len * (size_t)n_kv_heads;
        unsigned char *p = (unsigned char *)arena;
        c->kq = (int8_t *)p;   p += elems;
        c->kscale = (float *)p; p += rows * sizeof(float);
        c->vq = (int8_t *)p;   p += elems;
        c->vscale = (float *)p;
    }
    return 0;
}

void kv_cache_free(struct lm_kvcache *c)
{
    if (!c) return;
    free(c->arena);
    memset(c, 0, sizeof *c);
}

/* One (layer,pos,kv_head) tuple's row index into the [n_layers,seq_len,
 * n_kv_heads] grid -- the same index used by both write and read, so the two
 * cannot disagree about where a row lives. */
static int kvc_row(const struct lm_kvcache *c, int layer, int pos, int kv_head,
                   size_t *out)
{
    if (!c || !c->arena) return 0;
    if (layer < 0 || layer >= c->n_layers) return 0;
    if (pos < 0 || pos >= c->seq_len) return 0;
    if (kv_head < 0 || kv_head >= c->n_kv_heads) return 0;
    *out = ((size_t)layer * (size_t)c->seq_len + (size_t)pos) *
           (size_t)c->n_kv_heads + (size_t)kv_head;
    return 1;
}

void kv_cache_write(struct lm_kvcache *c, int layer, int pos, int kv_head,
                    const float *kvec, const float *vvec)
{
    size_t row;
    if (!kvec || !vvec || !kvc_row(c, layer, pos, kv_head, &row)) return;
    size_t off = row * (size_t)c->head_dim;

    if (c->dtype == LM_KV_F32) {
        memcpy(c->kf + off, kvec, (size_t)c->head_dim * sizeof(float));
        memcpy(c->vf + off, vvec, (size_t)c->head_dim * sizeof(float));
        return;
    }
    /* n=1: nn_quantize_q8 treats kvec as ONE row of head_dim values and
     * writes exactly one scale -- this IS "q8 per head per position", the
     * same per-row-symmetric kernel model.h's weights use, called at the
     * finest granularity it supports (a single row) rather than a second
     * quantiser written for this file. */
    nn_quantize_q8(c->kq + off, c->kscale + row, kvec, 1, c->head_dim);
    nn_quantize_q8(c->vq + off, c->vscale + row, vvec, 1, c->head_dim);
}

void kv_cache_read(const struct lm_kvcache *c, int layer, int pos, int kv_head,
                   float *kvec_out, float *vvec_out)
{
    size_t row;
    if (!kvec_out || !vvec_out || !kvc_row(c, layer, pos, kv_head, &row)) return;
    size_t off = row * (size_t)c->head_dim;

    if (c->dtype == LM_KV_F32) {
        memcpy(kvec_out, c->kf + off, (size_t)c->head_dim * sizeof(float));
        memcpy(vvec_out, c->vf + off, (size_t)c->head_dim * sizeof(float));
        return;
    }
    nn_dequantize_q8(kvec_out, c->kq + off, c->kscale + row, 1, c->head_dim);
    nn_dequantize_q8(vvec_out, c->vq + off, c->vscale + row, 1, c->head_dim);
}

/* ---------------------------------------------------------------- HOOKUP --
 *
 * This file was written under a rule that forbids editing infer.c, so the
 * cache above is proven correct standalone (tests/unit/kvbudget_test.c) and
 * against a shadow forward pass, never against `lm_state` itself. Wiring it
 * in is small and mechanical, for whoever owns infer.c next.
 *
 * THE SECOND CHANGE THIS PARAGRAPH DEMANDED IS DONE, and the demand is left
 * here in past tense rather than deleted because it is the reason the
 * activation block above changed with it. It read: "lm_forward's own shape
 * check (`if (hd * nh != dim ...) return NULL`) still assumes the
 * pre-head_dim-field world and refuses any model where q_dim = n_heads*
 * head_dim differs from dim ... so 'the format can express Qwen3-0.6B'
 * (kvcache.h) and 'infer.c can run it' are two separate claims -- only the
 * first is true yet." Both are true now: infer.c's wq/wo are typed
 * [q_dim,dim] / [dim,q_dim], the invariant is against q_dim, and the refusal
 * that remains is a REAL one (`geom()` refuses a header whose geometry
 * disagrees with the loader's, with LM_STATE_E_GEOM). `make test-lm-infer`
 * runs a forward pass at dim 1024 / 16 heads / head_dim 128 against a double
 * reference, and `test-lm-infer-negctl` restores the derivation and requires
 * exactly 17 checks to redden.
 *
 *   1. Add one field to `struct lm_state` (infer.h): `int kv_bits;` -- or
 *      replace s->kcache/s->vcache's f32 arrays with a `struct lm_kvcache`
 *      by value, which is the layout this file already produces.
 *   2. In lm_forward, the two `memcpy(krow, s->k, ...)` /
 *      `memcpy(vrow, s->v, ...)` lines (infer.c, right after RoPE) become
 *      `kv_cache_write(&s->kv, l, pos, kvh_of(hi)..., s->k, s->v)` --
 *      called once per LAYER with the kv-head loop kv_cache_write already
 *      does internally per call, so the call site changes shape slightly
 *      (once per kv_head, not once for the whole kv_dim vector, since
 *      kv_cache_write's row unit is one kv_head).
 *   3. The attention read loop's `s->kcache + (...)` pointer arithmetic
 *      becomes a `kv_cache_read` into a small on-stack head_dim buffer
 *      before the dot product -- which is the one place q8 costs something
 *      real: a dequantise per (head, timestep) instead of a pointer, i.e.
 *      the same O(seq_len) extra work nn_matvec_q8 already accepts for
 *      quantised WEIGHTS, now paid on every attention step instead of once
 *      per token.
 *   4. lm_state_bytes must switch its kcache/vcache sizing from the
 *      f32-only formula it has today to `kv_cache_bytes(..., s->kv_bits)` --
 *      this file's function, already exercised against exactly that formula
 *      in kvbudget_test.c.
 *
 * None of that is done here. What IS proven here: the budget arithmetic for
 * both cache widths agrees with lm_state_bytes's existing f32 formula
 * exactly (kvbudget_test.c), and the q8 cache's accuracy cost over a real
 * 256-token generation is measured, not assumed (same file).
 */
