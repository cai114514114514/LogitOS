/* Host test for mini-libc's allocator (c/apps/libc/src/malloc.c).
 *
 * THE DELIVERABLE IS A CURVE, NOT A DURATION. The old allocator walked the
 * physical block chain from the arena base on every call, so with N live blocks
 * allocation N+1 walked N headers. Measured here, on the host, with the real
 * file (N blocks of 24-56 bytes, all kept live):
 *
 *      N        old        per-doubling ratio    ns/alloc
 *     12500     183 ms            -                14657
 *     25000     760 ms          x4.15              30416
 *     50000    3232 ms          x4.25              64641
 *    100000   12706 ms          x3.93             127060
 *    200000   54810 ms          x4.31             274052
 *
 * x4 per doubling is O(N^2) with no ambiguity, and QuickJS allocates every JS
 * object through this malloc, so a React-scale page lands squarely on it. The
 * assertion below is therefore on the RATIO: doubling N must roughly double the
 * time. It is written with tolerance so it still means something on a different
 * machine, and it FAILS against the old allocator -- that negative control is
 * the only thing that makes it a test rather than a thermometer:
 *
 *     git -c core.autocrlf=false clone --no-hardlinks . /tmp/oldmalloc
 *     cd /tmp/oldmalloc && git checkout <pre-fix-rev> -- c/apps/libc/src/malloc.c
 *     # build this same test against it, then:
 *     MALLOC_TEST_SCALE_ONLY=1 MALLOC_SCALE_N=4000 ./malloc_test    # it is slow
 *
 * Everything below the scaling phase passes against the OLD allocator too, on
 * purpose: they are non-regression checks, not new requirements. The three that
 * do not are the three deliberate behaviour changes, and they are called out
 * where they are asserted: shrinking realloc now hands the tail back, and an
 * absurd size (align16 wrapping to 0) no longer bricks the heap.
 *
 * Correctness is not secondary: mixed alloc/free, random and reverse free order,
 * realloc growing and shrinking, calloc zeroing, exhaustion and recovery, and --
 * because a free list keeps its links in the payload of freed blocks -- that a
 * clobbered header degrades to NULL instead of faulting. Fragmentation is
 * asserted not to regress: sequences that fit before must still fit.
 *
 * Run: make test-malloc
 *   env MALLOC_SCALE_N=<n>          base N of the scaling sweep (default 30000)
 *   env MALLOC_TEST_SCALE_ONLY=1    skip the correctness phases (negative control)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

/* malloc.c is compiled into this binary with its entry points renamed, because
 * a host process cannot have two mallocs. */
void  *lmalloc(size_t);
void   lfree(void *);
void  *lrealloc(void *, size_t);
void  *lcalloc(size_t, size_t);
size_t lmalloc_usable_size(void *);
extern size_t malloc_peak;

#ifndef ARENA_SIZE
#define ARENA_SIZE (24u * 1024u * 1024u)
#endif

static int fails;

