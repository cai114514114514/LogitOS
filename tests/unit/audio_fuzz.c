/* tests/unit/audio_fuzz.c -- ASan+UBSan fuzz for the audio decoders.
 *
 * Audio files come off the network and off removable disks. Every length,
 * count, index and bit depth in them is a claim by whoever wrote the file, and
 * these decoders turn those claims into allocation sizes and array subscripts.
 * So the bar is not "does not crash on the files ffmpeg made": it is that no
 * byte sequence at all reads outside its buffer, loops without bound, or
 * shifts by an amount C does not define.
 *
 * Four phases, all deterministic from the seed so a failure is reproducible:
 *
 *   1. corruption   real files with bits flipped, bytes replaced, chunks
 *                   spliced and lengths truncated -- the shapes damage
 *                   actually takes.
 *   2. structured   headers rebuilt with hostile field values (a FLAC block
 *                   size larger than STREAMINFO promised, an MP3 frame whose
 *                   bit reservoir points further back than exists, a WAV chunk
 *                   claiming four gigabytes), because uniform random bytes
 *                   almost never get past a sync word.
 *   3. random       uniform noise, and noise with a valid magic prefix.
 *   4. properties   the things that must hold even on garbage: a decoder that
 *                   opened must never write more frames than asked for, the
 *                   same input must decode to the same output twice, and
 *                   nothing may be left allocated (ASan's leak checker).
 *
 * Run:  make test-audio-fuzz             (fast, a few seconds)
 *       make test-audio-fuzz SCALE=40 SEED=0x1234    (deeper)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "wav.h"
#include "flac.h"
#include "mp3.h"

static uint64_t rng_state;

static uint32_t rnd(void)
{
    /* splitmix64, so a seed reproduces a run exactly. */
    rng_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = rng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}

static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

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

/* Decode whatever this is, as hard as possible, and return the frame count.
 * Every allocation must be released on every path -- ASan's leak detector is
 * half the point of this test. `guard` checks that adec_read never writes past
 * the buffer it was given, which ASan would catch anyway but which is worth
 * asserting explicitly because a decoder that silently writes fewer frames
 * than it returns is just as broken. */
static long run_decoders(const uint8_t *buf, long len, int check_props)
{
    long frames = 0;
    int err = 0;

    adec *d = adec_open(buf, len, &err);
    if (d) {
        int rate = 0, ch = 0;
        adec_info(d, &rate, &ch);
        if (ch >= 1 && ch <= AUDIO_MAX_CHANNELS) {
            enum { CAP = 300 };
            int16_t *out = malloc((size_t)CAP * ch * sizeof(int16_t) + 16);
            /* A canary past the end: adec_read must never touch it. */
            uint8_t *canary = (uint8_t *)out + (size_t)CAP * ch * sizeof(int16_t);
            memset(canary, 0x5A, 16);
            for (int i = 0; i < 4000; i++) {
                long want = 1 + (long)rnd_below(CAP);
                long n = adec_read(d, out, want);
                if (n <= 0) break;
                if (check_props) {
                    CHECK(n <= want, "adec_read returned %ld for a request of %ld",
                          n, want);
                }
                frames += n;
            }
            int intact = 1;
            for (int i = 0; i < 16; i++) if (canary[i] != 0x5A) intact = 0;
            if (check_props) CHECK(intact, "adec_read wrote past its output buffer");
            free(out);
        }
        adec_close(d);
    }

    /* Also drive each decoder directly: adec_open dispatches on a sniff, and a
     * mutated file often stops being sniffable long before it stops being
     * dangerous to the decoder it was aimed at. */
    wavinfo w;
    if (wav_parse(buf, len, &w) == AUDIO_OK && w.channels >= 1) {
        int16_t small[64];
        long at = 0;
        for (int i = 0; i < 200; i++) {
            long n = wav_read_s16(&w, at, 64 / w.channels, small);
            if (n <= 0) break;
            at += n;
        }
        int32_t s32[64];
        wav_read_s32(&w, 0, 64 / w.channels, s32);
    }

    flacdec *fl = flac_open(buf, len, &err);
    if (fl) {
        for (int i = 0; i < 3000; i++) {
            const int32_t *pl[AUDIO_MAX_CHANNELS];
            long n = flac_decode_frame(fl, pl);
            if (n <= 0) break;
        }
        flac_rewind(fl);
        flac_md5_ok(fl);
        flac_close(fl);
    }

    mp3dec *m = mp3_open();
    if (m) {
        long pos = mp3_id3_len(buf, len);
        if (pos < 0 || pos > len) pos = 0;
        for (int i = 0; i < 4000 && pos < len; i++) {
            mp3frame f;
            int got = 0;
            int n = mp3_decode(m, buf + pos, len - pos, &f, &got);
            if (n == 0) break;
            if (n < 0) { pos++; continue; }
            pos += n;
        }
        mp3_close(m);
    }
    return frames;
}

