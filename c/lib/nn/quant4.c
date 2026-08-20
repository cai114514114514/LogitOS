/* quant4.c -- the four-bit weight format. See quant4.h for why every choice
 * below is the choice, with the number that decided it.
 *
 * THE QUANTISER IS ONE CODE PATH WITH THE RANGE AS A PARAMETER, and that is
 * not tidiness -- it is what makes the negative control worth having.
 * `quant_block_range` is handed the (lo,hi) it must span; `block_range`
 * computes it from the block. Under -DQ4_PER_TENSOR_SCALE the SAME function
 * is handed the whole tensor's range instead. So the control is not a second
 * quantiser that might differ in some other way too -- it differs in exactly
 * one input, which is the claim being tested.
 */
#include <string.h>
#include "quant4.h"

int q4_mode_ok(int mode) { return mode == Q4_SYM || mode == Q4_AFFINE; }

/* Even, because a block is packed as (L+1)/2 bytes with the first half of the
 * weights in the low nibbles -- an odd FULL block would put the split at a
 * different place in every block of a row and there is no reason to allow it.
 * (The last block of a row may still be short and odd; that case is real,
 * k=344 with blk=32 produces it, and it is handled rather than forbidden.)
 * The 1024 cap is what bounds `lo4`/`hi4` below to the stack. */
int q4_blk_ok(int blk) { return blk >= 2 && blk <= Q4_BLOCK_MAX && (blk & 1) == 0; }

int q4_blocks(int k, int blk)
{
    if (k <= 0 || !q4_blk_ok(blk)) return 0;
    return (k + blk - 1) / blk;
}

size_t q4_row_bytes(int k, int blk)
{
    if (k <= 0 || !q4_blk_ok(blk)) return 0;
    int nfull = k / blk, rem = k % blk;
    return (size_t)nfull * (size_t)(blk / 2) + (size_t)((rem + 1) / 2);
}

size_t q4_payload_bytes(int n, int k, int blk)
{
    if (n <= 0) return 0;
    return (size_t)n * q4_row_bytes(k, blk);
}

size_t q4_scale_off(int n, int k, int blk)
{
    size_t pay = q4_payload_bytes(n, k, blk);
    if (!pay) return 0;
    if (pay > (size_t)-1 - 3u) return 0;
    return (pay + 3u) & ~(size_t)3u;
}

size_t q4_bytes(int n, int k, int blk, int mode)
{
    if (n <= 0 || !q4_mode_ok(mode)) return 0;
    size_t off = q4_scale_off(n, k, blk);
    if (!off) return 0;
    size_t nbr = (size_t)q4_blocks(k, blk);
    size_t np = (mode == Q4_AFFINE) ? 2u : 1u;
    /* Every number here came off disk in the model-file case, so the products
     * are checked rather than trusted -- a wrapped size allocates a small
     * buffer for a huge tensor and fails somewhere else entirely. Returning 0
     * is unambiguous: no legal [n,k] with n,k >= 1 has a zero size. */
    if (nbr && (size_t)n > (size_t)-1 / nbr) return 0;
    size_t nb = (size_t)n * nbr;
    if (nb && np > (size_t)-1 / nb) return 0;
    size_t meta = nb * np;
    if (meta > (size_t)-1 / sizeof(float)) return 0;
    meta *= sizeof(float);
    if (meta > (size_t)-1 - off) return 0;
    return off + meta;
}

/* ------------------------------------------------------------- quantise -- */

static void block_range(const float *w, int L, float *lo, float *hi)
{
    float a = w[0], b = w[0];
    for (int i = 1; i < L; i++) {
        if (w[i] < a) a = w[i];
        if (w[i] > b) b = w[i];
    }
    *lo = a; *hi = b;
}

/* Quantise ONE block against a range somebody else chose.
 *
 * SYM spends 15 of the 16 codes: q in -7..7 stored biased by 8, so the stored
 * nibble is 1..15 and 0 never appears. Using -8..7 would reach all sixteen but
 * only downwards -- the positive side would still stop at 7*s, so a weight at
 * +amax would clip by a full step and the half-step bound this whole format is
 * gated on would be false. The wasted level is the price of that bound, it is
 * 1/16 of the codes, and quant4.h has the number for what it costs end to end.
 *
 * AFFINE spends all sixteen: q in 0..15 with w ~= lo + q*s.
 *
 * Rounding is half-away-from-zero and then CLAMPED, for the reason matmul.c
 * gives for q8: amax*inv is exactly 7.0 in exact arithmetic and can be
 * 7.0000005 after two f32 roundings, which would become 8 and, biased, 16 --
 * a nibble that does not exist, silently corrupting its neighbour. */
