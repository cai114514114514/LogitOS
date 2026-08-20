/* model.c -- LOGITLM loader: point a model at a blob, without copying it.
 *
 * See model.h for the format and the reasons behind it. This file is the one
 * place that walks the fixed payload order into descriptors, and it walks it
 * exactly once, in lm_open. lm_expected_size performs the SAME walk in
 * bytes-only form, first, so a truncated or corrupt file is refused with one
 * error code instead of a fault somewhere in the middle of layer 3.
 *
 * OVERFLOW IS CHECKED, NOT TRUSTED, because every field of the header is an
 * untrusted uint32_t off disk: dim*vocab*4 alone can exceed SIZE_MAX on a
 * 64-bit machine if a corrupt or hostile file sets either field near 2^32,
 * and dim/hidden/vocab/kv_dim all get cast to `int` for nn_wrap_f32/q8's
 * shape array (nn_tensor.dim[] is `int`, not size_t) -- so a header claiming
 * a dimension above INT_MAX would silently become a NEGATIVE shape below
 * with nothing catching it. Both are refused in lm_expected_size, before
 * either matters: a loader that computed the size first and range-checked
 * later would still be doing the size arithmetic on an untrusted number.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE NOW LINKS AGAINST c/lib/nn/quant4.c, and that is a NEW dependency
 * -- said here, at the top, because the symptom of missing it is a link error
 * in somebody else's target rather than anything that reads as a model
 * problem. Every host link line that names model.c must also name quant4.c:
 *
 *     cc ... c/lib/nn/model.c c/lib/nn/quant4.c ...
 *
 * The DEVICE build already needs no change: Makefile:4259's
 * LM_NN_SRC is a $(wildcard) over every .c in c/lib/nn, so it picks quant4.c
 * up on its own.
 * tests/nn.mk's three explicit host link lines (test-lm-format, test-lm-infer,
 * $(BUILD)/lm_host) do not, and each needs the one extra word.
 *
 * WHY THE DEPENDENCY IS NOT AVOIDABLE, since a size walk looks like it should
 * be self-contained: a q4 tensor's byte count is a function of the BLOCK
 * LAYOUT -- block size, tail-block packing, the pad before the scales -- which
 * is defined by the quantiser and nowhere else. Restating it here would put
 * the writer and the reader in disagreement about bytes per tensor the first
 * time either moved, and quant4.h states the cost of that precisely: it reads
 * as a truncated file, i.e. lm_open -5, which points at the disk rather than
 * at the two functions that actually disagree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "model.h"
#include "quant4.h"

/* THE HEADER IS 64 BYTES. Not _Static_assert: this file is also built by the
 * host test's bare `cc` invocation with no -std flag, where C11 is not
 * guaranteed. The illegal-negative-array-size trick has worked since C89 and
 * needs nothing from the compiler beyond "arrays cannot have a negative
 * size, and that is a compile error, not a runtime one". */
typedef char lm_header_is_64_bytes[(sizeof(struct lm_header) == 64) ? 1 : -1];

/* ------------------------------------------------------- overflow-safe size --
 *
 * Every number feeding this arithmetic came off disk. `struct sz` carries an
 * `ok` bit alongside the running total instead of using 0 as a sentinel for
 * "overflowed", because 0 is also a value a real header can drive this
 * arithmetic to at one step (e.g. a Q8 tensor with a genuinely empty
 * dimension, refused elsewhere for its own reason) -- folding the two would
 * make a single zero mean two different failures depending on which one
 * actually happened. */
struct sz { size_t v; int ok; };

static struct sz sz_val(size_t v) { struct sz s; s.v = v; s.ok = 1; return s; }
static struct sz sz_bad(void)     { struct sz s; s.v = 0; s.ok = 0; return s; }

static struct sz sz_mul(struct sz a, size_t b)
{
    if (!a.ok) return a;
    if (a.v != 0 && b > (size_t)-1 / a.v) return sz_bad();
    return sz_val(a.v * b);
}

static struct sz sz_add(struct sz a, struct sz b)
{
    if (!a.ok || !b.ok) return sz_bad();
    if (b.v > (size_t)-1 - a.v) return sz_bad();
    return sz_val(a.v + b.v);
}

