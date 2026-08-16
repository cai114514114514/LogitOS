#include "crypto.h"
#include "aes_backend.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* AES-GCM: the MODE lives here and is shared by every backend; the primitives
 * it is built from (key schedule, block encrypt/decrypt, GF(2^128) multiply)
 * come from aes_current_backend(). See aes_backend.h for why the split is
 * where it is. The portable implementations below are the reference
 * backend -- they are what runs on a CPU without AES-NI and what the
 * accelerated path is differentially tested against, so they stay. */

/* --- AES-128 (FIPS-197) ---
 * Byte-oriented S-box implementation. NOTE: the sbox is indexed by secret
 * state, so this is NOT constant-time (cache-timing side channel). On a CPU
 * with AES-NI + PCLMULQDQ this code is not the one that runs -- see
 * c/crypto/aead/aes_ni.c -- but it is still the fallback, and the caveat is
 * live wherever that fallback is selected; see TRANSPARENCY.md. */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

/* AES-128, AES-192 and AES-256 share this code, parameterised by key length:
 * 16 bytes -> 10 rounds and a 176-byte schedule, 24 -> 12 rounds and 208, 32 ->
 * 14 rounds and 240. AES-256 is here because TLS 1.2 servers commonly *prefer*
 * ECDHE_*_WITH_AES_256_GCM_SHA384, so without it a 1.2 handshake would
 * routinely land on our second choice or on nothing at all. AES-192 completes
 * the FIPS-197 key-size family; nothing in-tree negotiates it, but the round
 * count and the nk=6 key schedule (no extra SubWord -- that is an nk > 6 rule)
 * are one line each, and a GCM that cannot be asked for 192-bit keys is a
 * library gap, not a policy. Everything below the schedule is unchanged. */
#define AES_RK_MAX 240                          /* 4 * (14 + 1) * 4 */

static int aes_rounds(int keylen)
{
    return keylen == 32 ? 14 : keylen == 24 ? 12 : 10;
}

static void c_key_expand(const uint8_t *key, int keylen, uint8_t *rk)
{
    int nk = keylen / 4;                        /* key words: 4 (AES-128) or 8 */
    int total = 4 * (nk + 6 + 1) * 4;           /* bytes of expanded key */
    memcpy(rk, key, (size_t)keylen);
    uint8_t rcon = 1;
    for (int i = keylen; i < total; i += 4) {
        uint8_t t[4]; memcpy(t, rk + i - 4, 4);
        int w = i / 4;                          /* index of the word being built */
        if (w % nk == 0) {
            uint8_t tmp = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[tmp];
            rcon = xtime(rcon);
        } else if (nk > 6 && w % nk == 4) {
            /* AES-256 only (FIPS-197 5.2): an extra SubWord on every fourth
             * word. Leaving it out still yields a self-consistent cipher --
             * encrypt/decrypt round-trip fine -- so the mistake is invisible
             * until it interoperates with nothing. Hence the NIST vectors. */
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[i+j] = rk[i-keylen+j] ^ t[j];
    }
}

static void c_encrypt(const uint8_t *rk, int nr, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16]; memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= nr; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        uint8_t t[16];                          /* ShiftRows */
        for (int r = 0; r < 4; r++)
            for (int col = 0; col < 4; col++)
                t[col*4+r] = s[((col+r)%4)*4+r];
        memcpy(s, t, 16);
        if (round < nr) {                       /* MixColumns */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = s + col*4;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = xtime(a0)^(xtime(a1)^a1)^a2^a3;
                p[1] = a0^xtime(a1)^(xtime(a2)^a2)^a3;
                p[2] = a0^a1^xtime(a2)^(xtime(a3)^a3);
                p[3] = (xtime(a0)^a0)^a1^a2^xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16+i];
    }
    memcpy(out, s, 16);
}

/* Inverse S-box and xtime^-1 helpers for the inverse cipher (FIPS-197 5.3).
 * Built as a compile-time-constant inverse of `sbox` rather than a second
 * hand-typed 256-byte table, so the two can never disagree; the loop runs
 * once at first use and the result is as read-only as a static table. */
