/* quant4.h -- four bits a weight, and the block that makes four bits usable.
 *
 * WHY THIS EXISTS, in one measurement. The target is a 0.6B-class model
 * (Qwen3-0.6B's shape: 28 layers, dim 1024, 16 Q heads over 8 KV heads,
 * head_dim 128, intermediate 3072, vocab 151936, tied). Its weights cost:
 *
 *     f32   2274 MiB    impossible on a 512 MiB machine
 *     q8     568 MiB    LARGER THAN RAM. Still impossible.
 *     q4     284 MiB    fits on disk, and can be paged
 *
 * So q4 is not an optimisation of q8, it is the first format in which that
 * model exists at all -- which is the same argument nn.h makes for q8 one
 * size down, and it is why this is a header beside nn.h rather than a switch
 * inside it. (The weights still do not fit in RAM alongside anything; they are
 * meant to be MAPPED. That work is a separate line. This file's job is to make
 * the format and the kernel ready for it.)
 *
 * ===========================================================================
 * EVERY NUMBER BELOW WAS MEASURED ON 2026-08-20 AND THE COMMAND IS AT THE
 * BOTTOM OF THIS FILE. Two independently trained 824k-parameter models
 * (build/model.lm, 3000 steps; build/model8k.lm, 8000 steps) were quantised
 * from the SAME f32 weights and scored on the SAME bytes of the SAME corpus.
 * Nothing here is quoted from llama.cpp or from memory.
 *
 * ---------------------------------------------------------------------------
 * 1. PER BLOCK, NOT PER ROW -- and the per-row number is in the table.
 *
 * nn.h's q8 is per-ROW symmetric, and that is right at eight bits: 255 levels
 * across one row of a transformer weight matrix leaves the tail of the
 * distribution well resolved. At FOUR bits there are fifteen, and a row of
 * 1024 whose magnitude varies along its length spends most of them on the
 * loud part. Measured, as `d(nats)` = the cross-entropy a model loses against
 * its own f32 weights (full corpus, 126,111 bytes, both models):
 *
 *              model.lm   model8k.lm
 *   blk   32    +0.0066     +0.0313
 *   blk   64    +0.0094     +0.0323
 *   blk  128    +0.0133     +0.0383
 *   PER ROW     +0.0138     +0.0405     <- 2.1x and 1.3x the blk-32 damage
 *
 * A row scale costs 2.1x the damage of a 32-block on the model that
 * generalises, for 1.0 bit/weight less. It is not offered.
 *
 * THE BLOCK NEVER CROSSES A ROW. Flattening the tensor and blocking the
 * result would be simpler and would put one block astride two rows whose
 * magnitudes differ -- reintroducing, once per row boundary, exactly the
 * failure per-block exists to prevent. The cost of not doing it is that a row
 * whose length is not a multiple of the block ends in a SHORT block; that case
 * is real (this tree's own w2 is [dim, 344], and 344 = 10*32 + 24) and is
 * handled rather than forbidden.
 *
 * ---------------------------------------------------------------------------
 * 2. AFFINE, NOT SYMMETRIC -- and this is the choice the measurement REVERSED.
 *
 * Symmetric costs one number a block and spends one of its sixteen codes on
 * the symmetry (q in -7..7, so the stored nibble is 1..15 and 0 never occurs;
 * tests/unit/quant4_test.c asserts that about the BYTES). Affine costs two
 * numbers a block and spends none. At eight bits nn.h argues symmetric is the
 * better trade because weights are near-zero-centred by construction, and that
 * argument was expected to carry over. It does not.
 *
 * The comparison has to be at EQUAL STORAGE or it is not a comparison:
 * affine at block B costs the same bytes as symmetric at block B/2. Ladder,
 * `d(nats)`, full corpus, both models:
 *
 *   bits/wt   symmetric        affine           winner
 *   8.0       blk 8  +0.0045   blk 16 +0.0031   affine  (model.lm)
 *                    +0.0159          +0.0120   affine  (model8k)
 *   6.0       blk 16 +0.0046   blk 32 +0.0061   sym
 *                    +0.0224          +0.0185   affine
 *   5.0       blk 32 +0.0066   blk 64 +0.0055   affine
 *                    +0.0313          +0.0246   affine
 *   4.5       blk 64 +0.0094   blk128 +0.0056   affine
 *                    +0.0323          +0.0325   tie
 *
 * Affine wins or ties seven of eight rungs, by about 20% at the shipped point.
 * THE SECOND NUMBER PER BLOCK PAYS FOR ITSELF, which is not what q8's
 * reasoning predicted, and the reason is the level count: at eight bits the
 * wasted code is 1 in 256 and the asymmetry of a block is absorbed by the
 * resolution; at four bits it is 1 in 16.
 *
 * AND IT COSTS NOTHING IN THE INNER LOOP, which is the fact that makes the
 * trade one-sided. nn.h rejects a zero point for q8 because "the second term
 * is a sum of the activations that has to be recomputed per row". That is true
 * of a per-ROW zero point and false here, for two reasons: the sum is over a
 * BLOCK and depends only on x and the geometry, so it is computed ONCE per
 * matvec (q4_xsum) and shared by every row; and SYMMETRIC NEEDS THE SAME SUM
 * ANYWAY -- the loop accumulates raw nibbles 0..15 and the bias comes off as
 * `- 8*sum(x)`. See the epilogue in quant4.c: both modes are two flops a
 * block, and the inner loop has no mode branch at all.
 *
 * Affine is also the numerically better of the two here, measured rather than
 * argued: symmetric's `raw - 8*xsum` is a cancellation of two comparable
 * positive quantities and affine's `s*raw + lo*xsum` is not. Worst absolute
 * error of q4_matvec against an exact double dot of the dequantised weights,
 * same data: symmetric 3.56e-06, affine 2.16e-06.
 *
 * SYMMETRIC IS KEPT, and not as a courtesy: it is what the ladder above is
 * measured AGAINST, the sweep that produced it is a shipped gate, and a
 * rejected alternative with no runnable comparison behind it is a remembered
 * number. It is also the only mode that needs one array, which a caller
 * placing tensors by hand may care about.
 *
 * ---------------------------------------------------------------------------
 * 3. BLOCK 64. Not 32, and the reason is the ladder above rather than a habit.
 *
 * The shipped pair is (Q4_AFFINE, 64) = 4 + 64/64 = **5.03 bits/weight**, and
 * at ~5 bits/weight it is the best of everything measured, on both models.
 * Block 32 affine would be 6.01 bits/weight -- 448 MB for the 0.6B target
 * against 375 MB -- for +0.0006/+0.0061 nats, which is not what 73 MB buys on
 * a machine with 512 MiB of RAM.
 *
 * WHAT q4 COSTS, END TO END, which is the number that decides whether any of
 * this is usable. Held-out tenth of the corpus, 12,611 bytes, `nats/byte`:
 *
 *                       model.lm            model8k.lm
 *   f32                 1.8929              2.0229
 *   q8 per-row          +0.0002             +0.0004
 *   q4 affine blk 64    +0.0046             +0.0163
 *
 * The bar this line was given was 0.05 nats/byte. q4 costs a tenth of that on
 * the model that generalises and a third of it on the deliberately overfitted
 * one -- which is the expected direction: model8k.lm has memorised 113 KB of
 * text into 824k parameters, so its weights carry more information per weight
 * and lose more when they are rounded. Both are reported because quoting only
 * the friendly one would be choosing the model to fit the claim.
 *
 * ---------------------------------------------------------------------------
 * 4. THE PACKING: WHICH NIBBLE IS WHICH.
 *
 * A block of L weights occupies ceil(L/2) bytes. With h = (L+1)/2:
 *
 *     byte i   low nibble  = weight i          (i < h)
 *              high nibble = weight i + h      (i + h < L)
 *
 * so the FIRST HALF of the block is in the low nibbles and the SECOND HALF in
 * the high nibbles -- NOT consecutive pairs. That is the whole reason for the
 * split and it is about SSE2, not about aesthetics:
 *
 *   - halves: the four low nibbles of four consecutive bytes multiply x[q..q+3]
 *     and the four high nibbles multiply x[q+h..q+h+3]. Both are plain
 *     sequential 16-byte loads.
 *   - pairs: lane j would need x[2j] and x[2j+1] in separate registers, i.e. a
 *     deinterleave of the activation vector once per four weights, on a machine
 *     whose entire vector instruction set is SSE2.
 *
 * MEASURED, not assumed -- `clang -O2 -S` on this file emits, per four bytes
 * (eight weights) of q4_matvec's inner loop:
 *
 *     movd / pand / psrlw / punpcklbw / punpcklwd / cvtdq2ps  (x2, lo and hi)
 *     movups / mulps / addps                                  (x2)
 *
 * Pure SSE2, no AVX, no shuffle overhead in the reduction. matmul.c measured
 * this week that `float s0,s1,s2,s3` compiles to two 2-lane vectors and four
 * wasted shufps, and that a single `float s` does not vectorise at all (a
 * serial f32 reduction may not be reassociated without -ffast-math), so the
 * accumulator here is stated as ONE SSE2 register in the GNU vector spelling.
 *
 * Symmetric stores q+8 (biased), so the reconstruction is (nibble - 8) * scale
 * and the unpack is an AND or a SHIFT and nothing else. Affine stores q
 * directly in 0..15. Both make the inner loop identical -- see quant4.c.
 *
 * The odd tail byte's unused high nibble is written ZERO, so two writers
 * produce the same file from the same weights.
 *
 * WHAT THAT LOOP COSTS, AND A PREMISE IT REFUTES. `--bench`, best of three
 * rounds each calibrated to >=0.2 s of CPU time, on the host (NOT the device
 * -- QEMU TCG measures 211 MFLOP/s and will read these weights off a disk):
 *
 *   shape           f32 read   q8     q4     f32    q8     q4    q4/q8
 *   2048 x 1024      8192 KiB  2048   1280   5.45   5.56   2.93   0.53
 *   3072 x 1024     12288      3072   1920   3.84   6.07   2.91   0.48
 *   8192 x 1024     32768      8192   5120   2.77   6.02   3.40   0.56
 *   16384 x 2048   131072     32768  20480   2.67   5.21   2.53   0.49   GMAC/s
 *
 * q4 is about HALF q8's decode throughput, at every size from 43 KiB to
 * 32 MiB of q8 weights, while reading 40% fewer bytes. nn.h says a matvec
 * "does two operations per byte, so it is bound by memory bandwidth and not by
 * the multiplier"; on this host that is false for the quantised paths -- q8
 * holds ~6 GMAC/s whether its matrix is 43 KiB or 32 MiB, i.e. ~6 GB/s against
 * a DRAM that does several times that. Only f32 approaches bandwidth, and only
 * at the large shapes (131 MiB at 2.67 GMAC/s = 10.7 GB/s), which is why q4
 * beats f32 there (1.23x) and loses to it on a matrix that fits in L1.
 *
 * THE LAST ROW IS A CONTROL AND IT IS THE REASON THAT PARAGRAPH IS PHRASED
 * THAT WAY. The obvious explanation for q4 < q8 was cache residency: a 2 MB q8
 * matrix and a 1 MB q4 one both fit, so the read is not the cost and only the
 * unpack arithmetic is left. That predicts the ratio RISES as the matrix grows
 * past cache. It does not -- 0.53, 0.48, 0.56, 0.49 across a 750x range of
 * sizes, flat. The explanation was wrong and the flatness is the finding: this
 * kernel is compute-bound on this host at every size, so q4's advantage over
 * q8 is STORAGE and not speed. Which is the whole reason it exists: the 0.6B
 * target does not fit in q8 at all.
 *
 * REJECTED, WITH ITS NUMBER: unrolling the inner loop to eight bytes (sixteen
 * weights) an iteration with four accumulators measured q4/q8 at 0.55, 0.54,
 * 0.52 against 0.53, 0.56, 0.49 -- inside the run-to-run spread of the ratio.
 * It is not taken; twice the loop body for no measurable gain is a worse file.
 * The headroom that IS visible in the assembly is the 4-byte `movd` load,
 * which fills a quarter of a register; a 16-byte `movdqu` masked into low and
 * high nibbles would cut the load and mask count fourfold. That is a rewrite
 * against intrinsics rather than a shape plain C is likely to be given, so it
 * is named here rather than attempted.
 *
 * ---------------------------------------------------------------------------
 * 5. SCALES SEPARATE FROM THE PAYLOAD, f32, and the 0.5 bits that leaves.
 *
 * A tensor is [all packed nibbles][all scales][all minima], not an array of
 * {scale, 16 bytes} structs. Interleaving would put a block's scale in the
 * same cache line as its nibbles, which is llama.cpp's layout and is a real
 * advantage on a hand-written intrinsics kernel; here it would give the packed
 * array an 18-byte stride and destroy the alignment and contiguity the
 * compiler needs to emit the loop quoted above. Three sequential streams that
 * a prefetcher handles beat one stream that does not vectorise.
 *
 * THE METADATA IS f32 AND THAT IS THE ONE CHOICE HERE MEASURED TO BE WRONG,
 * recorded rather than quietly taken. Rounding every scale and minimum onto
 * the IEEE binary16 grid costs, on the same two models:
 *
 *   dlogit, affine blk 64:   f32 metadata 0.0328 / 0.0679
 *                            f16 metadata 0.0328 / 0.0679     -- identical
 *   d(nats):                 +0.0055 / +0.0246  ->  +0.0055 / +0.0246
 *
 * i.e. FREE to four decimal places, for 0.5 bits/weight -- 5.03 -> 4.51, or
 * 375 MB -> 336 MB on the 0.6B target. It is not taken here because it needs a
 * software half<->float converter (this machine has no F16C) with its own
 * rounding rules and its own gate, and because `struct nn_tensor.scale` is a
 * `float *` in a header three other files share. It is the next 0.5 bits and
 * it is cheap; it is named here with its number so nobody has to re-measure it.
 *
 * ---------------------------------------------------------------------------
 * 6. WHAT IS NOT HERE, on purpose.
 *
 * No search over a shrink factor on the scale. Scaling by 0.95*amax instead of
 * amax trades a little clipping for smaller steps and reliably lowers RMS
 * error by a few percent -- and it replaces a bound that holds BY CONSTRUCTION
 * (every weight within half a step, asserted per block) with a constant fitted
 * to a corpus. This file keeps the provable property.
 *
 * No second-level scale (llama.cpp's Q4_K quantises the block scales
 * themselves against a super-block). It is the right next step after f16
 * metadata and it is a different format, not a parameter of this one.
 *
 * No activation quantisation. The matvec dequantises the WEIGHT and multiplies
 * by an f32 activation, for the reason matmul.c gives for q8: an integer
 * accumulation needs the activations quantised too, which is a second
 * quantisation with its own scale and its own error on the one tensor that
 * changes every token.
 *
 * ===========================================================================
 * THE GATE
 *
 *   cc -Ic/lib/nn -O2 -w -o build/quant4_test tests/unit/quant4_test.c \
 *      c/lib/nn/quant4.c c/lib/nn/model.c c/lib/nn/infer.c \
 *      c/lib/nn/tensor.c c/lib/nn/matmul.c c/lib/nn/ops.c -lm
 *
 *   build/quant4_test                                  # 46 checks, 0 failed
 *   build/quant4_test --bench
 *   build/quant4_test --model build/model.lm --corpus CLAUDE.md
 *   build/quant4_test --model build/model.lm --corpus CLAUDE.md --sweep --full
 *
 * AND A SECOND GATE ON THE FORMAT SIDE, which the list above predates: q4 is
 * a LOGITLM dtype now (model.h), so a q4 tensor's placement inside a file is
 * checkable end to end and is checked by a different reader from the one
 * tested here.
 *
 *   build/lmshape --preset qwen3-0.6b --vocab 4096 --seq 256 --dtype q4 \
 *        --qemb --out build/q4.lm
 *   build/lmshape --preset qwen3-0.6b --vocab 4096 --seq 256 --dtype q4 \
 *        --qemb --verify build/q4.lm
 *
 * -> 310 tensors x {first, last} re-derived from the seed and compared against
 * what is at that offset, each against its own BLOCK's half-step bound, plus
 * two whole embedding rows through lm_embed_row (a second reader of the same
 * table, with its own stride and scale index). Measured on the shipped build:
 * worst |disk - seed| 0.003571 against bounds of the same order; the
 * lm_embed_row rows read worst 0.003564 against 0.003591.
 *
 * ITS CONTROL IS -DLM_Q4_EMB_HALF_STRIDE (in model.c) AND IT ONLY FIRES ON
 * SOME SHAPES, which is worth knowing before trusting it: the wrong stride is
 * `dim/2`, and that is EXACTLY RIGHT whenever dim is a whole number of blocks.
 * At the target shape (dim 1024) the control changes nothing and VERIFY OK is
 * printed by both builds. At `--dim 1023` -- the same file with an odd tail
 * block -- it reddens 951 of 2046 elements, worst 0.1072 against a bound of
 * 0.003574, while the 22-element tensor spot check above it stays green. Both
 * halves of that matter: the control fires, and it fires ONLY in the reader it
 * is aimed at.
 *
 * THE NEGATIVE CONTROL is a PLAUSIBLE WRONG q4 rather than a broken one: the
 * same quantiser, through the same parameter, handed the whole TENSOR's range
 * instead of the block's. It packs, it round-trips, every byte is a legal
 * nibble, and it produces a model that runs.
 *
 *   cc ... -DQ4_PER_TENSOR_SCALE ...                   # must FAIL: it does,
 *                                    reddening 11 checks and leaving 35 green
 *
 * WHAT SURVIVES IT IS THE POINT. The half-step bound passes -- all four
 * instances -- because that bound is expressed in the STORED scale, and every
 * weight really is within half a step of a scale chosen badly. So does every
 * q4_matvec check, correctly: the kernel is measured against the dequantised
 * weights and is not the thing that changed. So does the whole on-disk layout
 * group, correctly: nothing structural moved. What reddens is the bound
 * expressed in the BLOCKS' OWN extrema (4), the scale-spread check (4 -- the
 * control gives every block one identical scale, ratio exactly 1 against the
 * ~90x the data forces), affine's exact reconstruction of a pruned row (2),
 * and a one-weight tail block (1).
 *
 * THAT PER-BLOCK BOUND WAS PER-TENSOR FIRST AND THE CONTROL CAUGHT IT. Summed
 * over the tensor it is dominated by the loudest block -- the one a per-tensor
 * scale gets RIGHT -- so it reddened three of its four instances and passed
 * the fourth, on nothing but where the shared RNG had got to. Which is this
 * file's own warning about global bounds, made inside the test written to
 * avoid it. Per block it reddens all four. On a correct build the worst block
 * sits at 0.65-0.70 of its bound, against the 1/sqrt(3) = 0.577 that uniform
 * rounding error predicts -- so the bound is neither violated nor vacuous.
 *
 * End to end it is unmistakable, and the shape of the evidence is better than
 * the size of it: under the control, `blk 64` and `PER-ROW` print the SAME
 * numbers to four decimals -- +0.0181 nats, rms 0.2994 -- which is the
 * signature of a scale that does not depend on the block. The correct build
 * reads +0.0046 and 0.0994 for blk 64. That is 3.9x the damage.
 */
