/* quant4_test.c -- q4 against a double reference, per BLOCK, plus the only
 * number that decides whether q4 is usable: what it costs a trained model.
 *
 * THE SHAPE IS nn_test.c's, deliberately. Every `ref_*` here is the textbook
 * definition in double, written from quant4.h's stated layout rather than by
 * calling the code under test, and every bound is DERIVED from the arithmetic
 * and printed alongside what was measured.
 *
 * THE HALF-STEP BOUND IS ASSERTED PER BLOCK, not globally. A global worst-case
 * ratio is satisfied by the largest block on its own and says nothing about
 * the rest -- which is the entire failure mode a per-block scale exists to
 * prevent, so a global check would be measuring the thing it is supposed to
 * be distinguishing from. The per-block loop below reports WHICH block was
 * worst, so a regression names its own location.
 *
 * MODES:
 *   (no args)            the unit gate. Needs no model file.
 *   --model F --corpus F the end-to-end number: val nats/byte for f32, q8 and
 *                        q4, on the SAME held-out bytes and the SAME weights.
 *   --sweep              the same, for every (block size, mode) pair, which is
 *                        the measurement quant4.h's choices are argued from.
 *
 * Built and run by hand today -- see the bottom of quant4.h for the exact
 * command lines, and the report for the one-line make hookup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include "quant4.h"
#include "model.h"
#include "infer.h"

static int checks, failed;

static void ok_(const char *what) { checks++; printf("ok  : %s\n", what); }
static void bad_(const char *what, double got, double want, double bound)
{
    checks++; failed++;
    printf("FAIL: %s\n      got %.9g want %.9g (|err| %.3g > bound %.3g)\n",
           what, got, want, fabs(got - want), bound);
}
static void near_(const char *what, double got, double want, double bound)
{
    if (fabs(got - want) <= bound) ok_(what); else bad_(what, got, want, bound);
}
static void eqi(const char *what, long got, long want)
{
    checks++;
    if (got == want) printf("ok  : %s\n", what);
    else { failed++; printf("FAIL: %s\n      got %ld want %ld\n", what, got, want); }
}

/* xorshift64, not rand(): rand()'s sequence is a libc detail and a failure
 * here has to be reproducible on a machine with a different one. */
static unsigned long long g_seed = 88172645463325252ULL;
static double urand(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return (double)((g_seed >> 11) & 0xFFFFFFFFULL) / 4294967296.0;
}
static float frand(void) { return (float)(urand() * 2.0 - 1.0); }

/* ------------------------------------------------------------ references --
 *
 * ref_unpack reads one weight out of a packed row by walking quant4.h's stated
 * layout from scratch: block index, block length, the (L+1)/2 split, and the
 * nibble. It shares no code with q4_dequantize, which is what makes it able to
 * disagree with it. */
static int ref_unpack(const uint8_t *row, int k, int blk, int j)
{
    int b = j / blk, j0 = b * blk;
    int L = (k - j0 < blk) ? k - j0 : blk;
    int h = (L + 1) / 2;
    size_t off = 0;
    for (int t = 0; t < b; t++) {
        int t0 = t * blk, tl = (k - t0 < blk) ? k - t0 : blk;
        off += (size_t)((tl + 1) / 2);
    }
    int in = j - j0;
    if (in < h) return row[off + in] & 15;
    return row[off + (in - h)] >> 4;
}

static double ref_dot(const double *a, const float *b, int k)
{
    double s = 0.0;
    for (int i = 0; i < k; i++) s += a[i] * (double)b[i];
    return s;
}

/* ------------------------------------------------------------- unit gate -- */

static void t_sizes(void)
{
    printf("-- the byte arithmetic, including the row whose k is not a multiple\n");
    eqi("blocks(1024,32)", q4_blocks(1024, 32), 32);
    eqi("blocks(344,32)  -- 10 full + one short", q4_blocks(344, 32), 11);
    eqi("blocks(33,32)", q4_blocks(33, 32), 2);
    /* 344 = 10*32 + 24: ten 16-byte blocks plus a 12-byte one. Exactly k/2
     * here only because the remainder happens to be even; 33 is the case
     * where it is not. */
    eqi("row_bytes(344,32)", (long)q4_row_bytes(344, 32), 172);
    eqi("row_bytes(33,32)  -- the odd tail costs a whole byte",
        (long)q4_row_bytes(33, 32), 17);
    eqi("row_bytes(1024,32)", (long)q4_row_bytes(1024, 32), 512);
    /* [16,1024] sym: 8192 payload bytes + 16*32 scales*4 */
    eqi("bytes(16,1024,32,sym)", (long)q4_bytes(16, 1024, 32, Q4_SYM),
        16 * 512 + 16 * 32 * 4);
    eqi("bytes(16,1024,32,affine) -- two numbers a block, not one",
        (long)q4_bytes(16, 1024, 32, Q4_AFFINE), 16 * 512 + 16 * 32 * 8);
    eqi("an odd block size is refused", q4_blk_ok(31), 0);
    eqi("blk 0 is refused", q4_blk_ok(0), 0);
}

