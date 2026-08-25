/* c/lib/audio/opus_range.h -- the Opus range decoder, RFC 6716 section 4.1.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT AND NOT PART OF opus_celt.c.  Two
 * reasons, and the second is the one that matters.
 *
 * The shallow one: SILK and CELT share it. RFC 6716 4.1 is the entropy layer
 * for the WHOLE codec, so a hybrid frame decodes SILK symbols and CELT symbols
 * out of ONE decoder instance, in that order, with the raw-bit window growing
 * downward from the end of the same buffer while the range coder walks up from
 * the start. Anything that put it inside one of the two mode decoders would
 * have to be reached from the other.
 *
 * The deep one: it is the only part of this codec that is EXACTLY specified
 * and therefore the only part that can be tested to equality. Everything above
 * it -- band energies, PVQ shapes, the MDCT -- is checked against a quality
 * metric, because RFC 6716 defines conformance by opus_compare and its own
 * reference decoder disagrees with itself between the float and fixed-point
 * builds (see the top of opus.c). This file does not get that latitude. Two
 * range decoders either produce the same symbol sequence or one of them is
 * broken, and `rng` after the last symbol of a frame is a 32-bit checksum of
 * every decision made along the way.
 *
 * THAT CHECKSUM IS THE SHARPEST INSTRUMENT IN THIS SUBSYSTEM AND THE TEST
 * VECTORS CARRY IT.  An opus_demo `.bit` file stores, per packet, the
 * ENCODER's `rng` at the end of that packet, next to the payload. So
 * `test-opus` does not have to infer from a waveform that the bitstream parse
 * was right: it compares a 32-bit integer per packet, and reports the COUNT of
 * packets that disagree. A count of 4 out of 2147 is a bisectable fact; "the
 * audio sounds wrong" is not. Nothing float touches that number, so it stays
 * meaningful on a machine whose FPU rounds differently.
 *
 * Everything here is exact integer arithmetic on uint32_t, ported from the
 * reference in the RFC's Appendix A with the structure kept recognisable
 * rather than tidied -- a reader comparing this against RFC 6716 4.1 should
 * be able to do it line by line, because that is the only review that can
 * catch an off-by-one in an arithmetic coder.
 *
 * SAFETY: like abits.h next door, a read past the end of the buffer returns
 * zero bytes rather than reading memory it does not own. Opus packets arrive
 * off the network. `ec_read_byte` returning 0 past the end is not a
 * defensive addition either -- it is what the RFC's decoder does, because a
 * well-formed stream legitimately reads a few bytes beyond what the encoder
 * wrote (the encoder stops emitting once the value is pinned down).
 */
#ifndef LOGIT_OPUS_RANGE_H
#define LOGIT_OPUS_RANGE_H

#include <stdint.h>

/* Fractional bit resolution used by every "how many bits have I used" query in
 * CELT: 3 => eighths of a bit. RFC 6716 calls this BITRES. */
#define OPUS_BITRES 3

typedef struct {
    const uint8_t *buf;
    uint32_t storage;    /* bytes available to the range coder AND raw bits */
    uint32_t end_offs;   /* raw bits consumed from the END, in bytes */
    uint32_t end_window; /* buffered raw bits */
    int      nend_bits;  /* valid bits in end_window */
    int      nbits_total;
    uint32_t offs;       /* next range-coder byte */
    uint32_t rng;
    uint32_t val;
    uint32_t ext;
    int      rem;
    int      error;      /* sticky */
} orange;

/* ilog: index of the highest set bit plus one; 0 for 0. Named after the
 * reference's EC_ILOG because every bit-accounting expression below is a
 * transcription of one that used it. */
int      orange_ilog(uint32_t v);

void     orange_init(orange *d, const uint8_t *buf, uint32_t storage);

/* The four primitive symbol reads of RFC 6716 4.1. `decode` returns the
 * cumulative-frequency slot and MUST be followed by `update`; the icdf and
 * bit_logp forms do both at once. */
unsigned orange_decode(orange *d, unsigned ft);
unsigned orange_decode_bin(orange *d, unsigned bits);
void     orange_update(orange *d, unsigned fl, unsigned fh, unsigned ft);
int      orange_bit_logp(orange *d, unsigned logp);
int      orange_icdf(orange *d, const unsigned char *icdf, unsigned ftb);
uint32_t orange_dec_uint(orange *d, uint32_t ft);

/* Raw bits, taken from the END of the buffer, moving backwards. They are NOT
 * range-coded: the encoder writes them there precisely so that a value with a
 * uniform distribution costs no more than its exact width. */
uint32_t orange_dec_bits(orange *d, unsigned bits);

/* Bits consumed so far, whole and in 1/8ths. Both over-report slightly and
 * deliberately -- every rounding error is in the positive direction -- because
 * the encoder computes the same number and the two must agree exactly for the
 * bit allocation to agree. */
static inline int orange_tell(const orange *d)
{
    return d->nbits_total - orange_ilog(d->rng);
}
uint32_t orange_tell_frac(const orange *d);

static inline int orange_error(const orange *d) { return d->error; }

#endif /* LOGIT_OPUS_RANGE_H */
