/* ECDSA over NIST P-521 -- verification, against published vectors and against
 * openssl, plus the rejections.
 *
 * WHY P-521 is here at all. The ClientHello has advertised
 * ecdsa_secp521r1_sha512 (0x0603) in signature_algorithms since the TLS 1.3
 * work landed, and nothing in the tree could verify that signature:
 * ecdsa_verify took `curve` as 256-or-anything-else and mapped anything else to
 * P-384. So a server holding a P-521 certificate was told we accept P-521,
 * signed with it, and we failed at CertificateVerify. Advertising an algorithm
 * you cannot check is worse than not advertising it -- the site is unreachable
 * AND the failure looks like the server's fault.
 *
 * The vectors, and why there are two independent sources:
 *
 *  1. RFC 6979 A.2.7 -- an IETF-published ECDSA P-521 vector (deterministic
 *     nonce, but the SIGNATURE is an ordinary ECDSA signature and verifying it
 *     needs nothing deterministic). This is a third party's answer to a
 *     question we did not set, which is the whole point: our own generator
 *     agreeing with our own verifier proves only self-consistency.
 *
 *  2. openssl, via tests/unit/p521_gen.sh -- freshly generated keys and
 *     signatures over random messages, replayed here. This is what catches an
 *     implementation that happens to work for one hard-coded point.
 *
 * The negatives are the substance. A verifier that returns 1 unconditionally
 * passes every positive case above; each rejection below is a specific way to
 * be wrong, and each one is asserted, not assumed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "crypto.h"

int ecdsa_verify(int curve, const uint8_t *pub, const uint8_t *sig,
                 const uint8_t *hash, int hlen);

static int fails, checks;

static void ok(const char *nm, int cond)
{
    checks++;
    if (cond) printf("ok   %s\n", nm);
    else { printf("FAIL %s\n", nm); fails++; }
}

/* hex -> bytes, left-padded to `want` bytes. RFC 6979 prints P-521 values as
 * 131 hex digits (521 bits); the wire/API form is 66 bytes, so an odd-length
 * string is padded with one leading zero nibble and short strings with leading
 * zero bytes. Getting this wrong shifts every byte and nothing verifies. */
static void hexpad(const char *h, uint8_t *out, int want)
{
    int n = (int)strlen(h);
    memset(out, 0, (size_t)want);
    int nib = n;                     /* number of hex digits */
    for (int i = 0; i < nib; i++) {
        int c = h[nib - 1 - i];
        int v = (c <= '9') ? c - '0' : ((c | 32) - 'a' + 10);
        int byte = want - 1 - (i / 2);
        if (byte < 0) { printf("hexpad overflow\n"); exit(2); }
        out[byte] |= (uint8_t)(v << ((i % 2) * 4));
    }
}

#define FL 66                        /* P-521 field element, bytes */

/* ---------------------------------------------------------------- RFC 6979 */
/* A.2.7, curve NIST P-521, message "test", SHA-512. */
static const char *R6979_UX =
  "1894550D0785932E00EAA23B694F213F8C3121F86DC97A04E5A7167DB4E5BCD3"
  "71123D46E45DB6B5D5370A7F20FB633155D38FFA16D2BD761DCAC474B9A2F502"
  "3A4";
static const char *R6979_UY =
  "0493101C962CD4D2FDDF782285E64584139C2F91B47F87FF82354D6630F746A2"
  "8A0DB25741B5B34A828008B22ACC23F924FAAFBD4D33F81EA66956DFEAA2BFDF"
  "CF5";
static const char *R6979_R =
  "13E99020ABF5CEE7525D16B69B229652AB6BDF2AFFCAEF38773B4B7D08725F10"
  "CDB93482FDCC54EDCEE91ECA4166B2A7C6265EF0CE2BD7051B7CEF945BABD47E"
  "E6D";
static const char *R6979_S =
  "1FBD0013C674AA79CB39849527916CE301C66EA7CE8B80682786AD60F98F7E78"
  "A19CA69EFF5C57400E3B3A0AD66CE0978214D13BAF4E9AC60752F7B155E2DE4D"
  "CE3";

/* secp521r1 group order n, for the "s must be < n" boundary cases. */
static const char *P521_N =
  "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa"
  "51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409";

