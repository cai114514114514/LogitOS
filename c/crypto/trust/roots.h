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
/* Ed25519 (RFC 8410). It reuses the `ec` / `eclen` fields for the 32-byte raw
 * public key -- NOT because it is an EC point (it is not, and it carries no
 * 0x04 prefix) but because adding a fourth pointer to every row to hold the
 * same thing under another name buys nothing. `curve` is 0. */
#define ROOT_ED25519 3

struct root_ca {
    int type;                       /* ROOT_EC / ROOT_RSA */
    int curve;                      /* EC: 256 / 384 / 521 */
    const uint8_t *ec;  int eclen;  /* EC: point X||Y */
    const uint8_t *n;   int nlen;   /* RSA: modulus (big-endian) */
    const uint8_t *e;   int elen;   /* RSA: public exponent (big-endian) */
};

extern const struct root_ca logit_roots[];
extern const int logit_nroots;

/* The name of each row, in logit_roots[] order, exactly logit_nroots entries:
 * the basename of the PEM in tools/roots/ it was generated from. Kept as a
 * PARALLEL array rather than a ninth field of struct root_ca on purpose -- the
 * row is walked by two byte-comparison loops in net/x509.c (:422, :447) that
 * compare nothing but key material, and widening the row would touch both for
 * a string neither reads.
 *
 * It exists because a count is not an answer to "which CAs does this machine
 * trust?". The struct above is eight fields of key material and no name, so
 * until this array the honest answer from a running kernel was a number. The
 * slug is the right identifier and not merely the convenient one: adding a PEM
 * to tools/roots/ is how a root is trusted and deleting one is how it is
 * revoked, and `make check-roots` makes a bundle that disagrees with that
 * directory a hard build failure. */
extern const char *const logit_root_names[];

/* Print the trust store -- the two counts and every root's name -- once per
 * boot, to serial. Defined in c/net/tls/tls.c beside the code that consumes
 * the store; declared here because kernel_main calls it at boot, so a machine
 * that never opens a TLS connection still says what it trusts. */
void trust_banner(void);

/* Roots that exist as PEMs in tools/roots/ but were NOT compiled in, because
 * their key type is one this kernel cannot verify (Ed448, and anything not
 * in the ROOT_* list above). Each
 * entry is "slug: reason"; the array is 0-terminated and logit_nroots_skipped
 * is the count. This is deliberately visible rather than a generator-time
 * aside: a silently dropped root makes the trust store smaller than its own
 * documentation claims, and that is a security-relevant lie. */
extern const char *const logit_roots_skipped[];
extern const int logit_nroots_skipped;

#endif /* LOGIT_ROOTS_H */