/* Bytes a [rows,cols] matrix of `dtype` occupies on disk. NN_Q8 carries one
 * f32 scale per row after the int8 payload, per model.h's layout comment.
 * Only ever called here with a dtype lm_open has already validated as
 * NN_F32, NN_Q8 or NN_Q4. */
static struct sz mat_bytes(uint32_t dtype, uint32_t rows, uint32_t cols)
{
    /* q4 FIRST, and it does its own arithmetic rather than joining the
     * sz_mul/sz_add chain below: q4_mat_bytes already refuses every overflow
     * and every shape this format cannot express, and it returns 0 for all of
     * them -- which is exactly sz_bad(). Re-deriving the block count here to
     * feed the chain would be the second statement of the layout that this
     * file's header explains it must not have. */
    if (dtype == NN_Q4) {
        size_t b = q4_mat_bytes(rows, cols);
        return b ? sz_val(b) : sz_bad();
    }
    struct sz n = sz_mul(sz_val(rows), cols);
    if (dtype == NN_Q8)
        return sz_add(n, sz_mul(sz_val(rows), sizeof(float)));
    return sz_mul(n, sizeof(float));
}

/* A norm vector -- ALWAYS f32, regardless of dtype (model.h). */
static struct sz vec_bytes(uint32_t n) { return sz_mul(sz_val(n), sizeof(float)); }

/* The head geometry, in one place, because three functions need it and a
 * second copy of the "0 means derive it" rule is a second place for it to be
 * got wrong. Returns 0 on a header that cannot be honoured, and on success
 * writes head_dim, q_dim = n_heads*hd and kv_dim = n_kv_heads*hd -- each
 * already known to fit an int. */
static int lm_geom(const struct lm_header *h, uint32_t *hd_out,
                   uint32_t *q_out, uint32_t *kv_out)
{
    uint32_t hd;
    if (h->head_dim) {
        /* dim % n_heads is NOT required here, and that is the point of the
         * field: Qwen3-0.6B is dim 1024 over 16 heads of 128, where the
         * derived rule would give 64 -- the remainder check is SATISFIED, by
         * the wrong answer. A check that passes on a shape it cannot express
         * is worse than no check. */
        hd = h->head_dim;
    } else {
        if (h->dim % h->n_heads != 0) return 0;
        hd = h->dim / h->n_heads;
    }
    if (hd > (uint32_t)INT_MAX) return 0;
    /* q_dim and kv_dim are computed in 64 bits and then required to fit an
     * int. With a derived head_dim both are <= dim and could not overflow; an
     * explicit head_dim is an untrusted uint32 off disk multiplied by another
     * one, and 16 heads of 0x20000000 is a product that wraps to ZERO in 32
     * bits, sizing every attention tensor at nothing. */
    uint64_t q  = (uint64_t)h->n_heads    * hd;
    uint64_t kv = (uint64_t)h->n_kv_heads * hd;
    if (q > (uint64_t)INT_MAX || kv > (uint64_t)INT_MAX) return 0;
    *hd_out = hd; *q_out = (uint32_t)q; *kv_out = (uint32_t)kv;
    return 1;
}