static uint8_t inv_sbox[256];
static int inv_sbox_ready;

static void inv_sbox_init(void)
{
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    inv_sbox_ready = 1;
}

/* multiply by 9, 11, 13, 14 in GF(2^8) for InvMixColumns: each is a fixed
 * sum of doublings, written as expressions so the compiler folds them. */
static uint8_t mul9(uint8_t a)  { return xtime(xtime(xtime(a))) ^ a; }
static uint8_t mul11(uint8_t a) { return xtime(xtime(xtime(a)) ^ a) ^ a; }
static uint8_t mul13(uint8_t a) { return xtime(xtime(xtime(a) ^ a)) ^ a; }
static uint8_t mul14(uint8_t a) { return xtime(xtime(xtime(a) ^ a) ^ a); }

/* The straight inverse cipher over the SAME round-key layout c_encrypt
 * produced (FIPS-197 5.3.4): last round key first, InvShiftRows/InvSubBytes
 * before AddRoundKey, InvMixColumns on every round but the first and last.
 * Like c_encrypt this indexes tables with cipher state, so it is NOT
 * constant-time; the AES-NI backend's AESDEC is. */
static void c_decrypt(const uint8_t *rk, int nr, const uint8_t in[16], uint8_t out[16])
{
    if (!inv_sbox_ready) inv_sbox_init();
    uint8_t s[16]; memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[nr*16+i];
    for (int round = nr - 1; round >= 0; round--) {
        uint8_t t[16];                          /* InvShiftRows */
        for (int r = 0; r < 4; r++)
            for (int col = 0; col < 4; col++)
                t[col*4+r] = s[((col + 4 - r) % 4)*4+r];
        memcpy(s, t, 16);
        for (int i = 0; i < 16; i++) s[i] = inv_sbox[s[i]];
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16+i];
        if (round > 0) {                        /* InvMixColumns */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = s + col*4;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = mul14(a0)^mul11(a1)^mul13(a2)^mul9(a3);
                p[1] = mul9(a0)^mul14(a1)^mul11(a2)^mul13(a3);
                p[2] = mul13(a0)^mul9(a1)^mul14(a2)^mul11(a3);
                p[3] = mul11(a0)^mul13(a1)^mul9(a2)^mul14(a3);
            }
        }
    }
    memcpy(out, s, 16);
}

/* --- GHASH over GF(2^128) ---
 * Bit-serial: branches on the bits of the accumulator, which is derived from
 * the secret H, so this leaks through the branch predictor as well as the
 * cache. The PCLMULQDQ backend does not. */
static void c_gf_mul(uint8_t x[16], const uint8_t y[16])
{
    uint8_t z[16]; memset(z, 0, 16);
    uint8_t v[16]; memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7)))
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j-1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

/* The reference backend. Exported (rather than left as three statics) so the
 * dispatcher can name it, the AES-NI path can be differentially tested against
 * it, and crypto_simd_force_baseline() can put it back. */
static const struct aes_backend c_backend = {
    "c", c_key_expand, c_encrypt, c_decrypt, c_gf_mul, 0
};

const struct aes_backend *aes_backend_c(void) { return &c_backend; }

static void ghash(const struct aes_backend *be, const uint8_t H[16],
                  const uint8_t *aad, int aadlen,
                  const uint8_t *ct, int ctlen, uint8_t out[16])
{
    uint8_t y[16]; memset(y, 0, 16);
    for (int off = 0; off < aadlen; off += 16) {
        int n = aadlen - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) y[i] ^= aad[off+i];
        be->gf_mul(y, H);
    }
    for (int off = 0; off < ctlen; off += 16) {
        int n = ctlen - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) y[i] ^= ct[off+i];
        be->gf_mul(y, H);
    }
    uint8_t L[16]; memset(L, 0, 16);
    uint64_t ab = (uint64_t)aadlen * 8, cb = (uint64_t)ctlen * 8;
    for (int i = 0; i < 8; i++) L[i]   = (uint8_t)(ab >> (56 - 8*i));
    for (int i = 0; i < 8; i++) L[8+i] = (uint8_t)(cb >> (56 - 8*i));
    for (int i = 0; i < 16; i++) y[i] ^= L[i];
    be->gf_mul(y, H);
    memcpy(out, y, 16);
}

