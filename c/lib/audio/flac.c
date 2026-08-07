/* c/lib/audio/flac.c -- from-scratch FLAC decoder. See flac.h.
 *
 * The whole format is integer arithmetic over a bit stream, so "close enough"
 * has no meaning here: either every sample comes back exactly as the encoder
 * saw it or the decoder is wrong, and the file itself says which via the
 * STREAMINFO MD5.
 *
 * Structure: metadata blocks -> frames -> one subframe per channel -> a
 * predictor plus a Rice-coded residual. The only interesting arithmetic is the
 * LPC reconstruction, which must be done in 64 bits: a 32-order predictor with
 * 15-bit coefficients over 32-bit samples overflows int32 long before it
 * overflows the format, and doing it in 32 bits produces a decoder that works
 * on every small test file and fails on real high-resolution music.
 */

#include <stdlib.h>
#include <string.h>
#include "flac.h"
#include "abits.h"
#include "amd5.h"

struct flacdec {
    const uint8_t *data;
    long len;
    long first_frame;      /* byte offset of the first audio frame */

    /* STREAMINFO */
    int  min_block, max_block;
    int  rate, channels, bits;
    long total;            /* total frames, 0 = unknown */
    uint8_t md5[16];

    /* current frame */
    int32_t *plane[AUDIO_MAX_CHANNELS];
    long     plane_cap;
    long     pos;          /* byte offset of the next frame */
    long     frame_samples;

    amd5 md5ctx;
};

/* --- CRCs -------------------------------------------------------------- */
/* Both are the plain (non-reflected, zero-init) forms the FLAC spec names.
 * They are computed bytewise over the raw frame bytes, which is why the frame
 * parser tracks byte offsets alongside the bit reader. */

static uint8_t crc8(const uint8_t *p, long n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
    }
    return c;
}

static uint16_t crc16(const uint8_t *p, long n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            c = (uint16_t)((c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1));
    }
    return c;
}

/* --- residual ---------------------------------------------------------- */

static int read_residual(abits *b, int32_t *out, long blocksize, int order)
{
    uint32_t method = ab_u(b, 2);
    if (method > 1) return AUDIO_ERR_UNSUPPORTED;   /* 2 and 3 are reserved */
    int pbits  = method ? 5 : 4;
    uint32_t escape = method ? 31u : 15u;

    int porder = (int)ab_u(b, 4);
    long nparts = 1L << porder;
    if (blocksize % nparts) return AUDIO_ERR_CORRUPT;
    long per = blocksize >> porder;
    if (per < order) return AUDIO_ERR_CORRUPT;      /* first partition underflows */

    long idx = 0;
    for (long p = 0; p < nparts; p++) {
        long n = (p == 0) ? per - order : per;
        uint32_t param = ab_u(b, pbits);
        if (param == escape) {
            int raw = (int)ab_u(b, 5);
            for (long i = 0; i < n; i++)
                out[idx++] = raw ? ab_s(b, raw) : 0;
        } else {
            for (long i = 0; i < n; i++) {
                /* A Rice quotient is a unary run. Bound it: the run cannot
                 * exceed the bits left in the file, and without a bound a
                 * corrupt stream turns into a very long loop before it turns
                 * into an error. */
                uint32_t q = ab_unary(b, (uint32_t)(ab_left(b) > 0 ? ab_left(b) : 0));
                if (b->error) return AUDIO_ERR_CORRUPT;
                uint32_t r = ab_u(b, (int)param);
                uint32_t v = (q << param) | r;
                out[idx++] = (int32_t)((v >> 1) ^ (uint32_t)(-(int32_t)(v & 1)));
            }
        }
        if (b->error) return AUDIO_ERR_CORRUPT;
    }
    return AUDIO_OK;
}

/* --- subframes --------------------------------------------------------- */

/* The predictor sums are done in unsigned arithmetic and cast back. For a
 * valid stream every intermediate fits int32 by construction, but a corrupt
 * one can present residuals that overflow it, and signed overflow is undefined
 * behaviour -- a decoder for untrusted input may not have any. Unsigned wraps
 * with defined semantics and produces the identical result whenever the value
 * is in range, which is the only case that has to be right. */
