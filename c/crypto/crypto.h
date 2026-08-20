#ifndef LOGIT_CRYPTO_H
#define LOGIT_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* Overwrite key material through a volatile pointer so the compiler cannot
 * elide it as a dead store. Single shared implementation -- every crypto
 * primitive (and tls.c, rng.c) wipes through this. */
static inline void crypto_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* --- runtime SIMD dispatch (c/crypto/aead/aes_dispatch.c) ---
 * AES-GCM has a portable backend and an AES-NI + PCLMULQDQ one; which runs is
 * decided once from CPUID. Callers of aes*_gcm_* need none of this -- it is
 * here so the boot path can select and report, and so tests can pin a
 * backend. */
void        crypto_simd_init(void);            /* select once; idempotent */
const char *crypto_simd_backend_name(void);    /* "c" | "aesni" */
int         crypto_simd_constant_time(void);   /* 1 if AES-GCM is constant-time here */
void        crypto_simd_force_baseline(int on);/* pin the portable path (tests) */
/* 0 when the selected backend agrees with the portable one on the key
 * schedule, the block cipher and the GF multiply; else the 1-based index of
 * the first check that disagreed. */
int         crypto_simd_selftest(void);

/* --- SHA-256 --- */
#define SHA256_BLOCK 64
#define SHA256_LEN   32
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* SHA-224: the same core with the FIPS 180-4 6.3 IV, truncated to 28 bytes.
 * Shares struct sha256 and sha256_update -- call sha224_init, then the shared
 * update, then sha224_final, exactly as the SHA-512/t pair below does. It is
 * here because the family had a hole: 512/224 and 512/256 exist above, and
 * SHA-224 is the member every certificate profile and PKCS#11 token still
 * lists. Nothing in-tree negotiates it. */
#define SHA224_BLOCK 64
#define SHA224_LEN   28
void sha224_init(struct sha256 *c);
void sha224_final(struct sha256 *c, uint8_t out[28]);
void sha224(const void *data, size_t len, uint8_t out[28]);

/* --- SHA-384 (truncated SHA-512) --- */
#define SHA384_BLOCK 128
#define SHA384_LEN   48
#define SHA512_BLOCK 128
#define SHA512_LEN   64
struct sha512 { uint64_t h[8]; uint64_t len_hi, len_lo; uint8_t buf[128]; int n; };
void sha384_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha384_final(struct sha512 *c, uint8_t out[48]);
void sha384(const void *data, size_t len, uint8_t out[48]);
/* Full SHA-512 (same core as SHA-384). */
void sha512_init(struct sha512 *c);
void sha512_final(struct sha512 *c, uint8_t out[64]);
void sha512(const void *data, size_t len, uint8_t out[64]);
/* SHA-512/224 and SHA-512/256: the same core again, with the FIPS 180-4
 * IV-generation IVs (see sha384.c for why those differ from every other
 * SHA-2 member's). One streaming struct for the whole family: call the
 * member's _init, share sha512_update, call the member's _final. */
#define SHA512_224_BLOCK 128
#define SHA512_224_LEN   28
#define SHA512_256_BLOCK 128
#define SHA512_256_LEN   32
void sha512_224_init(struct sha512 *c);
void sha512_224_final(struct sha512 *c, uint8_t out[28]);
void sha512_224(const void *data, size_t len, uint8_t out[28]);
void sha512_256_init(struct sha512 *c);
void sha512_256_final(struct sha512 *c, uint8_t out[32]);
void sha512_256(const void *data, size_t len, uint8_t out[32]);

/* --- HMAC / HKDF (parameterised by hash length: 28 = SHA-224, 32 = SHA-256,
 * 48 = SHA-384, 64 = SHA-512) --- */
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