static void t_packing(void)
{
    printf("-- the packing: which nibble is which, and the level sym does not use\n");
    enum { K = 32 };
    float w[K]; uint8_t p[16]; float sc[1], mn[1];

    /* A block whose quantised codes are known in advance: w[j] = (j-16)/16 of
     * amax, so the codes walk the whole range. Reading them back through
     * ref_unpack (which does not share code with the packer) is what pins the
     * low/high assignment: a packer that put weight j in the HIGH nibble of
     * byte j/2 would still round-trip through its own unpacker. */
    for (int j = 0; j < K; j++) w[j] = (float)(j - 16) / 16.0f;
    q4_quantize(p, sc, mn, w, 1, K, 32, Q4_SYM);
    int mism = 0;
    for (int j = 0; j < K; j++) {
        int code = ref_unpack(p, K, 32, j) - 8;   /* sym stores q+8 */
        double want = (double)w[j] / (double)sc[0];
        int wi = (int)(want < 0 ? want - 0.5 : want + 0.5);
        if (wi > 7) wi = 7; if (wi < -7) wi = -7;
        if (code != wi) mism++;
    }
    eqi("every weight lands in the nibble quant4.h says it does", mism, 0);

    /* The first half of a block occupies the LOW nibbles. Weight 0 and weight
     * 16 therefore share byte 0 -- assert the exact byte, because "which half
     * of the block" is the one thing a reader and a writer can disagree about
     * while both round-trip perfectly against themselves. */
    {
        int lo = ref_unpack(p, K, 32, 0), hi = ref_unpack(p, K, 32, 16);
        eqi("byte 0 is weight 0 in the low nibble and weight 16 in the high",
            (long)p[0], (long)(lo | (hi << 4)));
    }

    /* SYM WASTES A LEVEL AND IT IS VISIBLE IN THE BYTES. Codes are q+8 with q
     * in -7..7, so nibble value 0 can never appear. That is the storage cost
     * of symmetry stated as a fact about the file rather than as an argument;
     * affine uses all sixteen. */
    int zero_nibbles = 0;
    for (int j = 0; j < K; j++) if (ref_unpack(p, K, 32, j) == 0) zero_nibbles++;
    eqi("symmetric never emits nibble 0 -- the level it spends on symmetry",
        zero_nibbles, 0);

    {
        int used[16]; memset(used, 0, sizeof used);
        float wa[K]; uint8_t pa[16]; float sa[1], ma[1];
        for (int j = 0; j < K; j++) wa[j] = (float)j / 31.0f;
        q4_quantize(pa, sa, ma, wa, 1, K, 32, Q4_AFFINE);
        for (int j = 0; j < K; j++) used[ref_unpack(pa, K, 32, j)] = 1;
        int n = 0; for (int j = 0; j < 16; j++) n += used[j];
        eqi("affine reaches all sixteen levels on a uniform ramp", n, 16);
    }

    /* An all-zero block: sym reconstructs exactly through a zero scale, and
     * the bytes are 0x88 (code 8 == q 0) rather than 0x00. A quantiser that
     * divided by amax would put inf in the scale here and poison a whole
     * output channel; a pruned row is not exotic. */
    {
        float wz[K], back[K]; uint8_t pz[16]; float sz[1], mz[1];
        for (int j = 0; j < K; j++) wz[j] = 0.0f;
        q4_quantize(pz, sz, mz, wz, 1, K, 32, Q4_SYM);
        eqi("an all-zero block gets a zero scale", (long)(sz[0] == 0.0f), 1);
        int allb = 1; for (int j = 0; j < 16; j++) if (pz[j] != 0x88) allb = 0;
        eqi("and packs as 0x88 -- the zero CODE, not a zero byte", allb, 1);
        q4_dequantize(back, pz, sz, mz, 1, K, 32, Q4_SYM);
        int nz = 0; for (int j = 0; j < K; j++) if (back[j] != 0.0f) nz++;
        eqi("the zero block reconstructs exactly", nz, 0);
    }

    /* The odd tail. k=33 with blk=32 leaves a block of ONE weight in the low
     * nibble of a byte whose high nibble is unused; that byte must be
     * deterministic (high nibble zero) or two writers produce different files
     * from the same weights. */
    {
        float wo[33], bo[33]; uint8_t po[17]; float so[2], mo[2];
        for (int j = 0; j < 33; j++) wo[j] = frand();
        q4_quantize(po, so, mo, wo, 1, 33, 32, Q4_SYM);
        eqi("the odd tail byte's unused high nibble is zero", (long)(po[16] >> 4), 0);
        q4_dequantize(bo, po, so, mo, 1, 33, 32, Q4_SYM);
        /* the one-weight block is exact: amax IS the weight, code 7 */
        near_("a one-weight tail block reconstructs its weight exactly",
              (double)bo[32], (double)wo[32], 1e-7 * fabs((double)wo[32]));
    }
}

/* The bound the half-step check asserts, derived rather than chosen.
 *
 * By construction |w - w_hat| <= s/2 where s is the block's step: sym has
 * s = amax/7 and every value is within 7 steps of zero; affine has
 * s = (hi-lo)/15 and every value is within 15 steps of lo. Rounding is
 * half-away-from-zero, so the error before any floating-point rounding is at
 * most exactly half a step.
 *
 * On top of that, three f32 roundings: s itself, the reciprocal used to
 * quantise, and the reconstruction multiply. Each is a relative 2^-24, and
 * the reconstructed value is at most 15 steps from the base, so the extra
 * absolute error is at most ~3 * 15 * s * 2^-24 = 2.7e-6 * s, i.e. 5.4e-6 of
 * a half-step. 1e-5 is that, rounded up one digit -- not a number fitted to
 * what was observed. */
#define HALFSTEP_SLOP 1e-5

static double halfstep_worst(const float *w, const float *back,
                             const float *scale, int n, int k, int blk,
                             int *worst_row, int *worst_blk)
{
    int nb = q4_blocks(k, blk);
    double worst = 0.0;
    *worst_row = -1; *worst_blk = -1;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < nb; b++) {
            float s = scale[(size_t)i * nb + b];
            if (s == 0.0f) continue;            /* a constant block: exact */
            double half = (double)s * 0.5;
            int j0 = b * blk, L = (k - j0 < blk) ? k - j0 : blk;
            for (int j = 0; j < L; j++) {
                double e = fabs((double)back[(size_t)i * k + j0 + j] -
                                (double)w[(size_t)i * k + j0 + j]);
                double r = e / half;
                if (r > worst) { worst = r; *worst_row = i; *worst_blk = b; }
            }
        }
    return worst;
}

