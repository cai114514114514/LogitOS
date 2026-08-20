/* lm_format_test.c -- the LOGITLM loader against blobs this file builds
 * itself, and against a size formula this file derives itself.
 *
 * NO MODEL FILE EXISTS YET, so the writer lives here: `build_blob` lays out a
 * header and a payload in the exact order model.h specifies, filling every
 * tensor with a position-and-identity-encoding pattern (`patf`/`patq`/`pats`)
 * so a wrong offset reads as a WRONG NUMBER, not a coincidentally plausible
 * one. `indep_expected_size` is a second, independent statement of the same
 * byte-count formula lm_expected_size implements -- written by hand from
 * model.h's payload-order comment, not by calling the function under test,
 * which is what makes the size check below worth anything.
 *
 * THE ONE CHECK THAT MATTERS MOST: the last float of the last tensor in the
 * file. Every offset before it can be right by accumulated coincidence in a
 * loader that is off by one somewhere in the middle -- rows that are too
 * short in one tensor and too long in the next can cancel until the very
 * end, where they cannot cancel any further. That is why it is asserted
 * explicitly rather than trusted because every earlier check passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "model.h"
#include "nn.h"

static int checks, failed;

static void eqi(const char *what, long got, long want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %ld want %ld\n", what, got, want); }
}
static void eqf(const char *what, float got, float want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else {
        failed++;
        printf("FAIL: %s\n      got %.9g want %.9g\n", what, (double)got, (double)want);
    }
}
static void eqp(const char *what, const void *got, const void *want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %p want %p\n", what, got, want); }
}
static void bad(const char *what)
{
    checks++; failed++;
    printf("FAIL: %s\n", what);
}

/* --------------------------------------------------------- test config --
 *
 * Deliberately small and deliberately NOT all-equal: n_heads != n_kv_heads
 * exercises the GQA path (kv_dim < dim), and dim/hidden/vocab/n_layers are
 * four different numbers so a loader that transposed two of them would
 * produce a wrong byte count rather than a right one by coincidence. */
enum {
    T_DIM     = 8,
    T_LAYERS  = 2,
    T_HEADS   = 4,
    T_KVHEADS = 2,
    T_HD      = T_DIM / T_HEADS,        /* 2 */
    T_KVDIM   = T_KVHEADS * T_HD,       /* 4 */
    T_HIDDEN  = 12,
    T_VOCAB   = 10,
    T_SEQLEN  = 16,
};

/* Per-tensor identity tags, so the value written at any position encodes
 * exactly which tensor (and which layer, for the eight that repeat) it came
 * from -- an offset bug reads as "wrong tensor's numbers here", not merely
 * "wrong number". */
enum {
    TAG_TOK_EMB = 0, TAG_ATT_NORM, TAG_WQ, TAG_WK, TAG_WV, TAG_WO,
    TAG_FFN_NORM, TAG_W1, TAG_W3, TAG_W2, TAG_FINAL_NORM, TAG_WCLS
};

static float patf(int tag, int layer, long idx)
{
    /* Exact in f32 for every (tag,layer,idx) this file uses: the largest
     * value is under 1,200,000, well inside float's 24-bit exact-integer
     * range (16,777,216). */
    return (float)(tag * 100000 + layer * 1000 + (int)idx);
}
static int8_t patq(int tag, int layer, long idx)
{
    /* Spans close to the full symmetric int8 range and varies with all three
     * inputs; never depends on modulo tricks large enough to be confused
     * with an unrelated tensor's pattern. */
    long v = (tag * 131 + layer * 977 + idx) % 255 - 127;
    return (int8_t)v;
}
static float pats(int tag, int layer, int row)
{
    return 0.5f + 0.25f * (float)(tag + layer + row);
}

/* ------------------------------------------------------- independent size --
 *
 * Hand-derived from model.h's payload-order comment, not from model.c. */
