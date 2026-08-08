/* Fuzz harness for every image decoder in c/lib/image + the Rust staticlib
 * (PNG/APNG, GIF, JPEG, SVG, BMP, ICO, WebP, EXIF).
 *
 * Images arrive off the network and are handed to hand-written parsers, which
 * is the classic remote-code-execution surface; this is the test that says the
 * parsers survive input nobody meant them to see.
 *
 * What it does, per iteration: take a seed file from the generated corpora,
 * mutate it (byte flips, run overwrites, splices between two seeds, truncation,
 * and targeted corruption of the 32-bit big-endian length fields that PNG and
 * ICO use as offsets), then run BOTH entry points -- img_decode and
 * img_decode_anim -- and free the result. Allocation is counted, so a decoder
 * that returns -1 having already allocated its output is a failure here even
 * when nothing crashes.
 *
 * It is built with -fsanitize=address,undefined AND -fno-sanitize-recover=all.
 * That second flag is not decoration: without it UBSan PRINTS the undefined
 * behaviour and carries on, the process exits 0, and the run reports clean --
 * a fuzz target that cannot fail. Which is exactly the hole the audio line
 * found in its own harness today. `make test-img-fuzz-negctl` compiles this
 * file with -DIMG_SABOTAGE and requires the run to FAIL, so the wiring is
 * checked rather than assumed.
 *
 * Usage: img_fuzz <iterations> <corpus-dir>...
 * Env:   IMG_FUZZ_SEED (default 1) -- every failure is reproducible from it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "img.h"

/* ---- allocator with a live count, so leaks are caught even without ASan ---- */
static long outstanding, peak_alloc;

void *kmalloc(unsigned long n)
{
#if IMG_SABOTAGE == 1
    /* NEGATIVE CONTROL: hand back a buffer one byte short of what was asked
     * for. Every decoder then writes one byte past its own allocation, which
     * is precisely the bug class this harness exists to catch, so ASan must
     * abort and the runner must report failure. If this run comes back clean,
     * the harness is a green light wired to nothing. */
    if (n > 16) n -= 1;
#endif
    void *p = malloc(n ? n : 1);
    if (p) { outstanding++; if (outstanding > peak_alloc) peak_alloc = outstanding; }
    return p;
}

void kfree(void *p) { if (p) outstanding--; free(p); }

/* ---- xorshift, so a failing iteration is reproducible from the seed ---- */
static uint64_t rngstate = 1;
static uint32_t rnd(void)
{
    rngstate ^= rngstate << 13;
    rngstate ^= rngstate >> 7;
    rngstate ^= rngstate << 17;
    return (uint32_t)(rngstate >> 16);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

/* ---- corpus ---- */
#define MAXSEEDS 256
static uint8_t *seed[MAXSEEDS];
static long seedlen[MAXSEEDS];
static char seedname[MAXSEEDS][256];
static int nseeds;

static void add_seed(const char *path)
{
    if (nseeds >= MAXSEEDS) return;
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (8 << 20)) { fclose(f); return; }
    uint8_t *b = malloc(n);
    if (b && fread(b, 1, n, f) == (size_t)n) {
        seed[nseeds] = b; seedlen[nseeds] = n;
        snprintf(seedname[nseeds], sizeof seedname[0], "%s", path);
        nseeds++;
    } else free(b);
    fclose(f);
}

static int interesting(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ok[] = { ".png", ".gif", ".jpg", ".jpeg", ".bmp",
                                ".ico", ".webp", ".svg", 0 };
    for (int i = 0; ok[i]; i++) if (strcmp(dot, ok[i]) == 0) return 1;
    return 0;
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!interesting(e->d_name)) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        add_seed(p);
    }
    closedir(d);
}

/* ---- one decode of both entry points, with the allocation balance checked --- */
static long fail_count, run_count;

static void run_once(const uint8_t *buf, int n, const char *what)
{
    long before = outstanding;
    struct image im;
    run_count++;
    if (img_decode(buf, n, &im) == 0) img_free(&im);
    if (outstanding != before) {
        printf("LEAK  img_decode %s n=%d delta=%ld\n", what, n, outstanding - before);
        fail_count++;
        outstanding = before;
    }

    before = outstanding;
    struct img_anim a;
    if (img_decode_anim(buf, n, &a) == 0) img_anim_free(&a);
    if (outstanding != before) {
        printf("LEAK  img_decode_anim %s n=%d delta=%ld\n", what, n, outstanding - before);
        fail_count++;
        outstanding = before;
    }
}

