#ifndef LOGIT_FIELD448_H
#define LOGIT_FIELD448_H
#include <stdint.h>

/* GF(2^448 - 2^224 - 1) -- the "Goldilocks" prime, RFC 7748 section 4.2.
 * This is the field curve448/X448 computes in; c/crypto/pubkey/x448.c is its
 * only consumer today. Ed448 needs the SAME field (it is a 4-isogeny of
 * curve448, RFC 7748 section 4.2) and should link against this file rather
 * than reimplementing it -- see field448.c's header for why 56-bit limbs make
 * that free.
 *
 * Representation: 8 limbs of 56 bits in uint64_t, limb i holding the
 * coefficient of 2^(56*i). This is not a stylistic echo of x25519.c's 5x51 --
 * for THIS prime it is closer to forced: 2^448-2^224-1 is a Solinas trinomial
 * whose middle term, 2^224, is 56*4, an EXACT limb boundary at radix 2^56.
 * That is what turns "the top half of a double-width product folds back into
 * the bottom AND the middle" (CLAUDE.md's warning) into two fixed-index
 * additions instead of a bit-shift-and-mask; see the derivation in
 * field448.c above fe448_mul. It is also why 56 bits and not, say, 32 or 64:
 * 448 = 56*8 exactly, AND each 56-bit limb packs into exactly 7 bytes with no
 * bit straddling a byte boundary in the 56-byte wire encoding (56 = 7*8) --
 * fe448_frombytes/fe448_tobytes are a plain byte copy per limb, not the
 * bit-shifted assembly x25519.c's fe_frombytes needs for 51-bit limbs over
 * 32-bit-aligned bytes.
 *
 * Every function here that touches a SECRET field element is constant time:
 * no branch and no memory access indexed by a limb's value. The one function
 * that is not, and cannot cheaply be made so, says so at its definition
 * (there is none in this file today -- fe448_invert uses a fixed public
 * exponent, so its branches are on compile-time-constant bits, not secret
 * data; see the comment there).
 */

typedef uint64_t fe448[8];

void fe448_copy(fe448 o, const fe448 a);
void fe448_frombytes(fe448 o, const uint8_t s[56]);
void fe448_tobytes(uint8_t s[56], const fe448 h);

void fe448_add(fe448 o, const fe448 a, const fe448 b);
void fe448_sub(fe448 o, const fe448 a, const fe448 b);
void fe448_mul(fe448 o, const fe448 a, const fe448 b);
void fe448_sq(fe448 o, const fe448 a);

/* Multiply by the curve448 Montgomery ladder constant a24 = 39081 (see
 * x448.c). A dedicated function for the same reason x25519.c has
 * fe_mul121666: it is a single-limb-times-scalar-constant loop, not a full
 * schoolbook multiply, and folding it into fe448_mul would hide that. */
void fe448_mul_a24(fe448 o, const fe448 a);

/* z^(p-2) = z^-1 for z != 0 (Fermat), by fixed exponentiation over a
 * compile-time-constant exponent -- see field448.c for why that constant
 * exponent makes this constant-time despite branching on exponent bits. */
void fe448_invert(fe448 o, const fe448 z);

/* Constant-time conditional swap: swaps a and b iff swap==1, leaves them
 * alone iff swap==0. swap MUST be exactly 0 or 1; UB (well-defined here, but
 * meaningless) otherwise. Branch-free, mask-based -- RFC 7748 section 5. */
void fe448_cswap(uint64_t swap, fe448 a, fe448 b);

#endif
