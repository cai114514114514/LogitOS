/* ML-KEM-768 (FIPS 203) host unit test.
 *
 * TWO KINDS OF CHECK LIVE HERE AND THEY ARE NOT INTERCHANGEABLE:
 *
 *  1. KNOWN ANSWERS produced by OpenSSL 3.5.5 (build/tlsx/gen_kat.sh). These are
 *     the reference. The generator REFUSES to emit a block unless openssl
 *     agrees with us on all four values first -- our ek must equal openssl's ek
 *     for the same seed, openssl must decapsulate our ciphertext to our shared
 *     secret, and openssl must produce the same implicitly-rejected secret for
 *     a corrupted one. So these constants are openssl's answers, not a
 *     photograph of ours.
 *
 *  2. PROPERTIES that a known-answer test cannot express -- that a corrupted
 *     ciphertext still yields SOME secret rather than an error, that the secret
 *     it yields differs from the true one, that an out-of-range encapsulation
 *     key is refused. These are checked over many random inputs.
 *
 * The full differential against a live openssl is a separate gate
 * (tests/unit/run-mlkem-openssl.sh, `make test-mlkem-openssl`); it is the
 * stronger test and the slower one. This file is the fast gate and needs no
 * openssl at runtime, which is what lets it run under ASan/UBSan in CI.
 *
 * WHAT NEITHER GATE CAN SEE, said plainly: constant-time behaviour. The
 * comparison and select in decaps are written branch-free and argued in
 * mlkem.c, but nothing here measures timing, so that property rests on reading
 * the code rather than on a number.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mlkem.h"
#include "keccak.h"

#include "mlkem_kat.inc"

static int pass, fail;
static void ck(int cond, const char *what)
{
    if (cond) { pass++; }
    else { fail++; printf("FAIL: %s\n", what); }
}

static void unhex(uint8_t *o, const char *h, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v;
    }
}
static void tohex(char *o, const uint8_t *b, int n)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o[2*i] = d[b[i] >> 4]; o[2*i+1] = d[b[i] & 15]; }
    o[2*n] = 0;
}
/* The KAT digests are SHA-256 (openssl's), so this file links c/crypto/hash's
 * sha256.c -- one extra TU, and it keeps the vector 64 hex digits instead of
 * 7,168. Declared here rather than including crypto.h, which drags in the
 * kernel's SIMD dispatch. */
void sha256(const void *data, size_t len, uint8_t out[32]);

static void digest_hex(char *out, const uint8_t *b, int n)
{
    uint8_t h[32];
    sha256(b, (size_t)n, h);
    tohex(out, h, 32);
}

