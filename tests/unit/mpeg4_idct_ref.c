/* tests/unit/mpeg4_idct_ref.c -- an INDEPENDENT oracle for c/lib/video/
 * mpeg4_idct.c, transcribed directly from FFmpeg's own reference IDCT.
 *
 * mpeg4_idct.c's own header comment states the bar: ISO/IEC 14496-2 Annex A
 * gives only a statistical accuracy requirement, not a bit-exact transform,
 * so "this file is bit-exact with FFmpeg's simple IDCT
 * (ff_simple_idct_int16_8bit), which is the oracle tests/mpeg4.mk diffs
 * against." This file IS that oracle, and it is a SEPARATE transcription,
 * not a copy of mpeg4_idct.c and not a call into it -- every constant and
 * every shift below was retyped from
 * build/ffmpeg-8.0.1/libavcodec/simple_idct_template.c (the BIT_DEPTH==8,
 * IN_IDCT_DEPTH==16 branch, HAVE_FAST_64BIT's DC shortcut folded to its
 * scalar equivalent) rather than derived from mpeg4_idct.c's source, so a
 * transcription slip in one file has no reason to appear in the other.
 *
 * It was cross-checked, once, against the ACTUAL compiled FFmpeg object
 * (build/ffmpeg-8.0.1/libavcodec/simple_idct.o, which nm shows has ZERO
 * undefined symbols -- fully self-contained x86-64 machine code) over the
 * same corpus tests/unit/mpeg4_idct_test.c generates, and agreed byte for
 * byte on every case. That object file is a local build artifact
 * (build/ is gitignored) and cannot be relied on to exist in every
 * checkout, which is why the committed gate below runs against THIS
 * transcription rather than against the vendored .o -- but the
 * cross-check is what makes this transcription trustworthy as a stand-in,
 * and it is recorded in the phase report rather than silently assumed.
 */
#include <stdint.h>

#define RW1 22725
#define RW2 21407
#define RW3 19266
#define RW4 16383
#define RW5 12873
#define RW6 8867
#define RW7 4520

#define R_ROW_SHIFT 11
#define R_COL_SHIFT 20
#define R_DC_SHIFT  3
#define R_COL_ROUND_IN ((1 << (R_COL_SHIFT - 1)) / RW4)