size_t lm_expected_size(const struct lm_header *h)
{
    if (!h) return 0;
    if (h->dtype != NN_F32 && h->dtype != NN_Q8 && h->dtype != NN_Q4) return 0;
    /* AN UNKNOWN FLAG IS A REFUSAL, not something to ignore. Every flag this
     * format has changes the payload LENGTH (LM_TIED removes wcls, LM_QKNORM
     * adds two vectors a layer, LM_QEMB changes the embedding's width), so a
     * reader that ignored one would not read a slightly-wrong model -- it
     * would walk a layout that is not there. That is also what makes the
     * extension safe in the other direction: a build from before this commit
     * REFUSES a QK-norm file (its length does not match) instead of misreading
     * it, which is the same protection `reserved` was put there to give. */
    if (h->flags & ~(uint32_t)LM_FLAGS_KNOWN) return 0;
    if (!h->dim || !h->n_layers || !h->n_heads || !h->n_kv_heads ||
        !h->hidden || !h->vocab || !h->seq_len)
        return 0;
    if (h->n_heads % h->n_kv_heads != 0) return 0;
    /* dim/n_heads/n_kv_heads/hidden/vocab each become an `int` shape entry
     * for nn_wrap_f32/q8 below (and head_dim/kv_dim in lm_model itself are
     * `int`), so every one of them has to fit an int before it is safe to
     * cast. seq_len is not part of the on-disk payload at all -- only
     * infer.c's KV cache is sized from it -- but this is still the one place
     * that reads the header for self-consistency, so it is checked here
     * rather than left for a caller to discover the hard way. */
    if (h->dim > (uint32_t)INT_MAX || h->n_heads > (uint32_t)INT_MAX ||
        h->n_kv_heads > (uint32_t)INT_MAX || h->hidden > (uint32_t)INT_MAX ||
        h->vocab > (uint32_t)INT_MAX || h->seq_len > (uint32_t)INT_MAX)
        return 0;

    uint32_t hd, q_dim, kv_dim;
    if (!lm_geom(h, &hd, &q_dim, &kv_dim)) return 0;

    /* The embedding's dtype, as one expression, because it is read here and
     * again in lm_open and the two must not be able to disagree. */
    uint32_t edt = (h->flags & LM_QEMB) ? h->dtype : (uint32_t)NN_F32;

    struct sz total = sz_val(sizeof(struct lm_header));
    total = sz_add(total, mat_bytes(edt, h->vocab, h->dim));           /* tok_emb */

    struct sz per_layer = vec_bytes(h->dim);                              /* att_norm */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, q_dim, h->dim));    /* wq */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, kv_dim, h->dim));   /* wk */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, kv_dim, h->dim));   /* wv */
    if (h->flags & LM_QKNORM) {
        per_layer = sz_add(per_layer, vec_bytes(hd));                    /* q_norm */
        per_layer = sz_add(per_layer, vec_bytes(hd));                    /* k_norm */
    }
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, h->dim, q_dim));    /* wo */
    per_layer = sz_add(per_layer, vec_bytes(h->dim));                    /* ffn_norm */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, h->hidden, h->dim)); /* w1 */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, h->hidden, h->dim)); /* w3 */
    per_layer = sz_add(per_layer, mat_bytes(h->dtype, h->dim, h->hidden)); /* w2 */

    total = sz_add(total, sz_mul(per_layer, h->n_layers));
    total = sz_add(total, vec_bytes(h->dim));                            /* final_norm */
    if (!(h->flags & LM_TIED))
        total = sz_add(total, mat_bytes(h->dtype, h->vocab, h->dim));    /* wcls */

    return total.ok ? total.v : 0;
}

/* Advance `p` past one [rows,cols] matrix of `dtype`, wrapping it into `out`.
 * Every multiplication here already ran, on these same numbers, inside
 * lm_expected_size -- lm_open only reaches this after `len` has been checked
 * equal to that function's result -- so nothing here re-checks for overflow;
 * the check already happened before a single byte was walked. */
static unsigned char *wrap_mat(struct nn_tensor *out, unsigned char *p,
                                uint32_t dtype, uint32_t rows, uint32_t cols)
{
    int dim[2] = { (int)rows, (int)cols };
    /* One line, and the pointer arithmetic -- payload, the pad, the scales,
     * the minima -- stays in the file that defines it. */
    if (dtype == NN_Q4) return q4_wrap_mat(out, p, rows, cols);
    if (dtype == NN_Q8) {
        size_t n = (size_t)rows * cols;
        float *scale = (float *)(p + n);
        nn_wrap_q8(out, (int8_t *)p, scale, 2, dim);
        return p + n + (size_t)rows * sizeof(float);
    }
    nn_wrap_f32(out, (float *)p, 2, dim);
    return p + (size_t)rows * cols * sizeof(float);
}

