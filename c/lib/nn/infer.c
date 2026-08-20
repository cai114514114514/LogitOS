/* infer.c -- one token in, one row of logits out.
 *
 * ONE ALLOCATION, AND IT IS THE WHOLE DESIGN. `lm_state_new` mallocs exactly
 * once and carves every buffer out of that block; a forward pass allocates
 * nothing at all. That is not frugality for its own sake -- it is what lets
 * this run from the WM's event loop, where there is no sensible failure path:
 * a `kmalloc` that returns NULL halfway through layer 2 leaves a caller with a
 * half-decoded token and nothing to do about it. Deciding the memory once, up
 * front, in a number the caller can print and refuse (`lm_state_bytes`), moves
 * the only failure to the one place that can handle it.
 *
 * THE LAYOUT IS COMPUTED BY THE SAME CODE THAT USES IT. `layout()` below runs
 * twice -- once with a NULL base to total the bytes, once with the real base to
 * assign the pointers -- so `lm_state_bytes(m) == s->arena_len` is true by
 * construction rather than by two hand-kept lists agreeing. The alternative (a
 * sizing function beside a carving function) is the classic way to allocate
 * 4 bytes less than you write, and the failure is a heap corruption three
 * subsystems away. tests/unit/lm_infer_test.c asserts the equality anyway,
 * because "by construction" is a claim too.
 *
 * WHAT THIS FILE DOES NOT CONTAIN: a matmul, a dot product, a softmax or an
 * exponential. Every one of those is a kernel in nn.h with a double-precision
 * reference behind it (`make test-nn`); a second copy here would be a second
 * rounding order with no reference at all. The one arithmetic loop written out
 * below is the attention-weighted sum over the value cache, and it is written
 * out because nn.h has no scaled-add -- see the comment at that loop.
 *
 * RETURN CONVENTION, stated because this library has two. `nn_new` in nn.h
 * returns 1 for success; every `lm_*` entry point (`lm_open`) returns 0 for
 * success and a negative refusal code. This file is `lm_*` and follows
 * `lm_open`: 0 is success, negative is refusal, and each code is distinct so a
 * caller's log says which refusal happened.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>   /* INT_MAX -- geom() below refuses a head geometry that
                       * does not fit the `int` every consumer of it uses */
#include "infer.h"
#include "nn.h"
#include "quant4.h"   /* HOOKUP 1/6: q4_mv, for mv()'s NN_Q4 arm */

/* THE TWO ARCHITECTURE CONSTANTS NOW COME OFF THE FILE. This block used to
 * read:
 *
 *     #define LM_RMS_EPS   1e-5f
 *     #define LM_ROPE_BASE 10000.0f
 *
 * and argued they belong in the code because they are properties of the
 * ARCHITECTURE, "and reading it from the header would let that happen
 * silently". The danger it names is real and is the reason model.h refuses a
 * negative or non-finite value instead of trusting the field -- but the fact
 * was wrong: Qwen3 is this same architecture (RMSNorm, SwiGLU, GQA, rotary)
 * with rope base 1e6 and eps 1e-6, so under the old rule it was not
 * expressible and the converter refused to write it at all.
 *
 * lm_rope_base()/lm_rms_eps() return the OLD constants when the file leaves
 * the fields zero, so every model written before today produces bit-identical
 * output through this function -- which is what tests/unit/lm_infer_test.c's
 * unchanged expectations check. */

/* The largest head_dim whose rope rotation is hoisted onto the stack below.
 * 256 covers every shape this format has held and the one it is aimed at
 * (Qwen3-0.6B is head_dim 128); the table is 2*(hd/2) doubles, so 2 KiB at the
 * bound. It is a FIXED bound and not a VLA because head_dim comes off a disk:
 * `double cs[hd]` on a header-supplied length is an unbounded stack allocation
 * driven by a file, which is a fault a caller cannot catch. Above the bound
 * lm_forward calls nn_rope per head exactly as it always did -- same numbers,
 * just without the hoist -- so the bound costs speed on a shape nobody has and
 * cannot cost correctness on any. */
#define LM_ROPE_MAX_HD 256

#ifdef LM_TRACE_HIDDEN
/* Set by the caller (build/lm_host under -DLM_TRACE_HIDDEN). `l` is -1 for the
 * embedding row and 0..n_layers-1 after that layer's FFN residual add, which
 * is exactly where transformers' output_hidden_states samples. */
void (*lm_trace_hidden)(int l, const float *x, int dim) = 0;
#endif

/* --------------------------------------------------------------- layout --
 *
 * A cursor over the arena. `base == NULL` means "just count", which is the
 * sizing pass; the pointer arithmetic is the only difference between the two
 * passes and that is the point. */
struct carve {
    unsigned char *base;
    size_t         off;
    int            bad;      /* a size that does not fit size_t; see take() */
};

