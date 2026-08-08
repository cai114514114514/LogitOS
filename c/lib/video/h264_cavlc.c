/* h264_cavlc.c -- CAVLC coefficient entropy decoding (ITU-T H.264, 9.2).
 *
 * One entry point: cavlc_decode() decodes a single residual block
 * (residual_block_cavlc with the level values handed back in zigzag scan
 * order, coef[0] = DC). Two block shapes exist:
 *   max = 16, nC >= 0: luma 4x4 / I16x16-AC / chroma-AC blocks
 *   max =  4, nC == -1: chroma DC 2x2 block (dedicated coeff_token table)
 *
 * SECURITY: the bitstream is UNTRUSTED. Every VLC lookup is bounds-checked
 * (unknown codewords and truncated reads both fail), level_prefix runs are
 * capped, TotalCoeff / total_zeros / run_before are validated against the
 * block size before any store, and every failure path returns -1 without
 * touching memory outside coef[0..15]. No allocation, no global state.
 *
 * Tables live in h264_tables.h as (bits,len) codeword pairs transcribed
 * from spec Tables 9-5 / 9-7 / 9-8 / 9-9 / 9-10. Decoding walks the code
 * one bit at a time; the tables are tiny (<= 68 entries) so a linear scan
 * per bit length is both simple and fast enough.
 */

#include "h264_int.h"     /* bs.h + the cavlc_decode prototype */
#include "h264_tables.h"  /* VLC codeword tables */

#include <string.h>

#ifdef H264_TRACE
#include <stdio.h>
#define CTRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define CTRACE(...)
#endif

/* level_prefix is a unary run of zero bits; a valid stream never exceeds
 * ~16 here, and 28 keeps the (1 << (prefix-3)) escape arithmetic inside
 * int32. Longer runs are corrupt. */
#define CAVLC_MAX_LEVEL_PREFIX 28

/* Longest codeword of each table family (see h264_tables.h). */
#define CT_VLC_MAX      16   /* coeff_token, 0 <= nC < 8 */
#define CDC_CT_VLC_MAX   8   /* coeff_token, chroma DC (nC == -1) */
#define TZ_VLC_MAX       9   /* total_zeros, 4x4 blocks */
#define CDC_TZ_VLC_MAX   3   /* total_zeros, chroma DC 2x2 */
#define RUN_VLC_MAX     11   /* run_before */

/* Generic prefix-code walk: shift in one bit at a time and look for a
 * (bits,len) match. Returns the table index, or -1 on a truncated read or
 * when no codeword matches within maxlen bits (corrupt stream). */
static int vlc_lookup(bs_t *bs, const uint8_t *bits, const uint8_t *len,
                      int n, int maxlen)
{
    uint32_t code = 0;
    int l, i;

    for (l = 1; l <= maxlen; l++) {
        code = (code << 1) | bs_u1(bs);
        if (bs_error(bs)) return -1;          /* ran off the end */
        for (i = 0; i < n; i++)
            if (len[i] == (uint8_t)l && bits[i] == code)
                return i;
    }
    return -1;                                 /* no such codeword */
}

/* coeff_token (spec 9.2.1, Table 9-5): combined VLC for TotalCoeff and
 * TrailingOnes. On success *tc is 0..16 (0..4 for chroma DC), *t1 0..3. */
static int decode_coeff_token(bs_t *bs, int nC, int *tc, int *t1)
{
    int idx;

    if (nC == -1) {
        /* chroma DC 2x2: dedicated column of Table 9-5 */
        idx = vlc_lookup(bs, h264_cdc_ct_bits, h264_cdc_ct_len, 20,
                         CDC_CT_VLC_MAX);
    } else if (nC >= 8) {
        /* Fixed-length 6-bit code: 000011 (value 3) encodes an empty
         * block; any other value v encodes TotalCoeff (v>>2)+1 and
         * TrailingOnes v&3. */
        uint32_t v = bs_u(bs, 6);
        if (bs_error(bs)) return -1;
        if (v == 3) { *tc = 0; *t1 = 0; return 0; }
        *tc = (int)(v >> 2) + 1;
        *t1 = (int)(v & 3);
        return 0;
    } else {
        int tab = (nC < 2) ? 0 : (nC < 4) ? 1 : 2;
        idx = vlc_lookup(bs, h264_ct_bits[tab], h264_ct_len[tab], 68,
                         CT_VLC_MAX);
    }
    if (idx < 0) return -1;
    *tc = idx >> 2;
    *t1 = idx & 3;
    return 0;
}

