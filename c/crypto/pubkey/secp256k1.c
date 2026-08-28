#include "secp256k1.h"
#include "crypto.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* See secp256k1.h for the WHY of every design choice below (own file, generic
 * Barrett, no low-S enforcement, no RFC 6979, blinding-not-constant-time).
 * This file only records the arithmetic itself and the traps in it.
 *
 * bn width: 10 limbs of 32 bits (320 bits) little-endian. secp256k1's field
 * and order are both 256 bits (8 limbs); the extra 2 limbs exist for exactly
 * one reason -- the blinded scalar kb = scalar + rho*n used by
 * secp256k1_ladder needs room for rho*n where rho < 2^31, i.e. kb <
 * 2^256 + 2^31*2^256 < 2^288, which needs 9 limbs, plus one limb of headroom
 * so bn_add's carry-out is provably always 0 rather than merely usually 0.
 * Every OTHER bn in this file (field/order elements) only ever holds a value
 * < p or < n, i.e. fits in the low 8 limbs -- the top 2 are dead weight for
 * them, paid once per operation via mod_mul's k=bn_words(m) restriction
 * below, the same trade ecdsa.c's own comment measures and accepts for its
 * three-curve table. */
#define NL 10
typedef uint32_t bn[NL];

static void bn_zero(bn a) { for (int i=0;i<NL;i++) a[i]=0; }
static void bn_copy(bn o, const bn a) { for (int i=0;i<NL;i++) o[i]=a[i]; }
static int  bn_iszero(const bn a){ uint32_t x=0; for(int i=0;i<NL;i++) x|=a[i]; return x==0; }

static void bn_from_be(bn o, const uint8_t *b, int len)
{
    bn_zero(o);
    for (int i = 0; i < len; i++) {
        int bit = (len - 1 - i) * 8;
        o[bit/32] |= (uint32_t)b[i] << (bit % 32);
    }
}
static void bn_to_be(uint8_t *b, const bn a, int len)
{
    for (int i = 0; i < len; i++) {
        int bit = (len - 1 - i) * 8;
        b[i] = (uint8_t)(a[bit/32] >> (bit % 32));
    }
}

static int bn_cmp(const bn a, const bn b)
{
    for (int i = NL-1; i >= 0; i--) { if (a[i]<b[i]) return -1; if (a[i]>b[i]) return 1; }
    return 0;
}
static uint32_t bn_add(bn o, const bn a, const bn b)
{
    uint64_t c=0;
    for (int i=0;i<NL;i++){ uint64_t s=(uint64_t)a[i]+b[i]+c; o[i]=(uint32_t)s; c=s>>32; }
    return (uint32_t)c;
}
static uint32_t bn_sub(bn o, const bn a, const bn b)
{
    uint64_t br=0;
    for (int i=0;i<NL;i++){ uint64_t d=(uint64_t)a[i]-b[i]-br; o[i]=(uint32_t)d; br=(d>>63)&1; }
    return (uint32_t)br;
}
static int bn_words(const bn m) { for (int i=NL-1;i>=0;i--) if (m[i]) return i+1; return 1; }

static void set_bn(bn o, const char *hex)      /* hex big-endian */
{
    bn_zero(o);
    int len = 0; while (hex[len]) len++;
    int bytes = len/2;
    for (int i = 0; i < bytes; i++) {
        int hi = hex[2*i], lo = hex[2*i+1];
        #define HX(ch) ((ch)<='9'?(ch)-'0':((ch)|32)-'a'+10)
        uint8_t v = (uint8_t)((HX(hi)<<4)|HX(lo));
        int bit = (bytes-1-i)*8;
        o[bit/32] |= (uint32_t)v << (bit%32);
        #undef HX
    }
}

/* ---- domain parameters, SEC 2 v2.0 section 2.4.1 ---- */
static bn K_P, K_N, K_GX, K_GY;   /* p, n, base point; a=0 and b=7 are handled
                                    * directly wherever they are used rather
                                    * than stored, see point_on_curve/secp256k1_dbl */
