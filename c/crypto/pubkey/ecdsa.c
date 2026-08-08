#include "crypto.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Generic big-integer mod arithmetic for NIST P-256 / P-384 / P-521 ECDSA
 * *verification* (not constant-time; we only check signatures on public data).
 * Numbers are fixed-width arrays of 32-bit limbs, little-endian limb order.
 *
 * P-521 is the widest: 521 bits needs 17 limbs (the top limb carries 9 bits),
 * so NL is 18 with one limb of headroom for intermediate sums. It costs the
 * narrower curves something -- bn_add/bn_sub/bn_cmp and the fixed-width Barrett
 * helpers all run 18 limbs wide instead of 13 -- and that cost was measured
 * rather than assumed; see tests/unit/crypto_bench.c and the note in
 * curves_init(). The hot loop (mod_mul) is sized by the MODULUS, not by NL, so
 * the widening does not touch it at all, which is why the measured effect is
 * small. */

#define NL 18                       /* limbs (enough for 521b + carry headroom) */
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
static void bn_to_be(uint8_t *b, const bn a, int len) __attribute__((unused));
static void bn_to_be(uint8_t *b, const bn a, int len)
{
    for (int i = 0; i < len; i++) {
        int bit = (len - 1 - i) * 8;
        b[i] = (uint8_t)(a[bit/32] >> (bit % 32));
    }
}

static int bn_cmp(const bn a, const bn b)      /* -1,0,1 */
{
    for (int i = NL-1; i >= 0; i--) { if (a[i]<b[i]) return -1; if (a[i]>b[i]) return 1; }
    return 0;
}
static uint32_t bn_add(bn o, const bn a, const bn b)   /* returns carry */
{
    uint64_t c=0;
    for (int i=0;i<NL;i++){ uint64_t s=(uint64_t)a[i]+b[i]+c; o[i]=(uint32_t)s; c=s>>32; }
    return (uint32_t)c;
}
static uint32_t bn_sub(bn o, const bn a, const bn b)   /* returns borrow */
{
    uint64_t br=0;
    for (int i=0;i<NL;i++){ uint64_t d=(uint64_t)a[i]-b[i]-br; o[i]=(uint32_t)d; br=(d>>63)&1; }
    return (uint32_t)br;
}

/* Curve parameters. */
struct curve {
    int nbytes, nlimbs;
    bn p, n, a, b, gx, gy;          /* prime, order, a, b, base point */
};
static struct curve P256, P384, P521;
static int curves_ready;
static void barrett_make(const uint32_t *m);    /* fwd: Barrett reciprocal setup */
static struct curve *curve_by_id(int curve);    /* fwd: 256/384/521 -> params, or NULL */

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