int lm_open(struct lm_model *m, const void *blob, size_t len)
{
    /* Every documented refusal code below assumes *m can be written; there is
     * no code for "no struct was given at all", so this collapses into -4
     * rather than dereference a NULL. */
    if (!m) return -4;
    memset(m, 0, sizeof *m);
    if (!blob || len < sizeof(struct lm_header)) return -5;

    struct lm_header h;
    memcpy(&h, blob, sizeof h);   /* blob's alignment is not promised */

    if (memcmp(h.magic, LM_MAGIC, sizeof h.magic) != 0) return -1;
    if (h.version != LM_VERSION) return -2;
    /* THREE, not four. `reserved` lost an entry to `head_dim`, and this bound
     * did not follow it for one build: the loop then read one uint32 PAST the
     * struct, off the caller's stack, and refused four of six models in
     * lm_infer_test with -3 for a reserved field that no longer exists. The
     * count is written as the array's own size for that reason -- the next
     * field taken out of `reserved` cannot leave it behind. */
    for (size_t i = 0; i < sizeof h.reserved / sizeof h.reserved[0]; i++)
        if (h.reserved[i] != 0) return -3;

    /* THE TWO ARCHITECTURE CONSTANTS (model.h). Zero means "the default" and
     * is the only value a writer can mean by silence, so it is not checked
     * here -- but everything else representable IS checked, because these are
     * the two numbers whose corruption produces no symptom. A NaN rope base
     * makes every position encoding NaN and the logits follow; a negative eps
     * can put a negative under the sqrt in rmsnorm. Both would surface as
     * garbage output attributed to the model rather than to the file, which
     * is the failure this format spends its whole `reserved` discipline
     * avoiding.
     *
     * Tested on the BIT PATTERN and not with isfinite(): this TU is built
     * -ffreestanding for the device and math.h is not guaranteed there, and
     * "exponent all ones" is the definition rather than an approximation of
     * it. Sign bit set is refused outright -- neither a rope base nor an eps
     * has a meaningful negative value. */
    { /* 0x7F800000 IS THE EXPONENT FIELD, bits 23..30, and the first draft of
       * this line masked with 0xFF000000 instead -- which includes the SIGN
       * bit and drops the exponent's low bit, so it matched +inf and MISSED
       * EVERY NaN. A NaN rope base is the more dangerous of the two: infinity
       * at least drives the frequencies to zero, while NaN poisons every
       * position encoding and every logit downstream with no error anywhere.
       * Caught by the NaN case in lm_format_test, which is why the two are
       * separate checks there rather than one "non-finite is refused". */
      const uint32_t bits[2] = { h.rope_base_f32, h.rms_eps_f32 };
      for (int i = 0; i < 2; i++) {
          if (bits[i] == 0) continue;                       /* use the default */
          if (bits[i] & 0x80000000u) return -4;             /* negative */
          if ((bits[i] & 0x7F800000u) == 0x7F800000u) return -4; /* inf or NaN */
      } }

    size_t expected = lm_expected_size(&h);
    if (!expected) return -4;
    if (len != expected) return -5;

    /* calloc, not malloc: the C standard requires calloc to fail rather than
     * silently wrap when nmemb * size overflows, which is the one
     * overflow-prone multiplication here that lm_expected_size's arithmetic
     * above does NOT already cover -- the descriptor array's size is a fact
     * about struct lm_layer, not about the file's byte layout. */
    struct lm_layer *layers =
        (struct lm_layer *)calloc(h.n_layers, sizeof(struct lm_layer));
    if (!layers) return -6;

    unsigned char *base = (unsigned char *)blob;
    unsigned char *p = base + sizeof(struct lm_header);

    /* Not recomputed by hand: lm_geom is the same call lm_expected_size just
     * made, so the walk below cannot be built on a different geometry from
     * the one the length was checked against. It cannot fail here -- the
     * expected size was non-zero, which required it to succeed -- and is
     * checked anyway, because "cannot fail" is how a NULL walk gets shipped. */
    uint32_t hd, q_dim, kv_dim;
    if (!lm_geom(&h, &hd, &q_dim, &kv_dim)) { free(layers); return -4; }
    uint32_t edt = (h.flags & LM_QEMB) ? h.dtype : (uint32_t)NN_F32;

    struct nn_tensor emb;
    float *tok_emb = (edt == NN_F32) ? (float *)p : NULL;
    p = wrap_mat(&emb, p, edt, h.vocab, h.dim);

    for (uint32_t L = 0; L < h.n_layers; L++) {
        struct lm_layer *ly = &layers[L];
        ly->att_norm = (float *)p;
        p += (size_t)h.dim * sizeof(float);
        p = wrap_mat(&ly->wq, p, h.dtype, q_dim,  h.dim);
        p = wrap_mat(&ly->wk, p, h.dtype, kv_dim, h.dim);
        p = wrap_mat(&ly->wv, p, h.dtype, kv_dim, h.dim);
        if (h.flags & LM_QKNORM) {
            ly->q_norm = (float *)p;  p += (size_t)hd * sizeof(float);
            ly->k_norm = (float *)p;  p += (size_t)hd * sizeof(float);
        }                             /* else both stay NULL from the calloc */
        p = wrap_mat(&ly->wo, p, h.dtype, h.dim, q_dim);
        ly->ffn_norm = (float *)p;
        p += (size_t)h.dim * sizeof(float);
        p = wrap_mat(&ly->w1, p, h.dtype, h.hidden, h.dim);
        p = wrap_mat(&ly->w3, p, h.dtype, h.hidden, h.dim);
        p = wrap_mat(&ly->w2, p, h.dtype, h.dim, h.hidden);
    }

    float *final_norm = (float *)p;
    p += (size_t)h.dim * sizeof(float);

    struct nn_tensor wcls;
    if (h.flags & LM_TIED) {
        /* A VIEW of the embedding, not a copy -- the whole point of tying is
         * that there is exactly one set of these weights on disk, and model.h
         * places nothing here for them to alias into. Copying the DESCRIPTOR
         * (rather than re-wrapping the f32 pointer, which is what this line
         * used to do) is also what makes LM_QEMB work at the output head with
         * no further change: infer.c's mv() dispatches on the tensor's own
         * dtype, so a quantised embedding is a quantised classifier. */
        wcls = emb;
    } else {
        p = wrap_mat(&wcls, p, h.dtype, h.vocab, h.dim);
    }

    m->h = h;
    m->head_dim = (int)hd;
    m->q_dim = (int)q_dim;
    m->kv_dim = (int)kv_dim;
    m->tok_emb = tok_emb;
    m->emb = emb;
    m->layer = layers;
    m->final_norm = final_norm;
    m->wcls = wcls;
    m->blob = (const unsigned char *)blob;
    m->blob_len = len;
    return 0;
}