static int params_ready;

static void barrett_make(const uint32_t *m);
static void params_init(void)
{
    if (params_ready) return;
    set_bn(K_P,  "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f");
    set_bn(K_N,  "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    set_bn(K_GX, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    set_bn(K_GY, "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
    barrett_make(K_P);
    barrett_make(K_N);
    params_ready = 1;
}

/* a+b mod m / a-b mod m, a,b assumed < m. */
static void mod_add(bn o, const bn a, const bn b, const bn m)
{
    uint32_t c = bn_add(o, a, b);
    if (c || bn_cmp(o, m) >= 0) { bn t; bn_sub(t, o, m); bn_copy(o, t); }
}
static void mod_sub(bn o, const bn a, const bn b, const bn m)
{
    bn t; uint32_t br = bn_sub(t, a, b);
    if (br) { bn t2; bn_add(t2, t, m); bn_copy(o, t2); } else bn_copy(o, t);
}

/* Generic fixed-width Barrett reduction -- the same KIND of engine ecdsa.c
 * runs for the NIST primes, independently re-derived here at NL=10 rather
 * than shared, because secp256k1's p has no relation to any NIST prime's
 * shape and a shared "curve parameter table" is exactly the trap
 * secp256k1.h warns against for the doubling formula; the reduction engine
 * has the same hazard in spirit (a generic Barrett step is agnostic to which
 * modulus it is given, which is precisely why it is SAFE to reuse the
 * *pattern* without sharing code -- unlike the a=-3 doubling shortcut, this
 * arithmetic does not encode any curve-specific identity). */
#define BW (2*NL + 4)
struct barrett { uint32_t m[NL]; uint32_t mu[NL+2]; int k; };
static struct barrett btab[4]; static int nbar;   /* p, n */

static void w_shl1(uint32_t *a){ uint32_t c=0; for(int i=0;i<BW;i++){ uint32_t nc=a[i]>>31; a[i]=(a[i]<<1)|c; c=nc; } }
static int  w_cmp(const uint32_t *a,const uint32_t *b){ for(int i=BW-1;i>=0;i--){ if(a[i]<b[i])return -1; if(a[i]>b[i])return 1; } return 0; }
static void w_subeq(uint32_t *a,const uint32_t *b){ uint64_t br=0; for(int i=0;i<BW;i++){ uint64_t d=(uint64_t)a[i]-b[i]-br; a[i]=(uint32_t)d; br=(d>>63)&1; } }
static void w_mul(uint32_t *o,const uint32_t *a,int al,const uint32_t *b,int bl){
    for(int i=0;i<BW;i++) o[i]=0;
    for(int i=0;i<al && i<BW;i++){ uint64_t c=0; int j;
        for(j=0;j<bl && i+j<BW;j++){ uint64_t s=(uint64_t)a[i]*b[j]+o[i+j]+c; o[i+j]=(uint32_t)s; c=s>>32; }
        int t=i+j; while(c && t<BW){ uint64_t s=(uint64_t)o[t]+c; o[t]=(uint32_t)s; c=s>>32; t++; }
    }
}
static void barrett_make(const uint32_t *m){
    if(nbar>=4) return;
    struct barrett *B=&btab[nbar++];
    int k=NL; while(k>0 && m[k-1]==0) k--; B->k=k;
    for(int i=0;i<NL;i++) B->m[i]=m[i];
    uint32_t rem[BW],q[BW],mw[BW];
    for(int i=0;i<BW;i++){ rem[i]=0; q[i]=0; mw[i]=0; }
    for(int i=0;i<k;i++) mw[i]=m[i];
    for(int bit=64*k; bit>=0; bit--){
        w_shl1(rem); if(bit==64*k) rem[0]|=1u;
        w_shl1(q);
        if(w_cmp(rem,mw)>=0){ w_subeq(rem,mw); q[0]|=1u; }
    }
    for(int i=0;i<NL+2;i++) B->mu[i]=q[i];
}
static int barrett_reduce(bn o, const uint32_t *prod, const uint32_t *m){
    int bi=-1;
    for(int i=0;i<nbar;i++){ int eq=1; for(int j=0;j<NL;j++) if(btab[i].m[j]!=m[j]){eq=0;break;} if(eq){bi=i;break;} }
    if(bi<0) return 0;
    struct barrett *B=&btab[bi]; int k=B->k;
    uint32_t q1[BW],q2[BW],q3[BW],qm[BW],r[BW],mw[BW];
    for(int i=0;i<BW;i++){ q1[i]=0; q3[i]=0; r[i]=0; mw[i]=0; }
    for(int i=0;i<k;i++) mw[i]=m[i];
    for(int i=0; i+(k-1) < 2*NL && i<BW; i++) q1[i]=prod[i+k-1];
    w_mul(q2,q1,k+2,B->mu,k+1);
    for(int i=0; i+(k+1)<BW; i++) q3[i]=q2[i+k+1];
    w_mul(qm,q3,k+2,m,k);
    uint64_t br=0;
    for(int i=0;i<=k;i++){ uint32_t pv=(i<2*NL)?prod[i]:0; uint64_t d=(uint64_t)pv-qm[i]-br; r[i]=(uint32_t)d; br=(d>>63)&1; }
    for(int t=0;t<3 && w_cmp(r,mw)>=0;t++) w_subeq(r,mw);
    bn_zero(o); for(int i=0;i<NL;i++) o[i]=r[i];
    return 1;
}

/* o = a*b mod m. The product loop is sized by words(m), not NL -- see the
 * bn-width comment at the top of this file for why the top 2 limbs of a
 * field/order element are always zero. */
static void mod_mul(bn o, const bn a, const bn b, const bn m)
{
    int k = bn_words(m);
    uint32_t prod[2*NL]; for (int i=0;i<2*NL;i++) prod[i]=0;
    for (int i=0;i<k;i++){
        uint64_t c=0;
        for (int j=0;j<k;j++){
            uint64_t s=(uint64_t)a[i]*b[j]+prod[i+j]+c;
            prod[i+j]=(uint32_t)s; c=s>>32;
        }
        prod[i+k]+=(uint32_t)c;
    }
    if (barrett_reduce(o, prod, m)) return;
    /* fallback: bit-by-bit shift-subtract for an unregistered modulus.
     * Unreachable in normal use (only K_P and K_N are ever registered), kept
     * so a caller that somehow reaches this with a third modulus fails
     * correctly instead of returning garbage. */
    bn r; bn_zero(r);
    int top = (2*NL*32) - 1;
    while (top >= 0 && !((prod[top/32] >> (top%32)) & 1)) top--;
    for (int bit = top; bit >= 0; bit--) {
        uint32_t carry = 0;
        for (int i=0;i<NL;i++){ uint32_t nc=r[i]>>31; r[i]=(r[i]<<1)|carry; carry=nc; }
        r[0] |= (prod[bit/32] >> (bit%32)) & 1;
        if (bn_cmp(r, m) >= 0) { bn t; bn_sub(t, r, m); bn_copy(r, t); }
    }
    bn_copy(o, r);
}

/* modular inverse via Fermat: a^(m-2) mod m (m prime; both K_P and K_N are). */
static void mod_inv(bn o, const bn a, const bn m)
{
    bn result, base, e; bn_zero(result); result[0]=1; bn_copy(base, a);
    bn two; bn_zero(two); two[0]=2; bn_sub(e, m, two);
    int top = NL*32 - 1;
    while (top >= 0 && !((e[top/32] >> (top%32)) & 1)) top--;
    for (int bit = 0; bit <= top; bit++) {
        if ((e[bit/32] >> (bit%32)) & 1) mod_mul(result, result, base, m);
        mod_mul(base, base, base, m);
    }
    bn_copy(o, result);
}

/* EC point in Jacobian coordinates. Point at infinity is Z==0. */
struct jpt { bn X, Y, Z; };

/* Doubling for a=0 curves (Bernstein-Lange "dbl-2009-l", EFD shortw-jacobian):
 *   A = X1^2 ; B = Y1^2 ; C = B^2
 *   D = 2*((X1+B)^2 - A - C)
 *   E = 3*A ; F = E^2
 *   X3 = F - 2*D
 *   Y3 = E*(D-X3) - 8*C
 *   Z3 = 2*Y1*Z1
 * This does NOT reduce to ecdsa.c's jpt_dbl with a=0 plugged in: ecdsa.c's
 * formula computes M = 3*(X-Z^2)*(X+Z^2), which is 3*X^2 - 3*Z^4, i.e. it
 * hard-codes the a=-3 identity a*Z^4 = -3*Z^4. secp256k1's a is 0, not -3,
 * so that M is off by a term of -3*Z^4 - 0 = -3*Z^4 relative to the correct
 * M=3*X^2 for a=0 -- SECP256K1_CTL_NIST_DOUBLE below wires that exact wrong
 * formula in as the negative control. */
static void secp256k1_dbl(struct jpt *r, const struct jpt *p)
{
    const bn *P = &K_P;
    if (bn_iszero(p->Z)) { bn_copy(r->X,p->X); bn_copy(r->Y,p->Y); bn_zero(r->Z); return; }
    bn A,B,C,E,F,t,t2;
    mod_mul(A, p->X, p->X, *P);                  /* X1^2 */
    mod_mul(B, p->Y, p->Y, *P);                  /* Y1^2 */
    mod_mul(C, B, B, *P);                        /* Y1^4 */
#ifdef SECP256K1_CTL_NIST_DOUBLE
    /* NEGATIVE CONTROL (test-secp256k1-negctl): the a=-3 shortcut from
     * ecdsa.c's NIST-curve doubling, deliberately wired onto a curve whose
     * a is 0. See secp256k1.h and the comment above this function. */
    bn z2, xm, xp, three;
    mod_mul(z2, p->Z, p->Z, *P);
    mod_sub(xm, p->X, z2, *P); mod_add(xp, p->X, z2, *P);
    mod_mul(E, xm, xp, *P);
    bn_zero(three); three[0]=3; mod_mul(E, E, three, *P);   /* E := "M" = 3(X-Z^2)(X+Z^2) */
    bn S; mod_mul(S, p->X, B, *P); bn four; bn_zero(four); four[0]=4; mod_mul(S, S, four, *P);
    mod_mul(F, E, E, *P); mod_add(t, S, S, *P); mod_sub(r->X, F, t, *P);
    mod_sub(t2, S, r->X, *P); mod_mul(t2, E, t2, *P);
    bn eight; bn_zero(eight); eight[0]=8; mod_mul(t, C, eight, *P);
    mod_sub(r->Y, t2, t, *P);
    mod_mul(t, p->Y, p->Z, *P); mod_add(r->Z, t, t, *P);
#else
    bn xb, sq, D;
    mod_add(xb, p->X, B, *P);
    mod_mul(sq, xb, xb, *P);                     /* (X1+B)^2 */
    mod_sub(t, sq, A, *P); mod_sub(t, t, C, *P);
    mod_add(D, t, t, *P);                        /* D = 2*((X1+B)^2-A-C) */
    bn three; bn_zero(three); three[0]=3; mod_mul(E, A, three, *P);   /* E = 3*A */
    mod_mul(F, E, E, *P);                         /* F = E^2 */
    mod_add(t, D, D, *P); mod_sub(r->X, F, t, *P); /* X3 = F - 2*D */
    mod_sub(t, D, r->X, *P); mod_mul(t, E, t, *P);
    bn eight; bn_zero(eight); eight[0]=8; mod_mul(t2, C, eight, *P);
    mod_sub(r->Y, t, t2, *P);                      /* Y3 = E*(D-X3) - 8*C */
    mod_mul(t, p->Y, p->Z, *P); mod_add(r->Z, t, t, *P);   /* Z3 = 2*Y1*Z1 */
#endif
}

/* Point addition does not depend on `a` -- these are the same well-known
 * formulas ecdsa.c uses (add-2007-bl / madd-2007-bl family), independently
 * transcribed rather than shared; see secp256k1.h. */
static void jpt_add_affine(struct jpt *r, const struct jpt *p,
                           const bn qx, const bn qy)
{
    const bn *P=&K_P;
    if (bn_iszero(p->Z)) { bn_copy(r->X,qx); bn_copy(r->Y,qy); bn_zero(r->Z); r->Z[0]=1; return; }
    bn z2,z3,u2,s2,h,hh,hhh,uh,t,t2;
    mod_mul(z2, p->Z, p->Z, *P);
    mod_mul(z3, z2, p->Z, *P);
    mod_mul(u2, qx, z2, *P);
    mod_mul(s2, qy, z3, *P);
    if (bn_cmp(u2, p->X)==0) {
        if (bn_cmp(s2, p->Y)==0) { secp256k1_dbl(r, p); return; }
        bn_copy(r->X,p->X); bn_copy(r->Y,p->Y); bn_zero(r->Z); return;
    }
    mod_sub(h, u2, p->X, *P);
    mod_sub(t, s2, p->Y, *P);
    bn rr; bn_copy(rr, t);
    mod_mul(hh, h, h, *P); mod_mul(hhh, hh, h, *P); mod_mul(uh, p->X, hh, *P);
    mod_mul(r->X, rr, rr, *P); mod_sub(r->X, r->X, hhh, *P);
    mod_add(t2, uh, uh, *P); mod_sub(r->X, r->X, t2, *P);
    mod_sub(t, uh, r->X, *P); mod_mul(t, rr, t, *P);
    mod_mul(t2, p->Y, hhh, *P); mod_sub(r->Y, t, t2, *P);
    mod_mul(r->Z, p->Z, h, *P);
}

static void jpt_add(struct jpt *r, const struct jpt *p, const struct jpt *q)
{
    const bn *P=&K_P;
    if (bn_iszero(p->Z)) { *r=*q; return; }
    if (bn_iszero(q->Z)) { *r=*p; return; }
    bn z1z1,z2z2,u1,u2,s1,s2,h,i,j,rr,v,t,t2;
    mod_mul(z1z1, p->Z, p->Z, *P);
    mod_mul(z2z2, q->Z, q->Z, *P);
    mod_mul(u1, p->X, z2z2, *P);
    mod_mul(u2, q->X, z1z1, *P);
    mod_mul(s1, p->Y, q->Z, *P); mod_mul(s1, s1, z2z2, *P);
    mod_mul(s2, q->Y, p->Z, *P); mod_mul(s2, s2, z1z1, *P);
    if (bn_cmp(u1,u2)==0) {
        if (bn_cmp(s1,s2)==0) { secp256k1_dbl(r, p); return; }
        bn_zero(r->X); bn_zero(r->Y); bn_zero(r->Z); return;
    }
    mod_sub(h, u2, u1, *P);
    mod_add(t, h, h, *P); mod_mul(i, t, t, *P);
    mod_mul(j, h, i, *P);
    mod_sub(t, s2, s1, *P); mod_add(rr, t, t, *P);
    mod_mul(v, u1, i, *P);
    mod_mul(r->X, rr, rr, *P); mod_sub(r->X, r->X, j, *P);
    mod_add(t, v, v, *P); mod_sub(r->X, r->X, t, *P);
    mod_sub(t, v, r->X, *P); mod_mul(t, rr, t, *P);
    mod_mul(t2, s1, j, *P); mod_add(t2, t2, t2, *P); mod_sub(r->Y, t, t2, *P);
    mod_add(t, p->Z, q->Z, *P); mod_mul(t, t, t, *P);
    mod_sub(t, t, z1z1, *P); mod_sub(t, t, z2z2, *P); mod_mul(r->Z, t, h, *P);
}

/* r = k*P (P affine), scanning the low `nbits` bits of k, MSB first. Not
 * constant time -- see secp256k1.h. */
static void jpt_mul_bits(struct jpt *r, const bn k, int nbits, const bn px, const bn py)
{
    struct jpt acc; bn_zero(acc.X); bn_zero(acc.Y); bn_zero(acc.Z);
    for (int bit = nbits - 1; bit >= 0; bit--) {
        struct jpt t; secp256k1_dbl(&t, &acc); acc=t;
        if ((k[bit/32] >> (bit%32)) & 1) { struct jpt t2; jpt_add_affine(&t2, &acc, px, py); acc=t2; }
    }
    *r = acc;
}
static void jpt_mul(struct jpt *r, const bn k, const bn px, const bn py)
{ jpt_mul_bits(r, k, 256, px, py); }

static void jpt_affine(bn ox, bn oy, const struct jpt *p)
{
    bn zi, z2, z3;
    mod_inv(zi, p->Z, K_P);
    mod_mul(z2, zi, zi, K_P);
    mod_mul(z3, z2, zi, K_P);
    mod_mul(ox, p->X, z2, K_P);
    mod_mul(oy, p->Y, z3, K_P);
}

/* y^2 == x^3 + 7 (mod p). a=0 drops the a*x term entirely -- there is no
 * "a*x" call to make here, which is itself part of the a=0 trap: it is easy
 * to paste in a generic y^2=x^3+a*x+b check with a wired to something
 * nonzero without noticing the curve never asked for that term. Callers
 * must have range-checked qx,qy < p first (Barrett assumes reduced inputs). */
static int point_on_curve(const bn qx, const bn qy)
{
    bn x3, rhs, y2, seven;
    mod_mul(x3, qx, qx, K_P);
    mod_mul(x3, x3, qx, K_P);
    bn_zero(seven); seven[0] = 7;
    mod_add(rhs, x3, seven, K_P);
    mod_mul(y2, qy, qy, K_P);
    return bn_cmp(y2, rhs) == 0;
}

/* Build kb = scalar + rho*n, forcing rho into [2^30,2^31) so the product's
 * width is fixed (see the bn-width comment at the top of this file for why
 * NL=10 is exactly enough). Returns the bit count to scan, or -1 on the
 * (unreachable in practice) carry-out. */
static void bn_mul_small(bn o, const bn a, uint32_t m)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) { uint64_t t = (uint64_t)a[i]*m + c; o[i] = (uint32_t)t; c = t >> 32; }
}
static int blind_scalar(bn kb, const bn scalar, uint32_t rho)
{
    rho = (rho & 0x7fffffffu) | 0x40000000u;
    bn t; bn_mul_small(t, K_N, rho);
    if (bn_add(kb, t, scalar)) return -1;
    crypto_wipe(t, sizeof t);
    return 256 + 32;
}

