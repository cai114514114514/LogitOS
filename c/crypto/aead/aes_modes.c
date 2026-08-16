/* AES-CTR and AES-CBC/PKCS#7 -- the unauthenticated AES modes.
 *
 * WHY THESE ARE HERE AND NOT BESIDE TLS: GCM (aesgcm.c) is what a network
 * protocol should use and this tree's TLS does. CTR and CBC exist because the
 * rest of the world did not finish adopting AEAD: firmware images, on-disk
 * key blobs and a dozen file formats are AES-CBC/PKCS#7, and raw CTR is the
 * inner loop of GCM itself and of several DRM/streaming scramblers. These are
 * compatibility/OFM primitives, not recommendations -- every caller that CAN
 * use GCM should.
 *
 * THE SPLIT mirrors aesgcm.c exactly: the MODE lives here once, shared by
 * every backend; the block primitives come from aes_current_backend(). CTR
 * uses only the encryptor (a stream cipher both ways); CBC decrypt is the one
 * customer of the backend's decrypt primitive. Nothing here reaches around
 * the dispatch -- on an AES-NI CPU these run AES-NI, and the "never silently
 * select an unimplemented path" property is structural: there IS no
 * portable-only path here to fall into, because the modes call the same
 * function pointers GCM calls.
 *
 * CTR COUNTER SEMANTICS: SP 800-38A 6.5 -- the increment is over the whole
 * 128-bit block, big-endian, with carry from byte 15 up to byte 0. That is a
 * DIFFERENT rule from GCM's inc32 (low four bytes only) and choosing GCM's
 * here is invisible until a counter block ends in ffffffff, at which point
 * GCM's rule replays a keystream block and SP 800-38A's does not. The
 * ffffffff-edge vectors in the diff battery pin the difference.
 *
 * CBC PADDING: PKCS#7 (RFC 5652 6.3) -- always pad, so a full block of
 * padding follows any exact multiple of 16; decrypt validates the padding
 * with a constant-time accumulate over all 16 candidate bytes and one branch
 * at the end. The early-exit version (check byte by byte, bail at the first
 * mismatch) is the standard implementation and leaks the pad length -- hence
 * the plaintext length -- through timing, which is a real oracle when the
 * same key encrypts many records (TLS 1.0 CBC all over again). */
#include "crypto.h"
#include "aes_backend.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

#define AES_RK_MAX 240                          /* 4 * (14 + 1) * 4, as in aesgcm.c */

static int aes_rounds(int keylen)
{
    return keylen == 32 ? 14 : keylen == 24 ? 12 : 10;
}

/* --- CTR ------------------------------------------------------------------ */
static void ctr_core(const uint8_t *key, int keylen, const uint8_t iv[16],
                     const uint8_t *in, int len, uint8_t *out)
{
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(key, keylen, rk);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (int off = 0; off < len; off += 16) {
        be->encrypt(rk, nr, ctr, ks);
        int n = len - off; if (n > 16) n = 16;
        for (int i = 0; i < n; i++) out[off+i] = in[off+i] ^ ks[i];
        for (int i = 15; i >= 0; i--)            /* full 128-bit ++, with carry */
            if (++ctr[i]) break;
    }
    crypto_wipe(ks, sizeof ks);                  /* keystream */
    crypto_wipe(rk, sizeof rk);                  /* expanded key */
}

void aes128_ctr(const uint8_t key[16], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out)
{ if (len >= 0) ctr_core(key, 16, iv, in, len, out); }

void aes192_ctr(const uint8_t key[24], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out)
{ if (len >= 0) ctr_core(key, 24, iv, in, len, out); }

void aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out)
{ if (len >= 0) ctr_core(key, 32, iv, in, len, out); }

/* --- CBC encrypt ------------------------------------------------------------
 * PKCS#7: pad = 16 - (len % 16), always 1..16 bytes, every pad byte = pad. */
