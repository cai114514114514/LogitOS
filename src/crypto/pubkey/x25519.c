#include "crypto.h"

/* Curve25519 field arithmetic mod p = 2^255 - 19, represented as 5 limbs of
 * 51 bits in uint64_t (standard radix-2^51 representation). RFC 7748. */

typedef uint64_t fe[5];

static void fe_copy(fe o, const fe a) { for (int i=0;i<5;i++) o[i]=a[i]; }

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
    /* carry-reduce */
    uint64_t c;
    for (int round = 0; round < 3; round++) {
        c=t[0]>>51; t[0]&=0x7ffffffffffff; t[1]+=c;
        c=t[1]>>51; t[1]&=0x7ffffffffffff; t[2]+=c;
        c=t[2]>>51; t[2]&=0x7ffffffffffff; t[3]+=c;
        c=t[3]>>51; t[3]&=0x7ffffffffffff; t[4]+=c;
        c=t[4]>>51; t[4]&=0x7ffffffffffff; t[0]+=c*19;
    }
    /* final reduction mod p */
    uint64_t q = (t[0]+19)>>51;
    q = (t[1]+q)>>51; q=(t[2]+q)>>51; q=(t[3]+q)>>51; q=(t[4]+q)>>51;
    t[0]+=19*q;
    c=t[0]>>51; t[0]&=0x7ffffffffffff; t[1]+=c;
    c=t[1]>>51; t[1]&=0x7ffffffffffff; t[2]+=c;
    c=t[2]>>51; t[2]&=0x7ffffffffffff; t[3]+=c;
    c=t[3]>>51; t[3]&=0x7ffffffffffff; t[4]+=c;
    t[4]&=0x7ffffffffffff;
    uint64_t o0=t[0]|(t[1]<<51), o1=(t[1]>>13)|(t[2]<<38), o2=(t[2]>>26)|(t[3]<<25), o3=(t[3]>>39)|(t[4]<<12);
    for (int i=0;i<8;i++) s[i]   =(uint8_t)(o0>>(8*i));
    for (int i=0;i<8;i++) s[8+i] =(uint8_t)(o1>>(8*i));
    for (int i=0;i<8;i++) s[16+i]=(uint8_t)(o2>>(8*i));
    for (int i=0;i<8;i++) s[24+i]=(uint8_t)(o3>>(8*i));
}

