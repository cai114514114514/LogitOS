/* c/lib/video/mpeg4_idct.c -- the 8x8 inverse DCT for MPEG-4 Part 2 and H.263.
 *
 * WHICH IDCT, AND WHY THE QUESTION HAS NO ANSWER INSIDE THE STANDARD.
 * ISO/IEC 14496-2 Annex A does not define the inverse transform. It states an
 * ACCURACY REQUIREMENT (the IEEE 1180 statistical bounds, plus a periodic
 * intra-refresh rule to bound drift) and leaves the arithmetic to the
 * implementer. ITU-T H.263 Annex A is the same document in a different
 * typeface. So two decoders that both CONFORM legitimately differ by +/-1 per
 * sample, and because P- and B-VOPs predict from the reconstruction, that
 * difference compounds along a GOP rather than staying local.
 *
 * The consequence for a test is unavoidable and worth stating plainly: THERE
 * IS NO BIT-EXACT ANSWER TO COMPARE AGAINST UNLESS THE TRANSFORM IS PINNED.
 * "Within a tolerance" is not a bar this tree accepts for anything it can
 * pin, so the transform is pinned: this file is bit-exact with FFmpeg's
 * simple IDCT (ff_simple_idct_int16_8bit), which is the oracle tests/mpeg4.mk
 * diffs against, and tools/genmpeg4.sh passes "-idct simple -cpuflags 0" on
 * every reference decode so the oracle is that exact C routine and not one of
 * its SIMD cousins.
 *
 * AND THE CONVERSE, WHICH IS THE PART A READER NEEDS: a file encoded by Xvid
 * -- which is most MPEG-4 Part 2 in the wild -- was encoded against XVID'S OWN
 * IDCT in the encoder's reconstruction loop. Decoding it with any other
 * conforming IDCT, ours or FFmpeg's, produces a picture that DRIFTS from what
 * the encoder saw, by design of the standard. That drift is expected, it is
 * not a defect in this file, and it is why FFmpeg ships "-idct xvid" at all.
 * Nothing here claims bit-exactness against a decoder that is not the pinned
 * one.
 *
 * WHAT THIS TRANSFORM IS. A separable 8-point integer IDCT with 14-bit
 * constants W_k = round(cos(k*pi/16) * sqrt(2) * 2^14), rows rounded at 11
 * bits and columns at 20. The butterfly is the ordinary even/odd
 * decomposition and every one of its coefficient positions is DERIVED from
 * cos((2n+1)k*pi/16); tests/unit/mpeg4_idct_test.c re-derives all 32 of them
 * in double and compares. Three things had to be taken from the pinned
 * implementation rather than from the mathematics, because each is a CHOICE
 * and each is observable:
 *
 *   1. W4 is 16383, not 16384. The column pass's rounding constant is
 *      therefore written W4 * ((1 << 19) / W4) = W4 * 32 = 524256, not
 *      1 << 19 = 524288. A 32-unit difference ahead of a 20-bit shift moves a
 *      sample whenever the exact value lands within 32 of a rounding boundary.
 *
 *   2. The row pass has a DC-only shortcut that is NOT algebraically equal to
 *      the general path. (W4*x + 1024) >> 11 equals 8*x only while
 *      -1024 <= x <= 1024; the shortcut writes 8*x for every x. A flat
 *      macroblock -- the commonest block in any picture -- takes that branch,
 *      and at intra DC levels above 1024 it comes out one step brighter than
 *      the general path would make it. Deleting the shortcut "as an
 *      optimisation" changes the picture.
 *
 *   3. The row pass stores back into int16_t and is allowed to WRAP there.
 *      The shortcut's 16-bit truncation is that wrap made explicit, and the
 *      intermediate sums are computed in unsigned 32-bit so that they wrap
 *      rather than trap.
 *
 * Everything else -- the skips of all-zero coefficient groups, the ordering --
 * is exact and may be rearranged freely.
 */
#include <stdint.h>
#include "mpeg4_int.h"

