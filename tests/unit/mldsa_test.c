/* ML-DSA (FIPS 204) host unit test, over OFFICIAL NIST ACVP vectors.
 *
 * FIPS 204 ITSELF CARRIES NO WORKED-EXAMPLE APPENDIX (unlike, say, FIPS 197's
 * AES). The authoritative machine-readable vectors are NIST's own ACVP-Server
 * repository (github.com/usnistgov/ACVP-Server), gen-val/json-files/
 * ML-DSA-{keyGen,sigGen,sigVer}-FIPS204/{prompt,expectedResults}.json --
 * tests/unit/mldsa_kat.inc is a hand-picked, hashed-down subset of those
 * (small enough to embed; see that file's own header for exactly which
 * tcIds and why). This is the SAME evidentiary standard ML-KEM's
 * mlkem_kat.inc is held to (a known answer from a source this file did not
 * write), just from ACVP directly rather than by first checking OpenSSL's
 * agreement -- OpenSSL is exercised separately by
 * tests/unit/run-mldsa-openssl.sh, which is the differential the sign-loop
 * subtleties below actually need (see that file's header).
 *
 * THREE KINDS OF VECTOR, THREE DIFFERENT THINGS THEY PROVE:
 *  - keyGen: our pk/sk from a seed must hash to what ACVP recorded. Exercises
 *    ExpandA, ExpandS, the NTT, Power2Round and every encode function keygen
 *    touches -- but NOT the sign-loop rejection logic at all.
 *  - sigGen (DETERMINISTIC groups only, rnd = 0^32): our signature over a
 *    given sk/message/context must hash to what ACVP recorded. This is the
 *    ONLY vector kind that exercises the rejection loop end to end -- keyGen
 *    vectors cannot, and a round-trip self-test cannot distinguish "rejects
 *    correctly" from "never rejects and got lucky" (see mldsa.c's header on
 *    why a dropped rejection check is the dangerous failure mode).
 *  - sigVer: a given (pk, message, context, signature) must verify to the
 *    recorded testPassed. Includes at least one PASS and one FAIL case per
 *    parameter set -- a suite that only ever fed itself signatures it just
 *    produced would never exercise Verify's independent rejection paths
 *    (malformed hint encoding, out-of-range z, wrong challenge hash).
 *
 * WHAT NEITHER GATE CAN SEE: constant-time behaviour. See mldsa.c's header --
 * modular reduction and the rejection loop are both documented as NOT proven
 * constant time, and nothing here measures timing.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mldsa.h"
#include "keccak.h"

#include "mldsa_kat.inc"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) { pass++; }
    else { fail++; printf("FAIL: %s\n", what); }
}

static void unhex(uint8_t *o, const char *h, int n)
{
    for (int i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
}
static void tohex(char *o, const uint8_t *b, int n)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o[2*i] = d[b[i] >> 4]; o[2*i+1] = d[b[i] & 15]; }
    o[2*n] = 0;
}
void sha256(const void *data, size_t len, uint8_t out[32]);  /* c/crypto/hash/sha256.c */
static void digest_hex(char *out, const uint8_t *b, int n)
{
    uint8_t h[32];
    sha256(b, (size_t)n, h);
    tohex(out, h, 32);
}

/* -------------------------------------------------------------- keyGen */

static void run_keygen(const mldsa_params *p, const char *seedhex,
                        const char *pk_sha256, const char *sk_sha256, const char *tag)
{
    static uint8_t pk[MLDSA87_PK], sk[MLDSA87_SK];
    uint8_t xi[32];
    char hex[128], what[128];

    unhex(xi, seedhex, 32);
    mldsa_keygen_internal(p, xi, pk, sk);

    digest_hex(hex, pk, p->pk_bytes);
    snprintf(what, sizeof what, "%s keygen: pk matches ACVP", tag);
    ck(!strcmp(hex, pk_sha256), what);

    digest_hex(hex, sk, p->sk_bytes);
    snprintf(what, sizeof what, "%s keygen: sk matches ACVP", tag);
    ck(!strcmp(hex, sk_sha256), what);
}

/* -------------------------------------------------------------- sigGen */

static int hexlen(const char *h) { return (int)(strlen(h) / 2); }

