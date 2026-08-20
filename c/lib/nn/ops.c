/* ops.c -- the non-matmul kernels of a decoder-only transformer.
 *
 * ONE MEASURED CONSTRAINT SHAPES EVERY LINE HERE: this machine's libm is a
 * DOUBLE-ONLY SUBSET. `third_party/libm` ships exp.c, sqrt.c, sin.c, cos.c and
 * pow.c and no float variants at all -- `expf`, `sqrtf` and `cosf` do not
 * exist in a ring-3 link on this OS. So every transcendental below goes
 * through the double routine and casts back, and that is written down because
 * the alternative failure is silent and awful: `expf` compiles against a host
 * header during a host test, links against host glibc, passes, and then does
 * not link in the .aex -- or worse, resolves to something else.
 *
 * The cast is not a compromise in accuracy either. Computing a softmax's
 * exponential in double and rounding once to float is MORE accurate than an
 * f32 exponential, not less; what it costs is speed, and under TCG a double
 * op and a float op cost about the same anyway. Where that stops being true
 * the answer is to measure it and say so.
 */
#include <math.h>
#include "nn.h"

void nn_rmsnorm(float *y, const float *x, const float *g, int n, float eps)
{
    if (!y || !x || n <= 0) return;
    /* The sum of squares accumulates in DOUBLE. This is the one place in the
     * file where that is load-bearing rather than incidental: it is a sum of
     * n non-negative terms, so there is no cancellation to hide behind, and at
     * a hidden size of a few thousand an f32 accumulator loses the low bits of
     * every term after the first few hundred. RMS norm runs twice per layer
     * and its output scales EVERYTHING downstream, so a systematic error here
     * is a systematic error in the whole residual stream. */
#ifdef NN_RMS_SUBTRACT_MEAN
    /* NEGATIVE CONTROL (tests/nn.mk): this is LayerNorm, which is what RMS
     * norm becomes if you "remember" that a normalisation centres its input.
     * It is the single most common way to get this op wrong, and on
     * zero-mean data it is INDISTINGUISHABLE from the right answer -- which
     * is why the test feeds it a deliberately mean-3 input. */
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += (double)x[i];
    mean /= (double)n;
    double ss = 0.0;
    for (int i = 0; i < n; i++) { double d = (double)x[i] - mean; ss += d * d; }
    ss /= (double)n;
    ss += (double)eps;
    float inv = (float)(1.0 / sqrt(ss));
    if (g) for (int i = 0; i < n; i++) y[i] = g[i] * (float)(((double)x[i] - mean) * inv);
    else   for (int i = 0; i < n; i++) y[i] = (float)(((double)x[i] - mean) * inv);
#else
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * (double)x[i];
    ss /= (double)n;
    ss += (double)eps;
    float inv = (float)(1.0 / sqrt(ss));
    if (g) for (int i = 0; i < n; i++) y[i] = g[i] * (x[i] * inv);
    else   for (int i = 0; i < n; i++) y[i] = x[i] * inv;
#endif
}

void nn_softmax(float *x, int n)
{
    if (!x || n <= 0) return;
    /* SHIFT BY THE MAX FIRST. Attention logits reach the tens routinely and
     * expf(89) is already +inf in f32 -- one inf makes the whole row NaN after
     * the division, and a NaN in an attention row propagates through the rest
     * of the forward pass without ever producing an error, just wrong text.
     * The shift is mathematically a no-op (exp(a-m)/sum exp(a-m) is identical)
     * and is the difference between a model that works and one that does not. */
#ifdef NN_NO_SOFTMAX_SHIFT
    /* NEGATIVE CONTROL: the textbook formula, without the shift. It is
     * correct for every input a hand-written test is likely to use and wrong
     * for every input a real attention row produces. */
    float m = 0.0f;
#else
    float m = x[0];
    for (int i = 1; i < n; i++) if (x[i] > m) m = x[i];
#endif
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double e = exp((double)(x[i] - m));
        x[i] = (float)e;
        sum += e;
    }
    /* sum >= 1 always, because the max term contributes exp(0) = 1 -- so there
     * is no division-by-zero branch here and its absence is a consequence of
     * the shift above, not an oversight. */
    float inv = (float)(1.0 / sum);
    for (int i = 0; i < n; i++) x[i] *= inv;
}

void nn_silu(float *x, int n)
{
    if (!x || n <= 0) return;
    for (int i = 0; i < n; i++) {
        double v = (double)x[i];
        x[i] = (float)(v / (1.0 + exp(-v)));
    }
}

void nn_swiglu(float *out, const float *a, const float *b, int n)
{
    if (!out || !a || !b || n <= 0) return;
    /* silu(a) * b, in one pass, because that is how a llama MLP uses it and
     * splitting it into silu-then-multiply costs a second walk over the
     * largest activation in the layer (the intermediate size is several times
     * the hidden size). */
    for (int i = 0; i < n; i++) {
        double v = (double)a[i];
        out[i] = (float)(v / (1.0 + exp(-v))) * b[i];
    }
}

