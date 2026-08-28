#include "field448.h"

/* GF(2^448 - 2^224 - 1), 8 limbs of 56 bits. See field448.h for why this
 * radix is close to forced by the prime's shape, not a stylistic choice.
 *
 * THE REDUCTION, DERIVED (read this before touching fe448_mul).
 * p = 2^448 - 2^224 - 1, so 2^448 = 2^224 + 1 (mod p). A schoolbook multiply
 * of two 8-limb numbers produces double-width limbs t[0..14] (t[k] is the
 * coefficient of 2^(56k)). For k <= 7, t[k] contributes to output limb k
 * directly. For k >= 8, write 56k = 56(k-8) + 448, so
 *
 *   t[k] * 2^(56k) = t[k] * 2^(56(k-8)) * 2^448
 *                  = t[k] * 2^(56(k-8)) * (2^224 + 1)      (mod p)
 *                  = t[k] * 2^(56(k-8)) + t[k] * 2^(56(k-4))
 *
 * (224 = 56*4 is the whole reason 56-bit limbs are the natural radix for
 * this prime: the fold lands EXACTLY on a limb boundary, twice). k ranges
 * 8..14, so (k-8) ranges 0..6 and (k-4) ranges 4..10. The three terms whose
 * high half (k-4) is itself >= 8 -- k=12,13,14, landing on "limb" 8,9,10 --
 * get folded AGAIN by the identical rule (index m -> limb(m-8) + limb(m-4)
 * with m-8 in 0..2 and m-4 in 4..6). Working through both folds by hand
 * gives, entirely in terms of the ORIGINAL t[]:
 *
 *   o[0] = t[0] + t[8]  + t[12]
 *   o[1] = t[1] + t[9]  + t[13]
 *   o[2] = t[2] + t[10] + t[14]
 *   o[3] = t[3] + t[11]
 *   o[4] = t[4] + t[8]  + 2*t[12]
 *   o[5] = t[5] + t[9]  + 2*t[13]
 *   o[6] = t[6] + t[10] + 2*t[14]
 *   o[7] = t[7] + t[11]
 *
 * This was checked, not trusted: a 2000-trial Python model of exactly this
 * limb arithmetic (random a,b in [0,2^448), schoolbook multiply, apply the
 * formula above, reduce mod p) was compared against Python's native (a*b)%p
 * before a line of this file was written. CLAUDE.md's warning about this
 * prime -- "a reduction written by analogy to 25519 is wrong in a way that
 * shows up only for a fraction of inputs" -- is why that step happened before
 * this file did, not after.
 *
 * MAGNITUDE, so the __uint128_t accumulators below are provably not lying:
 * fe448_add/fe448_sub do NOT carry-propagate their output (matching
 * x25519.c's fe_add/fe_sub); every add or sub in x448.c's ladder is followed
 * immediately by exactly one fe448_mul or fe448_sq, never chained into a
 * second add/sub. Inputs to those two are therefore always either freshly
 * carry-propagated (< 2^56 + a couple of ULP) or the result of ONE add/sub of
 * such values (< roughly 2^58.5, using the +4p headroom fe448_sub adds -- see
 * there). A single limb-pair product is then at most on the order of 2^59 *
 * 2^59 = 2^118; fe448_mul sums at most 3 such terms (one doubled) into any
 * one output accumulator, so at most a shade over 2^120 -- comfortably inside
 * __uint128_t's 2^128, with room to spare rather than balanced on the edge.
 *
 * CONSTANT TIME: fe448_add, fe448_sub, fe448_mul, fe448_sq, fe448_mul_a24 and
 * fe448_cswap touch secret limb VALUES but never branch on them and never use
 * one as a memory index -- every loop bound and every carry step is a fixed
 * arithmetic mask (`c = x >> 56`, not `if (x >= ...)`), the same discipline
 * x25519.c's field layer uses. fe448_invert is the one function in this file
 * whose source has an `if` gated on data that came from a field element --
 * see the comment there for why that is still constant time with respect to
 * the SECRET (the data it branches on is a compile-time public constant, the
 * exponent p-2, not z).
 */

#define M56 0xffffffffffffffULL /* 2^56 - 1, 14 hex digits */

/* p's own limbs, for fe448_sub's headroom add and nowhere else. Derived from
 * p = 2^448-2^224-1 = "all 448 bits set except bit 224" (subtracting 2^224
 * from 2^448-1, whose bits are all 1, just clears that one bit; no borrow).
 * Bit 224 is exactly the low bit of limb 4, so limb4 = 2^56-2 and every other
 * limb is 2^56-1. */
