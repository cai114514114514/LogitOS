/* model.h -- LOGITLM: one file, one model.
 *
 * WHY A FORMAT OF OUR OWN, stated once so nobody re-litigates it: this
 * filesystem has 256 INODES and 223 of them are already used. Any format that
 * ships a directory of shards, a tokenizer JSON and a config JSON cannot be
 * installed on this machine at all. GGUF would fit that rule and is the
 * obvious candidate, but it carries a key-value metadata section, twenty
 * quantisation blocktypes and an alignment contract, all of which exist to
 * serve portability between runtimes this machine does not have. What is
 * needed here is the smallest thing that can be MAPPED and read in place.
 *
 * MAPPED, AND THAT IS THE WHOLE DESIGN. Every tensor is contiguous, in a fixed
 * order, at a computable offset, with no compression and no interleaving. So
 * a loader does not read the file -- it points at it. That matters twice:
 *
 *   - a 512 MiB machine cannot afford a copy of the weights on top of the
 *     weights;
 *   - and the mapping is not hypothetical. SYS_MMAP_FILE, c/kernel/mm/pcache.c
 *     and MM_FAULT_FILE already exist and are wired into reclaim tier 1.
 *     (An earlier draft of this comment said the opposite, quoting a CLAUDE.md
 *     paragraph written before that work landed and never updated. Both are
 *     corrected now. The lesson is the one CLAUDE.md states about itself: a
 *     stale claim in the first file everybody reads is not inert.)
 *
 *     What that machinery has never had is a REAL WORKLOAD. The only caller of
 *     SYS_MMAP_FILE in the whole tree is fsroot/as/examples/pcachecheck.as -- a
 *     script written to exercise it. A model larger than physical memory, paged
 *     in through it under pressure, would be the first, and is the only part of
 *     this line that is operating-system work rather than an application that
 *     happens to live in an OS repository.
 *
 * THE HEADER IS 64 BYTES AND EVERY FIELD IS A u32. No packing questions, no
 * endianness question worth asking on a machine that is only ever x86-64, and
 * a header a python writer can emit with struct.pack in one line.
 */
#ifndef LOGIT_LM_MODEL_H
#define LOGIT_LM_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "nn.h"

#define LM_MAGIC   "LOGITLM"        /* 7 chars + the NUL = 8 bytes */
#define LM_VERSION 1u

/* flags. A reader REFUSES a flag it does not know (lm_expected_size checks
 * `flags & ~LM_FLAGS_KNOWN`) rather than ignoring it, for the same reason
 * `reserved` is refused: an unknown flag changes the PAYLOAD LENGTH, so
 * ignoring it does not produce a slightly-wrong model, it produces a reader
 * walking a layout that is not there. */
#define LM_TIED    1u               /* the output head IS the token embedding */
#define LM_QKNORM  2u               /* per-layer q_norm/k_norm -- see below */
#define LM_QEMB    4u               /* tok_emb is stored at `dtype`, not f32 */
/* LM_ROPE_NEOX -- rotate (x[i], x[i+hd/2]) instead of (x[2i], x[2i+1]).
 *
 * A MODEL PROPERTY, not a build-time one, for exactly the reason rope_base is
 * (see below): both pairings are valid rotary embeddings, both preserve every
 * pair's length, and a model run under the wrong one produces finite logits
 * and fluent, confident, wrong text. llama2.c and the RoFormer paper use the
 * interleaved pairing; GPT-NeoX, every huggingface `rotate_half`, and Qwen3
 * use this one.
 *
 * UNLIKE rope_base IT IS A FLAG AND NOT A VALUE, and that is not a style
 * choice: LM_FLAGS_KNOWN already makes a reader REFUSE a flag it does not
 * understand, which is the behaviour wanted here. A file that needs a pairing
 * this build does not implement must not be run under the other one.
 *
 * CLEAR = interleaved, so every model written before this flag existed is
 * bit-identical through lm_forward. */
#define LM_ROPE_NEOX 8u
#define LM_FLAGS_KNOWN (LM_TIED | LM_QKNORM | LM_QEMB | LM_ROPE_NEOX)

/* On-disk header. Exactly 64 bytes; asserted at compile time in model.c and
 * asserted against a real file by tests/unit/lm_format_test.c, because "the
 * struct matches the writer" is a claim and a claim you cannot read back is a
 * comment. */
