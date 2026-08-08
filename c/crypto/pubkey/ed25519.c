#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

/* Ed25519 (RFC 8032) -- SIGN, VERIFY and KEY GENERATION.
 *
 * WHY THIS IS THE FIRST SIGNING PRIMITIVE IN THE TREE
 * ---------------------------------------------------
 * Everything in c/crypto before this file is a TLS *client*: it verifies other
 * people's signatures and does ephemeral key agreement. Nothing could produce a
 * signature, which means the machine had no identity and could not check
 * anything it downloaded. Ed25519 is the right first one to add:
 *
 *   - The per-signature nonce is DERIVED, not random: r = H(prefix || M) mod L
 *     where `prefix` is the second half of the expanded private key. A broken
 *     RNG therefore cannot leak the key. ECDSA's nonce is random and a biased
 *     or repeated one recovers the private scalar outright (PS3, 2010; Sony's
 *     was a constant). Since this kernel's entropy is exactly what Part 1 of
 *     this workstream is fixing, "the signature scheme does not depend on it"
 *     is a property worth having on the very first one.
 *   - It is small: one hash (SHA-512, already here) and one curve. No new
 *     hash, no DER encoding, no salt length to recover.
 *   - It empties tools/genroots.py's skip list, which up to now compiled the
 *     NAMES of unusable roots into the kernel and printed them at first
 *     handshake.
 *
 * CONSTANT TIME -- said explicitly, because this is the first file here where
 * it matters. ecdsa_verify's own note is right that verification touches only
 * public data. Signing and key generation do not.
 *
 *   ed25519_keypair / ed25519_sign are constant time with respect to the
 *   private key:
 *     - ge_scalarmult_base runs a fixed 255 iterations, each doing one double
 *     - and one addition, and selects between them with fe_cmov (an arithmetic
 *       mask, no branch, no table index). No windowing, no precomputed table:
 *       a window index is a secret used as a memory address, which is the
 *       cache-timing leak that has to be either avoided or scrubbed, and the
 *       cheap way to avoid it is to not have a table.
 *     - sc_reduce / sc_muladd are fixed-trip binary reductions whose only
 *       conditional is a masked subtract.
 *     - The field layer (fe_mul, fe_sq, fe_invert, fe_pow) has no data-dependent
 *       branch and no data-dependent index; fe_invert's ladder is a fixed
 *       addition chain.
 *   ed25519_verify is NOT constant time and does not need to be: signature,
 *   message and public key are all public. ge_double_scalarmult uses the same
 *   fixed-trip ladder anyway (it is simply the code that was already there),
 *   so in practice it leaks nothing either, but the guarantee is not claimed.
 *   ge_frombytes' sqrt is likewise a fixed addition chain.
 *
 *   NOT constant time, and it cannot be: the comparison in ed25519_verify of
 *   the recomputed R against the transmitted one is a plain memcmp over public
 *   bytes. That is correct -- an early exit there tells an attacker only how
 *   many bytes of a signature they already hold happened to match.
 *
 * Field arithmetic is radix-2^51 in five uint64 limbs, the same representation
 * x25519.c uses. It is duplicated rather than shared because x25519.c's copy is
 * Montgomery-x-only and has no need for fe_cmov / fe_neg / fe_pow22523, and
 * making one header out of two static sets is a bigger change to a working file
 * than it is worth. */

/* ---------------------------------------------------------------- field ---- */

typedef uint64_t fe[5];

#define M51 0x7ffffffffffffULL

static void fe_0(fe o)              { for (int i=0;i<5;i++) o[i]=0; }
static void fe_1(fe o)              { o[0]=1; for (int i=1;i<5;i++) o[i]=0; }
static void fe_copy(fe o, const fe a){ for (int i=0;i<5;i++) o[i]=a[i]; }
static void fe_add(fe o, const fe a, const fe b){ for(int i=0;i<5;i++) o[i]=a[i]+b[i]; }

static void fe_sub(fe o, const fe a, const fe b)
{
    /* + 2p, so the result stays non-negative before the carry chain. */
    o[0] = a[0] + 0xfffffffffffdaULL - b[0];
    o[1] = a[1] + 0xffffffffffffeULL - b[1];
    o[2] = a[2] + 0xffffffffffffeULL - b[2];
    o[3] = a[3] + 0xffffffffffffeULL - b[3];
    o[4] = a[4] + 0xffffffffffffeULL - b[4];
}

