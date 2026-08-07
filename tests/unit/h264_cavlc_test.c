/* tests/unit/h264_cavlc_test.c -- host unit test for h264_cavlc.c.
 *
 * Three layers of verification:
 *   1. Hand-constructed bit strings taken straight from spec Tables 9-5 /
 *      9-7 / 9-8 / 9-9 / 9-10: every coeff_token table (all four nC ranges
 *      plus the chroma-DC nC==-1 table), TrailingOnes boundary cases, the
 *      suffixLength=1 start, level_prefix escape paths, total_zeros /
 *      run_before boundaries, and the zerosLeft>6 run table.
 *   2. Error-path tests: truncated streams, unknown codewords, absurd
 *      level_prefix runs, bad arguments -- all must return -1, never crash.
 *   3. A mini CAVLC *encoder* written independently from the spec text
 *      (levels encoded by inverting the levelCode arithmetic, not by
 *      copying the decoder), plus a random roundtrip of 120k blocks.
 *
 * Build: gcc -O2 -Wall -Wextra -I c/lib/video \
 *          -o /tmp/t tests/unit/h264_cavlc_test.c c/lib/video/h264_cavlc.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bs.h"
#include "h264_tables.h"

int cavlc_decode(bs_t *bs, int nC, int max, int coef[16]);

static int g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); \
        g_fail++; \
    } \
} while (0)

/* ------------------------------------------------------------ bit I/O -- */
/* Feed a "01011..." string into a bs_t. */
static void feed(bs_t *bs, uint8_t *buf, const char *bits)
{
    int n = (int)strlen(bits), i;
    memset(buf, 0, (n + 7) / 8);
    for (i = 0; i < n; i++) {
        if (bits[i] == '1') buf[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        else if (bits[i] != '0') { printf("bad test bitstring\n"); exit(2); }
    }
    bs_init(bs, buf, (n + 7) / 8);
}

typedef struct { uint8_t buf[256]; int bitpos; } bw_t;

static void bw_put(bw_t *w, uint32_t bits, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        if ((bits >> i) & 1u) w->buf[w->bitpos >> 3] |= (uint8_t)(0x80 >> (w->bitpos & 7));
        w->bitpos++;
    }
}
static void bw_put1(bw_t *w, int b) { bw_put(w, (uint32_t)b, 1); }

/* ------------------------------------------------------ mini encoder -- */
/* Independent CAVLC encoder, written from the spec text. Uses the VLC
 * codeword tables only for the prefix codes (those same codewords are
 * pinned down by the hand tests above); the level coding is derived from
 * the levelCode arithmetic, not shared with the decoder. */

static void enc_coeff_token(bw_t *w, int nC, int tc, int t1)
{
    if (nC == -1) {
        int idx = tc * 4 + t1;
        bw_put(w, h264_cdc_ct_bits[idx], h264_cdc_ct_len[idx]);
    } else if (nC >= 8) {
        uint32_t v = (tc == 0) ? 3u : (uint32_t)(((tc - 1) << 2) | t1);
        bw_put(w, v, 6);
    } else {
        int tab = (nC < 2) ? 0 : (nC < 4) ? 1 : 2;
        int idx = tc * 4 + t1;
        bw_put(w, h264_ct_bits[tab][idx], h264_ct_len[tab][idx]);
    }
}

/* Emit level_prefix zeros + '1', then suffixSize suffix bits. */
static void enc_prefix_suffix(bw_t *w, int prefix, uint32_t suffix, int ssize)
{
    int i;
    for (i = 0; i < prefix; i++) bw_put1(w, 0);
    bw_put1(w, 1);
    if (ssize > 0) bw_put(w, suffix, ssize);
}

/* Escape-form emitter: code is what remains after the (15<<sl) or 30 base;
 * picks prefix 15 or the smallest p >= 16 whose range covers it. */