static void quant_block_range(uint8_t *bp, float *sc, float *mn,
                              const float *w, int L, int mode,
                              float lo, float hi)
{
    int h = (L + 1) / 2, nh = L - h, i;
    unsigned char lo4[Q4_BLOCK_MAX / 2 + 1], hi4[Q4_BLOCK_MAX / 2 + 1];
    float s, inv, base;
    int qmin, qmax, bias;

    if (mode == Q4_AFFINE) {
        s = (hi > lo) ? (hi - lo) / 15.0f : 0.0f;
        base = lo; qmin = 0; qmax = 15; bias = 0;
    } else {
        float amax = (lo < 0.0f ? -lo : lo);
        if (hi > amax) amax = hi;
        if (-hi > amax) amax = -hi;
        s = (amax > 0.0f) ? amax / 7.0f : 0.0f;
        base = 0.0f; qmin = -7; qmax = 7; bias = 8;
    }
    /* A constant block (sym: all zero; affine: hi == lo) gets a ZERO scale and
     * reconstructs EXACTLY, because every value in it is `base`. Dividing by a
     * zero range here would put inf or NaN into the scale and poison a whole
     * output channel -- and a constant block is not exotic, it is what a pruned
     * row or an unused embedding slot looks like. */
    inv = (s > 0.0f) ? 1.0f / s : 0.0f;
    *sc = s;
    if (mn) *mn = base;

    for (i = 0; i < L; i++) {
        float v = (w[i] - base) * inv;
        int r = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
        if (r > qmax) r = qmax;
        if (r < qmin) r = qmin;
        r += bias;
        if (i < h) lo4[i] = (unsigned char)r; else hi4[i - h] = (unsigned char)r;
    }
    for (i = 0; i < nh; i++) bp[i] = (uint8_t)(lo4[i] | (hi4[i] << 4));
    /* An odd L leaves one byte with no high nibble. It is written ZERO rather
     * than left alone, so two writers produce the same file from the same
     * weights and a byte-for-byte comparison of two builds means something. */
    for (; i < h; i++) bp[i] = (uint8_t)lo4[i];
}

void q4_quantize(uint8_t *packed, float *scale, float *minv,
                 const float *w, int n, int k, int blk, int mode)
{
    if (!packed || !scale || !w || n <= 0 || k <= 0) return;
    if (!q4_blk_ok(blk) || !q4_mode_ok(mode)) return;
    if (mode == Q4_AFFINE && !minv) return;
    int nb = q4_blocks(k, blk);
    size_t rb = q4_row_bytes(k, blk);

#ifdef Q4_PER_TENSOR_SCALE
    /* THE NEGATIVE CONTROL (quant4.h). One range for the whole tensor, fed
     * into the same quantiser through the same parameter. It still packs, it
     * still round-trips, every size check still passes and every byte is still
     * a legal nibble -- what changes is the ACCURACY, which is the only thing
     * this control may be allowed to move. */
    float tlo, thi;
    block_range(w, n * k, &tlo, &thi);
#endif

    for (int i = 0; i < n; i++) {
        const float *row = w + (size_t)i * k;
        uint8_t *rp = packed + (size_t)i * rb;
        size_t off = 0;
        for (int b = 0; b < nb; b++) {
            int j0 = b * blk;
            int L = (k - j0 < blk) ? k - j0 : blk;
            float lo, hi;
#ifdef Q4_PER_TENSOR_SCALE
            lo = tlo; hi = thi;
#else
            block_range(row + j0, L, &lo, &hi);
#endif
            quant_block_range(rp + off, scale + (size_t)i * nb + b,
                              minv ? minv + (size_t)i * nb + b : 0,
                              row + j0, L, mode, lo, hi);
            off += (size_t)((L + 1) / 2);
        }
    }
}

