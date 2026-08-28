#ifndef LOGIT_PUBKEY_SECP256K1_H
#define LOGIT_PUBKEY_SECP256K1_H

#include <stdint.h>

/* ECDSA over secp256k1 (SEC 2 v2.0 section 2.4.1; ECDSA itself is FIPS 186-5 /
 * SEC 1 v2.0). NOT a NIST curve -- SECG/Certicom -- so unlike ecdsa.c's P-256/
 * P-384/P-521 there is no CAVP known-answer set for it anywhere. See
 * tests/unit/secp256k1_test.c for what stands in for one.
 *
 * THIS FILE IS NOT WIRED INTO ANYTHING. No consumer, no trust store, no TLS,
 * no aex, no login. It is a library primitive, gated on its own, exactly the
 * way pbkdf2.c and the P-521 addition to ecdsa.c were before anything called
 * them. See CLAUDE.md's category (b) discussion for why that distinction
 * matters here.
 *
 * WHY IT IS ITS OWN FILE AND NOT A FOURTH ROW IN ecdsa.c's curve table:
 * secp256k1 has a = 0. Every curve ecdsa.c already knows has a = -3, which is
 * what licenses the "3(X-Z^2)(X+Z^2)" shortcut in its Jacobian doubling
 * formula (jpt_dbl). Plugging a=0's parameters into that formula does not
 * fail loudly -- it computes a doubling for a DIFFERENT curve (a=-3, not 0),
 * and every point that formula produces satisfies y^2 = x^3 - 3x + 7, not
 * secp256k1's y^2 = x^3 + 7. Measured (SECP256K1_CTL_NIST_DOUBLE; see the
 * comment above secp256k1_dbl() in the .c file, and honest_assessment in the
 * agent transcript that built this file for the transcript of watching it
 * fail): in THIS implementation the corrupted keygen/sign round-trip is
 * caught even without an external vector, because point_on_curve() hard-
 * codes secp256k1's own a=0,b=7 equation and correctly refuses a public key
 * that does not satisfy it -- so the wrong curve is visible the moment two
 * independently-written pieces of the same file disagree with each other, not
 * only against an outside oracle. That is a property of THIS file having a
 * separate, curve-equation-only sanity check; it is not a property of the bug
 * class in general -- an implementation that skipped or shared that check (or
 * whose "on curve" test derived its (a,b) from the same corrupted table the
 * doubling used) would produce a self-consistent wrong signer with nothing
 * internal to catch it, which is why the external vectors below are still the
 * gate, not a curiosity: they are what would have caught this on an
 * implementation less lucky than this one. Point ADDITION does not depend on
 * `a`, so jpt_add / jpt_add_affine below are the same well-known formulas
 * ecdsa.c uses, independently transcribed rather than shared (each pubkey
 * curve file owns its field arithmetic here -- x25519.c and ed25519.c already
 * do not share bn code with ecdsa.c either, for the same p-shape reason noted
 * below).
 *
 * REDUCTION: p = 2^256 - 2^32 - 977. libsecp256k1's actual speed comes from a
 * reduction that exploits that specific shape (2^256 == 2^32 + 977 mod p).
 * This file does NOT implement that trick. It runs a generic fixed-width
 * Barrett reduction, the same *kind* of engine ecdsa.c already uses for the
 * NIST primes, just re-derived here for a single 256-bit modulus rather than
 * copied from ecdsa.c's static, curve-table-shaped version. A transcribed
 * NIST-style special reduction would be wrong for this prime's completely
 * different shape; a generic one is slower and is verifiably correct, which
 * is the trade this file is built to make -- nothing here has a consumer that
 * needs the speed.
 *
 * LOW-S: this file does NOT enforce it. FIPS 186-5 / SEC1 accept any valid
 * (r,s) with s in [1,n-1]; requiring s <= n/2 (BIP-62) is a Bitcoin consensus
 * policy layered on top of ECDSA, not part of ECDSA itself, and nothing here
 * claims to implement Bitcoin's transaction rules. The Wycheproof vector file
 * this gate runs (ecdsa_secp256k1_sha256_test.json) carries no `"result":
 * "acceptable"` cases for this curve -- every case is unambiguously valid or
 * invalid -- so this policy choice happens to not be exercised by the gate
 * either way; it is recorded here so the next reader does not have to
 * rediscover it from the vector file's absence of the word "acceptable".
 *
 * DETERMINISTIC k: NOT implemented. RFC 6979 does not cover secp256k1 (it is
 * a NIST-curve RFC); the deterministic-k convention Bitcoin/Ethereum wallets
 * actually use is the same HMAC-DRBG construction applied to this curve by
 * ecosystem convention (BIP-32/BIP-62 tooling), not a value any standards
 * body publishes vectors for. Rather than ship "RFC 6979" vectors that are
 * really "what our own HMAC-DRBG produces", secp256k1_sign takes k as
 * caller-supplied raw randomness -- exactly like ecdh_keygen's `priv` in
 * ecdsa.c -- which keeps this file free of any hash dependency and host-
 * testable standalone, and keeps every vector below externally sourced. A
 * caller that wants deterministic k builds RFC 6979's HMAC-DRBG externally
 * (ecdsa.c already has one) and feeds the result in as k.
 *
 * CONSTANT TIME: not attempted, and not silently. Exactly like ecdsa.c's
 * P-256/P-384/P-521 arithmetic, mod_mul's Barrett corrections, mod_add/
 * mod_sub's conditional subtracts, and secp256k1_ladder's data-dependent
 * point-add-or-not all branch on secret bits when `d` (the private key) or
 * `k` (the per-signature nonce) drive them. secp256k1_sign and
 * secp256k1_keygen scalar-blind exactly the way ecdh_keygen/ecdsa_sign do
 * (kb = scalar + rho*n for a caller-supplied rho, so n*P being the identity
 * makes kb*P == scalar*P while the bit pattern the ladder walks changes every
 * call) -- which narrows a single-trace timing leak the same amount it does
 * there, no more. That defence was argued in ecdsa.c on the strength of the
 * scalar being EPHEMERAL (a fresh ECDHE share, used once). It does not fully
 * carry over to a long-term secp256k1 signing key the way a wallet would use
 * one: blinding changes the bit pattern per call but the arithmetic is still
 * data-dependent branch-and-index, which a local cache/power-analysis
 * adversary can still exploit across many calls against the SAME key. A
 * production signer for a long-lived key needs a real constant-time ladder
 * (Montgomery ladder / fixed windowed comb with no data-dependent branch or
 * table index); that is future work, named rather than silently skipped, and
 * is exactly why this file is not wired into anything that would hold a
 * long-term key today. secp256k1_verify touches only public data and makes
 * no constant-time claim at all, same as ecdsa_verify.
 */