/* One level value (spec 9.2.2): level_prefix is unary, level_suffix has a
 * length that adapts via suffixLength. `first` marks the first non-T1
 * level of the block, which carries a +2 bias when TrailingOnes < 3.
 * Returns -1 on corrupt input; *suffixLength is updated in place. */
static int decode_level(bs_t *bs, int first, int t1, int *suffixLength,
                        int *out)
{
    int prefix = 0, suffixSize, sl = *suffixLength;
    int32_t levelCode, lev;

    /* level_prefix: count zero bits up to the terminating 1 */
    for (;;) {
        uint32_t b = bs_u1(bs);
        if (bs_error(bs)) return -1;
        if (b) break;
        if (++prefix > CAVLC_MAX_LEVEL_PREFIX) return -1;
    }

    /* levelSuffixSize selection (spec 9.2.2) */
    if (prefix == 14 && sl == 0)      suffixSize = 4;
    else if (prefix >= 15)            suffixSize = prefix - 3;
    else                              suffixSize = sl;

    levelCode = (int32_t)(prefix < 15 ? prefix : 15) << sl;
    if (suffixSize > 0) {
        levelCode += (int32_t)bs_u(bs, suffixSize);
        if (bs_error(bs)) return -1;
    }
    if (prefix >= 15 && sl == 0) levelCode += 15;
    if (prefix >= 16) levelCode += ((int32_t)1 << (prefix - 3)) - 4096;
    /* the first coded level is biased by 2 when fewer than 3 trailing
     * ones were coded (it cannot be +/-1 in that case) */
    if (t1 < 3 && first) levelCode += 2;

    /* even -> positive, odd -> negative */
    lev = (levelCode & 1) ? -((levelCode + 1) >> 1) : ((levelCode + 2) >> 1);
    *out = (int)lev;

    /* suffixLength adaptation: 0 always becomes 1 after the first level,
     * and the magnitude threshold test ALSO applies to that first level
     * (x264 common/vlc.c and ffmpeg h264_cavlc.c both apply both rules
     * sequentially -- sl==0 with |level| > 3 jumps straight to 2; the
     * plain reading of the spec's "otherwise" branch is wrong here). */
    {
        int next = (sl == 0) ? 1 : sl;
        int mag = (lev < 0) ? -lev : lev;
        if (next < 6 && mag > (3 << (next - 1))) next++;
        *suffixLength = next;
    }
    return 0;
}

