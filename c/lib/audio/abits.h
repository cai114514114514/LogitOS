/* c/lib/audio/abits.h -- MSB-first bit reader shared by the audio decoders.
 *
 * MP3 and FLAC both pack their syntax elements most-significant-bit first into
 * a byte stream, so one reader serves both. It is deliberately the same shape
 * as c/lib/video/bs.h: header-only, no allocation, and every read past the end
 * of the buffer returns zeros and sets a sticky error rather than reading
 * memory it does not own. Audio files arrive off the network, so "the caller
 * checked the length first" is not a safety argument -- the reader itself must
 * never step outside [data, data+len).
 *
 * The error flag is sticky on purpose. A decoder checks it at frame or block
 * boundaries instead of after every field, which keeps the parsers readable
 * without letting a truncated file walk off the end: once tripped, every
 * subsequent read is a no-op returning 0.
 */
#ifndef LOGIT_ABITS_H
#define LOGIT_ABITS_H

#include <stdint.h>

typedef struct {
    const uint8_t *data;
    long len;        /* bytes */
    long bitpos;     /* absolute bit position */
    int  error;      /* sticky: any read that ran past the end */
} abits;

static inline void ab_init(abits *b, const uint8_t *data, long len)
{
    b->data = data; b->len = len; b->bitpos = 0; b->error = 0;
}

static inline int  ab_error(const abits *b)     { return b->error; }
static inline long ab_left(const abits *b)      { return b->len * 8 - b->bitpos; }
static inline long ab_pos(const abits *b)       { return b->bitpos; }
static inline void ab_seek(abits *b, long bit)
{
    if (bit < 0 || bit > b->len * 8) { b->error = 1; return; }
    b->bitpos = bit;
}

/* Byte-align; returns the number of bits skipped. */
static inline int ab_align(abits *b)
{
    int n = (int)((8 - (b->bitpos & 7)) & 7);
    b->bitpos += n;
    return n;
}

/* Read n bits (0..32), MSB first. */
static inline uint32_t ab_u(abits *b, int n)
{
    if (n < 0 || n > 32) { b->error = 1; return 0; }
    if (n == 0) return 0;
    if (b->error || ab_left(b) < n) { b->error = 1; return 0; }
    uint32_t v = 0;
    while (n > 0) {
        long byte = b->bitpos >> 3;
        int  bit  = 7 - (int)(b->bitpos & 7);   /* index of next bit in byte */
        int  take = bit + 1 < n ? bit + 1 : n;
        v = (v << take) |
            ((uint32_t)(b->data[byte] >> (bit + 1 - take)) & ((1u << take) - 1u));
        b->bitpos += take;
        n -= take;
    }
    return v;
}

static inline uint32_t ab_u1(abits *b) { return ab_u(b, 1); }

/* Read n bits (0..64). FLAC's rice escape and UTF-8 frame numbers need >32. */
static inline uint64_t ab_u64(abits *b, int n)
{
    if (n < 0 || n > 64) { b->error = 1; return 0; }
    if (n <= 32) return ab_u(b, n);
    uint64_t hi = ab_u(b, n - 32);
    uint64_t lo = ab_u(b, 32);
    return (hi << 32) | lo;
}

/* Two's-complement signed field of n bits (1..32). */
static inline int32_t ab_s(abits *b, int n)
{
    if (n <= 0 || n > 32) { b->error = 1; return 0; }
    uint32_t v = ab_u(b, n);
    if (n == 32) return (int32_t)v;
    /* Sign-extend through unsigned arithmetic: shifting a negative signed
     * value left is undefined, and this decoder is fuzzed under UBSan. */
    uint32_t sign = 1u << (n - 1);
    if (v & sign) return (int32_t)(v | ~((1u << n) - 1u));
    return (int32_t)v;
}

/* Peek n bits without consuming. Past the end reads as zero bits and does NOT
 * set the error flag: Huffman decoding legitimately peeks a full codeword
 * width at the tail of a block where fewer bits remain. */
static inline uint32_t ab_peek(const abits *b, int n)
{
    if (n <= 0 || n > 32) return 0;
    uint32_t v = 0;
    long pos = b->bitpos;
    for (int i = 0; i < n; i++, pos++) {
        long byte = pos >> 3;
        uint32_t bit = 0;
        if (byte < b->len) bit = (uint32_t)(b->data[byte] >> (7 - (pos & 7))) & 1u;
        v = (v << 1) | bit;
    }
    return v;
}

/* Skip n bits. Unlike ab_u this accepts any n >= 0. */
static inline void ab_skip(abits *b, long n)
{
    if (n < 0 || b->error || ab_left(b) < n) { b->error = 1; return; }
    b->bitpos += n;
}

/* Unary: count zero bits up to and including the terminating one bit, and
 * return the count of zeros. `limit` bounds the run so a corrupt file cannot
 * make the caller loop for the length of the buffer with a useless result;
 * exceeding it sets the error flag. */
static inline uint32_t ab_unary(abits *b, uint32_t limit)
{
    uint32_t n = 0;
    while (!b->error) {
        if (ab_left(b) < 1) { b->error = 1; return 0; }
        if (ab_u1(b)) return n;
        if (++n > limit) { b->error = 1; return 0; }
    }
    return 0;
}

#endif /* LOGIT_ABITS_H */