static size_t indep_mat_bytes(uint32_t dtype, uint32_t rows, uint32_t cols)
{
    size_t n = (size_t)rows * cols;
    if (dtype == NN_Q8) return n + (size_t)rows * sizeof(float);
    return n * sizeof(float);
}
static size_t indep_vec_bytes(uint32_t n) { return (size_t)n * sizeof(float); }

static size_t indep_expected_size(uint32_t dtype, int tied)
{
    size_t total = sizeof(struct lm_header);
    total += indep_mat_bytes(NN_F32, T_VOCAB, T_DIM);              /* tok_emb */

    size_t per_layer = indep_vec_bytes(T_DIM);                     /* att_norm */
    per_layer += indep_mat_bytes(dtype, T_DIM, T_DIM);             /* wq */
    per_layer += indep_mat_bytes(dtype, T_KVDIM, T_DIM);           /* wk */
    per_layer += indep_mat_bytes(dtype, T_KVDIM, T_DIM);           /* wv */
    per_layer += indep_mat_bytes(dtype, T_DIM, T_DIM);             /* wo */
    per_layer += indep_vec_bytes(T_DIM);                           /* ffn_norm */
    per_layer += indep_mat_bytes(dtype, T_HIDDEN, T_DIM);          /* w1 */
    per_layer += indep_mat_bytes(dtype, T_HIDDEN, T_DIM);          /* w3 */
    per_layer += indep_mat_bytes(dtype, T_DIM, T_HIDDEN);          /* w2 */

    total += per_layer * T_LAYERS;
    total += indep_vec_bytes(T_DIM);                               /* final_norm */
    if (!tied) total += indep_mat_bytes(dtype, T_VOCAB, T_DIM);    /* wcls */
    return total;
}

/* -------------------------------------------------------------- writer --
 *
 * Independent of model.c's own offset walk: it writes in the same order
 * because that order is model.h's contract, not because it shares code with
 * the loader. */
static void build_header(struct lm_header *h, uint32_t dtype, uint32_t flags,
                          uint32_t dim, uint32_t n_layers, uint32_t n_heads,
                          uint32_t n_kv_heads, uint32_t hidden, uint32_t vocab,
                          uint32_t seq_len)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, LM_MAGIC, sizeof(LM_MAGIC));   /* 8 bytes incl. the NUL */
    h->version = LM_VERSION;
    h->dtype = dtype;
    h->dim = dim;
    h->n_layers = n_layers;
    h->n_heads = n_heads;
    h->n_kv_heads = n_kv_heads;
    h->hidden = hidden;
    h->vocab = vocab;
    h->seq_len = seq_len;
    h->flags = flags;
    /* reserved[] is already zero from the memset */
}
static void tc_header(struct lm_header *h, uint32_t dtype, uint32_t flags)
{
    build_header(h, dtype, flags, T_DIM, T_LAYERS, T_HEADS, T_KVHEADS,
                 T_HIDDEN, T_VOCAB, T_SEQLEN);
}

static unsigned char *put_f32vec(unsigned char *p, int tag, int layer, long n)
{
    float *f = (float *)p;
    for (long i = 0; i < n; i++) f[i] = patf(tag, layer, i);
    return p + (size_t)n * sizeof(float);
}
static unsigned char *put_mat(unsigned char *p, uint32_t dtype, int tag, int layer,
                               int rows, int cols)
{
    if (dtype == NN_Q8) {
        int8_t *q = (int8_t *)p;
        long n = (long)rows * cols;
        for (long i = 0; i < n; i++) q[i] = patq(tag, layer, i);
        p += (size_t)n;
        float *s = (float *)p;
        for (int r = 0; r < rows; r++) s[r] = pats(tag, layer, r);
        return p + (size_t)rows * sizeof(float);
    }
    return put_f32vec(p, tag, layer, (long)rows * cols);
}

