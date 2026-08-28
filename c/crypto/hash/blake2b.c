#include "blake2b.h"

/* RFC 7693 BLAKE2b. No memcpy/memset extern -- CLAUDE.md's host-reality table
 * names Apple's fortified <string.h> turning memset/memcpy into macros at
 * every -O level as the single most repeated way a c/crypto file has failed
 * to compile on this host; c/crypto/pq/keccak.c dodges the whole class by
 * writing the byte loops out, and this file does the same. */

static void bmemzero(void *p, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static void bmemcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
}

static const uint64_t blake2b_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

/* The message-word permutation for each of the 12 rounds (RFC 7693 SS 2.7).
 * Rounds 10 and 11 repeat rounds 0 and 1 -- that repetition is in the spec,
 * not a copy-paste mistake here. */
static const uint8_t blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
};

/* Rotate right, 64-bit. Written as shift-or, never a variable-count barrel
 * shift keyed on secret data -- there is no such thing here anyway, every
 * rotation amount below is a compile-time constant from the spec, so this is
 * inherently branch-free and index-free regardless of what's being rotated. */
static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

/* Rotation amounts per RFC 7693 2.1: 32, 24, 16, 63.
 *
 * THE TRAP NAMED IN THE TASK: `x >> 63 | x << 63` is a one-character slip
 * from `x >> 63 | x << 1` (rotate-right-by-63 == rotate-left-by-1) that
 * still produces full avalanche and a digest that looks like a digest --
 * ROR64_63() below is the one call site that constant is allowed to appear,
 * spelled out with its own name so a diff on this file shows a 63 changing
 * to something else rather than a shift count buried in an expression. */
#ifdef BLAKE2B_BUG_ROT63
/* NEGATIVE CONTROL: -DBLAKE2B_BUG_ROT63 turns the last rotation of every G
 * call into `x >> 63 | x << 63` -- both operands rotate by 63 in the SAME
 * direction, which collapses to `x` unchanged for the `<< 63` term XORed
 * against a mis-shifted `>> 63` term; concretely it produces a different,
 * wrong, but still fixed-shape function. This is the exact one-character
 * defect the task names, wired behind a flag so it can be built and watched
 * failing rather than only described. */
static inline uint64_t ror64_63(uint64_t x) { return (x >> 63) | (x << 63); }
#else
static inline uint64_t ror64_63(uint64_t x) { return ror64(x, 63); }
#endif

#define G(v, a, b, c, d, x, y)                          \
    do {                                                \
        v[a] = v[a] + v[b] + (x);                       \
        v[d] = ror64(v[d] ^ v[a], 32);                   \
        v[c] = v[c] + v[d];                              \
        v[b] = ror64(v[b] ^ v[c], 24);                   \
        v[a] = v[a] + v[b] + (y);                        \
        v[d] = ror64(v[d] ^ v[a], 16);                   \
        v[c] = v[c] + v[d];                              \
        v[b] = ror64_63(v[b] ^ v[c]);                     \
    } while (0)

static uint64_t load_le64(const uint8_t *p)
{
    return  (uint64_t)p[0]        | (uint64_t)p[1] << 8  |
            (uint64_t)p[2] << 16  | (uint64_t)p[3] << 24 |
            (uint64_t)p[4] << 32  | (uint64_t)p[5] << 40 |
            (uint64_t)p[6] << 48  | (uint64_t)p[7] << 56;
}

static void store_le64(uint8_t *p, uint64_t x)
{
    p[0] = (uint8_t)(x);       p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
    p[4] = (uint8_t)(x >> 32); p[5] = (uint8_t)(x >> 40);
    p[6] = (uint8_t)(x >> 48); p[7] = (uint8_t)(x >> 56);
}

/* F() -- RFC 7693 3.2. `last` is 0 or 1; there is no tree mode here so f[1]
 * (the "last node" flag) is always left 0, exactly as the reference
 * sequential-mode implementation does. */
