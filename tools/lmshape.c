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
 *
 * ===========================================================================
 * --weights: REAL WEIGHTS, THROUGH THE SAME WRITER
 * ===========================================================================
 *
 * `--weights FILE.gguf` replaces the PRNG and NOTHING ELSE. The seam is
 * elem(): it used to be `hash(seed, tensor, index) -> a number`, and it is now
 * `if there is a GGUF, read element `index` of the tensor this descriptor is
 * bound to; otherwise hash`. The header, build_list's payload order,
 * write_model's streaming pass, q4_quantize, nn_quantize_q8, the byte total
 * checked against lm_expected_size and the whole of --verify are untouched and
 * still gated by exactly what gated them before.
 *
 * That is the point rather than a convenience. A converter written as its own
 * program would be a SECOND writer of this format: a second statement of the
 * payload order, a second call sequence into the quantiser, a second place for
 * the q4 alignment pad to be got wrong. There is one writer, and --verify
 * checks the file it produced against the same source elem() read from -- so
 * "the weights are where the loader thinks they are" is checked for real
 * weights by the code that already checked it for pseudo-random ones.
 *
 * ---------------------------------------------------------------------------
 * TRANSPOSITION: NONE, AND HERE IS WHY AND HOW IT WAS PROVED.
 *
 * GGUF's `dims` list is written FASTEST-DIMENSION-FIRST: dim[0] is the
 * contiguous one. llama.cpp exports a torch Linear of shape [out, in] as
 * ne = [in, out] with the bytes UNCHANGED -- so the storage is `out` rows of
 * `in` contiguous values, which is [out, in] row-major. LOGITLM wants exactly
 * that: [n, k] row-major with k contiguous, because nn_matvec_f32(y, w, x, n,
 * k) walks w row by row. The two agree BYTE FOR BYTE and the copy is a
 * straight one.
 *
 * THE SHAPES PIN IT, and the third column below is not this file's opinion --
 * it is what `transformers` itself builds from the config.json that shipped
 * beside these weights, read mechanically rather than remembered:
 *
 *     python -c "
 *       import torch; from transformers import AutoConfig, AutoModelForCausalLM
 *       cfg = AutoConfig.from_pretrained('build/qwen')
 *       with torch.device('meta'): m = AutoModelForCausalLM.from_config(cfg)
 *       [print(n, tuple(p.shape)) for n,p in m.named_parameters()]"
 *
 *     GGUF name                 GGUF dims       our [rows,cols]  torch says
 *     token_embd.weight         [1024, 151936]  [151936, 1024]   (151936, 1024)
 *     blk.N.attn_q.weight       [1024, 2048]    [  2048, 1024]   (  2048, 1024)
 *     blk.N.attn_k.weight       [1024, 1024]    [  1024, 1024]   (  1024, 1024)
 *     blk.N.attn_v.weight       [1024, 1024]    [  1024, 1024]   (  1024, 1024)
 *     blk.N.attn_output.weight  [2048, 1024]    [  1024, 2048]   (  1024, 2048)
 *     blk.N.ffn_gate.weight     [1024, 3072]    [  3072, 1024]   (  3072, 1024)
 *     blk.N.ffn_up.weight       [1024, 3072]    [  3072, 1024]   (  3072, 1024)
 *     blk.N.ffn_down.weight     [3072, 1024]    [  1024, 3072]   (  1024, 3072)
 *
 * All eight agree, and the FOUR ASYMMETRIC ones are what make that a proof
 * rather than a coincidence. q_proj is (2048, 1024) and o_proj is (1024,
 * 2048); read the other way round our q_proj would come out (1024, 2048),
 * which is the shape torch gives o_proj. A convention that is wrong cannot
 * satisfy both rows, so the pair fixes it with no appeal to the spec at all.
 *
 * The SQUARE ones are the dangerous ones and are why the control exists: wk
 * and wv are 1024x1024, so transposing either changes no size, no byte total
 * and no structural check anywhere in this tool.
 *
 * A SHAPE ARGUMENT IS NOT A PROOF THAT THE CODE IMPLEMENTS IT, so `--matvec`
 * does one: it dequantises one named GGUF tensor into f32, runs the REAL
 * kernel (nn_matvec_f32) over it with a fixed x, and writes [n][k][x][y] to a
 * file that tools/gguf_check.py recomputes from an independent numpy load of
 * the same tensor in float64 (and in torch where torch exists). It is aimed at
 * blk.0.attn_k, the SQUARE one, because on a square tensor the shape argument
 * above says nothing and only the arithmetic can. Measured, host:
 *
 *     max|dy| 1.03583e-06, relative 4.81e-07, against a bound of 1e-3
 *     torch 2.13.0+cu132 reports the identical 1.03583e-06
 *
 * That residual is f32 accumulation over 1024 terms against f64, nothing else.
 * `--neg-transpose wk` is the control and reads 2.58929, relative 1.2.
 *
 * ---------------------------------------------------------------------------
 * TWO ARCHITECTURE CONSTANTS THE HEADER COULD NOT CARRY, AND NOW CAN. THIS
 * PARAGRAPH DESCRIBED A REFUSAL FOR AS LONG AS THE REFUSAL WAS TRUE, and it is
 * corrected in place rather than deleted, because the reasoning is the whole
 * reason the file it produces is worth trusting.
 *
 * It said: struct lm_header is 64 bytes, every field is spoken for, a reader
 * must REFUSE a non-zero `reserved`, so there is nowhere to put
 *
 *     qwen3.rope.freq_base                    1000000.0    vs 10000.0   100x
 *     qwen3.attention.layer_norm_rms_epsilon  9.9999999e-07 vs 1e-05     10x
 *
 * and therefore `--weights` would not write a file that infer.c was going to
 * run with the constants it had been COMPILED with, unless
 * --accept-arch-mismatch was passed by name.
 *
 * THE ARGUMENT FOR REFUSING STILL STANDS AND IS WORTH RE-READING. The eps is
 * the smaller of the two and its size is derivable: rmsnorm divides by
 * sqrt(mean(x^2) + eps), so using e2 where the model wants e1 scales every
 * normalised activation by sqrt((M+e1)/(M+e2)) ~= 1 - (e2-e1)/(2M) with
 * M = mean(x^2) -- 4.5e-06 relative at M = 1, growing as 1/M, and NOTHING HERE
 * HAS MEASURED M. THE ROPE BASE IS THE ONE THAT CANNOT BE ARGUED AWAY. A 100x
 * wrong base is a different position encoding: the model runs, every logit is
 * finite, the tok/s is unchanged, and the text is fluent and wrong. There is
 * no output you can look at that says so.
 *
 * WHAT CHANGED IS THE FORMAT, NOT THE ARGUMENT. c/lib/nn/model.h now carries
 * both as f32 bit patterns in two of the three `reserved` words, with zero
 * meaning "use the old default" -- a compatible extension, the same shape
 * LogitFS used for atime/mtime, and safe as a sentinel here for a reason
 * model.h states: unlike LogitFS's mode 0, neither a rope base of 0 nor an
 * eps of 0 is a legal value, so there is nothing a writer could mean by it.
 * lm_open refuses a negative or non-finite one (-4) instead of letting it
 * become a NaN 28 layers downstream.
 *
 * So --weights now STORES them and refuses nothing, --accept-arch-mismatch is
 * a no-op kept so an existing build rule does not start failing, and the gate
 * that used to watch lmshape's COPY of the two constants is replaced by
 * test-lm-ropebase, which checks that the stored number reaches the
 * arithmetic. That is the property the copy was only ever standing in for.
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
#include "gguf.h"

