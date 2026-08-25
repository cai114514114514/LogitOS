/* c/lib/video/mpeg12_idct.c -- the 8x8 inverse DCT, written to be BIT-EXACT
 * with FFmpeg's `-idct simple`.
 *
 * WHY THIS ONE AND NOT "A CORRECT" IDCT. See the long note at the top of
 * mpeg12.c: 13818-2 specifies the IDCT only by IEEE 1180 accuracy, so two
 * conforming decoders legitimately differ by +/-1 per sample and, because P
 * and B pictures predict from the result, the difference grows over a GOP.
 * There is no bit-exact answer to compare against unless the transform is
 * pinned, so this file pins it to the one the oracle uses.
 *
 * WHAT THIS TRANSFORM IS. A separable 8-point integer IDCT with 14-bit
 * constants W_k = round(cos(k*pi/16) * sqrt(2) * 2^14), rows rounded at 11
 * bits and columns at 20. It is not transcribed from FFmpeg's source: the
 * butterfly below is the standard even/odd decomposition and every one of its
 * 32 coefficient positions was DERIVED from cos((2n+1)k*pi/16) and checked
 * against the table (tests/unit/mpeg12_idct_test.c re-derives all of them in
 * double and compares). What had to be taken from the published description
 * of that implementation, because it is a choice and not a consequence, is
 * three things -- and each is a place where a "cleaner" implementation is
 * observably different:
 *
 *   1. W4 is 16383, not 16384. The rounding constant of the COLUMN pass is
 *      therefore written W4 * ((1 << 19) / W4) = W4 * 32 = 524256, not
 *      1 << 19 = 524288. A 32-unit difference before a 20-bit shift changes a
 *      sample whenever the exact value lands within 32 of a rounding boundary.
 *
 *   2. The row pass has a DC-only shortcut that is NOT algebraically equal to
 *      the general path. (W4*x + 1024) >> 11 equals 8*x only while
 *      -1024 <= x <= 1024; the shortcut writes 8*x for every x. An intra DC
 *      coefficient reaches 2040 at 8-bit precision, so a flat macroblock --
 *      the most common block on any real picture -- takes the branch and comes
 *      out one step brighter than the general path would make it. Removing the
 *      shortcut as "an optimisation" changes the picture.
 *
 *   3. The row pass stores back into int16_t and is allowed to wrap there.
 *      The shortcut's `& 0xffff` is that wrap made explicit.
 *
 * Everything else here -- the skips of zero coefficients, the ordering -- is
 * exact and may be rearranged freely.
 *
 * -DMPEG12_CONTROL_NO_DC_SHORTCUT compiles OUT choice #2 above: the row pass
 * always takes the general path, on the theory (argued wrong at #2) that the
 * shortcut is "just" an optimisation. It is the negative control for
 * tests/mpeg12.mk -- the plausible wrong implementation, not a mutilation.
 */
#include <stdint.h>
#include "mpeg12_int.h"

#define W1 22725   /* cos(1*pi/16) * sqrt(2) * 2^14 + 0.5 */
#define W2 21407
#define W3 19266
#define W4 16383   /* cos(4*pi/16) * sqrt(2) * 2^14 = 16384, minus one */
#define W5 12873
#define W6 8867
#define W7 4520

#define ROW_SHIFT 11
#define COL_SHIFT 20

/* The column pass's rounding constant, written the way it is computed rather
 * than as the value, because the integer divide is the whole point. */
#define COL_ROUND_IN ((1 << (COL_SHIFT - 1)) / W4)

static void idct_row(int16_t *row)
{
    int a0, a1, a2, a3, b0, b1, b2, b3;
    int x0 = row[0], x1 = row[1], x2 = row[2], x3 = row[3];
    int x4 = row[4], x5 = row[5], x6 = row[6], x7 = row[7];

#ifndef MPEG12_CONTROL_NO_DC_SHORTCUT
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        int16_t t = (int16_t)(((unsigned)x0 << 3) & 0xFFFFu);
        row[0] = row[1] = row[2] = row[3] = t;
        row[4] = row[5] = row[6] = row[7] = t;
        return;
    }
