#ifndef LOGIT_CHACHA_CORE_H
#define LOGIT_CHACHA_CORE_H
#include <stdint.h>

/* The ChaCha20 permutation and stream function (RFC 8439 section 2.3),
 * DEFINED ONCE in chacha20poly1305.c and shared from there. HChaCha20
 * (xchacha20poly1305.c) needs the exact same 20-round quarter-round
 * schedule -- HChaCha20's state is initialised identically to ordinary
 * ChaCha20 (draft-irtf-cfrg-xchacha-03 section 2.2's own figure labels the
 * two states "the same way ... except HChaCha20 uses a 128-bit nonce and
 * has no counter"), so the trick used here is to call chacha_block() with
 * the first word of HChaCha20's 16-byte nonce standing in for the block
 * counter and the remaining 12 bytes standing in for chacha_block's usual
 * nonce -- which reproduces HChaCha20's initial state exactly, because that
 * is what HChaCha20's own state diagram places there.
 *
 * The two are declared here, non-static, INSTEAD OF a second permutation
 * written from scratch in the XChaCha20 file -- CLAUDE.md's "the mode never
 * lives in a backend" rule and this tree's four-rasterizer history both say
 * a second copy of a core algorithm is the mistake, not the shortcut: two
 * implementations of the same 20-round schedule could each pass their own
 * known-answer test while disagreeing with each other, and the divergence
 * would look like a nonce or padding bug for as long as nobody thought to
 * suspect the permutation itself.
 *
 * xchacha20poly1305.c does NOT reuse the final state addition chacha_block()
 * performs (HChaCha20's own spec: the subkey is the raw post-round state,
 * "without adding the input words back in") -- it recomputes the same
 * trivial constants/key/counter/nonce layout chacha_block() builds and
 * subtracts it back out of chacha_block()'s output. That recomputation is
 * public bookkeeping (which word goes where), not the permutation, so
 * duplicating it here is not a second core.
 */
void chacha_block(const uint8_t key[32], uint32_t counter,
                   const uint8_t nonce[12], uint8_t out[64]);
void chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
              const uint8_t *in, int len, uint8_t *out);

#endif
