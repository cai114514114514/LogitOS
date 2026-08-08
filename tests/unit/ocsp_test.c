/* OCSP stapling (c/net/tls/ocsp.c), against responses a real `openssl ocsp`
 * responder produced (tests/unit/ocsp_gen.sh).
 *
 * The positive cases are cheap and the negative ones are the test. Revocation
 * checking is a feature whose ONLY job is to say no, so a verifier that returns
 * OCSP_OK unconditionally passes every "good" case and is worse than having no
 * verifier at all -- it converts "we do not check revocation" into "we say we
 * check revocation".
 *
 * Six responses, each isolating one decision, plus byte-level tampering:
 *
 *   good        accepted
 *   revoked     REFUSED with OCSP_E_REVOKED -- the case that did not exist
 *               before this file, and the whole reason it exists
 *   unknown     REFUSED: the responder says it does not know the certificate,
 *               which is not the same as "good"
 *   othercert   a VALID, current, correctly signed "good" -- about a different
 *               certificate. REFUSED. A verifier that reads the first
 *               SingleResponse without checking its CertID accepts this, and
 *               that verifier can be fed a good response for any certificate
 *               the same CA ever issued.
 *   delegated   signed by a responder certificate carrying id-kp-OCSPSigning:
 *               accepted, because that is the delegation RFC 6960 defines
 *   nodeleg     signed by a certificate the same CA issued that does NOT carry
 *               id-kp-OCSPSigning. REFUSED. This is the sharpest one: the
 *               signature is real, the chain is real, and accepting it lets
 *               anyone who can buy a certificate from the CA vouch for a
 *               revoked sibling.
 *
 * Plus: expiry on both sides (a response from the future, a response whose
 * nextUpdate has passed), and one flipped bit in each of the signature, the
 * CertID serial and the status byte. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "ocsp.h"
#include "x509.h"
#include "crypto.h"

static int checks, failed;
static void ok(int cond, const char *what)
{
    checks++;
    if (cond) printf("ok   %s\n", what);
    else { printf("FAIL %s\n", what); failed++; }
}

static uint8_t *slurp(const char *dir, const char *name, int *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); failed++; checks++; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (int)n;
    return b;
}

/* `now` inside the responses' validity window: they were made moments ago with
 * -ndays 7, so "right now" is what the host clock says. Taken once so every
 * case sees the same instant. */
static int64_t NOW;
#define SKEW 300

static const char *rcname(int rc) { return ocsp_strerror(rc); }

static void expect(const char *dir, const char *file,
                   const struct cert *leaf, const struct cert *issuer,
                   int want, const char *what)
{
    int n = 0;
    uint8_t *b = slurp(dir, file, &n);
    if (!b) return;
    int rc = ocsp_check(b, n, leaf, issuer, NOW, SKEW);
    char msg[220];
    snprintf(msg, sizeof msg, "%s [%s -> %s]", what, file, rcname(rc));
    ok(rc == want, msg);
    free(b);
}

/* Flip one bit at `off` and require a refusal (any refusal: which one depends
 * on which field the bit lands in, and pinning that would be pinning DER
 * layout rather than behaviour). */
static void tamper(const char *dir, const char *file,
                   const struct cert *leaf, const struct cert *issuer,
                   int off, const char *what)
{
    int n = 0;
    uint8_t *b = slurp(dir, file, &n);
    if (!b) return;
    if (off < 0) off += n;
    if (off < 0 || off >= n) { ok(0, what); free(b); return; }
    b[off] ^= 0x01;
    int rc = ocsp_check(b, n, leaf, issuer, NOW, SKEW);
    char msg[220];
    snprintf(msg, sizeof msg, "REJECT: %s [-> %s]", what, rcname(rc));
    ok(rc != OCSP_OK, msg);
    free(b);
}