static void enc_escape(bw_t *w, uint32_t rem)
{
    if (rem < 4096u) {
        enc_prefix_suffix(w, 15, rem, 12);
    } else {
        int p = 16;
        uint32_t base;
        for (;;) {
            base = (1u << (p - 3)) - 4096u;
            if (rem >= base && rem < base + (1u << (p - 3))) break;
            p++;
        }
        enc_prefix_suffix(w, p, rem - base, p - 3);
    }
}

/* Encode one level; mirrors the decoder's suffixLength bookkeeping. */
static void enc_level(bw_t *w, int lev, int first, int t1, int *sl)
{
    int32_t code = lev > 0 ? 2 * lev - 2 : -2 * lev - 1;
    if (t1 < 3 && first) code -= 2;
    if (code < 0) { printf("encoder: illegal first level %d\n", lev); exit(2); }

    if (*sl == 0) {
        if (code < 14)      enc_prefix_suffix(w, (int)code, 0, 0);
        else if (code < 30) enc_prefix_suffix(w, 14, (uint32_t)(code - 14), 4);
        else                enc_escape(w, (uint32_t)(code - 30));
    } else {
        int32_t thr = 15 << *sl;
        if (code < thr)
            enc_prefix_suffix(w, (int)(code >> *sl),
                              (uint32_t)(code & ((1 << *sl) - 1)), *sl);
        else
            enc_escape(w, (uint32_t)(code - thr));
    }

    if (*sl == 0) *sl = 1;
    else if (*sl < 6 && (lev < 0 ? -lev : lev) > (3 << (*sl - 1))) (*sl)++;
}

static void enc_total_zeros(bw_t *w, int max, int tc, int tz)
{
    if (tc == max) return;
    if (max == 4) bw_put(w, h264_cdc_tz_bits[tc - 1][tz], h264_cdc_tz_len[tc - 1][tz]);
    else          bw_put(w, h264_tz_bits[tc - 1][tz], h264_tz_len[tc - 1][tz]);
}

static void enc_run(bw_t *w, int zeros_left, int run)
{
    int row = zeros_left > 6 ? 6 : zeros_left - 1;
    bw_put(w, h264_run_bits[row][run], h264_run_len[row][run]);
}

/* Full block encoder: levels[] in scan order (levels[0] is lowest
 * frequency), run[i] = zeros before coefficient i. */
static void enc_block(bw_t *w, int nC, int max, int tc, int t1,
                      const int *level, const int *run, int tz)
{
    int i, zl;
    enc_coeff_token(w, nC, tc, t1);
    if (tc == 0) return;
    for (i = 0; i < t1; i++) bw_put1(w, level[i] < 0);   /* 1 = negative */
    if (t1 < tc) {
        int sl = (tc > 10 && t1 < 3) ? 1 : 0;
        for (i = t1; i < tc; i++) enc_level(w, level[i], i == t1, t1, &sl);
    }
    enc_total_zeros(w, max, tc, tz);
    zl = tz;
    /* run[] is indexed the way the SPEC and the decoder index it: run[i] pairs
     * with level[i], both in decode order (index 0 = the first thing coded =
     * the HIGHEST-frequency coefficient). run_before is coded for i = 0 ..
     * tc-2; run[tc-1] is the leftover and is never coded. This file used to
     * index run[] the other way round, which is why its expectations came out
     * mirrored. */
    for (i = 0; i < tc - 1; i++) {
        if (zl > 0) enc_run(w, zl, run[i]);
        zl -= run[i];
    }
    /* run[tc-1] is implicit (the leftover zeros) */
}

/* ------------------------------------------------------- hand vectors -- */
typedef struct { const char *name, *bits; int nC, max, tc; int coef[16]; } vec_t;