static float *take(struct carve *c, size_t nfloat)
{
    if (c->bad) return NULL;
    /* Every sub-buffer starts on a 16-byte boundary. malloc gives at least
     * that, and the padding keeps it. The padding is at most 12 bytes per
     * buffer, 13 buffers, so at most 156 bytes on an arena that is a megabyte
     * -- and it is INCLUDED in lm_state_bytes because it is included here.
     *
     * THE REASON GIVEN HERE USED TO BE "an unaligned base costs a movups per
     * load", AND THAT IS NO LONGER THE REASON. matmul.c loads with
     * `_mm_loadu_ps`, so the matvec loads are movups WHATEVER the base is --
     * they have to be, because a weight row sits at a model file's offset and
     * is not the arena's to align. (This paragraph said "matmul.c's vector
     * type declares `aligned(4)`" for three minutes, which was true of the
     * revision before the kernel moved to SSE2 intrinsics; same conclusion,
     * different mechanism, and the mechanism is the part a reader would go
     * looking for.) What alignment still buys is
     * real but smaller and different: a 16-byte-aligned activation never
     * straddles a cache line on a 16-byte load, so the sweep costs one line
     * fetch per load rather than occasionally two. Kept for that; the old
     * sentence would have sent someone hunting for a movaps that cannot
     * exist. */
    size_t pad = (nfloat + 3u) & ~(size_t)3u;
    if (pad > ((size_t)-1) / sizeof(float)) { c->bad = 1; return NULL; }
    size_t bytes = pad * sizeof(float);
    if (bytes > ((size_t)-1) - c->off) { c->bad = 1; return NULL; }
    float *p = c->base ? (float *)(c->base + c->off) : NULL;
    c->off += bytes;
    return p;
}

/* a*b with the overflow refused rather than wrapped. A wrapped product here
 * would size the KV cache at a few kilobytes for a header claiming a few
 * exabytes and then write past it -- the single most dangerous arithmetic in
 * the file, because the header comes off a disk. */
static int mul_ok(size_t a, size_t b, size_t *out)
{
    if (a && b > ((size_t)-1) / a) return 0;
    *out = a * b;
    return 1;
}

/* THE HEAD GEOMETRY, AND THE GUARD THAT REPLACES `dim % n_heads`.
 *
 * WHAT THE OLD CHECK WAS: `if (h->dim % h->n_heads) return 0;`, one line, and
 * it DID NOT GUARD. At the shape this file is aimed at -- Qwen3-0.6B, dim
 * 1024 over 16 query heads of head_dim 128 -- the remainder is ZERO, so the
 * check is satisfied BY THE WRONG ANSWER: 1024/16 is 64, half of 128. The KV
 * cache was then sized at half what the forward pass writes and `q` at half
 * what wq produces, and nothing anywhere said no. Measured before the fix, on
 * the qwen3-0.6b preset at vocab 4096 / seq 256: lm_state_bytes reported
 * 28.07 MiB for a state whose KV cache ALONE needs 56.00 MiB. A check that
 * passes on a shape it cannot express is worse than no check, because it
 * reads as coverage.
 *
 * What replaces it constrains three things the old one did not:
 *
 *  1. THE DIVISIBILITY RULE IS REQUIRED ONLY WHERE IT IS THE DEFINITION --
 *     when head_dim is 0 and hd is derived. model.h states exactly this
 *     ("`dim % n_heads == 0` is required ONLY in the derived case"), and
 *     model.c's lm_geom already reads it that way; this is the second reader
 *     of that rule and it now agrees with the first.
 *
 *  2. AN EXPLICIT head_dim IS AN UNTRUSTED u32 OFF A DISK, multiplied by
 *     another one. Every consumer downstream is an `int` -- lm_model's
 *     q_dim/kv_dim, lm_forward's locals, nn_matvec's n and k -- and 16 heads
 *     of 0x20000000 is a product that wraps to ZERO in 32 bits, which would
 *     size every attention buffer at nothing and then index it. So the
 *     products are formed in size_t and refused unless they fit an int.
 *     (With a derived head_dim both are <= dim and this cannot fire, which is
 *     why the old code never needed it.)
 *
 *  3. AGREEMENT, and this is the check whose absence was the bug. Once m has
 *     been through lm_open, m->head_dim/q_dim/kv_dim hold what model.c's
 *     lm_geom computed -- and, more to the point, what every WEIGHT TENSOR
 *     was shaped by. This function computes the same three numbers from the
 *     same header by a SECOND, independent route, and the arena is sized from
 *     this route while it is written by that one. Two independently
 *     maintained structures, one equality: the same discipline rmap.c uses
 *     for `rmap_count(f) == pmm_refcount(f)`, and for the same reason -- a
 *     bug in either side costs a refusal, never a silent mis-size.
 *
 *     It is SKIPPED when m->head_dim is 0, which is the not-yet-opened case
 *     lm_state_bytes exists to serve ("print it and refuse before
 *     allocating"): there is nothing to agree with yet.
 *
 * Returns LM_STATE_OK or a negative code, so lm_state_new and lm_state_why
 * hand a caller the SAME distinguishable answer from one place rather than
 * two functions each mapping "0 bytes" onto a reason of their own. */
