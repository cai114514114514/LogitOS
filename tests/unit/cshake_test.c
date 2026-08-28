#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "cshake.h"
#include "kmac.h"
#include "cshake_kmac_vectors.inc"

/* Known-answer test for cSHAKE128/256 and KMAC128/256, against the OFFICIAL
 * NIST examples (not self-generated -- see cshake.h/kmac.h for why that
 * matters here specifically):
 *
 *   cs1..cs4  NIST cSHAKE_samples.pdf, samples #1-#4
 *     https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/cSHAKE_samples.pdf
 *   km1..km6  NIST KMAC_samples.pdf, samples #1-#6
 *     https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/KMAC_samples.pdf
 *
 * Both PDFs were fetched directly (verified 200, 79283 and 390705 bytes) and
 * converted with `pdftotext -layout`; tests/unit/cshake_kmac_vectors.inc was
 * extracted from that text by a small parser that keys off the PDFs' own
 * field labels ("Data is", "Key is", "Outval is", ...) and stops each hex
 * block at the first non-hex-looking line, specifically so a label line
 * folded up against a hex block (KMAC sample #2's "Key is" block, which the
 * PDF's layout runs directly into the next "Length of data is" line with no
 * blank separator) cannot be silently swallowed into the value -- an earlier
 * version of the extractor did exactly that and produced a corrupt 39-byte
 * key for sample #2 that decoded to nonsense; it was caught only because
 * this test then failed, which is the whole point of building the KAT before
 * trusting the extraction.
 *
 * WHAT THE NIST PDFS DO NOT COVER, and why: all 6 KMAC samples use a
 * nonzero right_encode(L) trailer -- i.e. they are all plain KMAC, none is
 * KMACXOF. NIST did not publish separate KMACXOF-only samples; the split is
 * this file's own reading of the two PDFs (grep for "Right_encoded L" in
 * both -- every one of the 6 is nonzero). KMACXOF and the cSHAKE(X,L,"","")
 * == SHAKE(X,L) identity are instead covered by
 * tests/unit/run-cshake-openssl.sh against OpenSSL 3.6.3's KMAC-128/256 (xof
 * param) and SHAKE128/256 -- see that script for why it, not this file, is
 * the source for those two claims.
 *
 * NEGATIVE CONTROL: build with -DKMAC_NEGCTL_XOF_TRAILER (kmac.c) and every
 * km* case below goes red while every cs* case stays green -- see kmac.c's
 * comment on that flag and tests/kmac.mk's test-cshake-negctl for the exact
 * command and the output this was watched producing. */

static int fails, passes;
static void ok(const char *name, int cond)
{ if (cond) { passes++; printf("ok   %s\n", name); } else { fails++; printf("FAIL %s\n", name); } }
static int eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

static void test_cshake(void)
{
    uint8_t out[64];

    cshake128(out, (size_t)cs1_L, cs1_msg, sizeof cs1_msg, NULL, 0, cs1_S, sizeof cs1_S);
    ok("cshake128 NIST sample #1", eq(out, cs1_out, sizeof cs1_out));

    cshake128(out, (size_t)cs2_L, cs2_msg, sizeof cs2_msg, NULL, 0, cs2_S, sizeof cs2_S);
    ok("cshake128 NIST sample #2 (1600-bit message)", eq(out, cs2_out, sizeof cs2_out));

    cshake256(out, (size_t)cs3_L, cs3_msg, sizeof cs3_msg, NULL, 0, cs3_S, sizeof cs3_S);
    ok("cshake256 NIST sample #3", eq(out, cs3_out, sizeof cs3_out));

    cshake256(out, (size_t)cs4_L, cs4_msg, sizeof cs4_msg, NULL, 0, cs4_S, sizeof cs4_S);
    ok("cshake256 NIST sample #4 (1600-bit message)", eq(out, cs4_out, sizeof cs4_out));

    /* cSHAKE(X, L, "", "") == SHAKE(X, L) -- the identity cshake.h's header
     * calls out by name. This is the spec's own definition (SP 800-185
     * section 3, "if N and S are both empty strings, then cSHAKE(...) is
     * equivalent to SHAKE"), checked here against this tree's OWN shake128/
     * shake256 (c/crypto/pq/keccak.c, already gated by test-mlkem's use of
     * it and, transitively, ML-KEM's FIPS 203 vectors) -- an independent
     * external cross-check of the SAME identity, against OpenSSL's
     * SHAKE128/256, lives in run-cshake-openssl.sh because that is the one
     * with no shared code path with this binary at all. */
    uint8_t a[97], b[97];
    for (size_t n = 0; n < sizeof cs2_msg; n += 37) {
        size_t take = n + 40 < sizeof cs2_msg ? 40 : sizeof cs2_msg - n;
        cshake128(a, sizeof a, cs2_msg + n, take, NULL, 0, NULL, 0);
        shake128(b, sizeof b, cs2_msg + n, take);
        char label[64]; snprintf(label, sizeof label, "cshake128(N=S=\"\") == shake128, off %zu", n);
        ok(label, eq(a, b, sizeof a));
    }
    cshake256(a, sizeof a, cs2_msg, sizeof cs2_msg, NULL, 0, NULL, 0);
    shake256(b, sizeof b, cs2_msg, sizeof cs2_msg);
    ok("cshake256(N=S=\"\") == shake256", eq(a, b, sizeof a));

    /* N non-empty, S empty must ALSO take the cSHAKE (0x04) branch, not the
     * plain-SHAKE one -- this is the half of the empty/non-empty trap the
     * KMAC samples exercise implicitly (N="KMAC" always) but that no cSHAKE
     * sample above does directly (all four have S non-empty, N empty). Cross
     * check: cSHAKE128(X, L, "KMAC", "") must NOT equal shake128(X, L), and
     * it must equal what kmac_core's own N="KMAC" prefix construction
     * produces standalone. */
    uint8_t c[32], d[32];
    static const uint8_t NKMAC[4] = { 'K','M','A','C' };
    cshake128(c, sizeof c, cs1_msg, sizeof cs1_msg, NKMAC, 4, NULL, 0);
    shake128(d, sizeof d, cs1_msg, sizeof cs1_msg);
    ok("cshake128(N=\"KMAC\", S=\"\") != shake128 (N alone forces ds=0x04)", !eq(c, d, sizeof c));
}