static unsigned char *build_blob(uint32_t dtype, int tied, size_t *out_len)
{
    struct lm_header h;
    tc_header(&h, dtype, tied ? LM_TIED : 0u);
    size_t len = indep_expected_size(dtype, tied);
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) { *out_len = 0; return NULL; }
    memcpy(buf, &h, sizeof h);

    unsigned char *p = buf + sizeof h;
    p = put_f32vec(p, TAG_TOK_EMB, 0, (long)T_VOCAB * T_DIM);
    for (int L = 0; L < T_LAYERS; L++) {
        p = put_f32vec(p, TAG_ATT_NORM, L, T_DIM);
        p = put_mat(p, dtype, TAG_WQ, L, T_DIM, T_DIM);
        p = put_mat(p, dtype, TAG_WK, L, T_KVDIM, T_DIM);
        p = put_mat(p, dtype, TAG_WV, L, T_KVDIM, T_DIM);
        p = put_mat(p, dtype, TAG_WO, L, T_DIM, T_DIM);
        p = put_f32vec(p, TAG_FFN_NORM, L, T_DIM);
        p = put_mat(p, dtype, TAG_W1, L, T_HIDDEN, T_DIM);
        p = put_mat(p, dtype, TAG_W3, L, T_HIDDEN, T_DIM);
        p = put_mat(p, dtype, TAG_W2, L, T_DIM, T_HIDDEN);
    }
    p = put_f32vec(p, TAG_FINAL_NORM, 0, T_DIM);
    if (!tied) p = put_mat(p, dtype, TAG_WCLS, 0, T_VOCAB, T_DIM);

    /* The writer's own walk must land exactly on the end of what it
     * allocated. If it does not, the independent size formula and the write
     * order above have drifted apart from each other -- a bug in THIS file,
     * worth catching here rather than being blamed on model.c. */
    if ((size_t)(p - buf) != len) { free(buf); *out_len = 0; return NULL; }
    *out_len = len;
    return buf;
}

/* ------------------------------------------------------------- checkers -- */
static int is_zeroed(const struct lm_model *m)
{
    static const struct lm_model zero;   /* zero-initialised by static storage */
    return memcmp(m, &zero, sizeof zero) == 0;
}

/* --------------------------------------------------------------- tests -- */

static void t_header_size(void)
{
    printf("-- the on-disk header is exactly 64 bytes\n");
    eqi("sizeof(struct lm_header) == 64", (long)sizeof(struct lm_header), 64);
}

static void t_expected_size_independent(void)
{
    printf("-- lm_expected_size against a byte count this file derived, not lm_open's\n");
    struct lm_header h;

    tc_header(&h, NN_F32, 0);
    eqi("f32, untied", (long)lm_expected_size(&h), (long)indep_expected_size(NN_F32, 0));

    tc_header(&h, NN_F32, LM_TIED);
    eqi("f32, tied", (long)lm_expected_size(&h), (long)indep_expected_size(NN_F32, 1));

    tc_header(&h, NN_Q8, 0);
    eqi("q8, untied", (long)lm_expected_size(&h), (long)indep_expected_size(NN_Q8, 0));

    tc_header(&h, NN_Q8, LM_TIED);
    eqi("q8, tied", (long)lm_expected_size(&h), (long)indep_expected_size(NN_Q8, 1));

    /* Sanity: tied really is smaller (wcls is absent), and Q8 really is
     * smaller than f32 for the same shapes (the whole reason Q8 exists). */
    eqi("tied is smaller than untied (no wcls payload)",
        (long)(lm_expected_size(&h) < indep_expected_size(NN_Q8, 0)) , 1);
    struct lm_header hf, hq;
    tc_header(&hf, NN_F32, 0);
    tc_header(&hq, NN_Q8, 0);
    eqi("q8 is smaller than f32 for identical shapes",
        (long)(lm_expected_size(&hq) < lm_expected_size(&hf)), 1);
}