static void run_siggen(const mldsa_params *p, const char *skhex, const char *msghex,
                        const char *ctxhex, const char *sig_sha256, const char *tag)
{
    static uint8_t sk[MLDSA87_SK], msg[4096], ctx[256], sig[MLDSA87_SIG];
    uint8_t rnd[32] = {0};   /* deterministic variant: rnd = 0^32 */
    char hex[128], what[160];

    int sklen = hexlen(skhex), mlen = hexlen(msghex), ctxlen = hexlen(ctxhex);
    unhex(sk, skhex, sklen);
    if (mlen) unhex(msg, msghex, mlen);
    if (ctxlen) unhex(ctx, ctxhex, ctxlen);

    ck(sklen == p->sk_bytes, "sigGen: KAT sk length matches the parameter set");

    int rc = mldsa_sign(p, sk, msg, (size_t)mlen, ctx, (size_t)ctxlen, rnd, sig);
    snprintf(what, sizeof what, "%s sigGen: sign succeeds", tag);
    ck(rc == 0, what);
    if (rc != 0) return;

    digest_hex(hex, sig, p->sig_bytes);
    snprintf(what, sizeof what, "%s sigGen: signature matches ACVP (deterministic)", tag);
    ck(!strcmp(hex, sig_sha256), what);

    /* Re-signing the SAME (sk, message, ctx) with rnd=0 must reproduce the
     * exact same bytes -- the deterministic variant is a function, not a
     * distribution. A hedged implementation that ignored rnd would pass the
     * ACVP hash check by accident only if it always drew the same "random"
     * bytes, which is precisely the bug this line is cheap insurance against. */
    static uint8_t sig2[MLDSA87_SIG];
    mldsa_sign(p, sk, msg, (size_t)mlen, ctx, (size_t)ctxlen, rnd, sig2);
    snprintf(what, sizeof what, "%s sigGen: deterministic signing is reproducible", tag);
    ck(!memcmp(sig, sig2, (size_t)p->sig_bytes), what);

    /* And it must verify against the sk's own public key -- pk_from_sk isn't
     * implemented, so derive pk the honest way: keygen from the same xi is
     * not available (sigGen vectors hand us sk directly, not a seed), so
     * instead check the cheaper thing this CAN check without a real pk: that
     * corrupting the signature makes it fail against ITSELF is done below,
     * in run_sigver against a real ACVP (pk, sig) pair. */
    (void)tag;
}

/* -------------------------------------------------------------- sigVer */

static void run_sigver(const mldsa_params *p, const char *pkhex, const char *msghex,
                        const char *ctxhex, const char *sighex, int expect, const char *tag)
{
    static uint8_t pk[MLDSA87_PK], msg[4096], ctx[256], sig[MLDSA87_SIG];
    char what[160];

    int pklen = hexlen(pkhex), mlen = hexlen(msghex), ctxlen = hexlen(ctxhex), siglen = hexlen(sighex);
    unhex(pk, pkhex, pklen);
    if (mlen) unhex(msg, msghex, mlen);
    if (ctxlen) unhex(ctx, ctxhex, ctxlen);
    unhex(sig, sighex, siglen);

    ck(pklen == p->pk_bytes, "sigVer: KAT pk length matches the parameter set");
    ck(siglen == p->sig_bytes, "sigVer: KAT signature length matches the parameter set");

    int ok = mldsa_verify(p, pk, msg, (size_t)mlen, ctx, (size_t)ctxlen, sig);
    snprintf(what, sizeof what, "%s sigVer: verify == ACVP's testPassed (%s)",
             tag, expect ? "PASS" : "FAIL");
    ck((ok != 0) == (expect != 0), what);

    if (expect) {
        /* And the same signature with one bit flipped must NOT verify --
         * the cheapest possible check that Verify is not "return true". */
        sig[siglen / 2] ^= 0x01;
        int ok2 = mldsa_verify(p, pk, msg, (size_t)mlen, ctx, (size_t)ctxlen, sig);
        snprintf(what, sizeof what, "%s sigVer: a corrupted PASS signature is refused", tag);
        ck(ok2 == 0, what);
    }
}

