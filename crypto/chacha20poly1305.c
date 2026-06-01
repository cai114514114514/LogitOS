#include "crypto.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* --- ChaCha20 (RFC 8439 §2.3) --- */
static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
#define QR(a,b,c,d) \
    a+=b; d^=a; d=rotl(d,16); c+=d; b^=c; b=rotl(b,12); \
    a+=b; d^=a; d=rotl(d,8);  c+=d; b^=c; b=rotl(b,7)

static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

static void chacha_block(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0]=0x61707865; s[1]=0x3320646e; s[2]=0x79622d32; s[3]=0x6b206574;
    for (int i = 0; i < 8; i++) s[4+i] = rd32(key + 4*i);
    s[12] = counter;
    s[13] = rd32(nonce); s[14] = rd32(nonce+4); s[15] = rd32(nonce+8);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0],x[4],x[8], x[12]); QR(x[1],x[5],x[9], x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[8], x[13]); QR(x[3],x[4],x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4*i]=(uint8_t)v; out[4*i+1]=(uint8_t)(v>>8); out[4*i+2]=(uint8_t)(v>>16); out[4*i+3]=(uint8_t)(v>>24);
    }
}

static void chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                     const uint8_t *in, int len, uint8_t *out)
{
    uint8_t ks[64];
    for (int off = 0; off < len; off += 64) {
        chacha_block(key, counter++, nonce, ks);
        int n = len - off; if (n > 64) n = 64;
        for (int i = 0; i < n; i++) out[off+i] = in[off+i] ^ ks[i];
    }
}

/* --- Poly1305 (RFC 8439 §2.5), 130-bit accumulator over 5 26-bit limbs --- */
struct poly { uint32_t r[5], h[5], pad[4]; };

static void poly_init(struct poly *st, const uint8_t key[32])
{
    uint32_t t0 = rd32(key), t1 = rd32(key+4), t2 = rd32(key+8), t3 = rd32(key+12);
    st->r[0] = t0 & 0x3ffffff;
    st->r[1] = ((t0>>26)|(t1<<6)) & 0x3ffff03;
    st->r[2] = ((t1>>20)|(t2<<12)) & 0x3ffc0ff;
    st->r[3] = ((t2>>14)|(t3<<18)) & 0x3f03fff;
    st->r[4] = (t3>>8) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = rd32(key+16+4*i);
}

static void poly_blocks(struct poly *st, const uint8_t *m, int bytes, int final)
{
    uint32_t r0=st->r[0],r1=st->r[1],r2=st->r[2],r3=st->r[3],r4=st->r[4];
    uint32_t s1=r1*5, s2=r2*5, s3=r3*5, s4=r4*5;
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    while (bytes >= 16 || (final && bytes > 0)) {
        uint8_t blk[16]; int n = bytes < 16 ? bytes : 16;
        for (int i = 0; i < 16; i++) blk[i] = i < n ? m[i] : 0;
        uint32_t hibit = final ? 0 : (1u << 24);
        if (final && n < 16) { blk[n] = 1; hibit = 0; }
        uint32_t t0=rd32(blk),t1=rd32(blk+4),t2=rd32(blk+8),t3=rd32(blk+12);
        h0 += t0 & 0x3ffffff;
        h1 += ((t0>>26)|(t1<<6)) & 0x3ffffff;
        h2 += ((t1>>20)|(t2<<12)) & 0x3ffffff;
        h3 += ((t2>>14)|(t3<<18)) & 0x3ffffff;
        h4 += (t3>>8) | hibit;
        uint64_t d0=(uint64_t)h0*r0+(uint64_t)h1*s4+(uint64_t)h2*s3+(uint64_t)h3*s2+(uint64_t)h4*s1;
        uint64_t d1=(uint64_t)h0*r1+(uint64_t)h1*r0+(uint64_t)h2*s4+(uint64_t)h3*s3+(uint64_t)h4*s2;
        uint64_t d2=(uint64_t)h0*r2+(uint64_t)h1*r1+(uint64_t)h2*r0+(uint64_t)h3*s4+(uint64_t)h4*s3;
        uint64_t d3=(uint64_t)h0*r3+(uint64_t)h1*r2+(uint64_t)h2*r1+(uint64_t)h3*r0+(uint64_t)h4*s4;
        uint64_t d4=(uint64_t)h0*r4+(uint64_t)h1*r3+(uint64_t)h2*r2+(uint64_t)h3*r1+(uint64_t)h4*r0;
        uint32_t c;
        c=(uint32_t)(d0>>26); h0=(uint32_t)d0&0x3ffffff; d1+=c;
        c=(uint32_t)(d1>>26); h1=(uint32_t)d1&0x3ffffff; d2+=c;
        c=(uint32_t)(d2>>26); h2=(uint32_t)d2&0x3ffffff; d3+=c;
        c=(uint32_t)(d3>>26); h3=(uint32_t)d3&0x3ffffff; d4+=c;
        c=(uint32_t)(d4>>26); h4=(uint32_t)d4&0x3ffffff; h0+=c*5;
        c=h0>>26; h0&=0x3ffffff; h1+=c;
        m += n; bytes -= n;
        if (final) break;
    }
    st->h[0]=h0; st->h[1]=h1; st->h[2]=h2; st->h[3]=h3; st->h[4]=h4;
}

