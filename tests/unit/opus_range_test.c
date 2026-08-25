/* tests/unit/opus_range_test.c -- the Opus range decoder against RFC 6716 4.1.
 *
 * THIS FILE WAS MUTE UNTIL NOW, AND THAT IS THE THING WORTH RECORDING, NOT
 * JUST FIXING. The version this replaces defined `rfcdec` without a field its
 * own `rfc_update()` referenced (`d->rem_shadow`, which does not exist on the
 * struct), had a comment admitting as much directly above the broken function
 * ("the collision above is real ... rfc_update is rewritten below to use it"
 * -- there is no rewrite below), and `main()` printed a banner and returned 0
 * without calling a single `ck`/`ckv`. It could not fail. That is exactly the
 * MUTE category CLAUDE.md's test-suite section names ("computes a verdict and
 * exits 0 anyway") and argues should be an EMPTY list -- this file was a
 * silent member of it, reachable from `make test-opus-range` once that target
 * existed, and would have reported green forever regardless of what
 * `opus_range.c` did. It is fixed by actually running the lockstep below, not
 * by routing around it.
 *
 * WHY THIS TEST IS A SECOND IMPLEMENTATION AND NOT A TABLE OF EXPECTED
 * NUMBERS.  The obvious shape for this file would be a list of (input bytes
 * -> expected symbols) captured from a run. That records what the code did on
 * the day it was written, which is precisely the thing under suspicion. RFC
 * 6716 section 4.1 does not publish worked values either -- it publishes the
 * EQUATIONS (4.1.1 initialization, 4.1.2 decoding and its renormalization,
 * 4.1.3 the three alternate forms, 4.1.4 raw bits, 4.1.5 uniform integers).
 * So the reference this file checks against is those equations, transcribed
 * here directly, in the RFC's own algebraic form:
 *
 *      fs  = ft - min(val/(rng/ft) + 1, ft)                      (4.1.2)
 *      val = val - (rng/ft)*(ft - fh[k])                         (4.1.2)
 *      rng = (rng/ft)*(fh[k] - fl[k])       if fl[k] > 0         (4.1.2)
 *      rng = rng - (rng/ft)*(ft - fh[k])    otherwise            (4.1.2)
 *
 * `rfc_dec_uint` and `rfc_dec_bits` are built ON TOP of the transcribed
 * primitives above (decode/update/raw-bits) rather than copied from
 * `orange_dec_uint`/`orange_dec_bits` -- 4.1.5 and 4.1.4 define them
 * compositionally, as a caller of the lower primitives, so writing them the
 * same way here is following the RFC's own structure rather than mirroring
 * `opus_range.c`'s C. Two implementations written from the equations and
 * stepped in lockstep catch a transcription error; one implementation and a
 * captured table cannot.
 *
 * WHAT THE LOCKSTEP RUNS ON.  Pseudo-random bytes, deliberately -- a range
 * decoder has no notion of a malformed input (every byte string decodes to
 * SOMETHING), so random data exercises exactly the same paths a real stream
 * does, plus the ones a real encoder never emits. Which primitive to call
 * next (decode/decode_bin/bit_logp/icdf/dec_uint/dec_bits) and its parameters
 * are drawn from the same PRNG driving the buffer, and both decoders are
 * fed the IDENTICAL sequence of calls -- so a single disagreement anywhere
 * (returned symbol, `val`, `rng`, `tell`, `tell_frac`) is caught at the next
 * comparison rather than possibly self-correcting.
 *
 * THE NEGATIVE CONTROL lives in `opus_range.c` itself, behind
 * `#ifdef OPUS_RANGE_NEGCTL_NORM`: `orange_update()` drops RFC 6716 4.1.2's
 * special case for the symbol whose `fl` is 0 (`rng = rng - s`, which folds
 * back the remainder `ext*ft` loses to floor division) and always takes the
 * general `rng = ext*(fh-fl)` branch instead -- correct for every symbol
 * except that one. See the comment at the `#ifdef` for why this replaced an
 * earlier candidate (dropping the renormalization mask on `val`) that turned
 * out to be UNOBSERVABLE given this decoder's own invariants -- a control
 * that cannot be watched failing is worse than no control, so it was not
 * shipped. `tests/opus.mk` builds this file against the `-DOPUS_RANGE_NEGCTL_NORM`
 * build and requires it to fail; see that file for the exact count.
 *
 * THE STRONGEST CHECK ON THIS FILE IS NOT HERE AT ALL: `make test-opus`
 * compares a 32-bit range-coder checksum against the ENCODER's, per packet,
 * over all 12 official RFC 6716 test vectors -- 6,886 packets across the 3
 * pure-CELT vectors alone, `rng_mismatch=0` on every one of the 12 measured
 * 2026-08-24. This file is what would localise a failure if that count ever
 * goes nonzero; it could not do that job while it was MUTE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "opus_range.h"

static int failures;
static int checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void ckv(unsigned long got, unsigned long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL: %s: got %lu want %lu\n", what, got, want);
    }
}

/* ======================================================================== */
/* A decoder written from RFC 6716 section 4.1's equations, independently of */
/* opus_range.c's C -- see the file header for why dec_uint/dec_bits are     */
/* built on top of the primitives here rather than transcribed separately.  */
/* ======================================================================== */