/* ------------------------------------------------------------- QK-norm --
 *
 * See model.h for WHERE this goes (after the projection, before RoPE) and for
 * why the two orders are indistinguishable at a uniform gain.
 *
 * It calls nn_rmsnorm once per head rather than writing the arithmetic out,
 * and that is the whole reason it is three lines: nn_rmsnorm accumulates the
 * sum of squares in double and is the one op in this line with a negative
 * control behind it (tests/nn.mk NN_RMS_SUBTRACT_MEAN). A hand-written loop
 * here would be a second rounding order for the same operation, with no
 * reference and no control -- exactly what infer.c's header refuses to do for
 * the matmuls.
 *
 * IN PLACE IS SAFE, checked rather than assumed: nn_rmsnorm finishes both
 * accumulation passes over x before it writes a single y[i], and then writes
 * y[i] from x[i] at the same index, so y == x reads nothing it has already
 * overwritten (c/lib/nn/ops.c:47-54). */
void lm_qk_norm(float *v, const float *g, int nh, int head_dim, float eps)
{
    if (!v || !g || nh <= 0 || head_dim <= 0) return;
    for (int i = 0; i < nh; i++) {
        float *h = v + (size_t)i * head_dim;
        nn_rmsnorm(h, h, g, head_dim, eps);
    }
}