static const vec_t vecs[] = {
    /* -- coeff_token table 0 (0<=nC<2) -- */
    { "tab0 tc0", "1", 0, 16, 0, {0} },
    /* TC1 T1 1 ("01"), sign 1 -> -1, tz=0 ("1") */
    { "tab0 t1", "01" "1" "1", 0, 16, 1, { -1 } },
    /* TC2 T1 1 ("000100"), sign 0 -> +1; level 3: code 2 -> prefix "001";
     * tz=1 ("110"); run_before[1]=1 ("0"), run[0]=0 */
    { "tab0 lvl", "000100" "0" "001" "110" "0", 0, 16, 2,
      { 1, 0, 3 } },
    /* TC3 T1 3 ("00011"), signs 000 -> +1+1+1, tz=0 ("0101"), no runs */
    { "tab0 t1=3", "00011" "000" "0101", 0, 16, 3, { 1, 1, 1 } },
    /* TC2 T1 0 ("00000111"): levels 2 ("1"), 1 ("10" with sl=1);
     * tz=8 ("0010"); zerosLeft 8 > 6: run 7 ("0001"), run[0]=1 */
    { "tab0 run>6", "00000111" "1" "10" "0010" "0001", 0, 16, 2,
      { 0, 2, 0,0,0,0,0,0,0, 1 } },
    /* TC12 T1 0 ("000000000001011"): suffixLength starts at 1.
     * level 2: code 0 -> "1" + suffix 0 = "10" (first, bias applied);
     * 11 more level 2: prefix 1 suffix 0 = "010"; tz=0 ("0000") */
    { "tab0 sl1", "000000000001011" "10"
      "010" "010" "010" "010" "010" "010" "010" "010" "010" "010" "010"
      "0000", 0, 16, 12, { 2,2,2,2,2,2,2,2,2,2,2,2 } },
    /* level escape: TC1 T1 0 ("000101"), level 5000: code 9996 ->
     * prefix 16 (16 zeros + 1), suffix 13 bits 5870 = 1011011101110;
     * tz=0 ("1") */
    { "tab0 esc", "000101" "00000000000000001" "1011011101110" "1",
      0, 16, 1, { 5000 } },
    /* -- coeff_token table 1 (2<=nC<4) -- */
    { "tab1 tc0", "11", 2, 16, 0, {0} },
    /* TC1 T1 0 ("001011"), level 2 -> "1" (bias), tz=0 ("1") */
    { "tab1 lvl", "001011" "1" "1", 2, 16, 1, { 2 } },
    /* -- coeff_token table 2 (4<=nC<8) -- */
    { "tab2 tc0", "1111", 5, 16, 0, {0} },
    /* TC1 T1 1 ("1110" = bits 14 len 4), sign 0 -> +1, tz=0 ("1") */
    { "tab2 t1", "1110" "0" "1", 7, 16, 1, { 1 } },
    /* -- coeff_token FLC (nC>=8) -- */
    { "flc tc0", "000011", 8, 16, 0, {0} },
    /* v=5 -> TC2 T1 1 ("000101"), sign 0 -> +1; level -3: code 3 ->
     * prefix 3 "0001"; tz=0 ("111") */
    { "flc lvl", "000101" "0" "0001" "111", 12, 16, 2, { 1, -3 } },
    /* -- chroma DC (nC == -1, max = 4) -- */
    { "cdc tc0", "01", -1, 4, 0, {0} },
    /* TC2 T1 2 ("001"), signs 11 -> -1-1; tz=1 ("01"); run 1 ("0") */
    { "cdc blk", "001" "11" "01" "0", -1, 4, 2, { -1, 0, -1 } },
    /* TC4 T1 3 ("0000000" = bits 0 len 7), signs 010 -> +1 -1 +1;
     * level 1 (t1=3, no bias): prefix 0 "1"; tc == max -> no
     * total_zeros, no runs */
    { "cdc full", "0000000" "010" "1", -1, 4, 4, { 1, -1, 1, 1 } },
    /* -- total_zeros boundary: last row (TC15): tz=0 is "0", tz=1 "1" -- */
    /* TC15 T1 3 table 0 (bits 12 len 16), signs 000 -> +1+1+1; t1==3 so
     * suffixLength starts at 0: first level 1 is prefix 0 "1", then sl=1
     * and each remaining level 1 is prefix 0 + suffix 0 = "10";
     * tz=1 ("1"); run_before[14]=1 ("0"), rest implicit 0 */
    { "tab0 tz15", "0000000000001100" "000"
      "1" "10" "10" "10" "10" "10" "10" "10" "10" "10" "10" "10"
      "1" "0", 0, 16, 15,
      { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1 } },
};