static void t_recon(int mode, int blk, const char *name)
{
    printf("-- %s blk %d: half a step, per block, on rows of DIFFERENT magnitude\n",
           name, blk);
    enum { N = 16, K = 344 };   /* K deliberately not a multiple of 32 or 64 */
    static float w[N * K], back[N * K];
    static uint8_t p[N * 172];
    static float sc[N * 11], mn[N * 11];
    int nb = q4_blocks(K, blk);

    /* Rows whose magnitude falls by 2^-1 each: the case per-ROW quantisation
     * exists for (and per-TENSOR fails). And WITHIN each row a magnitude ramp
     * across its length, which is the case per-BLOCK exists for and per-row
     * does not catch -- a row that is uniform in magnitude is quantised just
     * as well by one scale as by eleven. */
    for (int i = 0; i < N; i++) {
        float mag = 1.0f;
        for (int e = 0; e < i; e++) mag *= 0.5f;
        for (int j = 0; j < K; j++) {
            float ramp = 1.0f / (1.0f + (float)j * 0.5f);
            w[(size_t)i * K + j] = frand() * mag * ramp;
        }
    }
    for (int j = 0; j < K; j++) w[(size_t)5 * K + j] = 0.0f;   /* a pruned row */

    q4_quantize(p, sc, mn, w, N, K, blk, mode);
    q4_dequantize(back, p, sc, mn, N, K, blk, mode);

    int wr, wb;
    double worst = halfstep_worst(w, back, sc, N, K, blk, &wr, &wb);
    near_("every weight reconstructs within half a step of ITS OWN block",
          worst, 0.0, 1.0 + HALFSTEP_SLOP);
    printf("      worst row %d block %d at %.6f of a half-step\n", wr, wb, worst);

    /* THE SAME BOUND EXPRESSED IN THE WEIGHTS RATHER THAN IN THE STORED
     * SCALE, and it is the only one of the two that can see a badly CHOSEN
     * scale. The check above divides by sc[], so a quantiser that used one
     * enormous scale everywhere satisfies it trivially -- every weight really
     * is within half of THAT step. Here the step is recomputed from each
     * block's own extrema, so the bound is a property of the DATA:
     *
     *     |err_j| <= step_b / 2   =>   sum_{j in b} err^2 <= L_b * (step_b/2)^2
     *
     * by construction, with no distributional assumption and nothing fitted.
     * This is the check -DQ4_PER_TENSOR_SCALE must redden.
     *
     * PER BLOCK, AND THE FIRST VERSION OF IT WAS PER TENSOR AND UNRELIABLE.
     * Summed over the whole tensor, the ratio is dominated by the LOUDEST
     * block -- the one a per-tensor scale happens to get right -- so the
     * control reddened three of these four and passed the fourth, on nothing
     * but which random draw the shared RNG had reached. That is the same
     * mistake this file's header names ("a global bound is satisfied by the
     * largest block alone"), made inside the test written to avoid it. */
    {
        double worst_ratio = 0.0; int wrr = -1, wrb = -1;
        double sse_all = 0.0, bnd_all = 0.0;
        for (int i = 0; i < N; i++)
            for (int b = 0; b < nb; b++) {
                int j0 = b * blk, L = (K - j0 < blk) ? K - j0 : blk;
                const float *bw = w + (size_t)i * K + j0;
                float lo = bw[0], hi = bw[0];
                for (int j = 1; j < L; j++) {
                    if (bw[j] < lo) lo = bw[j];
                    if (bw[j] > hi) hi = bw[j];
                }
                double step;
                if (mode == Q4_AFFINE) step = ((double)hi - (double)lo) / 15.0;
                else {
                    double a = fabs((double)lo);
                    if (fabs((double)hi) > a) a = fabs((double)hi);
                    step = a / 7.0;
                }
                double sse = 0.0;
                for (int j = 0; j < L; j++) {
                    double e = (double)back[(size_t)i * K + j0 + j] - (double)bw[j];
                    sse += e * e;
                }
                double bnd = (double)L * (step * 0.5) * (step * 0.5);
                sse_all += sse; bnd_all += bnd;
                /* A constant block has step 0 and must reconstruct EXACTLY,
                 * so its ratio is 0 or infinite and never in between. */
                double ratio = (bnd > 0.0) ? sqrt(sse / bnd)
                                           : (sse > 0.0 ? 1e30 : 0.0);
                if (ratio > worst_ratio) { worst_ratio = ratio; wrr = i; wrb = b; }
            }
        near_("EVERY BLOCK is within the bound its OWN extrema give",
              worst_ratio, 0.0, 1.0 + HALFSTEP_SLOP);
        printf("      worst row %d block %d at %.4f of its own bound; "
               "tensor rms %.6g vs bound %.6g\n",
               wrr, wrb, worst_ratio,
               sqrt(sse_all / ((double)N * K)), sqrt(bnd_all / ((double)N * K)));
    }

    /* AND THE SCALES MUST ACTUALLY TRACK THEIR BLOCKS. Row 0 ramps by
     * 1/(1+j/2) across its length, so its first block spans about 160x its
     * last block and the scales must too. A per-TENSOR quantiser gives every
     * block the identical scale -- ratio exactly 1 -- which is a structural
     * fact, not a statistical one, so the threshold below sits an order of
     * magnitude BELOW what the ramp forces rather than at what was observed. */
    {
        float smin = 0.0f, smax = 0.0f;
        for (int b = 0; b < nb; b++) {
            float v = sc[b];
            if (v <= 0.0f) continue;
            if (smin == 0.0f || v < smin) smin = v;
            if (v > smax) smax = v;
        }
        int spread_ok = (smin > 0.0f && smax >= 10.0f * smin);
        eqi("row 0 block scales span the row magnitude ramp (>=10x)",
            spread_ok, 1);
        printf("      row 0 scale spread %.1fx\n",
               smin > 0.0f ? (double)(smax / smin) : 0.0);
    }

    /* The pruned row: sym gives it a zero scale and affine a zero range, and
     * both must reconstruct it EXACTLY rather than to something small. */
    int nz = 0;
    for (int j = 0; j < K; j++) if (back[(size_t)5 * K + j] != 0.0f) nz++;
    eqi("the pruned row reconstructs exactly", nz, 0);
}