static void blake2b_compress(struct blake2b *c, const uint8_t block[BLAKE2B_BLOCKBYTES])
{
    uint64_t m[16];
    uint64_t v[16];
    int i;

    for (i = 0; i < 16; i++) m[i] = load_le64(block + i * 8);

    for (i = 0; i < 8; i++) v[i] = c->h[i];
    for (i = 0; i < 8; i++) v[8 + i] = blake2b_iv[i];

    v[12] ^= c->t[0];
    v[13] ^= c->t[1];
    v[14] ^= c->f[0];
    v[15] ^= c->f[1];

    for (i = 0; i < 12; i++) {
        const uint8_t *s = blake2b_sigma[i];
        G(v, 0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);
        G(v, 1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
        G(v, 2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
        G(v, 3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
        G(v, 0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);
        G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        G(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }

    for (i = 0; i < 8; i++) c->h[i] ^= v[i] ^ v[i + 8];
}

static void blake2b_increment_counter(struct blake2b *c, uint64_t inc)
{
    /* THE 128-BIT COUNTER, see blake2b.h. t0/t1 form one 128-bit value; t1
     * only ever moves on a carry out of t0, which no test in this gate can
     * reach (it needs 2^64 bytes of input). Written as an explicit carry
     * test rather than relying on unsigned wraparound semantics to make the
     * two-word intent visible at the call site. */
    uint64_t old = c->t[0];
    c->t[0] += inc;
    if (c->t[0] < old) c->t[1]++;
}

int blake2b_init(struct blake2b *c, size_t outlen, const void *key, size_t keylen)
{
    int i;

    if (outlen == 0 || outlen > BLAKE2B_OUTBYTES) return -1;
    if (keylen > BLAKE2B_KEYBYTES) return -1;

    bmemzero(c, sizeof(*c));
    for (i = 0; i < 8; i++) c->h[i] = blake2b_iv[i];

    /* Parameter block SS 2.5: in sequential mode with default fanout/depth,
     * every other parameter-block byte is 0, so XORing h[0] with the
     * (digest_length, key_length, fanout=1, depth=1) byte is the whole
     * parameterization -- RFC 7693 3.3's worked "abc" trace shows exactly
     * this value (0x01010000 ^ keylen<<8 ^ outlen) for the unkeyed case. */
    c->h[0] ^= 0x01010000ULL ^ ((uint64_t)keylen << 8) ^ (uint64_t)outlen;
    c->outlen = outlen;

    if (keylen > 0) {
        uint8_t block[BLAKE2B_BLOCKBYTES];
        bmemzero(block, sizeof(block));
        bmemcpy(block, key, keylen);
        blake2b_update(c, block, sizeof(block));
        bmemzero(block, sizeof(block));   /* key material: wipe before returning */
    }
    return 0;
}

void blake2b_update(struct blake2b *c, const void *data, size_t len)
{
    const uint8_t *in = (const uint8_t *)data;

    if (len == 0) return;

    {
        size_t left = c->buflen;
        size_t fill = BLAKE2B_BLOCKBYTES - left;

        if (len > fill) {
            bmemcpy(c->buf + left, in, fill);
            blake2b_increment_counter(c, BLAKE2B_BLOCKBYTES);
            blake2b_compress(c, c->buf);
            c->buflen = 0;
            in += fill;
            len -= fill;

            /* Keep at least one full block buffered until we know whether
             * more input is coming -- a block cannot be compressed as the
             * LAST block until final() sets the finalization flag, so the
             * final full block in a byte stream must never be consumed
             * here even when it exactly fills the buffer. */
            while (len > BLAKE2B_BLOCKBYTES) {
                blake2b_increment_counter(c, BLAKE2B_BLOCKBYTES);
                blake2b_compress(c, in);
                in += BLAKE2B_BLOCKBYTES;
                len -= BLAKE2B_BLOCKBYTES;
            }
        }
        bmemcpy(c->buf + c->buflen, in, len);
        c->buflen += len;
    }
}

void blake2b_final(struct blake2b *c, uint8_t *out)
{
    uint8_t digest[BLAKE2B_OUTBYTES];
    size_t i;

    blake2b_increment_counter(c, (uint64_t)c->buflen);
    c->f[0] = ~0ULL;   /* last block; no tree mode, so f[1] stays 0 */

    /* Zero-pad the tail. The branch here is on c->buflen, the PUBLIC total
     * message length so far modulo the block size -- not on any secret. */
    for (i = c->buflen; i < BLAKE2B_BLOCKBYTES; i++) c->buf[i] = 0;
    blake2b_compress(c, c->buf);

    for (i = 0; i < 8; i++) store_le64(digest + i * 8, c->h[i]);
    bmemcpy(out, digest, c->outlen);
    bmemzero(digest, sizeof(digest));
}

void blake2b512(const void *data, size_t len, uint8_t out[64])
{
    struct blake2b c;
    blake2b_init(&c, 64, 0, 0);
    blake2b_update(&c, data, len);
    blake2b_final(&c, out);
}

int blake2b_keyed(const void *data, size_t len, const void *key, size_t keylen,
                   uint8_t *out, size_t outlen)
{
    struct blake2b c;
    if (blake2b_init(&c, outlen, key, keylen) != 0) return -1;
    blake2b_update(&c, data, len);
    blake2b_final(&c, out);
    return 0;
}
