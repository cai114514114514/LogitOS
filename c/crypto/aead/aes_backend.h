#ifndef LOGIT_AES_BACKEND_H
#define LOGIT_AES_BACKEND_H

#include <stdint.h>

/* The four primitives the AES modes are built from, behind function pointers
 * so a faster/constant-time implementation can be selected once at boot.
 *
 * The split is deliberate: everything ABOVE these four (CTR mode, CBC mode,
 * the GHASH driver loop, the J0 construction, the tag comparison, the
 * decrypt-after-authenticate ordering) stays in ONE place in aesgcm.c /
 * aes_modes.c and is shared by every backend. A backend cannot get the mode
 * wrong, only the primitive -- and the primitive is exactly what a
 * differential test can cross-check block by block. A second full GCM
 * implementation would have doubled the amount of code that has to be right.
 *
 * `rk` is the flat expanded key: 16*(nr+1) bytes, FIPS-197 order. Both
 * backends must produce byte-identical schedules from the same key -- the
 * host test asserts it, because a schedule that differs is a bug that only
 * shows up as a wrong ciphertext much later.
 *
 * decrypt is the inverse cipher over the SAME `rk` the encryptor consumed --
 * the backends are responsible for whatever re-derivation their instruction
 * set needs (the AES-NI AESDEC chain wants the equivalent inverse schedule,
 * which AESIMC produces from these round keys on the fly). CTR never calls
 * it; GCM never calls it; CBC decrypt is its only customer today, and it was
 * added rather than hiding an inverse cipher inside aes_modes.c because "the
 * backend does the primitive" is the invariant the dispatch tests rest on: a
 * second implementation outside the table would exist precisely where the
 * differential cannot see it.
 *
 * gf_mul(x, y) is GCM's GF(2^128) multiply with the GCM bit convention
 * (byte 0 bit 7 is the x^0 coefficient), x *= y in place. */
struct aes_backend {
    const char *name;                                   /* "c" | "aesni" */
    void (*key_expand)(const uint8_t *key, int keylen, uint8_t *rk);
    void (*encrypt)(const uint8_t *rk, int nr,
                    const uint8_t in[16], uint8_t out[16]);
    void (*decrypt)(const uint8_t *rk, int nr,
                    const uint8_t in[16], uint8_t out[16]);
    void (*gf_mul)(uint8_t x[16], const uint8_t y[16]);
    int constant_time;                                  /* 1 = no secret-indexed loads/branches */
};

/* Portable reference. Always present, always correct, never removed: it is
 * what the accelerated path is tested against and what runs on a CPU without
 * AES-NI. Lives in aesgcm.c. */
const struct aes_backend *aes_backend_c(void);

/* AES-NI + PCLMULQDQ. NULL when the CPU lacks either, and on a non-x86 build. */
const struct aes_backend *aes_backend_ni(void);

/* The selected backend. Never NULL: falls back to aes_backend_c(). Selection
 * happens once (crypto_simd_init, or lazily on first call) and the result is a
 * pointer to a const struct in .rodata, so reading it needs no lock. */
const struct aes_backend *aes_current_backend(void);

#endif /* LOGIT_AES_BACKEND_H */