static void fe_add(fe o, const fe a, const fe b) { for(int i=0;i<5;i++) o[i]=a[i]+b[i]; }
static void fe_sub(fe o, const fe a, const fe b)
{
    /* add 2p to keep positive (note the spaces: 0x...e-b would lex as one token) */
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
    r1=(__uint128_t)a0*b1+(__uint128_t)a1*b0+(__uint128_t)a2*b4_19+(__uint128_t)a3*b3_19+(__uint128_t)a4*b2_19;
    r2=(__uint128_t)a0*b2+(__uint128_t)a1*b1+(__uint128_t)a2*b0+(__uint128_t)a3*b4_19+(__uint128_t)a4*b3_19;
    r3=(__uint128_t)a0*b3+(__uint128_t)a1*b2+(__uint128_t)a2*b1+(__uint128_t)a3*b0+(__uint128_t)a4*b4_19;
    r4=(__uint128_t)a0*b4+(__uint128_t)a1*b3+(__uint128_t)a2*b2+(__uint128_t)a3*b1+(__uint128_t)a4*b0;
    uint64_t c;
    c=(uint64_t)(r0>>51); o[0]=(uint64_t)r0&0x7ffffffffffff; r1+=c;
    c=(uint64_t)(r1>>51); o[1]=(uint64_t)r1&0x7ffffffffffff; r2+=c;
    c=(uint64_t)(r2>>51); o[2]=(uint64_t)r2&0x7ffffffffffff; r3+=c;
    c=(uint64_t)(r3>>51); o[3]=(uint64_t)r3&0x7ffffffffffff; r4+=c;
    c=(uint64_t)(r4>>51); o[4]=(uint64_t)r4&0x7ffffffffffff; o[0]+=c*19;
    c=o[0]>>51; o[0]&=0x7ffffffffffff; o[1]+=c;
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

static void fe_mul121666(fe o, const fe a)
{
    __uint128_t r; uint64_t c;
    __uint128_t t[5];
    for (int i=0;i<5;i++) t[i]=(__uint128_t)a[i]*121666;
    c=(uint64_t)(t[0]>>51); o[0]=(uint64_t)t[0]&0x7ffffffffffff; t[1]+=c;
    c=(uint64_t)(t[1]>>51); o[1]=(uint64_t)t[1]&0x7ffffffffffff; t[2]+=c;
    c=(uint64_t)(t[2]>>51); o[2]=(uint64_t)t[2]&0x7ffffffffffff; t[3]+=c;
    c=(uint64_t)(t[3]>>51); o[3]=(uint64_t)t[3]&0x7ffffffffffff; t[4]+=c;
    c=(uint64_t)(t[4]>>51); o[4]=(uint64_t)t[4]&0x7ffffffffffff; o[0]+=c*19;
    (void)r;
}

static void fe_invert(fe o, const fe z)
{
    fe z2,z9,z11,z2_5_0,z2_10_0,z2_20_0,z2_50_0,z2_100_0,t;
    int i;
    fe_sq(z2,z);
    fe_sq(t,z2); fe_sq(t,t); fe_mul(z9,t,z);
    fe_mul(z11,z9,z2);
    fe_sq(t,z11); fe_mul(z2_5_0,t,z9);
    fe_sq(t,z2_5_0); for(i=1;i<5;i++) fe_sq(t,t); fe_mul(z2_10_0,t,z2_5_0);
    fe_sq(t,z2_10_0); for(i=1;i<10;i++) fe_sq(t,t); fe_mul(z2_20_0,t,z2_10_0);
    fe_sq(t,z2_20_0); for(i=1;i<20;i++) fe_sq(t,t); fe_mul(t,t,z2_20_0);
    for(i=0;i<10;i++) fe_sq(t,t); fe_mul(z2_50_0,t,z2_10_0);
    fe_sq(t,z2_50_0); for(i=1;i<50;i++) fe_sq(t,t); fe_mul(z2_100_0,t,z2_50_0);
    fe_sq(t,z2_100_0); for(i=1;i<100;i++) fe_sq(t,t); fe_mul(t,t,z2_100_0);
    for(i=0;i<50;i++) fe_sq(t,t); fe_mul(t,t,z2_50_0);
    for(i=0;i<5;i++) fe_sq(t,t); fe_mul(o,t,z11);
}

static void cswap(uint64_t swap, fe a, fe b)
{
    uint64_t m = (uint64_t)0 - swap;
    for (int i=0;i<5;i++){ uint64_t t=m&(a[i]^b[i]); a[i]^=t; b[i]^=t; }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t e[32];
    for (int i=0;i<32;i++) e[i]=scalar[i];
    e[0]&=248; e[31]&=127; e[31]|=64;          /* clamp */

    fe x1,x2,z2,x3,z3,tmp0,tmp1;
    fe_frombytes(x1, point);
    fe one={1,0,0,0,0}, zero={0,0,0,0,0};
    fe_copy(x2, one); fe_copy(z2, zero);
    fe_copy(x3, x1);  fe_copy(z3, one);

    uint64_t swap=0;
    for (int t=254;t>=0;t--){
        uint64_t b=(e[t>>3]>>(t&7))&1;
        swap^=b; cswap(swap,x2,x3); cswap(swap,z2,z3); swap=b;
        fe a,aa,bb,bcd,da,cb,e_,c,d;
        fe_add(a,x2,z2); fe_sq(aa,a);
        fe_sub(bb,x2,z2); fe_sq(c,bb);          /* c = B^2 */
        fe_sub(e_,aa,c);                        /* E = AA - BB */
        fe_add(d,x3,z3); fe_sub(da,x3,z3);
        fe_mul(cb,da,a); fe_mul(bcd,d,bb);
        fe_add(tmp0,cb,bcd); fe_sq(x3,tmp0);
        fe_sub(tmp1,cb,bcd); fe_sq(tmp1,tmp1); fe_mul(z3,tmp1,x1);
        fe_mul(x2,aa,c);
        fe_mul121666(tmp0,e_); fe_add(tmp0,tmp0,c); fe_mul(z2,e_,tmp0);
    }
    cswap(swap,x2,x3); cswap(swap,z2,z3);
    fe_invert(z2,z2); fe_mul(x2,x2,z2);
    fe_tobytes(out, x2);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    uint8_t base[32]={9}; for(int i=1;i<32;i++) base[i]=0;
    x25519(out, scalar, base);
}