static void t_matvec(int mode, int blk, const char *name)
{
    printf("-- %s blk %d: the kernel adds NO error to the quantisation it inherits\n",
           name, blk);
    enum { N = 24, K = 344 };
    static float w[N * K], back[N * K], x[K], y[N];
    static uint8_t p[N * 172];
    static float sc[N * 11], mn[N * 11], xs[11];
    static double bd[K];
    int nb = q4_blocks(K, blk);

    for (size_t t = 0; t < (size_t)N * K; t++) w[t] = frand();
    for (int j = 0; j < K; j++) x[j] = frand();

    q4_quantize(p, sc, mn, w, N, K, blk, mode);
    q4_dequantize(back, p, sc, mn, N, K, blk, mode);
    q4_xsum(xs, x, K, blk);
    q4_matvec(y, p, sc, mn, x, N, K, blk, mode, xs);

    /* Against an exact double dot of the DEQUANTISED weights -- not the
     * originals. That distinction is the whole honesty of the check: q4
     * introduces error in the weights, once, offline, and what the kernel must
     * not do is introduce any more. Comparing against the originals would fold
     * the two together and let a broken kernel hide inside the error it is
     * allowed to have.
     *
     * THE BOUND IS THE STANDARD BLOCKED-DOT FORWARD ERROR, evaluated on this
     * data rather than against an assumed magnitude. A sum of m terms in f32
     * carries |err| <= gamma_m * sum|terms|, gamma_m ~ m*u with u = 2^-24;
     * here each block accumulates at most blk terms and the nb block results
     * are then summed, so m = blk + nb. The terms are the RAW nibbles (0..15)
     * times the activations, NOT the centred weights -- the kernel accumulates
     * raw and cancels the bias at the end, so it is the raw magnitude that
     * gets rounded. That inflation, up to 15/7 for symmetric, is what the
     * mode-free inner loop costs, and it is charged here rather than hidden. */
    double bound = 0.0, worst = 0.0;
    double u = 1.0 / 16777216.0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < K; j++) bd[j] = (double)back[(size_t)i * K + j];
        double r = ref_dot(bd, x, K);
        double e = fabs((double)y[i] - r);
        if (e > worst) worst = e;
        double mag = 0.0;
        for (int b = 0; b < nb; b++) {
            int j0 = b * blk, L = (K - j0 < blk) ? K - j0 : blk;
            double ax = 0.0;
            for (int j = 0; j < L; j++) ax += fabs((double)x[j0 + j]);
            double sv = (double)sc[(size_t)i * nb + b];
            double base = (mode == Q4_AFFINE)
                        ? fabs((double)mn[(size_t)i * nb + b]) : 0.0;
            mag += sv * 15.0 * ax + base * ax;
        }
        double bi = (double)(blk + nb) * u * mag;
        if (bi > bound) bound = bi;
    }
    near_("q4_matvec matches an exact dot of the dequantised weights",
          worst, 0.0, bound);
    printf("      worst absolute %.3g against the derived bound %.3g (%.1f%% of it)\n",
           worst, bound, 100.0 * worst / bound);
}

/* The ON-DISK layout, exercised the way model.c will walk it.
 *
 * TWO TENSORS BACK TO BACK, the first with an ODD payload, because that is the
 * only arrangement in which the alignment pad can be observed: one tensor
 * alone would put its scales wherever they land and read them back happily,
 * and every check would pass on a file that faults the moment a SECOND tensor
 * follows. The shape below (1 x 33 at blk 32) has a payload of 17 bytes, so
 * without the pad the scales would start at offset 17 and the next tensor at
 * an odd address. */
static void t_layout(void)
{
    printf("-- the on-disk layout, and the pad that makes it mappable\n");
    enum { R1 = 1, C1 = 33, R2 = 3, C2 = 64 };
    eqi("an odd payload is padded up to a multiple of 4",
        (long)q4_scale_off(R1, C1, Q4_BLOCK), 20);
    eqi("an already-aligned payload is not padded",
        (long)q4_scale_off(R2, C2, Q4_BLOCK), (long)q4_payload_bytes(R2, C2, Q4_BLOCK));

    size_t b1 = q4_mat_bytes(R1, C1), b2 = q4_mat_bytes(R2, C2);
    eqi("a q4 tensor is a whole number of 4-byte words",
        (long)((b1 % 4) | (b2 % 4)), 0);

    static unsigned char blob[4096];
    memset(blob, 0xEE, sizeof blob);          /* poison: an unwritten byte read
                                               * back as data is then visible */
    float w1[R1 * C1], w2[R2 * C2], back[R2 * C2];
    for (int i = 0; i < R1 * C1; i++) w1[i] = frand();
    for (int i = 0; i < R2 * C2; i++) w2[i] = frand();

    /* Write them exactly as a writer would: payload, pad, scales, minima. */
    unsigned char *wp = blob;
    for (int t = 0; t < 2; t++) {
        int R = t ? R2 : R1, C = t ? C2 : C1;
        const float *src = t ? w2 : w1;
        int nb = q4_blocks(C, Q4_BLOCK);
        size_t off = q4_scale_off(R, C, Q4_BLOCK);
        float *sc = (float *)(void *)(wp + off);
        float *mn = (Q4_MODE == Q4_AFFINE) ? sc + (size_t)R * nb : (float *)0;
        q4_quantize(wp, sc, mn, src, R, C, Q4_BLOCK, Q4_MODE);
        wp += q4_mat_bytes((uint32_t)R, (uint32_t)C);
    }

    /* Read them back the way lm_open will: one wrap per tensor, each returning
     * the pointer the next one starts at. */
    struct nn_tensor t1, t2;
    unsigned char *p = blob;
    p = q4_wrap_mat(&t1, p, R1, C1);
    p = q4_wrap_mat(&t2, p, R2, C2);
    eqi("the two wraps consume exactly the bytes the sizes claim",
        (long)(p - blob), (long)(b1 + b2));
    eqi("the wrapped dtype is NN_Q4, not NN_Q8", t1.dtype, NN_Q4);
    eqi("the scale pointer is 4-byte aligned",
        (long)(((uintptr_t)t2.scale) & 3u), 0);

    /* THE SECOND TENSOR IS THE ONE THAT MATTERS. Its bytes are only correct if
     * the first tensor's size -- pad included -- was right; an off-by-three
     * there shifts every value here and nothing earlier would have noticed. */
    q4_dequantize(back, (const uint8_t *)t2.q, t2.scale, t2.data,
                  R2, C2, Q4_BLOCK, Q4_MODE);
    float ref[R2 * C2];
    uint8_t pk[R2 * 32]; float sc2[R2 * 2], mn2[R2 * 2];
    q4_quantize(pk, sc2, mn2, w2, R2, C2, Q4_BLOCK, Q4_MODE);
    q4_dequantize(ref, pk, sc2, mn2, R2, C2, Q4_BLOCK, Q4_MODE);
    int diff = 0;
    for (int i = 0; i < R2 * C2; i++) if (back[i] != ref[i]) diff++;
    eqi("the SECOND tensor reads back exactly -- so the first one's size,\n"
        "       pad included, was right", diff, 0);
}

/* ------------------------------------------------------------ end-to-end -- */

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b) { fclose(f); return 0; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f); *len = (size_t)n; return b;
}

/* Round a float onto the IEEE binary16 grid, WITHOUT storing it as one. This
 * measures exactly what an f16 scale would cost in ACCURACY -- the only
 * question worth answering before deciding whether to pay for a software
 * half<->float converter on a machine with no F16C. Ties round up rather than
 * to even; that is a 1-ulp difference on a measurement whose signal is 2^-11,
 * and it is stated rather than hidden because a converter that SHIPPED would
 * have to make the other choice and be tested for it. */