/* The two architecture constants are NOT copied here any more. They used to
 * be: infer.c kept them as file-local #defines in a .c, exporting them was a
 * c/lib/nn edit this file was not entitled to make, and `make
 * test-gguf-arch-drift` grepped both files to catch the copy drifting. All of
 * that is gone because the constants themselves moved -- LM_ROPE_BASE_DFL and
 * LM_RMS_EPS_DFL are in model.h, which this file already includes, so there is
 * one definition and nothing to keep in step. The gate that watched the copy
 * was replaced rather than deleted: test-lm-ropebase checks the property the
 * copy only stood in for, which is that the number in the header reaches the
 * arithmetic.
 */

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
    /* --weights only. `gname` is the GGUF name this descriptor claims and is
     * filled even when the lookup FAILS, so map_report can print the name that
     * was not found rather than "a tensor". `g` is NULL until bound. */
    char        gname[64];
    const struct gguf_tensor *g;
    int         transpose; /* --neg-transpose, WRITE SIDE ONLY. See elem(). */
};

struct spec {
    uint32_t dim, n_layers, n_heads, n_kv_heads, head_dim, hidden, vocab, seq_len;
    uint32_t dtype, flags;
    uint64_t seed;
    int      unit_init;    /* --unit-init: the negative control, see elem() */
    /* --weights. NULL means the PRNG, which is every path that existed
     * before. Nothing below branches on it except elem() and build_list's
     * binding. */
    const struct gguf *gg;
    int      neg_transpose;      /* --neg-transpose */
    float    rope_base, rms_eps; /* what the GGUF says; not storable, see header */
};

/* Fill `out` with the descriptors, in payload order, and return how many.
 * With `out` NULL it only counts, so the caller can size the array from the
 * same code that fills it -- infer.c's layout() trick, and for the same
 * reason: two lists that must agree are one list called twice. */
/* THE TENSOR NAME MAP. Every LOGITLM tensor names exactly one GGUF tensor,
 * here and nowhere else, and `gname` is filled unconditionally so a lookup
 * that fails still knows what it was looking for. `%u` is the layer index; a
 * name with no `%u` is a whole-model tensor and `l` is ignored.
 *
 * The map is a FORMAT STRING PER DESCRIPTOR rather than a table keyed on the
 * LOGITLM name because two descriptors with the same LOGITLM name ("wq", once
 * per layer) must resolve to different GGUF tensors -- a name-keyed table
 * would need the layer as a second key, which is what the format string
 * already is. */
static void bindname(struct tdesc *d, const struct spec *sp,
                     const char *fmt, uint32_t l)
{
    snprintf(d->gname, sizeof d->gname, fmt, l);
    d->g = sp->gg ? gguf_find(sp->gg, d->gname) : NULL;
}

static int build_list(const struct spec *sp, struct tdesc *out)
{
    uint32_t hd = sp->head_dim ? sp->head_dim : sp->dim / sp->n_heads;
    uint32_t q_dim  = sp->n_heads    * hd;
    uint32_t kv_dim = sp->n_kv_heads * hd;
    uint32_t edt = (sp->flags & LM_QEMB) ? sp->dtype : (uint32_t)NN_F32;
    int n = 0;
    uint32_t l = 0;

#define PUT(nm, r, c, d, k, gg) do { \
        if (out) { memset(&out[n], 0, sizeof out[n]); \
                   out[n].name = (nm); out[n].rows = (r); out[n].cols = (c); \
                   out[n].dt = (d); out[n].kind = (k); \
                   bindname(&out[n], sp, (gg), l); } \
        n++; } while (0)

    PUT("tok_emb", sp->vocab, sp->dim, edt, TT_WEIGHT, "token_embd.weight");
    for (l = 0; l < sp->n_layers; l++) {
        PUT("att_norm", 1, sp->dim,   NN_F32,    TT_GAIN,   "blk.%u.attn_norm.weight");
        PUT("wq",  q_dim,     sp->dim, sp->dtype, TT_WEIGHT, "blk.%u.attn_q.weight");
        PUT("wk",  kv_dim,    sp->dim, sp->dtype, TT_WEIGHT, "blk.%u.attn_k.weight");
        PUT("wv",  kv_dim,    sp->dim, sp->dtype, TT_WEIGHT, "blk.%u.attn_v.weight");
        if (sp->flags & LM_QKNORM) {
            PUT("q_norm", 1, hd, NN_F32, TT_GAIN, "blk.%u.attn_q_norm.weight");
            PUT("k_norm", 1, hd, NN_F32, TT_GAIN, "blk.%u.attn_k_norm.weight");
        }
        PUT("wo",  sp->dim,    q_dim,   sp->dtype, TT_WEIGHT, "blk.%u.attn_output.weight");
        PUT("ffn_norm", 1, sp->dim, NN_F32, TT_GAIN,          "blk.%u.ffn_norm.weight");
        PUT("w1",  sp->hidden, sp->dim,    sp->dtype, TT_WEIGHT, "blk.%u.ffn_gate.weight");
        PUT("w3",  sp->hidden, sp->dim,    sp->dtype, TT_WEIGHT, "blk.%u.ffn_up.weight");
        PUT("w2",  sp->dim,    sp->hidden, sp->dtype, TT_WEIGHT, "blk.%u.ffn_down.weight");
    }
    l = 0;
    PUT("final_norm", 1, sp->dim, NN_F32, TT_GAIN, "output_norm.weight");
    if (!(sp->flags & LM_TIED))
        PUT("wcls", sp->vocab, sp->dim, sp->dtype, TT_WEIGHT, "output.weight");
#undef PUT
    return n;
}

/* Print the whole map and REFUSE anything about it that is not exactly right.
 * Three separate refusals, because they are three different bugs:
 *
 *   (a) a LOGITLM tensor with no GGUF tensor  -> a hole in the model
 *   (b) a shape disagreement                  -> the right name, wrong tensor,
 *       or the right tensor read the wrong way round
 *   (c) a GGUF tensor NOTHING claimed         -> the file carries something
 *       this converter does not know about, and what it is matters: an extra
 *       `output.weight` means the model is NOT tied and the file we are about
 *       to write is missing its classifier.
 *
 * (c) is the one a converter normally omits and it is the one that fails
 * quietly. The other two produce a program that stops; this one produces a
 * model that runs.
 *
 * `quiet` prints only the summary line and the failures -- 310 rows is the
 * right output for a person checking the map by eye once, and the wrong
 * output for a gate that runs it four times. */
