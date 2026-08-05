#ifndef AETHER_CRYPTO_H
#define AETHER_CRYPTO_H

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
/* Full SHA-512 (same core as SHA-384). */
void sha512_init(struct sha512 *c);
void sha512_final(struct sha512 *c, uint8_t out[64]);
void sha512(const void *data, size_t len, uint8_t out[64]);

/* --- HMAC / HKDF (parameterised by hash length: 32 = SHA-256, 48 = SHA-384) --- */
void hmac(int hlen, const uint8_t *key, int keylen,
          const uint8_t *msg, int msglen, uint8_t *out);
void hkdf_extract(int hlen, const uint8_t *salt, int saltlen,
                  const uint8_t *ikm, int ikmlen, uint8_t *prk);
void hkdf_expand(int hlen, const uint8_t *prk, const uint8_t *info, int infolen,
                 uint8_t *out, int outlen);
/* TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1). 0 ok, -1 if label > 64 bytes,
 * context > 64 bytes, or outlen out of range (fixed internal buffer). */
int hkdf_expand_label(int hlen, const uint8_t *secret, const char *label,
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

/* --- X25519 (RFC 7748) --- scalar*point on Curve25519; 32-byte little-endian. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* base point u=9 (key generation): out = scalar * basepoint. */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* --- ECDSA verify on NIST P-256 (curve=256) / P-384 (curve=384) ---
 * pub is the uncompressed point X||Y (2*flen bytes, big-endian); sig is r||s
 * (2*flen bytes, big-endian); hash is the message digest (hlen bytes). Returns
 * 1 if valid, 0 otherwise. flen = curve/8. */
int ecdsa_verify(int curve, const uint8_t *pub, const uint8_t *sig,
                 const uint8_t *hash, int hlen);

/* --- RSASSA-PKCS1-v1_5 verify --- n,e big-endian; sig big-endian (<= nlen);
 * hash is the digest (hlen 32 -> SHA-256, 48 -> SHA-384). 1 valid / 0 not. */
int rsa_pkcs1_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen, const uint8_t *hash, int hlen);

/* RSASSA-PSS verify (TLS 1.3 rsa_pss_rsae_*, salt len = hash len, MGF1 same
 * hash). mhash is the message digest (hlen 32 -> SHA-256, 48 -> SHA-384). */
int rsa_pss_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen, const uint8_t *mhash, int hlen);

#endif /* AETHER_CRYPTO_H */