void q4_dequantize(float *w, const uint8_t *packed, const float *scale,
                   const float *minv, int n, int k, int blk, int mode)
{
    if (!w || !packed || !scale || n <= 0 || k <= 0) return;
    if (!q4_blk_ok(blk) || !q4_mode_ok(mode)) return;
    if (mode == Q4_AFFINE && !minv) return;
    int nb = q4_blocks(k, blk);
    size_t rb = q4_row_bytes(k, blk);
    float bias = (mode == Q4_AFFINE) ? 0.0f : -8.0f;

    for (int i = 0; i < n; i++) {
        const uint8_t *rp = packed + (size_t)i * rb;
        float *wr = w + (size_t)i * k;
        size_t off = 0;
        for (int b = 0; b < nb; b++) {
            int j0 = b * blk;
            int L = (k - j0 < blk) ? k - j0 : blk;
            int h = (L + 1) / 2, nh = L - h;
            const uint8_t *bp = rp + off;
            float s = scale[(size_t)i * nb + b];
            float base = minv ? minv[(size_t)i * nb + b] : 0.0f;
            for (int q = 0; q < nh; q++) {
                unsigned c = bp[q];
                wr[j0 + q]     = base + s * ((float)(int)(c & 15u) + bias);
                wr[j0 + h + q] = base + s * ((float)(int)(c >> 4)  + bias);
            }
            for (int q = nh; q < h; q++)
                wr[j0 + q] = base + s * ((float)(int)(bp[q] & 15u) + bias);
            off += (size_t)h;
        }
    }
}

/* ---------------------------------------------------------------- matvec --
 *
 * FOUR LANES, NOT FOUR SCALARS. matmul.c measured this on this machine this
 * week and the finding is not specific to f32: `float s0,s1,s2,s3` with an
 * unrolled body compiles to two 2-lane vectors and four shufps of pure
 * overhead, and a single `float s` compiles to nothing vectorised at all
 * (a serial f32 reduction may not be reassociated without -ffast-math). So the
 * accumulator is stated as one SSE2 register, in the GNU vector spelling --
 * which needs no <emmintrin.h> and therefore survives a freestanding build.
 *
 * `aligned(4)` is load-bearing and its absence is a FAULT, not a slowdown:
 * without it the type carries 16-byte alignment, the compiler emits movaps,
 * and an activation vector at a 4-byte offset takes #GP. matmul.c pays the
 * same tax for the same reason.
 *
 * THE SUMMATION ORDER IS PART OF THE FORMAT'S CONTRACT, because two builds of
 * this kernel that disagree in the last bit produce different generated text
 * from the same weights. It is: lane l accumulates bytes l, l+4, l+8, ... of
 * the block; the fold is (a0+a1)+(a2+a3); LOW nibbles are folded before HIGH;
 * the scalar tail is appended after. Nothing may reorder that for speed
 * without saying so here.
 */
typedef float q4_v4 __attribute__((vector_size(16), aligned(4)));

static inline float q4_v4_sum(q4_v4 a) { return (a[0] + a[1]) + (a[2] + a[3]); }

void q4_xsum(float *xsum, const float *x, int k, int blk)
{
    if (!xsum || !x || k <= 0 || !q4_blk_ok(blk)) return;
    int nb = q4_blocks(k, blk);
    for (int b = 0; b < nb; b++) {
        int j0 = b * blk;
        int L = (k - j0 < blk) ? k - j0 : blk;
        q4_v4 a = {0.0f, 0.0f, 0.0f, 0.0f};
        int i = 0;
        for (; i + 3 < L; i += 4) a += *(const q4_v4 *)(x + j0 + i);
        float s = q4_v4_sum(a);
        for (; i < L; i++) s += x[j0 + i];
        xsum[b] = s;
    }
}

