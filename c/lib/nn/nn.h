/* c/lib/nn -- tensors and the kernels an inference pass is made of.
 *
 * RING 3, and filtered out of C_SRC on purpose, for the reason c/lib/video
 * gives: this allocates, it holds tens of megabytes of live weights, and a
 * matmul in ring 0 would hold the BKL for the length of a layer. The kernel's
 * job here is memory and files, not arithmetic.
 *
 * ---------------------------------------------------------------------------
 * WHAT DECIDES THE SHAPE OF THIS FILE -- three measured facts about THIS
 * machine, not three preferences:
 *
 *   512 MiB of RAM, and no swap unless a harness attaches one. So the weights
 *   are the budget: f32 is 4 bytes a parameter, which puts a 100 M-parameter
 *   model at 400 MB and out of reach before a single activation is allocated.
 *   Quantised weights are not an optimisation here, they are the difference
 *   between running and not running. Hence NN_Q8 below, in the same header as
 *   f32 rather than bolted on later.
 *
 *   SSE2 and nothing above it. The kernel enables SSE at boot (M15) and both
 *   halves build with -msse -msse2; there is no AVX, no FMA, and no runtime
 *   dispatch to pretend otherwise. Four f32 lanes is the whole machine.
 *
 *   256 INODES on a 64 MiB filesystem, 223 of them already used. A model is
 *   therefore ONE FILE. Any format that ships a directory of shards cannot be
 *   installed on this machine at all -- which is a real constraint on the
 *   format, decided here rather than discovered later.
 *
 * ---------------------------------------------------------------------------
 * THE ONE OPERATION. A transformer's forward pass is >95% matmul, and during
 * single-token decode every one of those matmuls has an M of 1 -- it is a
 * matrix times a VECTOR, which is memory-bound rather than compute-bound. So
 * `nn_matvec` is not a special case of `nn_matmul` here; it is the primary
 * entry point and matmul is the batched one. Writing it the other way round
 * optimises the case this machine spends almost none of its time in.
 *
 * ---------------------------------------------------------------------------
 * ACCUMULATION IS f32, AND THE GATE IS f64. Every kernel here accumulates in
 * float, because that is what the hardware does in one instruction and what
 * every production runtime does. The REFERENCE the tests compare against
 * accumulates in double, independently written, so the gate measures our
 * rounding against exact arithmetic instead of against a copy of ourselves.
 * The bound is stated per test rather than hidden in an epsilon: a dot product
 * of length k in f32 carries a relative error of order k*2^-24, and a test
 * that allows more than that is not measuring anything.
 */
#ifndef LOGIT_NN_H
#define LOGIT_NN_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- dtypes --
 *
 * Q8 is SYMMETRIC and PER-ROW: one f32 scale for each row of a weight matrix,
 * values in -127..127, zero always maps to 0. Per-row and not per-tensor
 * because a transformer's weight rows differ in magnitude by more than an
 * order of magnitude and a single tensor scale spends most of the int8 range
 * on the largest row; per-row costs 4 bytes a row, which against a row of
 * thousands of values is free. Symmetric and not affine because a zero point
 * turns the inner loop's accumulation into two terms, and the second one is a
 * sum of the activations that has to be recomputed per row -- for the accuracy
 * it buys on weights (which are near-zero-centred by construction) that is a
 * bad trade, and it is one this file therefore does not offer. */
enum { NN_F32 = 0, NN_Q8 = 1 };

/* A tensor is a shape and a pointer, and it does NOT own its data unless it
 * was made by nn_new. That split is deliberate: the weights of a model are
 * going to arrive as one read-only mapping of one file, and a tensor that
 * insisted on owning its bytes could not describe them. `own` says which. */
#define NN_MAXDIM 4

struct nn_tensor {
    int      dtype;
    int      ndim;
    int      dim[NN_MAXDIM];      /* dim[0] is the outermost */
    float   *data;                /* NN_F32: n elements. NN_Q8: unused */
    int8_t  *q;                   /* NN_Q8: n int8 values, row-major */
    float   *scale;               /* NN_Q8: one per row (dim[0] of a 2-D) */
    int      own;                 /* nn_free releases data/q/scale iff set */
};

/* Element count. Returns 0 for a tensor with no dimensions, which is also
 * what an all-zero struct answers -- so an uninitialised tensor reads as
 * empty rather than as a huge one. */
size_t nn_numel(const struct nn_tensor *t);
/* Bytes the payload occupies, so a caller can budget before allocating. */
size_t nn_bytes(const struct nn_tensor *t);

/* Allocate an owned tensor of the given shape. ndim <= NN_MAXDIM, every dim
 * positive. Returns 0 on refusal (bad shape, or the allocator said no) and
 * leaves *out zeroed -- never a half-built tensor, because the caller's error
 * path would then have to know which half. */
int  nn_new(struct nn_tensor *out, int dtype, int ndim, const int *dim);
void nn_free(struct nn_tensor *t);

/* Describe borrowed memory as a tensor. No copy, no ownership. This is the
 * entry point a mapped weight file uses. */
void nn_wrap_f32(struct nn_tensor *out, float *data, int ndim, const int *dim);
void nn_wrap_q8(struct nn_tensor *out, int8_t *q, float *scale,
                int ndim, const int *dim);

/* ---------------------------------------------------------------- kernels --
 *
 * All of these are pure functions over borrowed buffers: they allocate
 * nothing, so an inference loop can run with a fixed working set and a caller
 * can place every activation where it wants them. That is not austerity, it
 * is what makes the peak memory of a forward pass a number you can compute
 * before you run it -- which on a 512 MiB machine is the only way to know
 * whether a model fits. */