#define SYM_BITS   8
#define CODE_BITS  32
#define SYM_MAX    ((1u << SYM_BITS) - 1u)
#define CODE_TOP   ((uint32_t)1u << (CODE_BITS - 1))   /* 2^31 */
#define CODE_BOT   (CODE_TOP >> SYM_BITS)               /* 2^23 */
#define CODE_EXTRA ((CODE_BITS - 2) % SYM_BITS + 1)      /* 7 */
#define UINT_BITS  8

typedef struct {
    const unsigned char *buf;
    unsigned storage;
    unsigned offs;        /* next byte for the range coder */
    unsigned end_offs;    /* bytes consumed from the end, for raw bits */
    unsigned end_window;
    int      nend_bits;
    int      nbits_total;
    uint32_t val;
    uint32_t rng;
    uint32_t ext;          /* rng/ft (or rng>>bits), decode() to update() */
    int      rem;          /* one-byte renormalization lookahead, 4.1.1 */
} rfcdec;

/* Deliberately a plain bit-count loop rather than the branchless form in
 * opus_range.c -- same definition (0 for 0, else highest set bit + 1),
 * different code, so an ilog transcription bug on either side is visible. */
static int rfc_ilog(uint32_t v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static int rfc_byte(rfcdec *d)
{
    /* 4.1.1: "if no more input bytes remain, it uses zero bits". */
    return d->offs < d->storage ? d->buf[d->offs++] : 0;
}

static int rfc_byte_end(rfcdec *d)
{
    return d->end_offs < d->storage ?
        d->buf[d->storage - ++(d->end_offs)] : 0;
}

/* 4.1.2.1 Renormalization: while rng <= 2**23, shift in 8 more bits. */
static void rfc_norm(rfcdec *d)
{
    while (d->rng <= CODE_BOT) {
        int sym;
        d->nbits_total += SYM_BITS;
        d->rng <<= SYM_BITS;
        sym = d->rem;
        d->rem = rfc_byte(d);
        sym = (sym << SYM_BITS | d->rem) >> (SYM_BITS - CODE_EXTRA);
        d->val = ((d->val << SYM_BITS) + (SYM_MAX & ~(uint32_t)sym))
                 & (CODE_TOP - 1);
    }
}

static void rfc_init(rfcdec *d, const unsigned char *buf, unsigned storage)
{
    memset(d, 0, sizeof *d);
    d->buf = buf;
    d->storage = storage;
    /* 4.1.1: rng = 128, val = 127 - (b0>>1), then renormalize. */
    d->nbits_total = CODE_BITS + 1
                    - ((CODE_BITS - CODE_EXTRA) / SYM_BITS) * SYM_BITS;
    d->rng = 1u << CODE_EXTRA;
    d->rem = rfc_byte(d);
    d->val = d->rng - 1 - ((uint32_t)d->rem >> (SYM_BITS - CODE_EXTRA));
    rfc_norm(d);
}

/* 4.1.2: fs = ft - min(val/(rng/ft) + 1, ft) */
static unsigned rfc_decode(rfcdec *d, unsigned ft)
{
    unsigned s;
    d->ext = d->rng / ft;
    s = d->val / d->ext;
    return ft - (s + 1 < ft ? s + 1 : ft);
}

/* 4.1.3.1: the power-of-two-total special case, ext = rng >> bits. */
static unsigned rfc_decode_bin(rfcdec *d, unsigned bits)
{
    unsigned s, top = 1u << bits;
    d->ext = d->rng >> bits;
    s = d->val / d->ext;
    return top - (s + 1 < top ? s + 1 : top);
}

static void rfc_update(rfcdec *d, unsigned fl, unsigned fh, unsigned ft)
{
    uint32_t s = d->ext * (uint32_t)(ft - fh);
    d->val -= s;
    d->rng = fl > 0 ? d->ext * (uint32_t)(fh - fl) : d->rng - s;
    rfc_norm(d);
}

/* 4.1.3.2: the boolean/logp shortcut -- decode and update in one call. */
static int rfc_bit_logp(rfcdec *d, unsigned logp)
{
    uint32_t r = d->rng, v = d->val, s = r >> logp;
    int ret = v < s;
    if (!ret) d->val = v - s;
    d->rng = ret ? s : r - s;
    rfc_norm(d);
    return ret;
}

/* 4.1.3.3: the icdf table shortcut. icdf is a decreasing sequence ending
 * in 0; ftb is log2(ft). */
static int rfc_icdf(rfcdec *d, const unsigned char *icdf, unsigned ftb)
{
    uint32_t r = d->rng, v = d->val, s, t;
    int ret = -1;
    r >>= ftb;
    do {
        t = s = r * (uint32_t)icdf[++ret];
    } while (v < s);
    (void)t;
    d->val = v - s;
    d->rng = (ret == 0 ? d->rng : r * (uint32_t)icdf[ret - 1]) - s;
    rfc_norm(d);
    return ret;
}

/* 4.1.4: raw bits from the END of the buffer, moving backwards. */
static uint32_t rfc_dec_bits(rfcdec *d, unsigned bits)
{
    uint32_t window = d->end_window, ret;
    int available = d->nend_bits;
    if ((unsigned)available < bits) {
        do {
            window |= (uint32_t)rfc_byte_end(d) << available;
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

/* 4.1.5: uniformly distributed integers in [0, ft), built compositionally
 * from decode/update/dec_bits above -- exactly as the RFC defines it, and
 * NOT a copy of orange_dec_uint's C. */
static uint32_t rfc_dec_uint(rfcdec *d, uint32_t ft)
{
    unsigned s;
    int ftb;
    if (ft <= 1) return 0;
    ft--;
    ftb = rfc_ilog(ft);
    if (ftb > UINT_BITS) {
        uint32_t t;
        unsigned f;
        ftb -= UINT_BITS;
        f = (unsigned)(ft >> ftb) + 1;
        s = rfc_decode(d, f);
        rfc_update(d, s, s + 1, f);
        t = ((uint32_t)s << ftb) | rfc_dec_bits(d, (unsigned)ftb);
        if (t <= ft) return t;
        return ft;
    }
    ft++;
    s = rfc_decode(d, (unsigned)ft);
    rfc_update(d, s, s + 1, (unsigned)ft);
    return s;
}

static int rfc_tell(const rfcdec *d)
{
    return d->nbits_total - rfc_ilog(d->rng);
}

static uint32_t rfc_tell_frac(const rfcdec *d)
{
    uint32_t nbits, r;
    int l, i;
    nbits = (uint32_t)d->nbits_total << OPUS_BITRES;
    l = rfc_ilog(d->rng);
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

/* ======================================================================== */
/* Lockstep driver.                                                          */
/* ======================================================================== */

static uint32_t xrand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void run_lockstep(const unsigned char *buf, unsigned n, uint32_t seed,
                         int niter)
{
    orange od;
    rfcdec rd;
    uint32_t rs = seed ? seed : 0xA5A5A5A5u;
    int i;

    orange_init(&od, buf, n);
    rfc_init(&rd, buf, n);

    ckv(od.rng, rd.rng, "init rng");
    ckv(od.val, rd.val, "init val");
    ckv((unsigned long)orange_tell(&od), (unsigned long)rfc_tell(&rd),
        "init tell");
    ckv(orange_tell_frac(&od), rfc_tell_frac(&rd), "init tell_frac");

    for (i = 0; i < niter; i++) {
        unsigned op = xrand(&rs) % 5;
        switch (op) {
        case 0: { /* decode + update: a uniform ft-symbol alphabet.
                   * Half the draws use a SMALL ft (2..7) rather than a wide
                   * one -- with fl=s, fh=s+1 below, fl==0 exactly when the
                   * decoded symbol is 0, which is common at small ft
                   * (~1/ft) and would be too rare at ft~thousands to
                   * reliably exercise orange_update's fl==0 special case.
                   * That branch is what test-opus-range-negctl targets. */
            unsigned ft = (xrand(&rs) & 1) ? 2 + xrand(&rs) % 6000
                                            : 2 + xrand(&rs) % 6;
            unsigned s1 = orange_decode(&od, ft);
            unsigned s2 = rfc_decode(&rd, ft);
            ckv(s1, s2, "decode symbol");
            orange_update(&od, s1, s1 + 1, ft);
            rfc_update(&rd, s2, s2 + 1, ft);
            break;
        }
        case 1: { /* decode_bin + update */
            unsigned bits = 1 + xrand(&rs) % 15;
            unsigned top = 1u << bits;
            unsigned s1 = orange_decode_bin(&od, bits);
            unsigned s2 = rfc_decode_bin(&rd, bits);
            ckv(s1, s2, "decode_bin symbol");
            orange_update(&od, s1, s1 + 1, top);
            rfc_update(&rd, s2, s2 + 1, top);
            break;
        }
        case 2: { /* bit_logp */
            unsigned logp = 1 + xrand(&rs) % 14;
            int b1 = orange_bit_logp(&od, logp);
            int b2 = rfc_bit_logp(&rd, logp);
            ckv((unsigned long)b1, (unsigned long)b2, "bit_logp");
            break;
        }
        case 3: { /* icdf, from a small pool of valid decreasing tables */
            static const unsigned char icdf_a[] = { 192, 96, 32, 8, 0 };
            static const unsigned char icdf_b[] = { 224, 160, 96, 48, 16, 0 };
            static const unsigned char icdf_c[] = { 128, 0 };
            const unsigned char *tab;
            switch (xrand(&rs) % 3) {
            case 0:  tab = icdf_a; break;
            case 1:  tab = icdf_b; break;
            default: tab = icdf_c; break;
            }
            {
                int r1 = orange_icdf(&od, tab, 8);
                int r2 = rfc_icdf(&rd, tab, 8);
                ckv((unsigned long)r1, (unsigned long)r2, "icdf");
            }
            break;
        }
        default: { /* dec_uint or dec_bits */
            if (xrand(&rs) & 1) {
                uint32_t ft = 2 + xrand(&rs) % 100000000u;
                uint32_t u1 = orange_dec_uint(&od, ft);
                uint32_t u2 = rfc_dec_uint(&rd, ft);
                ckv(u1, u2, "dec_uint");
            } else {
                unsigned bits = 1 + xrand(&rs) % 24;
                uint32_t b1 = orange_dec_bits(&od, bits);
                uint32_t b2 = rfc_dec_bits(&rd, bits);
                ckv(b1, b2, "dec_bits");
            }
            break;
        }
        }
        ckv(od.rng, rd.rng, "rng after op");
        ckv(od.val, rd.val, "val after op");
    }

    ckv((unsigned long)orange_tell(&od), (unsigned long)rfc_tell(&rd),
        "final tell");
    ckv(orange_tell_frac(&od), rfc_tell_frac(&rd), "final tell_frac");
    ckv(od.rng, rd.rng, "final rng (the per-packet checksum in test-opus)");
}

int main(void)
{
    static const unsigned sizes[] = {
        1, 2, 3, 5, 8, 16, 32, 64, 128, 256, 1024, 4096, 16384
    };
    size_t b;

    printf("opus_range_test\n");

    for (b = 0; b < sizeof(sizes) / sizeof(sizes[0]); b++) {
        unsigned n = sizes[b];
        unsigned char *buf = (unsigned char *)malloc(n ? n : 1);
        uint32_t fill_seed = 0x9E3779B9u ^ ((uint32_t)b * 2654435761u);
        uint32_t rs = fill_seed;
        uint32_t run_seed = fill_seed ^ 0xABCDEF01u;
        unsigned i;

        if (!buf) { printf("FAIL: out of memory\n"); return 1; }
        for (i = 0; i < n; i++) buf[i] = (unsigned char)(xrand(&rs) & 0xFF);

        run_lockstep(buf, n, run_seed, 400);
        free(buf);
    }

    printf("opus_range_test: %d checks, %d failures\n", checks, failures);
    (void)ck;
    return failures ? 1 : 0;
}