static const uint64_t P448[8] = {
    M56, M56, M56, M56, M56 - 1, M56, M56, M56
};

void fe448_copy(fe448 o, const fe448 a)
{
    for (int i = 0; i < 8; i++) o[i] = a[i];
}

/* 56 bits is exactly 7 bytes -- no bit ever straddles a byte boundary, unlike
 * x25519's 51-bit limbs over 32-bit-aligned bytes. */
void fe448_frombytes(fe448 o, const uint8_t s[56])
{
    for (int i = 0; i < 8; i++) {
        const uint8_t *b = s + 7 * i;
        o[i] = (uint64_t)b[0]       | (uint64_t)b[1] << 8  | (uint64_t)b[2] << 16 |
               (uint64_t)b[3] << 24 | (uint64_t)b[4] << 32 | (uint64_t)b[5] << 40 |
               (uint64_t)b[6] << 48;
    }
}

void fe448_tobytes(uint8_t s[56], const fe448 h)
{
    fe448 t;
    fe448_copy(t, h);

    /* Defensive carry-propagate first: fe448_mul's own output can be a limb
     * or two ABOVE 2^56 by a tiny amount (the leftover wraparound carry it
     * adds to limb0/limb4 last, mirroring x25519's fe_tobytes doing the same
     * "carry-reduce, several rounds" before it trusts the value). Three
     * rounds is the same margin x25519.c budgets for the same reason. */
    for (int round = 0; round < 3; round++) {
        uint64_t c;
        c = t[0] >> 56; t[0] &= M56; t[1] += c;
        c = t[1] >> 56; t[1] &= M56; t[2] += c;
        c = t[2] >> 56; t[2] &= M56; t[3] += c;
        c = t[3] >> 56; t[3] &= M56; t[4] += c;
        c = t[4] >> 56; t[4] &= M56; t[5] += c;
        c = t[5] >> 56; t[5] &= M56; t[6] += c;
        c = t[6] >> 56; t[6] &= M56; t[7] += c;
        c = t[7] >> 56; t[7] &= M56;
        t[0] += c; t[4] += c;   /* 2^448 == 2^224 + 1 (mod p) */
    }

    /* t is now < 2^448 but not necessarily < p. Test by adding p's additive
     * inverse mod 2^448, which is 2^224+1 (i.e. +1 to limb0 AND +1 to limb4,
     * the same two-limb shape the multiply reduction uses): if that overflows
     * limb7 -- carries a 9th limb's worth out the top -- then t was >= p, and
     * the value with that overflow bit dropped (i.e. taken mod 2^448, which
     * happens for free because there is no 9th limb to hold it) is exactly
     * t - p. If it does NOT overflow, t was already canonical and the
     * candidate must be discarded. Both branches always run; the result is
     * chosen by an arithmetic mask, never an `if`, so which case occurred is
     * not visible in the instruction trace. */
    fe448 cand;
    fe448_copy(cand, t);
    cand[0] += 1; cand[4] += 1;
    uint64_t c;
    c = cand[0] >> 56; cand[0] &= M56; cand[1] += c;
    c = cand[1] >> 56; cand[1] &= M56; cand[2] += c;
    c = cand[2] >> 56; cand[2] &= M56; cand[3] += c;
    c = cand[3] >> 56; cand[3] &= M56; cand[4] += c;
    c = cand[4] >> 56; cand[4] &= M56; cand[5] += c;
    c = cand[5] >> 56; cand[5] &= M56; cand[6] += c;
    c = cand[6] >> 56; cand[6] &= M56; cand[7] += c;
    c = cand[7] >> 56; cand[7] &= M56;   /* c = 1 iff t >= p */

    uint64_t mask = (uint64_t)0 - c;     /* all-1 iff t >= p, else all-0 */
    for (int i = 0; i < 8; i++) t[i] = (t[i] & ~mask) | (cand[i] & mask);

    for (int i = 0; i < 8; i++) {
        const uint64_t v = t[i];
        uint8_t *b = s + 7 * i;
        b[0] = (uint8_t)(v);       b[1] = (uint8_t)(v >> 8);
        b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
        b[4] = (uint8_t)(v >> 32); b[5] = (uint8_t)(v >> 40);
        b[6] = (uint8_t)(v >> 48);
    }
}

