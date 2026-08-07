/* PCM primitives: format conversion, resampling, mixing, the ring.
 *
 * Nothing in this file touches the kernel. That is deliberate and it is the
 * reason tests/unit/audio_pcm_test.c can compile this exact file on the host
 * and assert on its output sample by sample -- a mixer tested by listening to
 * it is not tested. */
#include "snd.h"

int snd_fmt_bytes(int fmt)
{
    switch (fmt) {
    case SND_FMT_S16: return 2;
    case SND_FMT_U8:  return 1;
    case SND_FMT_S32: return 4;
    case SND_FMT_F32: return 4;
    default:          return 0;
    }
}

int snd_fmt_ok(unsigned rate, unsigned channels, int fmt)
{
    if (!snd_fmt_bytes(fmt)) return 0;
    /* Up to 8 in, because that is what the decoders produce (a 5.1 FLAC is 6).
     * Anything past the first two is DROPPED, not folded down: channels 0 and 1
     * are front-left and front-right in WAV/FLAC order, so taking them is a
     * real stereo signal rather than a wrong one. A proper downmix wants the
     * centre and LFE coefficients and is a separate piece of work -- see the
     * gap list. Refusing 6 channels outright would be worse: it would make a
     * surround file unplayable rather than merely un-surrounded. */
    if (channels < 1 || channels > 8) return 0;
    /* 4 kHz is below any real content; 192 kHz is above anything we would be
     * handed. The bound matters because rate feeds a Q16.16 step below and a
     * ratio outside ~1:1000 would overflow it. */
    if (rate < 4000 || rate > 192000) return 0;
    return 1;
}

/* --------------------------------------------------------- conversion ----- */

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* One source frame -> up to 2 channels of s16. Kept separate from the loop so
 * the channel-mapping rule is stated once. */
static void frame_to_s16(int16_t *dst, unsigned dst_ch,
                         const void *src, unsigned src_ch, int fmt, unsigned i)
{
    int32_t s[2] = { 0, 0 };
    unsigned c, n = src_ch > 2 ? 2 : src_ch;

    for (c = 0; c < n; c++) {
        unsigned k = i * src_ch + c;
        switch (fmt) {
        case SND_FMT_S16: s[c] = ((const int16_t *)src)[k]; break;
        /* U8 silence is 0x80, not 0: subtract the bias before scaling or every
         * 8-bit source arrives with a large DC offset and a thump at start. */
        case SND_FMT_U8:  s[c] = ((int32_t)((const uint8_t *)src)[k] - 128) << 8; break;
        case SND_FMT_S32: s[c] = ((const int32_t *)src)[k] >> 16; break;
        case SND_FMT_F32: {
            float f = ((const float *)src)[k];
            /* Clamp BEFORE the cast: (int32_t)(2.0f*32767) is defined, but a
             * NaN or an inf cast to int is not, and a decoder that emits one
             * bad float should make a click, not a trap. */
            if (!(f >= -1.0f)) f = (f > 0.0f) ? 1.0f : -1.0f;  /* also catches NaN */
            if (f > 1.0f) f = 1.0f;
            s[c] = (int32_t)(f * 32767.0f);
            break;
        }
        default: s[c] = 0; break;
        }
    }

    if (src_ch == 1) {
        /* Mono fans out: a mono decoder should be heard from both speakers, not
         * from the left one only. */
        for (c = 0; c < dst_ch; c++) dst[c] = clamp16(s[0]);
    } else if (dst_ch == 1) {
        dst[0] = clamp16((s[0] + s[1]) / 2);
    } else {
        for (c = 0; c < dst_ch; c++) dst[c] = clamp16(c < n ? s[c] : 0);
    }
}

unsigned pcm_to_s16(int16_t *dst, unsigned dst_ch,
                    const void *src, unsigned src_ch, int src_fmt,
                    unsigned frames)
{
    unsigned i;
    if (!dst || !src || !dst_ch || !src_ch) return 0;
    for (i = 0; i < frames; i++)
        frame_to_s16(dst + i * dst_ch, dst_ch, src, src_ch, src_fmt, i);
    return frames;
}

/* ---------------------------------------------------------- resampling ---- */