static int geom(const struct lm_model *m, size_t *hd_out, size_t *q_out,
                size_t *kv_out)
{
    const struct lm_header *h = &m->h;
#ifdef LM_DERIVE_HEAD_DIM
    /* NEGATIVE CONTROL -- `make test-lm-infer-negctl`. This is the rule as it
     * stood before the fix, whole: head_dim ignored, hd derived from dim, no
     * int bound and no agreement check. It is the OLD BEHAVIOUR rather than a
     * mutation of the new one, so what reddens under it is exactly what the
     * fix bought and not an artefact of how the control was written. */
    if (h->dim % h->n_heads) return LM_STATE_E_HEADER;
    *hd_out = h->dim / h->n_heads;
    *q_out  = h->dim;                       /* wq was [dim,dim] */
    *kv_out = (size_t)h->n_kv_heads * *hd_out;
    return LM_STATE_OK;
#else
    size_t hd;
    if (h->head_dim) {
        hd = h->head_dim;
    } else {
        if (h->dim % h->n_heads) return LM_STATE_E_HEADER;
        hd = h->dim / h->n_heads;
    }
    size_t q, kv;
    if (!mul_ok(h->n_heads,    hd, &q))  return LM_STATE_E_SIZE;
    if (!mul_ok(h->n_kv_heads, hd, &kv)) return LM_STATE_E_SIZE;
    if (hd > (size_t)INT_MAX || q > (size_t)INT_MAX || kv > (size_t)INT_MAX)
        return LM_STATE_E_SIZE;
    if (m->head_dim && ((size_t)m->head_dim != hd ||
                        (size_t)m->q_dim    != q  ||
                        (size_t)m->kv_dim   != kv))
        return LM_STATE_E_GEOM;
    *hd_out = hd; *q_out = q; *kv_out = kv;
    return LM_STATE_OK;
#endif
}

/* Runs twice: once to size, once to place. `s` may be NULL for the sizing
 * pass. Returns the byte total, or 0 if the header is not self-consistent or
 * the arithmetic does not fit -- and, when `why` is non-NULL, WHICH of those
 * it was. `why` is an out-parameter rather than a second entry point because
 * the reason has to come from the code that made the decision; a separate
 * "explain this header" function would be a second copy of every rule below,
 * free to disagree with the one that actually sizes the arena. */
static size_t layout(struct lm_state *s, const struct lm_model *m,
                     unsigned char *base, int *why)
{
    int rc;
#define REFUSE(code) do { if (why) *why = (code); return 0; } while (0)
    if (why) *why = LM_STATE_OK;
    if (!m) REFUSE(LM_STATE_E_ARG);
    const struct lm_header *h = &m->h;
    /* Derived here from the header rather than read from m->head_dim so the
     * answer is available for a header that has been read off a disk but not
     * yet opened -- which is the "print it and refuse before allocating" case
     * infer.h exists for. */
    if (!h->dim || !h->n_heads || !h->n_kv_heads || !h->n_layers ||
        !h->hidden || !h->vocab || !h->seq_len) REFUSE(LM_STATE_E_HEADER);
    if (h->n_heads % h->n_kv_heads) REFUSE(LM_STATE_E_HEADER);

    size_t hd, q_dim, kv_dim;
    rc = geom(m, &hd, &q_dim, &kv_dim);
    if (rc != LM_STATE_OK) REFUSE(rc);

    size_t dim    = h->dim;
    /* xb is written by TWO producers of different widths: nn_rmsnorm fills
     * `dim` of it, and the attention block fills n_heads*head_dim = q_dim.
     * With a derived head_dim those are equal and the distinction is
     * invisible; at the target shape q_dim is 2048 against dim 1024, so the
     * attention loop wrote 4 KiB past the buffer. */
    size_t xbw    = q_dim > dim ? q_dim : dim;
    size_t hidden = h->hidden;
    size_t att, cache, cache1;

    if (!mul_ok(h->n_heads, h->seq_len, &att))       REFUSE(LM_STATE_E_SIZE);
    if (!mul_ok(h->seq_len, kv_dim, &cache1))        REFUSE(LM_STATE_E_SIZE);
    if (!mul_ok(h->n_layers, cache1, &cache))        REFUSE(LM_STATE_E_SIZE);

    struct carve c;
    c.base = base; c.off = 0; c.bad = 0;

    float *x      = take(&c, dim);
    float *xb     = take(&c, xbw);     /* HOOKUP 3/6 */
    float *xb2    = take(&c, dim);
    float *hb     = take(&c, hidden);
    float *hb2    = take(&c, hidden);
    float *q      = take(&c, q_dim);   /* HOOKUP 3/6 */
    float *k      = take(&c, kv_dim);
    float *v      = take(&c, kv_dim);
    float *a      = take(&c, att);
    float *logits = take(&c, h->vocab);
    float *kc     = take(&c, cache);
    float *vc     = take(&c, cache);
    /* `deq` IS SIZED ZERO, DELIBERATELY, and infer.h's "see infer.c on why" is
     * this paragraph. It was there for a path that would need a weight row
     * materialised back into f32. That path does not exist: nn_matvec_q8
     * consumes the quantised matrix whole and applies the row scale once after
     * the accumulation, so no row is ever reconstructed; and the two tensors
     * this file reads elementwise -- the token embedding and the two norm
     * gains -- are f32 on disk by the format's own rule (model.h: "NORMS AND
     * THE EMBEDDING STAY f32 EVEN IN A Q8 FILE"). Sizing it anyway would put
     * `max(dim, hidden, vocab)` floats of arena in every state that no
     * instruction ever touches. It is NULL rather than a zero-length pointer
     * at the end of the arena so that a future caller who needs it faults at
     * its own first use instead of quietly writing over nothing. */
    if (c.bad) REFUSE(LM_STATE_E_SIZE);
#undef REFUSE

    if (s) {
        s->x = x; s->xb = xb; s->xb2 = xb2;
        s->hb = hb; s->hb2 = hb2;
        s->q = q; s->k = k; s->v = v;
        s->att = a; s->logits = logits;
        s->kcache = kc; s->vcache = vc;
        s->deq = NULL;
    }
    return c.off;
}

