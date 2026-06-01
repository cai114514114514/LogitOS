#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

int memcmp(const void *, const void *, size_t);

/* RSA signature *verification* only (public-key op, e small), enough to check
 * certificate chains. Big integers are fixed-width little-endian uint32 limbs.
 * 130 limbs = 4160 bits: holds a 4096-bit modulus plus the carry/shift headroom
 * needed by the double-and-add modular multiply (which keeps every intermediate
 * strictly below n, so no wide product is ever formed). Not constant-time --
 * we only operate on public data. */

#define RL 130
typedef uint32_t rbn[RL];

static void rb_zero(rbn a)            { for (int i=0;i<RL;i++) a[i]=0; }
static void rb_copy(rbn o, const rbn a){ for (int i=0;i<RL;i++) o[i]=a[i]; }

static int rb_cmp(const rbn a, const rbn b)            /* -1,0,1 */
{ for (int i=RL-1;i>=0;i--){ if(a[i]<b[i])return -1; if(a[i]>b[i])return 1; } return 0; }

static void rb_add(rbn o, const rbn a, const rbn b)
{ uint64_t c=0; for(int i=0;i<RL;i++){ uint64_t s=(uint64_t)a[i]+b[i]+c; o[i]=(uint32_t)s; c=s>>32; } }

static void rb_sub(rbn o, const rbn a, const rbn b)    /* a >= b */
{ uint64_t br=0; for(int i=0;i<RL;i++){ uint64_t d=(uint64_t)a[i]-b[i]-br; o[i]=(uint32_t)d; br=(d>>63)&1; } }

static void rb_shl1(rbn a)                             /* a <<= 1 */
{ uint32_t c=0; for(int i=0;i<RL;i++){ uint32_t nc=a[i]>>31; a[i]=(a[i]<<1)|c; c=nc; } }

static int rb_bit(const rbn a, int i) { return (a[i>>5] >> (i&31)) & 1; }
static int rb_topbit(const rbn a)
{ for (int i=RL*32-1;i>=0;i--) if (rb_bit(a,i)) return i; return -1; }

static void rb_from_be(rbn o, const uint8_t *b, int len)
{
    rb_zero(o);
    for (int i=0;i<len;i++){ int bit=(len-1-i)*8; o[bit/32] |= (uint32_t)b[i] << (bit%32); }
}

/* r = a*b mod n, requiring a < n and b < n (so r stays < n throughout). */
static void rb_modmul(rbn r, const rbn a, const rbn b, const rbn n)
{
    rbn t; rb_zero(t);
    for (int i = rb_topbit(b); i >= 0; i--) {
        rb_shl1(t);                  if (rb_cmp(t,n) >= 0) rb_sub(t,t,n);     /* t = 2t mod n */
        if (rb_bit(b,i)) { rb_add(t,t,a); if (rb_cmp(t,n) >= 0) rb_sub(t,t,n); } /* +a mod n */
    }
    rb_copy(r,t);
}

/* out = base^e mod n. */
static void rb_modexp(rbn out, const rbn base, const rbn e, const rbn n)
{
    rbn b, res, tmp;
    rb_copy(b, base);
    while (rb_cmp(b,n) >= 0) rb_sub(b,b,n);              /* ensure base < n */
    rb_zero(res); res[0] = 1;
    for (int i = rb_topbit(e); i >= 0; i--) {
        rb_modmul(tmp, res, res, n); rb_copy(res, tmp);
        if (rb_bit(e,i)) { rb_modmul(tmp, res, b, n); rb_copy(res, tmp); }
    }
    rb_copy(out, res);
}