int secp256k1_verify(const uint8_t *pub, const uint8_t *sig,
                     const uint8_t *hash, int hlen)
{
    params_init();
    if (pub[0] != 0x04) return 0;

    bn r, s, e, u1, u2;
    bn_from_be(r, sig, 32);
    bn_from_be(s, sig + 32, 32);
    if (bn_iszero(r) || bn_iszero(s) || bn_cmp(r, K_N) >= 0 || bn_cmp(s, K_N) >= 0)
        return 0;

    int use = hlen < 32 ? hlen : 32;
    bn_from_be(e, hash, use);
    if (bn_cmp(e, K_N) >= 0) { bn t; bn_sub(t, e, K_N); bn_copy(e, t); }

    bn w; mod_inv(w, s, K_N);
    mod_mul(u1, e, w, K_N);
    mod_mul(u2, r, w, K_N);

    bn qx, qy;
    bn_from_be(qx, pub + 1, 32); bn_from_be(qy, pub + 1 + 32, 32);
    if (bn_cmp(qx, K_P) >= 0 || bn_cmp(qy, K_P) >= 0) return 0;
    /* reject points off the curve -- see point_on_curve() for why this
     * alone defeats invalid-curve attacks: cofactor 1 means every on-curve
     * point other than infinity generates the full prime-order group. */
    if (!point_on_curve(qx, qy)) return 0;

    struct jpt t1, t2, R;
    jpt_mul(&t1, u1, K_GX, K_GY);
    jpt_mul(&t2, u2, qx, qy);
    jpt_add(&R, &t1, &t2);
    if (bn_iszero(R.Z)) return 0;

    bn zi, z2, rx;
    mod_inv(zi, R.Z, K_P); mod_mul(z2, zi, zi, K_P); mod_mul(rx, R.X, z2, K_P);
    if (bn_cmp(rx, K_N) >= 0) { bn t; bn_sub(t, rx, K_N); bn_copy(rx, t); }
    return bn_cmp(rx, r) == 0;
}

