#include "cshake.h"

/* See cshake.h for the design notes (why keccak.c/.h are not touched, the
 * empty-N-S trap, the bit/byte trap in left_encode/right_encode). This file
 * is the implementation of SP 800-185 sections 2.3.3 and 3.
 *
 * CONSTANT TIME: nothing here branches on, or indexes memory by, a secret
 * BYTE VALUE. The only data-dependent control flow is on LENGTHS (rate, N/S/
 * K/message lengths, output length), which are public parameters of the
 * call, not secret content -- the same standard sha256.c/sha384.c use for
 * their own length-driven padding in this tree. The sponge absorb/squeeze
 * loops (shake_absorb/shake_squeeze in keccak.c) are XOR-into-state and
 * read-from-state at a position derived only from a public byte counter.
 * KMAC's key bytes pass through here (kmac.c) purely as XOR input to the
 * permutation, at positions that do not depend on the key's VALUE -- so this
 * inherits the sponge's constant-time property rather than needing its own,
 * the same way SHA-3 needs none beyond what Keccak-f itself provides. */

static int enc_len(uint64_t x)
{
    /* Smallest n >= 1 such that 2^(8n) > x; x == 0 gives n == 1 (SP 800-185's
     * own special case -- the loop below naturally produces it because it
     * always executes at least once). */
    int n = 0;
    uint64_t t = x;
    do { n++; t >>= 8; } while (t);
    return n;
}

size_t left_encode(uint8_t out[9], uint64_t x)
{
    int n = enc_len(x);
    out[0] = (uint8_t)n;
    for (int i = 0; i < n; i++)
        out[1 + i] = (uint8_t)(x >> (8 * (n - 1 - i)));
    return (size_t)(n + 1);
}

size_t right_encode(uint8_t out[9], uint64_t x)
{
    int n = enc_len(x);
    for (int i = 0; i < n; i++)
        out[i] = (uint8_t)(x >> (8 * (n - 1 - i)));
    out[n] = (uint8_t)n;
    return (size_t)(n + 1);
}

void cshake_absorb(struct cshake_ctx *c, const uint8_t *in, size_t len)
{
    shake_absorb(&c->sp, in, len);
}

/* Reimplements keccak.c's private pad_and_switch(dsbyte) against struct
 * shake's public fields -- see cshake.h for why this is not a call into
 * keccak.c. Byte order and the pad10*1 shape (dsbyte at `pos`, 0x80 at the
 * last byte of the rate, XORed rather than OR'd/overwritten so the two
 * collapse correctly into one byte when pos == rate-1) mirror keccak.c
 * exactly -- verified against the NIST cSHAKE_samples.pdf "After Permutation"
 * intermediate state for sample #1 during development of this file. */
void cshake_finalize(struct cshake_ctx *c)
{
    uint64_t *st  = c->sp.st;
    int       pos = c->sp.pos;
    int       last = c->sp.rate - 1;
    st[pos  >> 3] ^= (uint64_t)c->ds << (8 * (pos  & 7));
    st[last >> 3] ^= (uint64_t)0x80  << (8 * (last & 7));
    keccakf1600(st);
    c->sp.pos = 0;
    c->sp.squeezing = 1;
}

void cshake_squeeze(struct cshake_ctx *c, uint8_t *out, size_t len)
{
    shake_squeeze(&c->sp, out, len);
}

void cshake_bp_begin(struct cshake_bp *bp, struct cshake_ctx *c, int rate)
{
    bp->c = c;
    bp->rate = rate;
    uint8_t wenc[9];
    size_t wlen = left_encode(wenc, (uint64_t)rate);   /* `rate` IS bytes here -- SP 800-185's w */
    cshake_absorb(c, wenc, wlen);
    bp->total = wlen;
}

void cshake_bp_feed_string(struct cshake_bp *bp, const uint8_t *X, size_t Xlen)
{
    /* encode_string(X) = left_encode(len(X) IN BITS) || X. This *8 is the
     * exact spot the header's byte/bit trap lives; every caller of this
     * function passes a byte length in, and the bit conversion happens once,
     * here. */
    uint8_t enc[9];
    size_t elen = left_encode(enc, (uint64_t)Xlen * 8);
    cshake_absorb(bp->c, enc, elen);
    if (Xlen) cshake_absorb(bp->c, X, Xlen);
    bp->total += elen + Xlen;
}

void cshake_bp_end(struct cshake_bp *bp)
{
    static const uint8_t zeros[168] = { 0 };   /* >= the larger of the two rates (168) */
    size_t rem = bp->total % (size_t)bp->rate;
    size_t pad = rem ? (size_t)bp->rate - rem : 0;
    while (pad) {
        size_t n = pad < sizeof(zeros) ? pad : sizeof(zeros);
        cshake_absorb(bp->c, zeros, n);
        pad -= n;
    }
}

static void cshake_ctx_init(struct cshake_ctx *c, int rate256,
                             const uint8_t *N, size_t Nlen,
                             const uint8_t *S, size_t Slen)
{
    if (rate256) shake256_init(&c->sp); else shake128_init(&c->sp);

    if (Nlen == 0 && Slen == 0) {
        /* cSHAKE(X, L, "", "") IS SHAKE(X, L) -- no bytepad prefix, domain
         * separator 0x1F. This is the branch the identity differential in
         * tests/unit/cshake_test.c exists to hold open. */
        c->ds = 0x1F;
        return;
    }
    c->ds = 0x04;
    int rate = rate256 ? 136 : 168;
    struct cshake_bp bp;
    cshake_bp_begin(&bp, c, rate);
    cshake_bp_feed_string(&bp, N, Nlen);
    cshake_bp_feed_string(&bp, S, Slen);
    cshake_bp_end(&bp);
}

void cshake128_ctx_init(struct cshake_ctx *c,
                         const uint8_t *N, size_t Nlen,
                         const uint8_t *S, size_t Slen)
{ cshake_ctx_init(c, 0, N, Nlen, S, Slen); }

void cshake256_ctx_init(struct cshake_ctx *c,
                         const uint8_t *N, size_t Nlen,
                         const uint8_t *S, size_t Slen)
{ cshake_ctx_init(c, 1, N, Nlen, S, Slen); }

static void cshake_oneshot(int rate256, uint8_t *out, size_t outlen,
                            const uint8_t *in, size_t inlen,
                            const uint8_t *N, size_t Nlen,
                            const uint8_t *S, size_t Slen)
{
    struct cshake_ctx c;
    cshake_ctx_init(&c, rate256, N, Nlen, S, Slen);
    cshake_absorb(&c, in, inlen);
    cshake_finalize(&c);
    cshake_squeeze(&c, out, outlen);
}

void cshake128(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen,
               const uint8_t *N, size_t Nlen, const uint8_t *S, size_t Slen)
{ cshake_oneshot(0, out, outlen, in, inlen, N, Nlen, S, Slen); }

void cshake256(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen,
               const uint8_t *N, size_t Nlen, const uint8_t *S, size_t Slen)
{ cshake_oneshot(1, out, outlen, in, inlen, N, Nlen, S, Slen); }