#endif

    a0 = W4 * x0 + (1 << (ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;

    a0 += W2 * x2;  a1 += W6 * x2;  a2 -= W6 * x2;  a3 -= W2 * x2;
    a0 += W4 * x4;  a1 -= W4 * x4;  a2 -= W4 * x4;  a3 += W4 * x4;
    a0 += W6 * x6;  a1 -= W2 * x6;  a2 += W2 * x6;  a3 -= W6 * x6;

    b0 =  W1 * x1 + W3 * x3 + W5 * x5 + W7 * x7;
    b1 =  W3 * x1 - W7 * x3 - W1 * x5 - W5 * x7;
    b2 =  W5 * x1 - W1 * x3 + W7 * x5 + W3 * x7;
    b3 =  W7 * x1 - W5 * x3 + W3 * x5 - W1 * x7;

    row[0] = (int16_t)((a0 + b0) >> ROW_SHIFT);
    row[1] = (int16_t)((a1 + b1) >> ROW_SHIFT);
    row[2] = (int16_t)((a2 + b2) >> ROW_SHIFT);
    row[3] = (int16_t)((a3 + b3) >> ROW_SHIFT);
    row[4] = (int16_t)((a3 - b3) >> ROW_SHIFT);
    row[5] = (int16_t)((a2 - b2) >> ROW_SHIFT);
    row[6] = (int16_t)((a1 - b1) >> ROW_SHIFT);
    row[7] = (int16_t)((a0 - b0) >> ROW_SHIFT);
}

/* One column: fills o[0..7] with the un-shifted sums, caller shifts + clips. */
static inline void idct_col(const int16_t *col, int *o)
{
    int a0, a1, a2, a3, b0, b1, b2, b3;
    int x0 = col[0],      x1 = col[8],      x2 = col[8 * 2], x3 = col[8 * 3];
    int x4 = col[8 * 4],  x5 = col[8 * 5],  x6 = col[8 * 6], x7 = col[8 * 7];

    a0 = W4 * (x0 + COL_ROUND_IN);
    a1 = a0; a2 = a0; a3 = a0;

    a0 += W2 * x2;  a1 += W6 * x2;  a2 -= W6 * x2;  a3 -= W2 * x2;
    a0 += W4 * x4;  a1 -= W4 * x4;  a2 -= W4 * x4;  a3 += W4 * x4;
    a0 += W6 * x6;  a1 -= W2 * x6;  a2 += W2 * x6;  a3 -= W6 * x6;

    b0 =  W1 * x1 + W3 * x3 + W5 * x5 + W7 * x7;
    b1 =  W3 * x1 - W7 * x3 - W1 * x5 - W5 * x7;
    b2 =  W5 * x1 - W1 * x3 + W7 * x5 + W3 * x7;
    b3 =  W7 * x1 - W5 * x3 + W3 * x5 - W1 * x7;

    o[0] = a0 + b0;  o[7] = a0 - b0;
    o[1] = a1 + b1;  o[6] = a1 - b1;
    o[2] = a2 + b2;  o[5] = a2 - b2;
    o[3] = a3 + b3;  o[4] = a3 - b3;
}

static inline uint8_t clip8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void m12_idct_put(uint8_t *dst, int stride, int16_t *block)
{
    int i, j, o[8];
    for (i = 0; i < 8; i++) idct_row(block + i * 8);
    for (i = 0; i < 8; i++) {
        idct_col(block + i, o);
        for (j = 0; j < 8; j++)
            dst[j * stride + i] = clip8(o[j] >> COL_SHIFT);
    }
}

void m12_idct_raw(int16_t *block, int *out)
{
    int i, j, o[8];
    for (i = 0; i < 8; i++) idct_row(block + i * 8);
    for (i = 0; i < 8; i++) {
        idct_col(block + i, o);
        for (j = 0; j < 8; j++) out[j * 8 + i] = o[j] >> COL_SHIFT;
    }
}

void m12_idct_add(uint8_t *dst, int stride, int16_t *block)
{
    int i, j, o[8];
    for (i = 0; i < 8; i++) idct_row(block + i * 8);
    for (i = 0; i < 8; i++) {
        idct_col(block + i, o);
        for (j = 0; j < 8; j++)
            dst[j * stride + i] = clip8(dst[j * stride + i] + (o[j] >> COL_SHIFT));
    }
}
