#ifndef LOGIT_KDF_SCRYPT_H
#define LOGIT_KDF_SCRYPT_H

#include <stdint.h>
#include <stddef.h>

/* scrypt (RFC 7914) -- the memory-hard KDF PBKDF2 in pbkdf2.c deliberately is
 * not. See scrypt.c's top comment for why this exists next to PBKDF2 rather
 * than instead of it, and for pbkdf2.c's own argument for why THIS machine's
 * login record stays PBKDF2 today.
 *
 * NO CONSUMER. This is a library primitive shipped with its own gate, not
 * wired into any trust path -- no c/crypto/trust, no TLS, no aex, no login.
 * See CLAUDE.md's rule on primitives vs. things that make a false claim.
 *
 * CALLER-SUPPLIED SCRATCH, ALWAYS. This file declares no static array and
 * calls no allocator -- there is none in the kernel -- so every function that
 * needs O(N*r) memory takes a scratch buffer and its length from the caller
 * and returns -1 rather than proceed if it is too small. scrypt_scratch_len()
 * computes the exact size; never guess it. Sizes get large fast: N=1024,r=8
 * needs ~1 MiB; N=16384,r=8 needs ~16 MiB; N=1048576,r=8 (RFC 7914 12's
 * fourth vector) needs just over 1 GiB. A caller on THIS kernel with real N
 * has to reserve that from somewhere sane (pmm_alloc_contig or a VMA) before
 * calling in -- this header does not solve that problem because nothing here
 * has a caller yet.
 *
 * CONSTANT TIME: partially, and the part that is not is not an implementation
 * bug -- it is the mechanism. Salsa20/8 (add-rotate-xor only, no table, no
 * secret-dependent branch) and scryptBlockMix (pure data movement and XOR)
 * are both naturally constant time, the same way ChaCha20 is and AES-without-
 * AES-NI is not. scryptROMix is NOT constant time: its second pass indexes
 * V[Integerify(X) mod N], and X is derived from the password. That data-
 * dependent memory access across an N-block table IS scrypt's memory-hardness
 * argument -- an attacker who cannot predict which block gets touched next
 * cannot precompute or discard blocks between touches, which is the entire
 * point of ROMix over a plain iterated hash. Removing the dependency removes
 * the property scrypt exists to buy. This is the documented, accepted shape
 * of every scrypt implementation (Percival's own included), stated here
 * rather than left for a reader to assume the file forgot AES-NI's lesson. */

/* Exact scratch length scryptROMix needs for (r, N): (N + 2) * 128 * r bytes
 * -- N blocks for V[], plus two working blocks (X, T). Exposed separately
 * from scrypt_scratch_len() because the KAT vectors for ROMix (RFC 7914 s10)
 * exercise it directly, without the PBKDF2 wrapping. */
uint64_t scrypt_romix_scratch_len(uint32_t r, uint64_t N);

/* Exact scratch length the full scrypt() call below needs for (N, r, p):
 * p * 128 * r bytes for the B[] block array, plus one ROMix scratch region
 * (reused sequentially across the p blocks -- p ROMix calls never run at
 * once, so they share one region rather than needing p of them). */
uint64_t scrypt_scratch_len(uint64_t N, uint32_t r, uint32_t p);

/* Salsa20/8 Core (RFC 7914 s3): 64 octets in, 64 octets out. NOT ChaCha20 --
 * see scrypt.c's top comment for the one-line reason that mistake is easy to
 * make in this tree specifically. `out` and `in` may alias (whole-block
 * add-back at the end reads `in` after `out` is otherwise settled; see the
 * .c for how that is handled without an extra copy). */
void salsa20_8_core(uint8_t out[64], const uint8_t in[64]);

/* scryptBlockMix (RFC 7914 s4). `in` and `out` are each 2*r 64-octet blocks
 * (128*r octets) and MUST be different buffers -- the interleaved write
 * pattern (s4 step 3: even Y[] indices land in the first half of B', odd
 * indices in the second) writes out[i%2 ? r+i/2 : i/2] before every in[j] has
 * necessarily been read, so an in-place call can read already-overwritten
 * input. Every caller in this file honours that; it is enforced by construction
 * (X/T ping-pong in scrypt_romix), not by a runtime check, because the two
 * buffers are always distinct scratch regions here. */
void scrypt_blockmix(uint32_t r, const uint8_t *in, uint8_t *out);

/* scryptROMix (RFC 7914 s5), IN PLACE on B (128*r octets). `scratch` must be
 * at least scrypt_romix_scratch_len(r, N) bytes. Returns -1 (B left
 * untouched) if N is not a power of two greater than 1, or if scratch is too
 * small; 0 on success. */
int scrypt_romix(uint32_t r, uint64_t N, uint8_t *B,
                  uint8_t *scratch, uint64_t scratch_len);

/* The full construction (RFC 7914 s6): PBKDF2-HMAC-SHA256(P, S, 1, p*128*r)
 * to expand, p independent scryptROMix passes, PBKDF2-HMAC-SHA256(P, B, 1,
 * dkLen) to compress. `scratch` must be at least
 * scrypt_scratch_len(N, r, p) bytes. Returns -1 (dk left untouched) on a bad
 * parameter (N not a power of two > 1, r == 0, p == 0, dklen == 0) or
 * undersized scratch; 0 on success. Wipes `scratch` before returning either
 * way -- every intermediate block is derived from the password. */
int scrypt(const uint8_t *pw, int pwlen, const uint8_t *salt, int saltlen,
           uint64_t N, uint32_t r, uint32_t p,
           uint8_t *dk, uint64_t dklen,
           uint8_t *scratch, uint64_t scratch_len);

#endif