#ifndef LOGIT_NN_QUANT4_H
#define LOGIT_NN_QUANT4_H

#include <stddef.h>
#include <stdint.h>
#include "nn.h"

/* A dtype of its own rather than a flag beside NN_Q8, because `nn_tensor.q`
 * holds HALF as many bytes as the element count -- a reader that assumed one
 * int8 per weight walks off the end at the midpoint of every tensor. */
enum { NN_Q4 = 2 };

enum { Q4_SYM = 0, Q4_AFFINE = 1 };

/* The shipped pair, chosen by the ladder above: 4 + 2*32/64 = 5.03 bits per
 * weight. Both are carried in the model header rather than compiled in, so a
 * file says which it is and a reader cannot silently apply the wrong one. */
#define Q4_BLOCK      64
#define Q4_MODE       Q4_AFFINE
#define Q4_BLOCK_MAX  1024      /* bounds quant4.c's per-block stack buffers */

/* Geometry. All four return 0 on a shape or a mode this format cannot express,
 * rather than a plausible number -- a size that is wrong by a factor is worse
 * than a refusal, because the caller allocates against it. */
int    q4_blocks(int k, int blk);        /* blocks in ONE row of k weights */
size_t q4_row_bytes(int k, int blk);     /* packed nibble bytes in one row */
size_t q4_payload_bytes(int n, int k, int blk);
size_t q4_bytes(int n, int k, int blk, int mode);   /* payload + metadata */
int    q4_mode_ok(int mode);
int    q4_blk_ok(int blk);               /* even, 2..Q4_BLOCK_MAX */

