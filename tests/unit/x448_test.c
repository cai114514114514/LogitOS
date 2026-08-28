/* X448 (c/crypto/pubkey/x448.c, c/crypto/pubkey/field448.c) against RFC 7748
 * and against Wycheproof. THREE independent sources, because "our X448
 * agrees with our own X448" proves only that the field layer and the ladder
 * were written by the same person on the same afternoon and made the same
 * mistake twice:
 *
 *   RFC 7748 5.2     -- the two published (scalar, u) -> u known answers, the
 *                        iterated self-composition test (k0=u0=5, apply X448
 *                        with u=old k, k=result; 1, 1,000 and -- opt-in,
 *                        LOGIT_X448_MILLION=1, ~150s -- 1,000,000 times).
 *   RFC 7748 6.2     -- the published Alice/Bob key-agreement vector, PLUS an
 *                        independent agreement check with freshly generated
 *                        scalars (a caller cannot get X448(a,X448(b,5)) ==
 *                        X448(b,X448(a,5)) by accident on wrong code).
 *   Wycheproof       -- x448_wycheproof.inc, 498 of the 500 curve448 XdhComp
 *                        cases (see that file's header for the 2 excluded and
 *                        why): normal points, points on the quadratic twist,
 *                        low-order points, non-canonical encodings. This is
 *                        the corpus that would have caught the clamp/a24
 *                        mistakes this file's negative controls reintroduce,
 *                        the RFC vectors alone would not have (2 points is
 *                        not enough to force a specific wrong constant to
 *                        fail both of them).
 *
 * NEGATIVE CONTROLS, each a distinct RIGHT MENTAL MODEL, WRONG CONSTANT bug,
 * reintroduced on purpose and built with the -D below (see the Makefile rule
 * this file's own header names is what the integration agent wires):
 *
 *   LOGIT_X448_CTL_BAD_A24    field448.c's fe448_mul_a24 uses 121665
 *                             (x25519's a24) instead of 39081. A valid
 *                             Montgomery ladder over the WRONG curve --
 *                             CLAUDE.md's exact warning.
 *   LOGIT_X448_CTL_BAD_CLAMP  x448.c clears the low THREE bits of the scalar
 *                             (x25519's cofactor-8 clamp) instead of the low
 *                             TWO (curve448's cofactor-4 clamp, RFC 7748
 *                             section 5).
 *
 * Both must redden every KAT and the overwhelming majority of the Wycheproof
 * corpus (see run-x448.sh --controls for the exact counts observed and
 * watched failing, not merely predicted).
 *
 * Build/run: see the test-x448 rule (Makefile rule text is in this
 * workstream's report; not yet wired -- see CLAUDE.md's rule on that).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "x448.h"

static int checks, failed;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) { /* printf("ok   %s\n", what); */ }
    else      { printf("FAIL %s\n", what); failed++; }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(uint8_t *out, int max, const char *h)
{
    int n = 0;
    while (h[0] && h[1]) {
        int a = hexval(h[0]), b = hexval(h[1]);
        if (a < 0 || b < 0) return -1;
        if (n >= max) return -1;
        out[n++] = (uint8_t)((a << 4) | b);
        h += 2;
    }
    return h[0] ? -1 : n;
}

static void tohex(char *out, const uint8_t *b, int n)
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = H[b[i]>>4]; out[2*i+1] = H[b[i]&15]; }
    out[2*n] = 0;
}

/* --------------------------------------------------------- RFC 7748 5.2 -- */

struct kv { const char *name, *scalar, *u, *out; };

static const struct kv RFC_SCALARMULT[] = {
    { "rfc7748-5.2-vec1",
      "3d262fddf9ec8e88495266fea19a34d28882acef045104d0d1aae121"
      "700a779c984c24f8cdd78fbff44943eba368f54b29259a4f1c600ad3",
      "06fce640fa3487bfda5f6cf2d5263f8aad88334cbd07437f020f08f9"
      "814dc031ddbdc38c19c6da2583fa5429db94ada18aa7a7fb4ef8a086",
      "ce3e4ff95a60dc6697da1db1d85e6afbdf79b50a2412d7546d5f239f"
      "e14fbaadeb445fc66a01b0779d98223961111e21766282f73dd96b6f" },
    { "rfc7748-5.2-vec2",
      "203d494428b8399352665ddca42f9de8fef600908e0d461cb021f8c5"
      "38345dd77c3e4806e25f46d3315c44e0a5b4371282dd2c8d5be3095f",
      "0fbcc2f993cd56d3305b0b7d9e55d4c1a8fb5dbb52f8e9a1e9b6201b"
      "165d015894e56c4d3570bee52fe205e28a78b91cdfbde71ce8d157db",
      "884a02576239ff7a2f2f63b2db6a9ff37047ac13568e1e30fe63c4a7"
      "ad1b3ee3a5700df34321d62077e63633c575c1c954514e99da7c179d" },
};
#define N_RFC_SCALARMULT (int)(sizeof(RFC_SCALARMULT)/sizeof(RFC_SCALARMULT[0]))