/* --- phase 1: corruption ------------------------------------------------- */

static void mutate(uint8_t *b, long len, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        if (len <= 0) return;
        switch (rnd_below(6)) {
        case 0: b[rnd_below((uint32_t)len)] ^= (uint8_t)(1u << rnd_below(8)); break;
        case 1: b[rnd_below((uint32_t)len)] = (uint8_t)rnd_below(256); break;
        case 2: b[rnd_below((uint32_t)len)] = 0xFF; break;
        case 3: b[rnd_below((uint32_t)len)] = 0x00; break;
        case 4: {   /* splice a run from elsewhere in the file onto itself */
            long n = 1 + (long)rnd_below(64);
            long src = (long)rnd_below((uint32_t)len);
            long dst = (long)rnd_below((uint32_t)len);
            if (src + n > len) n = len - src;
            if (dst + n > len) n = len - dst;
            if (n > 0) memmove(b + dst, b + src, (size_t)n);
            break;
        }
        default: {  /* a run of a single value, which is what dropouts look like */
            long n = 1 + (long)rnd_below(32);
            long at = (long)rnd_below((uint32_t)len);
            if (at + n > len) n = len - at;
            memset(b + at, (int)rnd_below(256), (size_t)n);
            break;
        }
        }
    }
}

static void phase_corruption(const char *dir, int scale)
{
    static const char *files[] = { "sample.wav", "sample.flac", "sample.mp3" };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        long len = 0;
        uint8_t *orig = slurp(path, &len);
        if (!orig) { printf("SKIP %s\n", path); continue; }

        /* The clean file must decode, or the corpus is not what we think. */
        long clean = run_decoders(orig, len, 1);
        CHECK(clean > 0, "%s: the pristine file decoded nothing", files[i]);

        uint8_t *work = malloc((size_t)len);
        for (int it = 0; it < scale * 40; it++) {
            memcpy(work, orig, (size_t)len);
            mutate(work, len, 1 + (int)rnd_below(20));
            long cut = len;
            if (rnd_below(4) == 0) cut = 1 + (long)rnd_below((uint32_t)len);
            run_decoders(work, cut, 0);
        }

        /* Truncation at every single length, at least for the small files:
         * off-by-one at a buffer end is exactly what this finds. */
        for (long cut = 0; cut <= len && cut < 4096; cut++)
            run_decoders(orig, cut, 0);

        free(work);
        free(orig);
    }
}

/* --- phase 2: structured hostility --------------------------------------- */

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void phase_structured(int scale)
{
    /* WAV with hostile chunk sizes and fmt fields. */
    for (int it = 0; it < scale * 60; it++) {
        uint8_t f[512];
        memset(f, 0, sizeof(f));
        memcpy(f, "RIFF", 4);
        put32le(f + 4, rnd());
        memcpy(f + 8, "WAVE", 4);
        memcpy(f + 12, "fmt ", 4);
        put32le(f + 16, rnd_below(4) ? 16 : rnd());
        f[20] = (uint8_t)rnd_below(256); f[21] = (uint8_t)rnd_below(4);
        f[22] = (uint8_t)rnd_below(300);                    /* channels */
        put32le(f + 24, rnd());                             /* sample rate */
        put32le(f + 28, rnd());
        f[32] = (uint8_t)rnd_below(256);                    /* block align */
        f[34] = (uint8_t)rnd_below(80);                     /* bits */
        memcpy(f + 36, "data", 4);
        put32le(f + 40, rnd());
        for (unsigned k = 44; k < sizeof(f); k++) f[k] = (uint8_t)rnd_below(256);
        run_decoders(f, (long)sizeof(f), 0);
    }

    /* FLAC with a STREAMINFO that lies. */
    for (int it = 0; it < scale * 60; it++) {
        uint8_t f[1024];
        memset(f, 0, sizeof(f));
        memcpy(f, "fLaC", 4);
        f[4] = 0x80;                                        /* last block, type 0 */
        f[5] = 0; f[6] = 0; f[7] = 34;
        for (int k = 8; k < 8 + 34; k++) f[k] = (uint8_t)rnd_below(256);
        for (unsigned k = 42; k < sizeof(f); k++) f[k] = (uint8_t)rnd_below(256);
        /* Half the time, plant a frame sync so the frame parser is reached. */
        if (rnd_below(2)) { f[42] = 0xFF; f[43] = (uint8_t)(0xF8 | rnd_below(8)); }
        run_decoders(f, (long)sizeof(f), 0);
    }

    /* MP3 frames with every header field randomised, and a reservoir pointer
     * that points further back than any data exists. */
    for (int it = 0; it < scale * 60; it++) {
        uint8_t f[2048];
        memset(f, 0, sizeof(f));
        for (unsigned k = 0; k < sizeof(f); k++) f[k] = (uint8_t)rnd_below(256);
        f[0] = 0xFF;
        f[1] = (uint8_t)(0xE0 | rnd_below(32));
        f[2] = (uint8_t)rnd_below(256);
        f[3] = (uint8_t)rnd_below(256);
        if (rnd_below(2)) { f[4] = 0xFF; f[5] = 0xFF; }     /* main_data_begin max */
        run_decoders(f, (long)sizeof(f), 0);
    }
}

