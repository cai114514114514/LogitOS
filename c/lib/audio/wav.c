/* c/lib/audio/wav.c -- RIFF/WAVE parsing and header emission. See wav.h. */

#include <string.h>
#include "wav.h"

#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

int wav_parse(const uint8_t *buf, long len, wavinfo *w)
{
    if (!buf || !w || len < 12) return AUDIO_ERR_CORRUPT;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return AUDIO_ERR_CORRUPT;

    /* The RIFF size field covers everything after it. Believe it only as far
     * as it is inside the buffer we were actually given -- files are commonly
     * truncated in transit, and a size that overruns is not licence to read. */
    long riff_end = 8 + (long)rd32(buf + 4);
#if defined(AUDIO_FUZZ_SABOTAGE)
    /* NEGATIVE CONTROL, never defined by a shipping build. Believing the
     * declared RIFF size is the canonical WAV parser bug: the chunk walk below
     * then reads `ch[0..7]` at offsets past the end of the caller's buffer.
     * test-audio-codec-fuzz-negctl compiles with this to prove the fuzzer's
     * sanitizers actually fire -- a fuzz target that has never caught anything
     * is indistinguishable from one wired to /dev/null. */
    (void)len;
#else
    if (riff_end > len || riff_end < 12) riff_end = len;
#endif

    memset(w, 0, sizeof(*w));
    int have_fmt = 0;
    long off = 12;

    while (off + 8 <= riff_end) {
        const uint8_t *ch = buf + off;
        uint32_t csz = rd32(ch + 4);
        long body = off + 8;
        /* A chunk claiming more bytes than remain is corrupt. Clamp only the
         * final data chunk (a truncated recording is still playable); anything
         * else is rejected so a bogus size cannot steer the parse. */
        long avail = riff_end - body;
        if ((long)csz > avail) {
            if (memcmp(ch, "data", 4) == 0) csz = (uint32_t)(avail > 0 ? avail : 0);
            else return AUDIO_ERR_CORRUPT;
        }

        if (memcmp(ch, "fmt ", 4) == 0) {
            if (csz < 16) return AUDIO_ERR_CORRUPT;
            const uint8_t *f = buf + body;
            uint16_t tag   = rd16(f);
            uint16_t nch   = rd16(f + 2);
            uint32_t rate  = rd32(f + 4);
            uint16_t align = rd16(f + 12);
            uint16_t bits  = rd16(f + 14);

            if (tag == WAVE_FORMAT_EXTENSIBLE) {
                /* cbSize >= 22, and the real tag is the first two bytes of the
                 * SubFormat GUID. Without this every 24-bit file written by a
                 * modern recorder looks like an unknown codec. */
                if (csz < 40) return AUDIO_ERR_CORRUPT;
                tag = rd16(f + 24);
                uint16_t valid = rd16(f + 18);
                if (valid && valid != bits) return AUDIO_ERR_UNSUPPORTED;
            }

            if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT)
                return AUDIO_ERR_UNSUPPORTED;
            if (nch < 1 || nch > AUDIO_MAX_CHANNELS) return AUDIO_ERR_RANGE;
            if (rate < AUDIO_MIN_RATE || rate > AUDIO_MAX_RATE) return AUDIO_ERR_RANGE;

            w->is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
            if (w->is_float) {
                if (bits != 32 && bits != 64) return AUDIO_ERR_UNSUPPORTED;
            } else {
                if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
                    return AUDIO_ERR_UNSUPPORTED;
            }
            w->rate = (int)rate;
            w->channels = (int)nch;
            w->bits = (int)bits;
            w->frame_bytes = (int)nch * ((int)bits / 8);
            /* blockAlign is redundant with channels*bits/8, which makes it a
             * consistency check rather than something to compute from. */
            if (align != 0 && align != w->frame_bytes) return AUDIO_ERR_CORRUPT;
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) return AUDIO_ERR_CORRUPT;   /* data before fmt */
            w->data = buf + body;
            w->data_len = (long)csz;
            w->frames = w->frame_bytes ? (long)csz / w->frame_bytes : 0;
            return AUDIO_OK;
        }

        /* Chunks are word aligned: an odd size is followed by a pad byte that
         * is not counted in the size field. */
        off = body + (long)csz + ((long)csz & 1);
    }
    return AUDIO_ERR_CORRUPT;   /* no data chunk */
}

/* One sample, at native depth, as an int32 in [-2^(bits-1), 2^(bits-1)-1]. */
static int32_t sample_i32(const wavinfo *w, const uint8_t *p)
{
    switch (w->bits) {
    case 8:  return (int32_t)p[0] - 128;                  /* 8-bit WAV is unsigned */
    case 16: return (int16_t)rd16(p);
    case 24: {
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        if (v & 0x800000u) v |= 0xFF000000u;
        return (int32_t)v;
    }
    default: return (int32_t)rd32(p);                     /* 32 */
    }
}

