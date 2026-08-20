/* kvcache.h -- the KV cache as a SECOND memory budget, sized on purpose.
 *
 * WHY THIS FILE EXISTS, stated with the numbers CLAUDE.md already gives:
 * at the Qwen3-0.6B shape (28 layers, 8 kv heads, head_dim 128) the f32 KV
 * cache is 56 MiB at a 256-token context and 7,168 MiB at 32,768 -- on a
 * 512 MiB machine the context length a model can hold is a MEMORY decision,
 * made before generation starts, not a knob turned until something breaks.
 * `lm_state_bytes` (infer.h) already answers "how much for THIS model's
 * seq_len baked into its header"; this file answers the more general
 * question a caller has to ask BEFORE a header exists at all -- "what would
 * a shape like this cost, at a context length and a KV precision I choose,
 * and what is the longest context that fits in what I actually have."
 *
 * TWO THINGS LIVE HERE, and they share a file because the second is what the
 * first is FOR:
 *
 *   1. A pure budget calculator (`lm_budget_compute` / `lm_budget_max_seq`)
 *      that composes weight bytes + KV bytes + activation bytes + logit
 *      bytes from a shape, a context length, and two independently chosen
 *      precisions (the matmul weights' and the KV cache's) -- see
 *      tests/unit/kvbudget_test.c for the table this produces.
 *   2. A real, runnable KV cache (`lm_kvcache` + kv_cache_write/read) at
 *      q8-per-head-per-position, which is what turns "the budget calculator
 *      says q8 is N bytes smaller" into a checkable claim about ACCURACY --
 *      the cache is read back by attention many times over a context, so
 *      the error made by quantising it does not stay where it was made, the
 *      way a quantised WEIGHT's error does. Measuring that is
 *      tests/unit/kvbudget_test.c's other half.
 *
 * NEITHER TOUCHES c/lib/nn/infer.c. infer.c's `lm_state` owns its own f32-only
 * KV cache and is another workflow's file; this module is deliberately
 * standalone -- it depends on nothing but nn.h's tensor kernels (the same
 * per-row-symmetric int8 quantiser model.h's weights use, `nn_quantize_q8` /
 * `nn_dequantize_q8`, so the KV cache's rounding rule is not a second one
 * invented here) and model.h's header shape. Wiring it into `lm_state` --
 * adding a cache-dtype field to `lm_state`, branching `lm_forward`'s cache
 * read/write on it -- is a small, mechanical change to infer.c that this
 * file's owner does not make; see the "HOOKUP" note at the bottom of
 * kvcache.c for exactly what it is.
 */
#ifndef LOGIT_LM_KVCACHE_H
#define LOGIT_LM_KVCACHE_H

#include <stddef.h>
#include <stdint.h>
#include "model.h"

/* KV cache storage width, in BITS PER VALUE -- not nn.h's NN_F32/NN_Q8 dtype
 * enum, on purpose. The KV cache is never a matmul operand (nn_matvec_* never
 * reads it, and never will: attention's dot products are against `hd`-length
 * vectors, not a weight row), so it needs no nn_tensor and no dispatch
 * through that enum. And unlike a weight matrix, it is not a fact about a
 * file on disk -- it is written by THIS run at THIS run's positions, so its
 * precision is a policy chosen when the state is allocated, not a header
 * field. */
enum { LM_KV_F32 = 32, LM_KV_Q8 = 8 };

/* Same reasoning, for the WEIGHT side of the budget calculator only. NN_Q8
 * exists in nn.h; NN_Q4 exists in quant4.h (a different file in this same
 * effort) as a BLOCK scheme -- Q4_BLOCK=32 weights share one scale (and, in
 * Q4_AFFINE mode, one more float for the zero point), not one scale per row
 * the way NN_Q8 is. Because that layout is already real and exported
 * (`q4_bytes(n,k,blk,mode)`, quant4.h), the q4 branch of
 * `lm_budget_compute` below calls it directly rather than re-deriving the
 * block arithmetic here -- the one duplicate-formula trap this file used to
 * have (see git history: an earlier draft of this comment assumed a
 * per-row q4 scale, before quant4.c existed to say otherwise). */
enum { LM_W_F32 = 32, LM_W_Q8 = 8, LM_W_Q4 = 4 };

