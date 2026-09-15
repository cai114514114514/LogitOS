/* Host-side HE-AAC v1 probe.
 *
 * This deliberately takes an external ADTS path instead of committing a
 * copyrighted media sample.  The live Bilibili gate extracts the site's
 * current audio representation, decodes it here, and compares the optional
 * f32 output against FFmpeg decoding the identical bytes.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "aac.h"
#include "media.h"

typedef struct {
    FILE *out;
    long samples;
    long nonfinite;
    double energy;
    int frames;
    int rate;
    int channels;
} probe_stats;

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    *len = ftell(f);
    if (*len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)*len);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)*len, f) != (size_t)*len) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return p;
}

static int take_frame(probe_stats *st, const aacframe *f)
{
    st->frames++;
    st->rate = f->rate;
    st->channels = f->channels;
    long n = (long)f->nsamples * f->channels;
    for (long i = 0; i < n; i++) {
        if (!isfinite(f->pcm[i])) st->nonfinite++;
        else st->energy += (double)f->pcm[i] * f->pcm[i];
    }
    st->samples += n;
    if (st->out && fwrite(f->pcm, sizeof(*f->pcm), (size_t)n, st->out) != (size_t)n)
        return AUDIO_ERR_CORRUPT;
    return AUDIO_OK;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s input.aac [output.f32]\n", argv[0]);
        return 2;
    }
    long len = 0;
    uint8_t *data = slurp(argv[1], &len);
    FILE *out = argc == 3 ? fopen(argv[2], "wb") : NULL;
    aacdec *dec = NULL;
    mdemux *media = NULL;
    int track = -1, media_err = 0;
    if (data && media_sniff(data, len) == MEDIA_CONT_MP4) {
        media = media_open(data, len, &media_err);
        if (media) {
            track = media_find_track(media, MEDIA_TRACK_AUDIO);
            const media_track *t = media_track_info(media, track);
            if (t && t->codec == MEDIA_CODEC_AAC)
                dec = aac_open_asc(t->extradata, t->extradata_len, &media_err);
        }
    } else {
        dec = aac_open();
    }
    if (!data || !dec || (argc == 3 && !out)) {
        fprintf(stderr, "setup failed\n");
        free(data);
        aac_close(dec);
        media_close(media);
        if (out) fclose(out);
        return 1;
    }

    long pos = 0;
    int error = 0;
    probe_stats st = { .out = out };
    if (media) {
        const media_track *t = media_track_info(media, track);
        for (long i = 0; t && i < t->nsamples; i++) {
            media_sample s;
            if (media_get_sample(media, track, i, &s) != 1) {
                error = AUDIO_ERR_CORRUPT;
                break;
            }
            aacframe f;
            int got = 0;
            int used = aac_decode_raw(dec, s.data, s.size, &f, &got);
            if (used < 0) { error = used; break; }
            if (got && (error = take_frame(&st, &f)) != AUDIO_OK) break;
            pos++;
        }
    } else {
        while (pos < len) {
            aacframe f;
            int got = 0;
            int used = aac_decode(dec, data + pos, len - pos, &f, &got);
            if (used <= 0) { error = used ? used : AUDIO_ERR_CORRUPT; break; }
            pos += used;
            if (got && (error = take_frame(&st, &f)) != AUDIO_OK) break;
        }
    }
    if (out) fclose(out);
    printf("input=%s frames=%d rate=%d channels=%d samples=%ld sbr=%d "
           "nonfinite=%ld rms=%.9g consumed=%ld/%ld error=%d\n",
           media ? "mp4" : "adts", st.frames, st.rate, st.channels,
           st.samples, aac_had_sbr(dec), st.nonfinite,
           st.samples ? sqrt(st.energy / st.samples) : 0.0, pos,
           media ? media_track_info(media, track)->nsamples : len, error);
    free(data);
    aac_close(dec);
    media_close(media);
    return error || !st.frames || st.rate != 48000 || st.channels != 2 ||
           !st.samples || st.nonfinite ? 1 : 0;
}