struct lm_header {
    char     magic[8];              /* "LOGITLM\0" */
    uint32_t version;               /* LM_VERSION */
    uint32_t dtype;                 /* NN_F32, NN_Q8 or NN_Q4 -- of the MATMUL
                                     * weights (and of the embedding, but only
                                     * when LM_QEMB says so) */
    uint32_t dim;
    uint32_t n_layers;
    uint32_t n_heads;
    uint32_t n_kv_heads;            /* == n_heads for plain MHA */
    uint32_t hidden;                /* the FFN intermediate size */
    uint32_t vocab;
    uint32_t seq_len;               /* the KV cache is sized from this */
    uint32_t flags;                 /* LM_* */
    uint32_t head_dim;              /* 0 = dim / n_heads. See below. */
    uint32_t rope_base_f32;         /* f32 BIT PATTERN; 0 = LM_ROPE_BASE_DFL */
    uint32_t rms_eps_f32;           /* f32 BIT PATTERN; 0 = LM_RMS_EPS_DFL   */
    uint32_t reserved[1];           /* zero; a reader must REFUSE a non-zero
                                     * one rather than ignore it, so a future
                                     * field cannot be silently misread */
};

/* THE TWO ARCHITECTURE CONSTANTS, AND WHY THEY MOVED INTO THE FILE.
 *
 * infer.c used to compile these in, arguing they are properties of the
 * ARCHITECTURE rather than of a file: "a file that wanted a different rope
 * base would be a different architecture wearing the same magic number".
 * That argument was right about the danger and wrong about the fact. Qwen3
 * is the same architecture this format already describes -- same RMSNorm,
 * same SwiGLU, same GQA, same rotary -- and it ships
 *
 *     rope.freq_base                    1000000.0     vs 10000.0   100x
 *     attention.layer_norm_rms_epsilon  9.9999999e-07 vs 1e-05      10x
 *
 * so under the old rule the model was not expressible, and the converter
 * refused to write it. That refusal was correct: A WRONG ROPE BASE IS THE
 * WORST FAILURE IN THIS WHOLE FILE. It does not crash, it does not produce a
 * NaN, it does not change the tokens/s -- every logit is finite and the text
 * is fluent and wrong, because a 100x base is simply a different position
 * encoding. There is no output a person can look at that says so.
 *
 * ZERO MEANS "THE OLD DEFAULT", so this is a compatible extension and not a
 * version bump -- the same shape LogitFS used when atime/mtime took over its
 * `reserved` bytes. Every file written before this field existed has zeroes
 * there and keeps reading exactly as it did.
 *
 * AND ZERO IS SAFE AS A SENTINEL HERE, WHICH IS NOT AUTOMATIC -- LogitFS had
 * to add LFS_MODE_SET precisely because mode 0 is a legal mode. Neither of
 * these has a legal zero: a rope base of 0 makes every frequency 1/0, and an
 * rmsnorm eps of 0 is a divide-by-zero on a zero row. So there is no value a
 * writer could mean by 0 other than "I did not say", and no presence bit is
 * needed. A NEGATIVE or non-finite value, on the other hand, IS
 * representable and is refused by lm_open (-4) rather than propagated into
 * NaN logits three layers down.
 *
 * They are stored as f32 BIT PATTERNS in uint32 fields so the header's own
 * "exactly 64 bytes, every field is a uint32" contract is unchanged -- a
 * float member would have said the same thing about the bytes and made the
 * struct's stated invariant false. */
#define LM_ROPE_BASE_DFL 10000.0f
#define LM_RMS_EPS_DFL   1e-5f

/* The accessors both readers use. They are here, in the header both the
 * runtime and the converter include, because the previous arrangement -- a
 * #define private to infer.c and a hand-kept COPY in tools/lmshape.c gated by
 * a grep-the-source test -- is exactly the "second constant to keep in sync"
 * this format's own comments argue against everywhere else. */
static inline float lm_f32_or(uint32_t bits, float dfl)
{
    float f;
    if (bits == 0) return dfl;
    /* memcpy, not a union or a cast through float*: the cast is a strict
     * aliasing violation that -O2 is entitled to miscompile, and this is the
     * one place in the format where a silently wrong value cannot be seen in
     * the output. */
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}
static inline float lm_rope_base(const struct lm_header *h)
{ return lm_f32_or(h->rope_base_f32, LM_ROPE_BASE_DFL); }
static inline float lm_rms_eps(const struct lm_header *h)
{ return lm_f32_or(h->rms_eps_f32, LM_RMS_EPS_DFL); }