static int map_report(const struct spec *sp, const struct tdesc *td, int nt, int quiet)
{
    const struct gguf *g = sp->gg;
    unsigned char *claimed = (unsigned char *)calloc((size_t)g->nt, 1);
    /* `shown` is reported rather than discarded: a map that printed nothing
     * and a map with nothing in it look identical on a terminal. */
    int bad = 0, shown = 0;

    if (!quiet)
        printf("  %-12s %-28s %-16s %-6s %s\n",
               "LOGITLM", "GGUF name", "shape [rows,cols]", "type", "");
    for (int i = 0; i < nt; i++) {
        const struct tdesc *d = &td[i];
        if (!d->g) {
            printf("  REFUSED: %s wants GGUF tensor `%s` and the file has no "
                   "such tensor.\n", d->name, d->gname);
            bad++;
            continue;
        }
        claimed[d->g - g->t] = 1;
        /* SHAPE. A gain is 1-D in GGUF ([hd] or [dim]) and 2-D here with
         * rows == 1, so the two are compared as (rows, cols) against
         * (ndim==1 ? 1 : dim[1], dim[0]) -- dim[0] is GGUF's CONTIGUOUS
         * dimension, which is our `cols`. That single line is the whole
         * transposition decision; it is argued at length in the file header
         * and proved by --matvec. */
        uint64_t grows = (d->g->ndim == 1) ? 1 : d->g->dim[1];
        uint64_t gcols = d->g->dim[0];
        int extra = 0;
        for (int k = 2; k < d->g->ndim; k++) if (d->g->dim[k] != 1) extra = 1;
        if (grows != d->rows || gcols != d->cols || extra) {
            printf("  REFUSED: %s is [%u,%u] and `%s` is [%llu,%llu] "
                   "(GGUF dims %llu%s%llu). These are not the same tensor, or "
                   "it is stored the other way round.\n",
                   d->name, d->rows, d->cols, d->gname,
                   (unsigned long long)grows, (unsigned long long)gcols,
                   (unsigned long long)d->g->dim[0], d->g->ndim > 1 ? ", " : "",
                   (unsigned long long)(d->g->ndim > 1 ? d->g->dim[1] : 0));
            bad++;
            continue;
        }
        /* tok_emb + layer 0 + the two whole-model tensors are printed in full;
         * the other 27 layers repeat the same eleven names and shapes and are
         * counted rather than scrolled. A map is checked by eye ONCE, and 310
         * identical-looking rows is how a reader stops checking. */
        if (!quiet && (i < 12 || i >= nt - 2)) {
            printf("  %-12s %-28s [%6u,%6u]   %-6s\n", d->name, d->gname,
                   d->rows, d->cols, gguf_type_name(d->g->type));
            shown++;
            if (i == 11 && nt > 14)
                printf("  %-12s %-28s (blk.1 .. blk.%u, same eleven names and "
                       "shapes)\n", "...", "...", sp->n_layers - 1);
        }
    }
    for (uint64_t k = 0; k < g->nt; k++) {
        if (claimed[k]) continue;
        printf("  REFUSED: the GGUF carries `%s` [%llu%s%llu] and NOTHING in "
               "the LOGITLM layout claims it. A skipped tensor is a model with "
               "a hole in it that every structural check passes over.\n",
               g->t[k].name, (unsigned long long)g->t[k].dim[0],
               g->t[k].ndim > 1 ? "," : "",
               (unsigned long long)(g->t[k].ndim > 1 ? g->t[k].dim[1] : 0));
        bad++;
    }
    free(claimed);
    printf("  name map        %d LOGITLM tensors <- %llu GGUF tensors, "
           "%d row(s) shown, %d refusal(s)\n",
           nt, (unsigned long long)g->nt, shown, bad);
    return bad;
}

/* One element's f32 value. This IS the model: everything else in the file is
 * layout. `t` is the descriptor index, which is why the list above must be
 * built the same way by the writer and the verifier -- and it is, because
 * both call build_list. */
