#include "crypto.h"

void *memcpy(void *, const void *, size_t);

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_init(struct sha256 *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->n = 0;
}

static void block(struct sha256 *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
        uint32_t maj = (a&b)^(a&cc)^(b&cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void sha256_update(struct sha256 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        int take = 64 - c->n;
        if ((size_t)take > len) take = (int)len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}

/* Padding + big-endian serialization of the first olen bytes. Split out of
 * sha256_final when SHA-224 arrived: 224 is this exact core with a different
 * IV, truncated to 7 of the 8 words, so the only honest way to add it is to
 * share the finish rather than to write a second one that could drift. */
static void finish(struct sha256 *c, uint8_t *out, int olen)
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t L[8];
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_update(c, L, 8);
    for (int i = 0; i < olen; i++)
        out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void sha256_final(struct sha256 *c, uint8_t out[32])
{ finish(c, out, 32); }

void sha256(const void *data, size_t len, uint8_t out[32])
{
    struct sha256 c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* --- SHA-224 (FIPS 180-4 6.3) --- the SHA-256 compression function with a
 * different IV, truncated to 28 bytes. Unlike the SHA-512/t pair, whose IVs
 * come from the IV-generation function, SHA-224's are simply the second
 * thirty-two bits of the fractional parts of the square roots of the 9th
 * through 16th primes -- published constants, not derived ones. */
void sha224_init(struct sha256 *c)
{
    c->h[0]=0xc1059ed8; c->h[1]=0x367cd507; c->h[2]=0x3070dd17; c->h[3]=0xf70e5939;
    c->h[4]=0xffc00b31; c->h[5]=0x68581511; c->h[6]=0x64f98fa7; c->h[7]=0xbefa4fa4;
    c->len = 0; c->n = 0;
}

void sha224_final(struct sha256 *c, uint8_t out[28])
{ finish(c, out, 28); }                      /* first 7 of 8 words */

void sha224(const void *data, size_t len, uint8_t out[28])
{
    struct sha256 c; sha224_init(&c); sha256_update(&c, data, len); sha224_final(&c, out);
}