size_t lm_state_bytes(const struct lm_model *m)
{
    return layout(NULL, m, NULL, NULL);
}

/* Why lm_state_bytes returned 0 -- LM_STATE_OK when it did not. It exists
 * because lm_state_bytes returns a size_t and a size_t has exactly one way to
 * say no: a caller sizing a model on a 512 MiB machine has to be able to tell
 * "this header is nonsense" from "this header describes a shape whose q_dim
 * does not fit an int" from "the loader and I disagree about the geometry",
 * and it must be able to tell them WITHOUT calling lm_state_new, which
 * allocates. Same codes lm_state_new returns, from the same call, because
 * both go through layout(). */
int lm_state_why(const struct lm_model *m)
{
    int why = LM_STATE_OK;
    (void)layout(NULL, m, NULL, &why);
    return why;
}

int lm_state_new(struct lm_state *s, const struct lm_model *m)
{
    if (!s || !m) return LM_STATE_E_ARG;
    memset(s, 0, sizeof *s);

    int why = LM_STATE_OK;
    size_t need = layout(NULL, m, NULL, &why);
    /* The refusal is layout()'s, passed through rather than flattened to one
     * code -- see lm_state_why. `why ? why : E_HEADER` and not a bare `why`
     * because a zero size with no reason recorded would return 0, i.e.
     * SUCCESS, on the one path that must not: an arena of nothing. */
    if (!need) return why ? why : LM_STATE_E_HEADER;
    unsigned char *p = (unsigned char *)malloc(need);
    if (!p) return LM_STATE_E_ALLOC;
    /* Zeroed, and the reason is diagnostic rather than functional: the causal
     * mask means no byte of the cache past `pos` is ever read, so garbage
     * there cannot change an answer (lm_infer_test.c pins exactly that). What
     * zeroing buys is that a cache-INDEXING bug produces the same wrong number
     * every run instead of one that depends on what the heap last held. */
    memset(p, 0, need);

    size_t got = layout(s, m, p, NULL);
    if (got != need) {                    /* cannot happen -- same inputs, same
                                           * code path. Checked because "cannot
                                           * happen" is how an arena overrun
                                           * gets shipped. */
        free(p);
        memset(s, 0, sizeof *s);
        return LM_STATE_E_CARVE;
    }
    s->m = m;
    s->pos = 0;
    s->arena = p;
    s->arena_len = need;
    return 0;
}

void lm_state_free(struct lm_state *s)
{
    if (!s) return;
    free(s->arena);
    memset(s, 0, sizeof *s);
}

void lm_state_reset(struct lm_state *s)
{
    if (!s) return;
    /* The cache is NOT wiped, and that is a decision. Forgetting is complete
     * because attention reads only t <= pos, so with pos back at 0 nothing
     * that was written is reachable. Wiping it would be a memset whose only
     * observable effect is on a debugger -- and it would HIDE a mask bug,
     * which is the one failure a reset could plausibly expose. */
    s->pos = 0;
}

/* ------------------------------------------------------------- the pass -- */

/* y[n] = W[n,k] . x[k], dispatched on the tensor's own dtype rather than on
 * the header's, because model.h lets the norms and the embedding stay f32 in a
 * Q8 file and a per-tensor dispatch is the only reading true to that.
 *
 * The shape check is not decoration: n and k here come from the header while
 * the tensor's dims come from the loader, and if those two ever disagree the
 * matvec reads off the end of a mapped file. Refusing costs one compare per
 * matvec (nine per layer) and turns a fault in someone else's memory into a
 * NULL return from lm_forward. */