static float to_f16(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = x & 0x80000000u;
    int e = (int)((x >> 23) & 0xFFu) - 127;
    uint32_t m = x & 0x7FFFFFu;
    float r; uint32_t y;
    if (((x >> 23) & 0xFFu) == 0xFFu || e > 15) {
        y = sign | 0x7F800000u; memcpy(&r, &y, 4); return r;
    }
    if (e < -24) { memcpy(&r, &sign, 4); return r; }
    if (e < -14) {                              /* an f16 subnormal */
        int sh = 13 + (-14 - e);
        uint32_t full = m | 0x800000u;
        uint32_t q = (full + (1u << (sh - 1))) >> sh;
        r = (float)q * 5.9604644775390625e-8f;  /* q * 2^-24 */
        return sign ? -r : r;
    }
    {
        uint32_t q = (m + 0x1000u) >> 13;
        if (q > 0x3FFu) {
            q = 0; e += 1;
            if (e > 15) { y = sign | 0x7F800000u; memcpy(&r, &y, 4); return r; }
        }
        y = sign | (uint32_t)((e + 127) << 23) | (q << 13);
        memcpy(&r, &y, 4);
        return r;
    }
}

/* Every f32 matmul tensor of an open model, as a writable [rows,cols] view
 * into the blob. Norms and tok_emb are excluded because model.h keeps them
 * f32 in a quantised file -- so quantising them here would measure a format
 * that does not exist. This mirrors exactly what lmtrain's --also-q8 does,
 * which is what makes the q8 column comparable to its published number. */
struct wmat { float *d; int rows, cols; };

static int collect(struct lm_model *m, struct wmat *out, int cap)
{
    int n = 0;
    for (unsigned L = 0; L < m->h.n_layers; L++) {
        struct nn_tensor *t[7] = { &m->layer[L].wq, &m->layer[L].wk,
                                   &m->layer[L].wv, &m->layer[L].wo,
                                   &m->layer[L].w1, &m->layer[L].w3,
                                   &m->layer[L].w2 };
        for (int i = 0; i < 7; i++) {
            if (n >= cap) return -1;
            out[n].d = t[i]->data; out[n].rows = t[i]->dim[0];
            out[n].cols = t[i]->dim[1]; n++;
        }
    }
    if (!(m->h.flags & LM_TIED)) {
        if (n >= cap) return -1;
        out[n].d = m->wcls.data; out[n].rows = m->wcls.dim[0];
        out[n].cols = m->wcls.dim[1]; n++;
    }
    return n;
}

/* Mean cross-entropy in nats per byte over the held-out tail, with the
 * windows FIXED: every configuration below sees exactly the same bytes at
 * exactly the same positions, so the difference between two rows is the
 * quantiser and nothing else. Non-overlapping windows covering the whole tail
 * rather than a random sample -- lmtrain samples 8 windows because it does
 * this every 500 steps; this runs once.
 *
 * IT ALSO RECORDS THE LOGITS, and that is not a convenience. The loss
 * differences between two q4 configurations turned out to be ~0.005 nats
 * against a run-to-run spread of the same size, and the first sweep printed a
 * NON-MONOTONE column -- a coarser block scoring better than a finer one,
 * which cannot be true of the quantiser and therefore was not being measured
 * by that column. `dlogit` is the second instrument: the RMS change in the
 * logits, mean-centred (softmax is shift-invariant, so the mean of a logit row
 * is not observable and charging a quantiser for it would be measuring
 * nothing), relative to the f32 logits' own spread. It averages 12,611 x 256
 * numbers instead of 12,611, so it ranks what the loss cannot. */
static double val_loss(struct lm_model *m, const unsigned char *data,
                       size_t lo, size_t hi, long *ntok,
                       float *logsink, const float *logref, double *dlogit)
{
    struct lm_state s;
    if (lm_state_new(&s, m) != 0) return -1.0;   /* 0 is success */
    int seq = (int)m->h.seq_len;
    int V = (int)m->h.vocab;
    double sum = 0.0; long cnt = 0;
    double dnum = 0.0, dden = 0.0;
    for (size_t w0 = lo; w0 + 2 <= hi; w0 += (size_t)seq) {
        size_t wl = hi - w0; if (wl > (size_t)seq) wl = (size_t)seq;
        if (wl < 2) break;
        lm_state_reset(&s);
        for (size_t t = 0; t + 1 < wl; t++) {
            const float *lg = lm_forward(m, &s, (int)data[w0 + t], (int)t);
            if (!lg) { lm_state_free(&s); return -1.0; }
            double mx = lg[0];
            for (int v = 1; v < V; v++) if ((double)lg[v] > mx) mx = lg[v];
            double z = 0.0;
            for (int v = 0; v < V; v++) z += exp((double)lg[v] - mx);
            int tgt = (int)data[w0 + t + 1];
            sum += -((double)lg[tgt] - mx - log(z));
            if (logsink) memcpy(logsink + (size_t)cnt * V, lg, (size_t)V * sizeof(float));
            if (logref) {
                const float *r = logref + (size_t)cnt * V;
                double ma = 0.0, mb = 0.0;
                for (int v = 0; v < V; v++) { ma += lg[v]; mb += r[v]; }
                ma /= V; mb /= V;
                for (int v = 0; v < V; v++) {
                    double a = (double)lg[v] - ma, b = (double)r[v] - mb;
                    dnum += (a - b) * (a - b);
                    dden += b * b;
                }
            }
            cnt++;
        }
    }
    lm_state_free(&s);
    *ntok = cnt;
    if (dlogit) *dlogit = (logref && dden > 0) ? sqrt(dnum / dden) : 0.0;
    return cnt ? sum / (double)cnt : -1.0;
}

/* bits per weight a configuration costs on disk, from the format's own
 * arithmetic rather than from a remembered constant. */
static double bits_per_weight(const struct wmat *w, int nw, int blk, int mode)
{
    double bits = 0.0, wts = 0.0;
    for (int i = 0; i < nw; i++) {
        bits += 8.0 * (double)q4_bytes(w[i].rows, w[i].cols, blk, mode);
        wts += (double)w[i].rows * (double)w[i].cols;
    }
    return bits / wts;
}

static double q8_bits_per_weight(const struct wmat *w, int nw)
{
    double bits = 0.0, wts = 0.0;
    for (int i = 0; i < nw; i++) {
        bits += 8.0 * ((double)w[i].rows * w[i].cols + 4.0 * w[i].rows);
        wts += (double)w[i].rows * (double)w[i].cols;
    }
    return bits / wts;
}

