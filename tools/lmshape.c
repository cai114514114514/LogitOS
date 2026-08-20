/* lmshape.c -- write a LOGITLM file of ANY shape, with no training and no
 * corpus. HOST ONLY; it is not linked into anything that boots.
 *
 *     cc -O2 -w -Ic/lib/nn -o build/lmshape tools/lmshape.c \
 *        c/lib/nn/model.c c/lib/nn/infer.c c/lib/nn/quant4.c \
 *        c/lib/nn/tensor.c c/lib/nn/matmul.c c/lib/nn/ops.c -lm
 *
 *     build/lmshape --preset qwen3-0.6b --dtype q4 --qemb --out build/q6.lm
 *     build/lmshape --verify build/q6.lm
 *     build/lmshape --forward build/q6.lm --tokens 32
 *
 * (That link line was wrong for as long as this file has existed -- it named
 * neither infer.c, whose lm_forward/lm_state_* the --forward mode calls, nor
 * quant4.c, which model.c now needs. Both were undefined references, i.e. the
 * documented command did not build. Corrected rather than left, because a
 * command line in a header is the one thing a reader will paste unread.)
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS. Everything this line has measured -- 128 tok/s on device,
 * +0.0000 nats for q8, a green forward pass against a double reference -- was
 * measured on ONE model: 824k parameters, 4 layers, dim 128, a 256-token
 * byte-level vocabulary. The target is 28 layers, dim 1024, head_dim 128,
 * vocab 151936: 724x the arithmetic and 700x the parameters. Nothing between
 * here and there is evidence, and the only way to get evidence before a
 * tokenizer, a converter and a set of real weights exist is to build a file
 * that has the SHAPE without having the MEANING.
 *
 * It answers exactly one question -- "does this stack survive that shape?" --
 * and it is worth saying what it cannot answer: a model of pseudo-random
 * weights has no perplexity worth printing, so any accuracy claim from a file
 * this tool wrote is a claim about arithmetic, never about language.
 *
 * ---------------------------------------------------------------------------
 * WHY C AND NOT PYTHON. Three reasons, and the third is the one that decided
 * it.
 *
 *   1. tools/lmtrain.c already made this argument for the trainer and it holds
 *      here unchanged: numpy is not vendored, the host is not guaranteed to
 *      have it, and a build step that starts with `pip install` fails on
 *      somebody else's machine six months from now.
 *
 *   2. Speed is not incidental at this size. The preset writes 596 million
 *      weights; a per-element Python loop is minutes, and `np.random` would be
 *      a SECOND definition of the pseudo-random stream that the C spot-checker
 *      below could not reproduce.
 *
 *   3. THE QUANTISER MUST BE THE REAL ONE. A q8 file this tool writes has to
 *      be byte-identical to what nn_quantize_q8 would produce, because the
 *      whole point is to test the path the device runs. A Python
 *      reimplementation of "scale = max|row| / 127, round to nearest" is a
 *      second rounding rule that agrees almost always -- and the cases where
 *      it does not are exactly the ones worth finding. Linking the kernel is
 *      the only way to have no second rule at all. quant4.c has landed and
 *      this file calls q4_quantize rather than copying it -- same argument,
 *      and with more force: q4 has a block size, a tail-block rule and an
 *      alignment pad, so a second writer has four places to disagree instead
 *      of one.
 *
 * ---------------------------------------------------------------------------
 * THE PRNG IS COUNTER-BASED, AND THAT IS A DESIGN DECISION, NOT A DETAIL. A
 * sequential generator (xorshift advanced once per weight) makes element
 * (tensor 214, row 900, column 12) reachable only by generating the 400
 * million elements before it. This one hashes (seed, tensor index, element
 * index) with splitmix64's finaliser, so ANY element is reproducible in O(1)
 * from the seed alone.
 *
 * That is what makes --verify possible on a file too large to regenerate: it
 * mmaps the model, walks the descriptors lm_open built, and re-derives the
 * FIRST AND LAST element of every tensor to compare against what is actually
 * on disk at that offset. An offset bug in the middle of the payload cannot
 * hide behind an accumulated coincidence -- lm_format_test.c's own argument
 * for checking the last float of the last tensor, applied to a file whose
 * last tensor is 155 million floats in.
 *
 * ---------------------------------------------------------------------------
 * THE SCALE IS 1/sqrt(fan_in), AND THE REASON IS NOT THE ONE THIS COMMENT
 * FIRST GAVE. It said: unit-variance weights compound through 28 layers,
 * reach f32's ceiling and produce NaN. `--unit-init` was added to prove it
 * and proved the opposite -- measured on this machine, dim 1024, hidden 3072,
 * q8, four tokens, seed 3:
 *
 *     layers    scaled init         unit init
 *        1      [-3.357,  3.353]    [-91.26,  93.09]
 *        4      [-3.248,  2.880]    [-111.6,  128.3]
 *       28      [-2.835,  3.906]    [-137.7,   92.32]
 *     non-finite logits: 0 in every one of those six runs.
 *
 * NOTHING COMPOUNDS, because this is a pre-norm architecture: every matmul's
 * input has just been through nn_rmsnorm, so each layer's contribution is set
 * by that layer's weights and not by what the previous 27 did. Depth moves
 * the range by 1.4x over a 28x change in depth. There is no overflow to
 * reach, and a claim that there is would have sent somebody looking for a
 * bug in the arithmetic.
 *
 * What the scale rule actually buys is the SCALE OF THE LAST MATMUL, and that
 * is derivable rather than remembered. The final rmsnorm hands the classifier
 * a vector of RMS ~1 over dim = 1024; a row of unit-variance weights dotted
 * with it is N(0, sqrt(1024)) = N(0, 32), and the largest of vocab = 256 such
 * draws sits near 2.89 sigma = 92.5 -- against the 93.09 measured. With
 * 1/sqrt(fan_in) the same row has standard deviation 1/32, the logit is
 * N(0,1), and 2.89 sigma is 2.89 -- against 3.35 measured. The 32x separating
 * the two columns above IS sqrt(dim), exactly.
 *
 * That still matters, for two reasons that are about measurement rather than
 * overflow: attention scores at that scale saturate the softmax into a
 * one-hot row, and a logit range of +-138 makes temperature and top-p
 * meaningless. A file built with --unit-init is a degenerate model that runs
 * perfectly well -- which is precisely why the failure had to be measured
 * instead of assumed.
 *
 * The distribution is UNIFORM on [-a, a] with a = sqrt(3/fan_in), not
 * Gaussian, and the two are interchangeable here for a stated reason: the
 * variance of U(-a,a) is a^2/3, so this choice has variance exactly 1/fan_in
 * -- the same as the N(0, 1/fan_in) a real init draws -- and variance is the
 * only property the scale argument above depends on. Box-Muller would cost a
 * log and a sqrt per weight, 596 million times, to change a fourth moment
 * nothing here reads.
 *
 * NORM GAINS ARE NOT 1.0, and this one is deliberate against what a real
 * initialisation does (which is exactly 1.0). A file whose every gain is one
 * cannot distinguish rmsnorm from a bare rescale, and -- see model.h's
 * lm_qk_norm comment -- cannot distinguish QK-norm applied BEFORE RoPE from
 * QK-norm applied after, because RoPE is a rotation and preserves the sum of
 * squares, so only a non-uniform gain sees the difference. Gains are drawn
 * from U(0.75, 1.25): strictly positive (a negative gain flips a sign and is
 * not what a trained model looks like), and with a coefficient of variation
 * of 1/(4*sqrt(3)) = 0.144, which is the same order as a trained model's.
 */
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#include "model.h"
#include "infer.h"
#include "nn.h"
#include "quant4.h"