#define SECP256K1_NBYTES 32

/* Uncompressed SEC1 point encoding: 0x04 || X || Y, 65 bytes. sig is r||s,
 * 32 bytes each, 64 bytes total -- raw, not DER (see secp256k1_verify_der for
 * that). hash/hlen: the leftmost min(hlen, 32) bytes of the digest are taken
 * as the integer e (SEC1 4.1.4 step 3), exactly as ecdsa_verify does. Returns
 * 1 if the signature verifies, 0 otherwise -- never a partial answer. */
int secp256k1_verify(const uint8_t *pub, const uint8_t *sig,
                      const uint8_t *hash, int hlen);

/* Same as secp256k1_verify, but `sig` is a DER SEQUENCE{INTEGER r, INTEGER
 * s} (the X9.62/SEC1 wire encoding every real signer emits), parsed
 * STRICTLY per X.690: definite short-form lengths only where they fit in one
 * byte, no non-minimal long-form lengths, each INTEGER's content minimal
 * (a leading 0x00 is accepted only when required to keep the value
 * non-negative), no trailing bytes inside or after the SEQUENCE. Anything
 * that fails to parse is reported as 0 -- SEC1 4.1.4 step 1 requires a
 * verifier that cannot decode the signature to reject, which is not
 * distinguishable from "decoded fine, math said no" at this boundary; a
 * caller that needs to tell "malformed" apart from "wrong" should parse the
 * DER itself before calling this. */
int secp256k1_verify_der(const uint8_t *pub, const uint8_t *sig, int siglen,
                          const uint8_t *hash, int hlen);

/* pub receives the uncompressed point 0x04||X||Y (65 bytes) for the private
 * scalar `priv` (32 bytes, raw big-endian). Returns -1 if priv is not in
 * [1, n-1] -- the caller must re-randomise and retry, same contract as
 * ecdh_keygen -- else 0. `blind` is scalar-blinding randomness (see the
 * CONSTANT TIME note above); 0 is a legal value and simply blinds with
 * rho's low end of its forced range. */
int secp256k1_keygen(const uint8_t *priv, uint32_t blind, uint8_t *pub);

/* Sign `hash` (hlen bytes) under the private scalar `priv` with the
 * caller-supplied per-signature secret `k` (32 bytes, raw big-endian --
 * MUST be uniformly random and MUST NOT ever repeat across two signatures
 * under the same key: a repeated or predictable k recovers `priv` outright
 * from any two signatures, the classic ECDSA failure). `sig` receives
 * r||s, 32 bytes each. Returns 0 on success; -1 if priv or k is out of
 * range, or -- so rarely it is unreachable in practice -- k happened to
 * produce r == 0 or s == 0, in which case the caller must draw a fresh k
 * and retry (SEC1 4.1.3 steps 3 and 6). `blind` blinds the k*G scalar
 * multiplication exactly as secp256k1_keygen blinds priv*G; it does not
 * change r or s (kb*G == k*G for any blind), only the bit pattern the
 * ladder walks to reach it. */
int secp256k1_sign(const uint8_t *priv, const uint8_t *hash, int hlen,
                    const uint8_t *k, uint32_t blind, uint8_t *sig);

#endif