/* Byte offset of the scale array WITHIN a tensor, i.e. the payload rounded up
 * to a multiple of 4.
 *
 * THE PADDING IS NOT COSMETIC AND THE FORMAT IS BROKEN WITHOUT IT. A row of k
 * weights occupies an ODD number of packed bytes whenever the final block is
 * odd -- k=33 at blk 32 gives 17 -- so `rows * row_bytes` is not generally a
 * multiple of four, and the f32 scales that follow it would start at an
 * unaligned address. In a file that is READ that is merely slow; in a file
 * that is MAPPED and whose scales are reached by casting a pointer, which is
 * the whole design of this format (model.h: "a loader does not read the file,
 * it points at it"), it is a fault on the first such tensor. Padding once per
 * tensor costs at most 3 bytes and also keeps the NEXT tensor aligned, which
 * an unpadded q4 tensor would silently break for everything after it.
 *
 * It is one pad per tensor rather than a rounded-up row_bytes because rounding
 * every row would cost 3 bytes of 17 on a narrow tensor -- 18% -- to solve a
 * problem that occurs once. */
size_t q4_scale_off(int n, int k, int blk);

/* Quantise a row-major f32 matrix W[n,k].
 *   packed  q4_payload_bytes(n,k,blk) bytes
 *   scale   n * q4_blocks(k,blk) floats
 *   minv    the same again, for Q4_AFFINE; may be NULL for Q4_SYM
 * A constant block (all-zero under Q4_SYM, hi==lo under Q4_AFFINE) gets a ZERO
 * scale and reconstructs EXACTLY -- there is no division by a zero range, for
 * the reason nn_quantize_q8 gives: a pruned or unused row is not exotic, and
 * an inf scale poisons a whole output channel silently. */
