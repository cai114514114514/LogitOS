#ifndef LOGIT_HASH_KMAC_H
#define LOGIT_HASH_KMAC_H

#include <stdint.h>
#include <stddef.h>

/* KMAC128/256 and their XOF variants (NIST SP 800-185 section 4), built on
 * cshake.c in this directory. KMAC IS cSHAKE with N = "KMAC": KMAC(K, X, L, S)
 * = cSHAKE(bytepad(encode_string(K), rate) || X || right_encode(L), L,
 * "KMAC", S) -- that "cSHAKE with N=KMAC" phrase in the spec is the reason
 * this file has no sponge code of its own, only the key-block bytepad and the
 * trailer, both built from cshake.h's exported pieces.
 *
 * OWN HEADER, NO CONSUMER TODAY, MUST NOT REACH c/crypto/trust / TLS / aex /
 * login -- same statement as every other file built under this workflow; see
 * CLAUDE.md category (b) and cshake.h's copy of the same paragraph.
 *
 * KMAC vs KMACXOF -- THE TRAP THIS SPLIT EXISTS TO AVOID: the two differ
 * ONLY in the trailer appended after the message and before finalize: KMAC
 * appends right_encode(L) (L = the requested output length, IN BITS); KMACXOF
 * appends right_encode(0) UNCONDITIONALLY, regardless of how many bytes are
 * actually squeezed. Same key, same message, same requested length: different
 * tag. The tempting shortcut -- implement kmac128() by calling kmacxof128()
 * and truncating/matching lengths -- produces a function that fails every
 * official KMAC vector while passing every KMACXOF vector, which reads as "my
 * new MAC is broken" rather than "I dropped the length trailer", because nothing
 * about the failure points at the trailer. kmac_core() (kmac.c) takes the
 * trailer choice as an explicit `int is_xof` precisely so the four public
 * entry points cannot drift into that shortcut by accident. The shipped
 * negative control (KMAC_NEGCTL_XOF_TRAILER, see kmac.c) IS that shortcut,
 * built on purpose and watched failing -- see the test for what that looked
 * like.
 *
 * CONSTANT TIME: see cshake.c's note -- the key K is absorbed the same way
 * message bytes are (XOR into sponge state at a position that depends only on
 * how many bytes have been absorbed so far, not on K's value), so this
 * inherits cSHAKE's constant-time property without needing anything extra.
 * `Klen` itself (not the key's VALUE) does drive control flow (how many zero
 * bytes bytepad adds) -- that is a length, public in every realistic use of a
 * MAC, exactly like HMAC's key-length branch in hmac_hkdf.c. */

void kmac128(uint8_t *out, size_t outlen,
             const uint8_t *K, size_t Klen,
             const uint8_t *X, size_t Xlen,
             const uint8_t *S, size_t Slen);
void kmac256(uint8_t *out, size_t outlen,
             const uint8_t *K, size_t Klen,
             const uint8_t *X, size_t Xlen,
             const uint8_t *S, size_t Slen);

/* KMACXOF128/256: an arbitrary-length-output MAC. outlen has no upper bound
 * tied to the security strength (it is a squeeze length, not a digest size);
 * unlike kmac128/256, TWO CALLS WITH DIFFERENT outlen ARE NOT ONE A PREFIX OF
 * THE OTHER IN GENERAL for plain KMAC (its trailer depends on L) -- they ARE
 * for KMACXOF (trailer is always right_encode(0)), which is the entire reason
 * the XOF variant exists as a separate, spec-defined function rather than
 * "call KMAC and ask for more bytes". */
void kmacxof128(uint8_t *out, size_t outlen,
                const uint8_t *K, size_t Klen,
                const uint8_t *X, size_t Xlen,
                const uint8_t *S, size_t Slen);
void kmacxof256(uint8_t *out, size_t outlen,
                const uint8_t *K, size_t Klen,
                const uint8_t *X, size_t Xlen,
                const uint8_t *S, size_t Slen);

#endif