int main(void)
{
    static uint8_t ek[MLKEM768_EK], dk[MLKEM768_DK];
    static uint8_t ct[MLKEM768_CT], ss[MLKEM768_SS], ss2[MLKEM768_SS];
    uint8_t d[32], z[32], m[32];
    char hex[8192];

    /* ---------------------------------------------------- 1. known answers */
    unhex(d, KAT_D, 32); unhex(z, KAT_Z, 32); unhex(m, KAT_M, 32);
    mlkem768_keygen_derand(d, z, ek, dk);

    digest_hex(hex, ek, MLKEM768_EK);
    ck(!strcmp(hex, KAT_EK_SHA256), "keygen: ek matches the OpenSSL vector");
    digest_hex(hex, dk, MLKEM768_DK);
    ck(!strcmp(hex, KAT_DK_SHA256), "keygen: dk matches the OpenSSL vector");

    ck(mlkem768_encaps_derand(ek, m, ct, ss) == 0, "encaps: accepts a valid ek");
    digest_hex(hex, ct, MLKEM768_CT);
    ck(!strcmp(hex, KAT_CT_SHA256), "encaps: ciphertext matches the vector");
    tohex(hex, ss, MLKEM768_SS);
    ck(!strcmp(hex, KAT_SS), "encaps: shared secret matches the vector");

    mlkem768_decaps(dk, ct, ss2);
    ck(!memcmp(ss, ss2, MLKEM768_SS), "decaps: recovers the encapsulated secret");

    /* Implicit rejection, against the value OPENSSL produced for the same
     * corrupted ciphertext. This is the check that a self-test cannot make:
     * the rejected secret is J(z || ct), which neither implementation can
     * guess, so agreeing on it is real evidence the FO transform matches. */
    {
        uint8_t bad[MLKEM768_CT];
        memcpy(bad, ct, MLKEM768_CT);
        bad[KAT_REJ_BYTE] ^= 0x01;
        mlkem768_decaps(dk, bad, ss2);
        tohex(hex, ss2, MLKEM768_SS);
        ck(!strcmp(hex, KAT_REJ_SS), "decaps: implicit rejection matches OpenSSL");
        ck(memcmp(ss2, ss, MLKEM768_SS) != 0,
           "decaps: the rejected secret is NOT the real one");
    }

    /* ------------------------------------------------------- 2. properties */

    /* Round trip over many independent keys. The seeds are deterministic so a
     * failure is reproducible; they are not a vector, they are coverage. */
    for (int t = 0; t < 24; t++) {
        for (int i = 0; i < 32; i++) { d[i] = (uint8_t)(t * 7 + i); z[i] = (uint8_t)(t * 13 + i); }
        for (int i = 0; i < 32; i++) m[i] = (uint8_t)(t * 31 + i);
        mlkem768_keygen_derand(d, z, ek, dk);
        if (mlkem768_encaps_derand(ek, m, ct, ss) != 0) { ck(0, "encaps rejected its own ek"); continue; }
        mlkem768_decaps(dk, ct, ss2);
        ck(!memcmp(ss, ss2, MLKEM768_SS), "round trip: decaps == encaps");
    }

    /* IMPLICIT REJECTION IS NOT AN ERROR. Corrupt every region of the
     * ciphertext -- the u part (c1) and the v part (c2) fail differently -- and
     * require a secret every time, never a failure, and never the real one.
     * mlkem768_decaps has no return value at all, which is the API expressing
     * this; the check here is that the VALUE changed and stayed unguessable. */
    for (int i = 0; i < 32; i++) { d[i] = (uint8_t)(0xA0 + i); z[i] = (uint8_t)(0x5F - i); }
    for (int i = 0; i < 32; i++) m[i] = (uint8_t)(i * 3 + 1);
    mlkem768_keygen_derand(d, z, ek, dk);
    mlkem768_encaps_derand(ek, m, ct, ss);
    {
        int offs[] = { 0, 1, 359, 960, 961, 1000, MLKEM768_CT - 1 };
        for (unsigned k = 0; k < sizeof offs / sizeof offs[0]; k++) {
            uint8_t bad[MLKEM768_CT];
            memcpy(bad, ct, MLKEM768_CT);
            bad[offs[k]] ^= 0x80;
            mlkem768_decaps(dk, bad, ss2);
            ck(memcmp(ss2, ss, MLKEM768_SS) != 0, "corrupt ct -> a different secret");
            /* and it must not be all zeroes, which is what a decaps that gave
             * up half way would leave in the caller's buffer */
            uint8_t acc = 0;
            for (int i = 0; i < MLKEM768_SS; i++) acc |= ss2[i];
            ck(acc != 0, "corrupt ct -> a nonzero secret (not an abandoned buffer)");
        }
    }

    /* The SAME corrupted ciphertext must always give the SAME secret: implicit
     * rejection is deterministic in (z, ct), not random. A version that drew
     * fresh randomness would pass every test above and break resumption-free
     * retries in ways nobody would trace back here. */
    {
        uint8_t bad[MLKEM768_CT], a[32], b[32];
        memcpy(bad, ct, MLKEM768_CT); bad[7] ^= 0x11;
        mlkem768_decaps(dk, bad, a);
        mlkem768_decaps(dk, bad, b);
        ck(!memcmp(a, b, 32), "implicit rejection is deterministic in (z, ct)");
    }

    /* FIPS 203 7.2 modulus check. Every 12-bit coefficient of ek must be < q;
     * 0xFF bytes decode to coefficients of 4095, which is not. Refusing is the
     * spec requirement -- accepting would make two byte strings the same key. */
    {
        uint8_t badek[MLKEM768_EK];
        memcpy(badek, ek, MLKEM768_EK);
        memset(badek, 0xFF, 384);                 /* first polynomial out of range */
        ck(mlkem768_encaps_derand(badek, m, ct, ss) == -1,
           "encaps: refuses an ek that fails the modulus check");
        /* and the untouched key is still accepted, so the check is not just
         * "always refuse" */
        ck(mlkem768_encaps_derand(ek, m, ct, ss) == 0,
           "encaps: still accepts the valid ek (the refusal is selective)");
    }

    /* Distinct seeds must give distinct keys -- the cheapest possible check
     * that the seed is actually consumed. A keygen that ignored d would pass
     * every round-trip test in this file. */
    {
        static uint8_t ek2[MLKEM768_EK], dk2[MLKEM768_DK];
        memset(d, 0, 32); memset(z, 0, 32);
        mlkem768_keygen_derand(d, z, ek, dk);
        d[31] = 1;
        mlkem768_keygen_derand(d, z, ek2, dk2);
        ck(memcmp(ek, ek2, MLKEM768_EK) != 0, "a one-bit change in d changes ek");
        memset(d, 0, 32); z[31] = 1;
        mlkem768_keygen_derand(d, z, ek2, dk2);
        ck(memcmp(ek, ek2, MLKEM768_EK) == 0, "z does NOT affect ek (it is only the reject seed)");
        ck(memcmp(dk, dk2, MLKEM768_DK) != 0, "a one-bit change in z DOES change dk");
    }

    /* ------------------------------------------------ 3. the Keccak layer */
    /* One FIPS 202 known answer each, so a Keccak break is reported here rather
     * than as an inscrutable ML-KEM mismatch. Empty-input digests are the
     * standard published values. */
    {
        uint8_t h[64];
        static const char *sha3_256_empty =
            "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a";
        static const char *sha3_512_empty =
            "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6"
            "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26";
        sha3_256(h, (const uint8_t *)"", 0);
        tohex(hex, h, 32); ck(!strcmp(hex, sha3_256_empty), "SHA3-256(\"\")");
        sha3_512(h, (const uint8_t *)"", 0);
        tohex(hex, h, 64); ck(!strcmp(hex, sha3_512_empty), "SHA3-512(\"\")");
        shake128(h, 32, (const uint8_t *)"", 0);
        tohex(hex, h, 32);
        ck(!strcmp(hex, "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26"),
           "SHAKE128(\"\", 32)");
        shake256(h, 32, (const uint8_t *)"", 0);
        tohex(hex, h, 32);
        ck(!strcmp(hex, "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f"),
           "SHAKE256(\"\", 32)");
    }

    printf("mlkem: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