static void gctr(const struct aes_backend *be, const uint8_t *rk, int nr,
                 const uint8_t icb[16],
                 const uint8_t *in, int len, uint8_t *out)
{
    uint8_t ctr[16]; memcpy(ctr, icb, 16);
    uint8_t ks[16];
    for (int off = 0; off < len; off += 16) {
        be->encrypt(rk, nr, ctr, ks);
        int n = len - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) out[off+i] = in[off+i] ^ ks[i];
        for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }   /* inc low 32 bits */
    }
    crypto_wipe(ks, sizeof ks);                 /* keystream */
}

/* J0, the pre-counter block (SP 800-38D 5.2.1.1). The 96-bit IV is the fast
 * path every TLS record takes: J0 = IV || 0^31 || 1, no GHASH at all. Any
 * other length pays the general construction: zero-pad the IV to a 128-bit
 * boundary, append 64 zero bits and the 64-bit big-endian bit-length of the
 * IV, and GHASH the whole string under H. Getting the length in BITS, not
 * bytes, is the classic error here; it is pinned by the McGrew-Viega
 * 60-byte-IV test cases (TC6/TC12/TC18), where an 8-vs-64 length field moves
 * every byte of the tag. */
static void gcm_j0(const struct aes_backend *be, const uint8_t H[16],
                   const uint8_t *iv, int ivlen, uint8_t j0[16])
{
    if (ivlen == 12) {
        memcpy(j0, iv, 12); j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
        return;
    }
    uint8_t y[16]; memset(y, 0, 16);
    for (int off = 0; off < ivlen; off += 16) {
        int n = ivlen - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) y[i] ^= iv[off+i];
        be->gf_mul(y, H);
    }
    uint8_t L[16]; memset(L, 0, 16);
    uint64_t bits = (uint64_t)ivlen * 8;
    for (int i = 0; i < 8; i++) L[8+i] = (uint8_t)(bits >> (56 - 8*i));
    for (int i = 0; i < 16; i++) y[i] ^= L[i];
    be->gf_mul(y, H);
    memcpy(j0, y, 16);
    crypto_wipe(y, sizeof y);
}

static void gcm_core(const uint8_t *key, int keylen, const uint8_t *iv, int ivlen,
                     const uint8_t *aad, int aadlen, const uint8_t *in, int len,
                     uint8_t *out, uint8_t tag[16])
{
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(key, keylen, rk);
    uint8_t H[16], zero[16]; memset(zero, 0, 16);
    be->encrypt(rk, nr, zero, H);
    uint8_t j0[16]; gcm_j0(be, H, iv, ivlen, j0);
    uint8_t ctr1[16]; memcpy(ctr1, j0, 16);
    for (int i = 15; i >= 12; i--) { if (++ctr1[i]) break; }   /* inc32(j0): counter starts at 2 */
    gctr(be, rk, nr, ctr1, in, len, out);       /* encrypt/decrypt with counter from 2 */
    uint8_t S[16]; ghash(be, H, aad, aadlen, out, len, S);
    uint8_t ej0[16]; be->encrypt(rk, nr, j0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = S[i] ^ ej0[i];
    crypto_wipe(rk, sizeof rk);              /* expanded key */
    crypto_wipe(H, sizeof H); crypto_wipe(ej0, sizeof ej0);
}

static int gcm_open(const uint8_t *key, int keylen, const uint8_t *iv, int ivlen,
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{
    if (aadlen < 0 || len < 0 || ivlen < 1 || ivlen > 1024) return -1;
    /* GHASH is over the ciphertext, so compute the tag from ct, then decrypt.
     * Decryption happens only after the tags compare equal -- handing back
     * unauthenticated plaintext is the classic GCM footgun. */
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(key, keylen, rk);
    uint8_t H[16], zero[16]; memset(zero, 0, 16);
    be->encrypt(rk, nr, zero, H);
    uint8_t j0[16]; gcm_j0(be, H, iv, ivlen, j0);
    uint8_t S[16]; ghash(be, H, aad, aadlen, ct, len, S);
    uint8_t ej0[16]; be->encrypt(rk, nr, j0, ej0);
    uint8_t t[16]; for (int i = 0; i < 16; i++) t[i] = S[i] ^ ej0[i];
    int diff = 0; for (int i = 0; i < 16; i++) diff |= t[i] ^ tag[i];
    if (diff) {
        crypto_wipe(rk, sizeof rk);
        crypto_wipe(H, sizeof H); crypto_wipe(ej0, sizeof ej0);
        return -1;
    }
    uint8_t ctr1[16]; memcpy(ctr1, j0, 16);
    for (int i = 15; i >= 12; i--) { if (++ctr1[i]) break; }   /* inc32(j0) */
    gctr(be, rk, nr, ctr1, ct, len, pt);
    crypto_wipe(rk, sizeof rk);
    crypto_wipe(H, sizeof H); crypto_wipe(ej0, sizeof ej0);
    return 0;
}

void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0) return;          /* negative lengths would walk backwards */
    gcm_core(key, 16, nonce, 12, aad, aadlen, pt, len, ct, tag);
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 16, nonce, 12, aad, aadlen, ct, len, tag, pt); }

