#include "crypto.h"

/* SHA-1, and it is here for exactly ONE reason, stated up front because a
 * broken hash arriving in a tree that deliberately had none is otherwise a
 * thing to be alarmed about.
 *
 * RFC 6960 4.1.1 defines an OCSP CertID as
 *     { hashAlgorithm, issuerNameHash, issuerKeyHash, serialNumber }
 * and every real responder on the public internet uses SHA-1 for those two
 * hashes -- it is the mandatory-to-implement default, and Let's Encrypt,
 * DigiCert and Sectigo all emit it. Without SHA-1 a stapled response cannot be
 * MATCHED to the certificate it is about, so the whole feature is unreachable.
 *
 * WHAT IT IS USED FOR, precisely: matching. The CertID is a LOOKUP KEY over a
 * candidate set of exactly one certificate -- the leaf we are already holding.
 * A SHA-1 collision would let an attacker construct a second certificate whose
 * CertID equals the leaf's; it would not let them forge the response, because
 * the response's authenticity comes from an ECDSA or RSA signature over the
 * whole ResponseData, verified against the issuer's key. Collision resistance
 * is not a property this use needs, and second-preimage resistance -- which
 * SHA-1 still has -- is.
 *
 * WHAT IT MUST NEVER BE USED FOR: nothing else. There is no SHA-1 entry in the
 * x509.c signature-algorithm table, no sha1WithRSA, no ecdsa-with-SHA1, and
 * this file is not declared in crypto.h. It is declared in ocsp.c and reachable
 * from nowhere else. If a second caller ever appears, that is the moment to
 * re-read this comment rather than to add a prototype to crypto.h. */

struct sha1_ctx { uint32_t h[5]; uint64_t len; uint8_t buf[64]; int n; };

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i]<<24)|((uint32_t)p[4*i+1]<<16)|((uint32_t)p[4*i+2]<<8)|p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);

    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                     k = 0xca62c1d6; }
        uint32_t t = rol(a,5) + f + e + k + w[i];
        e = d; d = c; c = rol(b,30); b = a; a = t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

void ocsp_sha1(const void *data, size_t len, uint8_t out[20])
{
    struct sha1_ctx c;
    c.h[0]=0x67452301; c.h[1]=0xefcdab89; c.h[2]=0x98badcfe;
    c.h[3]=0x10325476; c.h[4]=0xc3d2e1f0;
    c.len = 0; c.n = 0;

    const uint8_t *p = (const uint8_t *)data;
    size_t n = len;
    c.len = (uint64_t)len;
    while (n >= 64) { sha1_block(c.h, p); p += 64; n -= 64; }

    uint8_t tail[128];
    size_t t = 0;
    for (size_t i = 0; i < n; i++) tail[t++] = p[i];
    tail[t++] = 0x80;
    while ((t % 64) != 56) tail[t++] = 0;
    uint64_t bits = c.len * 8;
    for (int i = 7; i >= 0; i--) tail[t++] = (uint8_t)(bits >> (8*i));
    for (size_t i = 0; i < t; i += 64) sha1_block(c.h, tail + i);

    for (int i = 0; i < 5; i++) {
        out[4*i]   = (uint8_t)(c.h[i] >> 24);
        out[4*i+1] = (uint8_t)(c.h[i] >> 16);
        out[4*i+2] = (uint8_t)(c.h[i] >> 8);
        out[4*i+3] = (uint8_t)c.h[i];
    }
    crypto_wipe(&c, sizeof c);
    crypto_wipe(tail, sizeof tail);
}