/* ---- mutation ---- */
static void mutate(uint8_t *b, long n)
{
    int rounds = 1 + rnd_below(6);
    for (int r = 0; r < rounds; r++) {
        switch (rnd_below(6)) {
        case 0:                                  /* single byte flip */
            b[rnd_below((uint32_t)n)] ^= 1u << rnd_below(8);
            break;
        case 1:                                  /* random byte */
            b[rnd_below((uint32_t)n)] = (uint8_t)rnd();
            break;
        case 2: {                                /* overwrite a run */
            uint32_t o = rnd_below((uint32_t)n);
            uint32_t len = 1 + rnd_below(64);
            uint8_t v = (uint8_t)rnd();
            for (uint32_t i = 0; i < len && o + i < (uint32_t)n; i++) b[o + i] = v;
            break;
        }
        case 3: {                                /* corrupt a 32-bit BE field */
            uint32_t o = rnd_below((uint32_t)(n > 4 ? n - 4 : 1));
            uint32_t v = rnd();
            /* Values near the extremes are where the sign/overflow bugs are. */
            if (rnd_below(2)) v = 0x7fffffffu + rnd_below(4);
            b[o] = v >> 24; b[o+1] = v >> 16; b[o+2] = v >> 8; b[o+3] = v;
            break;
        }
        case 4: {                                /* splice in another seed */
            int s = rnd_below((uint32_t)nseeds);
            uint32_t o = rnd_below((uint32_t)n);
            uint32_t len = 1 + rnd_below(128);
            for (uint32_t i = 0; i < len && o + i < (uint32_t)n && i < (uint32_t)seedlen[s]; i++)
                b[o + i] = seed[s][i];
            break;
        }
        default: {                               /* swap two bytes */
            uint32_t a = rnd_below((uint32_t)n), c = rnd_below((uint32_t)n);
            uint8_t t = b[a]; b[a] = b[c]; b[c] = t;
            break;
        }
        }
    }
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? strtol(argv[1], 0, 10) : 20000;
    for (int i = 2; i < argc; i++) scan_dir(argv[i]);
    if (nseeds == 0) { printf("img_fuzz: no seeds found\n"); return 1; }

    const char *s = getenv("IMG_FUZZ_SEED");
    rngstate = s ? strtoull(s, 0, 10) : 1;
    if (rngstate == 0) rngstate = 1;
    printf("img_fuzz: %d seeds, %ld iterations, rng seed %llu\n",
           nseeds, iters, (unsigned long long)rngstate);

#if IMG_SABOTAGE == 2
    /* NEGATIVE CONTROL for the UBSan half: signed overflow. With
     * -fno-sanitize-recover=all this aborts; without it, it prints a line and
     * the run still exits 0. */
    {
        volatile int big = 2147483647;
        volatile int one = 1;
        printf("sabotage: %d\n", big + one);
    }
#endif

    /* 1. every seed at its full length and at truncated prefixes -- this is the
     *    shape real input takes, because res_fetch cuts images at 64 KiB. */
    for (int i = 0; i < nseeds; i++) {
        long L = seedlen[i];
        long cuts[] = { L, L - 1, L / 2, L / 3, L / 7, 65536, 1000, 256, 64, 33, 16, 8, 4, 1 };
        for (unsigned c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
            if (cuts[c] <= 0 || cuts[c] > L) continue;
            run_once(seed[i], (int)cuts[c], seedname[i]);
        }
    }

    /* 2. mutation rounds */
    long maxlen = 0;
    for (int i = 0; i < nseeds; i++) if (seedlen[i] > maxlen) maxlen = seedlen[i];
    uint8_t *work = malloc(maxlen);
    if (!work) { printf("oom\n"); return 1; }

    for (long it = 0; it < iters; it++) {
        int i = (int)rnd_below((uint32_t)nseeds);
        long n = seedlen[i];
        memcpy(work, seed[i], n);
        mutate(work, n);
        if (rnd_below(4) == 0) n = 1 + rnd_below((uint32_t)n);   /* also truncate */
        run_once(work, (int)n, seedname[i]);
        if ((it & 0x3ff) == 0x3ff) { printf("."); fflush(stdout); }
    }
    printf("\n");

    free(work);
    for (int i = 0; i < nseeds; i++) free(seed[i]);
    printf("img_fuzz: %ld decodes, peak %ld live allocations, %ld failures\n",
           run_count, peak_alloc, fail_count);
    if (outstanding != 0) {
        printf("img_fuzz: %ld allocations still outstanding at exit\n", outstanding);
        fail_count++;
    }
    printf(fail_count ? "IMG FUZZ FAILED\n" : "IMG FUZZ CLEAN\n");
    return fail_count ? 1 : 0;
}