static int cbc_encrypt_core(const uint8_t *key, int keylen, const uint8_t iv[16],
                            const uint8_t *pt, int len, uint8_t *ct)
{
    if (len < 0) return -1;
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(key, keylen, rk);
    uint8_t chain[16]; memcpy(chain, iv, 16);
    int pad = 16 - (len % 16);
    int total = len + pad;                       /* caller sized ct for this */

    int off;
    for (off = 0; off + 16 <= len; off += 16) {
        for (int i = 0; i < 16; i++) chain[i] ^= pt[off+i];
        be->encrypt(rk, nr, chain, ct + off);
        memcpy(chain, ct + off, 16);
    }
    /* final block: the remaining 0..15 plaintext bytes then the pad */
    uint8_t last[16];
    for (int i = 0; i < len - off; i++) last[i] = pt[off+i];
    for (int i = len - off; i < 16; i++) last[i] = (uint8_t)pad;
    for (int i = 0; i < 16; i++) chain[i] ^= last[i];
    be->encrypt(rk, nr, chain, ct + off);
    crypto_wipe(last, sizeof last);
    crypto_wipe(chain, sizeof chain);            /* chained through every block */
    crypto_wipe(rk, sizeof rk);
    return total;
}

/* --- CBC decrypt ------------------------------------------------------------
 * Constant-time PKCS#7 validation: `bad` accumulates over every one of the 16
 * possible pad positions with a mask built from a branchless comparison, and
 * the single `if (bad)` happens after all bytes have been read. */
static int cbc_decrypt_core(const uint8_t *key, int keylen, const uint8_t iv[16],
                            const uint8_t *ct, int len, uint8_t *pt)
{
    if (len < 16 || (len & 15)) return -1;       /* empty and ragged are not ours */
    const struct aes_backend *be = aes_current_backend();
    int nr = aes_rounds(keylen);
    uint8_t rk[AES_RK_MAX]; be->key_expand(key, keylen, rk);
    uint8_t chain[16]; memcpy(chain, iv, 16);
    uint8_t prev[16], dec[16];

    for (int off = 0; off < len; off += 16) {
        memcpy(prev, chain, 16);                  /* the XOR mask for this block */
        memcpy(chain, ct + off, 16);              /* snapshot BEFORE any pt write,
                                                    * so ct == pt (in place) is safe */
        be->decrypt(rk, nr, ct + off, dec);
        for (int i = 0; i < 16; i++) pt[off+i] = dec[i] ^ prev[i];
    }

    uint8_t pad = pt[len-1];
    unsigned bad = ((unsigned)pad - 1) >> 4;     /* nonzero if pad == 0 or > 16 */
    for (int i = 0; i < 16; i++) {
        /* mask = 0xff for the i < pad bytes that must equal pad, else 0; the
         * comparison compiles branchless (setcc), never a jump on `pad` */
        unsigned mask = (i < pad) ? 0xffu : 0x00u;
        bad |= (unsigned)(pt[len-1-i] ^ pad) & mask;
    }
    if (bad) {
        /* no plaintext leaves on a bad pad: wipe what we decrypted, not just
         * return -- the partial bytes are still attacker-chosen ciphertext
         * decrypted under the key */
        crypto_wipe(pt, (size_t)len);
        crypto_wipe(dec, sizeof dec);
        crypto_wipe(chain, sizeof chain);
        crypto_wipe(rk, sizeof rk);
        return -1;
    }
    crypto_wipe(dec, sizeof dec);
    crypto_wipe(chain, sizeof chain);
    crypto_wipe(rk, sizeof rk);
    return len - pad;
}

int aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct)
{ return cbc_encrypt_core(key, 16, iv, pt, len, ct); }

int aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt)
{ return cbc_decrypt_core(key, 16, iv, ct, len, pt); }

int aes192_cbc_encrypt(const uint8_t key[24], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct)
{ return cbc_encrypt_core(key, 24, iv, pt, len, ct); }

int aes192_cbc_decrypt(const uint8_t key[24], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt)
{ return cbc_decrypt_core(key, 24, iv, ct, len, pt); }

int aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct)
{ return cbc_encrypt_core(key, 32, iv, pt, len, ct); }

int aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt)
{ return cbc_decrypt_core(key, 32, iv, ct, len, pt); }
