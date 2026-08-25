/* c/lib/audio/opus_range.c -- Opus range decoder, RFC 6716 section 4.1.
 * See opus_range.h for why this is a separate TU and why it is held to
 * equality rather than to a tolerance. */

#include "opus_range.h"

/* RFC 6716 4.1, the constants the reference calls EC_*. Spelled out rather
 * than folded into literals because every shift below is derived from them and
 * a reader checking this against the RFC needs the same names. */
#define SYM_BITS   8
#define CODE_BITS  32
#define SYM_MAX    ((1u << SYM_BITS) - 1u)          /* 255 */
#define CODE_TOP   ((uint32_t)1u << (CODE_BITS - 1))/* 2^31 */
#define CODE_BOT   (CODE_TOP >> SYM_BITS)           /* 2^23 */
#define CODE_EXTRA ((CODE_BITS - 2) % SYM_BITS + 1) /* 7 */

#define UINT_BITS  8   /* EC_UINT_BITS: split point in orange_dec_uint */

int orange_ilog(uint32_t v)
{
    /* The reference's branchless form, kept because it is also the definition
     * used by orange_tell(): "0 for 0" is load-bearing, since ec_tell() on a
     * fresh decoder must read 1 and not something undefined. */
    int ret, m;
    ret = !!v;
    m = !!(v & 0xFFFF0000u) << 4; v >>= m; ret |= m;
    m = !!(v & 0xFF00u)     << 3; v >>= m; ret |= m;
    m = !!(v & 0xF0u)       << 2; v >>= m; ret |= m;
    m = !!(v & 0x0Cu)       << 1; v >>= m; ret |= m;
    ret += !!(v & 0x02u);
    return ret;
}

/* Past the end reads as zero. NOT a hardening measure bolted on: a conformant
 * encoder stops emitting bytes as soon as the value is pinned down, so a
 * correct decoder routinely asks for bytes that were never written, and
 * refusing them would break valid streams. It is also what keeps a truncated
 * download inside its own buffer. */
static int read_byte(orange *d)
{
    return d->offs < d->storage ? d->buf[d->offs++] : 0;
}

static int read_byte_from_end(orange *d)
{
    return d->end_offs < d->storage ?
        d->buf[d->storage - ++(d->end_offs)] : 0;
}

static void normalize(orange *d)
{
    while (d->rng <= CODE_BOT) {
        int sym;
        d->nbits_total += SYM_BITS;
        d->rng <<= SYM_BITS;
        sym = d->rem;
        d->rem = read_byte(d);
        sym = (sym << SYM_BITS | d->rem) >> (SYM_BITS - CODE_EXTRA);
        d->val = ((d->val << SYM_BITS) + (SYM_MAX & ~(uint32_t)sym))
                 & (CODE_TOP - 1);
    }
}

void orange_init(orange *d, const uint8_t *buf, uint32_t storage)
{
    d->buf = buf;
    d->storage = storage;
    d->end_offs = 0;
    d->end_window = 0;
    d->nend_bits = 0;
    /* The offset ec_tell() subtracts partial bits from. After the normalize
     * below it equals the encoder's, which is the whole point: the two sides
     * must agree on "bits used so far" to the eighth of a bit. */
    d->nbits_total = CODE_BITS + 1
                     - ((CODE_BITS - CODE_EXTRA) / SYM_BITS) * SYM_BITS;
    d->offs = 0;
    d->rng = 1u << CODE_EXTRA;
    d->rem = read_byte(d);
    d->val = d->rng - 1 - ((uint32_t)d->rem >> (SYM_BITS - CODE_EXTRA));
    d->ext = 0;
    d->error = 0;
    normalize(d);
}

unsigned orange_decode(orange *d, unsigned ft)
{
    unsigned s;
    d->ext = d->rng / ft;
    s = (unsigned)(d->val / d->ext);
    return ft - (s + 1 < ft ? s + 1 : ft);
}

unsigned orange_decode_bin(orange *d, unsigned bits)
{
    unsigned s, top = 1u << bits;
    d->ext = d->rng >> bits;
    s = (unsigned)(d->val / d->ext);
    return top - (s + 1 < top ? s + 1 : top);
}

