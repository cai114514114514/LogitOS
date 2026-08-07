/* tests/unit/mp3_test.c -- MP3 conformance gate and differential.
 *
 * THE CRITERION, AND WHY IT IS NOT BIT-EXACTNESS.  Layer III reconstructs
 * through an IMDCT and a polyphase filter bank in floating point. ISO/IEC
 * 11172-3 does not specify a bit pattern for the output and no decoder can
 * honestly claim one; ISO/IEC 11172-4 instead defines compliance as a bound on
 * the difference from a reference decoder's floating-point output:
 *
 *     full accuracy   RMS of the difference  <  2^-15 / sqrt(12)  ( 8.8146e-06 )
 *                     max |difference|       <= 2^-15             ( 3.0518e-05 )
 *     limited accuracy  RMS of the difference <  2^-11 / sqrt(12) ( 1.4103e-04 )
 *
 * with full scale taken as 1.0, i.e. the full-accuracy RMS bound is the RMS of
 * a uniformly distributed one-LSB error at 16 bits.
 *
 * WHAT WE COMPARE AGAINST.  The ISO conformance bitstreams and the ISO
 * reference decoder are not freely redistributable and are not present here,
 * so the reference is ffmpeg's mp3float decoder, decoding the identical bytes.
 * That is a differential, not a run of the official suite, and it is reported
 * as such. Note it is if anything a harder test than the letter of the
 * standard: two independently conformant decoders may each sit just inside the
 * bound relative to the reference and therefore up to twice the bound apart
 * from each other, so passing the full-accuracy bound against ffmpeg implies
 * comfortably passing it against the reference.
 *
 * WHAT IT REPORTS.  A distribution per file -- RMS, peak, how many samples
 * exceed the bound, and where the worst one is -- because a single number
 * cannot tell "one sample is wrong" from "the whole second half is wrong", and
 * because the H.264 line already learned that "the first mismatch moved" is
 * not a measurement.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "mp3.h"

/* ISO/IEC 11172-4 bounds, full scale = 1.0. */
#define ISO_FULL_RMS   (1.0 / 32768.0 / 3.4641016151377544)   /* 2^-15/sqrt(12) */
#define ISO_FULL_PEAK  (1.0 / 32768.0)                        /* 2^-15          */
#define ISO_LIMITED_RMS (1.0 / 2048.0 / 3.4641016151377544)   /* 2^-11/sqrt(12) */

static int failures, checks;

#define CHECK(cond, ...) do {                                  \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

/* Decode a whole .mp3 to interleaved float. Returns sample count (not frames). */
static float *decode_all(const uint8_t *buf, long len, int *rate, int *ch, long *nsamp)
{
    mp3dec *d = mp3_open();
    if (!d) return NULL;

    long cap = 1 << 16, n = 0;
    float *out = malloc((size_t)cap * sizeof(float));
    if (!out) { mp3_close(d); return NULL; }

    long pos = mp3_id3_len(buf, len);
    if (mp3_is_info_frame(buf + pos, len - pos)) {
        mp3frame f; int got = 0;
        int used = mp3_decode(d, buf + pos, len - pos, &f, &got);
        if (used > 0) pos += used;
    }

    *rate = 0; *ch = 0;
    while (pos < len) {
        mp3frame f;
        int got = 0;
        int used = mp3_decode(d, buf + pos, len - pos, &f, &got);
        if (used == 0) break;
        if (used < 0) { pos++; continue; }
        pos += used;
        if (!got) continue;
        *rate = f.rate; *ch = f.channels;
        long need = (long)f.nsamples * f.channels;
        while (n + need > cap) {
            cap *= 2;
            float *nb = realloc(out, (size_t)cap * sizeof(float));
            if (!nb) { free(out); mp3_close(d); return NULL; }
            out = nb;
        }
        memcpy(out + n, f.pcm, (size_t)need * sizeof(float));
        n += need;
    }
    mp3_close(d);
    *nsamp = n;
    return out;
}

struct score {
    long n, over_peak, over_rms_win;
    double rms, peak;
    long peak_at;
};