/* ------------------------------------------------------------------ rope --
 *
 * SPLIT INTO ANGLE AND ROTATION, and the reason is arithmetic rather than
 * tidiness. The angle for pair i depends on (i, n, pos, theta) and NOTHING
 * ELSE -- not on x. One decode step calls nn_rope once per query head and once
 * per kv head, in every layer, all at the same pos, the same n and the same
 * theta: at the reference shape (4 layers, 4+4 heads, head_dim 32) that is 32
 * calls a token recomputing the SAME sixteen angles, so
 *
 *     4 layers x 2 (q,k) x 4 heads x 16 pairs = 512 pow + 512 cos + 512 sin
 *
 * per token, against 823,296 multiply-adds -- 1,536 double transcendentals for
 * 16 distinct answers. Hoisting them is a 32x cut, and infer.c takes it.
 *
 * The two halves below are the ONE definition of each: nn_rope computes the
 * angles and rotates, nn_rope_build computes the angles, nn_rope_apply
 * rotates, and all three go through `rope_angle` and `rope_pair`. So the fast
 * path cannot drift from the reference path -- there is only one copy of the
 * frequency and one copy of the pairing, which matters because a rope bug does
 * not raise an error, it produces a fluent model that is wrong.
 *
 * THE TABLE IS DOUBLE, NOT FLOAT, and that is not caution -- it is what makes
 * the hoist free of consequences. The rotation below multiplies a double c by
 * a double x0 and rounds ONCE, to float. Storing c as float rounds twice, and
 * every rotated value moves in its last bit -- so the model's generated bytes
 * would change, and a performance change does not get to spend that. At
 * head_dim 32 the table is 16 pairs x 2 x 8 = 256 BYTES, so f32 would have
 * saved 128 bytes for the whole property. */

/* The angle of pair i. The ONE place this frequency is defined. */
static double rope_angle(int i, int n, int pos, float theta)
{
    double freq = 1.0 / pow((double)theta, (double)(2 * i) / (double)n);
    return (double)pos * freq;
}

/* Rotate pair i by (c,s). THE ONE PLACE THE PAIRING LIVES.
 *
 * `pairing` selects which two coordinates of the head make up pair i:
 * NN_ROPE_INTERLEAVED takes (x[2i], x[2i+1]), NN_ROPE_NEOX takes
 * (x[i], x[i+half]). nn.h argues why this is a model property rather than a
 * build-time one and carries the measurement.
 *
 * THE ROTATION ITSELF IS WRITTEN ONCE. Only the two INDICES depend on the
 * convention, so the arithmetic below -- and its double-precision multiply
 * that rounds exactly once -- cannot differ between the two paths. Writing
 * them as two branches with a rotation in each is how one of them gets a
 * rounding fix the other does not. */
static void rope_pair(float *x, int i, int half, double c, double s, int pairing)
{
#ifdef NN_ROPE_SPLIT_HALF
    /* NEGATIVE CONTROL: INVERT whatever the caller asked for. It used to
     * hard-select huggingface's pairing, which was a correct control while
     * interleaved was the only thing this file could do -- and would now be a
     * control that AGREES with the shipped behaviour on every Qwen3 model,
     * i.e. a control that cannot be watched failing on the one model the line
     * is aimed at. Inverting keeps it wrong for every caller whatever they
     * asked for, which is what a control has to be. */
    pairing = !pairing;
#endif
    int i0 = (pairing == NN_ROPE_NEOX) ? i        : 2 * i;
    int i1 = (pairing == NN_ROPE_NEOX) ? i + half : 2 * i + 1;
    float x0 = x[i0], x1 = x[i1];
    x[i0] = (float)((double)x0 * c - (double)x1 * s);
    x[i1] = (float)((double)x0 * s + (double)x1 * c);
}

void nn_rope(float *x, int n, int pos, float theta, int pairing)
{
    if (!x || n <= 1) return;
    int half = n / 2;
    for (int i = 0; i < half; i++) {
        double a = rope_angle(i, n, pos, theta);
        rope_pair(x, i, half, cos(a), sin(a), pairing);
    }
}

void nn_rope_build(double *cs, int n, int pos, float theta)
{
    if (!cs || n <= 1) return;
    int half = n / 2;
    for (int i = 0; i < half; i++) {
        double a = rope_angle(i, n, pos, theta);
        cs[2 * i]     = cos(a);
        cs[2 * i + 1] = sin(a);
    }
}

void nn_rope_apply(float *x, int n, const double *cs, int pairing)
{
    if (!x || !cs || n <= 1) return;
    int half = n / 2;
    for (int i = 0; i < half; i++)
        rope_pair(x, i, half, cs[2 * i], cs[2 * i + 1], pairing);
}

void nn_add(float *y, const float *x, int n)
{
    if (!y || !x || n <= 0) return;
    for (int i = 0; i < n; i++) y[i] += x[i];
}