/* ---- strict DER parse of SEQUENCE{INTEGER r, INTEGER s} (X.690 sec 8) ---- */

/* Parses one DER length at p[0..], returns bytes consumed (>=1) or -1.
 * Rejects the BER indefinite form (0x80), non-minimal long form (a length
 * that would have fit in the short form, or a long form with a leading zero
 * byte), and anything with more than 4 length-of-length bytes (a signature
 * this tree ever handles is at most a few hundred bytes). */
static int der_len(const uint8_t *p, int avail, long *out)
{
    if (avail < 1) return -1;
    if ((p[0] & 0x80) == 0) { *out = p[0]; return 1; }
    int nb = p[0] & 0x7f;
    if (nb == 0 || nb > 4) return -1;
    if (avail < 1 + nb) return -1;
    long v = 0;
    for (int i = 0; i < nb; i++) v = (v << 8) | p[1 + i];
    if (v < 128) return -1;          /* should have used the short form */
    if (nb > 1 && p[1] == 0) return -1;
    *out = v;
    return 1 + nb;
}

/* Parses one INTEGER at der[*pos], advancing *pos past it. Strips the
 * mandatory sign-guard 0x00 pad when present, rejects a redundant one,
 * rejects a negative value (top bit set with no pad) and an empty content.
 * Writes up to `max` content bytes to `out`/`outn`. Returns 1/0. */
