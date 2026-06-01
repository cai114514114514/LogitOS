#ifndef AQUA_CRYPTO_H
#define AQUA_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* --- SHA-256 --- */
#define SHA256_BLOCK 64
#define SHA256_LEN   32
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* --- SHA-384 (truncated SHA-512) --- */
#define SHA384_BLOCK 128
#define SHA384_LEN   48
struct sha512 { uint64_t h[8]; uint64_t len_hi, len_lo; uint8_t buf[128]; int n; };
void sha384_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha384_final(struct sha512 *c, uint8_t out[48]);
void sha384(const void *data, size_t len, uint8_t out[48]);

/* --- HMAC / HKDF (parameterised by hash length: 32 = SHA-256, 48 = SHA-384) --- */
void hmac(int hlen, const uint8_t *key, int keylen,
          const uint8_t *msg, int msglen, uint8_t *out);
void hkdf_extract(int hlen, const uint8_t *salt, int saltlen,
                  const uint8_t *ikm, int ikmlen, uint8_t *prk);
void hkdf_expand(int hlen, const uint8_t *prk, const uint8_t *info, int infolen,
                 uint8_t *out, int outlen);
/* TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1). */
void hkdf_expand_label(int hlen, const uint8_t *secret, const char *label,
                       const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen);

/* --- AEAD: ChaCha20-Poly1305 (RFC 8439) ---
 * key=32, nonce=12. seal writes ciphertext (len bytes) + tag[16]; open verifies
 * the tag and writes plaintext. open returns 0 on success, -1 on tag mismatch. */
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, int aadlen,
                            const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, int aadlen,
                            const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* --- AEAD: AES-128-GCM --- key=16, nonce=12. Same seal/open contract. */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

#endif /* AQUA_CRYPTO_H */
