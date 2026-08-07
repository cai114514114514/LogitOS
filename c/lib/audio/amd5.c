/* c/lib/audio/amd5.c -- MD5 per RFC 1321. See amd5.h for why it lives here. */

#include <string.h>
#include "amd5.h"

static const uint32_t K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint8_t R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void block(amd5 *m, const uint8_t *p)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);

    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);        g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);        g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                 g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);              g = (7 * i) & 15; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + rotl(a + f + K[i] + w[g], R[i]);
        a = tmp;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

void amd5_init(amd5 *m)
{
    m->a = 0x67452301u; m->b = 0xefcdab89u;
    m->c = 0x98badcfeu; m->d = 0x10325476u;
    m->len = 0; m->have = 0;
}

void amd5_update(amd5 *m, const uint8_t *p, unsigned long n)
{
    m->len += n;
    if (m->have) {
        unsigned take = 64 - m->have;
        if ((unsigned long)take > n) take = (unsigned)n;
        memcpy(m->buf + m->have, p, take);
        m->have += take; p += take; n -= take;
        if (m->have == 64) { block(m, m->buf); m->have = 0; }
    }
    while (n >= 64) { block(m, p); p += 64; n -= 64; }
    if (n) { memcpy(m->buf, p, n); m->have = (unsigned)n; }
}

void amd5_final(amd5 *m, uint8_t out[16])
{
    uint64_t bits = m->len * 8;
    uint8_t pad[72];
    unsigned padlen = (m->have < 56) ? (56 - m->have) : (120 - m->have);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    amd5_update(m, pad, padlen + 8);

    uint32_t h[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
}
