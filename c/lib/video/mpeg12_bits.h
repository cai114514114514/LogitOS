/* c/lib/video/mpeg12_bits.h -- the bit reader for MPEG-1/2 video.
 *
 * Not bs.h: that reader is H.264's, its more_rbsp_data() and Exp-Golomb are
 * meaningless here, and MPEG-1/2 needs two things it does not offer -- a
 * non-consuming peek of up to 24 bits (the end of a slice is "the next 23 bits
 * are zero", and every VLC lookup peeks nine before it knows how many it will
 * take) and a hard limit that is the byte BEFORE the next start code.
 *
 * Reading past the limit yields zero bits and sets `over` once. The zeros are
 * not a guess: a caller that peeks past the end is about to find no valid code
 * there (the twelve-zero pattern is reserved in both coefficient tables --
 * see tools/gen_mpeg12_tables.py) or is running the end-of-slice test, which
 * wants exactly that answer. `over` is what turns it into an error at the
 * next parse boundary.
 */
#ifndef LOGIT_MPEG12_BITS_H
#define LOGIT_MPEG12_BITS_H

#include <stdint.h>

typedef struct {
    const uint8_t *data;
    int len;        /* bytes available */
    int pos;        /* absolute bit position */
    int over;       /* sticky: a read ran past the end */
} m12br;

static inline void m12_br_init(m12br *b, const uint8_t *d, int len)
{
    b->data = d; b->len = len; b->pos = 0; b->over = 0;
}

static inline int m12_left(const m12br *b) { return b->len * 8 - b->pos; }

/* Peek n bits (1..24) without consuming; zero-padded past the end. */
static inline uint32_t m12_peek(const m12br *b, int n)
{
    uint32_t v = 0;
    int byte = b->pos >> 3, bit = b->pos & 7, i;
    /* four bytes cover any 24-bit field at any bit offset */
    for (i = 0; i < 4; i++)
        v = (v << 8) | (byte + i < b->len ? b->data[byte + i] : 0u);
    return (v >> (32 - bit - n)) & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
}

static inline void m12_skip(m12br *b, int n)
{
    b->pos += n;
    if (b->pos > b->len * 8) b->over = 1;
}

static inline uint32_t m12_u(m12br *b, int n)
{
    uint32_t v = m12_peek(b, n);
    m12_skip(b, n);
    return v;
}

static inline uint32_t m12_u1(m12br *b) { return m12_u(b, 1); }

/* n bits, two's complement. */
static inline int32_t m12_s(m12br *b, int n)
{
    uint32_t v = m12_u(b, n);
    return (int32_t)(v << (32 - n)) >> (32 - n);
}

static inline void m12_align(m12br *b) { b->pos = (b->pos + 7) & ~7; }

#endif /* LOGIT_MPEG12_BITS_H */