/* --- TLS 1.2 PRF (RFC 5246 §5) --- P_hash over HMAC, an entirely different
 * construction from the HKDF ladder above; see the comment in hmac_hkdf.c.
 * `hlen` selects the hash the negotiated suite names (32 = SHA-256 for the
 * *_SHA256 suites, 48 = SHA-384 for *_SHA384). `label` is a NUL-terminated
 * ASCII literal ("master secret", "key expansion", "client finished", ...) that
 * is prepended to `seed`. Writes exactly outlen bytes; silently writes nothing
 * on a bad hlen / over-long label or seed. */
void tls12_prf(int hlen, const uint8_t *secret, int seclen, const char *label,
               const uint8_t *seed, int seedlen, uint8_t *out, int outlen);

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

/* --- AEAD: AES-192-GCM --- key=24, nonce=12. Same seal/open contract; the
 * FIPS-197 middle key size (12 rounds, nk=6 schedule). Nothing in-tree
 * negotiates it; it exists so the AES-GCM family is complete rather than
 * 128-or-256 with a hole. */
void aes192_gcm_seal(const uint8_t key[24], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes192_gcm_open(const uint8_t key[24], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* --- AEAD: AES-256-GCM --- key=32, nonce=12. Same seal/open contract. Shares
 * the whole implementation with AES-128-GCM (14 rounds instead of 10). Present
 * because TLS 1.2 servers commonly prefer ECDHE_*_WITH_AES_256_GCM_SHA384. */
void aes256_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes256_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, int aadlen,
                     const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* --- AES-GCM with arbitrary IV lengths (SP 800-38D 5.2.1.1) --- Same seal/open
 * contract as the fixed 12-byte functions above, but the IV may be any length
 * 1..1024: for the 96-bit case J0 is IV||0^31||1 (the fast path the fixed
 * functions take); otherwise J0 = GHASH_H(IV || 0^s || 0^64 || [len(IV)]_64),
 * exactly the construction a non-96-bit IV requires. Nothing in-tree
 * negotiates a non-96-bit GCM IV (TLS never has one); this exists because
 * every other GCM consumer in the wild may hand us one -- GMAC in IPsec, some
 * pre-shared-key record layers, and the "just use the 8-byte sequence number"
 * designs that predate the 96-bit convention. 96-bit calls through these
 * produce byte-identical output to the fixed functions (pinned by test). */
void aes128_gcm_seal_iv(const uint8_t key[16], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes128_gcm_open_iv(const uint8_t key[16], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);
void aes192_gcm_seal_iv(const uint8_t key[24], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes192_gcm_open_iv(const uint8_t key[24], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);
void aes256_gcm_seal_iv(const uint8_t key[32], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int  aes256_gcm_open_iv(const uint8_t key[32], const uint8_t *iv, int ivlen,
                        const uint8_t *aad, int aadlen,
                        const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* --- AES-CTR (SP 800-38A 6.5, F.5) --- key 16/24/32; iv is the FULL 16-byte
 * initial counter block, big-endian. NOTE: the counter increments over the
 * whole 128-bit block with carry -- NOT GCM's inc32, which only wraps the low
 * four bytes; SP 800-38A's CTR is the wide-counter variant. CTR is symmetric:
 * the same call encrypts and decrypts. in and out may alias. Not
 * authenticated: callers who need integrity use GCM above. */
void aes128_ctr(const uint8_t key[16], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out);
void aes192_ctr(const uint8_t key[24], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out);
void aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                const uint8_t *in, int len, uint8_t *out);

/* --- AES-CBC with PKCS#7 padding (SP 800-38A 6.2 + RFC 5652 6.3) ---
 * encrypt appends a full padding block when len is a multiple of 16 (PKCS#7
 * always pads), so the ciphertext is ((len/16)+1)*16 bytes; that length is
 * the return value, or -1 on a negative len. decrypt takes a multiple of 16
 * ciphertext bytes and returns the unpadded length, or -1 if the padding
 * does not check; the padding check is a constant-time accumulate over all
 * 16 candidate bytes -- the pad length is secret-ish (it is derived from the
 * plaintext tail) and an early-exit loop would leak it to a timing probe.
 * CBC here is for file/record encryption where a full extra pass of GCM is
 * not the point; iv is 16 bytes and is clobbered into the caller's copy of
 * the chaining state by nothing -- pass a fresh one per message. */
int aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct);
int aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt);
int aes192_cbc_encrypt(const uint8_t key[24], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct);
int aes192_cbc_decrypt(const uint8_t key[24], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt);
int aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *pt, int len, uint8_t *ct);
int aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *ct, int len, uint8_t *pt);

/* --- X25519 (RFC 7748) --- scalar*point on Curve25519; 32-byte little-endian. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* base point u=9 (key generation): out = scalar * basepoint. */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* --- ECDHE on NIST P-256 (curve=256) / P-384 (curve=384) ---
 * Key agreement for the TLS 1.3 named groups secp256r1/secp384r1, used when a
 * server refuses x25519 (which stays the preferred, constant-time group).
 * flen = curve/8. `priv` is flen bytes of raw randomness, big-endian; both
 * calls reject it unless it lies in [1, n-1], so the caller re-randomises and
 * retries rather than reducing (a reduction would bias the scalar). `blind` is
 * 32 fresh random bits used to blind the scalar multiplication -- see the long
 * comment in ecdsa.c for what that does and does not buy.
 *   ecdh_keygen: pub <- 0x04 || X || Y  (1 + 2*flen bytes)
 *   ecdh_shared: out <- X coordinate of priv*peer (flen bytes, big-endian);
 *                the peer point is range- and on-curve-checked.
 * Both return 0 on success, -1 on a rejected scalar or peer point. */
int ecdh_keygen(int curve, const uint8_t *priv, uint32_t blind, uint8_t *pub);
int ecdh_shared(int curve, const uint8_t *priv, uint32_t blind,
                const uint8_t *peer, int peerlen, uint8_t *out);

/* --- ECDSA verify on NIST P-256 (curve=256) / P-384 (curve=384) ---
 * pub is the uncompressed point X||Y (2*flen bytes, big-endian); sig is r||s
 * (2*flen bytes, big-endian); hash is the message digest (hlen bytes). Returns
 * 1 if valid, 0 otherwise. flen = curve/8. */
int ecdsa_verify(int curve, const uint8_t *pub, const uint8_t *sig,
                 const uint8_t *hash, int hlen);

/* --- ECDSA sign on P-256 (256) / P-384 (384) / P-521 (521) ------------------
 * The counterpart to ecdsa_verify, and the reason it exists is the TLS SERVER:
 * a client only ever verifies, so until c/net/tls/tls_server.c there was no
 * caller and the only signature this tree could PRODUCE was Ed25519.
 *
 * `priv` is the private scalar, flen bytes big-endian (flen = x509_ec_flen);
 * `hash` is the message digest, hlen in {32,48,64}; `sig` receives r||s, flen
 * bytes each. Returns 0, or -1 for an unknown curve, a scalar outside
 * [1, n-1], or an hlen no HMAC here dispatches on.
 *
 * k IS DETERMINISTIC (RFC 6979) and no RNG is consulted. `blind` is 32 fresh
 * random bits used only to blind the scalar multiplication, exactly as in
 * ecdh_keygen; it cannot change the output, so RFC 6979's r and s are a
 * known-answer oracle for any value of it. See the block comment in ecdsa.c
 * for why the RNG was taken out of this path rather than trusted.
 *
 * `mac` is this file's own hmac(), passed in rather than called: ecdsa.c is
 * deliberately dependency-free so `make test-crypto` can link it on its own,
 * and calling hmac() directly stopped that target LINKING. Same reason
 * ed25519_keypair takes its randbytes. Pass `hmac`; NULL returns -1. */
typedef void (*ecdsa_hmac_fn)(int hlen, const uint8_t *key, int keylen,
                              const uint8_t *msg, int msglen, uint8_t *out);
int ecdsa_sign(int curve, const uint8_t *priv, const uint8_t *hash, int hlen,
               uint32_t blind, ecdsa_hmac_fn mac, uint8_t *sig);

/* --- RSASSA-PKCS1-v1_5 verify --- n,e big-endian; sig big-endian (<= nlen);
 * hash is the digest (hlen 32 -> SHA-256, 48 -> SHA-384). 1 valid / 0 not. */
int rsa_pkcs1_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                     const uint8_t *sig, int siglen, const uint8_t *hash, int hlen);