static int der_int(const uint8_t *der, int len, int *pos, uint8_t *out, int max, int *outn)
{
    if (*pos >= len || der[*pos] != 0x02) return 0;
    (*pos)++;
    long ilen;
    int used = der_len(der + *pos, len - *pos, &ilen);
    if (used <= 0) return 0;
    *pos += used;
    if (ilen < 1 || ilen > 1000 || *pos + ilen > len) return 0;
    const uint8_t *c = der + *pos;
    *pos += (int)ilen;
    if (c[0] & 0x80) return 0;                                  /* negative */
    if (ilen > 1 && c[0] == 0x00 && !(c[1] & 0x80)) return 0;    /* non-minimal pad */
    int n = (int)ilen, off = 0;
    if (c[0] == 0x00 && n > 1) { off = 1; n -= 1; }
    if (n > max) return 0;
    for (int i = 0; i < n; i++) out[i] = c[off + i];
    *outn = n;
    return 1;
}

static int der_parse_sig(const uint8_t *der, int len, uint8_t r32[32], uint8_t s32[32])
{
    if (len < 2 || der[0] != 0x30) return 0;
    int pos = 1;
    long seqlen;
    int used = der_len(der + pos, len - pos, &seqlen);
    if (used <= 0) return 0;
    pos += used;
    if (seqlen != len - pos) return 0;         /* the SEQUENCE must be the whole blob */

    uint8_t rbuf[32], sbuf[32]; int rn = 0, sn = 0;
    if (!der_int(der, len, &pos, rbuf, sizeof rbuf, &rn)) return 0;
    if (!der_int(der, len, &pos, sbuf, sizeof sbuf, &sn)) return 0;
    if (pos != len) return 0;                  /* no trailing bytes */

    memset(r32, 0, 32); memset(s32, 0, 32);
    memcpy(r32 + 32 - rn, rbuf, rn);
    memcpy(s32 + 32 - sn, sbuf, sn);
    return 1;
}

