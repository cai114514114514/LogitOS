#include "crypto.h"
#include <stdint.h>
#include <stddef.h>

int memcmp(const void *, const void *, size_t);

/* RSA signature *verification* only (public-key op, e small), enough to check
 * certificate chains. Big integers are fixed-width little-endian limbs. Not
 * constant-time -- we only operate on public data.
 *
 * THE LIMB IS 64 BITS, AND THAT IS THE WHOLE OPTIMISATION.
 * -------------------------------------------------------
 * Montgomery multiplication is O(s^2) word multiplies for an s-word modulus, so
 * the word size enters the cost QUADRATICALLY: at 32 bits a 2048-bit modulus is
 * 64 words and one mont_mul is 2*64^2 = 8192 multiplies; at 64 bits it is 32
 * words and 2*32^2 = 2048. Four times fewer, and each one is still a single
 * host instruction -- x86-64's MULQ produces the full 128-bit product, which C
 * reaches through __uint128_t, and QEMU/TCG translates it to the host's MULQ
 * rather than synthesising it. So the 4x is real on hardware AND under
 * emulation, which is the machine that matters here.
 *
 * Measured, host, `make test-crypto-bench`, 64-bit limbs plus the Montgomery-
 * form conversion below: RSA-2048 verify 357 us -> 56 us (6.4x), RSA-3072 484
 * -> 126, RSA-4096 631 -> 224. The certificate-chain phase of a real handshake
 * is one to three of these.
 *
 * WIDTH: 68 limbs = 4352 bits. A 4096-bit modulus is 64 limbs, and mont_mul's
 * CIOS accumulator needs s+2 of them, so 66 is the true minimum; 68 leaves the
 * bounds checks room to be stated in bytes without being tight. */

#define RL       68
#define RL_BYTES (RL * 8)          /* the same width said in bytes, for the
                                    * EM/DB buffers */

/* The ACCEPTED input sizes are part of this file's contract, not a consequence
 * of the limb width: the vector suite pins "rsa_modexp_be with nl=517 must be
 * refused". They were RL*4-4 and RL*4 when a limb was 32 bits and there were
 * 130 of them; widening a signature verifier's accepted domain as a side effect
 * of an unrelated speedup is exactly the kind of silent behaviour change this
 * change is not allowed to make, so the two numbers are stated outright. 516
 * bytes is 65 limbs, and mont_mul's s+1-word result still fits RL=68. */
#define RSA_MAX_N   516            /* largest modulus, bytes */
#define RSA_MAX_IN  520            /* largest base / exponent, bytes */

typedef uint64_t rbn[RL];

static void rb_zero(rbn a)            { for (int i=0;i<RL;i++) a[i]=0; }
static void rb_copy(rbn o, const rbn a){ for (int i=0;i<RL;i++) o[i]=a[i]; }

/* The three hot primitives take an explicit limb count.
 *
 * WHY, and it is worth a paragraph: every value in a modexp is bounded by 2n,
 * so only s+1 limbs of an rbn are ever nonzero -- 33 of 68 for a 2048-bit
 * modulus. The conversion of the base into Montgomery form runs one of these
 * per bit of the modulus (2048 iterations), and running each over the full 68
 * limbs did twice the work for no information. Sized to s+1 it is exact and
 * half the cost. Callers that are not on that path keep the full-width forms
 * below. */
static int rb_cmp_n(const rbn a, const rbn b, int L)
{ for (int i=L-1;i>=0;i--){ if(a[i]<b[i])return -1; if(a[i]>b[i])return 1; } return 0; }

static void rb_sub_n(rbn o, const rbn a, const rbn b, int L)   /* a >= b */
{ uint64_t br=0; for(int i=0;i<L;i++){ __uint128_t d=(__uint128_t)a[i]-b[i]-br; o[i]=(uint64_t)d; br=(uint64_t)((d>>127)&1); } }

static void rb_shl1_n(rbn a, int L)                            /* a <<= 1 */
{ uint64_t c=0; for(int i=0;i<L;i++){ uint64_t nc=a[i]>>63; a[i]=(a[i]<<1)|c; c=nc; } }

/* Full width, for the two range checks on caller-supplied values, where the
 * modulus's limb count is not yet known and correctness beats speed. */
static int rb_cmp(const rbn a, const rbn b) { return rb_cmp_n(a, b, RL); }