static void ck(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf(cond ? "ok: " : "FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!cond) fails++;
}

static void ckq(int cond, const char *what)   /* quiet on success: loop bodies */
{
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static uint32_t rngstate = 0x12345678u;
static uint32_t rng(void)
{
    uint32_t x = rngstate;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rngstate = x;
}
static void rng_reset(void) { rngstate = 0x12345678u; }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- basics --- */

static void phase_basic(void)
{
    printf("\n--- basics ---\n");
    void *a = lmalloc(1), *b = lmalloc(1), *c = lmalloc(4000);
    ck(a && b && c, "small and large allocations succeed");
    ck(((uintptr_t)a % 16) == 0 && ((uintptr_t)b % 16) == 0 && ((uintptr_t)c % 16) == 0,
       "every payload is 16-byte aligned");
    ck(a != b && b != c, "distinct allocations get distinct addresses");
    ck(lmalloc_usable_size(a) >= 1 && lmalloc_usable_size(c) >= 4000,
       "malloc_usable_size is at least the requested size");
    ck(lmalloc_usable_size(NULL) == 0, "malloc_usable_size(NULL) is 0");
    lfree(NULL);
    ck(1, "free(NULL) is a no-op");
    lfree(a); lfree(b); lfree(c);

    void *z1 = lmalloc(0), *z2 = lmalloc(0);
    ck(z1 && z2 && z1 != z2, "malloc(0) returns distinct freeable pointers");
    lfree(z1); lfree(z2);

    ck(lmalloc(ARENA_SIZE) == NULL, "a request the arena cannot hold returns NULL");

    void *w = lmalloc(ARENA_SIZE - 4096);
    ck(w != NULL, "a near-whole-arena block fits at the start");
    lfree(w);
}

/* ------------------------------------------------- no-overlap + patterns --- */

#define MAXLIVE 40000
static struct { unsigned char *p; size_t sz; unsigned char pat; } live[MAXLIVE];
static int nlive;

static void live_add(unsigned char *p, size_t sz)
{
    /* the block we just got must not overlap any block we already hold */
    for (int i = 0; i < nlive; i++) {
        if (p < live[i].p + live[i].sz && live[i].p < p + sz) {
            printf("FAIL: OVERLAP: new [%p,+%zu) vs live #%d [%p,+%zu)\n",
                   (void *)p, sz, i, (void *)live[i].p, live[i].sz);
            fails++;
            return;
        }
    }
    unsigned char pat = (unsigned char)(rng() | 1);
    memset(p, pat, sz);
    live[nlive].p = p; live[nlive].sz = sz; live[nlive].pat = pat;
    nlive++;
}

static void live_drop(int i)
{
    for (size_t k = 0; k < live[i].sz; k++)
        if (live[i].p[k] != live[i].pat) {
            printf("FAIL: block #%d clobbered at +%zu (%02x != %02x)\n",
                   i, k, live[i].p[k], live[i].pat);
            fails++;
            break;
        }
    lfree(live[i].p);
    live[i] = live[--nlive];               /* swap-remove */
}

/* Overlap is the invariant that matters: two live blocks sharing a byte means
 * the allocator handed the same memory to two owners. The payload pattern check
 * catches the slower version of the same bug, where a header write lands inside
 * somebody's data. */
static void phase_mixed(const char *label, int rounds, size_t maxsz, int random_order)
{
    printf("\n--- mixed workload: %s ---\n", label);
    nlive = 0;
    int before = fails;
    for (int r = 0; r < rounds; r++) {
        if (nlive < MAXLIVE && (nlive == 0 || (rng() % 100) < 62)) {
            size_t sz = 1 + rng() % maxsz;
            unsigned char *p = lmalloc(sz);
            if (!p) {                       /* arena full: make room and carry on */
                int target = nlive / 2;
                while (nlive > target) live_drop((int)(rng() % (unsigned)nlive));
                continue;
            }
            ckq(lmalloc_usable_size(p) >= sz, "usable_size >= requested");
            live_add(p, sz);
        } else if (nlive) {
            live_drop(random_order ? (int)(rng() % (unsigned)nlive) : nlive - 1);
        }
    }
    while (nlive) live_drop(random_order ? (int)(rng() % (unsigned)nlive) : nlive - 1);
    ck(fails == before, "no overlapping live blocks and no clobbered payloads");

    /* everything is free again, so the arena must be one coalesced block */
    void *big = lmalloc(ARENA_SIZE - 64 * 1024);
    ck(big != NULL, "after draining, a near-arena-sized block still fits "
                    "(the free blocks coalesced back into one)");
    lfree(big);
}

/* --------------------------------------------------------------- realloc --- */

static void phase_realloc(void)
{
    printf("\n--- realloc / calloc ---\n");
    unsigned char *p = lmalloc(32);
    if (!p) { ck(0, "realloc phase could not even allocate 32 bytes"); return; }
    for (int i = 0; i < 32; i++) p[i] = (unsigned char)(i * 7 + 1);

    int content_ok = 1;
    size_t sz = 32;
    for (int step = 0; step < 12; step++) {           /* grow */
        size_t nsz = sz * 2 + 17;
        unsigned char *q = lrealloc(p, nsz);
        ckq(q != NULL, "realloc grow succeeded");
        if (!q) break;
        for (size_t i = 0; i < 32; i++)
            if (q[i] != (unsigned char)(i * 7 + 1)) content_ok = 0;
        p = q; sz = nsz;
    }
    ck(content_ok, "realloc growing preserves the original bytes");

    content_ok = 1;
    for (int step = 0; step < 12; step++) {           /* shrink */
        size_t nsz = sz / 2 + 1;
        if (nsz < 32) nsz = 32;
        unsigned char *q = lrealloc(p, nsz);
        ckq(q != NULL, "realloc shrink succeeded");
        if (!q) break;
        for (size_t i = 0; i < 32; i++)
            if (q[i] != (unsigned char)(i * 7 + 1)) content_ok = 0;
        p = q; sz = nsz;
    }
    ck(content_ok, "realloc shrinking preserves the surviving bytes");
    lfree(p);

    void *rn = lrealloc(NULL, 100);
    ck(rn != NULL, "realloc(NULL, n) behaves as malloc");
    lfree(rn);
    void *r = lmalloc(100);
    ck(lrealloc(r, 0) == NULL, "realloc(p, 0) frees and returns NULL");

    /* Shrinking must actually hand the tail back, or a shrink loop is a leak.
     * With the arena carved into exactly two blocks there is no slack to hide
     * behind: the 1 MiB request can only succeed out of what the shrink freed. */
    void *big = lmalloc(2 * 1024 * 1024);
    void *rest = lmalloc(ARENA_SIZE - 2 * 1024 * 1024 - 4096);
    ck(big && rest, "carved the arena into a 2 MiB block and the remainder");
    ck(lmalloc(1024 * 1024) == NULL, "with the arena carved up, 1 MiB does not fit");
    void *shrunk = lrealloc(big, 64);
    ck(shrunk == big, "realloc shrinking in place returns the same pointer");
    void *reclaimed = lmalloc(1024 * 1024);
    ck(reclaimed != NULL, "and 1 MiB now fits: the shrink returned the tail to the heap");
    lfree(reclaimed); lfree(shrunk); lfree(rest);

    unsigned char *cz = lcalloc(300, 7);
    int zero = cz != NULL;
    for (int i = 0; cz && i < 300 * 7; i++) if (cz[i]) zero = 0;
    ck(cz && zero, "calloc zeroes what it returns");
    lfree(cz);
    ck(lcalloc((size_t)1 << 40, (size_t)1 << 40) == NULL, "calloc overflow returns NULL");
}

/* ----------------------------------------------- exhaustion and recovery --- */

static void phase_exhaust(void)
{
    printf("\n--- exhaustion / recovery / fragmentation ---\n");
    size_t cap = ARENA_SIZE / 1024 + 64;
    void **v = malloc(sizeof(void *) * cap);
    size_t n = 0, per = 1000;

    while (n < cap) { void *q = lmalloc(per); if (!q) break; v[n++] = q; }
    ck(n > 0 && n < cap, "filling with %zu-byte blocks ends in a clean NULL", per);

    /* utilisation: a 1000-byte request rounds to 1008 and carries a 16-byte
     * header, so a healthy allocator gets within a few percent of the ideal. */
    size_t ideal = ARENA_SIZE / (1008 + 16);
    printf("    filled %zu blocks of %zu (ideal %zu, %.1f%%)\n",
           n, per, ideal, 100.0 * (double)n / (double)ideal);
    ck(n * 100 >= ideal * 97, "arena utilisation is within 3%% of the ideal block count");

    for (size_t i = 0; i < n; i++) lfree(v[i]);
    void *whole = lmalloc(ARENA_SIZE - 4096);
    ck(whole != NULL, "after freeing everything, an almost-whole-arena block fits again");
    lfree(whole);

    /* Fragmentation non-regression, deterministic and arena-relative: three
     * interleaved size classes, then the two larger ones freed, then a re-fill
     * with a size that only fits if the freed neighbours coalesced. */
    const size_t cyc[3] = { 24, 512, 40000 };
    size_t triple = 24 + 512 + 40000 + 3 * 16;
    size_t triples = (size_t)(ARENA_SIZE * 0.6) / triple;
    if (triples > 900) triples = 900;
    size_t idx = 0;
    for (size_t i = 0; i < triples * 3 && idx < cap; i++) {
        void *q = lmalloc(cyc[i % 3]);
        if (!q) break;
        v[idx++] = q;
    }
    size_t kept = idx;
    ck(kept == triples * 3, "laid down %zu interleaved blocks", triples * 3);
    for (size_t i = 0; i < kept; i++) if (i % 3 != 0) { lfree(v[i]); v[i] = NULL; }
    size_t refilled = 0;
    for (size_t i = 0; i < triples && kept + i < cap; i++) {
        void *q = lmalloc(30000);
        if (!q) break;
        v[kept + refilled++] = q;
    }
    printf("    re-filled %zu of %zu 30000-byte blocks into the freed holes\n",
           refilled, triples);
    ck(refilled * 100 >= triples * 95,
       "freed holes coalesce well enough to re-fill with larger blocks");
    for (size_t i = 0; i < kept + refilled; i++) lfree(v[i]);
    free(v);

    /* and after all of that churn the arena coalesces back to whole */
    void *w2 = lmalloc(ARENA_SIZE - 4096);
    ck(w2 != NULL, "the whole arena is one free block again after the churn");
    lfree(w2);
}

/* ------------------------------------------------------ peak accounting ---- */

static void phase_peak(void)
{
    printf("\n--- malloc_peak accounting ---\n");
    enum { NP = 3000 };
    static void *v[NP];
    size_t before = malloc_peak;
    for (int i = 0; i < NP; i++) v[i] = lmalloc(4096);
    ck(malloc_peak >= before, "malloc_peak never goes backwards");
    ck(malloc_peak >= (size_t)NP * 4096, "malloc_peak covers a %d x 4096 live set", NP);
    for (int i = 0; i < NP; i++) lfree(v[i]);
    size_t held = malloc_peak;
    void *tiny = lmalloc(16);
    ck(malloc_peak == held, "freeing, then allocating small, does not raise the peak");
    lfree(tiny);
}

/* ------------------------------------------------------------- scaling ----- */

/* The workload the old allocator was quadratic on: allocate N small blocks and
 * hold every one of them. Returns the best of `reps` passes, in ms -- the
 * minimum, because scheduler noise can only ever make a pass slower. */
static double scale_hold(size_t N, int reps, void **tab)
{
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        rng_reset();
        double t0 = now_ms();
        for (size_t i = 0; i < N; i++) tab[i] = lmalloc(24 + (rng() % 33));
        double dt = now_ms() - t0;
        if (tab[N - 1] == NULL) { printf("FAIL: arena too small for N=%zu\n", N); fails++; }
        for (size_t i = 0; i < N; i++) lfree(tab[i]);
        if (dt < best) best = dt;
    }
    return best;
}

/* Same, but half the blocks are freed part-way, so the second half must come off
 * the free lists rather than off the untouched tail of the arena. */
static double scale_reuse(size_t N, int reps, void **tab)
{
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        rng_reset();
        double t0 = now_ms();
        for (size_t i = 0; i < N; i++) tab[i] = lmalloc(24 + (rng() % 33));
        for (size_t i = 0; i < N; i += 2) { lfree(tab[i]); tab[i] = NULL; }
        for (size_t i = 0; i < N; i += 2) tab[i] = lmalloc(24 + (rng() % 33));
        double dt = now_ms() - t0;
        for (size_t i = 0; i < N; i++) lfree(tab[i]);
        if (dt < best) best = dt;
    }
    return best;
}