static void ref_idct_row(int16_t *row)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;

    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        int16_t v = (int16_t)(uint16_t)((uint32_t)row[0] << R_DC_SHIFT);
        row[0] = row[1] = row[2] = row[3] = v;
        row[4] = row[5] = row[6] = row[7] = v;
        return;
    }

    a0 = (uint32_t)(RW4 * row[0]) + (1u << (R_ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;

    a0 += (uint32_t)( RW2 * row[2]);
    a1 += (uint32_t)( RW6 * row[2]);
    a2 += (uint32_t)(-RW6 * row[2]);
    a3 += (uint32_t)(-RW2 * row[2]);

    b0 = (uint32_t)( RW1 * row[1]) + (uint32_t)( RW3 * row[3]);
    b1 = (uint32_t)( RW3 * row[1]) + (uint32_t)(-RW7 * row[3]);
    b2 = (uint32_t)( RW5 * row[1]) + (uint32_t)(-RW1 * row[3]);
    b3 = (uint32_t)( RW7 * row[1]) + (uint32_t)(-RW5 * row[3]);

    if (row[4] | row[5] | row[6] | row[7]) {
        a0 += (uint32_t)( RW4 * row[4]) + (uint32_t)( RW6 * row[6]);
        a1 += (uint32_t)(-RW4 * row[4]) + (uint32_t)(-RW2 * row[6]);
        a2 += (uint32_t)(-RW4 * row[4]) + (uint32_t)( RW2 * row[6]);
        a3 += (uint32_t)( RW4 * row[4]) + (uint32_t)(-RW6 * row[6]);

        b0 += (uint32_t)( RW5 * row[5]); b0 += (uint32_t)( RW7 * row[7]);
        b1 += (uint32_t)(-RW1 * row[5]); b1 += (uint32_t)(-RW5 * row[7]);
        b2 += (uint32_t)( RW7 * row[5]); b2 += (uint32_t)( RW3 * row[7]);
        b3 += (uint32_t)( RW3 * row[5]); b3 += (uint32_t)(-RW1 * row[7]);
    }

    row[0] = (int16_t)((int32_t)(a0 + b0) >> R_ROW_SHIFT);
    row[7] = (int16_t)((int32_t)(a0 - b0) >> R_ROW_SHIFT);
    row[1] = (int16_t)((int32_t)(a1 + b1) >> R_ROW_SHIFT);
    row[6] = (int16_t)((int32_t)(a1 - b1) >> R_ROW_SHIFT);
    row[2] = (int16_t)((int32_t)(a2 + b2) >> R_ROW_SHIFT);
    row[5] = (int16_t)((int32_t)(a2 - b2) >> R_ROW_SHIFT);
    row[3] = (int16_t)((int32_t)(a3 + b3) >> R_ROW_SHIFT);
    row[4] = (int16_t)((int32_t)(a3 - b3) >> R_ROW_SHIFT);
}

#define REF_IDCT_COLS(col) do {                                      \
        a0 = (uint32_t)(RW4 * ((col)[8 * 0] + R_COL_ROUND_IN));      \
        a1 = a0; a2 = a0; a3 = a0;                                   \
                                                                      \
        a0 += (uint32_t)( RW2 * (col)[8 * 2]);                       \
        a1 += (uint32_t)( RW6 * (col)[8 * 2]);                       \
        a2 += (uint32_t)(-RW6 * (col)[8 * 2]);                       \
        a3 += (uint32_t)(-RW2 * (col)[8 * 2]);                       \
                                                                      \
        b0 = (uint32_t)(RW1 * (col)[8 * 1]);                         \
        b1 = (uint32_t)(RW3 * (col)[8 * 1]);                         \
        b2 = (uint32_t)(RW5 * (col)[8 * 1]);                         \
        b3 = (uint32_t)(RW7 * (col)[8 * 1]);                         \
                                                                      \
        b0 += (uint32_t)( RW3 * (col)[8 * 3]);                       \
        b1 += (uint32_t)(-RW7 * (col)[8 * 3]);                       \
        b2 += (uint32_t)(-RW1 * (col)[8 * 3]);                       \
        b3 += (uint32_t)(-RW5 * (col)[8 * 3]);                       \
                                                                      \
        if ((col)[8 * 4]) {                                          \
            a0 += (uint32_t)( RW4 * (col)[8 * 4]);                   \
            a1 += (uint32_t)(-RW4 * (col)[8 * 4]);                   \
            a2 += (uint32_t)(-RW4 * (col)[8 * 4]);                   \
            a3 += (uint32_t)( RW4 * (col)[8 * 4]);                   \
        }                                                            \
        if ((col)[8 * 5]) {                                          \
            b0 += (uint32_t)( RW5 * (col)[8 * 5]);                   \
            b1 += (uint32_t)(-RW1 * (col)[8 * 5]);                   \
            b2 += (uint32_t)( RW7 * (col)[8 * 5]);                   \
            b3 += (uint32_t)( RW3 * (col)[8 * 5]);                   \
        }                                                            \
        if ((col)[8 * 6]) {                                          \
            a0 += (uint32_t)( RW6 * (col)[8 * 6]);                   \
            a1 += (uint32_t)(-RW2 * (col)[8 * 6]);                   \
            a2 += (uint32_t)( RW2 * (col)[8 * 6]);                   \
            a3 += (uint32_t)(-RW6 * (col)[8 * 6]);                   \
        }                                                            \
        if ((col)[8 * 7]) {                                          \
            b0 += (uint32_t)( RW7 * (col)[8 * 7]);                   \
            b1 += (uint32_t)(-RW5 * (col)[8 * 7]);                   \
            b2 += (uint32_t)( RW3 * (col)[8 * 7]);                   \
            b3 += (uint32_t)(-RW1 * (col)[8 * 7]);                   \
        }                                                            \
    } while (0)

static inline uint8_t ref_clip8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void ref_idct_col_put(uint8_t *dest, int line_size, int16_t *col)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;
    REF_IDCT_COLS(col);
    dest[0] = ref_clip8((int32_t)(a0 + b0) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a1 + b1) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a2 + b2) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a3 + b3) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a3 - b3) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a2 - b2) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a1 - b1) >> R_COL_SHIFT); dest += line_size;
    dest[0] = ref_clip8((int32_t)(a0 - b0) >> R_COL_SHIFT);
}

static void ref_idct_col_add(uint8_t *dest, int line_size, int16_t *col)
{
    uint32_t a0, a1, a2, a3, b0, b1, b2, b3;
    REF_IDCT_COLS(col);
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a0 + b0) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a1 + b1) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a2 + b2) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a3 + b3) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a3 - b3) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a2 - b2) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a1 - b1) >> R_COL_SHIFT)); dest += line_size;
    dest[0] = ref_clip8(dest[0] + ((int32_t)(a0 - b0) >> R_COL_SHIFT));
}

void ref_idct_put(uint8_t *dest, int line_size, int16_t *block)
{
    int i;
    for (i = 0; i < 8; i++) ref_idct_row(block + i * 8);
    for (i = 0; i < 8; i++) ref_idct_col_put(dest + i, line_size, block + i);
}

void ref_idct_add(uint8_t *dest, int line_size, int16_t *block)
{
    int i;
    for (i = 0; i < 8; i++) ref_idct_row(block + i * 8);
    for (i = 0; i < 8; i++) ref_idct_col_add(dest + i, line_size, block + i);
}
