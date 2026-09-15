/* tests/unit/mpeg4_idct_test.c -- bit-exact differential for c/lib/video/
 * mpeg4_idct.c's m4_idct_put/m4_idct_add against tests/unit/
 * mpeg4_idct_ref.c's independent transcription of FFmpeg's
 * ff_simple_idct_int16_8bit -- the pinned oracle mpeg4_idct.c's own header
 * names. Every mismatch is a real mismatch: there is no tolerance, because
 * the whole point of pinning the transform (see that header) was to make
 * this comparison bit-exact rather than statistical.
 *
 * The corpus is built to reach the three things the header calls out as
 * DELIBERATE CHOICES rather than derivable from the mathematics, so a test
 * that only fed smooth, small-magnitude blocks would never exercise any of
 * them:
 *   (1) W4 = 16383, not 16384 -- probed with DC values whose column-pass
 *       rounding sits within 32 of a boundary.
 *   (2) The DC-only row shortcut's 16-bit wrap, which is NOT algebraically
 *       equal to the general path once |DC| > 1024 -- probed with DC values
 *       spanning the full int16 range, including values that wrap.
 *   (3) Intermediate row-pass storage into int16_t, allowed to wrap --
 *       probed with full-magnitude coefficients at every position, not just
 *       realistic post-dequant ones.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

void m4_idct_put(uint8_t *dest, int line_size, int16_t *block);
void m4_idct_add(uint8_t *dest, int line_size, int16_t *block);
void ref_idct_put(uint8_t *dest, int line_size, int16_t *block);
void ref_idct_add(uint8_t *dest, int line_size, int16_t *block);

static uint64_t rngstate = 0x2545F4914F6CDD1Dull;
static uint32_t rnd32(void)
{
    rngstate ^= rngstate << 13;
    rngstate ^= rngstate >> 7;
    rngstate ^= rngstate << 17;
    return (uint32_t)rngstate;
}
static int16_t rnd_range(int lo, int hi) /* inclusive */
{
    uint32_t span = (uint32_t)(hi - lo + 1);
    return (int16_t)(lo + (int)(rnd32() % span));
}

typedef struct { int16_t b[64]; const char *label; } case_t;

#define MAXCASES 4096
static case_t cases[MAXCASES];
static int ncases = 0;

static void add_case(const int16_t *b, const char *label)
{
    if (ncases >= MAXCASES) return;
    memcpy(cases[ncases].b, b, sizeof(int16_t) * 64);
    cases[ncases].label = label;
    ncases++;
}

static void build_corpus(void)
{
    int16_t z[64];
    memset(z, 0, sizeof z);
    add_case(z, "all-zero");

    /* (1) & (2): DC-only, full int16 range including values that trip the
     * 16-bit row-shortcut wrap and values sitting near W4-rounding
     * boundaries. Every multiple-of-something-near-1024 near the shortcut's
     * documented break point (+-1024) is deliberately included. */
    static const int dcvals[] = {
        0, 1, -1, 2, -2, 7, -7, 8, -8, 100, -100,
        1023, 1024, 1025, -1023, -1024, -1025,
        2000, -2000, 4095, -4095, 4096, -4096,
        8191, -8191, 8192, -8192,
        16383, -16383, 16384, -16384,
        32767, -32768, 30000, -30000, 21845, -21845,
    };
    for (size_t i = 0; i < sizeof(dcvals) / sizeof(dcvals[0]); i++) {
        int16_t b[64]; memset(b, 0, sizeof b);
        b[0] = (int16_t)dcvals[i];
        add_case(b, "dc-only");
    }

    /* Impulse at every one of the 64 positions, at several magnitudes --
     * isolates which basis function a mismatch belongs to. */
    static const int impulses[] = { 1, -1, 17, -17, 255, -255, 2047, -2048, 32767, -32768 };
    for (int pos = 0; pos < 64; pos++) {
        for (size_t i = 0; i < sizeof(impulses) / sizeof(impulses[0]); i++) {
            int16_t b[64]; memset(b, 0, sizeof b);
            b[pos] = (int16_t)impulses[i];
            add_case(b, "impulse");
        }
    }

    /* Realistic post-dequant blocks: small-ish, zigzag-decaying magnitude,
     * random sign -- what an actual coded macroblock looks like. */
    for (int t = 0; t < 400; t++) {
        int16_t b[64];
        for (int i = 0; i < 64; i++) {
            int mag = 200 - i * 2; if (mag < 0) mag = 0;
            b[i] = mag ? rnd_range(-mag, mag) : 0;
        }
        add_case(b, "realistic");
    }

    /* (3): full int16 range at EVERY position simultaneously -- the
     * adversarial case for row-pass wraparound, since nothing here bounds
     * coefficients the way a real quantiser would. */
    for (int t = 0; t < 400; t++) {
        int16_t b[64];
        for (int i = 0; i < 64; i++) b[i] = rnd_range(-32768, 32767);
        add_case(b, "full-range");
    }

    /* Mid-magnitude random, several hundred, the bulk of ordinary coverage. */
    for (int t = 0; t < 800; t++) {
        int16_t b[64];
        for (int i = 0; i < 64; i++) b[i] = rnd_range(-2048, 2047);
        add_case(b, "mid-random");
    }

    /* Sparse blocks (most coefficients zero, a handful set) -- the shape
     * m4_idct_row's own "skip the high half if row[4..7] all zero" branch
     * and REF_IDCT_COLS's per-coefficient `if` guards are FOR. */
    for (int t = 0; t < 400; t++) {
        int16_t b[64]; memset(b, 0, sizeof b);
        int nnz = 1 + (int)(rnd32() % 6);
        for (int k = 0; k < nnz; k++) b[rnd32() % 64] = rnd_range(-2000, 2000);
        add_case(b, "sparse");
    }
}