/* mmap is used by --verify only. It is the host's version of the access the
 * device will get through SYS_MMAP_FILE, which is what the format was built
 * for -- so verifying a 568 MiB model costs no resident memory here for the
 * same reason it will not on device. */
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------- the PRNG --
 *
 * splitmix64's finaliser. Not xorshift: xorshift is a state ADVANCE and this
 * needs a HASH -- a function of the counter, so element n costs the same as
 * element 0. The finaliser is the standard one and passes the usual test
 * batteries as a counter-mode generator, which is the property being used. */
static uint64_t mix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* The stream address of one element: (seed, tensor, index). The tensor index
 * is multiplied by a large odd constant rather than shifted into a bitfield,
 * so a tensor of 155 million elements cannot run into the next tensor's
 * stream -- with a shift, the field width would be a cap on tensor size and
 * exceeding it would silently make two tensors share values. */
static double u01(uint64_t seed, uint32_t tensor, uint64_t idx)
{
    uint64_t h = mix64(seed ^ (tensor * 0xD1B54A32D192ED03ULL) ^ mix64(idx));
    /* 53 bits scaled by 2^-53: every value exactly representable in double
     * and the result in [0,1). The TOP bits, because a multiply-xorshift
     * finaliser's low bits are its weakest. */
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

/* ------------------------------------------------------- the tensor list --
 *
 * A FOURTH statement of model.h's payload order (after model.h itself,
 * model.c's walk and lm_format_test.c's builder), and being a separate
 * statement is the point rather than the cost: the file this writes is
 * checked against lm_expected_size, which walks the order independently, so
 * the two agreeing is evidence. A writer that asked model.c where to put
 * things would agree with model.c by construction and prove nothing. */
enum { TT_WEIGHT, TT_GAIN };

struct tdesc {
    const char *name;
    uint32_t    rows, cols;
    uint32_t    dt;        /* NN_F32, NN_Q8 or NN_Q4, per tensor -- norms stay f32 */
    int         kind;      /* TT_WEIGHT (1/sqrt(fan_in)) or TT_GAIN (near 1) */
};

struct spec {
    uint32_t dim, n_layers, n_heads, n_kv_heads, head_dim, hidden, vocab, seq_len;
    uint32_t dtype, flags;
    uint64_t seed;
    int      unit_init;    /* --unit-init: the negative control, see elem() */
};

/* Fill `out` with the descriptors, in payload order, and return how many.
 * With `out` NULL it only counts, so the caller can size the array from the
 * same code that fills it -- infer.c's layout() trick, and for the same
 * reason: two lists that must agree are one list called twice. */
static int build_list(const struct spec *sp, struct tdesc *out)
{
    uint32_t hd = sp->head_dim ? sp->head_dim : sp->dim / sp->n_heads;
    uint32_t q_dim  = sp->n_heads    * hd;
    uint32_t kv_dim = sp->n_kv_heads * hd;
    uint32_t edt = (sp->flags & LM_QEMB) ? sp->dtype : (uint32_t)NN_F32;
    int n = 0;

#define PUT(nm, r, c, d, k) do { \
        if (out) { out[n].name = (nm); out[n].rows = (r); out[n].cols = (c); \
                   out[n].dt = (d); out[n].kind = (k); } \
        n++; } while (0)

    PUT("tok_emb", sp->vocab, sp->dim, edt, TT_WEIGHT);
    for (uint32_t l = 0; l < sp->n_layers; l++) {
        PUT("att_norm", 1, sp->dim,   NN_F32,    TT_GAIN);
        PUT("wq",  q_dim,     sp->dim, sp->dtype, TT_WEIGHT);
        PUT("wk",  kv_dim,    sp->dim, sp->dtype, TT_WEIGHT);
        PUT("wv",  kv_dim,    sp->dim, sp->dtype, TT_WEIGHT);
        if (sp->flags & LM_QKNORM) {
            PUT("q_norm", 1, hd, NN_F32, TT_GAIN);
            PUT("k_norm", 1, hd, NN_F32, TT_GAIN);
        }
        PUT("wo",  sp->dim,    q_dim,   sp->dtype, TT_WEIGHT);
        PUT("ffn_norm", 1, sp->dim, NN_F32, TT_GAIN);
        PUT("w1",  sp->hidden, sp->dim,    sp->dtype, TT_WEIGHT);
        PUT("w3",  sp->hidden, sp->dim,    sp->dtype, TT_WEIGHT);
        PUT("w2",  sp->dim,    sp->hidden, sp->dtype, TT_WEIGHT);
    }
    PUT("final_norm", 1, sp->dim, NN_F32, TT_GAIN);
    if (!(sp->flags & LM_TIED))
        PUT("wcls", sp->vocab, sp->dim, sp->dtype, TT_WEIGHT);
#undef PUT
    return n;
}

/* One element's f32 value. This IS the model: everything else in the file is
 * layout. `t` is the descriptor index, which is why the list above must be
 * built the same way by the writer and the verifier -- and it is, because
 * both call build_list. */
static float elem(const struct tdesc *td, uint64_t seed, uint32_t t, uint64_t idx,
                  int unit_init)
{
    double u = u01(seed, t, idx);
    if (td->kind == TT_GAIN)
        return (float)(0.75 + 0.5 * u);                 /* U(0.75, 1.25) */
    /* --unit-init: the scale rule removed, unit variance instead. Kept
     * because it is the instrument that REFUTED this file's original claim
     * about NaN (see the header table) and is the only way to re-measure what
     * the rule buys after a change to the norms or the output head. It is not
     * called a negative control: it reddens nothing, because the model it
     * builds is degenerate and finite rather than broken. */
    if (unit_init) return (float)(2.0 * sqrt(3.0) * (u - 0.5));
    /* fan_in is the ROW LENGTH: a matvec's output element is a dot product
     * over cols terms. Using rows here (the output width) is the classic
     * transposed-fan mistake and would scale w2 by 1/sqrt(dim) where it needs
     * 1/sqrt(hidden) -- a factor of sqrt(3) at the preset's shape, in the one
     * tensor that feeds the residual stream. */
    double a = sqrt(3.0 / (double)td->cols);
    return (float)(a * (2.0 * u - 1.0));
}

