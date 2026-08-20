/* ecdsa_sign: an independent RFC 6979 reference, then a differential against
 * openssl -- two oracles that share no code with each other or with us.
 *
 * WHY BOTH. "openssl verified our signature" is the weaker half on its own:
 * ECDSA verification accepts a signature made with ANY k, so it cannot see a
 * k-generation bug -- and k-generation is where ECDSA kills you (a repeated k
 * hands over the private key outright). tests/unit/ecdsa6979_ref.py computes
 * the deterministic k, r and s from the RFC in Python arbitrary-precision
 * integers and affine coordinates; a signer whose DRBG is seeded slightly
 * wrong (V and K swapped, the 0x00/0x01 separators transposed, bits2octets
 * applied to the wrong value) fails against it with an exact byte mismatch,
 * while still producing signatures openssl is perfectly happy with.
 *
 * The reverse also holds, which is why the openssl half is not redundant: both
 * we and the Python are implementations of the same RFC, so a shared
 * MISREADING of the document would reproduce in both. openssl never sees
 * either file and checks the signature against the public key alone.
 *
 * Expectations come in on stdin (the Python's output), one case per line:
 *     curve hlen priv_hex msg_hex r_hex s_hex
 * so nothing in this file is a remembered constant.
 *
 * Build/run: tests/tlsx.mk, `make test-ecdsa-sign`.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "crypto.h"

static int pass, fail;

static int hex2bin(const char *h, uint8_t *o, int max)
{
    int n = 0;
    while (h[2*n] && h[2*n+1] && n < max) {
        int hi = h[2*n], lo = h[2*n+1];
        #define HX(c) ((c) <= '9' ? (c)-'0' : ((c)|32)-'a'+10)
        o[n] = (uint8_t)((HX(hi) << 4) | HX(lo));
        #undef HX
        n++;
    }
    return n;
}

static void check(const char *what, int ok)
{
    if (ok) pass++;
    else { fail++; printf("FAIL %s\n", what); }
}

static void digest(int hlen, const void *m, size_t ml, uint8_t *out)
{
    if (hlen == 32) sha256(m, ml, out);
    else if (hlen == 48) sha384(m, ml, out);
    else sha512(m, ml, out);
}

/* ------------------------------------------------ RFC 6979 agreement ------ */

static void run_vector(int curve, int hlen, const char *privhex,
                       const char *msg, const char *rhex, const char *shex)
{
    int flen = (curve == 521) ? 66 : curve / 8;
    uint8_t priv[66], want[132], sig[132], h[64];
    if (hex2bin(privhex, priv, sizeof priv) != flen) {
        printf("FAIL vector shape (priv, c=%d)\n", curve); fail++; return;
    }
    if (hex2bin(rhex, want, flen) != flen || hex2bin(shex, want + flen, flen) != flen) {
        printf("FAIL vector shape (r/s, c=%d)\n", curve); fail++; return;
    }
    digest(hlen, msg, strlen(msg), h);

    if (ecdsa_sign(curve, priv, h, hlen, 0u, hmac, sig) != 0) {
        printf("FAIL P-%d \"%s\": sign returned -1\n", curve, msg); fail++; return;
    }
    if (memcmp(sig, want, (size_t)(2*flen)) != 0) {
        fail++;
        printf("FAIL P-%d \"%s\" != RFC 6979 reference\n  got  ", curve, msg);
        for (int i = 0; i < 2*flen; i++) printf("%02x", sig[i]);
        printf("\n  want ");
        for (int i = 0; i < 2*flen; i++) printf("%02x", want[i]);
        printf("\n");
        return;
    }
    pass++;

    /* The blinding must not reach the output. (k + rho*n)*G == k*G exactly, so
     * two different rho values MUST give byte-identical signatures; if they do
     * not, the blinding is corrupting the scalar rather than randomising the
     * ladder, and every RFC 6979 vector would then be a coin flip. */
    uint8_t sig2[132];
    if (ecdsa_sign(curve, priv, h, hlen, 0xdeadbeefu, hmac, sig2) != 0) {
        printf("FAIL P-%d blinded sign returned -1\n", curve); fail++; return;
    }
    check("blinding leaves the signature unchanged",
          memcmp(sig, sig2, (size_t)(2*flen)) == 0);

    /* Our own verifier must accept it. Weakest assertion here (one
     * implementation agreeing with itself) and present only to localise a
     * failure: openssl rejecting what this accepts points at the signer. */
    uint8_t pub[133];
    check("keygen", ecdh_keygen(curve, priv, 0x1234u, pub) == 0);
    check("our verify accepts our signature",
          ecdsa_verify(curve, pub + 1, sig, h, hlen) == 1);
    h[0] ^= 1;
    check("our verify rejects a tampered digest",
          ecdsa_verify(curve, pub + 1, sig, h, hlen) == 0);
    h[0] ^= 1;
    sig[flen] ^= 1;
    check("our verify rejects a tampered s",
          ecdsa_verify(curve, pub + 1, sig, h, hlen) == 0);
}