#define W1 22725   /* cos(1*pi/16) * sqrt(2) * 2^14 + 0.5 */
#define W2 21407
#define W3 19266
/* The negative control: MPEG4_IDCT_CONTROL_W4_16384 uses the value the
 * cosine formula actually rounds to (see the file header, note 1) instead
 * of the pinned oracle's 16383. It is wrong only where a column's exact
 * value lands within 32 units of a rounding boundary ahead of the >>20
 * shift, so it does not redden every case -- see tests/mpeg4.mk for the
 * measured count. */
#ifdef MPEG4_IDCT_CONTROL_W4_16384
#define W4 16384
#else
#define W4 16383   /* cos(4*pi/16) * sqrt(2) * 2^14 = 16384, minus one */
#endif
#define W5 12873
#define W6 8867
#define W7 4520

#define ROW_SHIFT 11
#define COL_SHIFT 20
#define DC_SHIFT  3

/* Written the way it is computed, not as the value, because the integer
 * division by a W4 that is one short is the whole point. */
#define COL_ROUND_IN ((1 << (COL_SHIFT - 1)) / W4)

static void idct_row(int16_t *row)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;

    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        /* See note 2 above: 8*row[0], truncated to 16 bits, for every row[0]. */
        int16_t v = (int16_t)(uint16_t)((uint32_t)row[0] << DC_SHIFT);
        row[0] = row[1] = row[2] = row[3] = v;
        row[4] = row[5] = row[6] = row[7] = v;
        return;
    }

    a0 = (uint32_t)(W4 * row[0]) + (1u << (ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;

    a0 += (uint32_t)( W2 * row[2]);
    a1 += (uint32_t)( W6 * row[2]);
    a2 += (uint32_t)(-W6 * row[2]);
    a3 += (uint32_t)(-W2 * row[2]);

    b0 = (uint32_t)( W1 * row[1]) + (uint32_t)( W3 * row[3]);
    b1 = (uint32_t)( W3 * row[1]) + (uint32_t)(-W7 * row[3]);
    b2 = (uint32_t)( W5 * row[1]) + (uint32_t)(-W1 * row[3]);
    b3 = (uint32_t)( W7 * row[1]) + (uint32_t)(-W5 * row[3]);

    if (row[4] | row[5] | row[6] | row[7]) {
        a0 += (uint32_t)( W4 * row[4]) + (uint32_t)( W6 * row[6]);
        a1 += (uint32_t)(-W4 * row[4]) + (uint32_t)(-W2 * row[6]);
        a2 += (uint32_t)(-W4 * row[4]) + (uint32_t)( W2 * row[6]);
        a3 += (uint32_t)( W4 * row[4]) + (uint32_t)(-W6 * row[6]);

        b0 += (uint32_t)( W5 * row[5]); b0 += (uint32_t)( W7 * row[7]);
        b1 += (uint32_t)(-W1 * row[5]); b1 += (uint32_t)(-W5 * row[7]);
        b2 += (uint32_t)( W7 * row[5]); b2 += (uint32_t)( W3 * row[7]);
        b3 += (uint32_t)( W3 * row[5]); b3 += (uint32_t)(-W1 * row[7]);
    }

    row[0] = (int16_t)((int32_t)(a0 + b0) >> ROW_SHIFT);
    row[7] = (int16_t)((int32_t)(a0 - b0) >> ROW_SHIFT);
    row[1] = (int16_t)((int32_t)(a1 + b1) >> ROW_SHIFT);
    row[6] = (int16_t)((int32_t)(a1 - b1) >> ROW_SHIFT);
    row[2] = (int16_t)((int32_t)(a2 + b2) >> ROW_SHIFT);
    row[5] = (int16_t)((int32_t)(a2 - b2) >> ROW_SHIFT);
    row[3] = (int16_t)((int32_t)(a3 + b3) >> ROW_SHIFT);
    row[4] = (int16_t)((int32_t)(a3 - b3) >> ROW_SHIFT);
}

#define IDCT_COLS(col) do {                                          \
        a0 = (uint32_t)(W4 * ((col)[8 * 0] + COL_ROUND_IN));         \
        a1 = a0; a2 = a0; a3 = a0;                                   \
                                                                     \
        a0 += (uint32_t)( W2 * (col)[8 * 2]);                        \
        a1 += (uint32_t)( W6 * (col)[8 * 2]);                        \
        a2 += (uint32_t)(-W6 * (col)[8 * 2]);                        \
        a3 += (uint32_t)(-W2 * (col)[8 * 2]);                        \
                                                                     \
        b0 = (uint32_t)(W1 * (col)[8 * 1]);                          \
        b1 = (uint32_t)(W3 * (col)[8 * 1]);                          \
        b2 = (uint32_t)(W5 * (col)[8 * 1]);                          \
        b3 = (uint32_t)(W7 * (col)[8 * 1]);                          \
                                                                     \
        b0 += (uint32_t)( W3 * (col)[8 * 3]);                        \
        b1 += (uint32_t)(-W7 * (col)[8 * 3]);                        \
        b2 += (uint32_t)(-W1 * (col)[8 * 3]);                        \
        b3 += (uint32_t)(-W5 * (col)[8 * 3]);                        \
                                                                     \
        if ((col)[8 * 4]) {                                          \
            a0 += (uint32_t)( W4 * (col)[8 * 4]);                    \
            a1 += (uint32_t)(-W4 * (col)[8 * 4]);                    \
            a2 += (uint32_t)(-W4 * (col)[8 * 4]);                    \
            a3 += (uint32_t)( W4 * (col)[8 * 4]);                    \
        }                                                            \
        if ((col)[8 * 5]) {                                          \
            b0 += (uint32_t)( W5 * (col)[8 * 5]);                    \
            b1 += (uint32_t)(-W1 * (col)[8 * 5]);                    \
            b2 += (uint32_t)( W7 * (col)[8 * 5]);                    \
            b3 += (uint32_t)( W3 * (col)[8 * 5]);                    \
        }                                                            \
        if ((col)[8 * 6]) {                                          \
            a0 += (uint32_t)( W6 * (col)[8 * 6]);                    \
            a1 += (uint32_t)(-W2 * (col)[8 * 6]);                    \
            a2 += (uint32_t)( W2 * (col)[8 * 6]);                    \
            a3 += (uint32_t)(-W6 * (col)[8 * 6]);                    \
        }                                                            \
        if ((col)[8 * 7]) {                                          \
            b0 += (uint32_t)( W7 * (col)[8 * 7]);                    \
            b1 += (uint32_t)(-W5 * (col)[8 * 7]);                    \
            b2 += (uint32_t)( W3 * (col)[8 * 7]);                    \
            b3 += (uint32_t)(-W1 * (col)[8 * 7]);                    \
        }                                                            \
    } while (0)

