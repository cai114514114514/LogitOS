/* tests/unit/h265_cabac_test.c -- the HEVC arithmetic decoder, on its own.
 *
 * H.264 baseline used CAVLC, so CABAC is entirely new code with no sibling to
 * cross-check against. The strongest available check is a ROUND TRIP: this
 * file contains the spec's arithmetic *encoder* (9.3.4.4 -- EncodeDecision,
 * EncodeBypass, EncodeTerminate, PutBit, EncodeFlush), written from the same
 * text as the decoder but as an independent program, and asserts that every
 * bin sequence it encodes comes back out of h265_cabac.c unchanged.
 *
 * That is deliberately a different arrangement from the H.264 CAVLC test next
 * door, whose in-test encoder disagreed with the decoder about level coding
 * and so could never be wired into a gate. The difference is that an
 * arithmetic coder's encoder and decoder are duals of ONE state machine: the
 * context update (transIdxMps/transIdxLps/valMps) is character-for-character
 * the same in both, so a round trip that survives 200 000 bins with adapting
 * contexts is evidence about the shared table and the shared adaptation, not
 * only about the two halves agreeing with each other. What a round trip
 * cannot prove is the INITIALISATION -- both halves read init_values[] -- so
 * that is checked separately against hand-computed values from 9.3.2.2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h265.h"
#include "h265_int.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ===================== the spec's arithmetic encoder (9.3.4.4) ========== */
static const uint8_t e_range_lps[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},
    {116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    { 95,116,137,158},{ 90,110,130,150},{ 85,104,123,142},{ 81, 99,117,135},
    { 77, 94,111,128},{ 73, 89,105,122},{ 69, 85,100,116},{ 66, 80, 95,110},
    { 62, 76, 90,104},{ 59, 72, 86, 99},{ 56, 69, 81, 94},{ 53, 65, 77, 89},
    { 51, 62, 73, 85},{ 48, 59, 69, 80},{ 46, 56, 66, 76},{ 43, 53, 63, 72},
    { 41, 50, 59, 69},{ 39, 48, 56, 65},{ 37, 45, 54, 62},{ 35, 43, 51, 59},
    { 33, 41, 48, 56},{ 32, 39, 46, 53},{ 30, 37, 43, 50},{ 29, 35, 41, 48},
    { 27, 33, 39, 45},{ 26, 31, 37, 43},{ 24, 30, 35, 41},{ 23, 28, 33, 39},
    { 22, 27, 32, 37},{ 21, 26, 30, 35},{ 20, 24, 29, 33},{ 19, 23, 27, 31},
    { 18, 22, 26, 30},{ 17, 21, 25, 28},{ 16, 20, 23, 27},{ 15, 19, 22, 25},
    { 14, 18, 21, 24},{ 14, 17, 20, 23},{ 13, 16, 19, 22},{ 12, 15, 18, 21},
    { 12, 14, 17, 20},{ 11, 14, 16, 19},{ 11, 13, 15, 18},{ 10, 12, 15, 17},
    { 10, 12, 14, 16},{  9, 11, 13, 15},{  9, 11, 12, 14},{  8, 10, 12, 14},
    {  8,  9, 11, 13},{  7,  9, 11, 12},{  7,  9, 10, 12},{  7,  8, 10, 11},
    {  6,  8,  9, 11},{  6,  7,  9, 10},{  6,  7,  8,  9},{  2,  2,  2,  2}
};
static const uint8_t e_lps[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};
static const uint8_t e_mps[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};

typedef struct {
    uint8_t *buf;
    int cap, bytepos, bitpos;
    uint32_t low;
    uint32_t range;
    int first_bit, outstanding;
    uint8_t state[H265_NCTX];
} enc_t;