/* ---------------------------------------------------------------- shape --
 *
 * A superset of lm_header's shape fields, close enough to it now to be
 * built FROM one (`lm_shape_from_header`) or turned back INTO a synthetic
 * one (inline in kvcache.c's `lm_budget_compute`, at the LM_W_F32/LM_W_Q8
 * branch, used to call the real `lm_expected_size` for those two weight
 * widths rather than duplicating model.c's payload-order arithmetic a
 * second time).
 *
 * head_dim IS INDEPENDENT of dim/n_heads, on purpose, mirroring model.h's
 * own `head_dim` field: Qwen3-0.6B is hidden 1024, 16 query heads, head_dim
 * 128, where dim/n_heads would give 64. THIS USED TO BE A GAP IN THE
 * ON-DISK FORMAT -- an earlier draft of this file argued at length that a
 * Qwen3-shaped model could not be written as a v1 LOGITLM file at all, and
 * that argument is why `head_dim`/`q_dim`/`kv_dim` and the `qknorm`/`qemb`
 * flags below exist as fields here rather than being read off `dim`. It
 * shipped as a real header field (model.h, `h->head_dim`, 0 = derive) while
 * this file was being written, closing the gap -- so `lm_shape_from_header`
 * below reads it rather than always deriving it, and kvbudget_test.c's "format
 * gap" section is a MEASUREMENT that it closed (a real header, at the real
 * Qwen3-0.6B shape, accepted by lm_expected_size), not an argument that it
 * exists. */
struct lm_shape {
    int dim;          /* residual stream width */
    int n_layers;
    int n_heads;
    int n_kv_heads;
    int head_dim;      /* INDEPENDENT of dim/n_heads -- model.h's h->head_dim */
    int hidden;         /* FFN intermediate size */
    int vocab;
    int tied;           /* nonzero: no separate wcls tensor (model.h LM_TIED) */
    int qknorm;          /* nonzero: per-layer q_norm/k_norm (model.h LM_QKNORM) */
    int qemb;             /* nonzero: embedding stored at weight_bits, not
                           always f32 (model.h LM_QEMB) -- matters only for
                           weight_bits != LM_W_F32; see model.h's own note on
                           why a 152k-token vocabulary needs this flag */
};

/* Fill *out from a self-consistent LOGITLM header. Returns 0 and leaves *out
 * zeroed on the same refusals lm_expected_size makes (a zero dimension, an
 * unknown flag, dim % n_heads with head_dim left at 0, n_heads % n_kv_heads,
 * or a field too large to become an int) -- so a caller cannot get a shape
 * lm_expected_size would itself have refused; it is in fact CALLED here for
 * exactly that check, not re-derived. Returns 1 on success. */
int lm_shape_from_header(struct lm_shape *out, const struct lm_header *h);

/* ------------------------------------------------------------- budget --
 *
 * Deliverable 1. Every field is a pure function of (shape, seq_len, weight
 * width, kv width): nothing here allocates or reads a file, so a caller can
 * ask "what would this cost" before there is a model, a blob, or even a
 * finished on-disk format to ask lm_expected_size or lm_state_bytes about. */
struct lm_budget {
    size_t weight_bytes;   /* at LM_W_F32/LM_W_Q8, the real LOGITLM file size
                            -- lm_expected_size() itself, called on a header
                            built from *sh, so this is never a second copy of
                            model.c's arithmetic. At LM_W_Q4, model.h's dtype
                            field cannot name it yet (lm_expected_size
                            refuses any dtype it does not know), so this is
                            quant4.h's `q4_bytes()` composed over the same
                            payload order by hand -- a real byte count from
                            the real block-quantiser, not a guessed one */
    size_t kv_bytes;        /* K+V cache, every layer, every position up to
                             seq_len, at the chosen kv_bits */
    size_t act_bytes;       /* everything else lm_state_bytes counts, MINUS
                             the cache: the residual stream and its scratch,
                             the MLP's two hidden buffers, this step's q/k/v,
                             and the attention SCORE row -- n_heads*seq_len
                             floats, which also grows with context and is
                             easy to forget because it is not called "cache" */
    size_t logit_bytes;     /* [vocab] floats, always f32 -- nn_softmax and
                             both samplers in infer.h read logits as f32 */
    size_t total_bytes;     /* the sum, overflow-checked */
};

/* 0 on success. Negative if the shape is not self-consistent (a zero or
 * negative dimension, weight_bits not an LM_W_ value, kv_bits not an LM_KV_
 * value) or the arithmetic does not fit a size_t -- the same refusal
 * discipline model.c's lm_expected_size uses, and for the same reason: every
 * field here can come from a caller exploring a shape that has not been
 * checked against anything yet. On refusal *out is left zeroed. */
int lm_budget_compute(const struct lm_shape *sh, int seq_len,
                      int weight_bits, int kv_bits, struct lm_budget *out);