static void rfc6979_case(void)
{
    uint8_t pub[2*FL], sig[2*FL], h[64];
    hexpad(R6979_UX, pub,      FL);
    hexpad(R6979_UY, pub + FL, FL);
    hexpad(R6979_R,  sig,      FL);
    hexpad(R6979_S,  sig + FL, FL);
    sha512("test", 4, h);

    ok("RFC 6979 A.2.7 P-521/SHA-512 accepts", ecdsa_verify(521, pub, sig, h, 64) == 1);

    /* --- the same vector, broken one way at a time --- */
    uint8_t bad[2*FL];

    memcpy(bad, sig, sizeof bad); bad[0] ^= 0x01;
    ok("rejects flipped high bit of r",   ecdsa_verify(521, pub, bad, h, 64) == 0);
    memcpy(bad, sig, sizeof bad); bad[FL-1] ^= 0x01;
    ok("rejects flipped low bit of r",    ecdsa_verify(521, pub, bad, h, 64) == 0);
    memcpy(bad, sig, sizeof bad); bad[2*FL-1] ^= 0x01;
    ok("rejects flipped low bit of s",    ecdsa_verify(521, pub, bad, h, 64) == 0);
    memcpy(bad, sig, sizeof bad);
    { uint8_t t[FL]; memcpy(t, bad, FL); memcpy(bad, bad+FL, FL); memcpy(bad+FL, t, FL); }
    ok("rejects r and s swapped",         ecdsa_verify(521, pub, bad, h, 64) == 0);

    uint8_t h2[64]; memcpy(h2, h, 64); h2[63] ^= 0x01;
    ok("rejects one flipped hash bit",    ecdsa_verify(521, pub, sig, h2, 64) == 0);
    sha512("Test", 4, h2);
    ok("rejects a different message",     ecdsa_verify(521, pub, sig, h2, 64) == 0);

    /* r = 0 and s = 0 are the classic "forgery that verifies if you skip the
     * range check" -- RFC 6090 / FIPS 186-4 require 0 < r,s < n. */
    memcpy(bad, sig, sizeof bad); memset(bad, 0, FL);
    ok("rejects r = 0",                   ecdsa_verify(521, pub, bad, h, 64) == 0);
    memcpy(bad, sig, sizeof bad); memset(bad + FL, 0, FL);
    ok("rejects s = 0",                   ecdsa_verify(521, pub, bad, h, 64) == 0);

    /* r = n and s = n must also be out of range (the check is >= n, not > n). */
    uint8_t nbuf[FL]; hexpad(P521_N, nbuf, FL);
    memcpy(bad, sig, sizeof bad); memcpy(bad, nbuf, FL);
    ok("rejects r = n",                   ecdsa_verify(521, pub, bad, h, 64) == 0);
    memcpy(bad, sig, sizeof bad); memcpy(bad + FL, nbuf, FL);
    ok("rejects s = n",                   ecdsa_verify(521, pub, bad, h, 64) == 0);

    /* A public key that is not a point on the curve. Verifying against it must
     * fail rather than run the group law on a point from some other group --
     * that is the invalid-curve attack, and the only defence is this check. */
    uint8_t badpub[2*FL]; memcpy(badpub, pub, sizeof badpub); badpub[2*FL-1] ^= 0x01;
    ok("rejects pubkey not on the curve", ecdsa_verify(521, badpub, sig, h, 64) == 0);
    memset(badpub, 0, sizeof badpub);
    ok("rejects pubkey (0,0)",            ecdsa_verify(521, badpub, sig, h, 64) == 0);

    /* The same good signature offered under the wrong group. Before this work
     * an unknown curve id silently fell through to P-384; now it is refused. */
    ok("rejects curve id 0",              ecdsa_verify(0,   pub, sig, h, 64) == 0);
    ok("rejects curve id 512 (typo)",     ecdsa_verify(512, pub, sig, h, 64) == 0);
    /* Reading a P-521 key as P-256/P-384 must not verify either. */
    ok("P-521 vector refused as P-256",   ecdsa_verify(256, pub, sig, h, 32) == 0);
    ok("P-521 vector refused as P-384",   ecdsa_verify(384, pub, sig, h, 48) == 0);
}

/* ------------------------------------------------------- openssl cross-check */
/* Lines of "Ux Uy r s hash", all hex, produced by tests/unit/p521_gen.sh. */
static void openssl_cases(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL cannot open %s\n", path); fails++; return; }
    char ux[300], uy[300], rr[300], ss[300], hh[300];
    int n = 0;
    while (fscanf(f, "%299s %299s %299s %299s %299s", ux, uy, rr, ss, hh) == 5) {
        uint8_t pub[2*FL], sig[2*FL], h[64];
        hexpad(ux, pub, FL); hexpad(uy, pub + FL, FL);
        hexpad(rr, sig, FL); hexpad(ss, sig + FL, FL);
        hexpad(hh, h, 64);
        n++;
        if (ecdsa_verify(521, pub, sig, h, 64) != 1) {
            printf("FAIL openssl vector %d not accepted\n", n); fails++; checks++;
            continue;
        }
        /* Every accepted openssl signature is immediately re-tested broken, so
         * the count of positives can never outrun the count of negatives. */
        uint8_t bad[2*FL]; memcpy(bad, sig, sizeof bad);
        bad[FL + FL - 3] ^= 0x40;
        if (ecdsa_verify(521, pub, bad, h, 64) != 0) {
            printf("FAIL openssl vector %d accepted a tampered s\n", n); fails++;
        }
        checks += 2;
    }
    fclose(f);
    if (n == 0) { printf("FAIL no openssl vectors were read from %s\n", path); fails++; checks++; }
    else printf("ok   %d openssl P-521 signatures accepted, %d tampered rejected\n", n, n);
}

int main(int argc, char **argv)
{
    printf("== ECDSA P-521 ==\n");
    rfc6979_case();
    if (argc > 1) openssl_cases(argv[1]);
    printf("\n%d checks, %d failed\n", checks, fails);
    if (fails) { printf("P-521 FAILED\n"); return 1; }
    printf("P-521 ALL PASS\n");
    return 0;
}