unsigned pcm_resample(int16_t *dst, unsigned dst_max,
                      const int16_t *src, unsigned src_frames,
                      unsigned ch, unsigned src_rate, unsigned dst_rate,
                      uint32_t *phase, int16_t *hist, unsigned *used)
{
    unsigned out = 0, consumed = 0, c;
    uint32_t ph, step;

    if (used) *used = 0;
    if (!dst || !src || !ch || ch > 2 || !src_rate || !dst_rate) return 0;

    /* Equal rates: the identity path, byte for byte. Worth having explicitly --
     * the interpolator below would also return the input unchanged, but only
     * because 0 phase never advances into a fraction, and relying on that is
     * how a resampler that is supposed to be transparent stops being. */
    if (src_rate == dst_rate) {
        unsigned n = src_frames < dst_max ? src_frames : dst_max;
        for (out = 0; out < n * ch; out++) dst[out] = src[out];
        if (n) for (c = 0; c < ch; c++) hist[c] = src[(n - 1) * ch + c];
        if (used) *used = n;
        return n;
    }

    /* Q16.16 step: how far to advance through the input per output frame.
     * src_rate <= 192000 so src_rate<<16 is < 2^33 -- compute in 64-bit and the
     * bound in snd_fmt_ok keeps the result inside 32 bits. */
    step = (uint32_t)(((uint64_t)src_rate << 16) / dst_rate);
    ph = *phase;

    while (out < dst_max) {
        unsigned idx = ph >> 16;
        uint32_t frac;
        if (idx >= src_frames) break;          /* need more input */
        frac = ph & 0xFFFF;

        for (c = 0; c < ch; c++) {
            /* a = the frame at idx, b = the next one. idx == 0 takes `a` from
             * the carried history, which is what makes the seam between two
             * periods continuous instead of a click every 21 ms. */
            int32_t a = (idx == 0) ? hist[c] : src[(idx - 1) * ch + c];
            int32_t b = src[idx * ch + c];
            dst[out * ch + c] = clamp16(a + (int32_t)(((int64_t)(b - a) * frac) >> 16));
        }
        out++;
        ph += step;
    }

    consumed = ph >> 16;
    if (consumed > src_frames) consumed = src_frames;
    /* Carry the last consumed frame forward as history, and rebase the phase to
     * the fraction only -- the integer part has been paid for by `consumed`. */
    if (consumed) for (c = 0; c < ch; c++) hist[c] = src[(consumed - 1) * ch + c];
    *phase = ph - (consumed << 16);

    if (used) *used = consumed;
    return out;
}

/* -------------------------------------------------------------- mixing ---- */

void pcm_mix_add(int16_t *dst, const int16_t *src, unsigned samples)
{
    unsigned i;
    /* int32 accumulate then clamp. Summing in int16 wraps, and a wrap turns two
     * loud streams into a full-scale square wave -- the loudest possible noise,
     * from the quietest possible mistake. */
    for (i = 0; i < samples; i++)
        dst[i] = clamp16((int32_t)dst[i] + (int32_t)src[i]);
}

void pcm_silence(void *dst, int fmt, unsigned samples)
{
    unsigned i;
    if (fmt == SND_FMT_U8) {
        uint8_t *p = (uint8_t *)dst;
        for (i = 0; i < samples; i++) p[i] = 0x80;   /* NOT zero */
        return;
    }
    {
        uint8_t *p = (uint8_t *)dst;
        unsigned n = samples * (unsigned)snd_fmt_bytes(fmt);
        for (i = 0; i < n; i++) p[i] = 0;
    }
}

/* ---------------------------------------------------------------- ring ---- */

void pcm_ring_init(struct pcm_ring *r, uint8_t *buf, unsigned size)
{
    r->buf = buf; r->size = size; r->head = 0; r->tail = 0;
}

unsigned pcm_ring_used(const struct pcm_ring *r)
{
    return (unsigned)(r->head - r->tail);
}

unsigned pcm_ring_free(const struct pcm_ring *r)
{
    return r->size - (unsigned)(r->head - r->tail);
}

unsigned pcm_ring_write(struct pcm_ring *r, const void *src, unsigned bytes)
{
    const uint8_t *s = (const uint8_t *)src;
    unsigned room = pcm_ring_free(r), n, off, first;

    n = bytes < room ? bytes : room;
    off = (unsigned)(r->head % r->size);
    first = r->size - off;
    if (first > n) first = n;

    for (unsigned i = 0; i < first; i++) r->buf[off + i] = s[i];
    for (unsigned i = 0; i < n - first; i++) r->buf[i] = s[first + i];
    r->head += n;
    return n;
}

unsigned pcm_ring_peek(const struct pcm_ring *r, void *dst, unsigned bytes)
{
    uint8_t *d = (uint8_t *)dst;
    unsigned have = pcm_ring_used(r), n, off, first;

    n = bytes < have ? bytes : have;
    off = (unsigned)(r->tail % r->size);
    first = r->size - off;
    if (first > n) first = n;

    for (unsigned i = 0; i < first; i++) d[i] = r->buf[off + i];
    for (unsigned i = 0; i < n - first; i++) d[first + i] = r->buf[i];
    return n;
}

void pcm_ring_advance(struct pcm_ring *r, unsigned bytes)
{
    unsigned have = pcm_ring_used(r);
    r->tail += (bytes < have ? bytes : have);
}

unsigned pcm_ring_read(struct pcm_ring *r, void *dst, unsigned bytes)
{
    unsigned n = pcm_ring_peek(r, dst, bytes);
    r->tail += n;
    return n;
}