void q4_quantize(uint8_t *packed, float *scale, float *minv,
                 const float *w, int n, int k, int blk, int mode);

/* Reconstruct. For the tests, and for a caller that wants to SEE the error it
 * is accepting rather than infer it. */
void q4_dequantize(float *w, const uint8_t *packed, const float *scale,
                   const float *minv, int n, int k, int blk, int mode);

/* Per-block sums of the activation vector: q4_blocks(k,blk) floats.
 *
 * SEPARATE FROM q4_matvec, and required by BOTH modes, because it depends only
 * on x and the block geometry -- so an n-row matvec computes it once instead of
 * n times, and a caller running several matvecs against the SAME activations
 * (q, k and v of one layer all read the residual stream) computes it once for
 * all of them. It is the caller's buffer for the reason nn.h gives for every
 * kernel here: nothing in this file allocates, so the peak memory of a forward
 * pass is a number you can compute before you run it. */
void q4_xsum(float *xsum, const float *x, int k, int blk);

/* y[n] = W[n,k] . x[k], W quantised. `xsum` must be q4_xsum's output for this
 * x and this blk. The scale is applied ONCE per block, after an accumulation
 * of raw nibbles -- not per element. */
void q4_matvec(float *y, const uint8_t *packed, const float *scale,
               const float *minv, const float *x, int n, int k,
               int blk, int mode, const float *xsum);