static float elem(const struct tdesc *td, const struct spec *sp, uint32_t t,
                  uint64_t idx)
{
    /* ------------------------------------------------------- THE SEAM ----
     * Real weights enter here and only here. `idx` is the flat row-major
     * index into [rows, cols], and because GGUF's dim[0] IS our `cols` (the
     * contiguous dimension), it is the same flat index on both sides -- so
     * this is a copy, not a transposition. map_report has already refused any
     * descriptor whose shape disagrees, which is what makes that sentence
     * checked rather than asserted.
     *
     * `transpose` is --neg-transpose, and it is set on the WRITE side only
     * (write_model sets it after build_list; verify never does). That
     * asymmetry is the control's whole design: the verifier is the
     * instrument, and an instrument that shares the bug cannot see it. It is
     * the same argument this file's verify() already makes about asking
     * model.c where to put things. */
    if (td->g) {
        if (td->transpose) {
            uint64_t r = idx / td->cols, c = idx % td->cols;
            return gguf_get(td->g, c * (uint64_t)td->rows + r);
        }
        return gguf_get(td->g, idx);
    }
    uint64_t seed = sp->seed;
    int unit_init = sp->unit_init;
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

    /* THE TWO ARCHITECTURE CONSTANTS. Zero here means "the file does not say"
     * and the runtime falls back to LM_ROPE_BASE_DFL / LM_RMS_EPS_DFL -- which
     * is what every synthetic --preset writes, so a PRNG model is byte-for-byte
     * what it was before these fields existed. The GGUF path fills them from
     * the file's own metadata, so a real model carries its own position
     * encoding rather than silently inheriting llama-2's. */
    if (sp->rope_base != 0.0f) memcpy(&h->rope_base_f32, &sp->rope_base, 4);
    if (sp->rms_eps   != 0.0f) memcpy(&h->rms_eps_f32,   &sp->rms_eps,   4);
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
/* --neg-transpose NAME. A file-scope pointer rather than a spec field for one
 * reason: it must NOT reach verify(), and a spec field would be handed to
 * verify() by every caller that already hands it the spec. See elem(). */
static const char *g_neg_transpose = NULL;

/* Set the flag on every descriptor the control names. Shared by write_model
 * and dump_elems and by NOTHING ELSE -- in particular not by verify(), which
 * is the instrument and must not inherit the bug it is looking for.
 *
 * dump_elems needs it for a reason worth recording, because the first version
 * did not have it and the control silently passed: the dump used to read
 * gguf_get() directly instead of going through elem(), so it reported what the
 * GGUF SAYS rather than what the converter would WRITE, and a transposition
 * that lived entirely inside elem() was invisible to it. A spot check that
 * does not go through the code under test is a spot check of the oracle. */
static int apply_neg_transpose(struct tdesc *td, int nt)
{
    int hit = 0;
    if (!g_neg_transpose) return 0;
    for (int i = 0; i < nt; i++)
        if (!strcmp(td[i].name, g_neg_transpose)) { td[i].transpose = 1; hit++; }
    if (!hit)
        fprintf(stderr, "lmshape: --neg-transpose %s names no tensor in the "
                "layout. A control that transposes nothing is not a control.\n",
                g_neg_transpose);
    return hit;
}

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

    if (sp->gg) {
        /* THE MAP IS CHECKED BEFORE A BYTE IS WRITTEN, not as it goes. A
         * converter that refuses on tensor 200 of 310 leaves a plausible
         * truncated file behind, and the next thing to touch it is lm_open,
         * which reports -5 with no memory of why. */
        if (map_report(sp, td, nt, quiet)) {
            fprintf(stderr, "lmshape: the name map does not close. Nothing written.\n");
            free(td); return 1;
        }
        /* --neg-transpose: WRITE SIDE ONLY, see elem(). Applied here rather
         * than in build_list so verify()'s own build_list call cannot inherit
         * it -- the control has to be invisible to the instrument. */
        if (g_neg_transpose) {
            int hit = apply_neg_transpose(td, nt);
            if (!hit) { free(td); return 1; }
            printf("  NEGATIVE CONTROL  %s transposed in %d descriptor(s) on the\n"
                   "                    WRITE side only. The header, the byte total\n"
                   "                    and lm_open are unaffected by construction.\n",
                   g_neg_transpose, hit);
        }
    }

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
                row[c] = elem(t, sp, (uint32_t)i, base + c);
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

/* THE HALF-STEP BOUND IS NOT EXACTLY HALF A STEP, AND REAL WEIGHTS ARE WHAT
 * FOUND THAT. Every quantised arm below used `tol = 0.5 * stored_scale`, which
 * is the right statement about the IDEAL grid and one that the f32 grid misses
 * by a few ulps. Measured, the first time this file was pointed at real
 * weights (Qwen3-0.6B Q8_0 -> q4, token 0 column 400):
 *
 *     err   0.0040679946541786194
 *     bound 0.0040679932571947575        excess 1.397e-09, i.e. 3.4e-07 relative
 *
 * ONE element of 2048, and the reason it had never appeared is the reason it
 * was always going to appear here: the PRNG path draws from a continuum, where
 * landing EXACTLY on the midpoint between two quantisation levels has
 * probability zero. A weight out of a Q8_0 file is `f16_scale * int8` -- a
 * dyadic rational -- and a block's levels are built from the same kind of
 * number, so exact ties are ordinary. At a tie the rounding error IS half a
 * step, which is where the difference between "half a step" and "half of the
 * f32 that stores the step" becomes visible.
 *
 * DERIVED, not fitted. Reconstruction is r = fl(min + fl(q*scale)) with
 * u = 2^-24 the f32 unit roundoff, and `scale` is itself fl((hi-lo)/15):
 *
 *     fl(q*scale) = q*scale*(1+d1)             |d1| <= u
 *     r           = (min + q*scale*(1+d1))(1+d2)   |d2| <= u
 *     => |r - (min + q*scale)| <= u*(|q*scale| + |r|),  |q*scale| <= 15*scale
 *     and the true step differs from `scale` by <= u*scale
 *     => |r - w| <= 0.5*scale + u*(16*scale + |r|)
 *
 * The slack is 32u = 1.9e-06 of the bound. A placement error -- a tensor read
 * one row early, which is what this check exists to catch -- moves an element
 * by O(1) relative, so nothing about the check's power changes. Left as
 * 0.5*scale it reports a FAILURE on a correct conversion, which is worse than
 * either: a gate that reddens on the truth teaches people to ignore it. */
#define Q_ULP 5.9604644775390625e-8      /* 2^-24 */
static double qtol(double scale, double got)
{
    return 0.5 * scale + Q_ULP * (16.0 * scale + fabs(got));
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
static int verify(const char *path, const struct spec *sp, int have_seed, int nrand)
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
    /* `sp->gg` is a source of truth exactly as the seed is: with --weights the
     * elements are re-derived from the GGUF instead of from the PRNG, through
     * the SAME elem() the writer used, so this checks the same property (is
     * each tensor where the loader thinks it is) about a real model. What it
     * deliberately does NOT inherit is --neg-transpose; see elem(). */
    if (have_seed || sp->gg) {
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
            /* TWO GROUPS OF PROBES, COUNTED AND BOUNDED SEPARATELY, because
             * they catch different bugs and one of them cannot catch the
             * other's:
             *
             *   [extent] first and last. These pin where a tensor STARTS and
             *            ENDS -- a tensor placed one row early has a plausible
             *            first element and cannot have the right last one --
             *            and they are provably blind to a transposed read (see
             *            the note below the loop).
             *   [random] `nrand` positions per tensor from the same counter
             *            PRNG. These are what see an ORIENTATION bug, and they
             *            are the only thing in the tool that does.
             *
             * Both are compared against the SAME per-element half-step bound,
             * so a random probe is not a weaker check -- it is the same check
             * somewhere a transposition can move it. */
            double worst = 0.0, worst_tol = 0.0;
            const char *worst_name = "";
            double rworst = 0.0, rworst_tol = 0.0;
            const char *rworst_name = "";
            int nrand_bad = 0;
            long nrand_done = 0;
            for (int i = 0; i < nt; i++) {
                uint64_t n = (uint64_t)td[i].rows * td[i].cols;
                int ne = 2 + nrand;
                for (int e = 0; e < ne; e++) {
                    int rnd = (e >= 2);
                    uint64_t ix;
                    if (e == 0)      ix = 0;
                    else if (e == 1) ix = n - 1;
                    else ix = mix64(sp->seed ^ mix64(((uint64_t)i << 20) +
                                                     (uint64_t)(e - 2))) % n;
                    float want = elem(&td[i], sp, (uint32_t)i, ix);
                    float got; double tol;
                    if (gains[i]) { got = gains[i][ix]; tol = 0.0; }
                    else if (tens[i]->dtype == NN_F32) {
                        got = tens[i]->data[ix]; tol = 0.0;
                    } else if (tens[i]->dtype == NN_Q4) {
                        /* The element is reconstructed through q4_dequantize
                         * -- the real one -- over the single row it lives in,
                         * so this checks the LAYOUT (is that element where the
                         * loader says it is) and not the quantiser, which
                         * tests/unit/quant4_test.c already gates against a
                         * double reference. */
                        uint32_t r = (uint32_t)(ix / td[i].cols);
                        uint32_t c = (uint32_t)(ix % td[i].cols);
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
                        tol = qtol((double)tens[i]->scale[(size_t)r * nb +
                                                          (size_t)(c / Q4_BLOCK)],
                                   (double)got);
                        free(rowbuf);
                    } else {
                        uint32_t r = (uint32_t)(ix / td[i].cols);
                        float s = tens[i]->scale[r];
                        got = (float)tens[i]->q[ix] * s;
                        /* DERIVED, not fitted: nn_quantize_q8 rounds to the
                         * nearest multiple of scale = max|row|/127, so the
                         * reconstruction error of any element is at most half
                         * a step -- plus the f32 slack qtol() derives, which a
                         * dyadic weight sitting exactly on a tie needs and a
                         * pseudo-random one never does. Anything above that is
                         * not quantisation. */
                        tol = qtol((double)s, (double)got);
                    }
                    double err = fabs((double)got - (double)want);
                    if (rnd) {
                        nrand_done++;
                        if (err > rworst) { rworst = err; rworst_tol = tol;
                                            rworst_name = td[i].name; }
                    } else {
                        if (err > worst) { worst = err; worst_tol = tol;
                                           worst_name = td[i].name; }
                    }
                    if (err > tol) {
                        /* TAGGED, and the tag is what tests/nn.mk counts. The
                         * two groups fail for different reasons and a control
                         * that reddens one must not be credited with the
                         * other -- the same argument test-lm-infer-negctl's
                         * `[head_dim]` tag makes one screen up in that file. */
                        printf("  FAIL[%s]: %s[%llu] on disk %.9g, from the "
                               "source %.9g (err %.3g > %.3g)\n",
                               rnd ? "random" : "extent", td[i].name,
                               (unsigned long long)ix, (double)got,
                               (double)want, err, tol);
                        bad++;
                        if (rnd) nrand_bad++;
                    }
                }
            }
            printf("  spot check      %d tensors x {first,last} = %d elements, "
                   "worst |disk - src| %.4g against its own half-step bound "
                   "%.4g (%s)\n", nt, nt * 2, worst, worst_tol, worst_name);
            if (nrand)
                printf("  random probe    %d tensors x %d = %ld elements, worst "
                       "|disk - src| %.4g against its own half-step bound %.4g "
                       "(%s), %d over\n", nt, nrand, nrand_done, rworst,
                       rworst_tol, rworst_name, nrand_bad);
            /* FIRST AND LAST CANNOT SEE A TRANSPOSED READ. AT ALL, AT ANY
             * SHAPE. This paragraph said "of a SQUARE transpose" and predicted
             * that a non-square tensor would redden `last`; the control was
             * run and reddened NOTHING for either wk [1024,1024] or w1
             * [3072,1024], so the prediction was wrong and the real property
             * is stronger. The arithmetic, which is why:
             *
             *   idx = 0            -> (r,c) = (0,0)
             *                      -> c*rows + r = 0                       = idx
             *   idx = rows*cols-1  -> (r,c) = (rows-1, cols-1)
             *                      -> (cols-1)*rows + (rows-1)
             *                       = rows*cols - rows + rows - 1
             *                       = rows*cols - 1                        = idx
             *
             * Both endpoints are FIXED POINTS of (r,c)->(c,r) for every rows
             * and cols, because they are the two corners the transposition
             * maps to themselves. So this loop is structurally blind to an
             * orientation bug, and the RANDOM spot check (--dump-elems,
             * cross-checked by tools/gguf_check.py) is not a nicety beside it
             * -- it is the only thing in the tool that can see one.
             *
             * That is not a reason to change the element choice here. First
             * and last are the right probes for what this loop is FOR, which
             * is EXTENT: a tensor placed one row early has a plausible first
             * element and cannot have the right last one. Extent and
             * orientation are different bugs and they get different probes.
             * Measured in tests/nn.mk's test-gguf-negctl, which asserts the
             * zero rather than printing it. */

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
            double eworst = 0.0, ebound = 0.0, eratio = 0.0;
            char ewho[160]; ewho[0] = 0;
            int ebad = 0;
            for (int e = 0; e < 2 && erow; e++) {
                if (lm_embed_row(&m, etok[e], erow) != 0) {
                    printf("  FAIL: lm_embed_row refused token %d (dtype %d)\n",
                           etok[e], m.emb.dtype);
                    ebad++; bad++; continue;
                }
                for (uint32_t c = 0; c < m.h.dim; c++) {
                    uint64_t idx = (uint64_t)etok[e] * m.h.dim + c;
                    double want = elem(et, sp, 0, idx);
                    double err = fabs((double)erow[c] - want);
                    double tol = 0.0;
                    if (m.emb.dtype == NN_Q8)
                        tol = qtol((double)m.emb.scale[etok[e]], (double)erow[c]);
                    else if (m.emb.dtype == NN_Q4) {
                        size_t nbe = (size_t)q4_blocks((int)m.h.dim, Q4_BLOCK);
                        tol = qtol((double)m.emb.scale[(size_t)etok[e] * nbe
                                                       + c / Q4_BLOCK],
                                   (double)erow[c]);
                    }
                    if (err > eworst) { eworst = err; ebound = tol; }
                    /* THE WORST ERROR IS NOT THE WORST VIOLATION, and reporting
                     * only the first hid a real question for one session: the
                     * count said 1 of 2048 while the printed worst error was
                     * INSIDE the printed bound, because the two lines were
                     * about different elements. Each element has its own bound
                     * (its block's scale), so the statistic that ranks
                     * violations is err/tol, and the offender is named. */
                    if (err > tol) {
                        double ratio = tol > 0 ? err / tol : (err > 0 ? 1e300 : 0);
                        if (ratio > eratio) {
                            eratio = ratio;
                            snprintf(ewho, sizeof ewho,
                                     "token %d col %u: disk %.9g src %.9g "
                                     "err %.17g bound %.17g", etok[e], c,
                                     (double)erow[c], want, err, tol);
                        }
                        ebad++;
                    }
                }
            }
            if (ebad) {
                printf("  FAIL: lm_embed_row disagrees with the source on %d of "
                       "%u elements (worst violation err/bound %.6g -- %s)\n",
                       ebad, 2u * m.h.dim, eratio, ewho);
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
        printf("  spot check      SKIPPED -- pass --seed N (and the shape), or "
               "--weights FILE.gguf, to re-derive the weights\n");
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

/* ================================================================ --weights ==
 *
 * The shape comes off the FILE, not off a --preset. That is not tidiness: a
 * preset is a fourth statement of numbers that already exist in three places
 * (the GGUF metadata, config.json, and the tensor shapes), and the failure
 * mode of a preset that drifts from the weights it is used with is a model
 * with the right byte count and the wrong geometry. Every field below is read
 * and then CROSS-CHECKED against a tensor shape, so the metadata and the
 * payload have to agree before anything is written.
 */
static int arch_from_gguf(struct spec *sp, const struct gguf *g,
                          int accept_mismatch, int have_dtype, int have_seq)
{
    const char *arch; uint64_t alen;
    if (gguf_str(g, "general.architecture", &arch, &alen)) return 1;
    if (alen != 5 || memcmp(arch, "qwen3", 5) != 0) {
        fprintf(stderr, "lmshape: general.architecture is `%.*s`, and this "
                "converter only knows qwen3.\n"
                "  REFUSED rather than attempted: the LOGITLM payload order is "
                "qwen3's -- QK-norm between wv and wo, SwiGLU, tied head -- and "
                "another architecture that happens to have the same tensor "
                "NAMES would be laid out wrong with every shape check passing.\n",
                (int)alen, arch);
        return 1;
    }

    uint32_t v;
    struct { const char *key; uint32_t *dst; } m[] = {
        { "qwen3.embedding_length",          &sp->dim },
        { "qwen3.block_count",               &sp->n_layers },
        { "qwen3.attention.head_count",      &sp->n_heads },
        { "qwen3.attention.head_count_kv",   &sp->n_kv_heads },
        { "qwen3.attention.key_length",      &sp->head_dim },
        { "qwen3.feed_forward_length",       &sp->hidden },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        if (gguf_u32(g, m[i].key, &v)) return 1;
        *m[i].dst = v;
    }
    /* key_length and value_length are separate fields in GGUF and ONE field in
     * struct lm_header. Checked rather than assumed: a model with different k
     * and v head widths is not expressible in this format at all, and picking
     * key_length silently would produce a file whose v projection is the wrong
     * size with no complaint from anything. */
    if (gguf_u32(g, "qwen3.attention.value_length", &v)) return 1;
    if (v != sp->head_dim) {
        fprintf(stderr, "lmshape: key_length %u != value_length %u. struct "
                "lm_header has ONE head_dim, so this model is not expressible "
                "in LOGITLM. REFUSED.\n", sp->head_dim, v);
        return 1;
    }

    const struct gguf_tensor *emb = gguf_find(g, "token_embd.weight");
    if (!emb || emb->ndim != 2) {
        fprintf(stderr, "lmshape: no 2-D token_embd.weight\n"); return 1;
    }
    sp->vocab = (uint32_t)emb->dim[1];
    if (emb->dim[0] != sp->dim) {
        fprintf(stderr, "lmshape: embedding_length says %u, token_embd's "
                "contiguous dimension is %llu. The metadata and the payload "
                "disagree.\n", sp->dim, (unsigned long long)emb->dim[0]);
        return 1;
    }
    /* The tokenizer's token list is a THIRD statement of the vocabulary size
     * and it is free to check. A mismatch here is a repacked file. */
    uint64_t ntok = 0;
    if (gguf_alen(g, "tokenizer.ggml.tokens", &ntok) == 0 && ntok != sp->vocab) {
        fprintf(stderr, "lmshape: token_embd has %u rows and "
                "tokenizer.ggml.tokens has %llu entries.\n",
                sp->vocab, (unsigned long long)ntok);
        return 1;
    }

    /* FLAGS ARE DERIVED FROM WHAT IS PRESENT, not asserted.
     *   LM_TIED   <- there is no `output.weight`. That is exactly what tying
     *                means on disk, and it is the one flag whose wrong value
     *                changes the file LENGTH, so lm_expected_size catches a
     *                mistake here immediately.
     *   LM_QKNORM <- blk.0.attn_q_norm.weight exists.
     *   LM_QEMB   <- the source embedding is quantised. Storing a dequantised
     *                Q8_0 table as f32 would be 594 MiB holding q8's
     *                information; --no-qemb is there for a byte-level model,
     *                where model.h's original argument still holds. */
    sp->flags = 0;
    if (!gguf_find(g, "output.weight")) sp->flags |= LM_TIED;
    if (gguf_find(g, "blk.0.attn_q_norm.weight")) sp->flags |= LM_QKNORM;
    if (emb->type != GGML_F32) sp->flags |= LM_QEMB;

    /* LM_ROPE_NEOX <- THE ARCHITECTURE, AND THERE IS NOTHING ELSE TO READ.
     *
     * The GGUF carries no rope-type key -- `qwen3.rope.freq_base` is the only
     * rope entry in the file's 28 kv pairs, checked by dumping all of them.
     * llama.cpp does not store it either: it hardcodes a rope type per
     * architecture (qwen3 is NEOX), so the pairing is a fact about the NAME
     * and this is a lookup, not a parse.
     *
     * IT IS SET HERE AND NOT DEFAULTED, and arch_from_gguf above REFUSES any
     * architecture but qwen3, which is what makes a one-entry table safe: a
     * second architecture cannot reach this line without somebody editing
     * that refusal, and whoever edits it has to answer this question too. The
     * alternative -- default to interleaved and let a new architecture fall
     * through -- writes a file that runs, is fluent, and is wrong, which is
     * the failure this whole converter is arranged to prevent.
     *
     * MEASURED (build/qwen3_f32.lm, layer 0, position 1, against
     * transformers' own apply_rotary_pos_emb on identical inputs): NEOX
     * max|d| 6.7e-07 over a q scale of 28.79, interleaved 11.31. */
    sp->flags |= LM_ROPE_NEOX;

    if (!have_seq) {
        /* seq_len is THE ONE NUMBER THAT IS A CHOICE, and the file's own
         * answer is unusable: qwen3.context_length is 40960, and the KV cache
         * at that length is n_layers * seq * kv_dim * 2 * 4 bytes = 28 * 40960
         * * 1024 * 8 = 9.4 GB on a machine with 512 MiB. 512 is the preset's
         * figure (112 MiB) and is carried here for comparability. */
        uint32_t ctx = 0;
        gguf_u32(g, "qwen3.context_length", &ctx);
        sp->seq_len = 512;
        printf("  seq_len         %u chosen (the file says context_length %u, "
               "whose KV cache would be %.1f GB)\n", sp->seq_len, ctx,
               (double)sp->n_layers * ctx * sp->n_kv_heads * sp->head_dim * 2 * 4 / 1e9);
    }
    if (!have_dtype) {
        /* q8 rather than f32, because f32 is 2273.8 MiB of a source that holds
         * 8-bit information. Stated as a default with a reason rather than
         * forced, because q4 (355.5 MiB) is what fits the 512 MiB machine and
         * the caller may well want it. */
        sp->dtype = NN_Q8;
        printf("  dtype           q8 by default (the source is Q8_0; f32 would "
               "be 2273.8 MiB holding 8-bit information -- pass --dtype q4 for "
               "355.5 MiB)\n");
    }

    /* ------------------------------------------ THE TWO ARCHITECTURE
     * CONSTANTS. THIS WAS A REFUSAL, and the refusal was right at the time:
     * struct lm_header had no field for either, so this file would have been
     * run by infer.c with the constants it was COMPILED with -- rope base
     * 10000 against this model's 1000000. That is a different position
     * encoding, and it produces fluent, confident, wrong text with every
     * logit finite, the tok/s unchanged, and nothing in the output that says
     * so. Refusing was the only honest thing the writer could do.
     *
     * c/lib/nn carries both now, as f32 bit patterns in two of the three
     * `reserved` words, zero meaning "use the old default" (model.h argues
     * why zero is a safe sentinel for these two and was NOT for LogitFS's
     * mode). So the constants are STORED rather than dropped, and this is the
     * one line of this whole converter that decides whether the model speaks
     * Qwen or speaks noise.
     *
     * --accept-arch-mismatch is kept and is now a NO-OP: it was the escape
     * hatch for measuring throughput on a file that could not carry them, and
     * a build rule that still passes it should not start failing. */
    sp->rope_base = 0.0f; sp->rms_eps = 0.0f;
    if (gguf_f32(g, "qwen3.rope.freq_base", &sp->rope_base)) return 1;
    if (gguf_f32(g, "qwen3.attention.layer_norm_rms_epsilon", &sp->rms_eps)) return 1;
    /* Written as !(x > 0) rather than (x <= 0) so a NaN is refused too -- every
     * comparison with NaN is false, so the negated form catches it and the
     * direct form waves it through into NaN logits 28 layers away. */
    if (!(sp->rope_base > 0.0f) || !(sp->rms_eps > 0.0f)) {
        fprintf(stderr, "lmshape: REFUSED -- the GGUF's rope_base (%g) or rms_eps "
                "(%g) is not a positive finite number.\n",
                (double)sp->rope_base, (double)sp->rms_eps);
        return 1;
    }
    int drift = (sp->rope_base != LM_ROPE_BASE_DFL) ||
                (sp->rms_eps   != LM_RMS_EPS_DFL);
    printf("  rope_base       GGUF %.1f   format default %.1f   %s\n",
           (double)sp->rope_base, (double)LM_ROPE_BASE_DFL,
           drift ? "*** STORED IN THE HEADER ***" : "same as the default");
    printf("  rms_eps         GGUF %.3g   format default %.3g   %s\n",
           (double)sp->rms_eps, (double)LM_RMS_EPS_DFL,
           drift ? "*** STORED IN THE HEADER ***" : "same as the default");
    if (drift && accept_mismatch)
        printf("  --accept-arch-mismatch  passed, and is a NO-OP now: the header\n"
               "                  carries both constants.\n");
    return 0;
}

/* ------------------------------------------------- the random spot check --
 *
 * --dump-elems prints `ggufname,row,col,value` for N elements spread over
 * every mapped tensor, straight out of the same gguf_get() the writer used.
 * tools/gguf_check.py dequantises the same elements with numpy and demands
 * EXACT equality.
 *
 * RANDOM AND NOT FIRST/LAST, because --verify already does first/last and
 * cannot see a square transposition (both are fixed points -- see the note in
 * verify()). This is the check the negative control reddens.
 *
 * The positions come from the same mix64 the PRNG path uses, so they are
 * reproducible from --seed without a list having to be carried anywhere. */
static int dump_elems(const struct spec *sp, int nsamp, const char *path)
{
    int nt = build_list(sp, NULL);
    struct tdesc *td = (struct tdesc *)calloc((size_t)nt, sizeof *td);
    if (!td) return 1;
    build_list(sp, td);
    if (map_report(sp, td, nt, 1)) { free(td); return 1; }
    if (g_neg_transpose && !apply_neg_transpose(td, nt)) { free(td); return 1; }

    FILE *f = path ? fopen(path, "w") : stdout;
    if (!f) { perror(path); free(td); return 1; }
    fprintf(f, "# name,row,col,value -- from tools/lmshape.c --dump-elems, "
               "seed %llu\n", (unsigned long long)sp->seed);
    int per = nsamp / nt; if (per < 1) per = 1;
    long n = 0;
    for (int i = 0; i < nt; i++) {
        for (int k = 0; k < per; k++) {
            uint64_t h = mix64(sp->seed ^ mix64(((uint64_t)i << 20) + (uint64_t)k));
            uint64_t idx = h % ((uint64_t)td[i].rows * td[i].cols);
            /* %.17g, not %.9g. Nine digits round-trip a FLOAT among floats,
             * but the oracle parses to a double and compares a promoted
             * float32 -- so the decimal has to identify the DOUBLE exactly, or
             * an exact check silently becomes a one-ulp tolerance, which is
             * where a constant factor of 1.0001 would hide. */
            /* THROUGH elem(), NOT gguf_get. The row and column written are the
             * LOGITLM position, and the value is what the WRITER would put
             * there -- so the oracle looks the same position up in the GGUF and
             * the two agree only if the converter read it the right way round.
             * Reading gguf_get here would report what the file says at that
             * position, which agrees with the oracle no matter what the
             * converter does; see apply_neg_transpose. */
            fprintf(f, "%s,%llu,%llu,%.17g\n", td[i].gname,
                    (unsigned long long)(idx / td[i].cols),
                    (unsigned long long)(idx % td[i].cols),
                    (double)elem(&td[i], sp, (uint32_t)i, idx));
            n++;
        }
    }
    if (path) fclose(f);
    /* stdout when the CSV went to a file, stderr only when the CSV IS stdout.
     * Unconditional stderr put this line through the middle of another
     * printf's output in build/gate.log -- the two streams are separately
     * buffered, so an interleave is not even deterministic. */
    fprintf(path ? stdout : stderr,
            "  element dump    %ld elements over %d tensors -> %s\n",
            n, nt, path ? path : "stdout");
    free(td);
    return 0;
}

/* ------------------------------------------------- the orientation proof --
 *
 * Dequantise one GGUF tensor, run the REAL kernel over it, write [n][k][x][y].
 * tools/gguf_check.py recomputes y from its own load of the same tensor in
 * float64 and reports max|dy|.
 *
 * IT USES nn_matvec_f32 RATHER THAN A LOOP WRITTEN HERE, because the question
 * is not "is my arithmetic right" -- it is "does the runtime's kernel, walking
 * this buffer as [n,k] with k contiguous, compute the product the weights
 * mean". A loop written in this function would answer a question about the
 * loop. */
static int matvec_proof(const struct gguf *g, const char *tname,
                        const char *xypath, uint64_t seed, int transpose)
{
    const struct gguf_tensor *t = gguf_find(g, tname);
    if (!t) { fprintf(stderr, "lmshape: no GGUF tensor `%s`\n", tname); return 1; }
    if (t->ndim != 2) { fprintf(stderr, "lmshape: `%s` is not 2-D\n", tname); return 1; }

    /* OUR reading: n rows of k contiguous weights, k = GGUF's dim[0]. */
    int n = (int)t->dim[1], k = (int)t->dim[0];
    if (transpose) { int s = n; n = k; k = s; }

    float *w = (float *)malloc((size_t)n * k * sizeof(float));
    float *x = (float *)malloc((size_t)k * sizeof(float));
    float *y = (float *)malloc((size_t)n * sizeof(float));
    if (!w || !x || !y) { fprintf(stderr, "lmshape: out of memory\n"); return 1; }

    for (uint64_t i = 0; i < (uint64_t)n * k; i++) {
        uint64_t src = i;
        if (transpose) { uint64_t r = i / k, c = i % k; src = c * (uint64_t)n + r; }
        w[i] = gguf_get(t, src);
    }
    /* x is U(-1,1) from the same counter-based PRNG, so it is reproducible --
     * but it is WRITTEN OUT rather than reproduced by the oracle. A second
     * definition of the PRNG in Python would be a second thing that can be
     * wrong, and it would fail this check while saying nothing about the
     * weights. */
    for (int i = 0; i < k; i++) x[i] = (float)(2.0 * u01(seed, 0xABCDu, (uint64_t)i) - 1.0);

    nn_matvec_f32(y, w, x, n, k);

    FILE *f = fopen(xypath, "wb");
    if (!f) { perror(xypath); return 1; }
    int32_t hdr[2] = { n, k };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(x, sizeof(float), (size_t)k, f);
    fwrite(y, sizeof(float), (size_t)n, f);
    if (fclose(f) != 0) { perror("close"); return 1; }

    double s = 0.0, mx = 0.0;
    for (int i = 0; i < n; i++) { s += y[i]; if (fabs(y[i]) > mx) mx = fabs(y[i]); }
    printf("  matvec          %s as [n=%d, k=%d]%s\n", tname, n, k,
           transpose ? "  (TRANSPOSED -- negative control)" : "");
    printf("                  nn_matvec_f32: sum(y) %.9g, max|y| %.6g -> %s\n",
           s, mx, xypath);
    free(w); free(x); free(y);
    return 0;
}

/* ---------------------------------------------------------------- main -- */

static void usage(void)
{
    fprintf(stderr,
"lmshape -- write a LOGITLM file of any shape, deterministic from a seed.\n"
"\n"
"  --out FILE          write the model\n"
"  --verify FILE       open an existing model and check it (add --seed and\n"
"                      the shape, or --weights, to re-derive its weights)\n"
"  --probes N          make --verify check N RANDOM elements per tensor as\n"
"                      well as the first and last. First and last pin a\n"
"                      tensor's EXTENT and are fixed points of a transposed\n"
"                      read at EVERY shape, so only these see an orientation\n"
"                      bug. Default 0, i.e. --verify is unchanged without it.\n"
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
"  --qemb              store the embedding at --dtype instead of f32\n"
"\n"
"REAL WEIGHTS (the seam is elem(); everything else is unchanged):\n"
"  --weights F.gguf    take every weight from a GGUF instead of the PRNG. The\n"
"                      SHAPE comes off the file too, so --preset/--dim/... are\n"
"                      ignored. Works with --out, --verify and --dump-elems.\n"
"  --accept-arch-mismatch\n"
"                      NO-OP, kept so an existing build rule does not start\n"
"                      failing. It used to force a write when the GGUF's rope\n"
"                      base / rms eps differed from the ones infer.c had been\n"
"                      COMPILED with, because the header could not carry them\n"
"                      and the default was to REFUSE. The header carries both\n"
"                      now, so there is nothing left to override.\n"
"  --no-qemb           keep the embedding f32 even from a quantised source\n"
"  --dump-elems FILE   write name,row,col,value for --samples random elements,\n"
"                      for tools/gguf_check.py --dequant to check\n"
"  --samples N         how many (default 512)\n"
"  --matvec NAME --xy FILE\n"
"                      dequantise one GGUF tensor, run nn_matvec_f32 over it,\n"
"                      write [n][k][x][y] for tools/gguf_check.py --matvec\n"
"  --neg-transpose NM  NEGATIVE CONTROL: read LOGITLM tensor NM (wk, w1, ...)\n"
"                      transposed, on the WRITE side only. Also flips --matvec.\n");
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
    const char *wpath = NULL, *dumpe = NULL, *mvname = NULL, *xypath = NULL;
    int accept_mismatch = 0, have_dtype = 0, have_seq = 0, no_qemb = 0;
    int nsamp = 512, nprobe = 0;
    struct gguf gg;

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
        else if (!strcmp(a, "--seq"))    { sp.seq_len = u32arg(NEXT("--seq"), "--seq"); have_seq = 1; }
        else if (!strcmp(a, "--weights"))  wpath = NEXT("--weights");
        else if (!strcmp(a, "--accept-arch-mismatch")) accept_mismatch = 1;
        else if (!strcmp(a, "--no-qemb"))  no_qemb = 1;
        else if (!strcmp(a, "--dump-elems")) dumpe = NEXT("--dump-elems");
        else if (!strcmp(a, "--samples"))  nsamp = (int)u32arg(NEXT("--samples"), "--samples");
        else if (!strcmp(a, "--probes"))   nprobe = (int)u32arg(NEXT("--probes"), "--probes");
        else if (!strcmp(a, "--matvec"))   mvname = NEXT("--matvec");
        else if (!strcmp(a, "--xy"))       xypath = NEXT("--xy");
        else if (!strcmp(a, "--neg-transpose")) g_neg_transpose = NEXT("--neg-transpose");
        else if (!strcmp(a, "--seed"))     sp.seed = strtoull(NEXT("--seed"), NULL, 10);
        else if (!strcmp(a, "--no-seed"))  have_seed = 0;
        else if (!strcmp(a, "--unit-init")) sp.unit_init = 1;
        else if (!strcmp(a, "--tied"))     sp.flags |= LM_TIED;
        else if (!strcmp(a, "--untied"))   sp.flags &= ~(uint32_t)LM_TIED;
        else if (!strcmp(a, "--qknorm"))   sp.flags |= LM_QKNORM;
        else if (!strcmp(a, "--qemb"))     sp.flags |= LM_QEMB;
        else if (!strcmp(a, "--dtype")) {
            const char *d = NEXT("--dtype");
            have_dtype = 1;
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

    /* --weights REPLACES the shape as well as the values, so it runs before
     * anything reads sp. A --preset alongside it is not an error and not
     * honoured: the file's own metadata wins, and the header_of() below is
     * built from it. */
    if (wpath) {
        if (gguf_open(&gg, wpath)) return 1;
        printf("weights %s\n", wpath);
        printf("  gguf            v%u, %llu tensors, %llu keys, data at %llu\n",
               gg.version, (unsigned long long)gg.nt,
               (unsigned long long)gg.nkv, (unsigned long long)gg.data0);
        if (arch_from_gguf(&sp, &gg, accept_mismatch, have_dtype, have_seq)) {
            gguf_close(&gg); return 1;
        }
        if (no_qemb) sp.flags &= ~(uint32_t)LM_QEMB;
        sp.gg = &gg;
    } else if (g_neg_transpose || dumpe || mvname || accept_mismatch) {
        fprintf(stderr, "lmshape: --neg-transpose/--dump-elems/--matvec/"
                "--accept-arch-mismatch need --weights.\n");
        return 2;
    }

    if (mvname) {
        if (!xypath) { fprintf(stderr, "lmshape: --matvec needs --xy FILE\n"); return 2; }
        int rc = matvec_proof(&gg, mvname, xypath, sp.seed, g_neg_transpose != NULL);
        gguf_close(&gg);
        return rc;
    }
    if (dumpe) {
        int rc = dump_elems(&sp, nsamp, dumpe);
        gguf_close(&gg);
        return rc;
    }
    if (fwd) return forward(fwd, ntok, against);
    if (ver) {
        int rc = verify(ver, &sp, have_seed, nprobe);
        if (wpath) gguf_close(&gg);
        return rc;
    }

    struct lm_header h;
    header_of(&h, &sp);
    size_t want = lm_expected_size(&h);
    uint32_t hd = sp.head_dim ? sp.head_dim : (sp.n_heads ? sp.dim / sp.n_heads : 0);

    printf("shape   dim=%u layers=%u heads=%u kv_heads=%u head_dim=%u "
           "hidden=%u vocab=%u seq=%u\n",
           sp.dim, sp.n_layers, sp.n_heads, sp.n_kv_heads, hd,
           sp.hidden, sp.vocab, sp.seq_len);
    /* The pairing is printed as "rope=neox" / "rope=interleaved" rather than
     * as a flag that is present or absent, because absent is a CHOICE here
     * and not a default -- and it is the choice that produces fluent, wrong
     * text. A reader skimming this line for what went into the file should
     * not have to know that a missing word means interleaved. */
    printf("flags   %s%s%s rope=%s dtype=%s seed=%llu\n",
           (sp.flags & LM_TIED) ? "tied " : "untied ",
           (sp.flags & LM_QKNORM) ? "qknorm " : "",
           (sp.flags & LM_QEMB) ? "qemb " : "",
           (sp.flags & LM_ROPE_NEOX) ? "neox" : "interleaved",
           sp.dtype == NN_Q8 ? "q8" : sp.dtype == NN_Q4 ? "q4" : "f32",
           (unsigned long long)sp.seed);
    if (!want) {
        fprintf(stderr, "lmshape: lm_expected_size refuses this header.\n");
        return 1;
    }
    printf("bytes   %llu  (%.1f MiB)\n", (unsigned long long)want, want / 1048576.0);

    if (dry) { if (wpath) gguf_close(&gg); return 0; }
    if (!out) { usage(); return 2; }
    int rc = write_model(out, &sp, 0);
    if (wpath) gguf_close(&gg);
    return rc;
}