static void header_of(struct lm_header *h, const struct spec *sp)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, LM_MAGIC, 8);
    h->version    = LM_VERSION;
    h->dtype      = sp->dtype;
    h->dim        = sp->dim;
    h->n_layers   = sp->n_layers;
    h->n_heads    = sp->n_heads;
    h->n_kv_heads = sp->n_kv_heads;
    h->hidden     = sp->hidden;
    h->vocab      = sp->vocab;
    h->seq_len    = sp->seq_len;
    h->flags      = sp->flags;
    h->head_dim   = sp->head_dim;
}

/* ------------------------------------------------------------- writing --
 *
 * NOTHING LARGER THAN ONE ROW IS HELD, with one stated exception. A q8 tensor
 * is [rows*cols int8][rows f32 scales] on disk -- the scales come AFTER all
 * the data -- so a single sequential pass has to keep the scale vector until
 * the payload is done. That is `rows` floats: 608 KiB for the preset's
 * embedding, the largest in the file, against a 568 MiB model. The
 * alternative is two passes over the tensor (generate to compute the scales,
 * generate again to quantise), which doubles the generation cost of the
 * largest files to save half a megabyte.
 *
 * Q4 MAKES THAT EXCEPTION 32x BIGGER AND IT IS STILL THE RIGHT TRADE, but the
 * number has to be said rather than inherited. A q4 tensor is
 * [packed][pad][rows*nblocks scales][rows*nblocks minima], so the deferred
 * metadata is 2 * rows * ceil(cols/Q4_BLOCK) floats, not `rows`. At the
 * preset with --qemb the embedding is 151936 x 1024 -> 16 blocks a row ->
 * 2,430,976 blocks -> 19.4 MB held while 78 MB of nibbles stream past it.
 * This tool is HOST ONLY (it is not linked into anything that boots), so 19.4
 * MB of a 15 GB host is not a constraint; on the device it would be, which is
 * exactly why the device never writes one of these files. */