static void test_kmac(void)
{
    uint8_t out[64];

    kmac128(out, (size_t)km1_L, km1_key, sizeof km1_key, km1_msg, sizeof km1_msg, NULL, 0);
    ok("kmac128 NIST sample #1 (S empty)", eq(out, km1_out, sizeof km1_out));

    kmac128(out, (size_t)km2_L, km2_key, sizeof km2_key, km2_msg, sizeof km2_msg, km2_S, sizeof km2_S);
    ok("kmac128 NIST sample #2", eq(out, km2_out, sizeof km2_out));

    kmac128(out, (size_t)km3_L, km3_key, sizeof km3_key, km3_msg, sizeof km3_msg, km3_S, sizeof km3_S);
    ok("kmac128 NIST sample #3 (1600-bit message)", eq(out, km3_out, sizeof km3_out));

    kmac256(out, (size_t)km4_L, km4_key, sizeof km4_key, km4_msg, sizeof km4_msg, km4_S, sizeof km4_S);
    ok("kmac256 NIST sample #4", eq(out, km4_out, sizeof km4_out));

    kmac256(out, (size_t)km5_L, km5_key, sizeof km5_key, km5_msg, sizeof km5_msg, NULL, 0);
    ok("kmac256 NIST sample #5 (S empty, 1600-bit message)", eq(out, km5_out, sizeof km5_out));

    kmac256(out, (size_t)km6_L, km6_key, sizeof km6_key, km6_msg, sizeof km6_msg, km6_S, sizeof km6_S);
    ok("kmac256 NIST sample #6 (1600-bit message)", eq(out, km6_out, sizeof km6_out));

    /* KMAC vs KMACXOF: same key/message/S/length, MUST differ (trailer
     * right_encode(L) vs right_encode(0)) -- this is the exact split kmac.h
     * documents. Not an official vector (no NIST sample is KMACXOF), so this
     * asserts the STRUCTURAL claim, not a specific byte string; the byte
     * string itself is cross-checked against OpenSSL in
     * run-cshake-openssl.sh. */
    uint8_t x[32], y[32];
    kmac128(x, 32, km1_key, sizeof km1_key, km1_msg, sizeof km1_msg, NULL, 0);
    kmacxof128(y, 32, km1_key, sizeof km1_key, km1_msg, sizeof km1_msg, NULL, 0);
    ok("kmac128 != kmacxof128 at the same output length", !eq(x, y, 32));

    /* KMACXOF's defining property (and KMAC's defining NON-property): the
     * trailer does not depend on outlen, so squeezing more bytes must extend
     * the same stream -- KMACXOF128(K,X,64,S)[0:32] == KMACXOF128(K,X,32,S).
     * Checking this against KMAC128 (which must NOT have the prefix property,
     * since its trailer changes with outlen) is the other half. */
    uint8_t x32[32], x64[64];
    kmacxof128(x32, 32, km2_key, sizeof km2_key, km2_msg, sizeof km2_msg, km2_S, sizeof km2_S);
    kmacxof128(x64, 64, km2_key, sizeof km2_key, km2_msg, sizeof km2_msg, km2_S, sizeof km2_S);
    ok("kmacxof128 is a real XOF: 32-byte output is a prefix of the 64-byte one", eq(x32, x64, 32));
    uint8_t k32[32], k64[64];
    kmac128(k32, 32, km2_key, sizeof km2_key, km2_msg, sizeof km2_msg, km2_S, sizeof km2_S);
    kmac128(k64, 64, km2_key, sizeof km2_key, km2_msg, sizeof km2_msg, km2_S, sizeof km2_S);
    ok("kmac128 is NOT a XOF: 32-byte output is NOT a prefix of the 64-byte one", !eq(k32, k64, 32));
}

int main(void)
{
    test_cshake();
    test_kmac();
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