static void e_writebit(enc_t *e, int b)
{
    if (e->bytepos >= e->cap) return;
    if (b) e->buf[e->bytepos] |= (uint8_t)(0x80 >> e->bitpos);
    if (++e->bitpos == 8) { e->bitpos = 0; e->bytepos++; }
}
static void e_putbit(enc_t *e, int b)
{
    if (e->first_bit) e->first_bit = 0;
    else e_writebit(e, b);
    while (e->outstanding > 0) { e_writebit(e, 1 - b); e->outstanding--; }
}
static void e_renorm(enc_t *e)
{
    while (e->range < 256) {
        if (e->low < 256) {
            e_putbit(e, 0);
        } else if (e->low >= 512) {
            e->low -= 512;
            e_putbit(e, 1);
        } else {
            e->low -= 256;
            e->outstanding++;
        }
        e->range <<= 1;
        e->low <<= 1;
    }
}
static void e_init(enc_t *e, uint8_t *buf, int cap)
{
    memset(buf, 0, (size_t)cap);
    e->buf = buf; e->cap = cap; e->bytepos = 0; e->bitpos = 0;
    e->low = 0; e->range = 510; e->first_bit = 1; e->outstanding = 0;
}
static void e_decision(enc_t *e, int ctx, int bin)
{
    uint8_t s = e->state[ctx];
    int st = s >> 1, mps = s & 1;
    uint32_t lps = e_range_lps[st][(e->range >> 6) & 3];
    e->range -= lps;
    if (bin != mps) {
        e->low += e->range;
        e->range = lps;
        if (st == 0) mps = 1 - mps;
        st = e_lps[st];
    } else {
        st = e_mps[st];
    }
    e->state[ctx] = (uint8_t)((st << 1) | mps);
    e_renorm(e);
}
static void e_bypass(enc_t *e, int bin)
{
    e->low <<= 1;
    if (bin) e->low += e->range;
    if (e->low >= 1024) { e_putbit(e, 1); e->low -= 1024; }
    else if (e->low < 512) e_putbit(e, 0);
    else { e->low -= 512; e->outstanding++; }
}
static void e_flush(enc_t *e)
{
    e->range = 2;
    e_renorm(e);
    e_putbit(e, (int)((e->low >> 9) & 1));
    /* WriteBits( ( ( ivlLow >> 7 ) & 3 ) | 1, 2 ) */
    e_writebit(e, (int)((e->low >> 8) & 1));
    e_writebit(e, 1);
}
static void e_terminate(enc_t *e, int bin)
{
    e->range -= 2;
    if (bin) { e->low += e->range; e_flush(e); }
    else e_renorm(e);
}

/* ================================ tests ================================= */

/* A deterministic bin script: element kind, context, value. */
enum { K_DEC, K_BYP, K_TERM };
typedef struct { uint8_t kind; uint8_t ctx; uint8_t val; } bin_t;

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) & 0xFFFFFF;
}

static void roundtrip(const char *name, bin_t *bins, int n, int init_type, int qp)
{
    static uint8_t buf[1 << 21];
    enc_t e;
    cabac_t c;

    /* Both halves start from the same initialised contexts. The decoder's
     * initialiser is the one under test; the encoder borrows it, which is
     * exactly why the init values get their own direct check below. */
    h265_cabac_init_ctx(&c, init_type, qp);
    memcpy(e.state, c.state, sizeof e.state);

    e_init(&e, buf, (int)sizeof buf);
    for (int i = 0; i < n; i++) {
        if (bins[i].kind == K_DEC) e_decision(&e, bins[i].ctx, bins[i].val);
        else if (bins[i].kind == K_BYP) e_bypass(&e, bins[i].val);
        else e_terminate(&e, bins[i].val);
    }
    e_terminate(&e, 1);                    /* flush */
    int len = e.bytepos + (e.bitpos ? 1 : 0);

    h265_cabac_init_ctx(&c, init_type, qp);
    if (h265_cabac_start(&c, buf, len, 0) != H265_OK) {
        CHECK(0, "%s: cabac_start rejected the encoder's output", name);
        return;
    }
    int bad = -1;
    for (int i = 0; i < n; i++) {
        int got;
        if (bins[i].kind == K_DEC) got = h265_cabac_decision(&c, bins[i].ctx);
        else if (bins[i].kind == K_BYP) got = h265_cabac_bypass(&c);
        else got = h265_cabac_terminate(&c);
        if (got != bins[i].val) { bad = i; break; }
    }
    CHECK(bad < 0, "%s: bin %d of %d decoded wrong (want %d)",
          name, bad, n, bad >= 0 ? bins[bad].val : 0);
    CHECK(!c.error, "%s: decoder set its error flag", name);
}