static void run_vecs(void)
{
    size_t v;
    for (v = 0; v < sizeof(vecs) / sizeof(vecs[0]); v++) {
        const vec_t *t = &vecs[v];
        uint8_t buf[64];
        bs_t bs;
        int coef[16], tc, i, ok = 1;
        for (i = 0; i < 16; i++) coef[i] = 0x7eadbeef;   /* poison */
        feed(&bs, buf, t->bits);
        tc = cavlc_decode(&bs, t->nC, t->max, coef);
        CHECK(tc == t->tc, "%s: tc %d, want %d", t->name, tc, t->tc);
        if (tc < 0) continue;
        for (i = 0; i < 16; i++)
            if (coef[i] != t->coef[i]) ok = 0;
        CHECK(ok, "%s: coef mismatch", t->name);
        if (!ok) {
            printf("  got :");
            for (i = 0; i < 16; i++) printf(" %d", coef[i]);
            printf("\n  want:");
            for (i = 0; i < 16; i++) printf(" %d", t->coef[i]);
            printf("\n");
        }
    }
}

/* --------------------------------------------------------- error paths -- */
static void run_errors(void)
{
    uint8_t buf[64];
    bs_t bs;
    int coef[16];

    /* truncated coeff_token: "0001" matches nothing and runs out */
    feed(&bs, buf, "0001");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "trunc ct not rejected");

    /* unknown coeff_token: 16 zeros in a row is no codeword (nC=0) */
    feed(&bs, buf, "0000000000000000" "0000000000000000");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "bad ct not rejected");

    /* level_prefix overflow: TC1 T1 0 then 30 zeros then a 1 */
    feed(&bs, buf, "000101" "000000000000000000000000000000" "1");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "prefix overflow not rejected");

    /* truncated inside level_suffix */
    feed(&bs, buf, "000101" "00000000000000001" "101");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "trunc suffix not rejected");

    /* truncated total_zeros */
    feed(&bs, buf, "01" "1" "00000000");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "trunc tz not rejected");

    /* truncated run_before (total_zeros decodes to 9, then nothing) */
    feed(&bs, buf, "00000111" "1" "10" "00011");
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "trunc run not rejected");

    /* bad arguments */
    feed(&bs, buf, "1");
    CHECK(cavlc_decode(&bs, 0, 4, coef) == -1, "max=4 with nC>=0 accepted");
    CHECK(cavlc_decode(&bs, -1, 16, coef) == -1, "nC=-1 with max=16 accepted");
    CHECK(cavlc_decode(&bs, 0, 8, coef) == -1, "max=8 accepted");
    CHECK(cavlc_decode(&bs, -2, 16, coef) == -1, "nC=-2 accepted");
    CHECK(cavlc_decode(NULL, 0, 16, coef) == -1, "NULL bs accepted");
    CHECK(cavlc_decode(&bs, 0, 16, NULL) == -1, "NULL coef accepted");

    /* empty buffer */
    bs_init(&bs, buf, 0);
    CHECK(cavlc_decode(&bs, 0, 16, coef) == -1, "empty stream not rejected");

    /* FLC table with only 5 bits left */
    feed(&bs, buf, "00001");
    CHECK(cavlc_decode(&bs, 8, 16, coef) == -1, "trunc flc not rejected");
}

/* ------------------------------------------------------------ roundtrip -- */
static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}

