/* The signed-package format (c/crypto/trust/pkgsig.c).
 *
 * There is no published vector for a format invented here, so the standard is
 * the other one this tree uses: every claim is asserted from the OUTSIDE (bytes
 * in, verdict out) and every way of saying yes wrongly is a case.
 *
 * The positive cases are a round trip through lpk_sign/lpk_verify. The negative
 * cases are the ones that matter, and they are chosen so that a verifier which
 * skips ONE step still fails: a container validator that never checks the
 * signature (that is test-pkg-control, built with PKGSIG_BREAK_NO_SIGCHECK); a
 * signature checker that forgets the payload digest; one that trusts any key; one
 * that ignores unknown flags; one that reads the name past its length.
 *
 * The trust-root half is tested both ways round: a package signed by the
 * built-in development key must be accepted with root_index >= 0, and the same
 * package signed by a DIFFERENT key must be refused with LPK_E_UNTRUSTED even
 * though its signature is perfectly valid. That second one is the whole
 * difference between "this file is intact" and "this file is from someone we
 * trust", and it is the one a verifier is most likely to conflate. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "pkgsig.h"
#include "crypto.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

/* The development seed, the private half of tools/pkgroots/logitos-dev.pub.
 * Published on purpose; see tools/pkgroots/DEV-SIGNING-KEY.txt. */
static const uint8_t DEV_SEED[32] = {
    0xdd,0x5e,0x99,0xf7,0xc5,0xf1,0x91,0x86,0xcc,0x00,0x53,0xd5,0x90,0x5b,0x54,0x01,
    0x65,0x86,0xd7,0xb3,0x66,0x67,0x25,0x97,0xd5,0xb6,0x19,0x76,0xd9,0x65,0xa0,0x0b };

/* Any other key. Its signatures are valid and it is not trusted. */
static const uint8_t OTHER_SEED[32] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32 };

#define PAYLOAD_N 5000
static uint8_t payload[PAYLOAD_N];
static uint8_t pkg[LPK_HDR_LEN + PAYLOAD_N];
static uint8_t scratch[LPK_HDR_LEN + PAYLOAD_N + 8];   /* + room for the trailing-byte case */

static uint64_t build(const uint8_t seed[32], const char *name, int plen)
{
    if (lpk_sign(pkg, name, payload, (uint64_t)plen, seed) != 0) return 0;
    memcpy(pkg + LPK_HDR_LEN, payload, (size_t)plen);
    return (uint64_t)(LPK_HDR_LEN + plen);
}

/* Corrupt a copy of the current package and require a specific refusal. */
static void reject(uint64_t n, int off, uint8_t xorv, int want, const char *what)
{
    memcpy(scratch, pkg, (size_t)n);
    scratch[off] ^= xorv;
    struct lpk out;
    int r = lpk_verify(scratch, n, &out, 0);
    char msg[160];
    snprintf(msg, sizeof msg, "REJECT: %s (got %d, want %d)", what, r, want);
    ok(r == want, msg);
}