static void test_roundtrips(void)
{
    static bin_t bins[200000];

    /* 1. All-MPS on a single context: the pure adaptation ladder. */
    for (int i = 0; i < 2000; i++) { bins[i].kind = K_DEC; bins[i].ctx = 7; bins[i].val = 0; }
    roundtrip("all-zero one ctx", bins, 2000, 0, 26);

    /* 2. Alternating, which drives the LPS transition table every other bin. */
    for (int i = 0; i < 2000; i++) {
        bins[i].kind = K_DEC; bins[i].ctx = 7; bins[i].val = (uint8_t)(i & 1);
    }
    roundtrip("alternating one ctx", bins, 2000, 0, 26);

    /* 3. Bypass only: no context state at all, just the range/offset engine. */
    for (int i = 0; i < 5000; i++) {
        bins[i].kind = K_BYP; bins[i].val = (uint8_t)(rnd() & 1);
    }
    roundtrip("bypass only", bins, 5000, 0, 26);

    /* 4. The real shape: random bins across every context, mixed with bypass
     *    and with non-terminating terminate bins (which do renormalise). */
    for (int t = 0; t < 3; t++) {
        for (int q = 10; q <= 45; q += 35) {
            int n = 200000;
            for (int i = 0; i < n; i++) {
                uint32_t r = rnd();
                if ((r & 15) == 15) { bins[i].kind = K_BYP; bins[i].val = (uint8_t)((r >> 4) & 1); }
                else if ((r & 255) == 7) { bins[i].kind = K_TERM; bins[i].val = 0; }
                else {
                    bins[i].kind = K_DEC;
                    bins[i].ctx = (uint8_t)((r >> 4) % H265_NCTX);
                    bins[i].val = (uint8_t)((r >> 20) & 1);
                }
            }
            char nm[64];
            snprintf(nm, sizeof nm, "mixed 200k initType=%d qp=%d", t, q);
            roundtrip(nm, bins, n, t, q);
        }
    }

    /* 5. Strongly skewed data, which is what real residual coding looks like:
     *    long MPS runs punctuated by rare LPS, so the state saturates at 62/63
     *    and stays there. That is the region where an off-by-one in
     *    transIdxMps would otherwise never be reached. */
    for (int i = 0; i < 100000; i++) {
        bins[i].kind = K_DEC;
        bins[i].ctx = (uint8_t)(i % 40);
        bins[i].val = (uint8_t)((rnd() % 200) == 0);
    }
    roundtrip("skewed 100k", bins, 100000, 1, 32);
}

/* 9.3.2.2 by hand: preCtxState = Clip3(1, 126, ((m * Clip3(0,51,qp)) >> 4) + n)
 * with m = (initValue >> 4) * 5 - 45 and n = ((initValue & 15) << 3) - 16. */
static void expect_ctx(cabac_t *c, int idx, int init_value, int qp, const char *what)
{
    int m = (init_value >> 4) * 5 - 45;
    int n = ((init_value & 15) << 3) - 16;
    int q = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    int pre = ((m * q) >> 4) + n;
    if (pre < 1) pre = 1;
    if (pre > 126) pre = 126;
    int mps = pre <= 63 ? 0 : 1;
    int st = mps ? pre - 64 : 63 - pre;
    CHECK(c->state[idx] == ((st << 1) | mps),
          "%s: ctx %d at qp %d = state %d mps %d, want state %d mps %d",
          what, idx, qp, c->state[idx] >> 1, c->state[idx] & 1, st, mps);
}