/* RSASSA-PSS verify (TLS 1.3 rsa_pss_rsae_*, salt len = hash len, MGF1 same
 * hash). mhash is the message digest (hlen 32 -> SHA-256, 48 -> SHA-384). */
int rsa_pss_verify(const uint8_t *n, int nlen, const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen, const uint8_t *mhash, int hlen);

/* --- Ed25519 (RFC 8032) -- the first primitive here that SIGNS ---------------
 * The private key is a 32-byte `seed`; the public key is derived from it, so
 * there is no separate "private key format" to get wrong and no way to pair a
 * seed with the wrong public key by accident (ed25519_sign takes `pub` only to
 * avoid re-deriving it, and a mismatched one simply produces a signature that
 * does not verify).
 *
 * CONSTANT TIME: ed25519_pubkey / ed25519_sign / ed25519_keypair are, with
 * respect to the seed -- fixed-trip ladder, fe_cmov selection, no secret-indexed
 * table. ed25519_verify is not claimed to be and does not need to be: every
 * input to it is public. See the file header of ed25519.c. */
void ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t mlen,
                  const uint8_t seed[32], const uint8_t pub[32]);
int  ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t mlen,
                    const uint8_t pub[32]);
/* Fresh key pair. `randbytes` is injected rather than called directly because
 * this file is linked into the kernel (kernel_random_bytes), the host tests (a
 * deterministic stub) and userland (SYS_GETRANDOM) -- three different sources,
 * one implementation. Returns 0, or -1 if the source produced 32 zero bytes,
 * which is what a dead entropy source looks like. */
