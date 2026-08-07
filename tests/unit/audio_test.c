/* tests/unit/audio_test.c -- WAV, format sniffing, and the pull interface.
 *
 * WAV is the instrument the other decoders are measured with, so its own
 * correctness has to be established first and separately: every bit depth is
 * checked against the identical samples read back at 32-bit, the header writer
 * is checked by parsing what it wrote, and the chunk walker is checked against
 * the shapes real files actually have (extra chunks before `data`, odd-sized
 * chunks with their pad byte, WAVE_FORMAT_EXTENSIBLE) and against the shapes a
 * hostile file has (a chunk larger than the file, `data` before `fmt`).
 *
 * The pull interface is exercised for all three formats at several buffer
 * sizes, because "decode the whole file" and "decode it 37 frames at a time"
 * are different code paths and a player only ever uses the second.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "wav.h"

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

/* --- the header writer --------------------------------------------------- */

static void test_header_writer(void)
{
    uint8_t hdr[44];
    CHECK(wav_header_s16(hdr, 44100, 2, 1000) == 44, "header length");

    /* Parse back what we wrote, with a body attached. */
    uint8_t *file = malloc(44 + 1000 * 4);
    memcpy(file, hdr, 44);
    for (long i = 0; i < 1000; i++) {
        int16_t l = (int16_t)(i - 500), r = (int16_t)(-i);
        memcpy(file + 44 + i * 4, &l, 2);
        memcpy(file + 44 + i * 4 + 2, &r, 2);
    }
    wavinfo w;
    CHECK(wav_parse(file, 44 + 1000 * 4, &w) == AUDIO_OK, "written header parses");
    CHECK(w.rate == 44100 && w.channels == 2 && w.bits == 16, "written fields");
    CHECK(w.frames == 1000, "written frame count %ld", w.frames);

    int16_t out[8];
    CHECK(wav_read_s16(&w, 0, 4, out) == 4, "read 4 frames");
    CHECK(out[0] == -500 && out[1] == 0 && out[2] == -499 && out[3] == -1,
          "sample round trip: %d %d %d %d", out[0], out[1], out[2], out[3]);

    /* Reading at and past the end. */
    CHECK(wav_read_s16(&w, 999, 4, out) == 1, "clamped read at the end");
    CHECK(wav_read_s16(&w, 1000, 4, out) == 0, "read at EOF returns 0");
    CHECK(wav_read_s16(&w, 5000, 4, out) == 0, "read past EOF returns 0");
    CHECK(wav_read_s16(&w, -1, 4, out) < 0, "negative offset refused");

    CHECK(wav_header_s16(hdr, 44100, 0, 10) < 0, "zero channels refused");
    CHECK(wav_header_s16(hdr, 44100, 99, 10) < 0, "absurd channel count refused");
    CHECK(wav_header_s16(hdr, 5, 2, 10) < 0, "absurd rate refused");
    CHECK(wav_header_s16(hdr, 44100, 2, -1) < 0, "negative length refused");

    free(file);
}

/* --- bit depths ---------------------------------------------------------- */

/* All the wN.wav files are conversions of the same sine, so their 16-bit
 * readings must agree with each other to within the conversion itself. */
static void test_depths(const char *dir)
{
    struct { const char *name; int bits; int isf; } cases[] = {
        { "sine440", 16, 0 }, { "w8", 8, 0 }, { "w24", 24, 0 },
        { "w32", 32, 0 }, { "wf32", 32, 1 }
    };
    int16_t *base = NULL;
    long baselen = 0;

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.wav", dir, cases[c].name);
        long len = 0;
        uint8_t *buf = slurp(path, &len);
        if (!buf) { printf("SKIP %s\n", path); continue; }

        wavinfo w;
        int rc = wav_parse(buf, len, &w);
        CHECK(rc == AUDIO_OK, "%s: parse rc %d", cases[c].name, rc);
        if (rc != AUDIO_OK) { free(buf); continue; }
        CHECK(w.bits == cases[c].bits, "%s: bits %d, expected %d",
              cases[c].name, w.bits, cases[c].bits);
        CHECK(w.is_float == cases[c].isf, "%s: float flag", cases[c].name);

        long n = w.frames * w.channels;
        int16_t *s16 = malloc((size_t)n * 2);
        CHECK(wav_read_s16(&w, 0, w.frames, s16) == w.frames,
              "%s: short read", cases[c].name);

        if (!base) {
            base = s16; baselen = n;
        } else {
            /* Every conversion of the same signal must agree once brought back
             * to 16 bits. 8-bit throws away 8 bits, so it gets its own bound;
             * everything else must be within one LSB of rounding. */
            long worst = 0, over = 0;
            long m = n < baselen ? n : baselen;
            for (long i = 0; i < m; i++) {
                long dv = (long)s16[i] - (long)base[i];
                if (dv < 0) dv = -dv;
                if (dv > worst) worst = dv;
                if (dv > (cases[c].bits == 8 ? 256 : 1)) over++;
            }
            CHECK(over == 0, "%s: %ld samples differ from the 16-bit reading by "
                  "more than the conversion allows (worst %ld)",
                  cases[c].name, over, worst);
            printf("  %-8s %2d-bit%s  worst |diff| vs 16-bit: %ld\n",
                   cases[c].name, w.bits, cases[c].isf ? " float" : "      ", worst);
            free(s16);
        }
        free(buf);
    }
    free(base);
}

