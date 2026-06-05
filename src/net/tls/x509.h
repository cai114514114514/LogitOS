#ifndef AQUA_X509_H
#define AQUA_X509_H

#include <stdint.h>

/* Parsed fields we need from one certificate (pointers reference the input DER,
 * which must outlive the struct). */
struct cert {
    const uint8_t *der; int derlen;
    const uint8_t *tbs; int tbslen;             /* signed region */
    int   sig_alg;                              /* SIG_ECDSA_* / SIG_RSA_* */
    const uint8_t *sig; int siglen;             /* ECDSA: DER SEQ{r,s}; RSA: raw */
    int   key_type;                             /* KEY_EC / KEY_RSA (this cert's key) */
    int   key_curve;                            /* EC: 256 or 384 */
    const uint8_t *pub; int publen;             /* EC point 04||X||Y */
    const uint8_t *rsa_n; int rsa_nlen;         /* RSA modulus (big-endian, minimal) */
    const uint8_t *rsa_e; int rsa_elen;         /* RSA exponent (big-endian, minimal) */
    const uint8_t *subject; int subjectlen;     /* raw DER of Subject Name */
    const uint8_t *issuer; int issuerlen;       /* raw DER of Issuer Name */
    const char *cn; int cnlen;                  /* leaf CN (for name check) */
    const uint8_t *san; int sanlen;             /* SubjectAltName extension value */
    int64_t not_before, not_after;              /* unix-ish seconds */
};

#define SIG_ECDSA_SHA256 1
#define SIG_ECDSA_SHA384 2
#define SIG_RSA_SHA256   3
#define SIG_RSA_SHA384   4
#define SIG_RSA_SHA512   5
#define SIG_ECDSA_SHA512 6
#define SIG_RSA_PSS_SHA256 7
#define SIG_RSA_PSS_SHA384 8
#define SIG_RSA_PSS_SHA512 9

#define KEY_EC  1
#define KEY_RSA 2

/* Parse one DER certificate. Returns 0 on success. */
int x509_parse(const uint8_t *der, int len, struct cert *out);

/* Verify that `child`'s signature was made by `issuer`'s public key. 0 = ok. */
int x509_verify_signed_by(const struct cert *child, const struct cert *issuer);

/* Verify a chain certs[0..n-1] (leaf first): each signed by the next, the top
 * signed by (or equal to) a built-in trusted root, plus host-name and validity
 * checks against `host` and `now` (unix seconds). Returns 0 if fully trusted,
 * <0 with a reason code otherwise. */
int x509_verify_chain(const struct cert *certs, int n, const char *host, int64_t now);

#define X509_OK            0
#define X509_E_PARSE      -1
#define X509_E_SIG        -2
#define X509_E_UNTRUSTED  -3
#define X509_E_NAME       -4
#define X509_E_EXPIRED    -5

#endif /* AQUA_X509_H */