void orange_update(orange *d, unsigned fl, unsigned fh, unsigned ft)
{
    uint32_t s = d->ext * (uint32_t)(ft - fh);
    d->val -= s;
#ifdef OPUS_RANGE_NEGCTL_NORM
    /* THE NEGATIVE CONTROL, tests/opus.mk's test-opus-range-negctl.
     * RFC 6716 4.1.2 splits the new `rng` two ways: `ext*(fh-fl)` for every
     * symbol EXCEPT the one whose `fl` is 0, which instead gets `rng - s`.
     * The two are not interchangeable -- `ext = rng/ft` is a floor division,
     * so `ext*ft` generally loses a remainder `r = rng - ext*ft` (0<=r<ft),
     * and `rng - s` is what folds that remainder back in for the fl==0
     * symbol rather than dropping it on the floor. This control drops the
     * special case and always takes the `ext*(fh-fl)` branch -- PLAUSIBLE,
     * because it reads as "the general formula, applied uniformly" and is
     * byte-for-byte correct on the majority of symbols (every one with
     * fl>0), silently wrong only on the one whose fl==0. (An earlier
     * candidate for this control -- dropping the CODE_TOP-1 renormalization
     * mask on `val` -- was tried and rejected: this decoder's invariant
     * val<rng<=CODE_BOT going into every renormalization shift means the
     * masked bit is always already 0, so that bug is unobservable here and
     * would have been a control that can never be watched failing.) */
    d->rng = d->ext * (uint32_t)(fh - fl);
#else
    d->rng = fl > 0 ? d->ext * (uint32_t)(fh - fl) : d->rng - s;
#endif
    normalize(d);
}

int orange_bit_logp(orange *d, unsigned logp)
{
    uint32_t r = d->rng, v = d->val, s = r >> logp;
    int ret = v < s;
    if (!ret) d->val = v - s;
    d->rng = ret ? s : r - s;
    normalize(d);
    return ret;
}

int orange_icdf(orange *d, const unsigned char *icdf, unsigned ftb)
{
    uint32_t r, v, s, t;
    int ret;
    s = d->rng;
    v = d->val;
    r = s >> ftb;
    ret = -1;
    do {
        t = s;
        s = r * (uint32_t)icdf[++ret];
    } while (v < s);
    d->val = v - s;
    d->rng = t - s;
    normalize(d);
    return ret;
}

uint32_t orange_dec_uint(orange *d, uint32_t ft)
{
    unsigned s;
    int ftb;
    /* ft must be > 1; orange_ilog is undefined-by-convention at 0 in the
     * reference, and every caller here passes a count of at least 2. */
    if (ft <= 1) { d->error = 1; return 0; }
    ft--;
    ftb = orange_ilog(ft);
    if (ftb > UINT_BITS) {
        uint32_t t;
        unsigned f;
        ftb -= UINT_BITS;
        f = (unsigned)(ft >> ftb) + 1;
        s = orange_decode(d, f);
        orange_update(d, s, s + 1, f);
        t = ((uint32_t)s << ftb) | orange_dec_bits(d, (unsigned)ftb);
        if (t <= ft) return t;
        d->error = 1;
        return ft;
    }
    ft++;
    s = orange_decode(d, (unsigned)ft);
    orange_update(d, s, s + 1, (unsigned)ft);
    return s;
}

uint32_t orange_dec_bits(orange *d, unsigned bits)
{
    uint32_t window = d->end_window, ret;
    int available = d->nend_bits;
    if ((unsigned)available < bits) {
        do {
            window |= (uint32_t)read_byte_from_end(d) << available;
            available += SYM_BITS;
        } while (available <= (int)(sizeof(uint32_t) * 8) - SYM_BITS);
    }
    ret = window & (((uint32_t)1u << bits) - 1u);
    window >>= bits;
    available -= (int)bits;
    d->end_window = window;
    d->nend_bits = available;
    d->nbits_total += (int)bits;
    return ret;
}

uint32_t orange_tell_frac(const orange *d)
{
    /* Squares `rng` BITRES times to recover the fractional part of
     * log2(rng). Integer only, and the reference's comment is worth keeping:
     * the answer is always slightly LARGER than the true value, so a symbol
     * coded with probability 2^-n can never appear to have cost less than n
     * bits -- which is what makes the encoder's and decoder's independently
     * computed budgets agree instead of drifting apart by a fraction. */
    uint32_t nbits, r;
    int l, i;
    nbits = (uint32_t)d->nbits_total << OPUS_BITRES;
    l = orange_ilog(d->rng);
    r = d->rng >> (l - 16);
    for (i = OPUS_BITRES; i-- > 0; ) {
        int b;
        r = r * r >> 15;
        b = (int)(r >> 16);
        l = l << 1 | b;
        r >>= b;
    }
    return nbits - (uint32_t)l;
}
