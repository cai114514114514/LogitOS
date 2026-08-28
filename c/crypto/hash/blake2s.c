/* BLAKE2s (RFC 7693, section 3). See blake2s.h for what this is and is not. */
#include "blake2s.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Same 8 words as SHA-256's IV (RFC 7693 2.6: fractional parts of sqrt of the
 * first 8 primes) -- BLAKE2 reuses them, it does not coincide with SHA-256 by
 * accident. Sharing sha256.c's table was considered and rejected: that table
 * is `static const` and file-local there, and reaching across files for eight
 * constants is not worth coupling two otherwise-independent primitives --
 * CLAUDE.md's "one jar, two doors" is about a VALUE that must agree, and here
 * the value is public and immutable, so a second copy cannot go stale the way
 * a policy constant can. */
static const uint32_t BLAKE2S_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

/* RFC 7693 Table (Appendix): the message-word permutation, one row per round.
 * BLAKE2b uses all 12 rows (rows 10 and 11 repeat rows 0 and 1); BLAKE2s uses
 * only the first 10 -- THE TRAP NAMED IN THE BRIEF. This table is correct for
 * both siblings; what must not happen is a shared "12 rounds" loop bound. */
static const uint8_t SIGMA[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4},
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8},
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13},
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9},
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11},
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10},
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0},
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint32_t load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32_le(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}

/* RFC 7693 3.1 G, BLAKE2s rotation constants R1..R4 = 16,12,8,7 -- the second
 * TRAP named in the brief: BLAKE2b's are 32,24,16,63 and a file that shares a
 * macro across both siblings and pulls one constant from the wrong table
 * still runs, still terminates, and hashes deterministically wrong. There is
 * no BLAKE2b in this file to share with, which is the guard against that: the
 * constants are inlined here, not parameterised.
 *
 * ALL EIGHT OPERANDS OF G, ACROSS THE WHOLE COMPRESSION FUNCTION, ARE MIXED
 * FROM SECRET STATE (the running hash h[], and in keyed mode the key itself,
 * enters the message schedule as ordinary input bytes). G itself is add /
 * xor / rotate-by-a-COMPILE-TIME-constant only -- no data-dependent branch,
 * no table lookup indexed by anything secret -- so it is constant-time on any
 * target where 32-bit add and a fixed-distance rotate are (true of every
 * general-purpose CPU this tree targets or hosts on; there is no AES-NI-style
 * exception here because there is no data-dependent S-box in ARX). */
#define G(a, b, c, d, x, y) do { \
    a = a + b + (x); \
    d = rotr32(d ^ a, 16); \
    c = c + d; \
    b = rotr32(b ^ c, 12); \
    a = a + b + (y); \
    d = rotr32(d ^ a, 8); \
    c = c + d; \
    b = rotr32(b ^ c, 7); \
} while (0)

/* RFC 7693 3.2 F. `last` is 1 for the final block of the whole message (sets
 * f0 = 0xFFFFFFFF), 0 otherwise. No tree mode here (single sequential leaf,
 * fanout=1 depth=1), so f1 (last-node) and v[15]'s XOR term are always 0 and
 * are simply omitted rather than XORed with a constant 0 -- the parameter
 * block below already folds fanout/depth/leaf_length/node_offset/node_depth/
 * inner_length/salt/personal, all zero in sequential mode, into that same
 * "omit rather than XOR-with-0" choice. */