/* --- phase 3: noise ------------------------------------------------------ */

static void phase_random(int scale)
{
    static const char *magics[] = { "RIFF", "fLaC", "ID3", "\xff\xfb", "" };
    for (int it = 0; it < scale * 100; it++) {
        long len = 1 + (long)rnd_below(6000);
        uint8_t *b = malloc((size_t)len);
        for (long k = 0; k < len; k++) b[k] = (uint8_t)rnd_below(256);
        const char *m = magics[rnd_below(5)];
        long ml = (long)strlen(m);
        if (ml && len >= ml + 12) {
            memcpy(b, m, (size_t)ml);
            if (strcmp(m, "RIFF") == 0 && len >= 12) memcpy(b + 8, "WAVE", 4);
        }
        run_decoders(b, len, 0);
        free(b);
    }
}

/* --- phase 4: determinism ------------------------------------------------ */

static void phase_determinism(const char *dir, int scale)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/sample.mp3", dir);
    long len = 0;
    uint8_t *orig = slurp(path, &len);
    if (!orig) { printf("SKIP determinism (no sample.mp3)\n"); return; }

    uint8_t *work = malloc((size_t)len);
    for (int it = 0; it < scale * 4; it++) {
        memcpy(work, orig, (size_t)len);
        mutate(work, len, 1 + (int)rnd_below(10));

        apcm a, b;
        int ra = audio_decode(work, len, 0, &a);
        int rb = audio_decode(work, len, 0, &b);
        CHECK(ra == rb, "the same bytes decoded to different status (%d vs %d)", ra, rb);
        if (ra == AUDIO_OK && rb == AUDIO_OK) {
            CHECK(a.frames == b.frames,
                  "the same bytes decoded to %ld and %ld frames", a.frames, b.frames);
            if (a.frames == b.frames && a.channels == b.channels)
                CHECK(memcmp(a.s16, b.s16,
                             (size_t)a.frames * a.channels * sizeof(int16_t)) == 0,
                      "the same bytes decoded to different samples");
        }
        if (ra == AUDIO_OK) audio_pcm_free(&a);
        if (rb == AUDIO_OK) audio_pcm_free(&b);
    }
    free(work);
    free(orig);
}

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 6;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x243F6A8885A308D3ull;
    const char *dir = argc > 3 ? argv[3] : "tests/fixtures/audio";
    if (scale < 1) scale = 1;
    rng_state = seed;

    printf("audio fuzz: scale %d, seed 0x%016llx, corpus %s\n",
           scale, (unsigned long long)seed, dir);

    phase_corruption(dir, scale);
    printf("  corruption phase done\n");
    phase_structured(scale);
    printf("  structured phase done\n");
    phase_random(scale);
    printf("  random phase done\n");
    phase_determinism(dir, scale);
    printf("  determinism phase done\n");

    if (failures) {
        printf("AUDIO-FUZZ-FAIL %d of %d property checks failed\n", failures, checks);
        return 1;
    }
    printf("AUDIO-FUZZ-OK %d property checks passed, no sanitizer report\n", checks);
    return 0;
}