#ifdef RSA_SLOW_CONTROL
/* THE NEGATIVE CONTROL, and the only reason this code still exists.
 *
 * This is the Montgomery-form conversion as it was before: a full bit-by-bit
 * modular multiply by R, five full-width limb passes per bit of the modulus.
 * `make test-crypto-bench-control` builds with -DRSA_SLOW_CONTROL and REQUIRES
 * the performance gate to fail against it. Without that, test-crypto-bench-gate
 * is an assertion nobody has ever seen fail, which is not evidence of anything.
 * It is compiled out of every other build. */
static int rb_bit(const rbn a, int i);
static int rb_topbit(const rbn a);
static void rb_add(rbn o, const rbn a, const rbn b)
{ uint64_t c=0; for(int i=0;i<RL;i++){ __uint128_t s=(__uint128_t)a[i]+b[i]+c; o[i]=(uint64_t)s; c=(uint64_t)(s>>64); } }
static void rb_sub(rbn o, const rbn a, const rbn b) { rb_sub_n(o, a, b, RL); }
static void rb_shl1(rbn a) { rb_shl1_n(a, RL); }

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
#endif

static int rb_bit(const rbn a, int i) { return (int)((a[i>>6] >> (i&63)) & 1); }
static int rb_topbit(const rbn a)
{ for (int i=RL*64-1;i>=0;i--) if (rb_bit(a,i)) return i; return -1; }

static void rb_from_be(rbn o, const uint8_t *b, int len)
{
    rb_zero(o);
    for (int i=0;i<len;i++){ int bit=(len-1-i)*8; o[bit/64] |= (uint64_t)b[i] << (bit%64); }
}

/* ---- Montgomery multiplication (the fast path for modexp) ----
 * The original modular multiply was bit-by-bit -- O(bits) modular shifts per
 * multiply -- so a single RSA verify (~17 squarings of a 2048-bit modulus) was
 * ~0.7 s on the emulator, a 4-cert chain took ~3 s, and that dominated every
 * HTTPS load and starved net_poll. CIOS Montgomery multiply is O(words^2) word
 * multiplies instead. n must be odd (true for RSA). The word is 64 bits; see
 * the header comment for why that is another 4x. */
static int rb_nwords(const rbn n){ for (int i=RL-1;i>=0;i--) if (n[i]) return i+1; return 1; }

static uint64_t mont_n0inv(uint64_t n0){      /* -n0^{-1} mod 2^64 (n0 odd) */
    uint64_t x = 1;                            /* Newton doubles correct bits each step:
                                                * 1 -> 2 -> 4 -> ... -> 64 needs SIX, one
                                                * more than the 32-bit version did. */
    for (int i = 0; i < 6; i++) x *= 2u - n0 * x;
    return (uint64_t)(0u - x);
}

/* r = a*b*R^{-1} mod n, R = 2^(64s), s = words(n), a,b < n.  (Koc CIOS)
 *
 * Every accumulation below is exactly representable: the worst case is
 * (2^64-1)^2 + (2^64-1) + (2^64-1) = 2^128 - 1, so a __uint128_t product plus a
 * 64-bit addend plus a 64-bit carry never overflows and the carry out is always
 * a full 64-bit word. */
static void mont_mul(rbn r, const rbn a, const rbn b, const rbn n, uint64_t n0inv, int s)
{
    uint64_t t[RL + 2] = { 0 };
    for (int i = 0; i <= s + 1; i++) t[i] = 0;
    for (int i = 0; i < s; i++) {
        uint64_t C = 0;
        for (int j = 0; j < s; j++) { __uint128_t p = (__uint128_t)a[j]*b[i] + t[j] + C; t[j] = (uint64_t)p; C = (uint64_t)(p >> 64); }
        __uint128_t sum = (__uint128_t)t[s] + C; t[s] = (uint64_t)sum; t[s+1] = (uint64_t)(sum >> 64);
        uint64_t m = t[0] * n0inv;
        C = (uint64_t)(((__uint128_t)m*n[0] + t[0]) >> 64);       /* low word becomes 0 */
        for (int j = 1; j < s; j++) { __uint128_t p = (__uint128_t)m*n[j] + t[j] + C; t[j-1] = (uint64_t)p; C = (uint64_t)(p >> 64); }
        sum = (__uint128_t)t[s] + C; t[s-1] = (uint64_t)sum; t[s] = t[s+1] + (uint64_t)(sum >> 64);
    }
    rbn tt; rb_zero(tt); for (int i = 0; i <= s; i++) tt[i] = t[i];   /* result is t[0..s], < 2n */
    if (rb_cmp_n(tt, n, s + 1) >= 0) rb_sub_n(tt, tt, n, s + 1);
    rb_copy(r, tt);
}

