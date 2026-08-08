#ifndef LOGIT_ROOTS_H
#define LOGIT_ROOTS_H

#include <stdint.h>

/* A built-in trusted root CA, reduced to just its public key (the trust
 * anchor). EC roots carry the point X||Y; RSA roots carry modulus + exponent
 * (big-endian, minimal). The chain verifier (net/x509.c) trusts a presented
 * chain when its top cert is signed by -- or byte-identical to -- one of these
 * keys. */
#define ROOT_EC  1
#define ROOT_RSA 2

struct root_ca {
    int type;                       /* ROOT_EC / ROOT_RSA */
    int curve;                      /* EC: 256 / 384 / 521 */
    const uint8_t *ec;  int eclen;  /* EC: point X||Y */
    const uint8_t *n;   int nlen;   /* RSA: modulus (big-endian) */
    const uint8_t *e;   int elen;   /* RSA: public exponent (big-endian) */
};

extern const struct root_ca logit_roots[];
extern const int logit_nroots;

/* Roots that exist as PEMs in tools/roots/ but were NOT compiled in, because
 * their key type is one this kernel cannot verify (P-521, Ed25519, ...). Each
 * entry is "slug: reason"; the array is 0-terminated and logit_nroots_skipped
 * is the count. This is deliberately visible rather than a generator-time
 * aside: a silently dropped root makes the trust store smaller than its own
 * documentation claims, and that is a security-relevant lie. */
extern const char *const logit_roots_skipped[];
extern const int logit_nroots_skipped;

#endif /* LOGIT_ROOTS_H */