int main(void)
{
    for (int i = 0; i < PAYLOAD_N; i++) payload[i] = (uint8_t)(i * 31 + (i >> 5));

    ok(pkg_root_count() >= 1, "the built-in signer set is not empty");
    printf("     trusted signer[0] = %s\n", pkg_root_name(0));

    /* --- round trip, trusted key ----------------------------------------- */
    uint64_t n = build(DEV_SEED, "hello.bin", PAYLOAD_N);
    ok(n == LPK_HDR_LEN + PAYLOAD_N, "lpk_sign produced a package");
    {
        struct lpk out;
        int r = lpk_verify(pkg, n, &out, 0);
        ok(r == LPK_OK, "a package signed by the built-in key verifies");
        ok(out.root_index >= 0, "it names which built-in key signed it");
        ok(out.payload_len == PAYLOAD_N, "payload length survives the round trip");
        ok(memcmp(out.payload, payload, PAYLOAD_N) == 0, "payload bytes survive the round trip");
        ok(strcmp(out.name, "hello.bin") == 0, "the name survives the round trip");
    }

    /* --- the empty payload, and the largest name -------------------------- */
    {
        uint64_t m = build(DEV_SEED, "e", 0);
        struct lpk out;
        ok(lpk_verify(pkg, m, &out, 0) == LPK_OK && out.payload_len == 0,
           "a zero-length payload is a valid package");
        char big[LPK_NAME_MAX + 1];
        memset(big, 'a', LPK_NAME_MAX); big[LPK_NAME_MAX] = 0;
        m = build(DEV_SEED, big, 16);
        ok(lpk_verify(pkg, m, &out, 0) == LPK_OK && (int)strlen(out.name) == LPK_NAME_MAX,
           "a name of exactly LPK_NAME_MAX is accepted");
        char toobig[LPK_NAME_MAX + 2];
        memset(toobig, 'a', LPK_NAME_MAX + 1); toobig[LPK_NAME_MAX + 1] = 0;
        ok(lpk_sign(pkg, toobig, payload, 16, DEV_SEED) == -1,
           "REJECT: lpk_sign refuses an over-long name");
    }

    /* --- untrusted signer -------------------------------------------------
     * A perfectly valid signature by a key we do not hold. This is the case a
     * verifier conflates with "intact", and the ONE that makes a trust store a
     * trust store. */
    n = build(OTHER_SEED, "hello.bin", PAYLOAD_N);
    {
        struct lpk out;
        ok(lpk_verify(pkg, n, &out, 0) == LPK_E_UNTRUSTED,
           "REJECT: a valid signature by an untrusted key");
        ok(lpk_verify(pkg, n, &out, 1) == LPK_OK && out.root_index == -1,
           "...and trust_any accepts the same bytes, so it really was the TRUST that failed");
    }

    /* --- everything below is against the trusted-key package -------------- */
    n = build(DEV_SEED, "hello.bin", PAYLOAD_N);

    reject(n, 0,   0x01, LPK_E_MAGIC,   "magic byte 0 flipped");
    reject(n, 7,   0x20, LPK_E_MAGIC,   "magic byte 7 flipped");
    reject(n, 8,   0x01, LPK_E_VERSION, "version bumped");
    reject(n, 12,  0x01, LPK_E_FIELD,   "an unknown flag bit set");
    reject(n, 28,  0x01, LPK_E_FIELD,   "the reserved word set");
    reject(n, 96,  0x01, LPK_E_DIGEST,  "one bit of the payload digest");
    reject(n, 160, 0x01, LPK_E_SIG,     "one bit of the signature (R half)");
    reject(n, 200, 0x01, LPK_E_SIG,     "one bit of the signature (S half)");
    reject(n, 128, 0x01, LPK_E_SIG,     "one bit of the signer key");
    reject(n, LPK_HDR_LEN, 0x01, LPK_E_DIGEST, "one bit of the payload");
    reject(n, LPK_HDR_LEN + PAYLOAD_N - 1, 0x80, LPK_E_DIGEST, "the LAST payload byte");
    reject(n, 32,  0x01, LPK_E_SIG,     "one bit of the name");

    /* the name field's padding: signed, invisible, and must not be free space */
    {
        memcpy(scratch, pkg, (size_t)n);
        scratch[32 + 60] = 'x';                   /* past name_len ("hello.bin") */
        struct lpk out;
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_FIELD,
           "REJECT: junk in the name field past name_len");
    }
    {
        memcpy(scratch, pkg, (size_t)n);
        scratch[32 + 5] = '/';                    /* a path separator in the name */
        struct lpk out;
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_FIELD,
           "REJECT: a path separator in the name");
    }

    /* --- truncation and length lies --------------------------------------- */
    {
        struct lpk out;
        ok(lpk_verify(pkg, LPK_HDR_LEN - 1, &out, 0) == LPK_E_SHORT,
           "REJECT: fewer bytes than a header");
        ok(lpk_verify(pkg, 0, &out, 0) == LPK_E_SHORT, "REJECT: zero bytes");
        ok(lpk_verify(pkg, n - 1, &out, 0) == LPK_E_SHORT,
           "REJECT: payload one byte short of the declared length");
        /* a payload_len that would wrap the header+len addition */
        memcpy(scratch, pkg, (size_t)n);
        for (int i = 0; i < 8; i++) scratch[16 + i] = 0xff;
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_SHORT,
           "REJECT: payload_len = 2^64-1 (the overflow case)");
        /* a name_len past the field */
        memcpy(scratch, pkg, (size_t)n);
        scratch[24] = LPK_NAME_MAX + 1;
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_FIELD, "REJECT: name_len past the field");
        ok(lpk_verify(NULL, n, &out, 0) < 0 && lpk_verify(pkg, n, NULL, 0) < 0,
           "REJECT: NULL arguments");
    }

    /* --- extra bytes after the payload -----------------------------------
     * Accepted deliberately (a package may be embedded in a larger stream) but
     * they must NOT become part of the payload -- otherwise a signed package
     * plus attacker-appended bytes is a signed package with attacker content. */
    {
        struct lpk out;
        memcpy(scratch, pkg, (size_t)n);
        scratch[n] = 0xAA;
        int r = lpk_verify(scratch, n + 1, &out, 0);
        ok(r == LPK_OK, "a trailing byte after the payload does not break the package");
        ok(out.payload_len == PAYLOAD_N, "...and it is NOT part of the payload");
    }

    /* --- domain separation ------------------------------------------------
     * The signature is over SHA-256(DOMAIN || header). A raw Ed25519 signature
     * by the same key over the same 160 header bytes -- what a naive signer
     * would produce -- must NOT verify, or a signature made for anything else
     * with that key could be replayed here. */
    {
        uint8_t pub[32];
        ed25519_pubkey(pub, DEV_SEED);
        memcpy(scratch, pkg, (size_t)n);
        ed25519_sign(scratch + 160, scratch, LPK_SIGNED_LEN, DEV_SEED, pub);
        struct lpk out;
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_SIG,
           "REJECT: a signature over the raw header (no domain separation)");
        /* and over the payload digest alone */
        ed25519_sign(scratch + 160, scratch + 96, 32, DEV_SEED, pub);
        ok(lpk_verify(scratch, n, &out, 0) == LPK_E_SIG,
           "REJECT: a signature over the payload digest alone");
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    if (!failed) printf("PKG ALL PASS\n");
    return failed ? 1 : 0;
}