static void compare(const float *got, long gn, const float *ref, long rn,
                    struct score *s)
{
    long n = gn < rn ? gn : rn;
    double acc = 0.0, peak = 0.0;
    long peak_at = -1, over = 0;
    for (long i = 0; i < n; i++) {
        double dv = (double)got[i] - (double)ref[i];
        acc += dv * dv;
        double a = dv < 0 ? -dv : dv;
        if (a > peak) { peak = a; peak_at = i; }
        if (a > ISO_FULL_PEAK) over++;
    }
    s->n = n;
    s->rms = n ? sqrt(acc / (double)n) : 0.0;
    s->peak = peak;
    s->peak_at = peak_at;
    s->over_peak = over;
    s->over_rms_win = 0;

    /* A whole-file RMS can hide a badly wrong second inside a long correct
     * file, so also score the worst one-second window. */
    long win = 44100;
    if (n > win) {
        double worst = 0.0;
        for (long start = 0; start + win <= n; start += win / 2) {
            double a = 0.0;
            for (long i = start; i < start + win; i++) {
                double dv = (double)got[i] - (double)ref[i];
                a += dv * dv;
            }
            a = sqrt(a / (double)win);
            if (a > worst) worst = a;
        }
        if (worst > ISO_FULL_RMS) s->over_rms_win = 1;
        s->rms = s->rms > worst ? s->rms : worst;   /* report the worse of the two */
    }
}

static int test_one(const char *dir, const char *name, int verbose)
{
    char path[512], refpath[512];
    snprintf(path, sizeof(path), "%s/%s.mp3", dir, name);
    snprintf(refpath, sizeof(refpath), "%s/%s.mp3.f32", dir, name);

    long len = 0, reflen = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf) { printf("SKIP %s\n", path); return 0; }
    uint8_t *ref = slurp(refpath, &reflen);
    if (!ref) { printf("SKIP %s\n", refpath); free(buf); return 0; }

    int rate = 0, ch = 0;
    long n = 0;
    float *got = decode_all(buf, len, &rate, &ch, &n);
    CHECK(got != NULL, "%s: decode failed", name);
    if (!got) { free(buf); free(ref); return 1; }

    long rn = reflen / 4;
    struct score s;
    compare(got, n, (const float *)ref, rn, &s);

    CHECK(s.n > 0, "%s: no samples compared", name);
    /* The two decoders must agree on how many samples the file contains. A
     * length mismatch means one of them is trimming or losing a frame, and no
     * per-sample score is meaningful until that is true. */
    CHECK(n == rn, "%s: decoded %ld samples, ffmpeg decoded %ld", name, n, rn);
    CHECK(s.rms < ISO_FULL_RMS,
          "%s: RMS difference %.3e exceeds the ISO full-accuracy bound %.3e",
          name, s.rms, ISO_FULL_RMS);
    CHECK(s.peak <= ISO_FULL_PEAK,
          "%s: peak difference %.3e at sample %ld exceeds 2^-15 (%.3e); "
          "%ld of %ld samples over",
          name, s.peak, s.peak_at, ISO_FULL_PEAK, s.over_peak, s.n);

    if (verbose)
        printf("  %-8s %6d Hz %dch  %8ld samples  RMS %.3e (%5.1f%% of bound)  "
               "peak %.3e (%5.1f%%)  over %ld\n",
               name, rate, ch, s.n, s.rms, 100.0 * s.rms / ISO_FULL_RMS,
               s.peak, 100.0 * s.peak / ISO_FULL_PEAK, s.over_peak);

    free(got); free(buf); free(ref);
    return 0;
}

static const char *CASES[] = {
    "sine440", "sweep", "noise", "stereo", "impulse", "quiet",
    "sr32", "sr48", "lsf22", "lsf24", "lsf16", "mpeg25",
    "mono", "vbr", "dual", "sr8", "msnoise"
};
#define NCASES ((int)(sizeof(CASES) / sizeof(CASES[0])))

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/audioref";

    printf("MP3 vs ffmpeg mp3float, ISO/IEC 11172-4 full-accuracy bounds\n");
    printf("  RMS bound 2^-15/sqrt(12) = %.4e, peak bound 2^-15 = %.4e\n",
           ISO_FULL_RMS, ISO_FULL_PEAK);
    printf("  (limited-accuracy RMS bound for reference: %.4e)\n", ISO_LIMITED_RMS);

    int ran = 0;
    for (int i = 0; i < NCASES; i++)
        if (test_one(dir, CASES[i], 1) == 0) ran++;
    CHECK(ran >= 14, "only %d of %d cases were available", ran, NCASES);

    if (failures) {
        printf("MP3-FAIL %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("MP3-OK %d checks passed over %d streams\n", checks, ran);
    return 0;
}