int secp256k1_verify_der(const uint8_t *pub, const uint8_t *sig, int siglen,
                         const uint8_t *hash, int hlen)
{
    uint8_t raw[64];
    if (!der_parse_sig(sig, siglen, raw, raw + 32)) return 0;
    return secp256k1_verify(pub, raw, hash, hlen);
}

int secp256k1_keygen(const uint8_t *priv, uint32_t blind, uint8_t *pub)
{
    params_init();
    bn d; bn_from_be(d, priv, 32);
    if (bn_iszero(d) || bn_cmp(d, K_N) >= 0) { crypto_wipe(d, sizeof d); return -1; }

    bn kb; int nbits = blind_scalar(kb, d, blind);
    int rc = -1;
    struct jpt R;
    if (nbits > 0) {
        jpt_mul_bits(&R, kb, nbits, K_GX, K_GY);
        if (!bn_iszero(R.Z)) {
            bn x, y; jpt_affine(x, y, &R);
            pub[0] = 0x04;
            bn_to_be(pub + 1, x, 32);
            bn_to_be(pub + 1 + 32, y, 32);
            rc = 0;
        }
    }
    crypto_wipe(d, sizeof d); crypto_wipe(kb, sizeof kb); crypto_wipe(&R, sizeof R);
    return rc;
}