static void blake2s_compress(struct blake2s *ctx, const uint8_t block[BLAKE2S_BLOCKBYTES], int last)
{
    uint32_t m[16];
    uint32_t v[16];
    int i;

    for (i = 0; i < 16; i++) m[i] = load32_le(block + i * 4);

    for (i = 0; i < 8; i++) v[i] = ctx->h[i];
    for (i = 0; i < 8; i++) v[8 + i] = BLAKE2S_IV[i];

    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (last) v[14] ^= 0xFFFFFFFFu;

    for (int r = 0; r < 10; r++) {
        const uint8_t *s = SIGMA[r];
        G(v[0], v[4], v[8],  v[12], m[s[0]],  m[s[1]]);
        G(v[1], v[5], v[9],  v[13], m[s[2]],  m[s[3]]);
        G(v[2], v[6], v[10], v[14], m[s[4]],  m[s[5]]);
        G(v[3], v[7], v[11], v[15], m[s[6]],  m[s[7]]);
        G(v[0], v[5], v[10], v[15], m[s[8]],  m[s[9]]);
        G(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        G(v[2], v[7], v[8],  v[13], m[s[12]], m[s[13]]);
        G(v[3], v[4], v[9],  v[14], m[s[14]], m[s[15]]);
    }

    for (i = 0; i < 8; i++) ctx->h[i] ^= v[i] ^ v[i + 8];
}

/* t is a 64-bit byte counter kept as two 32-bit halves because the target is
 * -mno-... no -- because the compression function's v[12]/v[13] XOR terms are
 * defined on the two halves separately (RFC 7693 3.2) and this avoids a
 * uint64_t/uint32_t split at every call site instead of one, here. */
static void increment_counter(struct blake2s *ctx, uint32_t inc)
{
    ctx->t[0] += inc;
    if (ctx->t[0] < inc) ctx->t[1]++;   /* carry: wrapped past 2^32 bytes */
}

static void blake2s_init0(struct blake2s *ctx)
{
    int i;
    for (i = 0; i < 8; i++) ctx->h[i] = BLAKE2S_IV[i];
    ctx->t[0] = 0; ctx->t[1] = 0;
    ctx->buflen = 0;
    memset(ctx->buf, 0, sizeof(ctx->buf));
}

/* RFC 7693 3.2 / 2.5: the parameter block. In sequential mode every field is
 * zero except digest_length and key_length, so the 32-byte block collapses to
 * one word XORed into h[0]: byte 0 = digest_length, byte 1 = key_length,
 * byte 2 = fanout (1), byte 3 = depth (1) -- little-endian, hence
 * 0x01010000 | (keylen<<8) | outlen.
 *
 * THE TRAP NAMED IN THE BRIEF: hardcoding 0x01010020 (keylen=0, outlen=32)
 * here is invisible against any unkeyed 32-byte-output vector -- every such
 * vector, including the whole of RFC 7693 Appendix B, would still pass -- and
 * silently produces the wrong hash for every other digest length and every
 * keyed call. That is exactly why the KAT battery below is 256 KEYED vectors,
 * not "abc" alone: it is the one shape of test that cannot be satisfied by
 * this shortcut. */
/* NEGATIVE CONTROL: -DLOGIT_BLAKE2S_BAD_PARAM hardcodes the parameter word to
 * the one value (keylen=0, outlen=32) that every unkeyed-32-byte vector --
 * including all of RFC 7693 Appendix B -- cannot distinguish from correct.
 * See tests/unit/blake2s_test.c and tests/unit/run-blake2s-openssl.sh for
 * what is and is not able to see this go red; it is the whole reason the KAT
 * gate is 256 KEYED vectors and not a handful of unkeyed "abc"-shaped ones. */
#ifdef LOGIT_BLAKE2S_BAD_PARAM
#define BLAKE2S_PARAM_WORD(outlen, keylen) (0x01010020u)
#else
#define BLAKE2S_PARAM_WORD(outlen, keylen) \
    (0x01010000u ^ ((uint32_t)(keylen) << 8) ^ (uint32_t)(outlen))
#endif

void blake2s_init(struct blake2s *ctx, size_t outlen)
{
    blake2s_init0(ctx);
    ctx->outlen = outlen;
    ctx->h[0] ^= BLAKE2S_PARAM_WORD(outlen, 0);
}

void blake2s_init_key(struct blake2s *ctx, size_t outlen, const void *key, size_t keylen)
{
    blake2s_init0(ctx);
    ctx->outlen = outlen;
    ctx->h[0] ^= BLAKE2S_PARAM_WORD(outlen, keylen);

    if (keylen > 0) {
        /* THE SECOND TRAP NAMED IN THE BRIEF: the key becomes ONE
         * zero-padded 64-byte block, fed through the ordinary update path so
         * it is counted into t exactly like any other input block. It is
         * buffered here, not compressed yet -- blake2s_update's fill logic
         * (mirroring the reference implementation) only compresses a block
         * once it knows a FOLLOWING byte exists, so a key-only, zero-length
         * message correctly compresses the key block as the single FINAL
         * block in blake2s_final, with t == 64, rather than as an
         * intermediate block with t == 64 followed by a phantom empty final
         * block at t == 64 again (which would be a second, wrong, path to
         * the same t value). */
        uint8_t kb[BLAKE2S_BLOCKBYTES];
        memset(kb, 0, sizeof(kb));
        memcpy(kb, key, keylen);
        blake2s_update(ctx, kb, sizeof(kb));
        memset(kb, 0, sizeof(kb));   /* key material off the stack */
    }
}

void blake2s_update(struct blake2s *ctx, const void *in, size_t inlen)
{
    const uint8_t *p = (const uint8_t *)in;

    if (inlen > 0) {
        size_t left = ctx->buflen;
        size_t fill = BLAKE2S_BLOCKBYTES - left;

        if (inlen > fill) {
            memcpy(ctx->buf + left, p, fill);
            increment_counter(ctx, BLAKE2S_BLOCKBYTES);
            blake2s_compress(ctx, ctx->buf, 0);
            ctx->buflen = 0;
            p += fill; inlen -= fill;

            /* Strictly greater-than: a message that is an exact multiple of
             * the block size must leave its LAST block sitting in ctx->buf
             * un-compressed, because only blake2s_final knows whether it is
             * really the last one (f0) -- another update() call could still
             * arrive. Compressing early here (`>=` instead of `>`) is a
             * fencepost bug invisible until a caller streams input in a
             * chunking that lands exactly on 64 bytes, which the multi-block
             * negative control below reproduces on purpose. */
            while (inlen > BLAKE2S_BLOCKBYTES) {
                increment_counter(ctx, BLAKE2S_BLOCKBYTES);
                blake2s_compress(ctx, p, 0);
                p += BLAKE2S_BLOCKBYTES; inlen -= BLAKE2S_BLOCKBYTES;
            }
        }
        memcpy(ctx->buf + ctx->buflen, p, inlen);
        ctx->buflen += inlen;
    }
}

/* THE THIRD TRAP NAMED IN THE BRIEF: t is incremented by ctx->buflen -- the
 * number of REAL bytes in the final block -- not by 64. The zero padding
 * added below is not "extra input", it is alignment filler the spec defines
 * as not counted; an implementation that always adds 64 here produces the
 * right digest only when the message happens to end on a block boundary,
 * which is exactly the case the whole-blocks half of the KAT corpus (inputs
 * of length 0, 64, 128, ...) would NOT catch since a whole-block message's
 * final chunk is also short of the buffer only when total length mod 64 != 0
 * -- so the corpus's 0-length AND partial-length vectors are what this must
 * survive, and the 256-vector KAT below runs every length 0..255. */
void blake2s_final(struct blake2s *ctx, uint8_t *out)
{
    uint8_t outbuf[BLAKE2S_OUTBYTES];
    int i;

    increment_counter(ctx, (uint32_t)ctx->buflen);
    memset(ctx->buf + ctx->buflen, 0, BLAKE2S_BLOCKBYTES - ctx->buflen);
    blake2s_compress(ctx, ctx->buf, 1);

    for (i = 0; i < 8; i++) store32_le(outbuf + 4 * i, ctx->h[i]);
    memcpy(out, outbuf, ctx->outlen);
    memset(outbuf, 0, sizeof(outbuf));
}

void blake2s(const void *in, size_t inlen, const void *key, size_t keylen,
             uint8_t *out, size_t outlen)
{
    struct blake2s ctx;
    if (keylen > 0) blake2s_init_key(&ctx, outlen, key, keylen);
    else            blake2s_init(&ctx, outlen);
    blake2s_update(&ctx, in, inlen);
    blake2s_final(&ctx, out);
}