void fe448_add(fe448 o, const fe448 a, const fe448 b)
{
    for (int i = 0; i < 8; i++) o[i] = a[i] + b[i];
}

/* Add 4p before subtracting to stay positive without a borrow chain -- 4p's
 * limbs are each just under 2^58, comfortably above the < ~2^57 that any
 * input reaching this function is bounded by (see the magnitude note at the
 * top of the file), the same "add a multiple of p" idiom x25519.c's fe_sub
 * uses with 2p for its own, smaller, limbs. */
void fe448_sub(fe448 o, const fe448 a, const fe448 b)
{
    for (int i = 0; i < 8; i++) o[i] = a[i] + 4 * P448[i] - b[i];
}

void fe448_mul(fe448 o, const fe448 a, const fe448 b)
{
    __uint128_t t[15];
    for (int k = 0; k < 15; k++) t[k] = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            t[i + j] += (__uint128_t)a[i] * b[j];

    __uint128_t r[8];
    r[0] = t[0] + t[8]  + t[12];
    r[1] = t[1] + t[9]  + t[13];
    r[2] = t[2] + t[10] + t[14];
    r[3] = t[3] + t[11];
    r[4] = t[4] + t[8]  + 2 * t[12];
    r[5] = t[5] + t[9]  + 2 * t[13];
    r[6] = t[6] + t[10] + 2 * t[14];
    r[7] = t[7] + t[11];

    /* THE BUG THIS COMMENT REPLACES, left in the record on purpose: the first
     * version of this reduction used a `uint64_t` carry variable, copying
     * x25519.c's fe_mul idiom verbatim. That is safe THERE because a 5-limb
     * 51-bit product's accumulator tops out around 2^104 and a carry out of
     * one limb is a few bits. It is NOT safe here: r[0..7] are each a sum of
     * up to THREE of the double-width t[] terms (one doubled), so they run up
     * to roughly 2^121-2^123, and `r[i] >> 56` is then itself up to ~66
     * BITS -- silently truncated by a uint64_t cast, every time, not just
     * near an edge. That is exactly the "wrong in a way that shows up only
     * for a fraction of inputs" shape CLAUDE.md warned this prime produces:
     * invisible whenever the discarded high bits of the carry happened to be
     * zero, wrong whenever they weren't. Caught by tracing a real ladder step
     * against an independent Python model of the EXACT limbs involved (not
     * just the abstract reduction formula, which is what the 2000-trial
     * check above the multiply actually covers) and finding the divergence
     * started at that cast.
     *
     * The fix keeps every carry in __uint128_t and runs the fold-and-wrap
     * TWICE. Once is not enough: after the first pass each limb is still
     * only bounded by the carry chain's own width (up to ~66 bits again, from
     * wrapping r[7]'s overflow into limb0/limb4), not yet < 2^56. A second
     * pass over now-much-smaller values finishes it. This was checked, not
     * assumed: a Python model of this exact two-pass loop, fed the worst
     * input this file's own operand-magnitude argument allows (every limb at
     * 2^59-1, well above what fe448_sub's 4p headroom actually produces),
     * converges to all eight limbs < 2^56 after pass 2, and stays converged
     * through 5 passes -- i.e. two is not a coincidence of the specific test
     * vector that exposed the truncation bug above, it is where the carry
     * chain's magnitude actually bottoms out. */
    for (int pass = 0; pass < 2; pass++) {
        __uint128_t c = 0, nr[8];
        for (int i = 0; i < 8; i++) {
            __uint128_t v = r[i] + c;
            nr[i] = v & M56;
            c = v >> 56;
        }
        nr[0] += c; nr[4] += c;        /* 2^448 == 2^224 + 1 (mod p) */
        for (int i = 0; i < 8; i++) r[i] = nr[i];
    }
    for (int i = 0; i < 8; i++) o[i] = (uint64_t)r[i];   /* now provably < 2^56 */
}

void fe448_sq(fe448 o, const fe448 a)
{
    fe448_mul(o, a, a);
}

/* a24 = (156326 - 2) / 4 = 39081 (RFC 7748 section 4.2, curve448's A=156326).
 * NOT x25519's 121665 -- CLAUDE.md's warning about this is the whole reason
 * this is its own function rather than a call site writing the literal:
 * `fe448_mul_a24` is one place to get it right instead of every ladder step.
 * A wrong constant here is not a crash, it's a working Montgomery ladder over
 * a DIFFERENT curve -- see tests/unit/x448_test.c's LOGIT_X448_CTL_BAD_A24
 * control, which is exactly this bug, reintroduced on purpose and watched
 * failing. */