static void t_open_f32_untied(void)
{
    printf("-- lm_open, f32, untied: every pointer at the value written there\n");
    size_t len;
    unsigned char *buf = build_blob(NN_F32, 0, &len);
    if (!buf) { bad("build_blob(f32, untied)"); return; }

    struct lm_model m;
    memset(&m, 0xAA, sizeof m);   /* poison, so success has to actually fill it */
    eqi("lm_open succeeds", lm_open(&m, buf, len), 0);

    eqi("head_dim = dim / n_heads", m.head_dim, T_HD);
    eqi("kv_dim = n_kv_heads * head_dim", m.kv_dim, T_KVDIM);

    eqf("tok_emb[0]", m.tok_emb[0], patf(TAG_TOK_EMB, 0, 0));
    eqf("tok_emb[last]", m.tok_emb[(long)T_VOCAB * T_DIM - 1],
        patf(TAG_TOK_EMB, 0, (long)T_VOCAB * T_DIM - 1));

    eqf("layer[0].att_norm[0]", m.layer[0].att_norm[0], patf(TAG_ATT_NORM, 0, 0));
    eqf("layer[0].att_norm[last]", m.layer[0].att_norm[T_DIM - 1],
        patf(TAG_ATT_NORM, 0, T_DIM - 1));

    eqi("layer[0].wq shape", (long)(m.layer[0].wq.dim[0] == T_DIM &&
                                    m.layer[0].wq.dim[1] == T_DIM), 1);
    eqf("layer[0].wq.data[0]", m.layer[0].wq.data[0], patf(TAG_WQ, 0, 0));
    eqf("layer[0].wq.data[last]", m.layer[0].wq.data[(long)T_DIM * T_DIM - 1],
        patf(TAG_WQ, 0, (long)T_DIM * T_DIM - 1));

    eqi("layer[0].wk shape is [kv_dim, dim], not [dim, dim]",
        (long)(m.layer[0].wk.dim[0] == T_KVDIM && m.layer[0].wk.dim[1] == T_DIM), 1);
    eqf("layer[0].wk.data[0]", m.layer[0].wk.data[0], patf(TAG_WK, 0, 0));
    eqf("layer[0].wk.data[last]", m.layer[0].wk.data[(long)T_KVDIM * T_DIM - 1],
        patf(TAG_WK, 0, (long)T_KVDIM * T_DIM - 1));

    eqf("layer[0].wv.data[0]", m.layer[0].wv.data[0], patf(TAG_WV, 0, 0));
    eqf("layer[0].wv.data[last]", m.layer[0].wv.data[(long)T_KVDIM * T_DIM - 1],
        patf(TAG_WV, 0, (long)T_KVDIM * T_DIM - 1));

    eqf("layer[0].wo.data[0]", m.layer[0].wo.data[0], patf(TAG_WO, 0, 0));
    eqf("layer[0].wo.data[last]", m.layer[0].wo.data[(long)T_DIM * T_DIM - 1],
        patf(TAG_WO, 0, (long)T_DIM * T_DIM - 1));

    eqf("layer[0].ffn_norm[0]", m.layer[0].ffn_norm[0], patf(TAG_FFN_NORM, 0, 0));
    eqf("layer[0].ffn_norm[last]", m.layer[0].ffn_norm[T_DIM - 1],
        patf(TAG_FFN_NORM, 0, T_DIM - 1));

    eqi("layer[0].w1 shape is [hidden, dim]",
        (long)(m.layer[0].w1.dim[0] == T_HIDDEN && m.layer[0].w1.dim[1] == T_DIM), 1);
    eqf("layer[0].w1.data[0]", m.layer[0].w1.data[0], patf(TAG_W1, 0, 0));
    eqf("layer[0].w1.data[last]", m.layer[0].w1.data[(long)T_HIDDEN * T_DIM - 1],
        patf(TAG_W1, 0, (long)T_HIDDEN * T_DIM - 1));

    eqf("layer[0].w3.data[0]", m.layer[0].w3.data[0], patf(TAG_W3, 0, 0));
    eqf("layer[0].w3.data[last]", m.layer[0].w3.data[(long)T_HIDDEN * T_DIM - 1],
        patf(TAG_W3, 0, (long)T_HIDDEN * T_DIM - 1));

    eqi("layer[0].w2 shape is [dim, hidden], the transpose of w1/w3",
        (long)(m.layer[0].w2.dim[0] == T_DIM && m.layer[0].w2.dim[1] == T_HIDDEN), 1);
    eqf("layer[0].w2.data[0]", m.layer[0].w2.data[0], patf(TAG_W2, 0, 0));
    eqf("layer[0].w2.data[last]", m.layer[0].w2.data[(long)T_DIM * T_HIDDEN - 1],
        patf(TAG_W2, 0, (long)T_DIM * T_HIDDEN - 1));

    /* Layer 1 (the LAST layer), spot-checked too: a per-layer stride bug
     * (using the wrong per_layer size, or reusing layer 0's) would still
     * pass every layer-0 check above and only show up here. */
    eqf("layer[1].att_norm[0]", m.layer[1].att_norm[0], patf(TAG_ATT_NORM, 1, 0));
    eqf("layer[1].w2.data[last]", m.layer[1].w2.data[(long)T_DIM * T_HIDDEN - 1],
        patf(TAG_W2, 1, (long)T_DIM * T_HIDDEN - 1));

    eqf("final_norm[0]", m.final_norm[0], patf(TAG_FINAL_NORM, 0, 0));
    eqf("final_norm[last]", m.final_norm[T_DIM - 1], patf(TAG_FINAL_NORM, 0, T_DIM - 1));

    /* THE LAST FLOAT OF THE LAST TENSOR IN THE FILE. */
    eqi("wcls shape", (long)(m.wcls.dim[0] == T_VOCAB && m.wcls.dim[1] == T_DIM), 1);
    eqf("wcls.data[0]", m.wcls.data[0], patf(TAG_WCLS, 0, 0));
    eqf("wcls.data[LAST] (last float of the last tensor)",
        m.wcls.data[(long)T_VOCAB * T_DIM - 1],
        patf(TAG_WCLS, 0, (long)T_VOCAB * T_DIM - 1));

    /* Without LM_TIED, wcls is its OWN storage, not a view of tok_emb. */
    eqi("wcls.data != tok_emb (untied means separate storage)",
        (long)(m.wcls.data != m.tok_emb), 1);

    eqi("blob_len records what was opened", (long)m.blob_len, (long)len);
    eqp("blob records the pointer that was opened", (const void *)m.blob, buf);

    lm_close(&m);
    eqi("lm_close zeroes the struct", is_zeroed(&m), 1);

    free(buf);
}