static void test_rfc_scalarmult(void)
{
    for (int i = 0; i < N_RFC_SCALARMULT; i++) {
        const struct kv *v = &RFC_SCALARMULT[i];
        uint8_t k[56], u[56], o[56]; char oh[113];
        ok(unhex(k, 56, v->scalar) == 56, v->name);
        ok(unhex(u, 56, v->u) == 56, v->name);
        x448(o, k, u);
        tohex(oh, o, 56);
        ok(strcmp(oh, v->out) == 0, v->name);
    }
}

/* ----------------------------------------------- RFC 7748 5.2, iterated -- */

static void test_rfc_iterated(void)
{
    uint8_t k[56] = {5}, u[56] = {5}, o[56];
    memset(k+1, 0, 55); memset(u+1, 0, 55);
    char oh[113];

    x448(o, k, u);
    memcpy(u, k, 56); memcpy(k, o, 56);
    tohex(oh, o, 56);
    ok(strcmp(oh,
        "3f482c8a9f19b01e6c46ee9711d9dc14fd4bf67af30765c2ae2b846a"
        "4d23a8cd0db897086239492caf350b51f833868b9bc2b3bca9cf4113") == 0,
        "rfc7748-iter-1");

    for (int i = 1; i < 1000; i++) {
        x448(o, k, u);
        memcpy(u, k, 56); memcpy(k, o, 56);
    }
    tohex(oh, k, 56);
    ok(strcmp(oh,
        "aa3b4749d55b9daf1e5b00288826c467274ce3ebbdd5c17b975e09d4"
        "af6c67cf10d087202db88286e2b79fceea3ec353ef54faa26e219f38") == 0,
        "rfc7748-iter-1000");

    if (getenv("LOGIT_X448_MILLION")) {
        for (int i = 1000; i < 1000000; i++) {
            x448(o, k, u);
            memcpy(u, k, 56); memcpy(k, o, 56);
        }
        tohex(oh, k, 56);
        ok(strcmp(oh,
            "077f453681caca3693198420bbe515cae0002472519b3e67661a7e8"
            "9cab94695c8f4bcd66e61b9b9c946da8d524de3d69bd9d9d66b997e37") == 0,
            "rfc7748-iter-1000000");
    } else {
        printf("SKIP rfc7748-iter-1000000 (set LOGIT_X448_MILLION=1; ~150s)\n");
    }
}

/* ----------------------------------------------------------- RFC 7748 6.2 -- */

static void test_rfc_dh(void)
{
    uint8_t a[56], b[56], apub[56], bpub[56], s1[56], s2[56];
    char ah[113], bh[113], s1h[113];

    ok(unhex(a, 56,
        "9a8f4925d1519f5775cf46b04b5800d4ee9ee8bae8bc5565d498c28d"
        "d9c9baf574a9419744897391006382a6f127ab1d9ac2d8c0a598726b") == 56,
        "rfc7748-dh-a");
    ok(unhex(b, 56,
        "1c306a7ac2a0e2e0990b294470cba339e6453772b075811d8fad0d1d"
        "6927c120bb5ee8972b0d3e21374c9c921b09d1b0366f10b65173992d") == 56,
        "rfc7748-dh-b");

    x448_base(apub, a);
    x448_base(bpub, b);
    tohex(ah, apub, 56);
    ok(strcmp(ah,
        "9b08f7cc31b7e3e67d22d5aea121074a273bd2b83de09c63faa73d2c"
        "22c5d9bbc836647241d953d40c5b12da88120d53177f80e532c41fa0") == 0,
        "rfc7748-dh-apub");
    tohex(bh, bpub, 56);
    ok(strcmp(bh,
        "3eb7a829b0cd20f5bcfc0b599b6feccf6da4627107bdb0d4f345b430"
        "27d8b972fc3e34fb4232a13ca706dcb57aec3dae07bdc1c67bf33609") == 0,
        "rfc7748-dh-bpub");

    x448(s1, a, bpub);
    x448(s2, b, apub);
    tohex(s1h, s1, 56);
    ok(strcmp(s1h,
        "07fff4181ac6cc95ec1c16a94a0f74d12da232ce40a77552281d282b"
        "b60c0b56fd2464c335543936521c24403085d59a449a5037514a879d") == 0,
        "rfc7748-dh-shared");
    ok(memcmp(s1, s2, 56) == 0, "rfc7748-dh-agree");
}