static void poly_finish(struct poly *st, uint8_t mac[16])
{
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    uint32_t c;
    c=h1>>26; h1&=0x3ffffff; h2+=c; c=h2>>26; h2&=0x3ffffff; h3+=c;
    c=h3>>26; h3&=0x3ffffff; h4+=c; c=h4>>26; h4&=0x3ffffff; h0+=c*5;
    c=h0>>26; h0&=0x3ffffff; h1+=c;
    uint32_t g0=h0+5; c=g0>>26; g0&=0x3ffffff;
    uint32_t g1=h1+c; c=g1>>26; g1&=0x3ffffff;
    uint32_t g2=h2+c; c=g2>>26; g2&=0x3ffffff;
    uint32_t g3=h3+c; c=g3>>26; g3&=0x3ffffff;
    uint32_t g4=h4+c-(1u<<26);
    uint32_t mask = (g4 >> 31) - 1;             /* if g4 didn't borrow, use g */
    h0=(h0&~mask)|(g0&mask); h1=(h1&~mask)|(g1&mask); h2=(h2&~mask)|(g2&mask);
    h3=(h3&~mask)|(g3&mask); h4=(h4&~mask)|(g4&mask);
    uint64_t f0=((uint64_t)h0|((uint64_t)h1<<26)) & 0xffffffff;
    uint64_t f1=((uint64_t)(h1>>6)|((uint64_t)h2<<20)) & 0xffffffff;
    uint64_t f2=((uint64_t)(h2>>12)|((uint64_t)h3<<14)) & 0xffffffff;
    uint64_t f3=((uint64_t)(h3>>18)|((uint64_t)h4<<8)) & 0xffffffff;
    uint64_t s;
    s = f0 + st->pad[0]; mac[0]=(uint8_t)s; mac[1]=(uint8_t)(s>>8); mac[2]=(uint8_t)(s>>16); mac[3]=(uint8_t)(s>>24);
    s = f1 + st->pad[1] + (s>>32); mac[4]=(uint8_t)s; mac[5]=(uint8_t)(s>>8); mac[6]=(uint8_t)(s>>16); mac[7]=(uint8_t)(s>>24);
    s = f2 + st->pad[2] + (s>>32); mac[8]=(uint8_t)s; mac[9]=(uint8_t)(s>>8); mac[10]=(uint8_t)(s>>16); mac[11]=(uint8_t)(s>>24);
    s = f3 + st->pad[3] + (s>>32); mac[12]=(uint8_t)s; mac[13]=(uint8_t)(s>>8); mac[14]=(uint8_t)(s>>16); mac[15]=(uint8_t)(s>>24);
}

/* AEAD construction (RFC 8439 §2.8): one-time Poly key from ChaCha block 0;
 * MAC over aad || pad16 || ct || pad16 || len(aad) || len(ct). */
static void aead_mac(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen, const uint8_t *ct, int ctlen,
                     uint8_t tag[16])
{
    uint8_t polykey[64];
    chacha_block(key, 0, nonce, polykey);
    struct poly st; poly_init(&st, polykey);
    uint8_t zero[16]; memset(zero, 0, 16);
    poly_blocks(&st, aad, aadlen & ~15, 0);
    if (aadlen & 15) { uint8_t b[16]; memset(b,0,16); memcpy(b, aad+(aadlen&~15), aadlen&15); poly_blocks(&st,b,16,0); }
    poly_blocks(&st, ct, ctlen & ~15, 0);
    if (ctlen & 15) { uint8_t b[16]; memset(b,0,16); memcpy(b, ct+(ctlen&~15), ctlen&15); poly_blocks(&st,b,16,0); }
    uint8_t lenblk[16];
    for (int i = 0; i < 8; i++) lenblk[i]   = (uint8_t)((uint64_t)aadlen >> (8*i));
    for (int i = 0; i < 8; i++) lenblk[8+i] = (uint8_t)((uint64_t)ctlen  >> (8*i));
    poly_blocks(&st, lenblk, 16, 0);
    poly_finish(&st, tag);
}

void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, int aadlen,
                            const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    chacha20(key, 1, nonce, pt, len, ct);           /* counter starts at 1 */
    aead_mac(key, nonce, aad, aadlen, ct, len, tag);
}

int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, int aadlen,
                           const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{
    uint8_t t[16];
    aead_mac(key, nonce, aad, aadlen, ct, len, t);
    int diff = 0; for (int i = 0; i < 16; i++) diff |= t[i] ^ tag[i];
    if (diff) return -1;
    chacha20(key, 1, nonce, ct, len, pt);
    return 0;
}