static double sample_f(const wavinfo *w, const uint8_t *p)
{
    if (w->bits == 32) {
        union { uint32_t u; float f; } v;
        v.u = rd32(p);
        return (double)v.f;
    }
    union { uint64_t u; double d; } v;
    v.u = (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
    return v.d;
}

static int16_t clamp16(double x)
{
    /* Round half away from zero, then saturate. NaN maps to 0 rather than to
     * whatever the cast happens to produce. */
    if (!(x == x)) return 0;
    double r = x >= 0 ? x + 0.5 : x - 0.5;
    if (r > 32767.0) return 32767;
    if (r < -32768.0) return -32768;
    return (int16_t)r;
}

long wav_read_s16(const wavinfo *w, long at, long n, int16_t *out)
{
    if (!w || !out || !w->data || at < 0 || n < 0) return AUDIO_ERR_RANGE;
    if (at >= w->frames) return 0;
    if (n > w->frames - at) n = w->frames - at;

    int nch = w->channels, bps = w->bits / 8;
    for (long i = 0; i < n; i++) {
        const uint8_t *p = w->data + (at + i) * w->frame_bytes;
        for (int c = 0; c < nch; c++, p += bps) {
            double v;
            if (w->is_float) {
                v = sample_f(w, p) * 32768.0;
            } else {
                int32_t s = sample_i32(w, p);
                /* Scale by shifting, never by dividing: 24->16 is an exact
                 * arithmetic shift of the same value, and a float divide would
                 * make the 16-bit path non-identity for no reason. */
                if (w->bits == 8)       v = (double)(s * 256);
                else if (w->bits == 16) v = (double)s;
                else if (w->bits == 24) v = (double)(s >> 8);
                else                    v = (double)(s >> 16);
            }
            *out++ = clamp16(v);
        }
    }
    return n;
}

long wav_read_s32(const wavinfo *w, long at, long n, int32_t *out)
{
    if (!w || !out || !w->data || at < 0 || n < 0) return AUDIO_ERR_RANGE;
    if (at >= w->frames) return 0;
    if (n > w->frames - at) n = w->frames - at;

    int nch = w->channels, bps = w->bits / 8;
    double scale = 1.0;
    if (w->is_float) {
        scale = 32768.0;   /* float WAV is nominally [-1,1); report at 16-bit scale */
    }
    for (long i = 0; i < n; i++) {
        const uint8_t *p = w->data + (at + i) * w->frame_bytes;
        for (int c = 0; c < nch; c++, p += bps) {
            if (w->is_float) {
                double v = sample_f(w, p) * scale;
                if (!(v == v)) v = 0;
                v = v >= 0 ? v + 0.5 : v - 0.5;
                if (v > 2147483647.0) v = 2147483647.0;
                if (v < -2147483648.0) v = -2147483648.0;
                *out++ = (int32_t)v;
            } else {
                *out++ = sample_i32(w, p);
            }
        }
    }
    return n;
}

int wav_header_s16(uint8_t out[44], int rate, int channels, long frames)
{
    if (!out || channels < 1 || channels > AUDIO_MAX_CHANNELS) return AUDIO_ERR_RANGE;
    if (rate < AUDIO_MIN_RATE || rate > AUDIO_MAX_RATE) return AUDIO_ERR_RANGE;
    if (frames < 0 || frames > 0x7FFFFFFFL / (2L * channels)) return AUDIO_ERR_RANGE;

    long dbytes = frames * channels * 2;
    uint32_t v;
    memcpy(out, "RIFF", 4);
    v = (uint32_t)(36 + dbytes);
    out[4] = (uint8_t)v; out[5] = (uint8_t)(v >> 8);
    out[6] = (uint8_t)(v >> 16); out[7] = (uint8_t)(v >> 24);
    memcpy(out + 8, "WAVEfmt ", 8);
    out[16] = 16; out[17] = out[18] = out[19] = 0;      /* fmt chunk size */
    out[20] = 1; out[21] = 0;                           /* PCM */
    out[22] = (uint8_t)channels; out[23] = 0;
    v = (uint32_t)rate;
    out[24] = (uint8_t)v; out[25] = (uint8_t)(v >> 8);
    out[26] = (uint8_t)(v >> 16); out[27] = (uint8_t)(v >> 24);
    v = (uint32_t)(rate * channels * 2);                /* byte rate */
    out[28] = (uint8_t)v; out[29] = (uint8_t)(v >> 8);
    out[30] = (uint8_t)(v >> 16); out[31] = (uint8_t)(v >> 24);
    out[32] = (uint8_t)(channels * 2); out[33] = 0;     /* block align */
    out[34] = 16; out[35] = 0;                          /* bits */
    memcpy(out + 36, "data", 4);
    v = (uint32_t)dbytes;
    out[40] = (uint8_t)v; out[41] = (uint8_t)(v >> 8);
    out[42] = (uint8_t)(v >> 16); out[43] = (uint8_t)(v >> 24);
    return 44;
}