/* out = base^e mod n (Montgomery square-and-multiply). */
static void rb_modexp(rbn out, const rbn base, const rbn e, const rbn n)
{
    int s = rb_nwords(n);
    int L = s + 1;                                   /* limbs any value here can occupy */
    uint64_t n0inv = mont_n0inv(n[0]);
    rbn b; rb_copy(b, base); while (rb_cmp_n(b,n,L) >= 0) rb_sub_n(b,b,n,L);   /* base < n */
    /* R = 2^(64s) mod n: start at 2^(64(s-1)) (<= n), double 64 times */
    rbn R; rb_zero(R); R[s-1] = 1; while (rb_cmp_n(R,n,L) >= 0) rb_sub_n(R,R,n,L);
    for (int k = 0; k < 64; k++) { rb_shl1_n(R,L); if (rb_cmp_n(R,n,L) >= 0) rb_sub_n(R,R,n,L); }

    /* base -> Montgomery form, i.e. aR = base * 2^(64s) mod n.
     *
     * This used to be a bit-by-bit modular multiply by R -- shift, compare,
     * subtract, conditionally add, compare again, once per bit of R. Doubling
     * `base` 64s times computes the identical value with two of those five
     * steps instead of five, because the multiplier is a power of two by
     * construction and needs no addend. On a 2048-bit modulus that was the
     * single most expensive thing in an RSA verify -- more than all seventeen
     * Montgomery squarings put together -- which is not something anyone would
     * have guessed from reading the file. */
    rbn aR;
#ifdef RSA_SLOW_CONTROL
    rb_modmul(aR, b, R, n);                          /* the pre-change path */
#else
    rb_copy(aR, b);
    for (int k = 0; k < 64 * s; k++) {
        rb_shl1_n(aR, L);
        if (rb_cmp_n(aR, n, L) >= 0) rb_sub_n(aR, aR, n, L);
    }
#endif
    rbn res; rb_copy(res, R);                        /* 1 -> Montgomery form (= R mod n) */
    rbn tmp;
    for (int i = rb_topbit(e); i >= 0; i--) {
        mont_mul(tmp, res, res, n, n0inv, s); rb_copy(res, tmp);
        if (rb_bit(e,i)) { mont_mul(tmp, res, aR, n, n0inv, s); rb_copy(res, tmp); }
    }
    rbn one; rb_zero(one); one[0] = 1;
    mont_mul(out, res, one, n, n0inv, s);            /* Montgomery form -> normal */
}

/* Test hook (used by tools/t/rsa_test.c): out = base^e mod n, big-endian. */
int rsa_modexp_be(const uint8_t *base, int bl, const uint8_t *e, int el,
                  const uint8_t *n, int nl, uint8_t *out)
{
    rbn B, E, N, M;
    /* nl must leave one limb of headroom: mont_mul copies s+1 result words
     * into an RL-limb rbn (s = rb_nwords(n)); bl/el just bound rb_from_be. */
    if (nl < 1 || nl > RSA_MAX_N || bl < 0 || bl > RSA_MAX_IN || el < 1 || el > RSA_MAX_IN) return -1;
    rb_from_be(B, base, bl); rb_from_be(E, e, el); rb_from_be(N, n, nl);
    if (!(N[0] & 1)) return -2;
    if (rb_cmp(B, N) >= 0) return -3;
    rb_modexp(M, B, E, N);
    for (int i = 0; i < nl; i++) { int bit = (nl-1-i)*8; out[i] = (uint8_t)(M[bit/64] >> (bit%64)); }
    return 0;
}