static int mv(float *y, const struct nn_tensor *w, const float *x, int n, int k)
{
    if (!w || w->ndim != 2 || w->dim[0] != n || w->dim[1] != k) return 0;
    /* HOOKUP 4/6. One line: q4 needs a per-block sum of the ACTIVATIONS that
     * q4_matvec takes and does not compute, and q4_mv puts that on its own
     * stack so no buffer has to be threaded through lm_state. quant4.c
     * derives what that costs (under 0.1% of the matvec it precedes). */
    if (w->dtype == NN_Q4) return q4_mv(y, w, x, n, k);
    if (w->dtype == NN_Q8) {
        if (!w->q || !w->scale) return 0;
        nn_matvec_q8(y, w->q, w->scale, x, n, k);
    } else {
        if (!w->data) return 0;
        nn_matvec_f32(y, w->data, x, n, k);
    }
    return 1;
}

const float *lm_forward(const struct lm_model *m, struct lm_state *s,
                        int token, int pos)
{
    if (!m || !s || !s->arena) return NULL;
    /* The state's buffers and its cache were both sized from s->m. Running a
     * different model through them would attend over another model's keys at
     * whatever shape this one happens to have. */
    if (s->m != m) return NULL;

    const struct lm_header *h = &m->h;
    int dim    = (int)h->dim;
    int hidden = (int)h->hidden;
    int vocab  = (int)h->vocab;
    int seq    = (int)h->seq_len;
    int nh     = (int)h->n_heads;
    int nkvh   = (int)h->n_kv_heads;
    int hd     = m->head_dim;
#ifdef LM_DERIVE_HEAD_DIM
    /* NEGATIVE CONTROL, the other half of geom()'s: before the fix there was
     * no q_dim at all -- wq was typed [dim,dim] and wo [dim,dim] -- so the
     * control restores `dim` in both places rather than reading m->q_dim,
     * which is what makes the whole decoupled shape unreachable again. */
    int q_dim  = dim;
#else
    int q_dim  = m->q_dim;
#endif
    int kv_dim = m->kv_dim;
    int nl     = (int)h->n_layers;

    if (hd <= 0 || kv_dim <= 0 || q_dim <= 0 || nkvh <= 0 || nh % nkvh) return NULL;
#ifdef LM_DERIVE_HEAD_DIM
    if (hd * nh != dim || nkvh * hd != kv_dim) return NULL;
#else
    /* HOOKUP 5/6. `hd * nh != dim` was the refusal that made the whole target
     * shape unreachable: Qwen3-0.6B is 16 heads of 128 over a 1024-wide
     * residual stream, so hd*nh is 2048 and dim is 1024, which is the
     * decoupled head_dim the format was extended for. The invariant that
     * actually holds is against q_dim, and it is still checked -- both of
     * these are computed by model.c's lm_geom and must agree with what this
     * file re-derives, or the matvecs read off the end of a mapped file. */
    if (hd * nh != q_dim || nkvh * hd != kv_dim) return NULL;
#endif
    if (token < 0 || token >= vocab) return NULL;
    /* pos is checked against the state, not merely against the cache bound.
     * Accepting a pos the caller invented would attend over cache rows holding
     * some other step's keys, and the output of that is a slightly worse model
     * rather than an error -- which is the failure mode that costs weeks. */
    if (pos != s->pos) return NULL;
    if (pos < 0 || pos >= seq) return NULL;
    if (!m->final_norm || !m->layer) return NULL;

    int kv_mul = nh / nkvh;                 /* query heads per kv head (GQA) */
    float ascale = (float)(1.0 / sqrt((double)hd));

    /* HOOKUP 6/6. `m->tok_emb` is NULL on a perfectly good model whose
     * embedding is quantised (LM_QEMB), and this memcpy would have read int8
     * or packed nibbles as floats -- a model that runs and is nonsense.
     * lm_embed_row branches on the tensor's own dtype and returns negative for
     * one it cannot read, which is refused here rather than defaulted to
     * zeroes. */
    if (lm_embed_row(m, token, s->x) != 0) return NULL;

    /* ONE ROPE ROTATION PER TOKEN, NOT THIRTY-TWO. The rotation depends on
     * (head_dim, pos, base) and on nothing else -- not on the layer, not on
     * which head, not on q versus k -- and all three are fixed for the whole
     * of this call. The loop below used to call nn_rope n_heads + n_kv_heads
     * times per layer, each recomputing the identical hd/2 angles: at the
     * reference shape 32 calls a token, 1,536 double pow/cos/sin, for 16
     * distinct answers. Built once here, they are 48.
     *
     * That ratio is why this is worth a stack buffer at all. ops.c counts it
     * against 823,296 multiply-adds a token, so on the host it was 6% -- but
     * this machine's libm is musl's double-only subset (ops.c's opening
     * paragraph) running under TCG, where a pow is not 30 cycles, and the
     * device is the machine the tokens/s number is for.
     *
     * The buffer is NOT in the arena, and that is deliberate: it lives for
     * one call and dies with the frame, so putting it in the state would add
     * bytes to lm_state_bytes -- a number infer.h exists to let a caller print
     * and refuse -- to describe memory no forward pass holds between calls. */
    /* Read ONCE per forward pass, not per layer: they are constant for the
     * whole call and re-reading them 28 times would invite a future edit to
     * take one of the two from a different place. */
#ifdef LM_IGNORE_HEADER_ARCH
    /* NEGATIVE CONTROL for t_ropebase, and it is the PLAUSIBLE wrong version
     * rather than an absurd one: this is exactly what this file did until the
     * header grew the fields, and it is what any build that missed the change
     * still does. It reads a valid file, runs at full speed, produces finite
     * logits and fluent text -- and encodes position with a base 100x off.
     * `make test-lm-ropebase-negctl` requires it to redden the pos>0 check and
     * to leave the pos-0 check PASSING, because a control that reddens
     * everything has not shown which property is load-bearing. */
    const float rope_base = LM_ROPE_BASE_DFL;
    const float rms_eps   = LM_RMS_EPS_DFL;
#else
    const float rope_base = lm_rope_base(&m->h);
    const float rms_eps   = lm_rms_eps(&m->h);
#endif
    /* WHICH TWO COORDINATES EACH ROTATION PAIRS. Off the file, like the base:
     * both conventions are rotations, so nothing downstream can tell them
     * apart, and the wrong one is fluent and wrong. model.h argues the flag. */
#ifdef LM_IGNORE_HEADER_ARCH
    /* The control ignores the pairing FLAG as well as the two VALUES, because
     * they are the same mistake carried in two different fields and a control
     * that covered only one would certify the other. Interleaved is what this
     * file did before the flag existed. */
    const int rope_pairing = NN_ROPE_INTERLEAVED;
#else
    const int rope_pairing = (m->h.flags & LM_ROPE_NEOX) ? NN_ROPE_NEOX
                                                         : NN_ROPE_INTERLEAVED;
#endif

    double rope_cs[LM_ROPE_MAX_HD];
    int rope_hoist = (hd <= LM_ROPE_MAX_HD);
    if (rope_hoist) nn_rope_build(rope_cs, hd, pos, rope_base);

#ifdef LM_TRACE_HIDDEN
    /* TRACE HOOK -- compiled out entirely unless asked for, so the shipped
     * forward pass has neither a branch nor a symbol for it.
     *
     * It exists because "the logits are wrong" is not a location. The residual
     * stream after every layer is 29 vectors of `dim`, and transformers can
     * hand back exactly the same 29 with output_hidden_states=True, so
     * comparing them turns one wrong number at the end into the INDEX OF THE
     * FIRST LAYER THAT DISAGREES -- which is the difference between reading 28
     * layers of code and reading one. */
    if (lm_trace_hidden) lm_trace_hidden(-1, s->x, dim);
#endif
    for (int l = 0; l < nl; l++) {
        const struct lm_layer *L = &m->layer[l];
        if (!L->att_norm || !L->ffn_norm) return NULL;

        nn_rmsnorm(s->xb, s->x, L->att_norm, dim, rms_eps);

        if (!mv(s->q, &L->wq, s->xb, q_dim,  dim)) return NULL;
        if (!mv(s->k, &L->wk, s->xb, kv_dim, dim)) return NULL;
        if (!mv(s->v, &L->wv, s->xb, kv_dim, dim)) return NULL;

        /* QK-NORM: projection -> per-head RMSNorm -> RoPE (model.h). The
         * descriptors are NULL unless the file set LM_QKNORM, so this is inert
         * on every model written before the flag existed. It must be HERE and
         * not after the rotation: RoPE preserves each head's sum of squares,
         * so the norm FACTOR is the same either way and only the gain vector
         * distinguishes the two orders -- which makes the wrong order
         * indistinguishable from f32 noise at the all-ones gain a test would
         * naturally use. */
        if (L->q_norm) lm_qk_norm(s->q, L->q_norm, nh,   hd, rms_eps);
        if (L->k_norm) lm_qk_norm(s->k, L->k_norm, nkvh, hd, rms_eps);

        /* RoPE IS PER HEAD. The rotation pairs (x[2i], x[2i+1]) within one
         * head's head_dim values and its frequencies are indexed by i within
         * that head, so rotating the whole `dim` vector in one call gives
         * every head but the first the wrong frequencies -- and produces a
         * model that is fluent and wrong, with no error anywhere. */
        if (rope_hoist) {
            for (int hi = 0; hi < nh;   hi++) nn_rope_apply(s->q + (size_t)hi * hd, hd, rope_cs, rope_pairing);
            for (int hi = 0; hi < nkvh; hi++) nn_rope_apply(s->k + (size_t)hi * hd, hd, rope_cs, rope_pairing);
        } else {
            for (int hi = 0; hi < nh;   hi++) nn_rope(s->q + (size_t)hi * hd, hd, pos, rope_base, rope_pairing);
            for (int hi = 0; hi < nkvh; hi++) nn_rope(s->k + (size_t)hi * hd, hd, pos, rope_base, rope_pairing);
        }

        float *krow = s->kcache + ((size_t)l * seq + pos) * kv_dim;
        float *vrow = s->vcache + ((size_t)l * seq + pos) * kv_dim;
        /* Two kv_dim copies per layer. They could be avoided by matvec-ing
         * straight into the cache row, at the cost of leaving s->k and s->v --
         * which infer.h declares -- permanently dead. At the reference shape
         * that is 4 layers x 2 x 128 floats = 4 KiB a token against roughly
         * 800k multiply-adds, so the copy is not where the time goes, and
         * keeping this step's k and v addressable is worth more than it. */
        memcpy(krow, s->k, (size_t)kv_dim * sizeof(float));
        memcpy(vrow, s->v, (size_t)kv_dim * sizeof(float));

        for (int hi = 0; hi < nh; hi++) {
            const float *qh  = s->q + (size_t)hi * hd;
            float       *att = s->att + (size_t)hi * seq;
            int          kvh = hi / kv_mul;
            size_t       off = (size_t)kvh * hd;

            for (int t = 0; t <= pos; t++) {
                const float *kt = s->kcache + ((size_t)l * seq + t) * kv_dim + off;
                /* A dot product is a matvec with n = 1. Calling the kernel for
                 * it costs a call per (head, timestep) -- at the reference
                 * shape and a full context that is 4*4*256 = 4096 calls a
                 * token -- and buys the one thing a hand-written dot here
                 * could not have: the same tested rounding order as every
                 * other dot product in the pass. */
                float d;
                nn_matvec_f32(&d, kt, qh, 1, hd);
                att[t] = d * ascale;
            }
            /* The causal mask is this length and nothing else: att[t] for
             * t > pos is never written and never read, so whatever the cache
             * holds beyond pos cannot reach the output. */
            nn_softmax(att, pos + 1);

            float *out = s->xb + (size_t)hi * hd;
            for (int i = 0; i < hd; i++) out[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                const float *vt = s->vcache + ((size_t)l * seq + t) * kv_dim + off;
                float a = att[t];
                /* THE ONE ARITHMETIC LOOP IN THIS FILE, and it is here because
                 * nn.h has no scaled add: nn_add is y += x with no coefficient.
                 * Adding one would be an op with exactly one caller and its own
                 * reference to write; doing it as (scale into scratch, then
                 * nn_add) would cost a pass over hd floats per timestep to
                 * avoid four lines. Neither is worth it -- but this is the one
                 * rounding order in the pass that `make test-nn` does not
                 * already cover, which is why lm_infer_test.c's reference
                 * recomputes the whole attention block in double. */
                for (int i = 0; i < hd; i++) out[i] += a * vt[i];
            }
        }

        if (!mv(s->xb2, &L->wo, s->xb, dim, q_dim)) return NULL;
        nn_add(s->x, s->xb2, dim);

        nn_rmsnorm(s->xb, s->x, L->ffn_norm, dim, rms_eps);
        if (!mv(s->hb,  &L->w1, s->xb, hidden, dim)) return NULL;
        if (!mv(s->hb2, &L->w3, s->xb, hidden, dim)) return NULL;
        /* out aliases a. Safe by inspection of nn_swiglu: element i reads
         * a[i] and b[i] and then writes out[i], so no write precedes a read of
         * the same index. It is also NECESSARY -- infer.h gives this file two
         * [hidden] buffers and the op needs two inputs and one output. */
        nn_swiglu(s->hb, s->hb, s->hb2, hidden);
        if (!mv(s->xb2, &L->w2, s->hb, dim, hidden)) return NULL;
        nn_add(s->x, s->xb2, dim);
#ifdef LM_TRACE_HIDDEN
        if (lm_trace_hidden) lm_trace_hidden(l, s->x, dim);
#endif
    }

    /* Into xb rather than in place, so s->x is left holding the final residual
     * stream -- the sentence embedding, and the one vector worth looking at
     * when the logits are wrong. The norm costs nothing either way. */
    nn_rmsnorm(s->xb, s->x, m->final_norm, dim, rms_eps);
    if (!mv(s->logits, &m->wcls, s->xb, vocab, dim)) return NULL;

    s->pos = pos + 1;
    return s->logits;
}