/* cavlc_decode -- see h264_int.h for the contract. */
int cavlc_decode(bs_t *bs, int nC, int max, int coef[16])
{
    int level[16], run[16];
    int tc, t1, zeros_left, i, pos;

    /* argument validation: the three block shapes from the contract --
     * 16-coeff blocks (I4x4 luma incl. DC), 15-coeff AC-only blocks
     * (I16x16 luma AC, chroma AC) and 4-coeff chroma DC (nC == -1).  The
     * 15 vs 16 distinction matters: with max=15 a full block (tc=15) codes
     * NO total_zeros element, so decoding AC blocks with max=16 would
     * desync the bitstream. */
    if (!bs || !coef) return -1;
    if (nC == -1) {
        if (max != 4) return -1;
    } else if (nC < 0 || (max != 16 && max != 15)) {
        return -1;
    }

    memset(coef, 0, 16 * sizeof(coef[0]));

#ifdef H264_TRACE
    int ct_start = bs->bitpos;
#endif
    if (decode_coeff_token(bs, nC, &tc, &t1) < 0) {
#ifdef H264_TRACE
        fprintf(stderr, "coeff_token FAIL nC=%d max=%d start=%d bits=", nC, max, ct_start);
        { bs_t t = *bs; t.bitpos = ct_start; t.error = 0;
          for (int i = 0; i < 20 && !bs_error(&t); i++) fputc('0' + (int)bs_u1(&t), stderr); }
        fprintf(stderr, "\n");
#endif
        return -1;
    }
    if (tc > max) return -1;                 /* impossible via the tables,
                                                but stay defensive */
    CTRACE("ct nC=%d max=%d tc=%d t1=%d bitpos=%d\n", nC, max, tc, t1, bs->bitpos);
    if (tc == 0) return 0;                   /* empty block: nothing more */

    /* trailing ones: one sign bit each (1 = negative) */
    for (i = 0; i < t1; i++) {
        uint32_t s = bs_u1(bs);
        if (bs_error(bs)) return -1;
        level[i] = s ? -1 : 1;
    }

    /* remaining levels with adaptive suffix length */
    if (t1 < tc) {
        int suffixLength = (tc > 10 && t1 < 3) ? 1 : 0;
        for (i = t1; i < tc; i++)
            if (decode_level(bs, i == t1, t1, &suffixLength,
                             &level[i]) < 0) {
                CTRACE("level FAIL i=%d tc=%d t1=%d sl=%d bitpos=%d\n", i, tc, t1, suffixLength, bs->bitpos);
                return -1;
            }
        for (i = 0; i < tc; i++) CTRACE("  lev[%d]=%d\n", i, level[i]);
    }

    /* total_zeros (spec 9.2.3): only coded when the block is not full */
    if (tc < max) {
        if (max == 4)
            zeros_left = vlc_lookup(bs, h264_cdc_tz_bits[tc - 1],
                                    h264_cdc_tz_len[tc - 1], 5 - tc,
                                    CDC_TZ_VLC_MAX);
        else
            zeros_left = vlc_lookup(bs, h264_tz_bits[tc - 1],
                                    h264_tz_len[tc - 1], 17 - tc,
                                    TZ_VLC_MAX);
        if (zeros_left < 0) { CTRACE("total_zeros FAIL tc=%d max=%d\n", tc, max); return -1; }
        CTRACE("total_zeros=%d tc=%d bitpos=%d\n", zeros_left, tc, bs->bitpos);
    } else {
        zeros_left = 0;
    }

    /* run_before (spec 9.2.3): one VLC per coefficient, decoded from the
     * highest-frequency coefficient backwards. The j-th decoded run pairs
     * with the j-th decoded level (level[j]), so store runs in DECODE
     * order: run[0] belongs to the trailing-ones end (highest frequency),
     * and the zeros left over go to run[tc-1] (lowest frequency, i.e. the
     * leading zeros of the scan). */
    for (i = 0; i < tc - 1; i++) {
        if (zeros_left > 0) {
            int row = zeros_left > 6 ? 6 : zeros_left - 1;
            int n = zeros_left > 6 ? 15 : zeros_left + 1;
            int r = vlc_lookup(bs, h264_run_bits[row], h264_run_len[row],
                               n, RUN_VLC_MAX);
            if (r < 0 || r > zeros_left) { CTRACE("run_before FAIL i=%d r=%d zl=%d\n", i, r, zeros_left); return -1; }
            run[i] = r;
            zeros_left -= r;
        } else {
            run[i] = 0;                      /* implicit, not coded */
        }
    }
    run[tc - 1] = zeros_left;

    /* scatter levels into zigzag scan positions (spec 9.2.3): coeffNum
     * accumulates from -1 upward while iterating the coefficient index from
     * TotalCoeff-1 DOWN to 0, so the last-decoded level (lowest frequency)
     * lands at the lowest scan position and the trailing ones (highest
     * frequency) land highest. pos stays < max because the runs sum to at
     * most max - tc, but check anyway */
    pos = -1;
    for (i = tc - 1; i >= 0; i--) {
        pos += run[i] + 1;
        if (pos >= max) { CTRACE("scatter FAIL pos=%d max=%d i=%d tc=%d\n", pos, max, i, tc); return -1; }
        coef[pos] = level[i];
    }
#ifdef H264_TRACE
    fprintf(stderr, "coef nC=%d max=%d:", nC, max);
    for (i = 0; i < max; i++) fprintf(stderr, " %d", coef[i]);
    fprintf(stderr, "\n");
#endif
    return tc;
}