/* DigestInfo DER prefixes for RSASSA-PKCS1-v1_5 (RFC 8017 §9.2). */
static const uint8_t DI_SHA256[] =
    {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
static const uint8_t DI_SHA384[] =
    {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
static const uint8_t DI_SHA512[] =
    {0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};

/* RSA primitive: out = sig^e mod n, serialized big-endian to nlen bytes (the
 * encoded message EM). Returns 0 on success, -1 if sizes are out of range. */
static int rsa_public(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                      const uint8_t *sig, int siglen, uint8_t *em)
{
    /* elen bounds rb_from_be(E); nlen must leave one limb of headroom (see
     * rsa_modexp_be): otherwise mont_mul's s+1-word result copy overruns rbn. */
    if (nlen < 1 || nlen > RSA_MAX_N || siglen > nlen || elen < 1 || elen > RSA_MAX_IN) return -1;
    rbn N, E, S, M;
    rb_from_be(N, n, nlen);
    if (!(N[0] & 1)) return -1;                      /* Montgomery requires an odd modulus */
    rb_from_be(E, e, elen);
    rb_from_be(S, sig, siglen);
    if (rb_cmp(S, N) >= 0) return -1;
    rb_modexp(M, S, E, N);
    for (int i=0;i<nlen;i++){ int bit=(nlen-1-i)*8; em[i]=(uint8_t)(M[bit/64] >> (bit%64)); }
    return 0;
}

static void hash_sel(int hlen, const uint8_t *in, int il, uint8_t *out)
{
    if (hlen == 32) sha256(in, il, out);
    else if (hlen == 48) sha384(in, il, out);
    else sha512(in, il, out);
}

/* MGF1 (RFC 8017 B.2.1) with SHA-256/384/512 (selected by hlen). */
static void mgf1(const uint8_t *seed, int seedlen, int hlen, uint8_t *mask, int masklen)
{
    uint8_t blk[64]; int done = 0;
    for (uint32_t c = 0; done < masklen; c++) {
        uint8_t in[64 + 4]; int il = 0;
        for (int i=0;i<seedlen;i++) in[il++] = seed[i];
        in[il++]=(uint8_t)(c>>24); in[il++]=(uint8_t)(c>>16); in[il++]=(uint8_t)(c>>8); in[il++]=(uint8_t)c;
        hash_sel(hlen, in, il, blk);
        for (int i=0;i<hlen && done<masklen;i++) mask[done++] = blk[i];
    }
}

/* Verify an RSASSA-PSS signature (TLS 1.3 rsa_pss_rsae_*, salt length = hash
 * length, MGF1 with the same hash). mhash is the message digest (hlen 32/48).
 * Returns 1 if valid, 0 otherwise. */
int rsa_pss_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen, const uint8_t *mhash, int hlen)
{
    if (hlen != 32 && hlen != 48 && hlen != 64) return 0;
    uint8_t em[RL_BYTES];
    if (rsa_public(n, nlen, e, elen, sig, siglen, em) != 0) return 0;
    int emLen = nlen;                                  /* modBits multiple of 8 => emLen == nlen */
    if (emLen < hlen + 2) return 0;
    if (em[emLen-1] != 0xbc) return 0;
    if (em[0] & 0x80) return 0;                        /* top bit (8*emLen-emBits=1) must be 0 */
    const uint8_t *maskedDB = em;
    int dblen = emLen - hlen - 1;
    const uint8_t *H = em + dblen;
    uint8_t dbmask[RL_BYTES]; mgf1(H, hlen, hlen, dbmask, dblen);
    uint8_t db[RL_BYTES];
    for (int i=0;i<dblen;i++) db[i] = maskedDB[i] ^ dbmask[i];
    db[0] &= 0x7f;                                     /* clear the same leftmost bit */
    /* DB = PS(zeros) || 0x01 || salt; recover salt length (TLS uses sLen=hLen,
     * but certificates may use any -- so find the 0x01 separator). */
    int i = 0;
    while (i < dblen && db[i] == 0) i++;
    if (i >= dblen || db[i] != 0x01) return 0;
    i++;
    const uint8_t *salt = db + i; int sLen = dblen - i;
    /* H' = Hash(8*0x00 || mHash || salt) */
    uint8_t mp[8 + 64 + RL_BYTES]; int mpl = 0;
    for (int k=0;k<8;k++) mp[mpl++] = 0;
    for (int k=0;k<hlen;k++) mp[mpl++] = mhash[k];
    for (int k=0;k<sLen;k++) mp[mpl++] = salt[k];
    uint8_t hp[64];
    hash_sel(hlen, mp, mpl, hp);
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
    else if (hlen == 64) { di = DI_SHA512; dilen = (int)sizeof DI_SHA512; }
    else return 0;

    uint8_t em[RL_BYTES];
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