int main(void)
{
    /* -------------------------------------------------------- 1. keyGen */
    run_keygen(&mldsa44_params, KAT_KG44_1_SEED, KAT_KG44_1_PK_SHA256, KAT_KG44_1_SK_SHA256, "44/1");
    run_keygen(&mldsa44_params, KAT_KG44_2_SEED, KAT_KG44_2_PK_SHA256, KAT_KG44_2_SK_SHA256, "44/2");
    run_keygen(&mldsa65_params, KAT_KG65_1_SEED, KAT_KG65_1_PK_SHA256, KAT_KG65_1_SK_SHA256, "65/1");
    run_keygen(&mldsa65_params, KAT_KG65_2_SEED, KAT_KG65_2_PK_SHA256, KAT_KG65_2_SK_SHA256, "65/2");
    run_keygen(&mldsa87_params, KAT_KG87_1_SEED, KAT_KG87_1_PK_SHA256, KAT_KG87_1_SK_SHA256, "87/1");
    run_keygen(&mldsa87_params, KAT_KG87_2_SEED, KAT_KG87_2_PK_SHA256, KAT_KG87_2_SK_SHA256, "87/2");

    /* -------------------------------------------------------- 2. sigGen */
    run_siggen(&mldsa44_params, KAT_SG44_1_SK, KAT_SG44_1_MSG, KAT_SG44_1_CTX, KAT_SG44_1_SIG_SHA256, "44/1");
    run_siggen(&mldsa44_params, KAT_SG44_2_SK, KAT_SG44_2_MSG, KAT_SG44_2_CTX, KAT_SG44_2_SIG_SHA256, "44/2");
    run_siggen(&mldsa65_params, KAT_SG65_1_SK, KAT_SG65_1_MSG, KAT_SG65_1_CTX, KAT_SG65_1_SIG_SHA256, "65/1");
    run_siggen(&mldsa65_params, KAT_SG65_2_SK, KAT_SG65_2_MSG, KAT_SG65_2_CTX, KAT_SG65_2_SIG_SHA256, "65/2");
    run_siggen(&mldsa87_params, KAT_SG87_1_SK, KAT_SG87_1_MSG, KAT_SG87_1_CTX, KAT_SG87_1_SIG_SHA256, "87/1");
    run_siggen(&mldsa87_params, KAT_SG87_2_SK, KAT_SG87_2_MSG, KAT_SG87_2_CTX, KAT_SG87_2_SIG_SHA256, "87/2");

    /* -------------------------------------------------------- 3. sigVer */
    run_sigver(&mldsa44_params, KAT_SV44_1_PK, KAT_SV44_1_MSG, KAT_SV44_1_CTX, KAT_SV44_1_SIG, KAT_SV44_1_EXPECT, "44/1");
    run_sigver(&mldsa44_params, KAT_SV44_2_PK, KAT_SV44_2_MSG, KAT_SV44_2_CTX, KAT_SV44_2_SIG, KAT_SV44_2_EXPECT, "44/2");
    run_sigver(&mldsa65_params, KAT_SV65_1_PK, KAT_SV65_1_MSG, KAT_SV65_1_CTX, KAT_SV65_1_SIG, KAT_SV65_1_EXPECT, "65/1");
    run_sigver(&mldsa65_params, KAT_SV65_2_PK, KAT_SV65_2_MSG, KAT_SV65_2_CTX, KAT_SV65_2_SIG, KAT_SV65_2_EXPECT, "65/2");
    run_sigver(&mldsa87_params, KAT_SV87_1_PK, KAT_SV87_1_MSG, KAT_SV87_1_CTX, KAT_SV87_1_SIG, KAT_SV87_1_EXPECT, "87/1");
    run_sigver(&mldsa87_params, KAT_SV87_2_PK, KAT_SV87_2_MSG, KAT_SV87_2_CTX, KAT_SV87_2_SIG, KAT_SV87_2_EXPECT, "87/2");

    /* --------------------------------------------------- 4. own round trip */
    /* Not a substitute for the ACVP vectors above (a round trip cannot see a
     * dropped rejection check -- see this file's header) but cheap coverage
     * that keygen -> sign -> verify agrees with ITSELF across many messages
     * and all three parameter sets, including empty message and empty ctx. */
    {
        const mldsa_params *sets[3] = { &mldsa44_params, &mldsa65_params, &mldsa87_params };
        for (int s = 0; s < 3; s++) {
            const mldsa_params *p = sets[s];
            static uint8_t pk[MLDSA87_PK], sk[MLDSA87_SK], sig[MLDSA87_SIG];
            for (int t = 0; t < 6; t++) {
                uint8_t xi[32], rnd[32], msg[64], ctx[8];
                for (int i = 0; i < 32; i++) { xi[i] = (uint8_t)(t * 7 + i); rnd[i] = (uint8_t)(t * 11 + i + 1); }
                for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(t * 13 + i);
                for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(t + i);
                int mlen = t * 9;             /* varies, including 0 */
                int ctxlen = t;                /* varies, including 0 */

                mldsa_keygen_internal(p, xi, pk, sk);
                int rc = mldsa_sign(p, sk, msg, (size_t)mlen, ctx, (size_t)ctxlen, rnd, sig);
                char what[96];
                snprintf(what, sizeof what, "%s round trip: sign succeeds (t=%d)", p->name, t);
                ck(rc == 0, what);
                if (rc != 0) continue;

                int ok = mldsa_verify(p, pk, msg, (size_t)mlen, ctx, (size_t)ctxlen, sig);
                snprintf(what, sizeof what, "%s round trip: verify accepts own signature (t=%d)", p->name, t);
                ck(ok == 1, what);

                /* Wrong context must be refused -- meaningless when ctxlen==0
                 * (an empty context is not in M' at all, so mutating an
                 * unused byte of the ctx buffer changes nothing; t=0 is
                 * exactly that case). */
                if (ctxlen > 0) {
                    ctx[0] ^= 0xFF;
                    int ok2 = mldsa_verify(p, pk, msg, (size_t)mlen, ctx, (size_t)ctxlen, sig);
                    snprintf(what, sizeof what, "%s round trip: wrong context is refused (t=%d)", p->name, t);
                    ck(ok2 == 0, what);
                }
            }
        }
    }

    printf("mldsa: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