/* HEAD_DIM IS NOT dim / n_heads, AND THAT IS NOT A HYPOTHETICAL. This field
 * used to be reserved[0] and the head size was derived, which is true of
 * llama-2 and of every model this tree has built so far -- and false of the
 * one it is aimed at. Qwen3-0.6B is hidden 1024, 16 query heads, head_dim
 * 128: q_proj emits 16*128 = 2048 values from a 1024-wide residual stream and
 * o_proj maps them back. The derived rule gives 64 and there is no way to say
 * otherwise, so the shape was not expressible at all.
 *
 * The arithmetic says the same thing, which is how the gap was found rather
 * than remembered. Per-token multiply-adds for that config, counted as
 * matvec parameters:
 *
 *   head_dim 128 (explicit):  28 layers * (2*1024*2048 + 2*1024*1024        <- attn
 *                                          + 3*1024*3072)                  <- ffn
 *                             + 151936*1024                                <- head
 *                           = 595,984,384  ->  1.19 GFLOP/token
 *   head_dim  64 (derived) :  507,904,000  ->  1.02 GFLOP/token
 *
 * 1.19 GFLOP is the figure this line's memory budget was written against, so
 * the budget already assumed a field the format did not have.
 *
 * CONSEQUENCES, spelled out because two of them are shapes elsewhere in the
 * payload: with q_dim = n_heads * head_dim,
 *     wq is [q_dim, dim]   (NOT [dim, dim])
 *     wo is [dim, q_dim]   (NOT [dim, dim])
 *     kv_dim = n_kv_heads * head_dim, unchanged in form
 * and when head_dim is 0 every one of those collapses to exactly the bytes a
 * file written before this field existed already has. `dim % n_heads == 0` is
 * required ONLY in the derived case -- Qwen3-0.6B does not satisfy it and
 * does not need to. */