static void fe_mul(fe o, const fe a, const fe b)
{
    __uint128_t r0,r1,r2,r3,r4;
    uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
    uint64_t b0=b[0],b1=b[1],b2=b[2],b3=b[3],b4=b[4];
    uint64_t b1_19=b1*19,b2_19=b2*19,b3_19=b3*19,b4_19=b4*19;
    r0=(__uint128_t)a0*b0+(__uint128_t)a1*b4_19+(__uint128_t)a2*b3_19+(__uint128_t)a3*b2_19+(__uint128_t)a4*b1_19;
    r1=(__uint128_t)a0*b1+(__uint128_t)a1*b0   +(__uint128_t)a2*b4_19+(__uint128_t)a3*b3_19+(__uint128_t)a4*b2_19;
    r2=(__uint128_t)a0*b2+(__uint128_t)a1*b1   +(__uint128_t)a2*b0   +(__uint128_t)a3*b4_19+(__uint128_t)a4*b3_19;
    r3=(__uint128_t)a0*b3+(__uint128_t)a1*b2   +(__uint128_t)a2*b1   +(__uint128_t)a3*b0   +(__uint128_t)a4*b4_19;
    r4=(__uint128_t)a0*b4+(__uint128_t)a1*b3   +(__uint128_t)a2*b2   +(__uint128_t)a3*b1   +(__uint128_t)a4*b0;
    uint64_t c;
    c=(uint64_t)(r0>>51); o[0]=(uint64_t)r0&M51; r1+=c;
    c=(uint64_t)(r1>>51); o[1]=(uint64_t)r1&M51; r2+=c;
    c=(uint64_t)(r2>>51); o[2]=(uint64_t)r2&M51; r3+=c;
    c=(uint64_t)(r3>>51); o[3]=(uint64_t)r3&M51; r4+=c;
    c=(uint64_t)(r4>>51); o[4]=(uint64_t)r4&M51; o[0]+=c*19;
    c=o[0]>>51; o[0]&=M51; o[1]+=c;
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* o = b ? b_val : a_val, branch-free. `c` must be 0 or 1. */
static void fe_cmov(fe o, const fe a, uint64_t c)
{
    uint64_t m = (uint64_t)0 - c;
    for (int i=0;i<5;i++) o[i] ^= m & (o[i] ^ a[i]);
}

static void fe_frombytes(fe o, const uint8_t *s)
{
    uint64_t t0 =  (uint64_t)s[0] | (uint64_t)s[1]<<8 | (uint64_t)s[2]<<16 | (uint64_t)s[3]<<24 | (uint64_t)s[4]<<32 | (uint64_t)s[5]<<40 | ((uint64_t)s[6]&7)<<48;
    uint64_t t1 = ((uint64_t)s[6]>>3) | (uint64_t)s[7]<<5 | (uint64_t)s[8]<<13 | (uint64_t)s[9]<<21 | (uint64_t)s[10]<<29 | (uint64_t)s[11]<<37 | ((uint64_t)s[12]&63)<<45;
    uint64_t t2 = ((uint64_t)s[12]>>6) | (uint64_t)s[13]<<2 | (uint64_t)s[14]<<10 | (uint64_t)s[15]<<18 | (uint64_t)s[16]<<26 | (uint64_t)s[17]<<34 | (uint64_t)s[18]<<42 | ((uint64_t)s[19]&1)<<50;
    uint64_t t3 = ((uint64_t)s[19]>>1) | (uint64_t)s[20]<<7 | (uint64_t)s[21]<<15 | (uint64_t)s[22]<<23 | (uint64_t)s[23]<<31 | (uint64_t)s[24]<<39 | ((uint64_t)s[25]&15)<<47;
    uint64_t t4 = ((uint64_t)s[25]>>4) | (uint64_t)s[26]<<4 | (uint64_t)s[27]<<12 | (uint64_t)s[28]<<20 | (uint64_t)s[29]<<28 | (uint64_t)s[30]<<36 | ((uint64_t)s[31]&127)<<44;
    o[0]=t0; o[1]=t1; o[2]=t2; o[3]=t3; o[4]=t4;
}

static void fe_tobytes(uint8_t *s, const fe h)
{
    fe t; fe_copy(t, h);
    uint64_t c;
    for (int round = 0; round < 3; round++) {
        c=t[0]>>51; t[0]&=M51; t[1]+=c;
        c=t[1]>>51; t[1]&=M51; t[2]+=c;
        c=t[2]>>51; t[2]&=M51; t[3]+=c;
        c=t[3]>>51; t[3]&=M51; t[4]+=c;
        c=t[4]>>51; t[4]&=M51; t[0]+=c*19;
    }
    uint64_t q = (t[0]+19)>>51;
    q = (t[1]+q)>>51; q=(t[2]+q)>>51; q=(t[3]+q)>>51; q=(t[4]+q)>>51;
    t[0]+=19*q;
    c=t[0]>>51; t[0]&=M51; t[1]+=c;
    c=t[1]>>51; t[1]&=M51; t[2]+=c;
    c=t[2]>>51; t[2]&=M51; t[3]+=c;
    c=t[3]>>51; t[3]&=M51; t[4]+=c;
    t[4]&=M51;
    uint64_t o0=t[0]|(t[1]<<51), o1=(t[1]>>13)|(t[2]<<38), o2=(t[2]>>26)|(t[3]<<25), o3=(t[3]>>39)|(t[4]<<12);
    for (int i=0;i<8;i++) s[i]   =(uint8_t)(o0>>(8*i));
    for (int i=0;i<8;i++) s[8+i] =(uint8_t)(o1>>(8*i));
    for (int i=0;i<8;i++) s[16+i]=(uint8_t)(o2>>(8*i));
    for (int i=0;i<8;i++) s[24+i]=(uint8_t)(o3>>(8*i));
}

static void fe_neg(fe o, const fe a) { fe z; fe_0(z); fe_sub(o, z, a); }

static int fe_isnonzero(const fe a)
{
    uint8_t s[32]; fe_tobytes(s, a);
    uint8_t r = 0; for (int i=0;i<32;i++) r |= s[i];
    return r != 0;
}

static int fe_isnegative(const fe a) { uint8_t s[32]; fe_tobytes(s, a); return s[0] & 1; }

/* o = z^(2^252 - 3), the exponent that both the inverse (via one more square
 * and multiply) and the sqrt candidate are built from. Fixed addition chain. */
static void fe_pow22523(fe o, const fe z)
{
    fe t0,t1,t2; int i;
    fe_sq(t0,z);
    fe_sq(t1,t0); fe_sq(t1,t1); fe_mul(t1,z,t1);
    fe_mul(t0,t0,t1);
    fe_sq(t0,t0); fe_mul(t0,t1,t0);
    fe_sq(t1,t0); for(i=1;i<5;i++) fe_sq(t1,t1); fe_mul(t0,t1,t0);
    fe_sq(t1,t0); for(i=1;i<10;i++) fe_sq(t1,t1); fe_mul(t1,t1,t0);
    fe_sq(t2,t1); for(i=1;i<20;i++) fe_sq(t2,t2); fe_mul(t1,t2,t1);
    fe_sq(t1,t1); for(i=1;i<10;i++) fe_sq(t1,t1); fe_mul(t0,t1,t0);
    fe_sq(t1,t0); for(i=1;i<50;i++) fe_sq(t1,t1); fe_mul(t1,t1,t0);
    fe_sq(t2,t1); for(i=1;i<100;i++) fe_sq(t2,t2); fe_mul(t1,t2,t1);
    fe_sq(t1,t1); for(i=1;i<50;i++) fe_sq(t1,t1); fe_mul(t0,t1,t0);
    fe_sq(t0,t0); fe_sq(t0,t0); fe_mul(o,t0,z);
}

/* z^(p-2) = z^(2^255-21), from the same chain: three more squarings take
 * 2^252-3 to 2^255-24, and one multiply by z^3 lands on 2^255-21. */
static void fe_invert(fe o, const fe z)
{
    fe t, z2, z3;
    fe_pow22523(t, z);                       /* z^(2^252-3)   */
    fe_sq(t,t); fe_sq(t,t); fe_sq(t,t);      /* z^(2^255-24)  */
    fe_sq(z2, z); fe_mul(z3, z2, z);         /* z^3           */
    fe_mul(o, t, z3);
}

/* -------------------------------------------------------------- constants -- */

/* d = -121665/121666 mod p, and 2*d. sqrtm1 = sqrt(-1) mod p. */
static fe D, D2, SQRTM1;
static fe B_x, B_y;
static int ed_ready;

static void fe_fromhex_le(fe o, const uint8_t le[32]) { fe_frombytes(o, le); }

static void ed_init(void)
{
    if (ed_ready) return;
    /* All four constants as 32-byte little-endian canonical encodings, which is
     * how RFC 8032 states them; no runtime inversion is needed and no value is
     * derived from another, so a typo in one cannot be masked by another. */
    static const uint8_t d_le[32] = {
        0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75,0xab,0xd8,0x41,0x41,0x4d,0x0a,0x70,0x00,
        0x98,0xe8,0x79,0x77,0x79,0x40,0xc7,0x8c,0x73,0xfe,0x6f,0x2b,0xee,0x6c,0x03,0x52 };
    static const uint8_t d2_le[32] = {
        0x59,0xf1,0xb2,0x26,0x94,0x9b,0xd6,0xeb,0x56,0xb1,0x83,0x82,0x9a,0x14,0xe0,0x00,
        0x30,0xd1,0xf3,0xee,0xf2,0x80,0x8e,0x19,0xe7,0xfc,0xdf,0x56,0xdc,0xd9,0x06,0x24 };
    static const uint8_t sqrtm1_le[32] = {
        0xb0,0xa0,0x0e,0x4a,0x27,0x1b,0xee,0xc4,0x78,0xe4,0x2f,0xad,0x06,0x18,0x43,0x2f,
        0xa7,0xd7,0xfb,0x3d,0x99,0x00,0x4d,0x2b,0x0b,0xdf,0xc1,0x4f,0x80,0x24,0x83,0x2b };
    /* Base point B: y = 4/5, x the even root. Both stated outright. */
    static const uint8_t by_le[32] = {
        0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
        0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66 };
    static const uint8_t bx_le[32] = {
        0x1a,0xd5,0x25,0x8f,0x60,0x2d,0x56,0xc9,0xb2,0xa7,0x25,0x95,0x60,0xc7,0x2c,0x69,
        0x5c,0xdc,0xd6,0xfd,0x31,0xe2,0xa4,0xc0,0xfe,0x53,0x6e,0xcd,0xd3,0x36,0x69,0x21 };
    fe_fromhex_le(D, d_le);
    fe_fromhex_le(D2, d2_le);
    fe_fromhex_le(SQRTM1, sqrtm1_le);
    fe_fromhex_le(B_y, by_le);
    fe_fromhex_le(B_x, bx_le);
    ed_ready = 1;
}

/* ---------------------------------------------------------------- group ---- */

/* Extended twisted-Edwards coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, xy = T/Z.
 * The addition below is the complete a = -1 formula (Hisil-Wong-Carter-Dawson
 * 2008, "add-2008-hwcd-3"). It is COMPLETE: it is correct for P == Q and for
 * the identity, which is why there is no separate doubling routine and no
 * exceptional case for the ladder to leak through. It costs 9 field
 * multiplies where a dedicated double costs 4 squarings; that is the price of
 * not having a branch on secret data. */
struct ge { fe X, Y, Z, T; };

static void ge_zero(struct ge *p) { fe_0(p->X); fe_1(p->Y); fe_1(p->Z); fe_0(p->T); }

static void ge_add(struct ge *r, const struct ge *p, const struct ge *q)
{
    fe a,b,c,d,e,f,g,h,t0,t1;
    fe_sub(t0, p->Y, p->X); fe_sub(t1, q->Y, q->X); fe_mul(a, t0, t1);
    fe_add(t0, p->Y, p->X); fe_add(t1, q->Y, q->X); fe_mul(b, t0, t1);
    fe_mul(c, p->T, q->T);  fe_mul(c, c, D2);
    fe_mul(d, p->Z, q->Z);  fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

static void ge_cmov(struct ge *r, const struct ge *a, uint64_t c)
{
    fe_cmov(r->X, a->X, c); fe_cmov(r->Y, a->Y, c);
    fe_cmov(r->Z, a->Z, c); fe_cmov(r->T, a->T, c);
}

/* r = k*p. Fixed 255 iterations, one add and one complete-formula double per
 * bit, selected with fe_cmov: no branch and no table index depends on k. */
static void ge_scalarmult(struct ge *r, const uint8_t k[32], const struct ge *p)
{
    struct ge acc, tmp;
    ge_zero(&acc);
    for (int i = 254; i >= 0; i--) {
        ge_add(&acc, &acc, &acc);            /* double (complete formula) */
        ge_add(&tmp, &acc, p);
        ge_cmov(&acc, &tmp, (uint64_t)((k[i >> 3] >> (i & 7)) & 1));
    }
    *r = acc;
    crypto_wipe(&tmp, sizeof tmp);
}

static void ge_base(struct ge *p)
{
    ed_init();
    fe_copy(p->X, B_x); fe_copy(p->Y, B_y); fe_1(p->Z); fe_mul(p->T, B_x, B_y);
}

static void ge_scalarmult_base(struct ge *r, const uint8_t k[32])
{ struct ge b; ge_base(&b); ge_scalarmult(r, k, &b); }

static void ge_tobytes(uint8_t out[32], const struct ge *p)
{
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(out, y);
    out[31] ^= (uint8_t)(fe_isnegative(x) << 7);
}

/* Decode a point. Returns 0 on success, -1 if the encoding names no point on
 * the curve. Public input, so the branches here are fine. */
static int ge_frombytes(struct ge *p, const uint8_t s[32])
{
    ed_init();
    fe u, v, v3, vxx, check, x, y, y2, one;
    uint8_t t[32];
    for (int i=0;i<32;i++) t[i]=s[i];
    t[31] &= 0x7f;
    fe_frombytes(y, t);
    fe_1(one);
    fe_sq(y2, y);
    fe_sub(u, y2, one);                       /* u = y^2 - 1     */
    fe_mul(v, y2, D); fe_add(v, v, one);      /* v = d*y^2 + 1   */
    /* x = (u/v)^((p+3)/8) = u v^3 (u v^7)^((p-5)/8) */
    fe_sq(v3, v); fe_mul(v3, v3, v);          /* v^3 */
    fe_sq(x, v3); fe_mul(x, x, v); fe_mul(x, x, u);   /* u v^7 */
    fe_pow22523(x, x);                        /* (u v^7)^((p-5)/8) */
    fe_mul(x, x, v3); fe_mul(x, x, u);        /* u v^3 (u v^7)^((p-5)/8) */

    fe_sq(vxx, x); fe_mul(vxx, vxx, v);
    fe_sub(check, vxx, u);                    /* v x^2 - u */
    if (fe_isnonzero(check)) {
        fe_add(check, vxx, u);                /* v x^2 + u */
        if (fe_isnonzero(check)) return -1;
        fe_mul(x, x, SQRTM1);
    }
    if (fe_isnegative(x) != (s[31] >> 7)) {
        if (!fe_isnonzero(x)) return -1;      /* x == 0 with the sign bit set */
        fe_neg(x, x);
    }
    fe_copy(p->X, x); fe_copy(p->Y, y); fe_1(p->Z); fe_mul(p->T, x, y);
    return 0;
}

/* ------------------------------------------------------------ scalars ------ */

/* Scalars are 4 little-endian uint64 limbs; products are 8. Reduction mod
 *   L = 2^252 + 27742317777372353535851937790883648493
 * is a fixed-trip binary reduction: 512 iterations of "shift one bit in, then
 * subtract L if the running remainder is >= L", where the subtract is a masked
 * one. That is slower than ref10's radix-2^21 Barrett chain and it is a page
 * of code instead of four, with one conditional whose condition is a borrow
 * bit rather than a comparison the compiler may turn into a branch. */

static const uint64_t SC_L[4] = {
    0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0x0000000000000000ULL, 0x1000000000000000ULL
};

/* r -= L if r >= L, branch-free. Returns nothing. */
static void sc_cond_sub_l(uint64_t r[4])
{
    uint64_t t[4], borrow = 0;
    for (int i = 0; i < 4; i++) {
        /* SC_L[i] is never UINT64_MAX, so SC_L[i] + borrow cannot wrap and the
         * borrow-out is just the unsigned comparison (a setb, not a branch). */
        uint64_t s = SC_L[i] + borrow;
        t[i] = r[i] - s;
        borrow = (uint64_t)(r[i] < s);
    }
    uint64_t m = (uint64_t)0 - (1 - borrow);      /* all-ones when r >= L */
    for (int i = 0; i < 4; i++) r[i] ^= m & (r[i] ^ t[i]);
}

/* out = x mod L, where x is `nbytes` little-endian bytes (32 or 64). */
static void sc_reduce_bytes(uint8_t out[32], const uint8_t *x, int nbytes)
{
    uint64_t r[4] = {0,0,0,0};
    for (int bit = nbytes * 8 - 1; bit >= 0; bit--) {
        /* r <<= 1 */
        uint64_t carry = 0;
        for (int i = 0; i < 4; i++) { uint64_t nc = r[i] >> 63; r[i] = (r[i] << 1) | carry; carry = nc; }
        /* carry out of the top is impossible: r < L < 2^253 before the shift. */
        r[0] |= (uint64_t)((x[bit >> 3] >> (bit & 7)) & 1);
        sc_cond_sub_l(r);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) out[i*8+j] = (uint8_t)(r[i] >> (8*j));
    crypto_wipe(r, sizeof r);
}

/* out = (a*b + c) mod L. a, b, c are 32-byte little-endian scalars < L. */
static void sc_muladd(uint8_t out[32], const uint8_t a[32], const uint8_t b[32], const uint8_t c[32])
{
    uint64_t A[4]={0}, B[4]={0}, P[8]={0};
    for (int i=0;i<4;i++) for (int j=0;j<8;j++) {
        A[i] |= (uint64_t)a[i*8+j] << (8*j);
        B[i] |= (uint64_t)b[i*8+j] << (8*j);
    }
    for (int i = 0; i < 4; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            __uint128_t t = (__uint128_t)A[i]*B[j] + P[i+j] + carry;
            P[i+j] = (uint64_t)t;
            carry  = t >> 64;
        }
        int k = i + 4;
        while (carry) { __uint128_t t = (__uint128_t)P[k] + carry; P[k] = (uint64_t)t; carry = t >> 64; k++; }
    }
    /* + c, widened to the 512-bit product */
    __uint128_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t ci = 0;
        for (int j = 0; j < 8; j++) ci |= (uint64_t)c[i*8+j] << (8*j);
        __uint128_t t = (__uint128_t)P[i] + ci + carry;
        P[i] = (uint64_t)t; carry = t >> 64;
    }
    for (int i = 4; i < 8 && carry; i++) { __uint128_t t = (__uint128_t)P[i] + carry; P[i]=(uint64_t)t; carry = t>>64; }

    uint8_t buf[64];
    for (int i=0;i<8;i++) for (int j=0;j<8;j++) buf[i*8+j] = (uint8_t)(P[i] >> (8*j));
    sc_reduce_bytes(out, buf, 64);
    crypto_wipe(buf, sizeof buf);
    crypto_wipe(A, sizeof A); crypto_wipe(B, sizeof B); crypto_wipe(P, sizeof P);
}

