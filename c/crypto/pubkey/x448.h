#ifndef LOGIT_X448_H
#define LOGIT_X448_H
#include <stdint.h>

/* X448 (RFC 7748 sections 4.2, 5, 6.2) -- Diffie-Hellman over curve448, the
 * ~224-bit-security counterpart to the X25519 already in
 * c/crypto/pubkey/x25519.c. See x448.c's header for the field it runs over
 * and the two clamp/constant traps this file exists to have gotten right.
 *
 * Originally only a primitive with a known-answer test. Since 2026-09-10,
 * net/tls uses it for TLS 1.2/1.3 client and TLS 1.3 server key agreement;
 * that protocol layer validates the 56-byte width and rejects zero secrets.
 *
 * out[56] = X448(scalar[56], point[56]). scalar is clamped internally per
 * RFC 7748 section 5 -- callers pass raw random bytes, not a pre-clamped
 * scalar (same convention as x25519()).
 *
 * ON AN ALL-ZERO OUTPUT: RFC 7748 section 6.2 states the all-zero check as a
 * MAY for X448's use in a DH protocol ("both sides MAY check ... whether the
 * resulting shared K is the all-zero value and abort if so"), not a MUST,
 * and section 5 places no requirement on X448() itself at all. This function
 * performs NO such check and returns whatever the ladder computes, including
 * an all-zero result for a low-order or otherwise adversarial `point` --
 * exactly x25519()'s existing behaviour in this tree (x25519.c makes no
 * zero-output check either), kept for the same "one jar, two doors" reason:
 * two sibling primitives disagreeing on this would be the kind of thing
 * CLAUDE.md's rule 4 is about. A caller building a protocol on top of this
 * owns the all-zero check required by its protocol (TLS requires it). */
void x448(uint8_t out[56], const uint8_t scalar[56], const uint8_t point[56]);

/* X448(scalar, 5) -- the base point is u=5 (one byte 0x05 then 55 zero
 * bytes), RFC 7748 section 6.2. */
void x448_base(uint8_t out[56], const uint8_t scalar[56]);

#endif