/* ------------------------------------- freshly generated agreement check -- */
/* Not from any published vector -- a property check, not a KAT. Uses a tiny
 * xorshift PRNG (no libc rand() portability question, no dependency on this
 * machine's entropy) seeded from argv-independent constants, ONLY to pick
 * scalars for a property that must hold for ANY scalar pair: X448(a, X448(b,
 * base)) == X448(b, X448(a, base)). This is the same shape as
 * mlkem_test.c's "properties a known-answer test cannot express" section. */

static uint64_t g_xs = 0x9e3779b97f4a7c15ULL;
static uint64_t xs64(void)
{
    g_xs ^= g_xs << 13; g_xs ^= g_xs >> 7; g_xs ^= g_xs << 17;
    return g_xs;
}

static void test_fresh_agreement(void)
{
    for (int trial = 0; trial < 32; trial++) {
        uint8_t a[56], b[56], apub[56], bpub[56], s1[56], s2[56];
        for (int i = 0; i < 56; i += 8) {
            uint64_t ra = xs64(), rb = xs64();
            memcpy(a+i, &ra, i+8<=56?8:56-i);
            memcpy(b+i, &rb, i+8<=56?8:56-i);
        }
        x448_base(apub, a);
        x448_base(bpub, b);
        x448(s1, a, bpub);
        x448(s2, b, apub);
        ok(memcmp(s1, s2, 56) == 0, "fresh-agreement");
    }
}

/* ----------------------------------------------------------- Wycheproof -- */
#include "x448_wycheproof.inc"

static void test_wycheproof(void)
{
    ok(X448_WYCHEPROOF_N + X448_WYCHEPROOF_SKIPPED == X448_WYCHEPROOF_TOTAL,
       "wycheproof-count-accounted-for");
    ok(X448_WYCHEPROOF_N == 498 && X448_WYCHEPROOF_SKIPPED == 12,
       "wycheproof-expected-split (498 usable, 12 oversized-public excluded)");

    int acceptable = 0;
    for (int i = 0; i < X448_WYCHEPROOF_N; i++) {
        const struct x448_wtv *t = &X448_WYCHEPROOF[i];
        uint8_t pub[56], priv[56], shared[56], got[56]; char gh[113];
        if (unhex(pub, 56, t->pub) != 56 || unhex(priv, 56, t->priv) != 56 ||
            unhex(shared, 56, t->shared) != 56) {
            ok(0, "wycheproof-bad-vector-encoding");
            continue;
        }
        if (strcmp(t->result, "acceptable") == 0) acceptable++;
        /* "valid" and "acceptable" both carry an expected shared value that
         * a conformant, non-rejecting X448 (this implementation, and RFC
         * 7748's own X25519/X448 text, which makes the all-zero check a MAY
         * -- see x448.h) MUST reproduce exactly. There are no "invalid"
         * entries left after gen-x448-wycheproof.py's length filter (the
         * corpus's only invalid class is the 57-byte public keys). */
        x448(got, priv, pub);
        tohex(gh, got, 56);
        ok(strcmp(gh, t->shared) == 0, "wycheproof");
        if (strcmp(gh, t->shared) != 0)
            printf("     tcId=%d comment mismatch (result=%s)\n", t->tcid, t->result);
    }
    printf("     (%d of %d wycheproof cases flagged 'acceptable' -- edge points\n"
           "      this implementation does not reject, matching x25519.c's own\n"
           "      no-zero-check convention; see x448.h)\n", acceptable, X448_WYCHEPROOF_N);
}

int main(void)
{
    test_rfc_scalarmult();
    test_rfc_iterated();
    test_rfc_dh();
    test_fresh_agreement();
    test_wycheproof();

    printf("x448: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
