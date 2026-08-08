/* c/lib/audio/audio.c -- format sniffing, the whole-file decode, and the pull
 * interface the PCM layer consumes. See audio.h.
 */

#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "wav.h"
#include "flac.h"
#include "mp3.h"
#include "aac.h"

audio_format audio_sniff(const uint8_t *data, long len)
{
    if (!data) return AUDIO_FMT_UNKNOWN;
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0)
        return AUDIO_FMT_WAV;
    if (len >= 4 && memcmp(data, "fLaC", 4) == 0)
        return AUDIO_FMT_FLAC;

    /* ADTS has no magic either, but its header is far more constrained than
     * MP3's: twelve sync bits, a layer field that must be zero, a sampling
     * frequency index below 13, and a frame length that must land exactly on
     * another header. Requiring the SECOND header is what keeps a random
     * 0xFFF from being called AAC. */
    for (long i = 0; i < 4096 && i + 7 <= len; i++) {
        long fl = aac_adts_frame_len(data + i, len - i);
        if (fl <= 0) continue;
        if (i + fl + 2 <= len) {
            if (aac_adts_frame_len(data + i + fl, len - i - fl) > 0)
                return AUDIO_FMT_AAC;
        } else if (i + fl == len) {
            return AUDIO_FMT_AAC;                 /* single-frame file */
        }
        break;
    }

    /* MP3 has no magic. An ID3v2 tag is a strong hint, and otherwise the only
     * honest test is that a frame header parses AND the frame it describes is
     * followed by another one -- a lone 11-bit sync pattern occurs by chance
     * every few hundred bytes of arbitrary data. */
    long off = mp3_id3_len(data, len);
    if (off > 0) return AUDIO_FMT_MP3;
    for (long i = 0; i < 4096 && i + 4 <= len; i++) {
        if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0) continue;
        mp3dec *probe = NULL;
        (void)probe;
        /* Cheap structural check without opening a decoder: ask the info-frame
         * helper, which parses a header and validates its derived frame size. */
        mp3frame f;
        int got = 0;
        mp3dec *d = mp3_open();
        if (!d) return AUDIO_FMT_UNKNOWN;
        int n = mp3_decode(d, data + i, len - i, &f, &got);
        mp3_close(d);
        if (n > 0) {
            /* A second header must sit exactly where the first frame ends. */
            long j = i + n;
            if (j + 2 <= len && data[j] == 0xFF && (data[j + 1] & 0xE0) == 0xE0)
                return AUDIO_FMT_MP3;
            if (j >= len) return AUDIO_FMT_MP3;   /* single-frame file */
        }
        break;
    }
    return AUDIO_FMT_UNKNOWN;
}

const char *audio_format_name(audio_format f)
{
    switch (f) {
    case AUDIO_FMT_WAV:  return "wav";
    case AUDIO_FMT_FLAC: return "flac";
    case AUDIO_FMT_MP3:  return "mp3";
    case AUDIO_FMT_AAC:  return "aac";
    default:             return "unknown";
    }
}

void audio_f32_to_s16(const float *in, int16_t *out, long n)
{
    for (long i = 0; i < n; i++) {
        double v = (double)in[i] * 32768.0;
        if (!(v == v)) { out[i] = 0; continue; }     /* NaN in, silence out */
        v = v >= 0 ? v + 0.5 : v - 0.5;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (int16_t)v;
    }
}

void audio_pcm_free(apcm *p)
{
    if (!p) return;
    free(p->s16);
    free(p->f32);
    p->s16 = NULL;
    p->f32 = NULL;
    p->frames = 0;
}

/* --- pull interface ------------------------------------------------------ */

struct adec {
    audio_format fmt;
    const uint8_t *data;
    long len;

    int rate, channels;
    long total;                /* -1 when unknown */

    /* WAV */
    wavinfo w;
    long wpos;

    /* FLAC */
    flacdec *fl;
    const int32_t *fplane[AUDIO_MAX_CHANNELS];
    long fhave, fpos;
    int fbits;

    /* MP3 */
    mp3dec *m3;
    long mpos;                 /* byte cursor */
    const float *mpcm;
    long mhave, mtaken;

    /* AAC */
    aacdec *ac;
    long apos;
    const float *apcm;
    long ahave, ataken;
};