/* Describe borrowed q4 memory as a tensor: `q` takes the packed nibbles,
 * `scale` the per-block scales, and `data` -- unused for a quantised tensor --
 * the per-block minima (NULL under Q4_SYM). See quant4.c on why that reuse
 * rather than a fifth pointer in a struct three other files share. */
void q4_wrap(struct nn_tensor *out, uint8_t *packed, float *scale, float *minv,
             int ndim, const int *dim);

/* The two calls that carry NN_Q4 into a LOGITLM file. On-disk layout of one
 * q4 tensor, which is model.h's q8 layout with the pad q8 does not need:
 *
 *     [packed nibbles][pad to 4][rows*nblocks scales][rows*nblocks minima]
 *
 * the last present only under Q4_AFFINE. `q4_mat_bytes` returns 0 for a shape
 * this format cannot express -- which model.c maps to its own sz_bad(), so a
 * corrupt header is refused by the size walk before a byte is dereferenced.
 * See the comment above them in quant4.c for the exact patch. */
size_t q4_mat_bytes(uint32_t rows, uint32_t cols);
unsigned char *q4_wrap_mat(struct nn_tensor *out, unsigned char *p,
                           uint32_t rows, uint32_t cols);

/* y[n] = W[n,k] . x[k] for a tensor q4_wrap_mat produced: the shape check,
 * the per-block activation sums and q4_matvec, in one call that allocates
 * nothing. Returns 1, or 0 for a tensor this build cannot multiply -- which
 * is the same convention infer.c's `mv()` already uses, so the hookup there is
 * exactly one line:
 *
 *     if (w->dtype == NN_Q4) return q4_mv(y, w, x, n, k);
 *
 * See quant4.c for what putting `xsum` on this function's stack costs (under
 * 0.1% of the matvec it precedes) and why the alternative -- threading a
 * scratch buffer through lm_state -- was not worth a permanent arena field. */
int q4_mv(float *y, const struct nn_tensor *w, const float *x, int n, int k);

#endif /* LOGIT_NN_QUANT4_H */
