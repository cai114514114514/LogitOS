/* XChaCha20-Poly1305 -- draft-irtf-cfrg-xchacha-03 (an IETF CFRG draft that
 * never became an RFC; there is no RFC-numbered appendix to cite, which is
 * why the vectors below are cited by section number of that draft plus an
 * external differential rather than "RFC NNNN Appendix A"). Its inner AEAD
 * is RFC 8439 unchanged -- this file supplies only the 192-bit-nonce
 * subkey derivation (HChaCha20) and the nonce split that feeds it into the
 * existing chacha20_poly1305_seal/open in chacha20poly1305.c.
 *
 * NOT WIRED INTO ANYTHING. Primitive + gate only -- see xchacha20poly1305.h.
 *
 * Why this exists with no consumer today: XChaCha20-Poly1305's whole value
 * is the 192-bit nonce. RFC 8439's 96-bit nonce makes RANDOM nonce
 * generation unsafe past roughly 2^32 messages under one key (birthday
 * bound on a 96-bit space) -- a system that does not want to keep a
 * per-key counter across restarts/processes/machines needs either 192 bits
 * of random nonce or a distributed-counter protocol. Every real design
 * that reached for that trade (libsodium's default AEAD, age's payload
 * encryption, and the construction WireGuard's own extended-nonce
 * discussions cite) reached for exactly this one.
 *
 * === THE NONCE SPLIT (draft section 2.3), and it is NOT symmetric ===
 * The 24-byte XChaCha20 nonce splits as:
 *   nonce[0:16]  -> HChaCha20's nonce, produces the 32-byte subkey
 *   nonce[16:24] -> the last 8 bytes of the INNER (RFC 8439) 12-byte nonce,
 *                   with FOUR ZERO BYTES PREPENDED, not appended.
 * Prepending vs appending is invisible from types (both produce "a 12-byte
 * array"), so it is exactly the kind of thing that compiles, passes a
 * self-consistency check, and produces a working-looking AEAD that will
 * never interoperate with anything else on earth. Only an external vector
 * with a real inner nonce catches it -- draft A.1's ciphertext is that
 * vector, and this file's negative control (XCHACHA_BUG_NONCE_APPEND)
 * demonstrates it going red.
 *
 * === HChaCha20's OUTPUT (draft section 2.2), and it is NOT chacha_block ===
 * chacha_block() (chacha20poly1305.c, shared via chacha_core.h) returns
 * le32(x[i] + s[i]) for every word -- the ordinary ChaCha20 "add the input
 * back in" step (RFC 8439 section 2.3, step 4). HChaCha20 stops one step
 * earlier: its output is the raw post-permutation state, words 0-3 and
 * 12-15, WITHOUT that addition. Skipping the subtraction below (i.e.
 * reusing chacha_block's output as-is) is documented in the draft's own
 * prose as the standalone-vector's entire reason to exist, and is this
 * file's other negative control (XCHACHA_BUG_ADD_BACK): it is invisible
 * against a full AEAD round-trip test written against your OWN
 * implementation (seal and a matching open agree with each other either
 * way), and only shows up against draft section 2.2.1's independent
 * HChaCha20-only vector.
 */
#include "crypto.h"
#include "chacha_core.h"
#include "xchacha20poly1305.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifndef XCHACHA_BUG_ADD_BACK
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
#endif

void hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32])
{
    /* HChaCha20's state is ChaCha20's state with the 16-byte nonce filling
     * the last four words (draft section 2.2's own figure) -- the same
     * slots chacha_block() fills with (counter, 12-byte nonce). Word 0 of
     * HChaCha20's nonce goes where the block counter goes; words 1-3 go
     * where chacha_block's 12-byte nonce goes. That is not a coincidence
     * being exploited, it is what the draft's two state diagrams show
     * side by side -- the layouts are literally the same. */
    uint32_t counter = rd32(nonce);
    uint8_t block[64];
    chacha_block(key, counter, nonce + 4, block);

    /* Recompute the same four constants + eight key words + (counter,
     * nonce) layout chacha_block() built, so it can be subtracted back out
     * of chacha_block's output (see the file header). This is bookkeeping
     * about word placement, not the permutation -- the permutation itself
     * (the 20 rounds) is never re-implemented here, only called through
     * chacha_block(). */
    static const uint32_t iv[4] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
    };
    uint32_t s[16];
    s[0] = iv[0]; s[1] = iv[1]; s[2] = iv[2]; s[3] = iv[3];
    for (int i = 0; i < 8; i++) s[4 + i] = rd32(key + 4 * i);
    s[12] = counter;
    s[13] = rd32(nonce + 4);
    s[14] = rd32(nonce + 8);
    s[15] = rd32(nonce + 12);

#ifdef XCHACHA_BUG_ADD_BACK
    /* NEGATIVE CONTROL: reuse chacha_block's output directly, i.e. forget
     * to undo RFC 8439 step 4's addition. Per the file header this is "the
     * single most common HChaCha20 bug", and it is deliberately invisible
     * to a self-consistency (seal-then-open) test -- only the standalone
     * draft-2.2.1 vector, checked below, must catch it. */
    memcpy(out, block, 16);
    memcpy(out + 16, block + 48, 16);
#else
    uint32_t bw[16];
    for (int i = 0; i < 16; i++) bw[i] = rd32(block + 4 * i);
    for (int i = 0; i < 4; i++)  wr32(out + 4 * i, bw[i] - s[i]);
    for (int i = 0; i < 4; i++)  wr32(out + 16 + 4 * i, bw[12 + i] - s[12 + i]);
#endif

    crypto_wipe(block, sizeof block);
    crypto_wipe(s, sizeof s);
}

void xchacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[24],
                             const uint8_t *aad, int aadlen,
                             const uint8_t *pt, int len, uint8_t *ct,
                             uint8_t tag[16])
{
    uint8_t subkey[32];
    hchacha20(key, nonce, subkey);

    uint8_t inner[12];
#ifdef XCHACHA_BUG_NONCE_APPEND
    /* NEGATIVE CONTROL: append the four zero bytes instead of prepending
     * them -- a real-looking inner nonce, wrong RFC 8439 word position for
     * the block-counter-adjacent constant-zero prefix the draft specifies.
     * Round-trips against itself; disagrees with draft A.1's ciphertext. */
    memcpy(inner, nonce + 16, 8);
    memset(inner + 8, 0, 4);
#else
    memset(inner, 0, 4);
    memcpy(inner + 4, nonce + 16, 8);
#endif

    chacha20_poly1305_seal(subkey, inner, aad, aadlen, pt, len, ct, tag);
    crypto_wipe(subkey, sizeof subkey);
}

int xchacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[24],
                            const uint8_t *aad, int aadlen,
                            const uint8_t *ct, int len, const uint8_t tag[16],
                            uint8_t *pt)
{
    uint8_t subkey[32];
    hchacha20(key, nonce, subkey);

    uint8_t inner[12];
#ifdef XCHACHA_BUG_NONCE_APPEND
    memcpy(inner, nonce + 16, 8);
    memset(inner + 8, 0, 4);
#else
    memset(inner, 0, 4);
    memcpy(inner + 4, nonce + 16, 8);
#endif

    int r = chacha20_poly1305_open(subkey, inner, aad, aadlen, ct, len, tag, pt);
    crypto_wipe(subkey, sizeof subkey);
    return r;
}