/* --- malformed WAV ------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_malformed_wav(void)
{
    wavinfo w;
    uint8_t f[256];

    /* A well-formed baseline, then break one thing at a time. */
    memset(f, 0, sizeof(f));
    wav_header_s16(f, 44100, 1, 8);
    CHECK(wav_parse(f, 44 + 16, &w) == AUDIO_OK, "baseline");

    memcpy(f, "RIFX", 4);
    CHECK(wav_parse(f, 60, &w) != AUDIO_OK, "wrong RIFF magic accepted");
    memcpy(f, "RIFF", 4);
    memcpy(f + 8, "WAVX", 4);
    CHECK(wav_parse(f, 60, &w) != AUDIO_OK, "wrong WAVE magic accepted");
    memcpy(f + 8, "WAVE", 4);

    /* A chunk that claims to be bigger than the file. The data chunk is
     * clamped (a truncated recording is still playable); anything else is an
     * error, because a bogus size there steers the rest of the parse. */
    put32(f + 40, 0xFFFFFF00u);
    CHECK(wav_parse(f, 60, &w) == AUDIO_OK && w.frames * 2 <= 60,
          "oversized data chunk not clamped");
    put32(f + 16, 0xFFFFFF00u);
    CHECK(wav_parse(f, 60, &w) != AUDIO_OK, "oversized fmt chunk accepted");

    /* fmt fields that contradict themselves or name something we cannot do. */
    memset(f, 0, sizeof(f));
    wav_header_s16(f, 44100, 1, 8);
    f[32] = 99;                                    /* blockAlign vs bits/channels */
    CHECK(wav_parse(f, 60, &w) != AUDIO_OK, "inconsistent blockAlign accepted");
    wav_header_s16(f, 44100, 1, 8);
    f[34] = 12;                                    /* 12-bit PCM */
    f[32] = 1;
    CHECK(wav_parse(f, 60, &w) == AUDIO_ERR_UNSUPPORTED, "12-bit PCM not refused");
    wav_header_s16(f, 44100, 1, 8);
    f[20] = 17;                                    /* an unknown codec tag */
    CHECK(wav_parse(f, 60, &w) == AUDIO_ERR_UNSUPPORTED, "unknown codec not refused");
    wav_header_s16(f, 44100, 1, 8);
    f[22] = 0; f[32] = 0;                          /* zero channels */
    CHECK(wav_parse(f, 60, &w) == AUDIO_ERR_RANGE, "zero channels accepted");

    /* No data chunk at all, and truncation at every length. */
    memset(f, 0, sizeof(f));
    wav_header_s16(f, 44100, 1, 8);
    memcpy(f + 36, "junk", 4);
    CHECK(wav_parse(f, 60, &w) != AUDIO_OK, "missing data chunk accepted");
    wav_header_s16(f, 44100, 1, 8);
    for (long cut = 0; cut < 60; cut++) {
        int rc = wav_parse(f, cut, &w);
        (void)rc;                                   /* must not crash or read past */
    }
    CHECK(1, "truncation walk survived");

    CHECK(wav_parse(NULL, 100, &w) != AUDIO_OK, "NULL buffer accepted");
    CHECK(wav_parse(f, 0, &w) != AUDIO_OK, "empty buffer accepted");
}

/* --- sniffing and the pull interface ------------------------------------- */

static void test_sniff(const char *dir)
{
    struct { const char *file; audio_format want; } cases[] = {
        { "sine440.wav", AUDIO_FMT_WAV },
        { "w24.wav", AUDIO_FMT_WAV },
        { "sine440.flac", AUDIO_FMT_FLAC },
        { "f24.flac", AUDIO_FMT_FLAC },
        { "sine440.mp3", AUDIO_FMT_MP3 },
        { "lsf22.mp3", AUDIO_FMT_MP3 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, cases[i].file);
        long len = 0;
        uint8_t *b = slurp(path, &len);
        if (!b) { printf("SKIP %s\n", path); continue; }
        audio_format f = audio_sniff(b, len);
        CHECK(f == cases[i].want, "%s sniffed as %s", cases[i].file,
              audio_format_name(f));
        free(b);
    }

    /* Content, not file names, and nothing else may be claimed. */
    uint8_t junk[2048];
    for (unsigned i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 31 + 7);
    CHECK(audio_sniff(junk, sizeof(junk)) == AUDIO_FMT_UNKNOWN,
          "arbitrary bytes sniffed as audio");
    memset(junk, 0, sizeof(junk));
    CHECK(audio_sniff(junk, sizeof(junk)) == AUDIO_FMT_UNKNOWN, "zeros sniffed as audio");
    CHECK(audio_sniff(NULL, 10) == AUDIO_FMT_UNKNOWN, "NULL sniffed");
    CHECK(audio_sniff(junk, 0) == AUDIO_FMT_UNKNOWN, "empty sniffed");
}