static int rand_level(int allow_one)
{
    uint32_t r = rnd(), mag;
    int lev;
    switch (r % 10) {
    case 0: case 1: case 2: case 3: mag = 1 + rnd() % 4; break;
    case 4: case 5: case 6:        mag = 5 + rnd() % 60; break;
    case 7: case 8:                mag = 60 + rnd() % 2000; break;
    default:                       mag = 2000 + (rnd() % 60000); break;
    }
    if (!allow_one && mag == 1) mag = 2;
    lev = (int)mag;
    return (r & 0x80000000u) ? -lev : lev;
}

static void run_roundtrip(int ncases)
{
    int k, i, pass = 0;
    for (k = 0; k < ncases; k++) {
        bw_t w;
        bs_t bs;
        int nC, max, tc, t1, tz, zl, pos;
        int level[16] = {0}, run[16] = {0}, coef[16], want[16];
        int use_cdc = (rnd() % 5) == 0;

        memset(&w, 0, sizeof w);
        if (use_cdc) { nC = -1; max = 4; }
        else { nC = (int)(rnd() % 17); max = 16; }

        tc = (int)(rnd() % (uint32_t)(max + 1));
        t1 = 0; tz = 0;
        memset(want, 0, sizeof want);
        if (tc > 0) {
            int m = tc < 3 ? tc : 3;
            t1 = (int)(rnd() % (uint32_t)(m + 1));
            for (i = 0; i < t1; i++) level[i] = (rnd() & 1) ? -1 : 1;
            for (i = t1; i < tc; i++)
                level[i] = rand_level(!(i == t1 && t1 < 3));
            tz = (int)(rnd() % (uint32_t)(max - tc + 1));
            /* distribute tz zeros across the coded runs run[0..tc-2]; whatever
             * is left is run[tc-1], exactly as the decoder derives it */
            zl = tz;
            for (i = 0; i < tc - 1; i++) {
                run[i] = zl > 0 ? (int)(rnd() % (uint32_t)(zl + 1)) : 0;
                zl -= run[i];
            }
            run[tc - 1] = zl;
            /* Scatter per spec 9.2.3: coeffNum walks UP from -1 while i walks
             * DOWN from TotalCoeff-1, so the last-decoded level (lowest
             * frequency) lands at the lowest scan position and the trailing
             * ones (highest frequency) land highest. Running i upward instead
             * mirrors the block, which is what this test used to assert. */
            pos = -1;
            for (i = tc - 1; i >= 0; i--) {
                pos += run[i] + 1;
                want[pos] = level[i];
            }
        }

        enc_block(&w, nC, max, tc, t1, level, run, tz);
        /* append random garbage past the block end: the decoder must not
         * be confused into consuming it (bs bounds still apply) */
        bw_put(&w, rnd(), (int)(rnd() % 16));

        bs_init(&bs, w.buf, (w.bitpos + 7) / 8);
        for (i = 0; i < 16; i++) coef[i] = 0x7eadbeef;
        {
            int got = cavlc_decode(&bs, nC, max, coef);
            CHECK(got == tc, "rt %d: tc %d want %d (nC=%d)", k, got, tc, nC);
            if (got >= 0 && memcmp(coef, want, sizeof want) != 0) {
                CHECK(0, "rt %d: coef mismatch (nC=%d tc=%d t1=%d tz=%d)",
                      k, nC, tc, t1, tz);
                printf("  got :");
                for (i = 0; i < 16; i++) printf(" %d", coef[i]);
                printf("\n  want:");
                for (i = 0; i < 16; i++) printf(" %d", want[i]);
                printf("\n");
                if (g_fail > 20) { printf("too many failures\n"); exit(1); }
            } else if (got >= 0) {
                pass++;
            }
        }
    }
    printf("roundtrip: %d/%d ok\n", pass, ncases);
}

int main(void)
{
    run_vecs();
    run_errors();
    run_roundtrip(120000);
    if (g_fail) { printf("FAILED: %d checks\n", g_fail); return 1; }
    printf("ALL PASS\n");
    return 0;
}