static double rms_rel(const float *a, const float *b, size_t n)
{
    double se = 0.0, sw = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        se += d * d; sw += (double)b[i] * (double)b[i];
    }
    return sqrt(se / (sw > 0 ? sw : 1));
}

static int e2e(const char *mpath, const char *cpath, int sweep, int full)
{
    size_t mlen = 0, clen = 0;
    unsigned char *blob = slurp(mpath, &mlen);
    if (!blob) { printf("e2e: cannot read %s\n", mpath); return 1; }
    unsigned char *corp = slurp(cpath, &clen);
    if (!corp) { printf("e2e: cannot read %s\n", cpath); return 1; }

    struct lm_model m;
    int rc = lm_open(&m, blob, mlen);
    if (rc != 0) { printf("e2e: lm_open(%s) = %d\n", mpath, rc); return 1; }
    if (m.h.dtype != NN_F32) {
        printf("e2e: %s is not an f32 model (dtype %u) -- the round trip needs\n"
               "  the unquantised weights to start from.\n", mpath, m.h.dtype);
        return 1;
    }
    char desc[256]; lm_describe(&m, desc, sizeof desc);
    printf("model  : %s\n         %s\n", mpath, desc);

    /* lmtrain's own 90/10 split, so the held-out region is the one the
     * published f32 and q8 numbers were measured on. The corpus is a LIVE
     * FILE (tools/lmtrain.md says so) -- its byte count is printed because a
     * different count means a different split and a different absolute loss.
     * The DIFFERENCES between the rows below are unaffected either way. */
    /* --full scores the WHOLE corpus instead of the held-out tenth, and it is
     * not cheating: the quantity being ranked is `d(nats)`, the degradation one
     * quantiser causes relative to the SAME f32 weights, and that difference is
     * as well defined on text the model was trained on as on text it was not.
     * Ten times the bytes is a third of the noise, which is what the first
     * sweep needed -- its d(nats) column came out NON-MONOTONE in block size
     * (a coarser block scoring better than a finer one), which cannot be a
     * property of the quantiser and therefore was not a measurement of it.
     * The absolute `nats/byte` figure under --full is of course NOT a held-out
     * number and must not be quoted as one; that is what the default is for. */
    size_t lo = full ? 0 : (size_t)((double)clen * 0.9);
    printf("corpus : %s (%lu bytes, scored %lu..%lu%s)\n",
           cpath, (unsigned long)clen, (unsigned long)lo, (unsigned long)clen,
           full ? " -- the FULL corpus, NOT held out" : " -- held out");

    static struct wmat W[512];
    int nw = collect(&m, W, 512);
    if (nw < 0) { printf("e2e: more than 512 weight matrices\n"); return 1; }

    size_t maxn = 0, totw = 0;
    for (int i = 0; i < nw; i++) {
        size_t n = (size_t)W[i].rows * W[i].cols;
        if (n > maxn) maxn = n;
        totw += n;
    }
    printf("weights: %d matmul matrices, %lu values (the norms and tok_emb\n"
           "         stay f32 -- model.h's rule, and lmtrain's for q8)\n",
           nw, (unsigned long)totw);

    /* Every configuration starts from the SAME f32 weights rather than from
     * the previous round trip: quantising an already-quantised tensor is a
     * different (and much kinder) question than the one being asked. */
    unsigned char *pristine = (unsigned char *)malloc(mlen);
    uint8_t *pk = (uint8_t *)malloc(maxn);
    float *sc = (float *)malloc((maxn / 2 + 8) * sizeof(float));
    float *mn = (float *)malloc((maxn / 2 + 8) * sizeof(float));
    int8_t *q8 = (int8_t *)malloc(maxn);
    float *s8 = (float *)malloc((maxn + 8) * sizeof(float));
    float *ref = (float *)malloc(maxn * sizeof(float));
    if (!pristine || !pk || !sc || !mn || !q8 || !s8 || !ref) {
        printf("e2e: out of memory\n"); return 1;
    }
    memcpy(pristine, blob, mlen);

    size_t ntmax = clen - lo + 8;
    float *lref = (float *)malloc(ntmax * (size_t)m.h.vocab * sizeof(float));
    if (!lref) { printf("e2e: out of memory for the logit reference\n"); return 1; }

    long ntok = 0; double dl = 0.0;
    double base = val_loss(&m, corp, lo, clen, &ntok, lref, NULL, NULL);
    if (base < 0) { printf("e2e: the f32 forward pass failed\n"); return 1; }

    printf("\n%-22s %10s %10s %9s %8s %8s %9s\n",
           "config", "nats/byte", "bits/byte", "d(nats)", "bits/wt", "dlogit", "rms rel");
    printf("%-22s %10.4f %10.4f %9s %8.2f %8s %9s\n",
           "f32 (reference)", base, base / log(2.0), "--", 32.0, "--", "--");
    printf("       %ld held-out bytes scored, identical windows for every row below\n",
           ntok);

    /* ---- q8, the published comparison ---- */
    {
        double rr = 0.0;
        for (int i = 0; i < nw; i++) {
            size_t n = (size_t)W[i].rows * W[i].cols;
            memcpy(ref, W[i].d, n * sizeof(float));
            nn_quantize_q8(q8, s8, W[i].d, W[i].rows, W[i].cols);
            nn_dequantize_q8(W[i].d, q8, s8, W[i].rows, W[i].cols);
            double r = rms_rel(W[i].d, ref, n);
            if (r > rr) rr = r;
        }
        long nt; double v = val_loss(&m, corp, lo, clen, &nt, NULL, lref, &dl);
        printf("%-22s %10.4f %10.4f %+9.4f %8.2f %8.4f %9.4f\n",
               "q8 per-row sym", v, v / log(2.0), v - base,
               q8_bits_per_weight(W, nw), dl, rr);
        memcpy(blob, pristine, mlen);
    }

    /* ---- q4. THE LADDER IS BY EQUAL STORAGE, not by equal block size.
     * Affine spends two numbers a block where symmetric spends one, so
     * affine at B costs the same bytes as symmetric at B/2 -- comparing them
     * at the same B would be comparing a bigger file with a smaller one and
     * calling the bigger one more accurate. ---- */
    int blks_all[6] = { 8, 16, 32, 64, 128, 0 };
    int blks_one[2] = { Q4_BLOCK, 0 };
    int *blks = sweep ? blks_all : blks_one;
    int modes[2] = { Q4_SYM, Q4_AFFINE };
    const char *mname[2] = { "sym", "affine" };

    /* THE THIRD AXIS: f16 metadata. The payload is unchanged and every
     * per-block number costs 2 bytes instead of 4, which is 0.5 bits/weight at
     * blk 32 and 1.0 at blk 16 -- the difference between a 0.6B model at 373 MB
     * and at 336 MB. What it costs in accuracy is a relative 2^-11 on a number
     * whose own quantisation step is 1/15, so it should be free; "should be" is
     * why it is measured rather than assumed. */
    for (int f16 = 0; f16 < (sweep ? 2 : 1); f16++)
    for (int mi = 0; mi < 2; mi++)
        for (int bi = 0; blks[bi]; bi++) {
            int blk = blks[bi], mode = modes[mi];
            double rr = 0.0;
            for (int i = 0; i < nw; i++) {
                size_t n = (size_t)W[i].rows * W[i].cols;
                int nb = q4_blocks(W[i].cols, blk);
                memcpy(ref, W[i].d, n * sizeof(float));
                q4_quantize(pk, sc, mn, W[i].d, W[i].rows, W[i].cols, blk, mode);
                if (f16) {
                    size_t ns = (size_t)W[i].rows * (size_t)nb;
                    for (size_t t = 0; t < ns; t++) {
                        sc[t] = to_f16(sc[t]);
                        if (mode == Q4_AFFINE) mn[t] = to_f16(mn[t]);
                    }
                }
                q4_dequantize(W[i].d, pk, sc, mn, W[i].rows, W[i].cols, blk, mode);
                double r = rms_rel(W[i].d, ref, n);
                if (r > rr) rr = r;
            }
            long nt; double v = val_loss(&m, corp, lo, clen, &nt, NULL, lref, &dl);
            char lbl[40];
            snprintf(lbl, sizeof lbl, "q4 %s blk %d%s", mname[mi], blk,
                     f16 ? " f16sc" : "");
            double bw = bits_per_weight(W, nw, blk, mode);
            if (f16) bw = 4.0 + (bw - 4.0) * 0.5;
            printf("%-22s %10.4f %10.4f %+9.4f %8.2f %8.4f %9.4f\n",
                   lbl, v, v / log(2.0), v - base, bw, dl, rr);
            memcpy(blob, pristine, mlen);
        }

    /* A per-ROW q4 scale, which is the thing quant4.h rejects: the same code
     * with the block set to the whole row. Printed in the same table so "a row
     * scale is too coarse at four bits" is a number here rather than an
     * assertion in a comment. */
    for (int mi = 0; mi < 2; mi++) {
        double rr = 0.0; int ok = 1;
        for (int i = 0; i < nw && ok; i++)
            if (W[i].cols % 2 || W[i].cols > 1024) ok = 0;
        if (!ok) continue;
        for (int i = 0; i < nw; i++) {
            size_t n = (size_t)W[i].rows * W[i].cols;
            memcpy(ref, W[i].d, n * sizeof(float));
            q4_quantize(pk, sc, mn, W[i].d, W[i].rows, W[i].cols,
                        W[i].cols, modes[mi]);
            q4_dequantize(W[i].d, pk, sc, mn, W[i].rows, W[i].cols,
                          W[i].cols, modes[mi]);
            double r = rms_rel(W[i].d, ref, n);
            if (r > rr) rr = r;
        }
        long nt; double v = val_loss(&m, corp, lo, clen, &nt, NULL, lref, &dl);
        char lbl[40];
        snprintf(lbl, sizeof lbl, "q4 %s PER-ROW", mname[mi]);
        printf("%-22s %10.4f %10.4f %+9.4f %8s %8.4f %9.4f\n",
               lbl, v, v / log(2.0), v - base, "~4.0", dl, rr);
        memcpy(blob, pristine, mlen);
    }

    lm_close(&m);
    free(pristine); free(pk); free(sc); free(mn); free(q8); free(s8); free(ref);
    free(lref); free(blob); free(corp);
    return 0;
}