/* 1 if the 32-byte little-endian scalar is < L. Used on the S half of a
 * signature: RFC 8032 5.1.7 requires the check, and skipping it makes
 * signatures malleable (S and S+L both verify), which is how a system that
 * hashes signatures for identity gets two ids for one signer. */
static int sc_is_canonical(const uint8_t s[32])
{
    for (int i = 31; i >= 0; i--) {
        uint8_t l = (uint8_t)(SC_L[i>>3] >> (8*(i&7)));
        if (s[i] < l) return 1;
        if (s[i] > l) return 0;
    }
    return 0;                                  /* equal to L is not < L */
}

/* ------------------------------------------------------------- interface --- */

void ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32])
{
    uint8_t h[64];
    sha512(seed, 32, h);
    h[0]  &= 248;
    h[31] &= 127;
    h[31] |= 64;
    struct ge A;
    ge_scalarmult_base(&A, h);
    ge_tobytes(pub, &A);
    crypto_wipe(h, sizeof h);
    crypto_wipe(&A, sizeof A);
}

void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t mlen,
                  const uint8_t seed[32], const uint8_t pub[32])
{
    uint8_t h[64], a[32], prefix[32], rbuf[64], r[32], kbuf[64], k[32];
    sha512(seed, 32, h);
    for (int i=0;i<32;i++) a[i]=h[i];
    a[0] &= 248; a[31] &= 127; a[31] |= 64;
    for (int i=0;i<32;i++) prefix[i]=h[32+i];

    /* r = SHA-512(prefix || M) mod L -- the deterministic nonce. */
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, prefix, 32);
    if (mlen) sha512_update(&c, msg, mlen);
    sha512_final(&c, rbuf);
    sc_reduce_bytes(r, rbuf, 64);

    struct ge R;
    ge_scalarmult_base(&R, r);
    ge_tobytes(sig, &R);

    /* k = SHA-512(R || A || M) mod L */
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pub, 32);
    if (mlen) sha512_update(&c, msg, mlen);
    sha512_final(&c, kbuf);
    sc_reduce_bytes(k, kbuf, 64);

    sc_muladd(sig + 32, k, a, r);              /* S = r + k*a mod L */

    crypto_wipe(h, sizeof h); crypto_wipe(a, sizeof a); crypto_wipe(prefix, sizeof prefix);
    crypto_wipe(rbuf, sizeof rbuf); crypto_wipe(r, sizeof r);
    crypto_wipe(kbuf, sizeof kbuf); crypto_wipe(k, sizeof k);
    crypto_wipe(&c, sizeof c); crypto_wipe(&R, sizeof R);
}