adec *adec_open(const uint8_t *data, long len, int *err)
{
    int e = AUDIO_ERR_CORRUPT;
    if (!data || len <= 0) { if (err) *err = AUDIO_ERR_RANGE; return NULL; }

    adec *d = (adec *)malloc(sizeof(*d));
    if (!d) { if (err) *err = AUDIO_ERR_OOM; return NULL; }
    memset(d, 0, sizeof(*d));
    d->data = data; d->len = len; d->total = -1;
    d->fmt = audio_sniff(data, len);

    if (d->fmt == AUDIO_FMT_WAV) {
        e = wav_parse(data, len, &d->w);
        if (e != AUDIO_OK) goto fail;
        d->rate = d->w.rate; d->channels = d->w.channels; d->total = d->w.frames;
    } else if (d->fmt == AUDIO_FMT_FLAC) {
        d->fl = flac_open(data, len, &e);
        if (!d->fl) goto fail;
        long tot = 0;
        flac_info(d->fl, &d->rate, &d->channels, &d->fbits, &tot);
        d->total = tot ? tot : -1;
    } else if (d->fmt == AUDIO_FMT_MP3) {
        d->m3 = mp3_open();
        if (!d->m3) { e = AUDIO_ERR_OOM; goto fail; }
        d->mpos = mp3_id3_len(data, len);
        /* Skip a Xing/Info header frame: it is metadata that happens to be a
         * decodable frame of silence, and playing it would prepend silence. */
        if (mp3_is_info_frame(data + d->mpos, len - d->mpos)) {
            mp3frame f; int got = 0;
            int n = mp3_decode(d->m3, data + d->mpos, len - d->mpos, &f, &got);
            if (n > 0) d->mpos += n;
        }
        /* Peek one frame to learn the geometry. Decoding it twice is harmless
         * and cheaper than a second header parser that could disagree. */
        {
            mp3dec *probe = mp3_open();
            if (!probe) { e = AUDIO_ERR_OOM; goto fail; }
            long p = d->mpos;
            while (p < len) {
                mp3frame f; int got = 0;
                int n = mp3_decode(probe, data + p, len - p, &f, &got);
                if (n <= 0) break;
                p += n;
                if (got) { d->rate = f.rate; d->channels = f.channels; break; }
            }
            mp3_close(probe);
            if (!d->rate) { e = AUDIO_ERR_CORRUPT; goto fail; }
        }
    } else if (d->fmt == AUDIO_FMT_AAC) {
        d->ac = aac_open();
        if (!d->ac) { e = AUDIO_ERR_OOM; goto fail; }
        /* Peek one frame for the geometry, exactly as the MP3 path does: the
         * ADTS header alone gives the rate but not the channel count when
         * channel_configuration is 0 and a PCE decides it. */
        {
            aacdec *probe = aac_open();
            if (!probe) { e = AUDIO_ERR_OOM; goto fail; }
            long q = 0;
            while (q < len) {
                aacframe f;
                int got = 0;
                int n = aac_decode(probe, data + q, len - q, &f, &got);
                if (n <= 0) break;
                q += n;
                if (got) { d->rate = f.rate; d->channels = f.channels; break; }
            }
            aac_close(probe);
            if (!d->rate) { e = AUDIO_ERR_CORRUPT; goto fail; }
        }
    } else {
        e = AUDIO_ERR_UNSUPPORTED;
        goto fail;
    }

    if (err) *err = AUDIO_OK;
    return d;

fail:
    adec_close(d);
    if (err) *err = e;
    return NULL;
}

void adec_close(adec *d)
{
    if (!d) return;
    if (d->fl) flac_close(d->fl);
    if (d->m3) mp3_close(d->m3);
    if (d->ac) aac_close(d->ac);
    free(d);
}

int adec_info(const adec *d, int *rate, int *channels)
{
    if (!d) return AUDIO_ERR_RANGE;
    if (rate) *rate = d->rate;
    if (channels) *channels = d->channels;
    return AUDIO_OK;
}

audio_format adec_format(const adec *d) { return d ? d->fmt : AUDIO_FMT_UNKNOWN; }
long adec_duration_frames(const adec *d) { return d ? d->total : -1; }

static int16_t clip16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