int  ed25519_keypair(uint8_t pub[32], uint8_t seed[32],
                     void (*randbytes)(uint8_t *, int));
/* Test hooks (see ed25519.c). */
int  ed25519_point_valid(const uint8_t p[32]);
int  ed25519_sc_reduce_selftest(void);

/* --- PBKDF2-HMAC-SHA224 / SHA256 / SHA-384 / SHA-512 (RFC 8018 5.2) ---------
 * Password -> key. `hlen` selects the PRF: 28 = HMAC-SHA-224, 32 = HMAC-SHA-256,
 * 48 = HMAC-SHA-384, 64 = HMAC-SHA-512. `iters` must be >= 1. Writes exactly dklen bytes. See
 * pbkdf2.c for why this and not scrypt/argon2, and for what it does and does
 * not defend against. */
void pbkdf2(int hlen, const uint8_t *pw, int pwlen,
            const uint8_t *salt, int saltlen, uint32_t iters,
            uint8_t *dk, int dklen);

/* One-step password hashing for an account record: generates a salt, runs
 * PBKDF2 and formats the result as a self-describing ASCII string
 *   $pbkdf2-sha256$<iters>$<salt-b64>$<dk-b64>
 * so the parameters travel with the hash and can be raised later without
 * invalidating existing records. Returns the string length, or -1 if `out` is
 * too small (128 bytes is always enough). */
int  pwhash_make(char *out, int max, const char *password,
                 uint32_t iters, void (*randbytes)(uint8_t *, int));
/* 1 if `password` matches the stored record, 0 otherwise. The digest
 * comparison is constant-time; the parameter parse is not (it is public). */
int  pwhash_check(const char *record, const char *password);

#endif /* LOGIT_CRYPTO_H */