void fe448_mul_a24(fe448 o, const fe448 a)
{
#ifdef LOGIT_X448_CTL_BAD_A24
    const uint64_t A24 = 121665; /* x25519's constant -- the transcription bug */
#else
    const uint64_t A24 = 39081;
#endif
    __uint128_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (__uint128_t)a[i] * A24;
    uint64_t c;
    c = (uint64_t)(t[0] >> 56); o[0] = (uint64_t)t[0] & M56; t[1] += c;
    c = (uint64_t)(t[1] >> 56); o[1] = (uint64_t)t[1] & M56; t[2] += c;
    c = (uint64_t)(t[2] >> 56); o[2] = (uint64_t)t[2] & M56; t[3] += c;
    c = (uint64_t)(t[3] >> 56); o[3] = (uint64_t)t[3] & M56; t[4] += c;
    c = (uint64_t)(t[4] >> 56); o[4] = (uint64_t)t[4] & M56; t[5] += c;
    c = (uint64_t)(t[5] >> 56); o[5] = (uint64_t)t[5] & M56; t[6] += c;
    c = (uint64_t)(t[6] >> 56); o[6] = (uint64_t)t[6] & M56; t[7] += c;
    c = (uint64_t)(t[7] >> 56); o[7] = (uint64_t)t[7] & M56;
    o[0] += c; o[4] += c;
}

void fe448_cswap(uint64_t swap, fe448 a, fe448 b)
{
    uint64_t m = (uint64_t)0 - swap;
    for (int i = 0; i < 8; i++) {
        uint64_t t = m & (a[i] ^ b[i]);
        a[i] ^= t; b[i] ^= t;
    }
}

/* z^(p-2) = z^-1 (Fermat; p is prime). EXP448 below is p-2 as a fixed,
 * compile-time PUBLIC constant, little-endian byte order matching
 * fe448_frombytes -- reproduce it with:
 *   python3 -c "print((2**448-2**224-1-2).to_bytes(56,'little').hex())"
 *
 * CONSTANT TIME, ARGUED RATHER THAN ASSUMED. This function does branch on
 * data (`if (bit) ...` below) -- the one place in this file that does. That
 * is safe because the bit it branches on comes from EXP448, a fixed array
 * baked into the binary, identical on every call; it does not depend on z in
 * any way. The sequence of squarings and multiplies executed is therefore
 * the same 448 squarings + up to 446 multiplies every single time this
 * function is called, for every z -- what varies between calls is only the
 * DATA flowing through that fixed sequence, never which operations run or
 * how many. That is the same standard the RFC itself sets: "the same
 * sequence of field operations is performed for all values of the secret
 * key" (7748 section 5.1) -- z is the secret key material here, EXP448 is
 * not key material at all.
 *
 * This is a plain square-and-multiply over all 448 exponent bits rather than
 * a hand-built addition chain (the kind x25519.c's fe_invert uses, built
 * around p-2's specific run-length structure for ~254 squarings + ~11
 * multiplies). That is a deliberate trade: an addition chain transcribed by
 * hand from memory, for THIS prime, is exactly the kind of thing CLAUDE.md's
 * "wrong in a way that shows up only for a fraction of inputs" warning is
 * about, and there was no official reference chain to copy verbatim. Plain
 * exponentiation by the literal bits of a value that was computed once in
 * Python and is checked by every KAT in tests/unit/x448_test.c has no step
 * that can be transcribed wrong without every single test failing. It costs
 * roughly 40x the multiplies of a tuned chain; this runs once per X448 call
 * (the final inversion, not inside the ladder loop), on a primitive with no
 * consumer yet, so that cost has nowhere to be felt today. */
static const uint8_t EXP448[56] = {
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static int exp448_bit(int i)
{
    return (EXP448[i >> 3] >> (i & 7)) & 1;
}

void fe448_invert(fe448 o, const fe448 z)
{
    fe448 r;
    fe448_copy(r, z);              /* bit 447 of EXP448 is always 1 */
    for (int i = 446; i >= 0; i--) {
        fe448_sq(r, r);
        if (exp448_bit(i)) fe448_mul(r, r, z);
    }
    fe448_copy(o, r);
}
