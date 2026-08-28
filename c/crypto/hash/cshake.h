#ifndef LOGIT_HASH_CSHAKE_H
#define LOGIT_HASH_CSHAKE_H

#include <stdint.h>
#include <stddef.h>
#include "keccak.h"   /* c/crypto/pq -- struct shake, keccakf1600, shake128/256_init.
                       * Not in CRYPTO_SRC's find(aead hash kdf pubkey) today; the
                       * make rule this file needs adds c/crypto/pq/keccak.c to the
                       * link line and -Ic/crypto/pq to the include path. See the
                       * end of this comment block for why keccak.c/.h are NOT
                       * touched to get this. */

/* cSHAKE128/256 (NIST SP 800-185 section 3) built on top of c/crypto/pq's
 * Keccak-f[1600] sponge (keccak.c/.h), which this file does not modify.
 *
 * OWN HEADER ON PURPOSE, same reason as blake2b.h/blake3.h in this directory:
 * c/crypto/crypto.h is shared and off limits. A consumer that wants cSHAKE
 * includes THIS file; kmac.h includes it too, because KMAC *is* cSHAKE with
 * N = "KMAC" (SP 800-185 section 4) -- that is not an implementation choice,
 * it is the definition, which is why kmac.c is a thin construction over this
 * file rather than a second sponge wrapper (CLAUDE.md's "the mode never lives
 * in a backend" rule, restated one level up: the MAC construction does not
 * duplicate the XOF core).
 *
 * NO CONSUMER TODAY. This is a primitive, not a trust decision. It must not
 * be reached from c/crypto/trust, TLS, aex's package check or the login path;
 * wiring it anywhere that could make a page, a package or a peer look
 * verified is a separate, unmade decision (CLAUDE.md category (b)).
 *
 * WHY keccak.c/.h ARE NOT TOUCHED, and it is the one non-obvious design
 * choice here: cSHAKE needs a domain-separator byte of 0x04 where plain SHAKE
 * uses 0x1F (see below), and keccak.c's finalize (`pad_and_switch`) is
 * `static` -- not exported, and deliberately not, because widening it invites
 * every future XOF-family addition (TupleHash, ParallelHash, SP 800-185's
 * other two) to do its own thing with the sponge internals instead of going
 * through one path. `struct shake`'s fields (st/rate/pos/squeezing) ARE
 * public, and keccakf1600() is exported, so the two-line pad10*1 step is
 * reimplemented here against those public fields rather than by widening
 * keccak.c's contract. This is the only reason cshake_finalize() below looks
 * like it is duplicating something -- it is duplicating two XORs and a
 * permutation call, not the sponge.
 *
 * THE TRAP THIS FILE EXISTS TO GET RIGHT: cSHAKE(X, L, "", "") -- BOTH N and S
 * empty -- is DEFINED to equal SHAKE(X, L), domain-separator 0x1F, with NO
 * bytepad prefix absorbed at all. The moment either N or S is non-empty, the
 * bytepad(encode_string(N) || encode_string(S), rate) prefix is absorbed and
 * the domain separator becomes 0x04. An implementation that always uses 0x04
 * passes every real-world cSHAKE call (N or S is normally non-empty) and
 * fails only the identity above; one that always uses 0x1F passes the empty
 * case and silently computes plain SHAKE for every cSHAKE call that matters.
 * Both are checked here: `struct cshake_ctx.ds` is picked once, in
 * cshake*_ctx_init(), from whether N and S are BOTH empty, and every KMAC
 * call goes through this file with N = "KMAC" (never empty), so KMAC always
 * gets ds = 0x04 regardless of whether its customization string S is empty --
 * this is not a special case in kmac.c, it falls out of calling this file
 * honestly. See tests/unit/cshake_test.c's identity check against OpenSSL
 * SHAKE128/256 for the case an implementation could get away with faking.
 */

/* SP 800-185 2.3.3: left_encode/right_encode encode a nonnegative integer as
 * a byte string that is unambiguously parseable from the front or the back
 * respectively -- left_encode puts the byte COUNT first, right_encode puts it
 * LAST. THESE INTEGERS ARE ALMOST ALWAYS BIT LENGTHS, NOT BYTE LENGTHS: every
 * caller in this file and in kmac.c multiplies a byte length by 8 before
 * calling left_encode, except for the rate `w` passed to the bytepad helpers
 * below (that one genuinely is bytes -- SP 800-185's own `w` parameter is
 * defined in bytes). Getting a single one of these *8 wrong produces a
 * deterministic wrong tag with no crash and no other symptom.
 *
 * out must have room for 9 bytes (a uint64_t needs at most 8 value bytes plus
 * 1 length byte). Returns the number of bytes written (2..9). */
size_t left_encode(uint8_t out[9], uint64_t x);
size_t right_encode(uint8_t out[9], uint64_t x);

struct cshake_ctx {
    struct shake sp;
    uint8_t      ds;   /* domain separator applied at finalize: 0x1F (plain
                        * SHAKE, N and S both empty) or 0x04 (cSHAKE proper) */
};

/* Primes the sponge and, unless N and S are both empty, absorbs
 * bytepad(encode_string(N) || encode_string(S), rate) -- rate is 168 for
 * cSHAKE128, 136 for cSHAKE256, chosen by which of these two you call. N/S
 * may be NULL when their respective length is 0. */
void cshake128_ctx_init(struct cshake_ctx *c,
                         const uint8_t *N, size_t Nlen,
                         const uint8_t *S, size_t Slen);
void cshake256_ctx_init(struct cshake_ctx *c,
                         const uint8_t *N, size_t Nlen,
                         const uint8_t *S, size_t Slen);

void cshake_absorb(struct cshake_ctx *c, const uint8_t *in, size_t len);
void cshake_finalize(struct cshake_ctx *c);   /* pad10*1 with c->ds; switch to squeeze */
void cshake_squeeze(struct cshake_ctx *c, uint8_t *out, size_t len);

/* One-shot forms. outlen/inlen/Nlen/Slen are all in BYTES (the *8 for the
 * bit-length encodings happens inside). */
void cshake128(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen,
               const uint8_t *N, size_t Nlen, const uint8_t *S, size_t Slen);
void cshake256(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen,
               const uint8_t *N, size_t Nlen, const uint8_t *S, size_t Slen);

/* bytepad(X, w) (SP 800-185 2.3.3), streamed rather than buffered: X can be
 * fed as one or more encode_string() pieces (cshake*_ctx_init feeds two --
 * encode_string(N) then encode_string(S); kmac.c feeds one -- encode_string(K))
 * without ever materialising bytepad's output. `rate` (w) is in BYTES.
 * cshake_bp_begin absorbs left_encode(rate); each cshake_bp_feed_string call
 * absorbs encode_string(X) = left_encode(len(X) in BITS) || X for one piece
 * and accumulates the running byte total; cshake_bp_end absorbs enough zero
 * bytes to reach the next multiple of `rate`. This is exposed (not static in
 * cshake.c) because kmac.c needs the exact same construction for K. */
struct cshake_bp { struct cshake_ctx *c; int rate; size_t total; };
void cshake_bp_begin(struct cshake_bp *bp, struct cshake_ctx *c, int rate);
void cshake_bp_feed_string(struct cshake_bp *bp, const uint8_t *X, size_t Xlen);
void cshake_bp_end(struct cshake_bp *bp);

#endif