static void t_open_f32_tied(void)
{
    printf("-- lm_open, f32, tied: wcls aliases tok_emb, and costs no bytes\n");
    size_t len;
    unsigned char *buf = build_blob(NN_F32, 1, &len);
    if (!buf) { bad("build_blob(f32, tied)"); return; }

    struct lm_model m;
    memset(&m, 0xAA, sizeof m);
    eqi("lm_open succeeds", lm_open(&m, buf, len), 0);

    eqi("wcls shape is still [vocab, dim]",
        (long)(m.wcls.dim[0] == T_VOCAB && m.wcls.dim[1] == T_DIM), 1);
    eqp("wcls.data IS tok_emb (same pointer, not a copy)",
        (const void *)m.wcls.data, (const void *)m.tok_emb);
    eqf("aliased wcls reads the embedding's actual values",
        m.wcls.data[(long)T_VOCAB * T_DIM - 1],
        patf(TAG_TOK_EMB, 0, (long)T_VOCAB * T_DIM - 1));

    /* final_norm is now the LAST tensor physically on disk (wcls has no
     * payload when tied), so its last value is this build's version of the
     * off-by-one check t_open_f32_untied does with wcls. */
    eqf("final_norm[last] (last tensor on disk when tied)",
        m.final_norm[T_DIM - 1], patf(TAG_FINAL_NORM, 0, T_DIM - 1));

    lm_close(&m);
    free(buf);
}