void q4_matvec(float *y, const uint8_t *packed, const float *scale,
               const float *minv, const float *x, int n, int k,
               int blk, int mode, const float *xsum)
{
    if (!y || !packed || !scale || !x || !xsum || n <= 0 || k <= 0) return;
    if (!q4_blk_ok(blk) || !q4_mode_ok(mode)) return;
    if (mode == Q4_AFFINE && !minv) return;
    int nb = q4_blocks(k, blk);
    size_t rb = q4_row_bytes(k, blk);
    int affine = (mode == Q4_AFFINE);

    for (int i = 0; i < n; i++) {
        const uint8_t *rp = packed + (size_t)i * rb;
        const float *rs = scale + (size_t)i * nb;
        const float *rm = minv ? minv + (size_t)i * nb : 0;
        float acc = 0.0f;
        size_t off = 0;
        for (int b = 0; b < nb; b++) {
            int j0 = b * blk;
            int L = (k - j0 < blk) ? k - j0 : blk;
            int h = (L + 1) / 2, nh = L - h;
            const uint8_t *bp = rp + off;
            /* THE TWO NIBBLES OF A BYTE FEED TWO SEPARATE CONTIGUOUS RUNS of
             * activations -- weight q from xl, weight q+h from xh -- which is
             * the entire reason the block is split in halves rather than
             * packed as consecutive pairs. Pairs would make lane j read x[2j]
             * and x[2j+1], a stride-2 gather that SSE2 must deinterleave once
             * per four weights; halves make both reads plain sequential
             * loads. */
            const float *xl = x + j0;
            const float *xh = x + j0 + h;
            q4_v4 a0 = {0.0f, 0.0f, 0.0f, 0.0f};
            q4_v4 a1 = {0.0f, 0.0f, 0.0f, 0.0f};
            int q = 0;
            for (; q + 3 < nh; q += 4) {
                unsigned c0 = bp[q], c1 = bp[q+1], c2 = bp[q+2], c3 = bp[q+3];
                q4_v4 vlo = { (float)(int)(c0 & 15u), (float)(int)(c1 & 15u),
                              (float)(int)(c2 & 15u), (float)(int)(c3 & 15u) };
                q4_v4 vhi = { (float)(int)(c0 >> 4),  (float)(int)(c1 >> 4),
                              (float)(int)(c2 >> 4),  (float)(int)(c3 >> 4) };
                a0 += vlo * *(const q4_v4 *)(xl + q);
                a1 += vhi * *(const q4_v4 *)(xh + q);
            }
            float raw = q4_v4_sum(a0) + q4_v4_sum(a1);
            for (; q < nh; q++) {
                unsigned c = bp[q];
                raw += (float)(int)(c & 15u) * xl[q];
                raw += (float)(int)(c >> 4)  * xh[q];
            }
            for (; q < h; q++) raw += (float)(int)(bp[q] & 15u) * xl[q];

            /* THE INNER LOOP HAS NO MODE BRANCH, and that is the finding that
             * shaped this file. It accumulates RAW nibbles (0..15); the whole
             * difference between symmetric and affine is two floats out here:
             *
             *   sym    sum (n-8)*x  =  raw - 8*sum(x)
             *   affine sum (lo+n*s)*x = s*raw + lo*sum(x)
             *
             * So symmetric's supposed advantage -- "one number a block, and no
             * activation sum to carry" -- does not exist: the bias subtraction
             * needs exactly the activation sum the affine minimum needs. The
             * trade between them is purely storage against accuracy, which is
             * what quant4.h measures rather than argues.
             *
             * `xsum` is computed ONCE for the whole matvec (q4_xsum), not per
             * row: it depends on x and the block geometry, and n rows share
             * both. Per row it would be n*k adds instead of k. */
            if (affine) acc += rs[b] * raw + rm[b] * xsum[b];
            else        acc += rs[b] * (raw - 8.0f * xsum[b]);
            off += (size_t)h;
        }
        y[i] = acc;
    }
}