/* ------------------------------------------------------------- refusals --- */

static void run_refusals(void)
{
    uint8_t h[32]; memset(h, 1, sizeof h);
    uint8_t sig[132];
    uint8_t zero[66]; memset(zero, 0, sizeof zero);
    uint8_t big[66];  memset(big, 0xff, sizeof big);   /* above n for all three */
    uint8_t ok[66];   memset(ok, 0, sizeof ok); ok[31] = 7;

    check("refuses d = 0",         ecdsa_sign(256, zero, h, 32, 0, hmac, sig) == -1);
    check("refuses d >= n",        ecdsa_sign(256, big,  h, 32, 0, hmac, sig) == -1);
    check("refuses unknown curve", ecdsa_sign(255, ok,   h, 32, 0, hmac, sig) == -1);
    /* An hlen no hmac() here dispatches on must be refused rather than
     * silently treated as SHA-256: the DRBG is keyed on the hash, so a wrong
     * one produces a signature that verifies as nothing. */
    check("refuses hlen = 20",     ecdsa_sign(256, ok,   h, 20, 0, hmac, sig) == -1);
}

/* ----------------------------------------------- material for openssl ------
 * Writes <dir>/c<curve>.{sig,pub,priv,msg} in RAW form; the shell half wraps
 * them for `openssl pkeyutl -verify`. The openssl call lives in the harness
 * rather than in a system() here so the reference command is visible in the
 * test output instead of buried in a C string. */
static void emit(const char *dir, int curve, int hlen, const char *privhex, const char *msg)
{
    int flen = (curve == 521) ? 66 : curve / 8;
    uint8_t priv[66], sig[132], pub[133], h[64];
    if (hex2bin(privhex, priv, sizeof priv) != flen) { fail++; return; }
    digest(hlen, msg, strlen(msg), h);
    if (ecdsa_sign(curve, priv, h, hlen, 0x4242u, hmac, sig) != 0) { fail++; return; }
    if (ecdh_keygen(curve, priv, 0x1234u, pub) != 0) { fail++; return; }

    char path[512]; FILE *f;
    #define W(ext, buf, len) \
        snprintf(path, sizeof path, "%s/c%d." ext, dir, curve); \
        f = fopen(path, "wb"); if (!f) { fail++; return; } \
        fwrite((buf), 1, (size_t)(len), f); fclose(f)
    W("sig",  sig,  2*flen);
    W("pub",  pub,  1 + 2*flen);
    W("priv", priv, flen);
    W("msg",  msg,  strlen(msg));
    #undef W
}

int main(int argc, char **argv)
{
    const char *emitdir = (argc > 1) ? argv[1] : 0;
    /* The message the emit() cases use. Only ONE per curve is handed to
     * openssl: the differential is about whether a third party accepts our
     * signature at all, and three curves is the axis that matters. */
    const char *emit_msg = "logitos tls server certificate verify";

    char line[1024];
    int cases = 0;
    while (fgets(line, sizeof line, stdin)) {
        int curve, hlen;
        char privhex[200], msghex[400], rhex[200], shex[200];
        /* Every field is hex, the message included, so there is no quoting to
         * get wrong and a message with a space in it cannot split a field. */
        if (sscanf(line, "%d %d %199s %399s %199s %199s",
                   &curve, &hlen, privhex, msghex, rhex, shex) != 6) continue;
        char msg[200];
        int ml = hex2bin(msghex, (uint8_t *)msg, (int)sizeof msg - 1);
        msg[ml] = 0;
        run_vector(curve, hlen, privhex, msg, rhex, shex);
        if (emitdir && strcmp(msg, emit_msg) == 0) emit(emitdir, curve, hlen, privhex, msg);
        cases++;
    }
    /* A harness that fed us nothing would otherwise print "0 failed" and pass,
     * which is the classic gate that cannot fail. */
    check("the reference produced cases", cases >= 6);

    run_refusals();
    printf("ecdsa_sign: %d passed, %d failed (%d reference vectors)\n", pass, fail, cases);
    return fail ? 1 : 0;
}