static inline uint8_t clip8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void idct_col_put(uint8_t *dest, int line_size, int16_t *col)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;
    IDCT_COLS(col);
    dest[0] = clip8((int32_t)(a0 + b0) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a1 + b1) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a2 + b2) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a3 + b3) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a3 - b3) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a2 - b2) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a1 - b1) >> COL_SHIFT); dest += line_size;
    dest[0] = clip8((int32_t)(a0 - b0) >> COL_SHIFT);
}

static void idct_col_add(uint8_t *dest, int line_size, int16_t *col)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;
    IDCT_COLS(col);
    dest[0] = clip8(dest[0] + ((int32_t)(a0 + b0) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a1 + b1) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a2 + b2) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a3 + b3) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a3 - b3) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a2 - b2) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a1 - b1) >> COL_SHIFT)); dest += line_size;
    dest[0] = clip8(dest[0] + ((int32_t)(a0 - b0) >> COL_SHIFT));
}

void m4_idct_put(uint8_t *dest, int line_size, int16_t *block)
{
    int i;
    for (i = 0; i < 8; i++) idct_row(block + i * 8);
    for (i = 0; i < 8; i++) idct_col_put(dest + i, line_size, block + i);
}

void m4_idct_add(uint8_t *dest, int line_size, int16_t *block)
{
    int i;
    for (i = 0; i < 8; i++) idct_row(block + i * 8);
    for (i = 0; i < 8; i++) idct_col_add(dest + i, line_size, block + i);
}
