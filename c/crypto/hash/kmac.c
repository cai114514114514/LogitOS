#include "kmac.h"
#include "cshake.h"

/* See kmac.h for the KMAC-vs-KMACXOF trailer trap this file is built around,
 * and cshake.h for the sponge/domain-separator notes this sits on top of.
 *
 * NEGATIVE CONTROL: -DKMAC_NEGCTL_XOF_TRAILER makes kmac_core() ALWAYS use
 * the XOF trailer (right_encode(0)) regardless of the `is_xof` argument --
 * i.e. it is the exact shortcut kmac.h's comment names ("implement KMAC by
 * calling KMACXOF and truncating"). Built and watched: every KMAC128/256
 * official vector goes red, every KMACXOF128/256 vector STAYS GREEN, because
 * KMACXOF's own trailer never changes. That asymmetry is the point -- a
 * control that turned everything red would not distinguish "the trailer
 * selection is wrong" from "the sponge is wrong", and the failure would look
 * like nothing built. This one looks exactly like what it is. */
static void kmac_core(int is256, uint8_t *out, size_t outlen,
                       const uint8_t *K, size_t Klen,
                       const uint8_t *X, size_t Xlen,
                       const uint8_t *S, size_t Slen, int is_xof)
{
    static const uint8_t NAME[4] = { 'K', 'M', 'A', 'C' };
    int rate = is256 ? 136 : 168;

    struct cshake_ctx c;
    if (is256) cshake256_ctx_init(&c, NAME, 4, S, Slen);
    else       cshake128_ctx_init(&c, NAME, 4, S, Slen);

    /* newX prefix: bytepad(encode_string(K), rate). */
    struct cshake_bp bp;
    cshake_bp_begin(&bp, &c, rate);
    cshake_bp_feed_string(&bp, K, Klen);
    cshake_bp_end(&bp);

    cshake_absorb(&c, X, Xlen);

#ifdef KMAC_NEGCTL_XOF_TRAILER
    is_xof = 1;
#endif
    uint8_t tenc[9];
    size_t tlen = is_xof ? right_encode(tenc, 0)
                          : right_encode(tenc, (uint64_t)outlen * 8);
    cshake_absorb(&c, tenc, tlen);

    cshake_finalize(&c);
    cshake_squeeze(&c, out, outlen);

    /* c->sp.st held key-derived sponge state through the whole call above;
     * wipe it rather than leave it on the stack for whatever the caller's
     * frame gets reused for next. Loop, not memset: this file, like the rest
     * of c/crypto, compiles into the freestanding kernel and must not assume
     * a libc memset is linkable (crypto.h's crypto_wipe does the same thing
     * the same way, but this file does not include crypto.h -- see kmac.h). */
    volatile uint64_t *vst = c.sp.st;
    for (int i = 0; i < 25; i++) vst[i] = 0;
}

void kmac128(uint8_t *out, size_t outlen, const uint8_t *K, size_t Klen,
             const uint8_t *X, size_t Xlen, const uint8_t *S, size_t Slen)
{ kmac_core(0, out, outlen, K, Klen, X, Xlen, S, Slen, 0); }

void kmac256(uint8_t *out, size_t outlen, const uint8_t *K, size_t Klen,
             const uint8_t *X, size_t Xlen, const uint8_t *S, size_t Slen)
{ kmac_core(1, out, outlen, K, Klen, X, Xlen, S, Slen, 0); }

void kmacxof128(uint8_t *out, size_t outlen, const uint8_t *K, size_t Klen,
                const uint8_t *X, size_t Xlen, const uint8_t *S, size_t Slen)
{ kmac_core(0, out, outlen, K, Klen, X, Xlen, S, Slen, 1); }

void kmacxof256(uint8_t *out, size_t outlen, const uint8_t *K, size_t Klen,
                const uint8_t *X, size_t Xlen, const uint8_t *S, size_t Slen)
{ kmac_core(1, out, outlen, K, Klen, X, Xlen, S, Slen, 1); }