static void test_context_init(void)
{
    cabac_t c;
    /* Spot-check the initValues that Tables 9-5..9-37 pin down, at three QPs.
     * These are the numbers a round trip can never catch. */
    for (int qp = 0; qp <= 51; qp += 17) {
        h265_cabac_init_ctx(&c, 0, qp);
        expect_ctx(&c, CTX_SAO_MERGE, 153, qp, "I sao_merge");
        expect_ctx(&c, CTX_SAO_TYPE_IDX, 200, qp, "I sao_type_idx");
        expect_ctx(&c, CTX_SPLIT_CU + 0, 139, qp, "I split_cu[0]");
        expect_ctx(&c, CTX_SPLIT_CU + 2, 157, qp, "I split_cu[2]");
        expect_ctx(&c, CTX_PART_MODE + 0, 184, qp, "I part_mode[0]");
        expect_ctx(&c, CTX_PREV_INTRA_LUMA, 184, qp, "I prev_intra_luma");
        expect_ctx(&c, CTX_INTRA_CHROMA, 63, qp, "I intra_chroma");
        expect_ctx(&c, CTX_CBF_LUMA + 0, 111, qp, "I cbf_luma[0]");
        expect_ctx(&c, CTX_CBF_CHROMA + 2, 182, qp, "I cbf_cb[2]");
        expect_ctx(&c, CTX_LAST_X + 17, 63, qp, "I last_x[17]");
        expect_ctx(&c, CTX_SUBBLOCK + 1, 171, qp, "I coded_sub_block[1]");
        expect_ctx(&c, CTX_SIG + 0, 111, qp, "I sig[0]");
        expect_ctx(&c, CTX_SIG + 41, 111, qp, "I sig[41]");
        expect_ctx(&c, CTX_GREATER1 + 21, 227, qp, "I greater1[21]");
        expect_ctx(&c, CTX_GREATER2 + 3, 167, qp, "I greater2[3]");

        h265_cabac_init_ctx(&c, 1, qp);
        expect_ctx(&c, CTX_SAO_TYPE_IDX, 185, qp, "P sao_type_idx");
        expect_ctx(&c, CTX_CU_SKIP + 0, 197, qp, "P cu_skip[0]");
        expect_ctx(&c, CTX_PRED_MODE, 149, qp, "P pred_mode");
        expect_ctx(&c, CTX_MERGE_FLAG, 110, qp, "P merge_flag");
        expect_ctx(&c, CTX_MERGE_IDX, 122, qp, "P merge_idx");
        expect_ctx(&c, CTX_INTER_PRED_IDC + 4, 31, qp, "P inter_pred_idc[4]");
        expect_ctx(&c, CTX_MVD_GREATER0, 140, qp, "P mvd_greater0");
        expect_ctx(&c, CTX_MVD_GREATER1, 198, qp, "P mvd_greater1");
        expect_ctx(&c, CTX_SPLIT_TRANSFORM + 2, 94, qp, "P split_transform[2]");
        expect_ctx(&c, CTX_SIG + 0, 155, qp, "P sig[0]");

        h265_cabac_init_ctx(&c, 2, qp);
        expect_ctx(&c, CTX_SAO_TYPE_IDX, 160, qp, "B sao_type_idx");
        expect_ctx(&c, CTX_PRED_MODE, 134, qp, "B pred_mode");
        expect_ctx(&c, CTX_MERGE_FLAG, 154, qp, "B merge_flag");
        expect_ctx(&c, CTX_MERGE_IDX, 137, qp, "B merge_idx");
        expect_ctx(&c, CTX_MVD_GREATER0, 169, qp, "B mvd_greater0");
        expect_ctx(&c, CTX_SPLIT_TRANSFORM + 0, 224, qp, "B split_transform[0]");
        expect_ctx(&c, CTX_SIG + 0, 170, qp, "B sig[0]");
        expect_ctx(&c, CTX_GREATER2 + 3, 107, qp, "B greater2[3]");
    }

    /* The three init types must actually differ where the tables say they do,
     * and agree where they say they agree -- a table pasted three times would
     * pass every expect_ctx above if the expectation were taken from the same
     * array. These come from the spec's text, not from init_values[]. */
    cabac_t i0, i1, i2;
    h265_cabac_init_ctx(&i0, 0, 30);
    h265_cabac_init_ctx(&i1, 1, 30);
    h265_cabac_init_ctx(&i2, 2, 30);
    CHECK(i0.state[CTX_SAO_MERGE] == i1.state[CTX_SAO_MERGE] &&
          i1.state[CTX_SAO_MERGE] == i2.state[CTX_SAO_MERGE],
          "sao_merge is 153 for all three init types");
    CHECK(i1.state[CTX_SPLIT_TRANSFORM] != i2.state[CTX_SPLIT_TRANSFORM],
          "split_transform differs between P (124) and B (224)");
    CHECK(i1.state[CTX_MVD_GREATER0] != i2.state[CTX_MVD_GREATER0],
          "abs_mvd_greater0 differs between P (140) and B (169)");
    CHECK(i1.state[CTX_MVD_GREATER1] == i2.state[CTX_MVD_GREATER1],
          "abs_mvd_greater1 is 198 for both P and B");
    CHECK(i0.state[CTX_SIG] != i1.state[CTX_SIG] &&
          i1.state[CTX_SIG] != i2.state[CTX_SIG],
          "sig_coeff_flag[0] is 111/155/170 across the three init types");
}