/* ----------------------------------------------------------------- bench --
 *
 * WHY A BENCH AT ALL: nn.h's argument for quantising weights is that a
 * decode-time matvec reads the whole matrix once and does two operations per
 * byte, so it is bound by the READ and not by the multiplier. That predicts q4
 * beats q8 beats f32 in roughly the ratio of their bytes. It is a prediction,
 * so it is measured.
 *
 * THE FIRST VERSION OF THIS BENCH WAS WRONG AND ITS NUMBERS WERE REPORTABLE,
 * which is the reason it is written out at this length. It fixed the repeat
 * count from the f32 byte count, so the largest case ran the kernel ONCE, with
 * every page fault of a fresh 1 GB allocation inside the timing; and it took a
 * single sample, so the same shape read 1.92 and then 5.44 GMAC/s for f32 on
 * two consecutive runs. Both numbers looked perfectly plausible. What is here
 * now CALIBRATES to at least 0.2 s of CPU time per case and takes the BEST of
 * three rounds -- best, not mean, because every source of noise on a shared
 * machine only ever makes a run slower.
 *
 * IT MEASURES THIS HOST, WHICH IS NOT THE TARGET. The device is QEMU TCG at a
 * measured 211 MFLOP/s with 512 MiB of RAM and weights that will arrive from
 * disk through the page cache. Nothing below should be read as a statement
 * about it. */
struct bctx {
    int kind;                       /* 0 f32, 1 q8, 2 q4 */
    float *y, *w, *x, *s8, *sc, *mn, *xs;
    int8_t *q8;
    uint8_t *pk;
    int n, k;
};

static void bcall(struct bctx *c)
{
    if (c->kind == 0)      nn_matvec_f32(c->y, c->w, c->x, c->n, c->k);
    else if (c->kind == 1) nn_matvec_q8(c->y, c->q8, c->s8, c->x, c->n, c->k);
    else q4_matvec(c->y, c->pk, c->sc, c->mn, c->x, c->n, c->k,
                   Q4_BLOCK, Q4_MODE, c->xs);
}

/* Seconds per matvec, best of three, after calibrating the repeat count so a
 * round is at least 0.2 s -- so clock()'s resolution is never the measurement
 * and a first-touch page fault is amortised over the whole round. */
