/* c/lib/video/mpeg4_bits.h -- the bit reader for MPEG-4 Part 2 / H.263.
 *
 * Two things this reader must do that H.264's bs.h does not, and they are why
 * it is a separate file rather than a reuse:
 *
 *   1. SHOW without consuming, past the end of the buffer, returning zeros.
 *      Every resync-marker probe in this format is "look at the next 16 bits
 *      and decide", and a probe that fails at the end of a picture is the
 *      normal case, not an error. A reader that sets a sticky error on a peek
 *      would make the last macroblock of every picture unreachable.
 *   2. Decode a prefix code one bit at a time. The RVLC tables run to 15 bits
 *      and a materialised lookup would cost 64 KiB of rodata in a ring-3
 *      binary; this decoder is held to bit-exactness, not to throughput.
 *
 * `overrun` is sticky and counts only bits actually CONSUMED past the end.
 */
#ifndef LOGIT_MPEG4_BITS_H
#define LOGIT_MPEG4_BITS_H

#include <stdint.h>
#include "mpeg4_tables.h"

typedef struct {
    const uint8_t *data;
    int nbits;        /* size of the buffer in bits */
    int pos;          /* absolute bit position */
    int overrun;      /* sticky: a read consumed bits past nbits */
} m4bits;

static inline void m4b_init(m4bits *b, const uint8_t *data, int nbytes)
{
    b->data = data; b->nbits = nbytes * 8; b->pos = 0; b->overrun = 0;
}

static inline int  m4b_count(const m4bits *b) { return b->pos; }
static inline int  m4b_left(const m4bits *b)  { return b->nbits - b->pos; }
static inline int  m4b_over(const m4bits *b)  { return b->overrun; }

/* Peek n bits (0..24) from an arbitrary position, zero-padded past the end. */
static inline uint32_t m4b_show_at(const m4bits *b, int at, int n)
{
    uint32_t v = 0;
    int i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        int p = at + i, bit = 0;
        if (p >= 0 && p < b->nbits)
            bit = (b->data[p >> 3] >> (7 - (p & 7))) & 1;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static inline uint32_t m4b_show(const m4bits *b, int n)
{
    return m4b_show_at(b, b->pos, n);
}

static inline void m4b_skip(m4bits *b, int n)
{
    b->pos += n;
    if (b->pos > b->nbits) b->overrun = 1;
}

static inline uint32_t m4b_u(m4bits *b, int n)
{
    uint32_t v = m4b_show(b, n);
    m4b_skip(b, n);
    return v;
}

static inline uint32_t m4b_u1(m4bits *b) { return m4b_u(b, 1); }

/* n bits read as a two's-complement signed value (H.263 / MPEG-4 "get_sbits"). */
static inline int32_t m4b_s(m4bits *b, int n)
{
    uint32_t v = m4b_u(b, n);
    if (n <= 0) return 0;
    if (v & (1u << (n - 1))) return (int32_t)(v | ~((1u << n) - 1));
    return (int32_t)v;
}

/* MPEG-4 intra-DC "get_xbits": n bits, where a leading 0 means the value is
 * negative and the magnitude is the complement (14496-2 Table B-14 note). */
static inline int32_t m4b_xbits(m4bits *b, int n)
{
    uint32_t v = m4b_u(b, n);
    if (n <= 0) return 0;
    if (v & (1u << (n - 1))) return (int32_t)v;
    return (int32_t)v - (int32_t)(1u << n) + 1;
}

static inline int m4b_aligned(const m4bits *b) { return (b->pos & 7) == 0; }
static inline void m4b_align(m4bits *b)
{
    if (b->pos & 7) m4b_skip(b, 8 - (b->pos & 7));
}

/* Decode one codeword of a prefix code. Returns the symbol, or -1 if no
 * codeword matches (which for a Kraft-incomplete table is a real possibility
 * and always means the bitstream is corrupt). */
static inline int m4b_vlc(m4bits *b, const m4_vlc *v)
{
    uint32_t code = 0;
    int len;
    if (m4b_left(b) <= 0) { b->overrun = 1; return -1; }
    for (len = 1; len <= v->maxlen; len++) {
        code = (code << 1) | m4b_show_at(b, b->pos + len - 1, 1);
        if (len >= v->minlen && v->cnt[len]) {
            /* binary search inside the block of `len`-bit codewords */
            int lo = v->off[len], hi = v->off[len] + v->cnt[len] - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                if (v->code[mid] == code) {
                    m4b_skip(b, len);
                    return v->sym[mid];
                }
                if (v->code[mid] < code) lo = mid + 1; else hi = mid - 1;
            }
        }
    }
    return -1;
}

#endif /* LOGIT_MPEG4_BITS_H */