int secp256k1_sign(const uint8_t *priv, const uint8_t *hash, int hlen,
                   const uint8_t *k, uint32_t blind, uint8_t *sig)
{
    params_init();
    bn d; bn_from_be(d, priv, 32);
    if (bn_iszero(d) || bn_cmp(d, K_N) >= 0) { crypto_wipe(d, sizeof d); return -1; }
    bn kk; bn_from_be(kk, k, 32);
    if (bn_iszero(kk) || bn_cmp(kk, K_N) >= 0) { crypto_wipe(d, sizeof d); crypto_wipe(kk, sizeof kk); return -1; }

    int use = hlen < 32 ? hlen : 32;
    bn e; bn_from_be(e, hash, use);
    if (bn_cmp(e, K_N) >= 0) { bn t; bn_sub(t, e, K_N); bn_copy(e, t); }

    bn kb; int nbits = blind_scalar(kb, kk, blind);
    int rc = -1;
    if (nbits > 0) {
        struct jpt R; jpt_mul_bits(&R, kb, nbits, K_GX, K_GY);
        crypto_wipe(kb, sizeof kb);
        if (!bn_iszero(R.Z)) {
            bn rx, ry; jpt_affine(rx, ry, &R);
            crypto_wipe(&R, sizeof R); crypto_wipe(ry, sizeof ry);
            if (bn_cmp(rx, K_N) >= 0) { bn t; bn_sub(t, rx, K_N); bn_copy(rx, t); }
            if (!bn_iszero(rx)) {
                bn kinv, rd, sum, s;
                mod_inv(kinv, kk, K_N);
                mod_mul(rd, rx, d, K_N);
                mod_add(sum, e, rd, K_N);
                mod_mul(s, kinv, sum, K_N);
                crypto_wipe(kinv, sizeof kinv); crypto_wipe(rd, sizeof rd); crypto_wipe(sum, sizeof sum);
                if (!bn_iszero(s)) {
                    bn_to_be(sig, rx, 32);
                    bn_to_be(sig + 32, s, 32);
                    crypto_wipe(s, sizeof s);
                    rc = 0;
                }
            }
        }
    }
    crypto_wipe(d, sizeof d); crypto_wipe(kk, sizeof kk); crypto_wipe(e, sizeof e); crypto_wipe(kb, sizeof kb);
    return rc;
}
