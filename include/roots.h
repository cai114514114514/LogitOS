#ifndef AQUA_ROOTS_H
#define AQUA_ROOTS_H

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
    int curve;                      /* EC: 256 / 384 */
    const uint8_t *ec;  int eclen;  /* EC: point X||Y */
    const uint8_t *n;   int nlen;   /* RSA: modulus (big-endian) */
    const uint8_t *e;   int elen;   /* RSA: public exponent (big-endian) */
};

extern const struct root_ca aqua_roots[];
extern const int aqua_nroots;

#endif /* AQUA_ROOTS_H */
