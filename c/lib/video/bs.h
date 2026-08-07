/* c/lib/video/bs.h -- bitstream reader for H.264 RBSP data.
 *
 * The reader works on an RBSP buffer (emulation-prevention bytes already
 * removed by the NAL layer). Every read past the end returns 0 bits and
 * sets bs->error -- callers check bs_error() at parse boundaries; no read
 * ever touches memory outside [data, data+len). Header-only, no allocation.
 */
#ifndef LOGIT_BS_H
#define LOGIT_BS_H

#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    int len;          /* bytes */
    int bitpos;       /* absolute bit position */
    int error;        /* sticky: any out-of-range read */
} bs_t;

static inline void bs_init(bs_t *bs, const uint8_t *data, int len)
{
    bs->data = data; bs->len = len; bs->bitpos = 0; bs->error = 0;
}

static inline int bs_error(const bs_t *bs) { return bs->error; }
static inline int bs_bits_left(const bs_t *bs) { return bs->len * 8 - bs->bitpos; }

/* more_rbsp_data(): 1 if bits remain beyond the rbsp trailing pattern
 * (a single stop '1' followed by zero bits to the end of the buffer). */
static inline int bs_more_rbsp_data(const bs_t *bs)
{
    if (bs->error) return 0;
    for (int i = bs->len - 1; i >= 0; i--) {
        if (bs->data[i]) {
            /* rbsp_stop_one_bit is the LAST one in the RBSP, i.e. the LOWEST
             * set bit of the last non-zero byte -- not its highest. The two
             * coincide when the byte is 0x80, which is the common case and is
             * why looking for the highest bit mostly worked: on a trailing byte
             * like 0b10110000 it puts the stop bit three positions too early,
             * the slice loop believes the data ran out, and the macroblocks at
             * the end of that slice are never decoded at all. */
            int h = 0;
            while (!((bs->data[i] >> h) & 1)) h++;
            int last = i * 8 + (7 - h);   /* absolute bit idx of the stop bit */
            return bs->bitpos < last;
        }
    }
    return 0;
}

/* Read n bits (0..32). */
static inline uint32_t bs_u(bs_t *bs, int n)
{
    if (n < 0 || n > 32 || bs_bits_left(bs) < n) { bs->error = 1; return 0; }
    uint32_t v = 0;
    while (n > 0) {
        int byte = bs->bitpos >> 3, bit = 7 - (bs->bitpos & 7);
        int take = bit + 1 < n ? bit + 1 : n;
        v = (v << take) | ((uint32_t)(bs->data[byte] >> (bit + 1 - take)) & ((1u << take) - 1));
        bs->bitpos += take; n -= take;
    }
    return v;
}

static inline uint32_t bs_u1(bs_t *bs) { return bs_u(bs, 1); }

/* Unsigned Exp-Golomb (spec 9.1). Guards against pathological leading-zero
 * runs: code_num is at most 2^32-1, so > 31 leading zeros is corrupt. */
static inline uint32_t bs_ue(bs_t *bs)
{
    int zeros = 0;
    while (bs_bits_left(bs) > 0 && zeros < 32) {
        if (bs_u1(bs)) {
            if (zeros == 0) return 0;
            return (1u << zeros) - 1 + bs_u(bs, zeros);
        }
        zeros++;
    }
    bs->error = 1; return 0;
}

/* Signed Exp-Golomb. */
static inline int32_t bs_se(bs_t *bs)
{
    uint32_t k = bs_ue(bs);
    if (bs->error) return 0;
    return (int32_t)((k + 1) >> 1) * ((k & 1) ? 1 : -1);
}

/* Mapped Exp-Golomb (cbp intra only; tables live with the caller). */
static inline uint32_t bs_me(bs_t *bs, const uint8_t *map, int maplen)
{
    uint32_t k = bs_ue(bs);
    if (bs->error || k >= (uint32_t)maplen) { bs->error = 1; return 0; }
    return map[k];
}

/* Truncated Exp-Golomb (only used with range <= 1, i.e. a single bit). */
static inline uint32_t bs_te(bs_t *bs, uint32_t range)
{
    if (range > 1) return bs_ue(bs);
    return bs_u1(bs) ^ 1;
}

static inline int bs_byte_aligned(const bs_t *bs) { return (bs->bitpos & 7) == 0; }

static inline void bs_align(bs_t *bs)
{
    if (bs->bitpos & 7) bs->bitpos += 8 - (bs->bitpos & 7);
}

#endif /* LOGIT_BS_H */