/* PAYLOAD ORDER, and it is fixed rather than named-and-indexed on purpose: a
 * name table is a second thing to keep in sync between the writer and the
 * reader, and with one writer and one reader the sync is the table. All
 * row-major.
 *
 *   tok_emb      [vocab, dim]          f32, or `dtype` when LM_QEMB
 *   per layer L in 0..n_layers-1:
 *     att_norm   [dim]                 ALWAYS f32
 *     wq         [q_dim, dim]          dtype
 *     wk         [kv_dim, dim]         dtype
 *     wv         [kv_dim, dim]         dtype
 *     q_norm     [hd]                  f32 -- ONLY when LM_QKNORM
 *     k_norm     [hd]                  f32 -- ONLY when LM_QKNORM
 *     wo         [dim, q_dim]          dtype
 *     ffn_norm   [dim]                 ALWAYS f32
 *     w1 (gate)  [hidden, dim]         dtype
 *     w3 (up)    [hidden, dim]         dtype
 *     w2 (down)  [dim, hidden]         dtype
 *   final_norm   [dim]                 ALWAYS f32
 *   wcls         [vocab, dim]          dtype -- ABSENT when LM_TIED is set
 *
 * where hd = head_dim ? head_dim : dim / n_heads,
 *       q_dim  = n_heads    * hd,
 *       kv_dim = n_kv_heads * hd.
 *
 * QK-NORM SITS BETWEEN wv AND wo, and the position is chosen rather than
 * appended: the two vectors are consumed between the q/k projections and
 * RoPE, so they are placed with the tensors that produce their input. The
 * alternative -- appending them after w2, where a new field naturally goes --
 * would put them a whole FFN away from anything that reads them, and the
 * payload order is the only documentation a reader of a hex dump has.
 *
 * ONE VECTOR PER LAYER, NOT ONE PER HEAD. Qwen3's q_norm is
 * `RMSNorm(head_dim)` and is applied to every head with the SAME gain; a
 * per-head gain would be n_heads times the bytes to express a model that does
 * not exist. Storing it per-head "in case" would also make a file written by
 * a converter that filled all heads identically indistinguishable from one
 * that did not, which is a difference no reader could then check.
 *
 * THE NORMS STAY f32 EVEN IN A Q8 FILE, and that is not timidity. A norm's
 * gain is `dim` numbers and quantising it saves nothing measurable while
 * putting a per-row scale on a vector that has one row.
 *
 * THE EMBEDDING USED TO STAY f32 TOO, on the argument that it is a LOOKUP and
 * nothing multiplies it. That argument was written against a 256-token
 * byte-level vocabulary, where the table is 256*dim and the point is
 * unarguable. It does not survive a real tokenizer: Qwen3's vocabulary is
 * 151,936, so at dim 1024 the table is 155,582,464 floats = 594 MiB in f32 --
 * MORE THAN TWICE the 284 MiB the whole q4 model is budgeted at, and on a
 * machine with 512 MiB of RAM. And with LM_TIED the same table IS the output
 * head, so "nothing multiplies it" is false by construction: the last matvec
 * of every token multiplies all of it.
 *
 * So LM_QEMB stores it at `dtype`, and it is a FLAG rather than the new rule
 * because for a byte-level model the old argument is still exactly right.
 *
 * A Q8 tensor on disk is [rows*cols int8][rows f32 scales], in that order.
 *
 * A Q4 TENSOR IS [packed nibbles][pad to 4][scales][minima], and this
 * paragraph used to say q4 was deliberately absent -- "REFUSED rather than
 * mis-sized until the quantiser that defines the block layout exists". The
 * quantiser exists (c/lib/nn/quant4.c) and NN_Q4 is a legal `dtype` now. The
 * rule that sentence was protecting is kept exactly: this file still does not
 * state the layout. model.c calls `q4_mat_bytes` and `q4_wrap_mat`, so the
 * block size, the odd-tail packing and the alignment pad have ONE definition,
 * in the quantiser, and the format cannot drift from it.
 *
 * The block size and the quantisation mode are properties of LM_VERSION and
 * NOT header fields (Q4_BLOCK / Q4_MODE in quant4.h). That is a deliberate
 * narrowing: a field would let a file ask for a block size the reader's
 * kernels do not implement, and the reader would have to refuse it anyway --
 * so the version number carries it, and a future block size is a version
 * bump, which every reader already refuses correctly.
 *
 * WHAT Q4 COSTS AND BUYS, since `dtype` is now a three-way choice a writer
 * has to make. Measured, at the shape this line is aimed at (28 layers, dim
 * 1024, head_dim 128, hidden 3072, vocab 151936, tied, qemb):
 *
 *     f32   2273.8 MiB     q8   570.5 MiB     q4   355.5 MiB
 *
 * (`build/lmshape --preset qwen3-0.6b --dtype DT --qemb --dry-run`, which
 * prints lm_expected_size and writes nothing.)
 *
 * AND 355.5 IS NOT THE 284 MiB A BACK-OF-ENVELOPE GIVES, which is worth
 * stating because 284 is the number this line's memory budget was written
 * against. 284 MiB is 595,984,384 weights at exactly 4.00 bits. No usable q4
 * costs 4.00 bits: quant4.h's shipped pair is a 64-weight block with an f32
 * scale and an f32 minimum, i.e. 4 + 64/64 = 5.03 bits, and 5.03/4.00 x 284 =
 * 357 MiB. The 71 MiB difference IS the block metadata, and quant4.h's ladder
 * is the measurement of what removing it would cost: per-row scales (the
 * 4.00-bit limit) score +0.0138 nats against block 64's +0.0046, three times
 * the damage.
 *
 * The accuracy cost of q4 against the same model's own f32 weights, measured
 * on a TRAINED model (quant4.h has the ladder): +0.0046 nats/byte.
 *
 * On this machine q4 is not an optimisation -- 570 MiB does not fit in 512
 * MiB of RAM at all, so it is the difference between a shape that can be
 * carried and one that cannot.
 */

/* ------------------------------------------------------------- the model --
 *
 * `lm_model` BORROWS the blob. It never copies and never frees it: the caller
 * owns the bytes, whether they came from a read or from a mapping, and the
 * whole point of the format is that those two cases are the same case. */
struct lm_layer {
    const float  *att_norm;
    struct nn_tensor wq, wk, wv, wo;
    /* NULL unless LM_QKNORM. NULL and not a pointer to a vector of ones: a
     * unit gain would make the flag unobservable from the descriptors, and
     * "did this file carry QK-norm?" is a question a caller has to be able to
     * answer from the model rather than by re-reading the header. */
    const float  *q_norm, *k_norm;
    const float  *ffn_norm;
    struct nn_tensor w1, w3, w2;
};

struct lm_model {
    struct lm_header h;
    int head_dim;                   /* h.head_dim, or dim / n_heads */
    int q_dim;                      /* n_heads    * head_dim */
    int kv_dim;                     /* n_kv_heads * head_dim */
    const float *tok_emb;           /* [vocab, dim] f32 -- NULL when LM_QEMB */
    struct nn_tensor emb;           /* the same table as a tensor, ALWAYS set:
                                     * f32 or quantised. tok_emb above is the
                                     * f32 fast path and stays NULL otherwise,
                                     * so a caller that memcpy's a row cannot
                                     * silently read int8 as float. */
    struct lm_layer *layer;         /* n_layers, malloc'd (the descriptors,
                                     * not the weights) */
    const float *final_norm;
    struct nn_tensor wcls;          /* == emb when LM_TIED */
    const unsigned char *blob;      /* what was handed in, for bounds checks */
    size_t blob_len;
};