static void test_bounds(void)
{
    cabac_t c;
    uint8_t buf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    /* ivlOffset == 511 is not a decodable state (9.3.2.5 requires < 510). */
    h265_cabac_init_ctx(&c, 0, 26);
    CHECK(h265_cabac_start(&c, buf, 4, 0) != H265_OK,
          "an all-ones prefix must be rejected, not decoded");

    /* Reading past the end must stay in bounds and eventually set the error
     * flag rather than run forever or walk off the buffer. */
    uint8_t tiny[2] = { 0x00, 0x00 };
    h265_cabac_init_ctx(&c, 0, 26);
    CHECK(h265_cabac_start(&c, tiny, 2, 0) == H265_OK, "start on a 2-byte substream");
    for (int i = 0; i < 4000; i++) h265_cabac_bypass(&c);
    CHECK(c.error, "4000 bins past a 2-byte buffer must set the error flag");

    /* Starting past the end is a bad entry point, not a crash. */
    h265_cabac_init_ctx(&c, 0, 26);
    CHECK(h265_cabac_start(&c, tiny, 2, 5) != H265_OK,
          "an entry point beyond the substream must be rejected");
}

static void test_bypass_n(void)
{
    /* bypass_n must be MSB-first: the multi-bit reads (last-position
     * suffixes, Golomb-Rice suffixes, sao_band_position) all depend on it. */
    static uint8_t buf[4096];
    enc_t e;
    cabac_t c;
    h265_cabac_init_ctx(&c, 0, 26);
    memcpy(e.state, c.state, sizeof e.state);
    e_init(&e, buf, (int)sizeof buf);

    uint32_t vals[] = { 0, 1, 2, 7, 8, 255, 0x1234, 0x7FFFFFFFu };
    int nbits[]     = { 1, 1, 3, 3, 5,   8,     16,         31 };
    for (int i = 0; i < 8; i++)
        for (int b = nbits[i] - 1; b >= 0; b--)
            e_bypass(&e, (int)((vals[i] >> b) & 1));
    e_terminate(&e, 1);
    int len = e.bytepos + (e.bitpos ? 1 : 0);

    h265_cabac_init_ctx(&c, 0, 26);
    h265_cabac_start(&c, buf, len, 0);
    for (int i = 0; i < 8; i++) {
        uint32_t got = h265_cabac_bypass_n(&c, nbits[i]);
        CHECK(got == vals[i], "bypass_n(%d) = %u, want %u", nbits[i], got, vals[i]);
    }
}

int main(void)
{
    test_context_init();
    test_bounds();
    test_bypass_n();
    test_roundtrips();
    printf("h265_cabac_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