int ed25519_keypair(uint8_t pub[32], uint8_t seed[32],
                    void (*randbytes)(uint8_t *, int))
{
    if (!randbytes) return -1;
    randbytes(seed, 32);
    /* An all-zero seed is astronomically unlikely and is also exactly what a
     * dead entropy source produces, so it is refused rather than signed with. */
    uint8_t z = 0; for (int i=0;i<32;i++) z |= seed[i];
    if (!z) return -1;
    ed25519_pubkey(pub, seed);
    return 0;
}

int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t mlen,
                   const uint8_t pub[32])
{
    if (!sc_is_canonical(sig + 32)) return 0;   /* S must be < L */

    struct ge A;
    if (ge_frombytes(&A, pub) != 0) return 0;
    /* -A: negate x, which in extended coordinates is negating X and T. */
    fe_neg(A.X, A.X);
    fe_neg(A.T, A.T);

    uint8_t kbuf[64], k[32];
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pub, 32);
    if (mlen) sha512_update(&c, msg, mlen);
    sha512_final(&c, kbuf);
    sc_reduce_bytes(k, kbuf, 64);

    /* Recompute R' = S*B - k*A and compare its encoding with the transmitted R.
     * This is the "compare encodings" form of RFC 8032 5.1.7, which is strictly
     * stronger than the group-equation form: a point and its cofactor-shifted
     * twin encode differently, so no cofactor clearing is needed and the two
     * variants of ed25519 verification cannot disagree here. */
    struct ge sB, kA, R;
    ge_scalarmult_base(&sB, sig + 32);
    ge_scalarmult(&kA, k, &A);
    ge_add(&R, &sB, &kA);

    uint8_t rc[32];
    ge_tobytes(rc, &R);
    uint8_t diff = 0;
    for (int i=0;i<32;i++) diff |= (uint8_t)(rc[i] ^ sig[i]);
    return diff == 0;
}

/* Exposed for the unit test only: it needs to know whether a 32-byte string is
 * a valid point encoding independently of a signature. */
int ed25519_point_valid(const uint8_t p[32])
{ struct ge g; return ge_frombytes(&g, p) == 0; }

/* Also test-only: the field/scalar layers have no other externally visible
 * behaviour, and a bug in either shows up as "every signature fails", which
 * says nothing about where. */
int ed25519_sc_reduce_selftest(void)
{
    /* L mod L == 0, (L-1) mod L == L-1, 2^512-1 mod L is fixed. */
    uint8_t l[32], out[32];
    for (int i=0;i<32;i++) l[i] = (uint8_t)(SC_L[i>>3] >> (8*(i&7)));
    sc_reduce_bytes(out, l, 32);
    for (int i=0;i<32;i++) if (out[i]) return 1;
    l[0]--;
    sc_reduce_bytes(out, l, 32);
    for (int i=0;i<32;i++) if (out[i] != l[i]) return 2;
    return 0;
}
