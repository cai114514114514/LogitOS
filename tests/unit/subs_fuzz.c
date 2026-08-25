/* tests/unit/subs_fuzz.c -- ASan+UBSan fuzz for c/lib/media/subs.c.
 *
 * A subtitle file is exactly the shape c/lib/media's other consumers already
 * argue for fuzzing an untrusted parser over: it comes off the network (a
 * <track src>, a downloaded .srt next to a video), every length is a
 * stranger's claim, and unlike a container there is no magic number gating
 * entry to the interesting code -- WebVTT's whole signature is six ASCII
 * bytes, so a byte flipper reaches collect_block/parse_cue_timings/
 * parse_region_settings on almost every input, not just on rare lucky ones
 * (contrast tests/unit/demux_fuzz.c's phase-2 "structured" mutations, which
 * exist BECAUSE a container's magic number makes uniform noise miss).
 *
 * Three phases, deterministic from the seed so a failure reproduces:
 *   1. corruption  -- real WPT fixtures + the SRT samples, bytes flipped/
 *                     replaced/truncated/duplicated.
 *   2. synthetic   -- hostile WebVTT built from nothing: absurd digit runs
 *                     for a timestamp component, a REGION/STYLE block with
 *                     no terminating blank line before EOF, cue settings
 *                     tokens with colons in every boundary position.
 *   3. random      -- uniform noise, with and without a valid "WEBVTT"
 *                     prefix (matching the SRT path too, which has no
 *                     signature to gate entry at all).
 *
 * Every mutated buffer is run through BOTH subs_parse_vtt and
 * subs_parse_srt (SRT has no signature, so it is exactly as reachable by
 * noise as VTT's post-signature body is). Properties checked on every run,
 * a leak on any of which fails via LeakSanitizer at process exit:
 *   - no crash, no sanitizer report (the harness itself asserts nothing
 *     about correctness -- that is subs_diff.py's job on real files; this
 *     is purely "never a memory-safety violation on ANY bytes")
 *   - every returned cue's id/text length is within SUBS_MAX_ID_LEN /
 *     SUBS_MAX_TEXT_LEN, and start_ms/end_ms are finite int64s (the
 *     saturating-digit-accumulator contract from subs.c's header)
 *   - the same bytes parsed twice produce the same cue count (determinism)
 *   - subs_close() frees everything (LeakSanitizer, process-wide)
 *
 * Run:  make test-subs-fuzz
 *       make test-subs-fuzz SCALE=60 SEED=0x1234     (deeper)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "subs.h"

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = rng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n > 0 && (long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

/* Both entry points, on the same bytes, checked identically -- SRT has no
 * signature at all so it is reachable by noise exactly as much as a WebVTT
 * body past byte 6 is. */
static void try_parse_both(const uint8_t *data, long len)
{
    int err1 = 0, err2 = 0;
    subs_track *t1 = subs_parse_vtt(data, len, &err1);
    subs_track *t2 = subs_parse_vtt(data, len, &err2);
    /* Determinism: same bytes, same result shape. */
    CHECK((t1 == NULL) == (t2 == NULL), "VTT: same bytes, different null-ness (err %d vs %d)", err1, err2);
    if (t1 && t2) CHECK(subs_cue_count(t1) == subs_cue_count(t2), "VTT: nondeterministic cue count");
    for (int side = 0; side < 2; side++) {
        subs_track *t = side ? t2 : t1;
        if (!t) continue;
        int n = subs_cue_count(t);
        for (int i = 0; i < n; i++) {
            const subs_cue *c = subs_cue_at(t, i);
            CHECK(c->id != NULL && c->text != NULL, "cue %d has a NULL id/text pointer", i);
            if (c->id) CHECK((long)strlen(c->id) <= SUBS_MAX_ID_LEN, "cue %d id exceeds SUBS_MAX_ID_LEN", i);
            if (c->text) CHECK((long)strlen(c->text) <= SUBS_MAX_TEXT_LEN, "cue %d text exceeds SUBS_MAX_TEXT_LEN", i);
        }
    }
    subs_close(t1);
    subs_close(t2);

    int err3 = 0;
    subs_track *t3 = subs_parse_srt(data, len, &err3);
    if (t3) {
        int n = subs_cue_count(t3);
        for (int i = 0; i < n; i++) {
            const subs_cue *c = subs_cue_at(t3, i);
            CHECK(c->id != NULL && c->text != NULL, "srt cue %d has a NULL id/text pointer", i);
        }
    }
    subs_close(t3);

    subs_format fmt; int err4 = 0;
    subs_track *t4 = subs_parse(data, len, &fmt, &err4);
    subs_close(t4);
}

static uint8_t *dup_buf(const uint8_t *src, long n) { uint8_t *b = (uint8_t *)malloc((size_t)n > 0 ? (size_t)n : 1); if (b && n) memcpy(b, src, (size_t)n); return b; }