/* --------------------------------------------------- embedding lookup -- */
int lm_embed_row(const struct lm_model *m, int t, float *out)
{
    if (!m || !out || t < 0 || (uint32_t)t >= m->h.vocab) return -1;
    int dim = (int)m->h.dim;
    if (m->emb.dtype == NN_F32) {
        if (!m->emb.data) return -1;
        memcpy(out, m->emb.data + (size_t)t * dim, (size_t)dim * sizeof(float));
        return 0;
    }
    if (m->emb.dtype == NN_Q8) {
        if (!m->emb.q || !m->emb.scale) return -1;
        /* nn_dequantize_q8 over ONE row. Called with n = 1 rather than
         * open-coded, for the reason lm_qk_norm gives: the reconstruction
         * rule (value * row scale, and a zero row scale reconstructing to
         * exact zero) is stated once, in the file the tests gate. */
        nn_dequantize_q8(out, m->emb.q + (size_t)t * dim, m->emb.scale + t,
                         1, dim);
        return 0;
    }
    if (m->emb.dtype == NN_Q4) {
        if (!m->emb.q || !m->emb.scale) return -1;
        /* THE ROW STRIDES DIFFER, which is the whole reason this branch is not
         * a copy of the q8 one with a different call. A q8 row is `dim` bytes
         * and carries ONE scale; a q4 row is q4_row_bytes(dim) bytes -- not
         * dim/2 in general, because the tail block rounds up -- and carries
         * q4_blocks(dim) scales and, under Q4_AFFINE, that many minima again.
         * Using dim/2 and t here would drift by one byte per row for any dim
         * whose last block is odd, i.e. read a different row every time, and
         * the model would still run. */
#ifdef LM_Q4_EMB_HALF_STRIDE
        /* THE NEGATIVE CONTROL, and it is the PLAUSIBLE wrong version rather
         * than a broken one: `dim/2`, which is what "4 bits a weight" says and
         * what the q8 branch above (stride == dim) invites by symmetry. It is
         * exactly right for every tensor whose row is a whole number of
         * blocks -- 1024, 2048 and 3072 all are -- and drifts by one byte per
         * row for any dim whose last block is odd. So it does not crash, it
         * reads a different row, and the model still runs. quant4.h names this
         * mistake; this switch is what lets it be watched failing. */
        size_t rb = (size_t)dim / 2;
#else
        size_t rb = q4_row_bytes(dim, Q4_BLOCK);
#endif
        size_t nb = (size_t)q4_blocks(dim, Q4_BLOCK);
        if (!rb || !nb) return -1;
        const float *mn = m->emb.data;   /* q4_wrap parks the minima here */
        if (Q4_MODE == Q4_AFFINE && !mn) return -1;
        q4_dequantize(out, (const uint8_t *)m->emb.q + (size_t)t * rb,
                      m->emb.scale + (size_t)t * nb,
                      mn ? mn + (size_t)t * nb : 0,
                      1, dim, Q4_BLOCK, Q4_MODE);
        return 0;
    }
    /* No fallback to zeroes. A zero embedding row is a legal-looking input
     * that produces a legal-looking model, and this is the one place that
     * would hide a dtype the build cannot read. */
    return -2;
}

void lm_close(struct lm_model *m)
{
    if (!m) return;
    free(m->layer);
    memset(m, 0, sizeof *m);
}

int lm_describe(const struct lm_model *m, char *buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    /* `layer` and not `tok_emb`: since LM_QEMB, tok_emb is NULL on a perfectly
     * good model whose embedding is quantised, and using it as the
     * open/not-open sentinel would report every such model as closed. `layer`
     * is the one pointer lm_open sets on every success and lm_close clears. */
    if (!m || !m->layer) { buf[0] = 0; return -1; }
    const char *dtype = m->h.dtype == NN_Q8 ? "q8"
                      : m->h.dtype == NN_Q4 ? "q4" : "f32";
    int n = snprintf(buf, (size_t)cap,
        /* rope= and eps= are printed ALWAYS, never as a present/absent word.
         * This is the one line a harness log carries about which model ran,
         * and the three architecture facts it now depends on -- pairing, base,
         * epsilon -- are precisely the ones whose wrong value produces fluent,
         * confident, wrong text with nothing else in the output to show it. A
         * flag word that is simply missing when interleaved would read as
         * "not applicable" rather than as a choice. */
        "LOGITLM dim=%u layers=%u heads=%u kv_heads=%u head_dim=%d hidden=%u "
        "vocab=%u seq_len=%u dtype=%s%s%s%s rope=%s/%g eps=%g",
        (unsigned)m->h.dim, (unsigned)m->h.n_layers, (unsigned)m->h.n_heads,
        (unsigned)m->h.n_kv_heads, m->head_dim, (unsigned)m->h.hidden,
        (unsigned)m->h.vocab, (unsigned)m->h.seq_len, dtype,
        (m->h.flags & LM_TIED)   ? " tied"   : "",
        (m->h.flags & LM_QKNORM) ? " qknorm" : "",
        (m->h.flags & LM_QEMB)   ? " qemb"   : "",
        (m->h.flags & LM_ROPE_NEOX) ? "neox" : "interleaved",
        (double)lm_rope_base(&m->h), (double)lm_rms_eps(&m->h));
    if (n < 0) return -1;               /* an encoding error, not a size one */
    return n < cap ? n : cap - 1;       /* snprintf's return can exceed cap */
}