int main(void)
{
    build_corpus();

    long total_put = 0, mism_put = 0;
    long total_add = 0, mism_add = 0;
    int worst_case_put = -1; long worst_put = 0;
    int worst_case_add = -1; long worst_add = 0;
    int first_bad_put = -1, first_bad_add = -1;

    for (int c = 0; c < ncases; c++) {
        int16_t blk_a[64], blk_b[64];
        memcpy(blk_a, cases[c].b, sizeof blk_a);
        memcpy(blk_b, cases[c].b, sizeof blk_b);

        uint8_t dest_a[64], dest_b[64];
        memset(dest_a, 0, sizeof dest_a);
        memset(dest_b, 0, sizeof dest_b);
        m4_idct_put(dest_a, 8, blk_a);
        ref_idct_put(dest_b, 8, blk_b);

        long m = 0;
        for (int i = 0; i < 64; i++) {
            total_put++;
            if (dest_a[i] != dest_b[i]) {
                m++;
                if (first_bad_put < 0) first_bad_put = c;
            }
        }
        mism_put += m;
        if (m > worst_put) { worst_put = m; worst_case_put = c; }

        /* add(): pre-fill dest with a nontrivial background so clipping at
         * both ends of 0..255 is exercised, using the SAME background for
         * both implementations. */
        uint8_t bg[64];
        for (int i = 0; i < 64; i++) bg[i] = (uint8_t)((i * 37 + c * 13) & 0xff);
        memcpy(dest_a, bg, sizeof bg);
        memcpy(dest_b, bg, sizeof bg);
        memcpy(blk_a, cases[c].b, sizeof blk_a);
        memcpy(blk_b, cases[c].b, sizeof blk_b);
        m4_idct_add(dest_a, 8, blk_a);
        ref_idct_add(dest_b, 8, blk_b);

        m = 0;
        for (int i = 0; i < 64; i++) {
            total_add++;
            if (dest_a[i] != dest_b[i]) {
                m++;
                if (first_bad_add < 0) first_bad_add = c;
            }
        }
        mism_add += m;
        if (m > worst_add) { worst_add = m; worst_case_add = c; }
    }

    printf("MPEG4-IDCT cases=%d put: %ld/%ld wrong (worst case %d \"%s\" = %ld/64, first bad case %d)\n",
           ncases, mism_put, total_put, worst_case_put,
           worst_case_put >= 0 ? cases[worst_case_put].label : "-", worst_put, first_bad_put);
    printf("MPEG4-IDCT cases=%d add: %ld/%ld wrong (worst case %d \"%s\" = %ld/64, first bad case %d)\n",
           ncases, mism_add, total_add, worst_case_add,
           worst_case_add >= 0 ? cases[worst_case_add].label : "-", worst_add, first_bad_add);

    if (mism_put == 0 && mism_add == 0) {
        printf("MPEG4-IDCT-OK bit-exact over %d cases (%ld put samples + %ld add samples)\n",
               ncases, total_put, total_add);
        return 0;
    }
    return 1;
}
