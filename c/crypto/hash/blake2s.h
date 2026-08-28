#ifndef LOGIT_CRYPTO_BLAKE2S_H
#define LOGIT_CRYPTO_BLAKE2S_H

#include <stdint.h>
#include <stddef.h>

/* BLAKE2s (RFC 7693) -- the 32-bit-word sibling of BLAKE2b: 10 rounds,
 * 64-byte blocks, rotations 16/12/8/7, digest 1..32 bytes, optional key
 * 0..32 bytes.
 *
 * NO CONSUMER. This is a from-scratch implementation of a hash primitive,
 * shipped for breadth the way pbkdf2.c and the pq/ directory were -- see
 * CLAUDE.md's category (b) argument for why a library primitive with a real
 * gate is not the same kind of debt as a mechanism with no enforcement point.
 * It is NOT wired into c/crypto/trust, TLS, aex's package check or login.
 * If a caller wants it, that is a separate, later decision.
 *
 * Digest length: BLAKE2s is a FAMILY of hash functions, not one function --
 * the output length nn is baked into the parameter block that seeds h[0], so
 * BLAKE2s-128 and BLAKE2s-256 are not the same function truncated, they start
 * from different initial state. blake2s_init(ctx, nn) with nn in 1..32.
 *
 * Keying: blake2s_init_key(ctx, nn, key, kk) with kk in 0..32. This is BLAKE2's
 * OWN keyed mode (a single zero-padded key block fed as the first input block,
 * counted into the byte counter) -- not HMAC-BLAKE2s wrapped around the
 * unkeyed hash. Do not build an HMAC construction on top of this file; if one
 * is ever needed it is its own translation unit, per CLAUDE.md's "the mode
 * never lives in a backend" rule -- the same argument that keeps AES's modes
 * out of aes_ni.c applies here to keeping a MAC construction out of the core.
 */

#define BLAKE2S_BLOCKBYTES  64
#define BLAKE2S_OUTBYTES    32
#define BLAKE2S_KEYBYTES    32

struct blake2s {
    uint32_t h[8];
    uint32_t t[2];          /* input bytes processed so far, mod 2^64, as two u32 */
    uint8_t  buf[BLAKE2S_BLOCKBYTES];
    size_t   buflen;
    size_t   outlen;
};

/* Unkeyed. outlen must be 1..32; anything else is a caller bug (asserts via
 * the KAT test, not checked at runtime -- consistent with sha256_init taking
 * no length because it has none, and with crypto.h's other init functions,
 * none of which validate their fixed parameters at runtime). */
void blake2s_init(struct blake2s *ctx, size_t outlen);

/* Keyed. keylen 0..32; keylen==0 is exactly blake2s_init(ctx, outlen). */
void blake2s_init_key(struct blake2s *ctx, size_t outlen,
                       const void *key, size_t keylen);

void blake2s_update(struct blake2s *ctx, const void *in, size_t inlen);

/* Writes ctx->outlen bytes to out (NOT always 32 -- caller-sized). */
void blake2s_final(struct blake2s *ctx, uint8_t *out);

/* One-shot convenience. key may be NULL iff keylen==0. */
void blake2s(const void *in, size_t inlen,
             const void *key, size_t keylen,
             uint8_t *out, size_t outlen);

#endif