/* DigestInfo DER prefixes for RSASSA-PKCS1-v1_5 (RFC 8017 §9.2). */
static const uint8_t DI_SHA256[] =
    {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const uint8_t DI_SHA384[] =
    {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};

/* RSA primitive: out = sig^e mod n, serialized big-endian to nlen bytes (the
 * encoded message EM). Returns 0 on success, -1 if sizes are out of range. */
static int rsa_public(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                      const uint8_t *sig, int siglen, uint8_t *em)
{
    if (nlen < 1 || nlen > RL*4 || siglen > nlen) return -1;
    rbn N, E, S, M;
    rb_from_be(N, n, nlen);
    rb_from_be(E, e, elen);
    rb_from_be(S, sig, siglen);
    if (rb_cmp(S, N) >= 0) return -1;
    rb_modexp(M, S, E, N);
    for (int i=0;i<nlen;i++){ int bit=(nlen-1-i)*8; em[i]=(uint8_t)(M[bit/32] >> (bit%32)); }
    return 0;
}

/* MGF1 (RFC 8017 B.2.1) with SHA-256 or SHA-384 (selected by hlen). */
static void mgf1(const uint8_t *seed, int seedlen, int hlen, uint8_t *mask, int masklen)
{
    uint8_t blk[48]; int done = 0;
    for (uint32_t c = 0; done < masklen; c++) {
        uint8_t in[512 + 4]; int il = 0;
        for (int i=0;i<seedlen;i++) in[il++] = seed[i];
        in[il++]=(uint8_t)(c>>24); in[il++]=(uint8_t)(c>>16); in[il++]=(uint8_t)(c>>8); in[il++]=(uint8_t)c;
        if (hlen == 32) sha256(in, il, blk); else sha384(in, il, blk);
        for (int i=0;i<hlen && done<masklen;i++) mask[done++] = blk[i];
    }
}

/* Verify an RSASSA-PSS signature (TLS 1.3 rsa_pss_rsae_*, salt length = hash
 * length, MGF1 with the same hash). mhash is the message digest (hlen 32/48).
 * Returns 1 if valid, 0 otherwise. */
int rsa_pss_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen, const uint8_t *mhash, int hlen)
{
    if (hlen != 32 && hlen != 48) return 0;
    uint8_t em[RL*4];
    if (rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return 0;
    int emLen = nlen;                                  /* modBits multiple of 8 => emLen == nlen */
    int sLen = hlen;
    if (emLen < hlen + sLen + 2) return 0;
    if (em[emLen-1] != 0xbc) return 0;
    if (em[0] & 0x80) return 0;                        /* top bit (8*emLen-emBits=1) must be 0 */
    const uint8_t *maskedDB = em;
    int dblen = emLen - hlen - 1;
    const uint8_t *H = em + dblen;
    uint8_t dbmask[RL*4]; mgf1(H, hlen, hlen, dbmask, dblen);
    uint8_t db[RL*4];
    for (int i=0;i<dblen;i++) db[i] = maskedDB[i] ^ dbmask[i];
    db[0] &= 0x7f;                                     /* clear the same leftmost bit */
    int i = 0;
    while (i < dblen - sLen - 1 && db[i] == 0) i++;
    if (i != dblen - sLen - 1 || db[i] != 0x01) return 0;
    const uint8_t *salt = db + dblen - sLen;
    /* H' = Hash(8*0x00 || mHash || salt) */
    uint8_t mp[8 + 48 + 48]; int mpl = 0;
    for (int k=0;k<8;k++) mp[mpl++] = 0;
    for (int k=0;k<hlen;k++) mp[mpl++] = mhash[k];
    for (int k=0;k<sLen;k++) mp[mpl++] = salt[k];
    uint8_t hp[48];
    if (hlen == 32) sha256(mp, mpl, hp); else sha384(mp, mpl, hp);
    return memcmp(hp, H, hlen) == 0;
}

/* Verify an RSASSA-PKCS1-v1_5 signature. n,e are big-endian; sig is big-endian
 * (length <= nlen); hash is the message digest (hlen = 32 -> SHA-256, 48 ->
 * SHA-384). Returns 1 if valid, 0 otherwise. */
int rsa_pkcs1_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen, const uint8_t *hash, int hlen)
{
    const uint8_t *di; int dilen;
    if      (hlen == 32) { di = DI_SHA256; dilen = (int)sizeof DI_SHA256; }
    else if (hlen == 48) { di = DI_SHA384; dilen = (int)sizeof DI_SHA384; }
    else return 0;

    uint8_t em[RL*4];
    if (rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return 0;

    /* EM must be 0x00 0x01 PS 0x00 T, PS = >=8 bytes of 0xFF, T = DigestInfo||hash,
     * and T must occupy exactly the remainder (no trailing slack). */
    int tlen = dilen + hlen;
    if (nlen < tlen + 11) return 0;
    if (em[0] != 0x00 || em[1] != 0x01) return 0;
    int i = 2;
    while (i < nlen && em[i] == 0xFF) i++;
    if (i - 2 < 8) return 0;                             /* PS too short */
    if (i >= nlen || em[i] != 0x00) return 0;
    i++;
    if (nlen - i != tlen) return 0;                     /* exact fit */
    if (memcmp(em + i, di, dilen) != 0) return 0;
    if (memcmp(em + i + dilen, hash, hlen) != 0) return 0;
    return 1;
}