/* ------------------------------------------------- the model-file hookup --
 *
 * These two exist so that carrying NN_Q4 in model.c is ONE LINE in each of its
 * two walkers rather than a parameter threaded through both. model.c's
 * `mat_bytes` and `wrap_mat` take (dtype, rows, cols) and know nothing of a
 * block size or a mode; giving them those would change ten call sites in a
 * file that is walked twice and must agree with itself byte for byte.
 *
 *     static struct sz mat_bytes(...)
 *   +     if (dtype == NN_Q4) { size_t b = q4_mat_bytes(rows, cols);
 *   +                           return b ? sz_val(b) : sz_bad(); }
 *
 *     static unsigned char *wrap_mat(...)
 *   +     if (dtype == NN_Q4) return q4_wrap_mat(out, p, rows, cols);
 *
 * plus `h->dtype != NN_Q4` in lm_expected_size's dtype check and "q4" in
 * lm_describe. The block size and mode are Q4_BLOCK/Q4_MODE, i.e. properties
 * of LM_VERSION rather than fields -- see quant4.h.
 *
 * THAT PATCH IS APPLIED. model.c carries NN_Q4 at all four sites and it costs
 * that file a link dependency on this one, which its own header states at the
 * top. It cost one more site than this comment predicted, and the extra one is
 * the one worth knowing about: `lm_embed_row` also had to learn q4, because a
 * q4 row's stride is q4_row_bytes(k) and not k/2 -- those agree for every k
 * that is a whole number of blocks, which is every tensor in the target shape,
 * so the naive version is correct on the model you would test with and wrong
 * on the next one. model.c carries -DLM_Q4_EMB_HALF_STRIDE as the control for
 * exactly that, and it reddens only at a shape with an odd tail block. */
size_t q4_mat_bytes(uint32_t rows, uint32_t cols)
{
    /* Both become an `int` shape entry inside q4_wrap below, so a dimension
     * above INT_MAX would arrive as a NEGATIVE one; refused here, before the
     * arithmetic that would be done on it. */
    if (!rows || !cols) return 0;
    if (rows > 0x7FFFFFFFu || cols > 0x7FFFFFFFu) return 0;
    return q4_bytes((int)rows, (int)cols, Q4_BLOCK, Q4_MODE);
}

unsigned char *q4_wrap_mat(struct nn_tensor *out, unsigned char *p,
                           uint32_t rows, uint32_t cols)
{
    size_t total = q4_mat_bytes(rows, cols);
    if (!total) { if (out) memset(out, 0, sizeof *out); return p; }
    int n = (int)rows, k = (int)cols;
    size_t off = q4_scale_off(n, k, Q4_BLOCK);
    size_t nb = (size_t)n * (size_t)q4_blocks(k, Q4_BLOCK);
    float *sc = (float *)(void *)(p + off);
    float *mn = (Q4_MODE == Q4_AFFINE) ? sc + nb : (float *)0;
    int dim[2] = { n, k };
    q4_wrap(out, p, sc, mn, 2, dim);
    return p + total;
}

void q4_wrap(struct nn_tensor *out, uint8_t *packed, float *scale, float *minv,
             int ndim, const int *dim)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (ndim <= 0 || ndim > NN_MAXDIM || !dim) return;
    out->dtype = NN_Q4;
    out->ndim = ndim;
    for (int i = 0; i < ndim; i++) out->dim[i] = dim[i];
    /* `q` carries the PACKED nibbles, so it is half as many bytes as the
     * element count -- a caller that reads nn_tensor.q as one int8 per weight
     * (which is what NN_Q8 means) walks off the end at the halfway point. That
     * is why NN_Q4 is a distinct dtype rather than a flag beside NN_Q8. */
    out->q = (int8_t *)packed;
    out->scale = scale;
    /* `data` is unused for a quantised tensor and carries the per-block
     * minimum for Q4_AFFINE. A fifth pointer in struct nn_tensor would be the
     * clean answer and it is not taken here: widening a struct three other
     * files share is a worse trade than one documented reuse, and the reuse is
     * safe because a quantised tensor has no f32 payload for `data` to mean.
     *
     * THIS COMMENT USED TO SAY "the shipped format is symmetric (quant4.h has
     * the numbers), so `minv` is NULL on every tensor a model file produces".
     * That was written against a draft in which Q4_MODE was Q4_SYM, and it
     * became false the moment the ladder in quant4.h chose affine -- the two
     * lines are eleven apart in the header and disagreed for four commits.
     * It is not a cosmetic staleness: model.c DEREFERENCES this pointer in
     * lm_embed_row and infer.c hands it to q4_matvec, so a reader who believed
     * the old sentence would have read a NULL here as the normal case and
     * dropped the check that catches a tensor wrapped wrong. Under the shipped
     * Q4_AFFINE it is non-NULL on every tensor of every model file. */
    out->data = minv;
    out->own = 0;
}