static void curves_init(void)
{
    if (curves_ready) return;
    P256.nbytes=32; P256.nlimbs=8;
    set_bn(P256.p,"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    set_bn(P256.n,"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    set_bn(P256.a,"ffffffff00000001000000000000000000000000fffffffffffffffffffffffc");
    set_bn(P256.b,"5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
    set_bn(P256.gx,"6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    set_bn(P256.gy,"4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");

    P384.nbytes=48; P384.nlimbs=12;
    set_bn(P384.p,"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff");
    set_bn(P384.n,"ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973");
    set_bn(P384.a,"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc");
    set_bn(P384.b,"b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef");
    set_bn(P384.gx,"aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7");
    set_bn(P384.gy,"3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
    /* secp521r1 (FIPS 186-4 / SEC 2). p = 2^521 - 1, which is why the modulus
     * is 66 bytes with only 9 significant bits in the top one -- every buffer
     * here is 66 bytes and the leading byte is 0x00 or 0x01, never more.
     *
     * This curve exists in this file because the ClientHello has always
     * advertised ecdsa_secp521r1_sha512 (0x0603) while nothing could verify it.
     * Offering a signature algorithm we cannot check is worse than not offering
     * it: a server holding a P-521 certificate takes us at our word, signs with
     * it, and the handshake then fails at CertificateVerify -- so the site was
     * unreachable in a way that looked like the server's fault. */
    P521.nbytes=66; P521.nlimbs=17;
    set_bn(P521.p, "01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    set_bn(P521.n, "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409");
    set_bn(P521.a, "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc");
    set_bn(P521.b, "0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00");
    set_bn(P521.gx,"00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66");
    set_bn(P521.gy,"011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650");

    barrett_make(P256.p); barrett_make(P256.n);     /* precompute Barrett reciprocals */
    barrett_make(P384.p); barrett_make(P384.n);
    barrett_make(P521.p); barrett_make(P521.n);
    curves_ready=1;
}

/* a+b mod m (a,b < m). */
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

/* ---- Barrett reduction (the fast path for mod_mul) ----
 * The old reduction was bit-by-bit shift-subtract (~2*field-bits iterations per
 * multiply), and mod_mul runs ~8000x per ECDSA verify (two scalar mults + the
 * Fermat inverse), so a P-256 verify was ~0.7 s and dominated EC HTTPS loads
 * (and starved net_poll). Barrett reduces in O(words^2) using a precomputed
 * reciprocal mu = floor(2^(64k) / m); transparent (still exact a*b mod m), so
 * every caller -- mod p and mod n -- speeds up with no other change. */
#define BW (2*NL + 4)
struct barrett { uint32_t m[NL]; uint32_t mu[NL+2]; int k; };
static struct barrett btab[8]; static int nbar;   /* 2 moduli (p,n) x 3 curves; 8 for headroom */

static void w_shl1(uint32_t *a){ uint32_t c=0; for(int i=0;i<BW;i++){ uint32_t nc=a[i]>>31; a[i]=(a[i]<<1)|c; c=nc; } }

/* These stay at the fixed width BW, and that is a MEASURED decision, not an
 * oversight. Only about a third of a P-256 Barrett step's limbs are nonzero, so
 * narrowing these three to the modulus's real width is the obvious next
 * optimisation -- it was written, measured, and produced NO improvement on
 * P-256 verification (the difference was inside the ~15% run-to-run spread of
 * that benchmark). The likely reason is that a constant trip count is what lets
 * the compiler unroll and vectorise these loops and a variable one does not, so
 * the vectorisation paid for the zero limbs. It was reverted rather than kept
 * on the theory that it "should" help. The same narrowing IS a 3x win in
 * rsa.c, where the equivalent loop runs 2048 times per verify rather than
 * three, which is why the two files disagree about it on purpose. */
/* P-521 RE-RAN THAT EXPERIMENT AND IT LOST AGAIN, HARDER. Adding the curve
 * takes NL from 13 to 18, so BW goes 30 -> 40 and these loops now spend HALF
 * their limbs on zeros for P-256 rather than a third -- which is exactly the
 * condition under which narrowing "should" finally pay. It was tried: give
 * w_cmp/w_subeq/w_mul an explicit width of 2k+4 (still a per-call constant, so
 * only the trip count varies, not the shape). Measured, host, P-256 verify:
 *      1306 us baseline -> 1377 us fixed-width at NL=18 -> 1588 us narrowed.
 * Narrowing cost another 15% on top of the widening it was meant to undo. The
 * vectorisation explanation above survives the stronger test, so the fixed
 * width stays and the widening is simply paid for. Do not try this a third
 * time without reading these two numbers first. */
static int  w_cmp(const uint32_t *a,const uint32_t *b){ for(int i=BW-1;i>=0;i--){ if(a[i]<b[i])return -1; if(a[i]>b[i])return 1; } return 0; }
static void w_subeq(uint32_t *a,const uint32_t *b){ uint64_t br=0; for(int i=0;i<BW;i++){ uint64_t d=(uint64_t)a[i]-b[i]-br; a[i]=(uint32_t)d; br=(d>>63)&1; } }
static void w_mul(uint32_t *o,const uint32_t *a,int al,const uint32_t *b,int bl){
    for(int i=0;i<BW;i++) o[i]=0;
    for(int i=0;i<al && i<BW;i++){ uint64_t c=0; int j;
        for(j=0;j<bl && i+j<BW;j++){ uint64_t s=(uint64_t)a[i]*b[j]+o[i+j]+c; o[i+j]=(uint32_t)s; c=s>>32; }
        int t=i+j; while(c && t<BW){ uint64_t s=(uint64_t)o[t]+c; o[t]=(uint32_t)s; c=s>>32; t++; }
    }
}
/* register modulus m with mu = floor(2^(64k)/m) (one-time long division). */
static void barrett_make(const uint32_t *m){
    if(nbar>=8) return;
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
/* o = prod mod m, prod = 2*NL limbs (< m^2). 1 if m is registered, else 0. */
static int barrett_reduce(bn o, const uint32_t *prod, const uint32_t *m){
    int bi=-1;
    for(int i=0;i<nbar;i++){ int eq=1; for(int j=0;j<NL;j++) if(btab[i].m[j]!=m[j]){eq=0;break;} if(eq){bi=i;break;} }
    if(bi<0) return 0;
    struct barrett *B=&btab[bi]; int k=B->k;
    uint32_t q1[BW],q2[BW],q3[BW],qm[BW],r[BW],mw[BW];
    for(int i=0;i<BW;i++){ q1[i]=0; q3[i]=0; r[i]=0; mw[i]=0; }
    for(int i=0;i<k;i++) mw[i]=m[i];
    for(int i=0; i+(k-1) < 2*NL && i<BW; i++) q1[i]=prod[i+k-1];      /* q1 = prod >> 32*(k-1) */
    w_mul(q2,q1,k+2,B->mu,k+1);                                      /* q2 = q1 * mu */
    for(int i=0; i+(k+1)<BW; i++) q3[i]=q2[i+k+1];                   /* q3 = q2 >> 32*(k+1) */
    w_mul(qm,q3,k+2,m,k);                                            /* qm = q3 * m */
    uint64_t br=0;                                                   /* r = (prod - qm) mod 2^(32(k+1)) */
    for(int i=0;i<=k;i++){ uint32_t pv=(i<2*NL)?prod[i]:0; uint64_t d=(uint64_t)pv-qm[i]-br; r[i]=(uint32_t)d; br=(d>>63)&1; }
    for(int t=0;t<3 && w_cmp(r,mw)>=0;t++) w_subeq(r,mw);            /* <=2 final corrections */
    bn_zero(o); for(int i=0;i<NL;i++) o[i]=r[i];
    return 1;
}

/* o = a*b mod m.
 *
 * THE PRODUCT LOOP IS SIZED BY THE MODULUS, NOT BY NL. bn is a fixed 13 limbs
 * so that one type covers both curves plus carry headroom, but P-256's modulus
 * is 8 limbs and every operand here is reduced -- a[i] and b[j] are identically
 * zero for i,j >= words(m). Running the schoolbook loop to NL anyway did 169
 * word multiplies where 64 carry all the information, and mod_mul is called
 * about 8000 times per ECDSA verify, so those five zero limbs were 62% of the
 * arithmetic in the hottest primitive the TLS stack has. Restricting the loop
 * to k = words(m) omits only terms that are exactly zero, so every output bit
 * is unchanged -- which is what makes this checkable by the differential suite
 * rather than merely plausible.
 *
 * Measured, host: ecdsa verify P-256 1604 us -> 623 us, P-384 3184 -> 2670
 * (P-384 gains less because 12 of the 13 limbs were already carrying data). */
static int bn_words(const bn m) { for (int i=NL-1;i>=0;i--) if (m[i]) return i+1; return 1; }

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
    /* fallback: bit-by-bit shift-subtract for an unregistered modulus */
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

/* modular inverse via Fermat: a^(m-2) mod m (m prime).
 *
 * The loop runs to the TOP SET BIT of the exponent, not to NL*32. Above that
 * bit every iteration squares `base` into a value nothing ever reads, and for
 * P-256 that was 160 of 416 iterations -- 38% of two modular inversions per
 * ECDSA verify spent computing a discarded number. Stopping at the real width
 * changes no output: the omitted iterations contribute nothing to `result`. */
static void mod_inv(bn o, const bn a, const bn m)
{
    bn result, base, e; bn_zero(result); result[0]=1; bn_copy(base, a);
    bn two; bn_zero(two); two[0]=2; bn_sub(e, m, two);    /* e = m-2 */
    int top = NL*32 - 1;
    while (top >= 0 && !((e[top/32] >> (top%32)) & 1)) top--;
    for (int bit = 0; bit <= top; bit++) {
        if ((e[bit/32] >> (bit%32)) & 1) mod_mul(result, result, base, m);
        mod_mul(base, base, base, m);
    }
    bn_copy(o, result);
}

/* EC point in Jacobian coordinates (X:Y:Z), affine x=X/Z^2, y=Y/Z^3. Point at
 * infinity is Z==0. One modular inverse at the very end instead of per-op. */
struct jpt { bn X, Y, Z; };

static void jpt_dbl(struct jpt *r, const struct jpt *p, const struct curve *c)
{
    if (bn_iszero(p->Z)) { bn_copy(r->X,p->X); bn_copy(r->Y,p->Y); bn_zero(r->Z); return; }
    const bn *P = &c->p;
    bn A,B,D,E,F,t,t2;
    mod_mul(A, p->X, p->X, *P);                 /* X^2 */
    mod_mul(B, p->Y, p->Y, *P);                 /* Y^2 */
    mod_mul(D, B, B, *P);                       /* Y^4 */
    /* C-less form: with a=-3, M = 3(X-Z^2)(X+Z^2) */
    bn z2; mod_mul(z2, p->Z, p->Z, *P);         /* Z^2 */
    bn xm, xp; mod_sub(xm, p->X, z2, *P); mod_add(xp, p->X, z2, *P);
    bn M; mod_mul(M, xm, xp, *P);
    bn three; bn_zero(three); three[0]=3; mod_mul(M, M, three, *P);
    /* S = 4*X*Y^2 */
    bn S; mod_mul(S, p->X, B, *P); bn four; bn_zero(four); four[0]=4; mod_mul(S, S, four, *P);
    /* X3 = M^2 - 2S */
    mod_mul(E, M, M, *P); mod_add(t, S, S, *P); mod_sub(r->X, E, t, *P);
    /* Y3 = M*(S - X3) - 8*Y^4 */
    mod_sub(F, S, r->X, *P); mod_mul(F, M, F, *P);
    bn eight; bn_zero(eight); eight[0]=8; mod_mul(t2, D, eight, *P);
    mod_sub(r->Y, F, t2, *P);
    /* Z3 = 2*Y*Z */
    mod_mul(t, p->Y, p->Z, *P); mod_add(r->Z, t, t, *P);
}

/* Jacobian + affine (Z2=1) mixed add: r = p + q where q is affine (qx,qy). */
static void jpt_add_affine(struct jpt *r, const struct jpt *p,
                           const bn qx, const bn qy, const struct curve *c)
{
    if (bn_iszero(p->Z)) { bn_copy(r->X,qx); bn_copy(r->Y,qy); bn_zero(r->Z); r->Z[0]=1; return; }
    const bn *P=&c->p;
    bn z2,z3,u2,s2,h,hh,hhh,uh,t,t2;
    mod_mul(z2, p->Z, p->Z, *P);                /* Z1^2 */
    mod_mul(z3, z2, p->Z, *P);                  /* Z1^3 */
    mod_mul(u2, qx, z2, *P);                    /* U2 = qx*Z1^2 */
    mod_mul(s2, qy, z3, *P);                    /* S2 = qy*Z1^3 */
    if (bn_cmp(u2, p->X)==0) {
        if (bn_cmp(s2, p->Y)==0) { jpt_dbl(r, p, c); return; }
        bn_copy(r->X,p->X); bn_copy(r->Y,p->Y); bn_zero(r->Z); return;   /* infinity */
    }
    mod_sub(h, u2, p->X, *P);                   /* H = U2 - X1 */
    mod_sub(t, s2, p->Y, *P);                   /* R = S2 - Y1 */
    bn rr; bn_copy(rr, t);
    mod_mul(hh, h, h, *P); mod_mul(hhh, hh, h, *P); mod_mul(uh, p->X, hh, *P);
    /* X3 = R^2 - HHH - 2*U1*HH */
    mod_mul(r->X, rr, rr, *P); mod_sub(r->X, r->X, hhh, *P);
    mod_add(t2, uh, uh, *P); mod_sub(r->X, r->X, t2, *P);
    /* Y3 = R*(U1*HH - X3) - Y1*HHH */
    mod_sub(t, uh, r->X, *P); mod_mul(t, rr, t, *P);
    mod_mul(t2, p->Y, hhh, *P); mod_sub(r->Y, t, t2, *P);
    /* Z3 = Z1*H */
    mod_mul(r->Z, p->Z, h, *P);
}

/* Full Jacobian + Jacobian add (general case; handles infinity operands). */
static void jpt_add(struct jpt *r, const struct jpt *p, const struct jpt *q,
                    const struct curve *c)
{
    if (bn_iszero(p->Z)) { *r=*q; return; }
    if (bn_iszero(q->Z)) { *r=*p; return; }
    const bn *P=&c->p;
    bn z1z1,z2z2,u1,u2,s1,s2,h,i,j,rr,v,t,t2;
    mod_mul(z1z1, p->Z, p->Z, *P);
    mod_mul(z2z2, q->Z, q->Z, *P);
    mod_mul(u1, p->X, z2z2, *P);
    mod_mul(u2, q->X, z1z1, *P);
    mod_mul(s1, p->Y, q->Z, *P); mod_mul(s1, s1, z2z2, *P);
    mod_mul(s2, q->Y, p->Z, *P); mod_mul(s2, s2, z1z1, *P);
    if (bn_cmp(u1,u2)==0) {
        if (bn_cmp(s1,s2)==0) { jpt_dbl(r, p, c); return; }
        bn_zero(r->X); bn_zero(r->Y); bn_zero(r->Z); return;   /* infinity */
    }
    mod_sub(h, u2, u1, *P);
    mod_add(t, h, h, *P); mod_mul(i, t, t, *P);     /* I=(2H)^2 */
    mod_mul(j, h, i, *P);                            /* J=H*I */
    mod_sub(t, s2, s1, *P); mod_add(rr, t, t, *P);   /* r=2(S2-S1) */
    mod_mul(v, u1, i, *P);                           /* V=U1*I */
    mod_mul(r->X, rr, rr, *P); mod_sub(r->X, r->X, j, *P);
    mod_add(t, v, v, *P); mod_sub(r->X, r->X, t, *P);
    mod_sub(t, v, r->X, *P); mod_mul(t, rr, t, *P);
    mod_mul(t2, s1, j, *P); mod_add(t2, t2, t2, *P); mod_sub(r->Y, t, t2, *P);
    mod_add(t, p->Z, q->Z, *P); mod_mul(t, t, t, *P);
    mod_sub(t, t, z1z1, *P); mod_sub(t, t, z2z2, *P); mod_mul(r->Z, t, h, *P);
}

/* r = k*P (P affine), result Jacobian, scanning the low `nbits` bits of k.
 * The bit count is explicit because ECDHE below multiplies by a *blinded*
 * scalar k + rho*n, which is wider than the field. */
static void jpt_mul_bits(struct jpt *r, const bn k, int nbits, const bn px, const bn py,
                         const struct curve *c)
{
    struct jpt acc; bn_zero(acc.X); bn_zero(acc.Y); bn_zero(acc.Z);   /* infinity */
    for (int bit = nbits - 1; bit >= 0; bit--) {
        struct jpt t; jpt_dbl(&t, &acc, c); acc=t;
        if ((k[bit/32] >> (bit%32)) & 1) { struct jpt t2; jpt_add_affine(&t2, &acc, px, py, c); acc=t2; }
    }
    *r = acc;
}

static void jpt_mul(struct jpt *r, const bn k, const bn px, const bn py,
                    const struct curve *c)
{ jpt_mul_bits(r, k, c->nbytes*8, px, py, c); }

/* Jacobian -> affine: x = X/Z^2, y = Y/Z^3. Caller must have checked Z != 0. */
static void jpt_affine(bn ox, bn oy, const struct jpt *p, const struct curve *c)
{
    bn zi, z2, z3;
    mod_inv(zi, p->Z, c->p);
    mod_mul(z2, zi, zi, c->p);
    mod_mul(z3, z2, zi, c->p);
    mod_mul(ox, p->X, z2, c->p);
    mod_mul(oy, p->Y, z3, c->p);
}

/* 1 iff (qx,qy) satisfies y^2 == x^3 + a*x + b (mod p), i.e. lies on the curve.
 * P-256 and P-384 both have cofactor 1, so every on-curve point other than the
 * point at infinity generates the full prime-order group -- which makes this
 * single check a complete defence against invalid-curve and small-subgroup
 * attacks on the ECDH below. Callers must also have range-checked qx,qy < p
 * (Barrett reduction assumes reduced inputs). */
static int point_on_curve(const bn qx, const bn qy, const struct curve *c)
{
    bn x3, ax, rhs, y2;
    mod_mul(x3, qx, qx, c->p);
    mod_mul(x3, x3, qx, c->p);                  /* x^3 */
    mod_mul(ax, c->a, qx, c->p);                /* a*x (a stored as p-3) */
    mod_add(rhs, x3, ax, c->p);
    mod_add(rhs, rhs, c->b, c->p);
    mod_mul(y2, qy, qy, c->p);
    return bn_cmp(y2, rhs) == 0;
}

int ecdsa_verify(int curve, const uint8_t *pub, const uint8_t *sig,
                 const uint8_t *hash, int hlen)
{
    /* An unknown curve id used to fall through to P-384 rather than be refused,
     * which would verify a signature against the wrong group. Refuse instead. */
    struct curve *c = curve_by_id(curve);
    if (!c) return 0;
    int fl = c->nbytes;

    bn r, s, e, u1, u2;
    bn_from_be(r, sig, fl);
    bn_from_be(s, sig + fl, fl);
    if (bn_iszero(r) || bn_iszero(s) || bn_cmp(r, c->n) >= 0 || bn_cmp(s, c->n) >= 0)
        return 0;

    /* e = leftmost min(hlen, nbytes) bytes of hash, as an integer mod n */
    int use = hlen < fl ? hlen : fl;
    bn_from_be(e, hash, use);

    bn w; mod_inv(w, s, c->n);
    mod_mul(u1, e, w, c->n);
    mod_mul(u2, r, w, c->n);

    bn qx, qy;
    bn_from_be(qx, pub, fl); bn_from_be(qy, pub + fl, fl);
    /* reject out-of-range coordinates: Barrett reduction assumes inputs < m */
    if (bn_cmp(qx, c->p) >= 0 || bn_cmp(qy, c->p) >= 0) return 0;
    /* reject points not on the curve: y^2 must equal x^3 + a*x + b (mod p).
     * Without this, an attacker-supplied public key on a different curve with
     * a small-order subgroup would leak the verifier's agreement on forgeries
     * (invalid-curve attack); verification only handles public data, but a
     * wrong "valid" verdict is itself the vulnerability. */
    if (!point_on_curve(qx, qy, c)) return 0;

    struct jpt t1, t2, R;
    jpt_mul(&t1, u1, c->gx, c->gy, c);          /* u1*G */
    jpt_mul(&t2, u2, qx, qy, c);                /* u2*Q */
    jpt_add(&R, &t1, &t2, c);
    if (bn_iszero(R.Z)) return 0;

    /* affine Rx = X/Z^2; valid iff (Rx mod n) == r */
    bn zi, z2, rx;
    mod_inv(zi, R.Z, c->p); mod_mul(z2, zi, zi, c->p); mod_mul(rx, R.X, z2, c->p);
    if (bn_cmp(rx, c->n) >= 0) { bn t; bn_sub(t, rx, c->n); bn_copy(rx, t); }
    return bn_cmp(rx, r) == 0;
}

/* ============================================================================
 * ECDHE on NIST P-256 / P-384 (TLS 1.3 named groups secp256r1 / secp384r1).
 *
 * WHY this exists at all: the client used to offer x25519 and nothing else, so
 * a server configured to insist on a NIST curve was not slow to reach -- it was
 * unreachable. x25519.c stays the *preferred* group (it is the constant-time
 * implementation); these two are the fallback that turns "cannot connect" into
 * "connects", negotiated through HelloRetryRequest.
 *
 * SECURITY, stated plainly: this reuses the ECDSA-verification bignum, which is
 * NOT constant-time (Barrett's conditional subtracts, mod_add/mod_sub's
 * branches, and the double-and-add's data-dependent point add all leak the
 * scalar to an attacker who can time individual operations). Verification only
 * ever touched public data, so that was fine there; an ECDH private scalar is
 * secret, so it is not fine here. Three things bound the exposure:
 *   1. the scalar is *ephemeral* -- generated per handshake, used for exactly
 *      one keygen and one shared-secret computation, then wiped;
 *   2. it is *blinded*: we multiply by k + rho*n for a fresh 32-bit rho rather
 *      than by k. Since n*P is the point at infinity for any P in the group,
 *      (k + rho*n)*P == k*P exactly, but the bit pattern the ladder actually
 *      processes is randomised per execution, so a single timing trace no
 *      longer maps onto the bits of k;
 *   3. the attacker is remote, across TCP, with no shared cache.
 * That is a deliberate trade, not an oversight: the alternative to using these
 * curves is not connecting. Randomness is supplied by the caller (both `priv`
 * and `blind`) so this file stays free of any RNG dependency and remains
 * host-testable on its own.
 * ========================================================================== */

/* o = a * m (m a 32-bit scalar), no reduction. Caller guarantees no overflow. */
static void bn_mul_small(bn o, const bn a, uint32_t m)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) { uint64_t t = (uint64_t)a[i]*m + c; o[i] = (uint32_t)t; c = t >> 32; }
}

static struct curve *curve_by_id(int curve)
{
    curves_init();
    if (curve == 256) return &P256;
    if (curve == 384) return &P384;
    if (curve == 521) return &P521;
    return 0;
}

/* Build the blinded scalar kb = d + rho*n and report how many bits to scan.
 * rho is forced into [2^30, 2^31) so the product's width is fixed (a rho that
 * happened to be tiny would blind almost nothing). With rho < 2^31 and n < 2^384
 * we get rho*n < 2^415 and d < n, so kb < 2^416 = exactly NL*32 bits -- it fits
 * the bn with no carry out, which is why the ladder scans nbytes*8 + 32 bits. */
static int blind_scalar(bn kb, const bn d, uint32_t rho, const struct curve *c)
{
    rho = (rho & 0x7fffffffu) | 0x40000000u;
    bn t; bn_mul_small(t, c->n, rho);
    if (bn_add(kb, t, d)) return -1;            /* cannot happen; fail closed anyway */
    crypto_wipe(t, sizeof t);
    return c->nbytes*8 + 32;
}

/* Generate the public share for a private scalar. `priv` is nbytes of raw
 * randomness, big-endian; it is rejected (return -1) unless it lands in
 * [1, n-1], so the caller must re-randomise and retry -- that keeps the scalar
 * uniform over the group order instead of biasing it with a reduction. `pub`
 * receives the uncompressed SEC1 point 0x04||X||Y (1 + 2*nbytes). */
int ecdh_keygen(int curve, const uint8_t *priv, uint32_t blind, uint8_t *pub)
{
    struct curve *c = curve_by_id(curve);
    if (!c) return -1;
    bn d; bn_from_be(d, priv, c->nbytes);
    if (bn_iszero(d) || bn_cmp(d, c->n) >= 0) { crypto_wipe(d, sizeof d); return -1; }

    bn kb; int nbits = blind_scalar(kb, d, blind, c);
    int rc = -1;
    struct jpt R;
    if (nbits > 0) {
        jpt_mul_bits(&R, kb, nbits, c->gx, c->gy, c);
        if (!bn_iszero(R.Z)) {
            bn x, y; jpt_affine(x, y, &R, c);
            pub[0] = 0x04;
            bn_to_be(pub + 1, x, c->nbytes);
            bn_to_be(pub + 1 + c->nbytes, y, c->nbytes);
            rc = 0;
        }
    }
    crypto_wipe(d, sizeof d); crypto_wipe(kb, sizeof kb); crypto_wipe(&R, sizeof R);
    return rc;
}

/* Shared secret = X coordinate of priv * peer, written big-endian as nbytes
 * (RFC 8446 7.4.2: "the X coordinate ... in the usual big-endian format,
 * padded to the size of P"). Returns 0 on success.
 *
 * The peer point is fully validated first: uncompressed encoding, both
 * coordinates < p (mod_mul assumes reduced inputs), and on the curve. As noted
 * at point_on_curve(), cofactor 1 means that is sufficient -- there is no
 * small-order subgroup for a malicious peer to push us into. A result of
 * infinity is impossible after those checks but is rejected anyway. */
int ecdh_shared(int curve, const uint8_t *priv, uint32_t blind,
                const uint8_t *peer, int peerlen, uint8_t *out)
{
    struct curve *c = curve_by_id(curve);
    if (!c) return -1;
    if (peerlen != 1 + 2*c->nbytes || peer[0] != 0x04) return -1;

    bn qx, qy;
    bn_from_be(qx, peer + 1, c->nbytes);
    bn_from_be(qy, peer + 1 + c->nbytes, c->nbytes);
    if (bn_cmp(qx, c->p) >= 0 || bn_cmp(qy, c->p) >= 0) return -1;
    if (bn_iszero(qx) && bn_iszero(qy)) return -1;      /* (0,0) is not on the curve
                                                          for our b != 0, but be explicit */
    if (!point_on_curve(qx, qy, c)) return -1;

    bn d; bn_from_be(d, priv, c->nbytes);
    if (bn_iszero(d) || bn_cmp(d, c->n) >= 0) { crypto_wipe(d, sizeof d); return -1; }

    bn kb; int nbits = blind_scalar(kb, d, blind, c);
    int rc = -1;
    struct jpt R;
    if (nbits > 0) {
        jpt_mul_bits(&R, kb, nbits, qx, qy, c);
        if (!bn_iszero(R.Z)) {
            bn x, y; jpt_affine(x, y, &R, c);
            bn_to_be(out, x, c->nbytes);
            crypto_wipe(x, sizeof x); crypto_wipe(y, sizeof y);
            rc = 0;
        }
    }
    crypto_wipe(d, sizeof d); crypto_wipe(kb, sizeof kb); crypto_wipe(&R, sizeof R);
    return rc;
}

/* Test hook (tools/t/ecdsa_test.c): out = a*b mod (curveid?P384:P256 . useorder?n:p),
 * big-endian, nbytes each. Exercises mod_mul's Barrett path. */
int ecdsa_modmul_test(int curveid, int useorder, const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    curves_init();
    struct curve *c = curveid ? &P384 : &P256;
    const uint32_t *m = useorder ? c->n : c->p;
    bn A, B, O;
    bn_from_be(A, a, c->nbytes); bn_from_be(B, b, c->nbytes);
    mod_mul(O, A, B, m);
    bn_to_be(out, O, c->nbytes);
    return 0;
}