static void t_open_q8_untied(void)
{
    printf("-- lm_open, q8: [rows*cols int8][rows f32 scale], norms stay f32\n");
    size_t len;
    unsigned char *buf = build_blob(NN_Q8, 0, &len);
    if (!buf) { bad("build_blob(q8, untied)"); return; }

    struct lm_model m;
    memset(&m, 0xAA, sizeof m);
    eqi("lm_open succeeds", lm_open(&m, buf, len), 0);

    eqi("layer[0].wq is NN_Q8", m.layer[0].wq.dtype, NN_Q8);
    eqi("layer[0].wq.q[0]", (long)m.layer[0].wq.q[0], (long)patq(TAG_WQ, 0, 0));
    eqi("layer[0].wq.q[last]",
        (long)m.layer[0].wq.q[(long)T_DIM * T_DIM - 1],
        (long)patq(TAG_WQ, 0, (long)T_DIM * T_DIM - 1));
    eqf("layer[0].wq.scale[0]", m.layer[0].wq.scale[0], pats(TAG_WQ, 0, 0));
    eqf("layer[0].wq.scale[last row]", m.layer[0].wq.scale[T_DIM - 1],
        pats(TAG_WQ, 0, T_DIM - 1));

    /* Norms and the embedding stay f32 EVEN IN A Q8 FILE (model.h). A loader
     * that quantised them anyway, or that read them as if quantised, would
     * fail exactly these two checks and pass everything about the matrices. */
    eqf("tok_emb[0] is still plain f32 in a q8 file",
        m.tok_emb[0], patf(TAG_TOK_EMB, 0, 0));
    eqf("layer[0].att_norm[0] is still plain f32 in a q8 file",
        m.layer[0].att_norm[0], patf(TAG_ATT_NORM, 0, 0));

    /* THE LAST TENSOR IN THE FILE, Q8 flavour: last int8 AND last scale. */
    eqi("wcls is NN_Q8", m.wcls.dtype, NN_Q8);
    eqi("wcls.q[LAST]",
        (long)m.wcls.q[(long)T_VOCAB * T_DIM - 1],
        (long)patq(TAG_WCLS, 0, (long)T_VOCAB * T_DIM - 1));
    eqf("wcls.scale[LAST row]", m.wcls.scale[T_VOCAB - 1], pats(TAG_WCLS, 0, T_VOCAB - 1));

    /* Layer 1's w2 (last per-layer tensor, last layer) -- the same stride
     * check t_open_f32_untied does, repeated because Q8's stride includes
     * the scale block and a bug there would not show up in the f32 build. */
    eqi("layer[1].w2.q[last]",
        (long)m.layer[1].w2.q[(long)T_DIM * T_HIDDEN - 1],
        (long)patq(TAG_W2, 1, (long)T_DIM * T_HIDDEN - 1));
    eqf("layer[1].w2.scale[last row]", m.layer[1].w2.scale[T_DIM - 1],
        pats(TAG_W2, 1, T_DIM - 1));

    lm_close(&m);
    free(buf);
}