/* ------------------------------------------------- the forward-pass hookup --
 *
 * infer.c's `mv()` dispatches on a tensor's own dtype and its whole body is
 * three lines per dtype. q4 does not fit that shape, for one reason: it needs
 * `xsum`, a per-block sum of the ACTIVATIONS, which q4_matvec takes as an
 * argument and does not compute (quant4.h says why -- n rows share it, and so
 * do the three matvecs of one layer that all read the same residual stream).
 * So the natural hookup would be a scratch buffer threaded from lm_state into
 * mv(), which is a signature change, an arena field and a new number inside
 * lm_state_bytes -- for a dispatch line.
 *
 * This function is the alternative, and it is where the trade is stated: it
 * puts xsum on ITS OWN STACK, so infer.c's hookup is
 *
 *     if (w->dtype == NN_Q4) return q4_mv(y, w, x, n, k);
 *
 * and nothing else in that file moves.
 *
 * WHAT THAT COSTS, derived rather than waved at: xsum is `k` additions and it
 * is recomputed per matvec instead of once per layer. A matvec of the same
 * shape is n*k multiply-adds, so the recomputation is 1/n of the work it sits
 * in front of -- at the target shape the smallest n is kv_dim = 1024, so it is
 * under 0.1%, and the largest waste is the three-way share it gives up on
 * (q, k, v at n = 2048/1024/1024 over one x): 3*1024 adds against 4.2 million
 * multiply-adds. That is not where a token's time goes, and the alternative
 * spends a permanent arena field to recover it.
 *
 * THE STACK BOUND IS FIXED AND NOT A VLA, for the reason infer.c gives about
 * LM_ROPE_MAX_HD: `k` arrives from a header on a disk, so `float xs[k/blk]`
 * is an unbounded stack allocation driven by a file. Above the bound this
 * REFUSES (returns 0) rather than falling back to something slower, because
 * there is no correct fallback that allocates nothing -- and infer.c reads a 0
 * from mv() as "this model cannot be run", which is the truth.
 *
 * 1024 blocks is 4 KiB of stack and covers k up to 65,536 at Q4_BLOCK 64 --
 * twenty-one times the widest matvec input at the target shape, whose largest
 * `k` is hidden = 3072 (48 blocks, 192 bytes). The first draft of this bound
 * was 8192 blocks = 32 KiB, on the reasoning that a generous bound costs
 * nothing because only `nb` of the entries are ever written. That is true of
 * the memory and false of the FRAME: c/kernel/sched/sched.c:16 gives an
 * ordinary kernel thread a 16 KiB stack, so a 32 KiB frame in a function
 * called nine times per layer is a number that has to be checked against the
 * machine rather than chosen for comfort. /bin/lm is a CLI program and gets a
 * 1 MiB stack (c/kernel/exec/exec.c:24, faulted in on touch), so 32 KiB would
 * in fact have been safe there -- but "safe in the one caller that exists
 * today" is not the property a bound in a shared kernel file should have. */
#define Q4_MV_MAX_BLOCKS 1024

int q4_mv(float *y, const struct nn_tensor *w, const float *x, int n, int k)
{
    if (!y || !w || !x || n <= 0 || k <= 0) return 0;
    if (w->dtype != NN_Q4 || w->ndim != 2) return 0;
    /* The shape check is infer.c's, restated here because this function is the
     * one that dereferences: n and k come from the header while w->dim came
     * from the loader, and a disagreement is a read off the end of a mapped
     * file. */
    if (w->dim[0] != n || w->dim[1] != k) return 0;
    if (!w->q || !w->scale) return 0;
    if (Q4_MODE == Q4_AFFINE && !w->data) return 0;

    int nb = q4_blocks(k, Q4_BLOCK);
    if (nb <= 0 || nb > Q4_MV_MAX_BLOCKS) return 0;

    float xs[Q4_MV_MAX_BLOCKS];
    q4_xsum(xs, x, k, Q4_BLOCK);
    q4_matvec(y, (const uint8_t *)w->q, w->scale, w->data, x, n, k,
              Q4_BLOCK, Q4_MODE, xs);
    return 1;
}
