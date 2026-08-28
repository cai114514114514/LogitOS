#ifndef LOGIT_HASH_BLAKE2B_H
#define LOGIT_HASH_BLAKE2B_H

#include <stdint.h>
#include <stddef.h>

/* BLAKE2b (RFC 7693) -- 64-bit words, 12 rounds, rotations 32/24/16/63.
 *
 * OWN HEADER ON PURPOSE. c/crypto/crypto.h is shared across every session
 * working this tree today and is explicitly off-limits (CLAUDE.md, and the
 * workflow that generated this file repeats it) -- exactly the pattern
 * aes_backend.h / keccak.h / mlkem.h already use. A consumer that wants
 * BLAKE2b includes THIS file.
 *
 * NO CONSUMER TODAY. This is a primitive, not a trust decision -- it is not
 * reachable from c/crypto/trust, TLS, aex's package check or the login path,
 * and it must stay that way; wiring it anywhere that could make a page, a
 * package or a peer look verified is a separate, unmade decision. See
 * CLAUDE.md's category-(b) discussion of exactly this kind of file.
 *
 * WHY BLAKE2b AND NOT ONLY BLAKE2s: BLAKE2s (32-bit words) already exists as
 * a sibling contribution; BLAKE2b is the 64-bit member and the one actually
 * deployed at scale -- Argon2's compression function G is a REDUCED (1-round,
 * 1024-byte-block) variant of BLAKE2b's round function, and libsodium's
 * generichash() is BLAKE2b. Do NOT share code with an Argon2 implementation:
 * Argon2's G runs ONE round over a 1024-byte block with its own permutation
 * of inputs, not twelve rounds over the 128-byte BLAKE2b block -- the two
 * only look related from a distance. This file exists so the Argon2 agent
 * has a correct BLAKE2b to read, not to link.
 *
 * THE 128-BIT COUNTER. BLAKE2b's message-length counter t is 128 bits,
 * carried in F() as two 64-bit words (t0 = low, t1 = high), because BLAKE2b's
 * own byte offsets can in principle exceed 2^64. A single 64-bit counter is
 * correct for every input under 16 EiB and therefore correct for every input
 * this gate, or any gate, will ever actually run -- so a single-word counter
 * is NOT a bug the KAT below can catch. `t1` is carried and incremented on
 * overflow of `t0` for exactly that reason; nothing in tests/unit/blake2b_test.c
 * exercises the carry, and that is a known, stated gap, not an oversight.
 *
 * CONSTANT TIME: the compression function G and the round loop are pure
 * fixed-shape arithmetic (add/xor/rotate) with no data-dependent branch and
 * no memory index derived from key or message bytes -- the whole core is
 * naturally constant-time on any target where the rotations below compile to
 * shifts-and-or rather than a variable-count barrel shift with a secret-
 * dependent trap (none of x86-64's rotate/shift instructions are). The
 * length-dependent tail padding (zero-filling the last block) branches only
 * on the PUBLIC message length, exactly like every hash function's Merkle-
 * Damgard-style padding in this tree (sha256.c, sha384.c). Key handling: the
 * key block is a fixed 128-byte block regardless of actual key length
 * (zero-padded), so keyed-mode time does not vary with key length either. */

#define BLAKE2B_BLOCKBYTES 128
#define BLAKE2B_OUTBYTES   64   /* max digest length */
#define BLAKE2B_KEYBYTES   64   /* max key length */

struct blake2b {
    uint64_t h[8];              /* chained state */
    uint64_t t[2];               /* message byte counter, low/high (128-bit) */
    uint64_t f[2];               /* finalization flags (f[1] used only by BLAKE2X, kept 0 here) */
    uint8_t  buf[BLAKE2B_BLOCKBYTES];
    size_t   buflen;
    size_t   outlen;             /* 1..64 */
};

/* outlen: digest length in bytes, 1..64. key/keylen: keylen 0..64; keylen==0
 * means unkeyed. Returns 0 on success, -1 on an out-of-range outlen/keylen
 * (never stubs to a plausible wrong answer -- CLAUDE.md rule 5). */
int blake2b_init(struct blake2b *c, size_t outlen, const void *key, size_t keylen);
void blake2b_update(struct blake2b *c, const void *data, size_t len);
void blake2b_final(struct blake2b *c, uint8_t *out);

/* One-shot convenience, unkeyed, full 64-byte output -- the shape sha256()
 * and sha384() use. Returns nothing to fail: outlen is fixed and valid. */
void blake2b512(const void *data, size_t len, uint8_t out[64]);

/* One-shot, keyed, variable output length. Returns 0/-1 exactly as
 * blake2b_init does. */
int blake2b_keyed(const void *data, size_t len, const void *key, size_t keylen,
                   uint8_t *out, size_t outlen);

#endif
