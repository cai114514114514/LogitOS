#include "x448.h"
#include "field448.h"

/* X448 -- RFC 7748 sections 4.2 (curve448), 5 (the X448 function / Montgomery
 * ladder), 6.2 (ECDH usage + the Alice/Bob test vector).
 *
 * THE TWO TRAPS THIS FILE EXISTS TO NOT FALL INTO (CLAUDE.md named both
 * before this was written, which is the only reason they are traps avoided
 * rather than bugs found):
 *
 *  1. X448's clamp is NOT X25519's clamp, copied. X25519 clears the low
 *     THREE bits of byte 0 (cofactor 8) and both sets bit 254 and clears bit
 *     255 of the top byte (255-bit field inside a 256-bit wire encoding).
 *     X448 clears the low TWO bits of byte 0 (cofactor 4 -- curve448's
 *     cofactor, RFC 7748 section 4.2) and sets ONLY the top bit (bit 447) of
 *     byte 55 -- nothing needs clearing up there, because the field is
 *     exactly 448 bits and the encoding is exactly 56 bytes, so there is no
 *     X25519-style "unused top bit of an oversized byte" to begin with. RFC
 *     7748 section 5's decodeScalar448 is quoted exactly:
 *       k_list[0] &= 252; k_list[55] |= 128
 *     LOGIT_X448_CTL_BAD_CLAMP below is this exact bug -- clamping X25519-
 *     style (clear 3 bits, cofactor 8) onto a curve448 scalar -- reintroduced
 *     on purpose and watched failing (see the test file).
 *
 *  2. a24 = 39081, not 121665 -- see field448.c's fe448_mul_a24, which is the
 *     only place the constant appears, and its own control for this.
 *
 * u-COORDINATE DECODING: RFC 7748 section 5 requires X25519 to mask the top
 * bit of its 32-byte encoding before use ("implementations of X25519 (but
 * not X448) MUST mask the most significant bit in the final byte") --
 * because X25519's 255-bit field is carried in a 256-bit wire encoding with
 * one spare bit. X448's field is exactly 448 bits in exactly 56 bytes: there
 * is no spare bit, so fe448_frombytes below does no masking at all, matching
 * the RFC's explicit "(but not X448)".
 *
 * NON-CANONICAL u: RFC 7748 section 5 also requires accepting a u-coordinate
 * that is not fully reduced mod p ("implementations MUST accept non-canonical
 * values and process them as if they had been reduced"). fe448_frombytes
 * cannot produce anything outside [0, 2^448) from 56 bytes, and field448.c's
 * fe448_mul was checked (see its header) against every a,b in that FULL
 * range, not just [0,p) -- so a non-canonical u is handled correctly by
 * construction, not by a special case. This is exercised for real by the
 * Wycheproof `NonCanonicalPublic`-flagged cases in tests/unit/x448_test.c.
 *
 * CONSTANT TIME: the ladder below touches the scalar bit-by-bit but the bit
 * only ever selects a cswap mask (fe448_cswap, mask-based, no branch) --
 * never an array index, never an `if`. That is the RFC 7748 section 5
 * requirement quoted in field448.c's header, applied here. The one non-
 * constant-time step in the whole x448()/fe448_invert() call graph is
 * documented at its own definition in field448.c, not here, because it does
 * not depend on secret data at all (see that comment).
 */

static void x448_clamp(uint8_t e[56], const uint8_t scalar[56])
{
    for (int i = 0; i < 56; i++) e[i] = scalar[i];
#ifdef LOGIT_X448_CTL_BAD_CLAMP
    e[0] &= 248;    /* X25519's 3-bit clamp (cofactor 8) on a cofactor-4 curve */
#else
    e[0] &= 252;    /* RFC 7748 section 5: clear the low TWO bits (cofactor 4) */
#endif
    e[55] |= 128;   /* set bit 447; nothing above it to clear -- see file header */
}

void x448(uint8_t out[56], const uint8_t scalar[56], const uint8_t point[56])
{
    uint8_t e[56];
    x448_clamp(e, scalar);

    fe448 x1, x2, z2, x3, z3;
    fe448_frombytes(x1, point);
    static const fe448 one = {1, 0, 0, 0, 0, 0, 0, 0};
    static const fe448 zero = {0, 0, 0, 0, 0, 0, 0, 0};
    fe448_copy(x2, one);  fe448_copy(z2, zero);
    fe448_copy(x3, x1);   fe448_copy(z3, one);

    uint64_t swap = 0;
    for (int t = 447; t >= 0; t--) {
        uint64_t kt = (e[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fe448_cswap(swap, x2, x3);
        fe448_cswap(swap, z2, z3);
        swap = kt;

        fe448 a, aa, b, bb, e_, c, d, da, cb, t0, t1;
        fe448_add(a, x2, z2);   fe448_sq(aa, a);
        fe448_sub(b, x2, z2);   fe448_sq(bb, b);
        fe448_sub(e_, aa, bb);
        fe448_add(c, x3, z3);   fe448_sub(d, x3, z3);
        fe448_mul(da, d, a);    fe448_mul(cb, c, b);
        fe448_add(t0, da, cb);  fe448_sq(x3, t0);
        fe448_sub(t1, da, cb);  fe448_sq(t1, t1);   fe448_mul(z3, x1, t1);
        fe448_mul(x2, aa, bb);
        fe448_mul_a24(t0, e_);  fe448_add(t0, aa, t0); fe448_mul(z2, e_, t0);
    }
    fe448_cswap(swap, x2, x3);
    fe448_cswap(swap, z2, z3);

    fe448 zi;
    fe448_invert(zi, z2);
    fe448_mul(x2, x2, zi);
    fe448_tobytes(out, x2);
}

void x448_base(uint8_t out[56], const uint8_t scalar[56])
{
    uint8_t base[56] = {5};
    for (int i = 1; i < 56; i++) base[i] = 0;
    x448(out, scalar, base);
}
