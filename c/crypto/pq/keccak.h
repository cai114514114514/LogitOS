#ifndef LOGIT_PQ_KECCAK_H
#define LOGIT_PQ_KECCAK_H

#include <stdint.h>
#include <stddef.h>

/* Keccak-f[1600] and the four FIPS 202 functions ML-KEM needs.
 *
 * This lives under c/crypto/pq rather than c/crypto/hash because ML-KEM is its
 * only caller today and the alternative -- adding a file to c/crypto/hash --
 * touches a directory another line is editing. If a second subsystem ever wants
 * SHA-3 (SLH-DSA and ML-DSA both would), moving the file is the right change at
 * that point; until then a shared header with one consumer is a claim of
 * generality nothing has tested.
 *
 * Naming: FIPS 203 calls these H, J, G, PRF and XOF. The mapping, from the spec
 * (FIPS 203 4.1), is kept here so mlkem.c can be read against the spec text:
 *   H   = SHA3-256      J = SHAKE256(.,32)     G = SHA3-512
 *   PRF = SHAKE256      XOF = SHAKE128
 */

/* Incremental SHAKE. `rate` is the sponge rate in BYTES (168 for SHAKE128,
 * 136 for SHAKE256), which is what separates the two -- the permutation is
 * identical. Split absorb/squeeze because SampleNTT rejection-samples an
 * unbounded number of blocks and cannot know in advance how many it needs. */
struct shake {
    uint64_t st[25];
    int      rate;
    int      pos;        /* bytes absorbed into, or squeezed out of, the block */
    int      squeezing;
};

void keccakf1600(uint64_t st[25]);

void shake128_init(struct shake *s);
void shake256_init(struct shake *s);
void shake_absorb(struct shake *s, const uint8_t *in, size_t len);
void shake_finalize(struct shake *s);                       /* pad; switch to squeeze */
void shake_squeeze(struct shake *s, uint8_t *out, size_t len);

/* One-shot forms. */
void shake128(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen);
void shake256(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen);
void sha3_256(uint8_t out[32], const uint8_t *in, size_t inlen);
void sha3_512(uint8_t out[64], const uint8_t *in, size_t inlen);

#endif