static void fixed_restore(int32_t *v, long n, int order)
{
    uint32_t *u = (uint32_t *)v;
    switch (order) {
    case 0:
        break;
    case 1:
        for (long i = 1; i < n; i++) u[i] += u[i - 1];
        break;
    case 2:
        for (long i = 2; i < n; i++) u[i] += 2u * u[i - 1] - u[i - 2];
        break;
    case 3:
        for (long i = 3; i < n; i++)
            u[i] += 3u * u[i - 1] - 3u * u[i - 2] + u[i - 3];
        break;
    default:
        for (long i = 4; i < n; i++)
            u[i] += 4u * u[i - 1] - 6u * u[i - 2] + 4u * u[i - 3] - u[i - 4];
        break;
    }
}

static int read_subframe(flacdec *d, abits *b, int32_t *v, long n, int bps)
{
    (void)d;
    if (ab_u1(b)) return AUDIO_ERR_CORRUPT;          /* zero bit padding */
    int type = (int)ab_u(b, 6);
    int wasted = 0;
    if (ab_u1(b)) {
        /* Wasted bits: a unary count, then every sample is shifted left by it
         * at the end. Encoders emit this for material whose low bits are
         * always zero, e.g. 16-bit audio stored in a 24-bit stream. */
        wasted = (int)ab_unary(b, 32) + 1;
        if (b->error || wasted >= bps) return AUDIO_ERR_CORRUPT;
        bps -= wasted;
    }
    if (b->error) return AUDIO_ERR_CORRUPT;
    if (bps <= 0 || bps > 32) return AUDIO_ERR_CORRUPT;

    int rc = AUDIO_OK;
    if (type == 0) {                                  /* CONSTANT */
        int32_t s = ab_s(b, bps);
        for (long i = 0; i < n; i++) v[i] = s;
    } else if (type == 1) {                           /* VERBATIM */
        for (long i = 0; i < n; i++) v[i] = ab_s(b, bps);
    } else if (type >= 8 && type <= 12) {             /* FIXED, order 0..4 */
        int order = type - 8;
        if (order > n) return AUDIO_ERR_CORRUPT;
        for (int i = 0; i < order; i++) v[i] = ab_s(b, bps);
        rc = read_residual(b, v + order, n, order);
        if (rc != AUDIO_OK) return rc;
        fixed_restore(v, n, order);
    } else if (type >= 32) {                          /* LPC, order 1..32 */
        int order = type - 31;
        if (order > n) return AUDIO_ERR_CORRUPT;
        for (int i = 0; i < order; i++) v[i] = ab_s(b, bps);
        int prec = (int)ab_u(b, 4);
        if (prec == 15) return AUDIO_ERR_CORRUPT;     /* the spec calls this invalid */
        prec += 1;
        int shift = ab_s(b, 5);
        if (shift < 0) return AUDIO_ERR_CORRUPT;      /* negative shift is undefined */
        int32_t coef[FLAC_MAX_LPC_ORDER];
        for (int i = 0; i < order; i++) coef[i] = ab_s(b, prec);
        if (b->error) return AUDIO_ERR_CORRUPT;
        rc = read_residual(b, v + order, n, order);
        if (rc != AUDIO_OK) return rc;
        /* 64-bit accumulator, not 32: see the file header comment. The final
         * accumulation is unsigned for the same reason fixed_restore is. */
        if (shift > 63) return AUDIO_ERR_CORRUPT;
        for (long i = order; i < n; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++)
                sum += (int64_t)coef[j] * (int64_t)v[i - 1 - j];
            v[i] = (int32_t)((uint32_t)v[i] + (uint32_t)(int32_t)(sum >> shift));
        }
    } else {
        return AUDIO_ERR_CORRUPT;                     /* reserved subframe type */
    }
    if (b->error) return AUDIO_ERR_CORRUPT;

    if (wasted)
        for (long i = 0; i < n; i++) v[i] = (int32_t)((uint32_t)v[i] << wasted);
    return AUDIO_OK;
}

/* --- frames ------------------------------------------------------------ */