long adec_read(adec *d, int16_t *out, long maxframes)
{
    if (!d || !out || maxframes < 0) return AUDIO_ERR_RANGE;
    if (maxframes == 0) return 0;

    if (d->fmt == AUDIO_FMT_WAV) {
        long n = wav_read_s16(&d->w, d->wpos, maxframes, out);
        if (n > 0) d->wpos += n;
        return n;
    }

    if (d->fmt == AUDIO_FMT_FLAC) {
        long done = 0;
        while (done < maxframes) {
            if (d->fpos >= d->fhave) {
                long n = flac_decode_frame(d->fl, d->fplane);
                if (n < 0) return done ? done : n;
                if (n == 0) break;
                d->fhave = n; d->fpos = 0;
            }
            long take = d->fhave - d->fpos;
            if (take > maxframes - done) take = maxframes - done;
            /* Scale to 16 bit by shifting: exact for 16-bit sources and a
             * plain truncation of the low bits otherwise. */
            int sh = d->fbits - 16;
            for (long i = 0; i < take; i++) {
                for (int c = 0; c < d->channels; c++) {
                    int32_t v = d->fplane[c][d->fpos + i];
                    if (sh > 0) v >>= sh;
                    else if (sh < 0) v <<= -sh;
                    out[(done + i) * d->channels + c] = clip16(v);
                }
            }
            d->fpos += take;
            done += take;
        }
        return done;
    }

    if (d->fmt == AUDIO_FMT_MP3) {
        long done = 0;
        while (done < maxframes) {
            if (d->mtaken >= d->mhave) {
                int got = 0;
                mp3frame f;
                long produced = 0;
                while (d->mpos < d->len) {
                    int n = mp3_decode(d->m3, d->data + d->mpos, d->len - d->mpos, &f, &got);
                    if (n == 0) { d->mpos = d->len; break; }
                    if (n < 0) {
                        /* Resynchronise: a damaged frame must not end the
                         * stream, so step one byte and look for the next sync. */
                        d->mpos++;
                        continue;
                    }
                    d->mpos += n;
                    if (got) { produced = f.nsamples; break; }
                }
                if (!produced) break;
                d->mpcm = f.pcm;
                d->mhave = produced;
                d->mtaken = 0;
                d->channels = f.channels;
            }
            long take = d->mhave - d->mtaken;
            if (take > maxframes - done) take = maxframes - done;
            for (long i = 0; i < take; i++)
                for (int c = 0; c < d->channels; c++) {
                    float v = d->mpcm[(d->mtaken + i) * d->channels + c];
                    audio_f32_to_s16(&v, &out[(done + i) * d->channels + c], 1);
                }
            d->mtaken += take;
            done += take;
        }
        return done;
    }

    if (d->fmt == AUDIO_FMT_AAC) {
        long done = 0;
        while (done < maxframes) {
            if (d->ataken >= d->ahave) {
                aacframe f;
                long produced = 0;
                while (d->apos < d->len) {
                    int got = 0;
                    int n = aac_decode(d->ac, d->data + d->apos, d->len - d->apos,
                                       &f, &got);
                    if (n == 0) { d->apos = d->len; break; }
                    if (n < 0) {
                        /* A damaged frame must not end the stream: step one
                         * byte and resynchronise on the next syncword. */
                        d->apos++;
                        continue;
                    }
                    d->apos += n;
                    if (got) { produced = f.nsamples; break; }
                }
                if (!produced) break;
                d->apcm = f.pcm;
                d->ahave = produced;
                d->ataken = 0;
                d->channels = f.channels;
            }
            long take = d->ahave - d->ataken;
            if (take > maxframes - done) take = maxframes - done;
            audio_f32_to_s16(d->apcm + d->ataken * d->channels,
                             out + done * d->channels,
                             take * d->channels);
            d->ataken += take;
            done += take;
        }
        return done;
    }

    return AUDIO_ERR_UNSUPPORTED;
}

/* --- whole-file decode --------------------------------------------------- */

int audio_decode(const uint8_t *data, long len, unsigned flags, apcm *out)
{
    if (!out) return AUDIO_ERR_RANGE;
    memset(out, 0, sizeof(*out));

    int err = AUDIO_OK;
    adec *d = adec_open(data, len, &err);
    if (!d) return err;

    out->rate = d->rate;
    out->channels = d->channels;
    if (d->fmt == AUDIO_FMT_WAV) out->bits = d->w.bits;
    else if (d->fmt == AUDIO_FMT_FLAC) out->bits = d->fbits;

    long cap = (d->total > 0) ? d->total : 65536;
    long n = 0;
    int16_t *buf = (int16_t *)malloc((size_t)cap * d->channels * sizeof(int16_t));
    if (!buf) { adec_close(d); return AUDIO_ERR_OOM; }

    for (;;) {
        if (n == cap) {
            if (cap > (long)1 << 28) { free(buf); adec_close(d); return AUDIO_ERR_RANGE; }
            long nc = cap * 2;
            int16_t *nb = (int16_t *)realloc(buf, (size_t)nc * d->channels * sizeof(int16_t));
            if (!nb) { free(buf); adec_close(d); return AUDIO_ERR_OOM; }
            buf = nb; cap = nc;
        }
        long got = adec_read(d, buf + n * d->channels, cap - n);
        if (got < 0) { free(buf); adec_close(d); return (int)got; }
        if (got == 0) break;
        n += got;
    }

    out->s16 = buf;
    out->frames = n;

    if ((flags & AUDIO_WANT_FLOAT) && d->fmt == AUDIO_FMT_MP3) {
        /* Re-decode for the float output rather than keeping both alive during
         * the first pass: the conformance test wants the pre-quantisation
         * samples, and everything else wants the integers. */
        float *f = (float *)malloc((size_t)n * d->channels * sizeof(float));
        if (!f) { adec_close(d); return AUDIO_OK; }   /* s16 is still valid */
        mp3dec *m = mp3_open();
        if (!m) { free(f); adec_close(d); return AUDIO_OK; }
        long pos = mp3_id3_len(data, len);
        if (mp3_is_info_frame(data + pos, len - pos)) {
            mp3frame fr; int got = 0;
            int nn = mp3_decode(m, data + pos, len - pos, &fr, &got);
            if (nn > 0) pos += nn;
        }
        long w = 0;
        while (pos < len && w < n) {
            mp3frame fr; int got = 0;
            int nn = mp3_decode(m, data + pos, len - pos, &fr, &got);
            if (nn == 0) break;
            if (nn < 0) { pos++; continue; }
            pos += nn;
            if (!got) continue;
            long take = fr.nsamples;
            if (take > n - w) take = n - w;
            memcpy(f + w * d->channels, fr.pcm,
                   (size_t)take * d->channels * sizeof(float));
            w += take;
        }
        mp3_close(m);
        out->f32 = f;
    }

    adec_close(d);
    return AUDIO_OK;
}