static void scaling_case(const char *label, double (*fn)(size_t, int, void **),
                         size_t base, int reps)
{
    size_t Ns[4] = { base, base * 2, base * 4, base * 8 };
    double ms[4];
    void **tab = malloc(sizeof(void *) * Ns[3]);

    printf("\n--- scaling: %s ---\n", label);
    fn(base / 4 + 1, 1, tab);                       /* warm-up, untimed */
    for (int i = 0; i < 4; i++) {
        ms[i] = fn(Ns[i], reps, tab);
        printf("    N=%-8zu %9.2f ms   %8.0f ns/alloc", Ns[i], ms[i],
               ms[i] * 1e6 / (double)Ns[i]);
        if (i) printf("   ratio x%.2f", ms[i] / (ms[i - 1] > 0.0 ? ms[i - 1] : 1e-9));
        printf("\n");
    }
    free(tab);

    /* Assert the SHAPE. The tolerances are wide enough to survive a loaded
     * machine and a different CPU, and still nowhere near the x4 per doubling
     * the old allocator produced. */
    for (int i = 1; i < 4; i++) {
        double r = ms[i] / (ms[i - 1] > 0.0 ? ms[i - 1] : 1e-9);
        ck(r < 2.80, "%s: doubling N from %zu to %zu multiplied time by x%.2f "
                     "(linear x2, quadratic x4; must be < x2.80)",
           label, Ns[i - 1], Ns[i], r);
    }
    double overall = ms[3] / (ms[0] > 0.0 ? ms[0] : 1e-9);
    ck(overall < 14.0, "%s: 8x the work cost x%.2f end to end "
                       "(linear x8, quadratic x64; must be < x14)", label, overall);
}