/* ------------------------------------------------------------- sampling -- */

int lm_sample_greedy(const float *logits, int n)
{
    if (!logits || n <= 0) return -1;
    int best = 0;
    float bv = logits[0];
    /* Strictly greater, so the FIRST maximum wins a tie. Any rule works as
     * long as it is fixed; this one is the one the test asserts, and a tie is
     * not exotic -- an untrained model's logits are full of them. */
    for (int i = 1; i < n; i++) if (logits[i] > bv) { bv = logits[i]; best = i; }
    return best;
}

/* xorshift64, written here rather than calling rand(): rand()'s sequence is a
 * libc detail and this code links against two different libcs (mini-libc on
 * the device, glibc in the host tests), so a "reproducible from a seed" run
 * would not reproduce across them. */
static unsigned long long xs64(unsigned long long *s)
{
    unsigned long long x = *s;
    /* Zero is xorshift's fixed point: seeded with it the generator emits zero
     * forever, and a sampler that always draws 0.0 always returns the first
     * nucleus member -- which reads as a broken model, not as a bad seed. */
    if (!x) x = 88172645463325252ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* 24 bits scaled by 2^-24: every value is exactly representable in f32 and the
 * result is in [0,1). Taking the LOW bits instead would sample xorshift's
 * weakest lanes; the top 24 of a >>40 are its best. */
static float xs_unit(unsigned long long *s)
{
    return (float)(xs64(s) >> 40) * (1.0f / 16777216.0f);
}

int lm_sample_topp(float *logits, int n, float temp, float topp,
                   unsigned long long *rng)
{
    if (!logits || n <= 0) return -1;
    /* temp <= 0 is greedy, not a division. Zero temperature IS the argmax in
     * the limit, so this is the continuous answer rather than a special case,
     * and it leaves `logits` untouched -- a caller sweeping temperature can
     * therefore call this repeatedly on one buffer only if it re-fills it,
     * which every other branch below forces anyway. */
    if (temp <= 0.0f) return lm_sample_greedy(logits, n);
    if (!rng) return -1;

    /* A divide per element, not a multiply by the reciprocal. 256 divides is
     * not where a token's time goes (the pass above it is ~800k multiply-adds)
     * and 1/temp rounds once more than the divide does, so at temp == 1 the
     * multiply would not be the identity it visibly ought to be. */
    for (int i = 0; i < n; i++) logits[i] /= temp;
    nn_softmax(logits, n);
    /* `logits` now holds probabilities. */

    if (topp > 1.0f) topp = 1.0f;

    /* THE NUCLEUS WITHOUT A SORT. Nucleus sampling is defined over the
     * probabilities in descending order, and the obvious implementation sorts
     * an array of (prob, index) pairs -- which needs n pairs of scratch that
     * this signature has nowhere to put, and infer.h's whole contract is that
     * nothing here allocates. So the descending order is WALKED instead of
     * materialised: each pass takes the largest element that comes after the
     * previous one, under the strict total order
     *
     *     a before b  iff  p_a > p_b, or (p_a == p_b and a < b)
     *
     * which is O(n) per member and O(n*k) for a nucleus of k. Measured cost at
     * this machine's vocabulary: the format is byte-level, n = 256, so the
     * worst case (topp = 1, every token in the nucleus) is 65,536 compares a
     * token, and a peaked distribution is a few hundred. A vocabulary in the
     * tens of thousands would want a partial sort, and the honest fix then is
     * to add a scratch pointer to this signature -- not to malloc here, which
     * would put a failure path in the one function that has no way to report
     * one. */
    float lastp = 0.0f;
    int   lasti = -1;
    int   first = 1;
    double cum = 0.0;
    float pmin = 0.0f;
    int   imin = -1;

    for (;;) {
        int best = -1;
        float bp = 0.0f;
        for (int i = 0; i < n; i++) {
            float p = logits[i];
            if (!first && !(p < lastp || (p == lastp && i > lasti))) continue;
            if (best < 0 || p > bp) { best = i; bp = p; }
        }
        if (best < 0) break;               /* the whole distribution is in */
        cum += (double)bp;
        lastp = bp; lasti = best; first = 0;
        pmin = bp; imin = best;
        if (cum >= (double)topp) break;
    }
    /* The loop admits one member before it tests the mass, so topp <= 0 yields
     * the top-1 nucleus -- i.e. greedy -- rather than the empty set the
     * definition literally gives, which there is nothing to sample from. (A
     * NaN topp fails every mass test instead and admits the whole
     * distribution; that is the safe direction of the two.) */
    if (imin < 0) return lm_sample_greedy(logits, n);

    /* Draw against the nucleus's own mass -- which is what "renormalise" means
     * here; dividing each member by `cum` first would be n divisions to reach
     * the same token. */
    double r = (double)xs_unit(rng) * cum;
    int last_in = imin;
    for (int i = 0; i < n; i++) {
        float p = logits[i];
        /* Membership restated from (pmin, imin), so the draw needs no record
         * of which indices were chosen. This is exactly the set the walk above
         * admitted, under the same total order. */
        if (!(p > pmin || (p == pmin && i <= imin))) continue;
        r -= (double)p;
        if (r < 0.0) return i;
        last_in = i;
    }
    /* Reached only when the subtractions leave r >= 0 by rounding, i.e. the
     * draw landed in the last ulp of the mass. Returning the last member is
     * the correct rounding of that, and returning -1 here would be an error
     * the caller cannot act on. */
    return last_in;
}