static int write_model(const char *path, const struct spec *sp, int quiet)
{
    struct lm_header h;
    header_of(&h, sp);

    size_t want = lm_expected_size(&h);
    if (!want) {
        fprintf(stderr, "lmshape: lm_expected_size refuses this header --\n"
                "  the shape is not self-consistent, or a dtype/flag is not "
                "one the format knows.\n");
        return 1;
    }

    int nt = build_list(sp, NULL);
    struct tdesc *td = (struct tdesc *)calloc((size_t)nt, sizeof *td);
    if (!td) { fprintf(stderr, "lmshape: out of memory for %d descriptors\n", nt); return 1; }
    build_list(sp, td);

    uint32_t maxcols = 0, maxrows = 0;
    size_t   maxmeta = 0;          /* q4: the largest rows*nblocks in the file */
    for (int i = 0; i < nt; i++) {
        if (td[i].cols > maxcols) maxcols = td[i].cols;
        if (td[i].dt == NN_Q8 && td[i].rows > maxrows) maxrows = td[i].rows;
        if (td[i].dt == NN_Q4) {
            size_t mb = (size_t)td[i].rows *
                        (size_t)q4_blocks((int)td[i].cols, Q4_BLOCK);
            if (mb > maxmeta) maxmeta = mb;
        }
    }

    float  *row = (float *)malloc((size_t)maxcols * sizeof(float));
    /* `qrow` carries a q8 row (cols bytes) OR a q4 row (q4_row_bytes bytes),
     * and one buffer serves both because the second is never larger: with an
     * even block size a q4 row is at most ceil(cols/2) bytes, which is <= cols
     * for every cols >= 1. Asserted below rather than left to that sentence. */
    int8_t *qrow = (int8_t *)malloc((size_t)maxcols);
    float  *scales = maxrows ? (float *)malloc((size_t)maxrows * sizeof(float)) : NULL;
    float  *q4sc = maxmeta ? (float *)malloc(maxmeta * sizeof(float)) : NULL;
    float  *q4mn = (maxmeta && Q4_MODE == Q4_AFFINE)
                 ? (float *)malloc(maxmeta * sizeof(float)) : NULL;
    if (!row || !qrow || (maxrows && !scales) ||
        (maxmeta && (!q4sc || (Q4_MODE == Q4_AFFINE && !q4mn)))) {
        fprintf(stderr, "lmshape: out of memory for the row buffers\n");
        return 1;
    }
    if (maxmeta && q4_row_bytes((int)maxcols, Q4_BLOCK) > (size_t)maxcols) {
        fprintf(stderr, "lmshape: a q4 row of %u weights needs %llu bytes, and "
                "the shared row buffer is %u.\n", maxcols,
                (unsigned long long)q4_row_bytes((int)maxcols, Q4_BLOCK), maxcols);
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    if (fwrite(&h, 1, sizeof h, f) != sizeof h) { perror("write header"); fclose(f); return 1; }

    for (int i = 0; i < nt; i++) {
        const struct tdesc *t = &td[i];
        for (uint32_t r = 0; r < t->rows; r++) {
            uint64_t base = (uint64_t)r * t->cols;
            for (uint32_t c = 0; c < t->cols; c++)
                row[c] = elem(t, sp->seed, (uint32_t)i, base + c, sp->unit_init);
            if (t->dt == NN_Q8) {
                nn_quantize_q8(qrow, &scales[r], row, 1, (int)t->cols);
                if (fwrite(qrow, 1, t->cols, f) != t->cols) goto wfail;
            } else if (t->dt == NN_Q4) {
                /* THE REAL QUANTISER, for the reason this file's header gives
                 * about nn_quantize_q8 and states again for q4: a second
                 * implementation of "block range -> scale and minimum ->
                 * nearest nibble" would agree almost always, and the cases
                 * where it did not would be the ones worth finding. */
                size_t nb = (size_t)q4_blocks((int)t->cols, Q4_BLOCK);
                size_t rb = q4_row_bytes((int)t->cols, Q4_BLOCK);
                q4_quantize((uint8_t *)qrow, q4sc + (size_t)r * nb,
                            q4mn ? q4mn + (size_t)r * nb : 0,
                            row, 1, (int)t->cols, Q4_BLOCK, Q4_MODE);
                if (fwrite(qrow, 1, rb, f) != rb) goto wfail;
            } else {
                if (fwrite(row, sizeof(float), t->cols, f) != t->cols) goto wfail;
            }
        }
        if (t->dt == NN_Q8 &&
            fwrite(scales, sizeof(float), t->rows, f) != t->rows) goto wfail;
        if (t->dt == NN_Q4) {
            size_t nb  = (size_t)q4_blocks((int)t->cols, Q4_BLOCK);
            size_t pay = q4_payload_bytes((int)t->rows, (int)t->cols, Q4_BLOCK);
            size_t off = q4_scale_off((int)t->rows, (int)t->cols, Q4_BLOCK);
            size_t meta = (size_t)t->rows * nb;
            /* THE PAD IS WRITTEN, NOT SEEKED OVER, and it is written as zeroes.
             * A seek past the end of a file leaves a hole that reads as zero
             * on this host and is a portability question elsewhere; more to
             * the point, `want == st_size` at the bottom of this function is
             * the check that the writer and lm_expected_size agree, and a hole
             * would satisfy it while leaving the pad's contents unstated. It
             * is at most 3 bytes per tensor (quant4.h). */
            static const unsigned char zero4[4] = { 0, 0, 0, 0 };
            size_t pad = off - pay;
            if (pad && fwrite(zero4, 1, pad, f) != pad) goto wfail;
            if (fwrite(q4sc, sizeof(float), meta, f) != meta) goto wfail;
            if (Q4_MODE == Q4_AFFINE &&
                fwrite(q4mn, sizeof(float), meta, f) != meta) goto wfail;
        }
    }
    /* fclose, and its return value is CHECKED. A short write on a full disk
     * surfaces here and nowhere else -- every fwrite above can succeed into
     * the stdio buffer and the failure only becomes visible at the flush. A
     * tool that ignored it would report success and leave a truncated model,
     * which lm_open would then refuse with -5 in someone else's session. */
    if (fclose(f) != 0) { perror("close"); return 1; }

    struct stat st;
    if (stat(path, &st) != 0) { perror("stat"); return 1; }
    if ((size_t)st.st_size != want) {
        fprintf(stderr, "lmshape: WROTE %lld bytes, lm_expected_size says %llu.\n"
                "  These are two independent walks of the payload order and they "
                "disagree.\n", (long long)st.st_size, (unsigned long long)want);
        return 1;
    }
    if (!quiet) {
        printf("wrote %s\n", path);
        printf("  bytes           %llu  (== lm_expected_size)\n",
               (unsigned long long)want);
        printf("  tensors         %d\n", nt);
    }
    free(td); free(row); free(qrow); free(scales); free(q4sc); free(q4mn);
    return 0;
wfail:
    perror("write payload");
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------ verifying --
 *
 * mmap, lm_open, then re-derive two elements of every tensor from the seed.
 * Two and not all of them because "all" is the file itself: regenerating 596
 * million weights to compare them is a slower copy of the writer, and it
 * would check the GENERATOR, which is deterministic by construction, rather
 * than the LAYOUT, which is the thing that can be wrong.
 *
 * First and last, because those are the two that pin a tensor's extent from
 * both ends: a tensor placed one row early still has a plausible first
 * element (the previous tensor's tail is also pseudo-random) but cannot have
 * the right last one. */
static int verify(const char *path, const struct spec *sp, int have_seed)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
    size_t len = (size_t)st.st_size;
    void *blob = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (blob == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    struct lm_model m;
    int rc = lm_open(&m, blob, len);
    printf("%s\n", path);
    printf("  size            %llu bytes\n", (unsigned long long)len);
    printf("  lm_open         %d %s\n", rc, rc ? "REFUSED" : "ok");
    if (rc) { munmap(blob, len); close(fd); return 1; }

    char desc[256];
    lm_describe(&m, desc, sizeof desc);
    printf("  %s\n", desc);
    printf("  lm_expected_size %llu %s\n",
           (unsigned long long)lm_expected_size(&m.h),
           lm_expected_size(&m.h) == len ? "== file size" : "MISMATCH");
    printf("  q_dim           %d   kv_dim %d   head_dim %d\n",
           m.q_dim, m.kv_dim, m.head_dim);

    /* The KV cache, printed here rather than left to be discovered: it is a
     * SECOND budget, it is not small, and it is a pure function of the header
     * so this is the one place that already knows it. */
    double kvtok = (double)m.h.n_layers * m.kv_dim * 2.0 * sizeof(float);
    printf("  KV cache        %.0f B/token, %.1f MiB at seq_len %u\n",
           kvtok, kvtok * m.h.seq_len / 1048576.0, m.h.seq_len);

    int bad = 0;
    if (have_seed) {
        int nt = build_list(sp, NULL);
        struct tdesc *td = (struct tdesc *)calloc((size_t)nt, sizeof *td);
        build_list(sp, td);
        /* The descriptors lm_open built, in payload order, so the comparison
         * is against where the LOADER thinks each tensor is -- not against an
         * offset this function recomputed, which would be checking the writer
         * against itself. */
        const struct nn_tensor **tens =
            (const struct nn_tensor **)calloc((size_t)nt, sizeof *tens);
        const float **gains = (const float **)calloc((size_t)nt, sizeof *gains);
        int k = 0;
        tens[k] = &m.emb; k++;
        for (uint32_t l = 0; l < m.h.n_layers; l++) {
            const struct lm_layer *L = &m.layer[l];
            gains[k] = L->att_norm; k++;
            tens[k] = &L->wq; k++;
            tens[k] = &L->wk; k++;
            tens[k] = &L->wv; k++;
            if (m.h.flags & LM_QKNORM) { gains[k] = L->q_norm; k++;
                                         gains[k] = L->k_norm; k++; }
            tens[k] = &L->wo; k++;
            gains[k] = L->ffn_norm; k++;
            tens[k] = &L->w1; k++;
            tens[k] = &L->w3; k++;
            tens[k] = &L->w2; k++;
        }
        gains[k] = m.final_norm; k++;
        if (!(m.h.flags & LM_TIED)) { tens[k] = &m.wcls; k++; }

        if (k != nt) {
            printf("  FAIL: the loader gave %d tensors, the writer's list has %d\n", k, nt);
            bad++;
        } else {
            double worst = 0.0;
            const char *worst_name = "";
            for (int i = 0; i < nt; i++) {
                uint64_t n = (uint64_t)td[i].rows * td[i].cols;
                uint64_t idx[2] = { 0, n - 1 };
                for (int e = 0; e < 2; e++) {
                    float want = elem(&td[i], sp->seed, (uint32_t)i, idx[e], sp->unit_init);
                    float got; double tol;
                    if (gains[i]) { got = gains[i][idx[e]]; tol = 0.0; }
                    else if (tens[i]->dtype == NN_F32) {
                        got = tens[i]->data[idx[e]]; tol = 0.0;
                    } else if (tens[i]->dtype == NN_Q4) {
                        /* The element is reconstructed through q4_dequantize
                         * -- the real one -- over the single row it lives in,
                         * so this checks the LAYOUT (is that element where the
                         * loader says it is) and not the quantiser, which
                         * tests/unit/quant4_test.c already gates against a
                         * double reference. */
                        uint32_t r = (uint32_t)(idx[e] / td[i].cols);
                        uint32_t c = (uint32_t)(idx[e] % td[i].cols);
                        int k = (int)td[i].cols;
                        size_t rb = q4_row_bytes(k, Q4_BLOCK);
                        size_t nb = (size_t)q4_blocks(k, Q4_BLOCK);
                        const float *mn = tens[i]->data;
                        float *rowbuf = (float *)malloc((size_t)k * sizeof(float));
                        q4_dequantize(rowbuf,
                                      (const uint8_t *)tens[i]->q + (size_t)r * rb,
                                      tens[i]->scale + (size_t)r * nb,
                                      mn ? mn + (size_t)r * nb : 0,
                                      1, k, Q4_BLOCK, Q4_MODE);
                        got = rowbuf[c];
                        /* DERIVED, not fitted, and it is the BLOCK's range
                         * rather than the row's: q4 rounds to the nearest of
                         * 16 levels spanning one block's own extrema, so the
                         * error of any element is at most half a step and the
                         * step is that block's stored scale. Reading the scale
                         * back rather than recomputing the range keeps this a
                         * check on placement -- a tensor read one row early
                         * gets a plausible value and the WRONG scale, and the
                         * bound is what notices. */
                        tol = 0.5 * (double)tens[i]->scale[(size_t)r * nb +
                                                           (size_t)(c / Q4_BLOCK)];
                        free(rowbuf);
                    } else {
                        uint32_t r = (uint32_t)(idx[e] / td[i].cols);
                        float s = tens[i]->scale[r];
                        got = (float)tens[i]->q[idx[e]] * s;
                        /* DERIVED, not fitted: nn_quantize_q8 rounds to the
                         * nearest multiple of scale = max|row|/127, so the
                         * reconstruction error of any element is at most half
                         * a step. Anything above that is not quantisation. */
                        tol = 0.5 * (double)s;
                    }
                    double err = fabs((double)got - (double)want);
                    if (err > worst) { worst = err; worst_name = td[i].name; }
                    if (err > tol) {
                        printf("  FAIL: %s[%llu] on disk %.9g, from the seed %.9g "
                               "(err %.3g > %.3g)\n", td[i].name,
                               (unsigned long long)idx[e], (double)got,
                               (double)want, err, tol);
                        bad++;
                    }
                }
            }
            printf("  spot check      %d tensors x {first,last} = %d elements, "
                   "worst |disk - seed| %.4g (%s)\n", nt, nt * 2, worst, worst_name);

            /* THE EMBEDDING AGAIN, THROUGH lm_embed_row, and it is a second
             * check of the same bytes on purpose. The loop above reads the
             * embedding as a TENSOR -- through the descriptor lm_open built --
             * and lm_embed_row is a different reader of the same table, with
             * its own row stride, its own scale index and (for q4) its own
             * dequantiser call. Nothing else in this tree calls it: infer.c's
             * one caller arrives through the hookup, and no host gate runs
             * that path at a quantised embedding. So a wrong stride there was
             * invisible everywhere while every other check stayed green.
             *
             * Two rows, first and last, for the reason the tensor loop gives:
             * a stride that drifts by a byte a row still produces a plausible
             * row 0 and cannot produce the right row vocab-1. The bound is the
             * one the tensor loop derives -- half a step of whichever scale
             * governs that element -- because lm_embed_row's answer must equal
             * the same reconstruction the descriptor gives. */
            const struct tdesc *et = &td[0];         /* tok_emb is index 0 */
            float *erow = (float *)malloc((size_t)m.h.dim * sizeof(float));
            int etok[2] = { 0, (int)m.h.vocab - 1 };
            double eworst = 0.0, ebound = 0.0;
            int ebad = 0;
            for (int e = 0; e < 2 && erow; e++) {
                if (lm_embed_row(&m, etok[e], erow) != 0) {
                    printf("  FAIL: lm_embed_row refused token %d (dtype %d)\n",
                           etok[e], m.emb.dtype);
                    ebad++; bad++; continue;
                }
                for (uint32_t c = 0; c < m.h.dim; c++) {
                    uint64_t idx = (uint64_t)etok[e] * m.h.dim + c;
                    double want = elem(et, sp->seed, 0, idx, sp->unit_init);
                    double err = fabs((double)erow[c] - want);
                    double tol = 0.0;
                    if (m.emb.dtype == NN_Q8)
                        tol = 0.5 * (double)m.emb.scale[etok[e]];
                    else if (m.emb.dtype == NN_Q4) {
                        size_t nbe = (size_t)q4_blocks((int)m.h.dim, Q4_BLOCK);
                        tol = 0.5 * (double)m.emb.scale[(size_t)etok[e] * nbe
                                                        + c / Q4_BLOCK];
                    }
                    if (err > eworst) { eworst = err; ebound = tol; }
                    if (err > tol) ebad++;
                }
            }
            if (ebad) {
                printf("  FAIL: lm_embed_row disagrees with the seed on %d of "
                       "%u elements (worst %.4g against bound %.4g)\n",
                       ebad, 2u * m.h.dim, eworst, ebound);
                bad++;
            } else {
                printf("  lm_embed_row    2 rows x %u = %u elements, worst "
                       "%.4g against its own half-step bound %.4g\n",
                       m.h.dim, 2u * m.h.dim, eworst, ebound);
            }
            free(erow);
        }
        free(td); free(tens); free(gains);
    } else {
        printf("  spot check      SKIPPED -- pass --seed N (and the shape) to "
               "re-derive the weights\n");
    }

    lm_close(&m);
    munmap(blob, len);
    close(fd);
    if (bad) { printf("  VERIFY FAILED: %d check(s)\n", bad); return 1; }
    printf("  VERIFY OK\n");
    return 0;
}

/* ------------------------------------------------------- the forward pass --
 *
 * THE POINT OF THE WHOLE TOOL, and it is deliberately here rather than in
 * /bin/lm: lm.c reads the model with fread into a malloc'd buffer under a 256
 * MiB budget, which is right for the device and cannot open a 930 MiB file at
 * all. This mode mmaps instead -- the access the format was designed for, and
 * the one SYS_MMAP_FILE will give on device -- so the question "does the
 * stack survive 28 layers of 1024" can be answered on the host today, before
 * the kernel side of that work lands.
 *
 * It prints tok/s and the GFLOP/s that follows from the parameter count,
 * because a rate with no arithmetic behind it cannot be compared with the
 * 12.94 GFLOP/s this host is already measured at.
 *
 * THE FIRST TOKEN IS TIMED SEPARATELY FROM THE REST, and that is not a
 * refinement -- at this size it is the difference between two numbers that
 * differ by more than an order of magnitude. mmap is lazy, so the first
 * forward pass faults in the ENTIRE model from disk (278 MiB at the shape
 * below) while doing one token of arithmetic. Folding that into an average
 * over 32 tokens reports neither the load cost nor the decode rate, and the
 * blend moves with `--tokens`, so it is not even a stable wrong answer. The
 * device is the same story with `fread` instead of a fault, which is why
 * /bin/lm prints its load time on its own line too.
 *
 * PEAK RSS COMES FROM VmHWM, not from adding up what this function allocated.
 * The model is MAPPED: its pages are resident because they were touched, not
 * because anything here asked for them, so an accounting built from malloc
 * sizes would miss the largest term in the budget entirely -- which is the one
 * term the question "does this fit in 512 MiB" is about.
 *
 * --against IS THE ONLY CORRECTNESS EVIDENCE AVAILABLE AT THIS SHAPE. The
 * weights are pseudo-random, so there is no perplexity worth printing and no
 * reference output to diff. What CAN be checked is that two files built from
 * the SAME SEED at different dtypes produce the same logits to within the
 * quantisation error -- and the two models are driven along ONE token
 * sequence, the reference's, rather than each along its own greedy chain. Two
 * chains would diverge at the first position where the argmax flips and every
 * number after it would be a comparison of two different prompts. */

/* VmHWM: the kernel's own high-water mark for this process's resident set.
 * Read from /proc rather than tracked here for the reason above -- and it is
 * a Linux file, so a host without it gets 0 and the line says "unavailable"
 * instead of a fabricated number. */
static long proc_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long v = 0;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0) {
            const char *p = line + klen;
            while (*p == ':' || *p == ' ' || *p == '\t') p++;
            v = strtol(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* One mapped, opened model plus its state. Kept as a struct because --against
 * needs two of everything and a second set of six locals is how one of them
 * ends up closed twice. */
struct loaded {
    int fd;
    void *blob;
    size_t len;
    struct lm_model m;
    struct lm_state s;
    int have_state;
    double t_map, t_state;
};

static int load(struct loaded *o, const char *path)
{
    memset(o, 0, sizeof *o);
    double t0 = now_s();
    o->fd = open(path, O_RDONLY);
    if (o->fd < 0) { perror(path); return 1; }
    struct stat st;
    if (fstat(o->fd, &st) != 0) { perror("fstat"); return 1; }
    o->len = (size_t)st.st_size;
    o->blob = mmap(NULL, o->len, PROT_READ, MAP_PRIVATE, o->fd, 0);
    if (o->blob == MAP_FAILED) { perror("mmap"); o->blob = NULL; return 1; }
    int rc = lm_open(&o->m, o->blob, o->len);
    if (rc) { fprintf(stderr, "lmshape: lm_open(%s) = %d\n", path, rc); return 1; }
    o->t_map = now_s() - t0;

    t0 = now_s();
    if (lm_state_new(&o->s, &o->m) != 0) {
        fprintf(stderr, "lmshape: lm_state_new refused (%.2f MiB)\n",
                lm_state_bytes(&o->m) / 1048576.0);
        return 1;
    }
    o->have_state = 1;
    o->t_state = now_s() - t0;
    return 0;
}

static void unload(struct loaded *o)
{
    if (o->have_state) lm_state_free(&o->s);
    if (o->m.layer) lm_close(&o->m);
    if (o->blob) munmap(o->blob, o->len);
    if (o->fd >= 0) close(o->fd);
    memset(o, 0, sizeof *o);
    o->fd = -1;
}

/* Multiply-adds per token: every matvec weight, twice (a multiply and an
 * add). The embedding is counted ONCE -- as the classifier, which is a
 * matvec -- and not for the lookup, which is a memcpy. */
static double matvec_params(const struct lm_model *m)
{
    double par = 0.0;
    for (uint32_t l = 0; l < m->h.n_layers; l++) {
        const struct lm_layer *L = &m->layer[l];
        par += (double)L->wq.dim[0] * L->wq.dim[1] + (double)L->wk.dim[0] * L->wk.dim[1]
             + (double)L->wv.dim[0] * L->wv.dim[1] + (double)L->wo.dim[0] * L->wo.dim[1]
             + (double)L->w1.dim[0] * L->w1.dim[1] + (double)L->w3.dim[0] * L->w3.dim[1]
             + (double)L->w2.dim[0] * L->w2.dim[1];
    }
    par += (double)m->wcls.dim[0] * m->wcls.dim[1];
    return par;
}

static int forward(const char *path, int ntok, const char *refpath)
{
    struct loaded A;
    if (load(&A, path)) { unload(&A); return 1; }

    char desc[256];
    lm_describe(&A.m, desc, sizeof desc);
    printf("%s\n", desc);
    printf("  file            %.1f MiB   (%llu bytes)\n",
           A.len / 1048576.0, (unsigned long long)A.len);
    printf("  open            %.3f s to mmap + lm_open (the pages are NOT read\n"
           "                  here -- the first token faults them in)\n", A.t_map);

    size_t sb = lm_state_bytes(&A.m);
    double kv = (double)A.m.h.n_layers * A.m.h.seq_len * A.m.kv_dim * 2 * sizeof(float);
    printf("  lm_state_bytes  %.2f MiB  (KV cache %.2f MiB of it, %.0f B/token)\n"
           "                  allocated + zeroed in %.3f s\n",
           sb / 1048576.0, kv / 1048576.0,
           (double)A.m.h.n_layers * A.m.kv_dim * 2 * sizeof(float), A.t_state);
    /* The equality infer.h promises, checked here rather than trusted -- this
     * is the one place in the tree that runs it at a shape where the two
     * halves of the layout are not the same number. */
    if (sb != A.s.arena_len)
        printf("  MISMATCH        lm_state_bytes %llu != arena %llu\n",
               (unsigned long long)sb, (unsigned long long)A.s.arena_len);

    double par = matvec_params(&A.m);
    printf("  matvec params   %.0f  ->  %.3f GFLOP/token\n", par, 2.0 * par / 1e9);

    struct loaded B;
    B.fd = -1; B.blob = NULL; B.have_state = 0; B.m.layer = NULL;
    if (refpath) {
        if (load(&B, refpath)) { unload(&A); unload(&B); return 1; }
        const struct lm_header *a = &A.m.h, *b = &B.m.h;
        if (a->dim != b->dim || a->n_layers != b->n_layers ||
            a->n_heads != b->n_heads || a->n_kv_heads != b->n_kv_heads ||
            a->head_dim != b->head_dim || a->hidden != b->hidden ||
            a->vocab != b->vocab || a->seq_len != b->seq_len) {
            printf("  --against REFUSED: the two files are different SHAPES, so a\n"
                   "    logit difference between them would not be about dtype.\n");
            unload(&A); unload(&B); return 1;
        }
        lm_describe(&B.m, desc, sizeof desc);
        printf("  against         %s\n", desc);
        printf("                  %.1f MiB\n", B.len / 1048576.0);
    }

    if (ntok > (int)A.m.h.seq_len) ntok = (int)A.m.h.seq_len;
    if (ntok < 1) ntok = 1;

    double sum = 0.0, lo = 1e300, hi = -1e300;
    int tok = 0, done = 0;
    long nonfinite = 0;
    double t_first = 0.0, t_rest = 0.0;
    /* The comparison's accumulators. `dmax` is the raw worst logit difference;
     * `drms` is over the MEAN-CENTRED difference, because softmax is invariant
     * to a constant added to a whole logit row -- so a uniform shift is
     * arithmetic noise the model cannot express and counting it would inflate
     * the number without changing a single prediction. */
    double dmax = 0.0, dss = 0.0;
    long dn = 0, argmax_same = 0;

    for (int i = 0; i < ntok; i++) {
        double t0 = now_s();
        const float *lg = lm_forward(&A.m, &A.s, tok, i);
        double dt = now_s() - t0;
        if (!lg) {
            printf("  lm_forward      REFUSED at position %d\n", i);
            break;
        }
        if (i == 0) t_first = dt; else t_rest += dt;

        const float *rg = NULL;
        if (refpath) {
            rg = lm_forward(&B.m, &B.s, tok, i);
            if (!rg) {
                printf("  reference lm_forward REFUSED at position %d\n", i);
                break;
            }
        }

        double mean = 0.0;
        for (uint32_t j = 0; j < A.m.h.vocab; j++) {
            double L = (double)lg[j];
            if (L != L || L > 1e30 || L < -1e30) nonfinite++;
            if (L < lo) lo = L;
            if (L > hi) hi = L;
            if (rg) mean += (double)lg[j] - (double)rg[j];
        }
        if (rg) {
            mean /= (double)A.m.h.vocab;
            for (uint32_t j = 0; j < A.m.h.vocab; j++) {
                double d = (double)lg[j] - (double)rg[j];
                if (fabs(d) > dmax) dmax = fabs(d);
                double c = d - mean;
                dss += c * c;
                dn++;
            }
            if (lm_sample_greedy(lg, (int)A.m.h.vocab) ==
                lm_sample_greedy(rg, (int)A.m.h.vocab)) argmax_same++;
        }

        /* The chain is driven by the REFERENCE when there is one, so both
         * models see byte-identical inputs at every position. Driving each by
         * its own argmax would compare two different prompts the moment one
         * token flipped. */
        tok = lm_sample_greedy(rg ? rg : lg, (int)A.m.h.vocab);
        sum += (double)lg[tok];
        done++;
    }

    if (done) {
        double secs = t_first + t_rest;
        printf("  first token     %.3f s   (arithmetic + faulting the whole file in)\n",
               t_first);
        if (done > 1)
            printf("  steady state    %d tokens in %.3f s -> %.2f tok/s, %.2f GFLOP/s\n",
                   done - 1, t_rest, (done - 1) / t_rest,
                   2.0 * par * (done - 1) / t_rest / 1e9);
        printf("  including load  %d tokens in %.3f s -> %.2f tok/s\n",
               done, secs, done / secs);
        printf("  logit checksum  %.9g   (deterministic: same file, same number)\n", sum);
        printf("  logit range     [%.4g, %.4g], non-finite %ld of %llu\n",
               lo, hi, nonfinite, (unsigned long long)A.m.h.vocab * done);
    }

    long hwm = proc_kb("VmHWM");
    long rss = proc_kb("VmRSS");
    if (hwm)
        printf("  peak RSS        %.1f MiB (VmHWM), now %.1f MiB%s\n",
               hwm / 1024.0, rss / 1024.0,
               refpath ? " -- BOTH models, so not the single-model figure" : "");
    else
        printf("  peak RSS        unavailable (no /proc/self/status)\n");

    if (refpath && dn) {
        printf("  vs reference    max|dlogit| %.5g   rms(mean-centred) %.5g\n"
                "                  argmax agrees at %ld of %d positions\n",
               dmax, sqrt(dss / (double)dn), argmax_same, done);
    }

    int fail = nonfinite ? 1 : 0;
    if (nonfinite) printf("  FORWARD FAILED: %ld non-finite logits\n", nonfinite);
    unload(&A);
    if (refpath) unload(&B);
    return (done && !fail) ? 0 : 1;
}

/* ---------------------------------------------------------------- main -- */

static void usage(void)
{
    fprintf(stderr,
"lmshape -- write a LOGITLM file of any shape, deterministic from a seed.\n"
"\n"
"  --out FILE          write the model\n"
"  --verify FILE       open an existing model and check it (add --seed and\n"
"                      the shape to also re-derive its weights)\n"
"  --forward FILE      mmap it and run --tokens N through lm_forward\n"
"  --tokens N          how many (default 8)\n"
"  --against FILE      run a SECOND model of the same shape along the same\n"
"                      token sequence and report max|dlogit|. The two files\n"
"                      must differ only in --dtype, or the difference is not\n"
"                      about quantisation.\n"
"  --dry-run           print the shape and the byte total, write nothing\n"
"\n"
"  --preset NAME       qwen3-0.6b | tiny\n"
"  --dim N --layers N --heads N --kv-heads N --head-dim N\n"
"  --hidden N --vocab N --seq N\n"
"  --dtype f32|q8|q4   of the MATMUL weights (norms are always f32)\n"
"  --seed N            default 1\n"
"  --unit-init         drop the 1/sqrt(fan_in) scale (unit variance). It does\n"
"                      NOT overflow -- it multiplies the logit range by\n"
"                      sqrt(dim); see the header table. Measured, not assumed.\n"
"  --tied / --untied   tie the output head to the embedding (default tied)\n"
"  --qknorm            per-layer q/k RMS-norm gains (Qwen3)\n"
"  --qemb              store the embedding at --dtype instead of f32\n");
}

static uint32_t u32arg(const char *v, const char *what)
{
    char *end;
    unsigned long long x = strtoull(v, &end, 10);
    if (*end || x > 0xFFFFFFFFULL) {
        fprintf(stderr, "lmshape: %s: not a 32-bit number: %s\n", what, v);
        exit(2);
    }
    return (uint32_t)x;
}

int main(int argc, char **argv)
{
    struct spec sp;
    memset(&sp, 0, sizeof sp);
    /* The default IS the shape everything in this line has been measured on
     * (tools/lmtrain.md's model), so `lmshape --out x.lm` produces something
     * comparable to what exists rather than something arbitrary. */
    sp.dim = 128; sp.n_layers = 4; sp.n_heads = 4; sp.n_kv_heads = 4;
    sp.hidden = 344; sp.vocab = 256; sp.seq_len = 256;
    sp.dtype = NN_F32; sp.flags = LM_TIED; sp.seed = 1;

    const char *out = NULL, *ver = NULL, *fwd = NULL, *against = NULL;
    int dry = 0, have_seed = 1, ntok = 8;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT(w) (++i < argc ? argv[i] : (fprintf(stderr, "lmshape: %s needs a value\n", w), exit(2), ""))
        if      (!strcmp(a, "--out"))      out = NEXT("--out");
        else if (!strcmp(a, "--verify"))   ver = NEXT("--verify");
        else if (!strcmp(a, "--dry-run"))  dry = 1;
        else if (!strcmp(a, "--forward"))  fwd = NEXT("--forward");
        else if (!strcmp(a, "--tokens"))   ntok = (int)u32arg(NEXT("--tokens"), "--tokens");
        else if (!strcmp(a, "--against"))  against = NEXT("--against");
        else if (!strcmp(a, "--dim"))      sp.dim = u32arg(NEXT("--dim"), "--dim");
        else if (!strcmp(a, "--layers"))   sp.n_layers = u32arg(NEXT("--layers"), "--layers");
        else if (!strcmp(a, "--heads"))    sp.n_heads = u32arg(NEXT("--heads"), "--heads");
        else if (!strcmp(a, "--kv-heads")) sp.n_kv_heads = u32arg(NEXT("--kv-heads"), "--kv-heads");
        else if (!strcmp(a, "--head-dim")) sp.head_dim = u32arg(NEXT("--head-dim"), "--head-dim");
        else if (!strcmp(a, "--hidden"))   sp.hidden = u32arg(NEXT("--hidden"), "--hidden");
        else if (!strcmp(a, "--vocab"))    sp.vocab = u32arg(NEXT("--vocab"), "--vocab");
        else if (!strcmp(a, "--seq"))      sp.seq_len = u32arg(NEXT("--seq"), "--seq");
        else if (!strcmp(a, "--seed"))     sp.seed = strtoull(NEXT("--seed"), NULL, 10);
        else if (!strcmp(a, "--no-seed"))  have_seed = 0;
        else if (!strcmp(a, "--unit-init")) sp.unit_init = 1;
        else if (!strcmp(a, "--tied"))     sp.flags |= LM_TIED;
        else if (!strcmp(a, "--untied"))   sp.flags &= ~(uint32_t)LM_TIED;
        else if (!strcmp(a, "--qknorm"))   sp.flags |= LM_QKNORM;
        else if (!strcmp(a, "--qemb"))     sp.flags |= LM_QEMB;
        else if (!strcmp(a, "--dtype")) {
            const char *d = NEXT("--dtype");
            if      (!strcmp(d, "f32")) sp.dtype = NN_F32;
            else if (!strcmp(d, "q8"))  sp.dtype = NN_Q8;
            /* q4 was refused here, with a four-line note listing the three
             * places that would have to change when quant4.c landed. All three
             * are done (model.c's mat_bytes and wrap_mat, lm_expected_size's
             * dtype check) and the note is deleted rather than left standing --
             * a refusal that names a plan is useful exactly until the plan is
             * executed, after which it is a claim the tree contradicts. */
            else if (!strcmp(d, "q4"))  sp.dtype = NN_Q4;
            else { fprintf(stderr, "lmshape: --dtype: f32|q8|q4\n"); return 2; }
        }
        else if (!strcmp(a, "--preset")) {
            const char *p = NEXT("--preset");
            if (!strcmp(p, "qwen3-0.6b")) {
                /* Qwen3-0.6B's shape, and every one of these is load-bearing
                 * somewhere: head_dim 128 is NOT dim/n_heads (see model.h),
                 * 16 over 8 is the GQA ratio nothing had tested above 2:1,
                 * and the 151936-entry vocabulary is what makes an f32
                 * embedding 594 MiB. seq_len is the ONE number here that is a
                 * choice rather than the architecture: 512 costs 112 MiB of
                 * KV cache on a 512 MiB machine, which is already the largest
                 * single allocation in the system. */
                sp.dim = 1024; sp.n_layers = 28; sp.n_heads = 16;
                sp.n_kv_heads = 8; sp.head_dim = 128; sp.hidden = 3072;
                sp.vocab = 151936; sp.seq_len = 512;
                sp.flags |= LM_TIED | LM_QKNORM;
            } else if (!strcmp(p, "tiny")) {
                sp.dim = 64; sp.n_layers = 2; sp.n_heads = 16; sp.n_kv_heads = 8;
                sp.head_dim = 0; sp.hidden = 32; sp.vocab = 8; sp.seq_len = 8;
                sp.flags |= LM_TIED;
            } else { fprintf(stderr, "lmshape: unknown preset %s\n", p); return 2; }
        }
        else { usage(); return 2; }
#undef NEXT
    }

    if (fwd) return forward(fwd, ntok, against);
    if (ver) return verify(ver, &sp, have_seed);

    struct lm_header h;
    header_of(&h, &sp);
    size_t want = lm_expected_size(&h);
    uint32_t hd = sp.head_dim ? sp.head_dim : (sp.n_heads ? sp.dim / sp.n_heads : 0);

    printf("shape   dim=%u layers=%u heads=%u kv_heads=%u head_dim=%u "
           "hidden=%u vocab=%u seq=%u\n",
           sp.dim, sp.n_layers, sp.n_heads, sp.n_kv_heads, hd,
           sp.hidden, sp.vocab, sp.seq_len);
    printf("flags   %s%s%s dtype=%s seed=%llu\n",
           (sp.flags & LM_TIED) ? "tied " : "untied ",
           (sp.flags & LM_QKNORM) ? "qknorm " : "",
           (sp.flags & LM_QEMB) ? "qemb " : "",
           sp.dtype == NN_Q8 ? "q8" : sp.dtype == NN_Q4 ? "q4" : "f32",
           (unsigned long long)sp.seed);
    if (!want) {
        fprintf(stderr, "lmshape: lm_expected_size refuses this header.\n");
        return 1;
    }
    printf("bytes   %llu  (%.1f MiB)\n", (unsigned long long)want, want / 1048576.0);

    if (dry) return 0;
    if (!out) { usage(); return 2; }
    return write_model(out, &sp, 0);
}