static double bench_kind(struct bctx *c)
{
    int rep = 1;
    double t = 0.0;
    for (;;) {
        clock_t a = clock();
        for (int r = 0; r < rep; r++) bcall(c);
        t = (double)(clock() - a) / CLOCKS_PER_SEC;
        if (t >= 0.2 || rep >= (1 << 22)) break;
        rep = rep * 2 + 1;
    }
    double best = t / rep;
    for (int round = 0; round < 2; round++) {
        clock_t a = clock();
        for (int r = 0; r < rep; r++) bcall(c);
        double tt = (double)(clock() - a) / CLOCKS_PER_SEC / rep;
        if (tt < best) best = tt;
    }
    return best;
}

static void bench_one(const char *what, int n, int k)
{
    size_t nel = (size_t)n * k;
    int nb = q4_blocks(k, Q4_BLOCK);
    struct bctx c;
    memset(&c, 0, sizeof c);
    c.n = n; c.k = k;
    c.w  = (float *)malloc(nel * sizeof(float));
    c.x  = (float *)malloc((size_t)k * sizeof(float));
    c.y  = (float *)malloc((size_t)n * sizeof(float));
    c.q8 = (int8_t *)malloc(nel);
    c.s8 = (float *)malloc((size_t)n * sizeof(float));
    c.pk = (uint8_t *)malloc(q4_payload_bytes(n, k, Q4_BLOCK));
    c.sc = (float *)malloc((size_t)n * nb * sizeof(float));
    c.mn = (float *)malloc((size_t)n * nb * sizeof(float));
    c.xs = (float *)malloc((size_t)nb * sizeof(float));
    if (!c.w || !c.x || !c.y || !c.q8 || !c.s8 || !c.pk || !c.sc || !c.mn || !c.xs) {
        printf("%-18s %6d x %-6d  SKIPPED -- out of memory, so no number is\n"
               "                             reported rather than a slow one\n",
               what, n, k);
        free(c.w); free(c.x); free(c.y); free(c.q8); free(c.s8);
        free(c.pk); free(c.sc); free(c.mn); free(c.xs);
        return;
    }
    for (size_t t = 0; t < nel; t++) c.w[t] = frand();
    for (int j = 0; j < k; j++) c.x[j] = frand();
    nn_quantize_q8(c.q8, c.s8, c.w, n, k);
    q4_quantize(c.pk, c.sc, c.mn, c.w, n, k, Q4_BLOCK, Q4_MODE);
    q4_xsum(c.xs, c.x, k, Q4_BLOCK);

    c.kind = 0; double tf = bench_kind(&c);
    c.kind = 1; double t8 = bench_kind(&c);
    c.kind = 2; double t4 = bench_kind(&c);
    double mac = (double)nel;

    printf("%-18s %5dx%-5d %5.0f/%4.0f/%4.0f KiB  %5.2f %5.2f %5.2f GMAC/s"
           "  q4/f32 %.2fx q4/q8 %.2fx\n",
           what, n, k,
           (double)(nel * 4) / 1024.0, (double)nel / 1024.0,
           (double)q4_bytes(n, k, Q4_BLOCK, Q4_MODE) / 1024.0,
           mac / tf / 1e9, mac / t8 / 1e9, mac / t4 / 1e9,
           tf / t4, t8 / t4);

    free(c.w); free(c.x); free(c.y); free(c.q8); free(c.s8);
    free(c.pk); free(c.sc); free(c.mn); free(c.xs);
}

static void bench(void)
{
    printf("-- matvec throughput. Q4_BLOCK %d, mode %s. Best of 3, each round\n"
           "   calibrated to >=0.2 s of CPU time. The three sizes printed are\n"
           "   what each kernel READS, which is the quantity in question.\n",
           Q4_BLOCK, Q4_MODE == Q4_AFFINE ? "affine" : "sym");
    printf("%-18s %5s %5s %-22s %5s %5s %5s\n",
           "", "n", "k", "f32/q8/q4 read", "f32", "q8", "q4");
    bench_one("wq  [q_dim,dim]", 2048, 1024);
    bench_one("wo  [dim,q_dim]", 1024, 2048);
    bench_one("w1  [hidden,dim]", 3072, 1024);
    bench_one("w2  [dim,hidden]", 1024, 3072);
    bench_one("head[vocab,dim]", 8192, 1024);
    bench_one("this tree w2", 128, 344);

    /* THE CONTROL FOR WHATEVER THE ROWS ABOVE SAY, not a sixth data point.
     *
     * If q4 comes out slower than q8 there, the obvious explanation is that
     * nn.h's premise has stopped holding: at 2 M weights a q8 matrix is 2 MB
     * and a q4 one is 1 MB, both cache-resident on this host, so the read is
     * no longer the cost and what is left is the unpack arithmetic -- of which
     * q4 does strictly more per weight than q8. That explanation makes a
     * PREDICTION: as the matrix grows past every cache, the q4/q8 ratio must
     * rise toward 1. Here is the case that tests it. If the ratio does not
     * move, the explanation is wrong and the kernel is simply slow -- which is
     * a different finding and a different fix. */
    printf("   -- the control: a q8 matrix far past any cache on this host\n");
    bench_one("out-of-cache", 16384, 2048);
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    const char *mp = 0, *cp = 0; int sweep = 0, full = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) mp = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) cp = argv[++i];
        else if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else if (!strcmp(argv[i], "--full")) full = 1;
        else if (!strcmp(argv[i], "--bench")) { bench(); return 0; }
    }
    if (mp && cp) return e2e(mp, cp, sweep, full);
    if (mp || sweep || full) {
        printf("quant4_test: --model and --corpus must be given together\n");
        return 1;
    }

    t_sizes();
    t_packing();
    t_layout();
    /* Both modes at the SHIPPED block size and at one that is not it, so a bug
     * that only appears when nb changes -- the short tail block moving, above
     * all -- has somewhere to be found. 344 is a multiple of neither 64 nor
     * 32, so the two runs exercise short final blocks of different lengths. */
    t_recon(Q4_SYM, Q4_BLOCK, "sym");
    t_recon(Q4_AFFINE, Q4_BLOCK, "affine");
    t_recon(Q4_SYM, 32, "sym");
    t_recon(Q4_AFFINE, 32, "affine");
    t_matvec(Q4_SYM, Q4_BLOCK, "sym");
    t_matvec(Q4_AFFINE, Q4_BLOCK, "affine");
    t_matvec(Q4_SYM, 32, "sym");
    t_matvec(Q4_AFFINE, 32, "affine");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
