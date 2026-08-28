#ifndef LOGIT_XCHACHA20POLY1305_H
#define LOGIT_XCHACHA20POLY1305_H
#include <stdint.h>

/* HChaCha20 (draft-irtf-cfrg-xchacha-03 section 2.2) and XChaCha20-Poly1305
 * (same draft, section 3) -- see xchacha20poly1305.c's header comment for
 * the full argument. Own header, deliberately not folded into the shared
 * c/crypto/crypto.h: a consumer that wants this includes this file, exactly
 * as aes_backend.h and keccak.h already do for their primitives.
 *
 * NOT WIRED INTO ANYTHING. No trust store, no TLS ciphersuite, no aex
 * package check, no login path reaches these symbols. It is a primitive
 * with a known-answer gate, not a decision that this construction should be
 * used anywhere in this tree yet -- that is a separate argument with its
 * own review, per the workflow this was built under.
 */

/* HChaCha20 block function. `nonce` is HChaCha20's 16-byte nonce (the first
 * 16 bytes of XChaCha20's 24-byte nonce); `out` receives the 32-byte
 * subkey. Exposed on its own because it has its own official test vector
 * (draft section 2.2.1) independent of the full AEAD, which is the only
 * way to catch the "forgot to skip the final add" bug in isolation -- see
 * the .c file. */
void hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32]);

/* XChaCha20-Poly1305 AEAD (draft section 3): a 192-bit (24-byte) nonce
 * instead of RFC 8439's 96-bit one, everything else (Poly1305 key
 * derivation, AAD/ciphertext/length framing, the 16-byte tag) identical to
 * RFC 8439 and delegated to chacha20_poly1305_seal/open unchanged.
 *
 * `aadlen`/`len` are `int`, matching chacha20_poly1305_seal/open's existing
 * signature in crypto.h -- negative values are rejected the same way. */
void xchacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[24],
                             const uint8_t *aad, int aadlen,
                             const uint8_t *pt, int len, uint8_t *ct,
                             uint8_t tag[16]);

/* Returns 0 and writes `pt` on a verified tag, -1 (leaving `pt` untouched)
 * otherwise -- same contract as chacha20_poly1305_open. */
int xchacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[24],
                            const uint8_t *aad, int aadlen,
                            const uint8_t *ct, int len, const uint8_t tag[16],
                            uint8_t *pt);

#endif