static int load_cert(const char *dir, const char *name, struct cert *c, uint8_t **keep)
{
    int n = 0;
    uint8_t *b = slurp(dir, name, &n);
    if (!b) return -1;
    *keep = b;
    if (x509_parse(b, n, c) != 0) { printf("FAIL parse %s\n", name); failed++; checks++; return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/ocsp";

    /* The responses were minted seconds ago by ocsp_gen.sh with -ndays 7, so
     * "now" is the host's clock. Read it the way http.c does: seconds. */
    NOW = (int64_t)time(NULL);

    struct cert leaf, other, revoked, stray, ca;
    uint8_t *kl, *ko, *kr, *ks, *kc;
    if (load_cert(dir, "cert_leaf.der", &leaf, &kl)) return 1;
    if (load_cert(dir, "cert_other.der", &other, &ko)) return 1;
    if (load_cert(dir, "cert_revoked.der", &revoked, &kr)) return 1;
    if (load_cert(dir, "cert_stray.der", &stray, &ks)) return 1;
    if (load_cert(dir, "cert_ca.der", &ca, &kc)) return 1;

    /* The parser additions this file depends on, asserted before they are used:
     * a wrong serial or a wrong SPKI span makes every CertID miss, and that
     * looks identical to a server stapling the wrong response. */
    ok(leaf.seriallen > 0 && leaf.serial != NULL, "the leaf's serial number was parsed");
    ok(ca.spki_keylen > 0 && ca.spki_key != NULL, "the issuer's subjectPublicKey span was parsed");
    ok(memcmp(leaf.issuer, ca.subject, (size_t)ca.subjectlen) == 0 &&
       leaf.issuerlen == ca.subjectlen, "the leaf's issuer is the CA's subject");

    /* --- the positive cases ---------------------------------------------- */
    expect(dir, "good.der", &leaf, &ca, OCSP_OK,
           "a good response signed by the issuer is accepted");
    expect(dir, "delegated.der", &leaf, &ca, OCSP_OK,
           "a good response signed by a delegated responder (OCSPSigning) is accepted");

    /* --- the case the whole feature exists for ---------------------------- */
    expect(dir, "revoked.der", &revoked, &ca, OCSP_E_REVOKED,
           "REJECT: a REVOKED certificate");

    /* --- not-good is not good --------------------------------------------- */
    expect(dir, "unknown.der", &stray, &ca, OCSP_E_UNKNOWN,
           "REJECT: the responder does not know this certificate");

    /* --- the CertID check -------------------------------------------------
     * A valid, current, correctly signed `good` about ANOTHER certificate. */
    expect(dir, "othercert.der", &leaf, &ca, OCSP_E_CERTID,
           "REJECT: a good response about a different certificate");
    expect(dir, "good.der", &other, &ca, OCSP_E_CERTID,
           "REJECT: the leaf's response offered for another leaf");

    /* --- the delegation check --------------------------------------------
     * Signed by a certificate this CA really issued, without OCSPSigning. */
    expect(dir, "nodeleg.der", &leaf, &ca, OCSP_E_SIGNER,
           "REJECT: signed by a CA-issued cert WITHOUT id-kp-OCSPSigning");

    /* --- wrong issuer ------------------------------------------------------
     * The same response, checked against a certificate that is not the issuer:
     * the CertID hashes cannot match, so it must miss rather than verify. */
    {
        /* Any refusal, not a specific one: the signature check runs before the
         * CertID scan, so which of the two fires depends on whether the wrong
         * "issuer" happens to have signed anything -- and pinning that would be
         * pinning the order of two checks rather than the outcome. */
        int n = 0;
        uint8_t *b = slurp(dir, "good.der", &n);
        if (b) {
            int rc = ocsp_check(b, n, &leaf, &leaf, NOW, SKEW);
            char m[160];
            snprintf(m, sizeof m, "REJECT: verified against the wrong issuer [-> %s]", rcname(rc));
            ok(rc != OCSP_OK, m);
            free(b);
        }
    }

    /* --- time -------------------------------------------------------------- */
    {
        int n = 0;
        uint8_t *b = slurp(dir, "good.der", &n);
        if (b) {
            int rc = ocsp_check(b, n, &leaf, &ca, NOW - 86400LL * 30, SKEW);
            char m[160]; snprintf(m, sizeof m, "REJECT: response is dated in the future [-> %s]", rcname(rc));
            ok(rc == OCSP_E_STALE, m);
            rc = ocsp_check(b, n, &leaf, &ca, NOW + 86400LL * 30, SKEW);
            snprintf(m, sizeof m, "REJECT: nextUpdate has passed [-> %s]", rcname(rc));
            ok(rc == OCSP_E_STALE, m);
            /* and it is still good one day from now, inside the 7-day window */
            rc = ocsp_check(b, n, &leaf, &ca, NOW + 86400LL, SKEW);
            ok(rc == OCSP_OK, "still accepted a day later, inside nextUpdate");
            free(b);
        }
    }

    /* --- bytes ------------------------------------------------------------- */
    tamper(dir, "good.der", &leaf, &ca, 0,   "the outermost DER tag");
    tamper(dir, "good.der", &leaf, &ca, 60,  "a byte inside the CertID");

    /* A single-bit sweep over exactly the bytes the signature covers.
     *
     * Aimed with ocsp_signed_span() rather than run over the whole file,
     * because several bytes of a real response are genuinely free and a sweep
     * that demanded they matter would be asserting a property no correct
     * verifier has: the outer SEQUENCE length has slack, the
     * signatureAlgorithm's NULL parameters are never read (only the OID is),
     * and good.der's certificate bag holds the ISSUER's own certificate, which
     * this verifier ignores because the issuer comes from the chain. What MUST
     * hold is that not one bit of tbsResponseData -- the CertID, the status,
     * the dates -- can move without the signature noticing. */
    {
        int n = 0;
        uint8_t *b = slurp(dir, "good.der", &n);
        if (b) {
            int off = 0, span = 0;
            ok(ocsp_signed_span(b, n, &off, &span) == 0 && span > 100,
               "located tbsResponseData inside the response");
            int bad = 0;
            for (int i = off; i < off + span && i < n; i++) {
                b[i] ^= 0x01;
                if (ocsp_check(b, n, &leaf, &ca, NOW, SKEW) == OCSP_OK) bad++;
                b[i] ^= 0x01;
            }
            char m[180];
            snprintf(m, sizeof m,
                     "REJECT: every one of %d single-bit flips in the SIGNED region (%d accepted)",
                     span, bad);
            ok(bad == 0, m);
            free(b);
        }
    }
    {
        /* The delegated response is verified end to end -- the responder
         * certificate is parsed, its EKU read, and its own signature checked
         * against the issuer -- so its signed region must be just as rigid. */
        int n = 0;
        uint8_t *b = slurp(dir, "delegated.der", &n);
        if (b) {
            int off = 0, span = 0;
            int bad = 0;
            if (ocsp_signed_span(b, n, &off, &span) == 0) {
                for (int i = off; i < off + span && i < n; i++) {
                    b[i] ^= 0x01;
                    if (ocsp_check(b, n, &leaf, &ca, NOW, SKEW) == OCSP_OK) bad++;
                    b[i] ^= 0x01;
                }
            } else { bad = -1; }
            char m[180];
            snprintf(m, sizeof m,
                     "REJECT: every one of %d flips in the DELEGATED response's signed region (%d accepted)",
                     span, bad);
            ok(bad == 0, m);
            free(b);
        }
    }
    {
        /* And the delegation itself is load-bearing: break one bit of the
         * responder certificate in the bag and the response must stop being
         * accepted, because that certificate is what entitles the signature. */
        int n = 0;
        uint8_t *b = slurp(dir, "delegated.der", &n);
        if (b) {
            int off = 0, span = 0;
            ocsp_signed_span(b, n, &off, &span);
            int bagstart = off + span + 200;      /* comfortably inside the bag */
            int bad = 0, tried = 0;
            for (int i = bagstart; i < n - 8; i += 37) {
                b[i] ^= 0x01; tried++;
                if (ocsp_check(b, n, &leaf, &ca, NOW, SKEW) == OCSP_OK) bad++;
                b[i] ^= 0x01;
            }
            char m[180];
            snprintf(m, sizeof m,
                     "REJECT: %d flips inside the delegated RESPONDER CERTIFICATE (%d accepted)",
                     tried, bad);
            ok(tried > 4 && bad == 0, m);
            free(b);
        }
    }

    /* truncation at every 16th byte must refuse and must not read out of
     * bounds -- this test runs under ASan, so an over-read is a failure even
     * when the verdict happens to be right. */
    {
        int n = 0;
        uint8_t *b = slurp(dir, "good.der", &n);
        if (b) {
            int bad = 0;
            for (int cut = 1; cut < n; cut += 16) {
                uint8_t *t = malloc((size_t)cut);
                memcpy(t, b, (size_t)cut);
                if (ocsp_check(t, cut, &leaf, &ca, NOW, SKEW) == OCSP_OK) bad++;
                free(t);
            }
            ok(bad == 0, "REJECT: every truncation of a good response");
            ok(ocsp_check(b, 0, &leaf, &ca, NOW, SKEW) != OCSP_OK, "REJECT: zero-length response");
            ok(ocsp_check(NULL, 10, &leaf, &ca, NOW, SKEW) != OCSP_OK, "REJECT: NULL response");
            free(b);
        }
    }

    free(kl); free(ko); free(kr); free(ks); free(kc);
    printf("\n%d checks, %d failed\n", checks, failed);
    if (!failed) printf("OCSP ALL PASS\n");
    return failed ? 1 : 0;
}