/* y[n] = W[n,k] . x[k]. The decode-time workhorse. */
void nn_matvec_f32(float *y, const float *w, const float *x, int n, int k);
/* The same, with W quantised per row: y[i] = scale[i] * sum_j q[i*k+j] * x[j].
 * The scale is applied ONCE per row, after the accumulation -- not per
 * element, which would be both slower and less accurate.
 *
 * That accumulation is in FLOAT, not in integer. This comment said "after an
 * integer accumulation" for as long as the function has existed and the loop
 * has never done one; matmul.c argues at the loop why it should not (an int32
 * sum needs the ACTIVATIONS quantised too, and the error of that second
 * quantisation lands on a test bound derived from the weights alone). The
 * sentence is corrected here rather than left standing, because a reader
 * optimising this kernel would start from a property it does not have. */
void nn_matvec_q8(float *y, const int8_t *q, const float *scale,
                  const float *x, int n, int k);

/* C[m,n] = A[m,k] . B[k,n], all row-major. The prefill path. */
void nn_matmul_f32(float *c, const float *a, const float *b, int m, int k, int n);

/* Quantise a row-major f32 matrix W[n,k] into q/scale, symmetric per row.
 * `scale[i]` is max|W[i,:]| / 127, and a row that is entirely zero gets a
 * scale of 0 rather than a division by zero -- which reconstructs exactly,
 * since every value in it is 0. */
void nn_quantize_q8(int8_t *q, float *scale, const float *w, int n, int k);
/* Reconstruct, for the tests and for a caller that wants to see the error it
 * is accepting rather than infer it. */
void nn_dequantize_q8(float *w, const int8_t *q, const float *scale,
                      int n, int k);

/* ------------------------------------------------------- transformer ops --
 *
 * The set below is what a llama-shaped decoder needs and nothing else. Each
 * is here because a forward pass calls it, and an op no forward pass calls is
 * an op with no reference and no reason. */

/* RMS norm: y[i] = x[i] * g[i] / sqrt(mean(x^2) + eps). No mean subtraction --
 * that is LayerNorm, a different op, and calling this one by that name is how
 * a port ends up subtracting a mean the reference never subtracted. */
void nn_rmsnorm(float *y, const float *x, const float *g, int n, float eps);

/* Softmax in place, over n contiguous values. Shifted by the max before the
 * exponential: without it a logit of 100 overflows f32 and the whole row
 * becomes NaN, which propagates silently through the attention it feeds. */
void nn_softmax(float *x, int n);

/* SiLU (x * sigmoid(x)), elementwise, and the SwiGLU pairing a llama MLP
 * wants: out[i] = silu(a[i]) * b[i]. */
void nn_silu(float *x, int n);
void nn_swiglu(float *out, const float *a, const float *b, int n);

/* THE PAIRING IS AN ARGUMENT NOW, AND IT USED TO BE A CONSTANT. This comment
 * used to end "the convention is named here rather than left to be discovered
 * by a model that outputs plausible nonsense", which was the right instinct
 * and the wrong conclusion: naming one convention in a header does not make a
 * model use it, and the model this line is aimed at uses the other one.
 *
 *   NN_ROPE_INTERLEAVED  rotates (x[2i], x[2i+1]) -- the RoFormer paper's own
 *                        presentation, llama2.c, and llama.cpp's "NORM" type.
 *   NN_ROPE_NEOX         rotates (x[i], x[i + n/2]) -- GPT-NeoX, every
 *                        huggingface `rotate_half` implementation, and
 *                        llama.cpp's "NEOX" type, which is what Qwen3 is.
 *
 * BOTH ARE VALID ROTARY EMBEDDINGS. Each preserves every pair's length, so
 * every property a test could check about "is it a rotation" holds for both,
 * and a model run under the wrong one produces finite logits and fluent,
 * confident, wrong text. That is why it is a parameter with no default rather
 * than a compile-time choice: a caller has to say which model it is holding.
 *
 * MEASURED, not asserted (build/qwen3.lm, layer 0, position 1, against
 * transformers' own apply_rotary_pos_emb on identical inputs):
 *     NEOX          max|d| 6.7e-07  over a q scale of 28.79
 *     interleaved   max|d| 11.31    over the same
 * Same weights, same angles, one line of pairing apart. */
#define NN_ROPE_INTERLEAVED 0
#define NN_ROPE_NEOX        1
void nn_rope(float *x, int n, int pos, float theta, int pairing);

/* THE SAME ROTATION, HOISTED. nn_rope's angles depend on (i, n, pos, theta)
 * and not on x, and a decode step calls it once per query head and once per kv
 * head in every layer -- all at one pos, one n, one theta. At the reference
 * shape that is 32 calls a token recomputing sixteen angles, 1,536 double
 * transcendentals for 16 distinct answers, against 823,296 multiply-adds.
 *
 * `nn_rope_build` computes one position's rotation into `cs` -- 2*(n/2)
 * DOUBLES, cos and sin interleaved -- and `nn_rope_apply` rotates with it.
 * ops.c routes all three entry points through one angle function and one
 * pairing function, so the hoisted path cannot drift from nn_rope, which stays
 * as the direct computing version test-nn gates against its double reference.
 *
 * cs is double and not float DELIBERATELY: the rotation multiplies in double
 * and rounds once, so an f32 table would round twice and move every rotated
 * value in its last bit -- a change in the model's generated bytes, bought for
 * 128 bytes at head_dim 32. See ops.c. */
void nn_rope_build(double *cs, int n, int pos, float theta);
void nn_rope_apply(float *x, int n, const double *cs, int pairing);

/* Elementwise add, y += x. */
void nn_add(float *y, const float *x, int n);

#endif /* LOGIT_NN_H */