/* Compute the exact byte size a file with this header must have. Exposed
 * because a loader that merely walks forward and trusts each tensor will run
 * off the end of a truncated file; comparing this against the actual length
 * FIRST turns "truncated model" into one error message instead of a fault
 * somewhere in layer 3. Returns 0 if the header is not self-consistent. */
size_t lm_expected_size(const struct lm_header *h);

/* Point a model at a blob. Returns 0 on success, negative on refusal:
 *   -1 bad magic        -2 wrong version      -3 reserved field set
 *   -4 inconsistent header (a zero dim, dim % n_heads, ...)
 *   -5 the blob is not lm_expected_size() bytes
 *   -6 out of memory
 * On any refusal *m is left zeroed -- never half-populated, because the
 * caller's error path would otherwise have to know which half. */
int  lm_open(struct lm_model *m, const void *blob, size_t len);
void lm_close(struct lm_model *m);

/* A one-line human description, for a log that has to say WHICH model ran. */
int  lm_describe(const struct lm_model *m, char *buf, int cap);

/* ------------------------------------------------------ QK-norm (Qwen3) --
 *
 * RMS-norm each head's slice of a projected q or k vector, IN PLACE, with one
 * gain vector shared by every head.
 *
 *   v      [nh * head_dim]   the whole projection, heads laid out contiguously
 *   g      [head_dim]        the layer's q_norm or k_norm
 *
 * IT GOES AFTER THE PROJECTION AND BEFORE ROPE, which is the whole reason
 * this function exists rather than a note saying "call nn_rmsnorm". Qwen3's
 * attention is q_norm(q_proj(x)) then rotary, and the two orders are NOT
 * interchangeable -- but the way they differ is a trap:
 *
 *   RoPE IS A ROTATION, so it preserves the sum of squares of a head's slice.
 *   The 1/sqrt(mean(x^2)) FACTOR is therefore identical whichever order you
 *   use. Only the elementwise gain distinguishes them, because the rotation
 *   mixes coordinates and g[i] does not commute with that mixing.
 *
 * The consequence for anyone testing this: with a UNIFORM gain vector -- and
 * an untrained model's q_norm is initialised to all-ones -- the two orders
 * are the SAME FUNCTION, so the most natural test that could be written
 * cannot see the bug at all. Measured in tests/unit/lmshape_test.c, head_dim
 * 8, position 3:
 *
 *     uniform gain      worst |correct - swapped| = 1.19e-07   (6.2e-08 of
 *                       the value scale -- one ulp of the norm factor, since
 *                       RoPE preserves the sum of squares only to f32
 *                       rounding: 2.27970906 before, 2.27970897 after)
 *     non-uniform gain  worst |correct - swapped| = 0.1176     (4.4% of it)
 *
 * Six orders of magnitude apart. The uniform figure is not zero -- an earlier
 * draft of that test asserted memcmp == 0 and failed -- but it is f32 noise,
 * which is exactly what makes it invisible to any tolerance a forward-pass
 * test could sensibly use.
 *
 * eps is the caller's because infer.c owns the architecture's value (its
 * LM_RMS_EPS), and a second copy here would be a second constant to keep in
 * step with it. */
void lm_qk_norm(float *v, const float *g, int nh, int head_dim, float eps);

/* --------------------------------------------------- embedding lookup --
 *
 * Materialise token `t`'s embedding row into `out` [dim]. This is a memcpy in
 * the f32 case and a dequantise in the LM_QEMB case, and it exists so that
 * the caller does not have to branch on the flag -- a caller that memcpy'd
 * unconditionally would read int8 bytes as floats and produce a model that
 * runs and is nonsense.
 *
 * Returns 0, or negative: -1 bad argument, -2 a dtype this build cannot
 * dequantise (q4 before the quantiser lands). It does NOT fall back to zeroes
 * on -2: a zero embedding row is a valid-looking input that produces a
 * plausible-looking model, which is the failure mode that costs weeks. */
int  lm_embed_row(const struct lm_model *m, int t, float *out);

#endif /* LOGIT_LM_MODEL_H */