static void t_refusals(void)
{
    printf("-- refusal codes -1..-5, each on a blob crafted to trigger exactly it\n");
    unsigned char hbuf[sizeof(struct lm_header)];
    struct lm_model m;

    /* -1 bad magic. */
    { struct lm_header h; tc_header(&h, NN_F32, 0); memcpy(hbuf, &h, sizeof h); }
    hbuf[0] = 'X';
    memset(&m, 0xAA, sizeof m);
    eqi("-1: bad magic is refused", lm_open(&m, hbuf, sizeof hbuf), -1);
    eqi("-1: *m left zeroed", is_zeroed(&m), 1);

    /* -2 wrong version. */
    { struct lm_header h; tc_header(&h, NN_F32, 0); h.version = LM_VERSION + 1;
      memcpy(hbuf, &h, sizeof h); }
    memset(&m, 0xAA, sizeof m);
    eqi("-2: wrong version is refused", lm_open(&m, hbuf, sizeof hbuf), -2);
    eqi("-2: *m left zeroed", is_zeroed(&m), 1);

    /* -3 a nonzero reserved field -- a future format, refused rather than
     * ignored, per model.h. */
    { struct lm_header h; tc_header(&h, NN_F32, 0); h.reserved[2] = 1;
      memcpy(hbuf, &h, sizeof h); }
    memset(&m, 0xAA, sizeof m);
    eqi("-3: a nonzero reserved field is refused", lm_open(&m, hbuf, sizeof hbuf), -3);
    eqi("-3: *m left zeroed", is_zeroed(&m), 1);

    /* -4 inconsistent header -- three different ways to be inconsistent,
     * because model.h names three and a loader that checked only one of
     * them would be trusting the other two. */
    { struct lm_header h; tc_header(&h, NN_F32, 0); h.dim = 0;
      memcpy(hbuf, &h, sizeof h); }
    memset(&m, 0xAA, sizeof m);
    eqi("-4: a zero dim is refused", lm_open(&m, hbuf, sizeof hbuf), -4);
    eqi("-4: *m left zeroed (zero dim)", is_zeroed(&m), 1);

    { struct lm_header h; tc_header(&h, NN_F32, 0); h.n_heads = T_DIM - 1; /* 7 !| 8 */
      memcpy(hbuf, &h, sizeof h); }
    memset(&m, 0xAA, sizeof m);
    eqi("-4: dim % n_heads != 0 is refused", lm_open(&m, hbuf, sizeof hbuf), -4);
    eqi("-4: *m left zeroed (dim % n_heads)", is_zeroed(&m), 1);

    { struct lm_header h; tc_header(&h, NN_F32, 0); h.n_heads = 4; h.n_kv_heads = 3;
      memcpy(hbuf, &h, sizeof h); }                 /* 3 does not divide 4 */
    memset(&m, 0xAA, sizeof m);
    eqi("-4: n_kv_heads not dividing n_heads is refused", lm_open(&m, hbuf, sizeof hbuf), -4);
    eqi("-4: *m left zeroed (n_kv_heads)", is_zeroed(&m), 1);

    /* -5 wrong size, off by exactly one byte -- the boundary a loader that
     * merely walks forward and trusts each tensor would not notice until it
     * read one byte past the buffer, in the middle of whichever tensor
     * happened to end there. */
    size_t full_len;
    unsigned char *full = build_blob(NN_F32, 0, &full_len);
    if (!full) {
        bad("build_blob(f32, untied) for the -5 case");
    } else {
        memset(&m, 0xAA, sizeof m);
        eqi("-5: one byte short is refused", lm_open(&m, full, full_len - 1), -5);
        eqi("-5: *m left zeroed", is_zeroed(&m), 1);

        memset(&m, 0xAA, sizeof m);
        eqi("-5: one byte too many is also refused", lm_open(&m, full, full_len + 1), -5);
        eqi("-5: *m left zeroed (too long)", is_zeroed(&m), 1);
        free(full);
    }
}

int main(void)
{
    t_header_size();
    t_expected_size_independent();
    t_open_f32_untied();
    t_open_f32_tied();
    t_open_q8_untied();
    t_refusals();

    printf("\nlm_format_test: %d checks, %d failures\n", checks, failed);
    if (failed) return 1;
    printf("lm_format_test: ALL PASS\n");
    return 0;
}