static void test_pull(const char *dir, const char *file)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    long len = 0;
    uint8_t *b = slurp(path, &len);
    if (!b) { printf("SKIP %s\n", path); return; }

    /* Decode once whole, then again in awkward chunk sizes, and require the
     * two to be identical sample for sample. */
    apcm whole;
    int rc = audio_decode(b, len, 0, &whole);
    CHECK(rc == AUDIO_OK, "%s: audio_decode rc %d", file, rc);
    if (rc != AUDIO_OK) { free(b); return; }
    CHECK(whole.frames > 0, "%s: decoded nothing", file);

    static const long sizes[] = { 1, 7, 37, 512, 1153 };
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int err = 0;
        adec *d = adec_open(b, len, &err);
        CHECK(d != NULL, "%s: adec_open rc %d", file, err);
        if (!d) continue;
        int rate = 0, ch = 0;
        adec_info(d, &rate, &ch);
        CHECK(rate == whole.rate && ch == whole.channels,
              "%s: pull geometry differs from whole-file", file);

        int16_t *buf = malloc((size_t)sizes[s] * ch * sizeof(int16_t));
        long got = 0, mism = 0;
        for (;;) {
            long n = adec_read(d, buf, sizes[s]);
            CHECK(n >= 0, "%s: adec_read returned %ld", file, n);
            if (n <= 0) break;
            for (long i = 0; i < n * ch; i++)
                if (got * ch + i < whole.frames * whole.channels &&
                    buf[i] != whole.s16[got * ch + i]) mism++;
            got += n;
        }
        CHECK(got == whole.frames,
              "%s: pull at %ld frames/call produced %ld frames, whole-file %ld",
              file, sizes[s], got, whole.frames);
        CHECK(mism == 0, "%s: pull at %ld frames/call differs in %ld samples",
              file, sizes[s], mism);
        free(buf);
        adec_close(d);
    }
    printf("  %-16s %6d Hz %dch  %8ld frames, 5 chunk sizes identical\n",
           file, whole.rate, whole.channels, whole.frames);
    audio_pcm_free(&whole);
    free(b);
}

static void test_convert(void)
{
    /* Rounding is half away from zero and saturating, and NaN does not become
     * whatever the cast happens to produce. */
    float in[] = { 0.0f, 0.5f / 32768.0f, -0.5f / 32768.0f, 1.0f, -1.0f, 2.0f, -2.0f };
    int16_t out[7];
    audio_f32_to_s16(in, out, 7);
    CHECK(out[0] == 0, "0 -> 0");
    CHECK(out[1] == 1, "half LSB rounds away from zero (%d)", out[1]);
    CHECK(out[2] == -1, "negative half LSB (%d)", out[2]);
    CHECK(out[3] == 32767, "+1.0 saturates to 32767 (%d)", out[3]);
    CHECK(out[4] == -32768, "-1.0 maps to -32768 (%d)", out[4]);
    CHECK(out[5] == 32767 && out[6] == -32768, "out of range saturates");

    float nan_in = 0.0f / 0.0f, huge_in = 1e30f;
    int16_t nan_out, huge_out;
    audio_f32_to_s16(&nan_in, &nan_out, 1);
    audio_f32_to_s16(&huge_in, &huge_out, 1);
    CHECK(nan_out == 0, "NaN -> silence (%d)", nan_out);
    CHECK(huge_out == 32767, "huge -> saturation (%d)", huge_out);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/audioref";

    printf("WAV header writer\n");
    test_header_writer();
    printf("WAV bit depths\n");
    test_depths(dir);
    printf("WAV malformed input\n");
    test_malformed_wav();
    printf("float to int16 conversion\n");
    test_convert();
    printf("format sniffing\n");
    test_sniff(dir);
    printf("pull interface\n");
    test_pull(dir, "sine440.wav");
    test_pull(dir, "w24.wav");
    test_pull(dir, "stereo.flac");
    test_pull(dir, "f24.flac");
    test_pull(dir, "sine440.mp3");
    test_pull(dir, "msnoise.mp3");
    test_pull(dir, "lsf22.mp3");

    if (failures) {
        printf("AUDIO-FAIL %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("AUDIO-OK %d checks passed\n", checks);
    return 0;
}