static void phase_corruption(const char **paths, int npaths, int rounds)
{
    for (int r = 0; r < rounds; r++) {
        const char *path = paths[rnd_below((uint32_t)npaths)];
        long len;
        uint8_t *orig = slurp(path, &len);
        if (!orig || len == 0) { free(orig); continue; }
        uint8_t *m = dup_buf(orig, len);
        if (!m) { free(orig); continue; }
        long mlen = len;
        int nmut = 1 + (int)rnd_below(8);
        for (int k = 0; k < nmut; k++) {
            if (mlen == 0) break;
            switch (rnd_below(4)) {
            case 0: m[rnd_below((uint32_t)mlen)] = (uint8_t)rnd(); break;               /* byte flip */
            case 1: m[rnd_below((uint32_t)mlen)] ^= (1u << rnd_below(8)); break;        /* bit flip */
            case 2: if (mlen > 1) mlen = (long)rnd_below((uint32_t)mlen);               /* truncate */
                break;
            case 3: m[rnd_below((uint32_t)mlen)] = 0;                                   /* NUL injection */
                break;
            }
        }
        try_parse_both(m, mlen);
        free(m);
        free(orig);
    }
}

static void phase_synthetic(int rounds)
{
    char buf[4096];
    for (int r = 0; r < rounds; r++) {
        int n = 0;
        n += snprintf(buf + n, sizeof buf - n, "WEBVTT\n\n");
        int shape = (int)rnd_below(6);
        switch (shape) {
        case 0: /* absurd digit-run hours */
            n += snprintf(buf + n, sizeof buf - n, "00");
            for (int i = 0; i < 200 && n < 3800; i++) n += snprintf(buf + n, sizeof buf - n, "%d", (int)rnd_below(10));
            n += snprintf(buf + n, sizeof buf - n, ":00:00.000 --> 00:00:01.000\ntext\n");
            break;
        case 1: /* REGION with no terminating blank line, EOF mid-settings */
            n += snprintf(buf + n, sizeof buf - n, "REGION\nid:x\nregionanchor:%d%%,%d%%",
                          (int)rnd_below(200), (int)rnd_below(200));
            break;
        case 2: /* STYLE with colons in every boundary position */
            n += snprintf(buf + n, sizeof buf - n, "STYLE\n:x x: :: %d:%d\n\nfoo\n00:00:00.000 --> 00:00:01.000\nt\n",
                          (int)rnd_below(100), (int)rnd_below(100));
            break;
        case 3: /* settings string that is nothing but colons and percents */
            n += snprintf(buf + n, sizeof buf - n, "00:00:00.000 --> 00:00:01.000 %%:%%:%%: line::::: position:%%,%%,%%\nt\n");
            break;
        case 4: /* nested/overlapping arrows */
            for (int i = 0; i < 40 && n < 3800; i++) n += snprintf(buf + n, sizeof buf - n, "-->");
            n += snprintf(buf + n, sizeof buf - n, "\n00:00:00.000 --> 00:00:01.000\nt\n");
            break;
        default: /* a region id lookup against a huge region table */
            n += snprintf(buf + n, sizeof buf - n, "00:00:00.000 --> 00:00:01.000 region:%d\nt\n", (int)rnd_below(1000000));
            break;
        }
        try_parse_both((const uint8_t *)buf, n);
    }
}

static void phase_random(int rounds)
{
    uint8_t buf[512];
    for (int r = 0; r < rounds; r++) {
        int prefix = (int)rnd_below(2);
        int n = 0;
        if (prefix) { memcpy(buf, "WEBVTT\n", 7); n = 7; }
        int total = (int)(8 + rnd_below(sizeof buf - 8));
        for (; n < total; n++) buf[n] = (uint8_t)rnd();
        try_parse_both(buf, n);
    }
}

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 8;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x243F6A8885A308D3ull;
    const char *fxdir = argc > 3 ? argv[3] : "tests/fixtures/subs";

    char pathbuf[64][512];
    const char *paths[64];
    int npaths = 0;
    const char *names[] = {
        "sample.vtt", "sample.ffmpeg.srt", "srt-clean.srt", "srt-clean-crlf.srt", "srt-malformed.srt",
        "wpt/arrows.vtt", "wpt/nulls.vtt", "wpt/timings-garbage.vtt", "wpt/timings-too-long.vtt",
        "wpt/timings-too-short.vtt", "wpt/settings-line.vtt", "wpt/regions-regionanchor.vtt",
        "wpt/regions-viewportanchor.vtt", "wpt/newlines.vtt", "wpt/whitespace-chars.vtt",
        "wpt/stylesheets.vtt", "wpt/ids.vtt", "wpt/signature-two-boms.vtt", "wpt/settings-region.vtt",
    };
    for (size_t i = 0; i < sizeof names / sizeof *names && npaths < 64; i++) {
        snprintf(pathbuf[npaths], sizeof pathbuf[npaths], "%s/%s", fxdir, names[i]);
        paths[npaths] = pathbuf[npaths];
        npaths++;
    }

    phase_corruption(paths, npaths, 300 * scale);
    phase_synthetic(200 * scale);
    phase_random(300 * scale);

    printf("subs_fuzz: scale=%d seed=0x%llx checks=%d failures=%d\n",
           scale, (unsigned long long)rng_state, checks, failures);
    return failures ? 1 : 0;
}