/* The question CLAUDE.md poses directly: given a byte ceiling, what is the
 * longest seq_len whose budget fits under it? Binary search over
 * lm_budget_compute rather than a closed form: the weight-byte term is
 * seq_len-INDEPENDENT while the kv/act/logit terms are not, and encoding
 * that split as a second, closed-form expression here would be exactly the
 * trap infer.c's own comment names -- "a sizing function beside a carving
 * function... the failure is a heap corruption three subsystems away." One
 * function computes the budget; this one only calls it.
 *
 * Returns the largest seq_len in [1, seq_cap] whose budget.total_bytes fits
 * ceiling_bytes, or 0 if even seq_len=1 does not fit (the weights alone
 * exceed the ceiling) or the shape is inconsistent. seq_cap bounds the
 * search and exists so a caller cannot be handed a search over a range whose
 * own arithmetic overflows before the ceiling does. */
int lm_budget_max_seq(const struct lm_shape *sh, int weight_bits, int kv_bits,
                      size_t ceiling_bytes, int seq_cap);

/* ---------------------------------------------------------------- cache --
 *
 * Deliverable 2. A quantised KV cache, usable on its own -- kv_cache_write/
 * read do not depend on lm_state or lm_forward, and infer.c is not touched
 * by this file at all. Layout is [n_layers][seq_len][n_kv_heads] rows of
 * head_dim values, K and V each their own allocation. "q8 per head per
 * position" is literally one nn_quantize_q8 ROW per (layer, pos, kv_head)
 * tuple -- n=1, k=head_dim -- reusing the exact per-row-symmetric kernel
 * model.h's weight quantiser uses, so the KV cache's rounding rule is not a
 * second one invented here; it is the same rule at a different granularity
 * (one row per scalar-valued (layer,pos,head) triple instead of one row per
 * matrix). */
struct lm_kvcache {
    int dtype;                  /* LM_KV_F32 or LM_KV_Q8 */
    int n_layers, seq_len, n_kv_heads, head_dim;
    float   *kf, *vf;            /* LM_KV_F32: [n_layers,seq_len,n_kv_heads,head_dim] */
    int8_t  *kq, *vq;            /* LM_KV_Q8:  same shape, int8 */
    float   *kscale, *vscale;    /* LM_KV_Q8:  one scale per (layer,pos,kv_head) row */
    void    *arena;              /* the one allocation everything above points into */
    size_t   arena_len;
};

/* Exactly what kv_cache_new will ask for -- a caller can print, refuse, or
 * feed this straight into lm_budget_compute's kv_bytes field (it is the same
 * arithmetic; lm_budget_compute calls this rather than a copy of it). */
size_t kv_cache_bytes(int n_layers, int seq_len, int n_kv_heads, int head_dim,
                      int kv_bits);

/* 0 on success, negative on a bad shape or an allocator refusal. Zeroed on
 * allocation for the same reason infer.c zeroes its arena: the causal mask
 * means no row past the caller's own `pos` bookkeeping is ever read, so
 * zeroing is diagnostic (a cache-indexing bug reads the same wrong number
 * every run) rather than functional. */
int  kv_cache_new(struct lm_kvcache *c, int n_layers, int seq_len,
                  int n_kv_heads, int head_dim, int kv_bits);
void kv_cache_free(struct lm_kvcache *c);

/* Store one position's key/value vectors for one kv head. kvec/vvec are
 * `head_dim` floats each. Quantises on WRITE when the cache is LM_KV_Q8, not
 * on read -- so the rounding this whole deliverable measures is paid exactly
 * once per value, at the position it enters the cache, not once per future
 * token that attends back to it. Re-quantising on every read would multiply
 * the error by up to `seq_len` re-roundings of the same number. Out-of-range
 * layer/pos/kv_head is a no-op (checked, not asserted: a caller exploring a
 * budget has no crash-only failure mode to offer). */
void kv_cache_write(struct lm_kvcache *c, int layer, int pos, int kv_head,
                    const float *kvec, const float *vvec);

/* Fetch one row back into caller-owned head_dim-float buffers, dequantising
 * if the cache is LM_KV_Q8. Always a copy, even for LM_KV_F32, so a caller
 * does not need two code paths for the two dtypes -- this module is a
 * measurement harness, not a hot loop, and the copy is head_dim floats
 * (<=128 at every shape this file was built against). Out-of-range indices
 * leave the output buffers untouched. */
void kv_cache_read(const struct lm_kvcache *c, int layer, int pos, int kv_head,
                   float *kvec_out, float *vvec_out);

#endif /* LOGIT_LM_KVCACHE_H */