/* --- AES-192-GCM --- key=24, nonce=12. Same seal/open contract; see the
 * key-size comment above the round-count helper. */
void aes192_gcm_seal(const uint8_t key[24], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0) return;
    gcm_core(key, 24, nonce, 12, aad, aadlen, pt, len, ct, tag);
}

int aes192_gcm_open(const uint8_t key[24], const uint8_t nonce[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 24, nonce, 12, aad, aadlen, ct, len, tag, pt); }

void aes256_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0) return;
    gcm_core(key, 32, nonce, 12, aad, aadlen, pt, len, ct, tag);
}

int aes256_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *aad, int aadlen,
                    const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 32, nonce, 12, aad, aadlen, ct, len, tag, pt); }

/* --- arbitrary-length IV entry points ------------------------------------
 * Same cores, an explicit ivlen. The 96-bit check inside gcm_j0 routes these
 * onto the exact fast path the fixed functions take, which is why the
 * ivlen==12 case needs no separate branch here -- and why the test that pins
 * _iv(12) == the fixed function matters: it is the assertion that the two
 * entry points share one code path, not two paths that happen to agree. */
void aes128_gcm_seal_iv(const uint8_t key[16], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0 || ivlen < 1 || ivlen > 1024) return;
    gcm_core(key, 16, iv, ivlen, aad, aadlen, pt, len, ct, tag);
}

int aes128_gcm_open_iv(const uint8_t key[16], const uint8_t *iv, int ivlen,
                       const uint8_t *aad, int aadlen,
                       const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 16, iv, ivlen, aad, aadlen, ct, len, tag, pt); }

void aes192_gcm_seal_iv(const uint8_t key[24], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0 || ivlen < 1 || ivlen > 1024) return;
    gcm_core(key, 24, iv, ivlen, aad, aadlen, pt, len, ct, tag);
}

int aes192_gcm_open_iv(const uint8_t key[24], const uint8_t *iv, int ivlen,
                       const uint8_t *aad, int aadlen,
                       const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 24, iv, ivlen, aad, aadlen, ct, len, tag, pt); }

void aes256_gcm_seal_iv(const uint8_t key[32], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    if (aadlen < 0 || len < 0 || ivlen < 1 || ivlen > 1024) return;
    gcm_core(key, 32, iv, ivlen, aad, aadlen, pt, len, ct, tag);
}

int aes256_gcm_open_iv(const uint8_t key[32], const uint8_t *iv, int ivlen,
                       const uint8_t *aad, int aadlen,
                       const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{ return gcm_open(key, 32, iv, ivlen, aad, aadlen, ct, len, tag, pt); }