/* --------------------------------------------------- corruption tolerance -- */

/* An allocator must never fault. Clobber a header the way a one-block buffer
 * overflow would and require the allocator to stay inside its arena: it may
 * refuse to allocate, it may leak the damaged region, it must not crash and it
 * must not hand out a pointer overlapping a live block. Runs LAST, because it
 * deliberately damages the heap. */
static void phase_corrupt(void)
{
    printf("\n--- hostile input / corruption tolerance (runs last: it damages the heap) ---\n");

    /* align16((size_t)-1) wraps to 0. The old allocator then "allocated" a
     * zero-size block AT THE ARENA BASE and wrote size 0 into its header -- which
     * is the end-of-heap sentinel value, so every later malloc saw an empty heap
     * and returned NULL. One bad size bricked the heap for the life of the
     * process. It is checked here, after everything else, precisely because the
     * old allocator cannot survive it and the negative control still has to be
     * able to reach the phases that follow it. */
    ck(lmalloc((size_t)-1) == NULL, "an absurd size returns NULL, it does not wrap to 0");
    void *live_after = lmalloc(64);
    ck(live_after != NULL, "and the heap still works afterwards (an absurd size does "
                           "not brick it)");
    lfree(live_after);

    unsigned char *a = lmalloc(64);
    unsigned char *b = lmalloc(64);
    unsigned char *c = lmalloc(64);
    if (!a || !b || !c || b < a) { ck(0, "corruption setup allocated three blocks"); return; }

    size_t span = (size_t)(b - a) + 64;               /* through b's header into b */
    memset(a, 0xA5, span);
    ck(1, "wrote 0xA5 through the next block's header without crashing");

    size_t us = lmalloc_usable_size(b);
    ck(us == 0 || us <= ARENA_SIZE,
       "malloc_usable_size on a clobbered block returns without faulting (%zu)", us);
    lfree(b);
    ck(1, "free() of a clobbered block returns without faulting");
    void *d = lmalloc(64);
    ck(d == NULL || ((uintptr_t)d % 16) == 0,
       "malloc after corruption either fails cleanly or returns a sane pointer");
    if (d)
        ck((unsigned char *)d + 64 <= a || (unsigned char *)d >= a + span,
           "and it does not hand back memory that overlaps the still-live block");
    void *e = lrealloc(c, 4096);
    ck(e == NULL || ((uintptr_t)e % 16) == 0, "realloc after corruption is also safe");

    void *f = lmalloc(256);
    if (f) {
        lfree((unsigned char *)f + 32);
        ck(1, "free() of an interior pointer is a no-op, not a crash");
    }
    /* double free must not corrupt the lists either */
    void *g = lmalloc(128);
    if (g) { lfree(g); lfree(g); ck(1, "double free() is a no-op, not list corruption"); }
}

int main(void)
{
    /* line-buffered: if a phase dies, the log must still show which one */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("mini-libc malloc: arena %u bytes (%.0f MiB)\n",
           (unsigned)ARENA_SIZE, ARENA_SIZE / 1048576.0);

    int scale_only = getenv("MALLOC_TEST_SCALE_ONLY") != NULL;
    if (!scale_only) {
        phase_basic();
        phase_mixed("LIFO free order, <=256 byte blocks", 120000, 256, 0);
        phase_mixed("random free order, <=256 byte blocks", 120000, 256, 1);
        phase_mixed("random free order, <=16 KiB blocks", 40000, 16384, 1);
        phase_realloc();
        phase_exhaust();
        phase_peak();
    }

    const char *env = getenv("MALLOC_SCALE_N");
    size_t base = env ? (size_t)strtoul(env, NULL, 10) : 30000;
    if (base < 500) base = 500;
    scaling_case("hold N live", scale_hold, base, 3);
    scaling_case("free half, re-allocate", scale_reuse, base, 3);

    if (!scale_only) phase_corrupt();

    printf("\n%s (%d failure%s)\n", fails ? "MALLOC TEST FAILED" : "MALLOC TEST PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