static const int BLOCKSIZE_TAB[16] = {
    0, 192, 576, 1152, 2304, 4608, -1, -2, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};
/* The frame header can restate the sample rate, but STREAMINFO is the
 * authority and a frame that disagrees is corrupt, so the rate codes are only
 * consumed for their width -- see the sr_code handling in the frame parser. */
static const int BPS_TAB[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* The frame number / sample number is coded like UTF-8 but up to 7 bytes.
 * Only the length matters to us -- the value is used for seeking, which this
 * decoder does not do -- but it must be consumed exactly or every field after
 * it is misaligned. */
static int skip_coded_number(abits *b)
{
    uint32_t c = ab_u(b, 8);
    if (b->error) return AUDIO_ERR_CORRUPT;
    int extra;
    if      ((c & 0x80) == 0x00) extra = 0;
    else if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else if ((c & 0xFC) == 0xF8) extra = 4;
    else if ((c & 0xFE) == 0xFC) extra = 5;
    else if (c == 0xFE)          extra = 6;
    else return AUDIO_ERR_CORRUPT;
    for (int i = 0; i < extra; i++)
        if ((ab_u(b, 8) & 0xC0) != 0x80) return AUDIO_ERR_CORRUPT;
    return b->error ? AUDIO_ERR_CORRUPT : AUDIO_OK;
}

long flac_decode_frame(flacdec *d, const int32_t *planes[])
{
    if (!d) return AUDIO_ERR_RANGE;
    if (d->pos >= d->len) return 0;

    const uint8_t *base = d->data + d->pos;
    long avail = d->len - d->pos;
    if (avail < 6) return 0;                 /* trailing junk shorter than a header */

    abits b;
    ab_init(&b, base, avail);

    if (ab_u(&b, 14) != 0x3FFEu) return AUDIO_ERR_CORRUPT;
    if (ab_u1(&b)) return AUDIO_ERR_CORRUPT;                 /* reserved */
    int variable = (int)ab_u1(&b);
    int bs_code  = (int)ab_u(&b, 4);
    int sr_code  = (int)ab_u(&b, 4);
    int ch_code  = (int)ab_u(&b, 4);
    int bps_code = (int)ab_u(&b, 3);
    if (ab_u1(&b)) return AUDIO_ERR_CORRUPT;                 /* reserved */
    (void)variable;

    if (ch_code > 10) return AUDIO_ERR_CORRUPT;
    int nch = (ch_code < 8) ? ch_code + 1 : 2;
    if (nch != d->channels) return AUDIO_ERR_CORRUPT;

    int bps = BPS_TAB[bps_code];
    if (bps == 0) bps = d->bits;                             /* 0 = get from STREAMINFO */
    if (bps != d->bits) return AUDIO_ERR_CORRUPT;

    if (skip_coded_number(&b) != AUDIO_OK) return AUDIO_ERR_CORRUPT;

    long blocksize;
    if (bs_code == 0) return AUDIO_ERR_CORRUPT;              /* reserved */
    else if (bs_code == 6) blocksize = (long)ab_u(&b, 8) + 1;
    else if (bs_code == 7) blocksize = (long)ab_u(&b, 16) + 1;
    else blocksize = BLOCKSIZE_TAB[bs_code];

    if (sr_code == 12)      ab_u(&b, 8);
    else if (sr_code == 13) ab_u(&b, 16);
    else if (sr_code == 14) ab_u(&b, 16);
    else if (sr_code == 15) return AUDIO_ERR_CORRUPT;

    if (b.error) return AUDIO_ERR_CORRUPT;
    if (blocksize <= 0 || blocksize > d->plane_cap) return AUDIO_ERR_CORRUPT;

    /* Header CRC-8 covers everything from the sync code up to (not including)
     * the CRC byte itself. Checking it here is what keeps a random 0x3FFE in
     * the middle of a corrupt file from being decoded as a frame. */
    long hdr_bytes = ab_pos(&b) / 8;
    if (ab_pos(&b) & 7) return AUDIO_ERR_CORRUPT;            /* header is byte aligned */
    uint32_t want8 = ab_u(&b, 8);
    if (b.error) return AUDIO_ERR_CORRUPT;
    if (crc8(base, hdr_bytes) != (uint8_t)want8) return AUDIO_ERR_CORRUPT;

    /* Stereo decorrelation: the side channel carries one extra bit because a
     * difference of two n-bit numbers needs n+1. */
    for (int c = 0; c < nch; c++) {
        int cbps = bps;
        if (ch_code == 8  && c == 1) cbps++;                 /* left/side  */
        if (ch_code == 9  && c == 0) cbps++;                 /* right/side */
        if (ch_code == 10 && c == 1) cbps++;                 /* mid/side   */
        int rc = read_subframe(d, &b, d->plane[c], blocksize, cbps);
        if (rc != AUDIO_OK) return rc;
    }
    ab_align(&b);

    long frame_bytes = ab_pos(&b) / 8;
    if (frame_bytes + 2 > avail) return AUDIO_ERR_CORRUPT;
    uint16_t got16 = crc16(base, frame_bytes);
    uint16_t want16 = (uint16_t)((base[frame_bytes] << 8) | base[frame_bytes + 1]);
    if (got16 != want16) return AUDIO_ERR_CORRUPT;

    /* Undo the stereo decorrelation in place. */
    if (ch_code == 8) {                                      /* left/side */
        for (long i = 0; i < blocksize; i++)
            d->plane[1][i] = (int32_t)((uint32_t)d->plane[0][i] - (uint32_t)d->plane[1][i]);
    } else if (ch_code == 9) {                               /* right/side */
        for (long i = 0; i < blocksize; i++)
            d->plane[0][i] = (int32_t)((uint32_t)d->plane[0][i] + (uint32_t)d->plane[1][i]);
    } else if (ch_code == 10) {                              /* mid/side */
        for (long i = 0; i < blocksize; i++) {
            int32_t side = d->plane[1][i];
            /* mid was stored with its low bit folded into the side's parity;
             * recovering it is a shift plus that parity bit, not a divide. */
#if AUDIO_SABOTAGE == 2
            /* NEGATIVE CONTROL (see `make test-audio-negctl`): drop the parity
             * bit. This is the classic way to get a FLAC decoder that is right
             * on half the samples and one LSB out on the other half -- audibly
             * perfect, and not lossless. Only -DAUDIO_SABOTAGE=2 builds it. */
            int32_t mid = (int32_t)((uint32_t)d->plane[0][i] << 1);
#else
            int32_t mid = (int32_t)(((uint32_t)d->plane[0][i] << 1) | ((uint32_t)side & 1));
#endif
            d->plane[0][i] = (int32_t)(((uint32_t)mid + (uint32_t)side)) >> 1;
            d->plane[1][i] = (int32_t)(((uint32_t)mid - (uint32_t)side)) >> 1;
        }
    }

    d->pos += frame_bytes + 2;
    d->frame_samples = blocksize;
    if (planes)
        for (int c = 0; c < nch; c++) planes[c] = d->plane[c];
    return blocksize;
}

/* --- open/close -------------------------------------------------------- */

flacdec *flac_open(const uint8_t *data, long len, int *err)
{
    int e = AUDIO_ERR_CORRUPT;
    flacdec *d = NULL;
    if (!data || len < 42) goto fail;
    if (memcmp(data, "fLaC", 4) != 0) goto fail;

    d = (flacdec *)malloc(sizeof(*d));
    if (!d) { e = AUDIO_ERR_OOM; goto fail; }
    memset(d, 0, sizeof(*d));
    d->data = data; d->len = len;

    long off = 4;
    int seen_streaminfo = 0;
    for (;;) {
        if (off + 4 > len) { e = AUDIO_ERR_CORRUPT; goto fail; }
        int last = data[off] >> 7;
        int type = data[off] & 0x7F;
        long blen = ((long)data[off + 1] << 16) | ((long)data[off + 2] << 8) | data[off + 3];
        off += 4;
        if (blen < 0 || off + blen > len) { e = AUDIO_ERR_CORRUPT; goto fail; }

        if (type == 0) {                       /* STREAMINFO */
            if (blen < 34 || seen_streaminfo) { e = AUDIO_ERR_CORRUPT; goto fail; }
            abits b;
            ab_init(&b, data + off, blen);
            d->min_block = (int)ab_u(&b, 16);
            d->max_block = (int)ab_u(&b, 16);
            ab_u(&b, 24);                      /* min frame size */
            ab_u(&b, 24);                      /* max frame size */
            d->rate     = (int)ab_u(&b, 20);
            d->channels = (int)ab_u(&b, 3) + 1;
            d->bits     = (int)ab_u(&b, 5) + 1;
            d->total    = (long)ab_u64(&b, 36);
            memcpy(d->md5, data + off + 18, 16);
            if (b.error) { e = AUDIO_ERR_CORRUPT; goto fail; }
            if (d->rate < AUDIO_MIN_RATE || d->rate > AUDIO_MAX_RATE) { e = AUDIO_ERR_RANGE; goto fail; }
            if (d->channels > AUDIO_MAX_CHANNELS) { e = AUDIO_ERR_RANGE; goto fail; }
            /* The spec allows 4..32 bits per sample. 33-bit side channels are
             * a FLAC 1.4 extension we do not do. */
            if (d->bits < 4 || d->bits > 32) { e = AUDIO_ERR_UNSUPPORTED; goto fail; }
            if (d->max_block < 16 || d->max_block > 65535) { e = AUDIO_ERR_RANGE; goto fail; }
            seen_streaminfo = 1;
        } else if (type == 127) {              /* invalid per the spec */
            e = AUDIO_ERR_CORRUPT; goto fail;
        }
        off += blen;
        if (last) break;
    }
    if (!seen_streaminfo) { e = AUDIO_ERR_CORRUPT; goto fail; }

    d->plane_cap = d->max_block;
    for (int c = 0; c < d->channels; c++) {
        d->plane[c] = (int32_t *)malloc((size_t)d->plane_cap * sizeof(int32_t));
        if (!d->plane[c]) { e = AUDIO_ERR_OOM; goto fail; }
    }
    d->first_frame = off;
    d->pos = off;
    amd5_init(&d->md5ctx);
    if (err) *err = AUDIO_OK;
    return d;

fail:
    if (d) flac_close(d);
    if (err) *err = e;
    return NULL;
}

void flac_close(flacdec *d)
{
    if (!d) return;
    for (int c = 0; c < AUDIO_MAX_CHANNELS; c++) free(d->plane[c]);
    free(d);
}

int flac_info(const flacdec *d, int *rate, int *channels, int *bits, long *total)
{
    if (!d) return AUDIO_ERR_RANGE;
    if (rate) *rate = d->rate;
    if (channels) *channels = d->channels;
    if (bits) *bits = d->bits;
    if (total) *total = d->total;
    return AUDIO_OK;
}

void flac_rewind(flacdec *d)
{
    if (!d) return;
    d->pos = d->first_frame;
    amd5_init(&d->md5ctx);
}

int flac_md5_ok(flacdec *d)
{
    if (!d) return AUDIO_ERR_RANGE;
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (d->md5[i]) { all_zero = 0; break; }
    if (all_zero) return AUDIO_ERR_UNSUPPORTED;

    flac_rewind(d);
    int bytes = (d->bits + 7) / 8;
    for (;;) {
        const int32_t *pl[AUDIO_MAX_CHANNELS];
        long n = flac_decode_frame(d, pl);
        if (n < 0) return (int)n;
        if (n == 0) break;
        /* The MD5 is over the samples interleaved, little endian, each
         * occupying ceil(bits/8) bytes -- exactly what the encoder hashed
         * before it ever looked at a predictor. */
        for (long i = 0; i < n; i++) {
            for (int c = 0; c < d->channels; c++) {
                uint32_t v = (uint32_t)pl[c][i];
                uint8_t b8[4];
                for (int k = 0; k < bytes; k++) b8[k] = (uint8_t)(v >> (8 * k));
                amd5_update(&d->md5ctx, b8, (unsigned long)bytes);
            }
        }
    }
    uint8_t got[16];
    amd5_final(&d->md5ctx, got);
    return memcmp(got, d->md5, 16) == 0 ? 1 : 0;
}
