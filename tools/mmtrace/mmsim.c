/* mmsim -- replay a LogitOS page-reference trace against several replacement
 * policies, one of which is the offline optimum, and report the gap.
 *
 * ===========================================================================
 * THE QUESTION
 *
 * c/kernel/mm/reclaim.c chooses its victim with a clock (second chance) over
 * physical frames. reclaim.h argues for that choice against an active/inactive
 * LRU and gives the reason -- with no hardware reference notification, a
 * recency list can only be built by the same accessed-bit sampling the clock's
 * sweep already is, so the lists would add bookkeeping without adding
 * information. That argument is about LRU. It says nothing about how far
 * either of them is from the best a policy could possibly do.
 *
 * Page replacement is one of the few systems problems with an exact offline
 * optimum: Belady's MIN evicts the page whose next reference is furthest in
 * the future. It cannot be implemented online, but on a RECORDED trace it can
 * be computed exactly. So the gap between this kernel and perfection is a
 * measurable number rather than an opinion, and this program measures it.
 *
 * ===========================================================================
 * WHAT A "MISS" IS HERE, STATED ONCE
 *
 * A miss is a reference to a page that is not resident. Every policy sees the
 * identical reference string and the identical number of frames, so the counts
 * are directly comparable.
 *
 * Misses split into two kinds and the split matters more than the total:
 *
 *   COMPULSORY -- the first reference to a page. No policy can avoid it; MIN
 *                 takes exactly as many as the clock does. On a workload that
 *                 touches each page once, EVERY policy is optimal and a ratio
 *                 near 1.00 means nothing at all.
 *   CAPACITY   -- a reference to a page that was resident and got evicted.
 *                 This is the only number a replacement policy controls, and
 *                 it is the one to compare.
 *
 * A report that quotes only the total will flatter whichever policy was fed
 * the most compulsory-heavy workload, which is why both are always printed.
 *
 * ===========================================================================
 * THE POLICIES
 *
 *   opt     Belady's MIN. Exact, via an indexed max-heap over the resident set
 *           keyed by each page's next reference.
 *   lru     True LRU. Not implementable on this hardware (see above) but the
 *           right reference point: if the clock is already at LRU, then the
 *           remaining gap is LRU-to-MIN, which is a different research problem
 *           from clock-to-LRU.
 *   fifo    Evict the page resident longest. The floor a policy must beat.
 *   clock   WHAT THIS KERNEL ACTUALLY DOES, modelled from reclaim.c:
 *           a hand sweeping FRAME numbers in ascending order with wraparound,
 *           a persistent hand position, one reference bit per frame set on
 *           every access and cleared only by the hand. Frames are handed out
 *           by a next-fit scan from a rotating hint, which is what
 *           c/kernel/mm/pmm.c does -- so allocation order and sweep order are
 *           correlated exactly as they are on the machine, and the model is
 *           not a textbook clock that happens to share a name.
 *   clockwm The same clock plus reclaim.c's WATERMARKS: nothing is evicted
 *           until free frames fall below low (total/32), and then a whole
 *           batch is taken until free reaches high (total/16). This is the
 *           shipped behaviour; `clock` is the same policy stripped of the
 *           batching, so the difference between the two is the price of the
 *           batching alone.
 *   rand    Uniform random victim. Present because it is the honest floor:
 *           any policy that cannot beat it is not doing anything.
 *
 * ===========================================================================
 * THE CONTROL THAT MUST BE RUN FIRST
 *
 *   mmsim --synth=seq,pages=N+1  through N frames
 *
 * A sequential scan of N+1 pages through N frames is the classic worst case
 * for LRU, FIFO and clock alike: each of them evicts precisely the page that
 * will be needed next, so they miss on EVERY reference, while MIN misses about
 * once per N. If this program does not reproduce that enormous gap on that
 * input, its MIN is wrong and every other number it prints is worthless.
 *
 * The mirror control is --synth=rand: on uniform random access every policy,
 * including MIN, converges to nearly the same miss rate. A harness that shows
 * the clock losing badly there is measuring something other than policy.
 *
 * Both are asserted by `make test-mmsim`.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmtrace_fmt.h"

#define INF 0xFFFFFFFFu

static void die(const char *m) { fprintf(stderr, "mmsim: %s\n", m); exit(1); }
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "mmsim: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

/* --------------------------------------------------------- the trace --- */
/* seq[i] holds the page id in the low 30 bits and the access kind in the top
 * two, so the shape analysis can ask "which references did the clock lose, and
 * were they code or data" without a second array the length of the trace. */
#define ID(x)   ((x) & 0x3FFFFFFFu)
#define KIND(x) ((x) >> 30)

static uint32_t *seq;          /* the reference string */
static uint64_t  nref;
static uint32_t  npages;

/* key -> dense id, open addressing. The key is (space << 40) | vpn. */
static uint64_t *ht_key;
static uint32_t *ht_val;
static uint64_t  ht_cap, ht_mask;
static uint64_t *pg_key;       /* dense id -> key, for reporting */
static uint32_t  pg_cap;

static void ht_init(uint64_t cap)
{
    ht_cap = 1; while (ht_cap < cap * 2) ht_cap <<= 1;
    ht_mask = ht_cap - 1;
    ht_key = xmalloc(ht_cap * 8);
    ht_val = xmalloc(ht_cap * 4);
    for (uint64_t i = 0; i < ht_cap; i++) ht_key[i] = ~0ull;
    pg_cap = 1 << 16;
    pg_key = xmalloc((size_t)pg_cap * 8);
}

static uint32_t intern(uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    uint64_t i = (h >> 32) & ht_mask;
    for (;;) {
        if (ht_key[i] == ~0ull) {
            ht_key[i] = key;
            ht_val[i] = npages;
            if (npages == pg_cap) {
                pg_cap *= 2;
                pg_key = realloc(pg_key, (size_t)pg_cap * 8);
                if (!pg_key) die("out of memory growing the page table");
            }
            pg_key[npages] = key;
            return npages++;
        }
        if (ht_key[i] == key) return ht_val[i];
        i = (i + 1) & ht_mask;
    }
}

/* An all-zero header means the emulator was killed before its exit callback
 * ran -- the records are all there and only the eight magic bytes are missing.
 * Refusing such a file discards a whole recording over metadata, so it is
 * accepted with a warning. Anything else that is not the magic is refused:
 * reading an arbitrary file as 16-byte records would produce a plausible
 * reference string out of noise, which is worse than an error. */
static void check_hdr(const struct mmt_hdr *h, const char *path)
{
    if (!memcmp(h->magic, MMT_MAGIC, 8)) {
        if (h->recsize != sizeof(struct mmt_rec)) die("record size mismatch");
        return;
    }
    const unsigned char *b = (const unsigned char *)h;
    for (size_t i = 0; i < sizeof *h; i++)
        if (b[i]) { fprintf(stderr, "mmsim: %s is not an mmtrace file\n", path); exit(1); }
    fprintf(stderr, "mmsim: %s has a blank header (the emulator was killed "
                    "before it could write one); reading it anyway\n", path);
}

/* ------------------------------------------------------- the policies --- */

struct result {
    const char *name;
    uint64_t    misses;         /* every reference to a non-resident page */
    uint64_t    compulsory;     /* of those, first-ever references */
    uint64_t    evictions;
    uint64_t    scanned;        /* clock only: frames the hand looked at */
    uint8_t    *missbits;       /* one bit per reference, for the shape pass */
};

static void mark(struct result *r, uint64_t i)
{ if (r->missbits) r->missbits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static int  got(const struct result *r, uint64_t i)
{ return r->missbits && (r->missbits[i >> 3] >> (i & 7)) & 1; }

/* Every policy shares this frame model, because the thing being compared is
 * the choice of victim and nothing else. `frame_of[page]` is the frame holding
 * a page or NONE; `page_at[frame]` is the inverse. Frames are handed out by
 * pmm.c's actual rule -- a next-fit scan from a rotating hint -- so that the
 * clock's sweep order stands in the same relation to allocation order that it
 * does on the machine. */
#define NONE 0xFFFFFFFFu

struct mem {
    uint32_t  n;
    uint32_t  free;
    uint32_t  hint;
    uint32_t *page_at;
    uint32_t *frame_of;
};

static void mem_init(struct mem *m, uint32_t n, uint32_t np)
{
    m->n = n; m->free = n; m->hint = 0;
    m->page_at  = xmalloc((size_t)n * 4);
    m->frame_of = xmalloc((size_t)np * 4);
    for (uint32_t i = 0; i < n; i++)  m->page_at[i] = NONE;
    for (uint32_t i = 0; i < np; i++) m->frame_of[i] = NONE;
}
static void mem_done(struct mem *m) { free(m->page_at); free(m->frame_of); }

static uint32_t mem_alloc(struct mem *m)      /* pmm.c next-fit; NONE if full */
{
    if (!m->free) return NONE;
    for (uint32_t k = 0; k < m->n; k++) {
        uint32_t f = m->hint + k; if (f >= m->n) f -= m->n;
        if (m->page_at[f] == NONE) { m->hint = f + 1 >= m->n ? 0 : f + 1; return f; }
    }
    return NONE;
}
static void mem_put(struct mem *m, uint32_t f, uint32_t p)
{ m->page_at[f] = p; m->frame_of[p] = f; m->free--; }
static void mem_evict(struct mem *m, uint32_t f)
{ m->frame_of[m->page_at[f]] = NONE; m->page_at[f] = NONE; m->free++; }

/* --- Belady's MIN ---------------------------------------------------------
 *
 * An indexed binary max-heap holding exactly the resident pages, keyed by the
 * index of each page's next reference (INF for "never again"). A reference to
 * a resident page raises its key, which is a sift-DOWN from its position; the
 * victim is the root. O(log frames) per reference and O(frames) of memory,
 * rather than the usual lazy heap that grows to the length of the trace. */
struct oheap {
    uint32_t *page, *key, *pos, n;
};
static void oh_swap(struct oheap *h, uint32_t a, uint32_t b)
{
    uint32_t pa = h->page[a], pb = h->page[b], ka = h->key[a];
    h->page[a] = pb; h->key[a] = h->key[b]; h->pos[pb] = a;
    h->page[b] = pa; h->key[b] = ka;        h->pos[pa] = b;
}
static void oh_up(struct oheap *h, uint32_t i)
{
    while (i && h->key[(i - 1) / 2] < h->key[i]) { oh_swap(h, i, (i - 1) / 2); i = (i - 1) / 2; }
}
static void oh_down(struct oheap *h, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && h->key[l] > h->key[m]) m = l;
        if (r < h->n && h->key[r] > h->key[m]) m = r;
        if (m == i) return;
        oh_swap(h, i, m); i = m;
    }
}

static void run_opt(struct result *r, uint32_t nframes, const uint32_t *next)
{
    struct mem m; mem_init(&m, nframes, npages);
    struct oheap h;
    h.page = xmalloc((size_t)nframes * 4);
    h.key  = xmalloc((size_t)nframes * 4);
    h.pos  = xmalloc((size_t)npages * 4);
    h.n = 0;
    for (uint32_t i = 0; i < npages; i++) h.pos[i] = NONE;

    uint8_t *seen = xmalloc(npages); memset(seen, 0, npages);

    for (uint64_t i = 0; i < nref; i++) {
        uint32_t p = ID(seq[i]);
        if (m.frame_of[p] != NONE) {
            uint32_t j = h.pos[p];
            h.key[j] = next[i];
            oh_up(&h, j); oh_down(&h, j);   /* the key only ever rises */
            continue;
        }
        r->misses++; mark(r, i);
        if (!seen[p]) { seen[p] = 1; r->compulsory++; }

        uint32_t f = mem_alloc(&m);
        if (f == NONE) {
            uint32_t victim = h.page[0];
            /* A page never referenced again (key INF) is always at the root,
             * which is exactly right and is most of what MIN evicts. */
            oh_swap(&h, 0, h.n - 1);
            h.pos[h.page[h.n - 1]] = NONE;
            h.n--;
            if (h.n) oh_down(&h, 0);
            mem_evict(&m, m.frame_of[victim]);
            r->evictions++;
            f = mem_alloc(&m);
        }
        mem_put(&m, f, p);
        h.page[h.n] = p; h.key[h.n] = next[i]; h.pos[p] = h.n; h.n++;
        oh_up(&h, h.n - 1);
    }
    free(seen); free(h.page); free(h.key); free(h.pos); mem_done(&m);
}

/* --- true LRU: an intrusive list over pages, most recent at the head ----- */
static void run_lru(struct result *r, uint32_t nframes)
{
    struct mem m; mem_init(&m, nframes, npages);
    uint32_t *prev = xmalloc((size_t)npages * 4), *nxt = xmalloc((size_t)npages * 4);
    uint32_t head = NONE, tail = NONE;
    uint8_t *seen = xmalloc(npages); memset(seen, 0, npages);

    #define UNLINK(p) do { \
        if (prev[p] != NONE) nxt[prev[p]] = nxt[p]; else head = nxt[p]; \
        if (nxt[p]  != NONE) prev[nxt[p]] = prev[p]; else tail = prev[p]; } while (0)
    #define PUSH(p) do { prev[p] = NONE; nxt[p] = head; \
        if (head != NONE) { prev[head] = p; } \
        head = p; \
        if (tail == NONE) { tail = p; } } while (0)

    for (uint64_t i = 0; i < nref; i++) {
        uint32_t p = ID(seq[i]);
        if (m.frame_of[p] != NONE) { UNLINK(p); PUSH(p); continue; }
        r->misses++; mark(r, i);
        if (!seen[p]) { seen[p] = 1; r->compulsory++; }
        uint32_t f = mem_alloc(&m);
        if (f == NONE) {
            uint32_t victim = tail;
            UNLINK(victim);
            mem_evict(&m, m.frame_of[victim]);
            r->evictions++;
            f = mem_alloc(&m);
        }
        mem_put(&m, f, p); PUSH(p);
    }
    #undef UNLINK
    #undef PUSH
    free(prev); free(nxt); free(seen); mem_done(&m);
}

/* --- FIFO ---------------------------------------------------------------- */
static void run_fifo(struct result *r, uint32_t nframes)
{
    struct mem m; mem_init(&m, nframes, npages);
    uint32_t *q = xmalloc((size_t)nframes * 4);
    uint32_t qh = 0, qt = 0, qn = 0;
    uint8_t *seen = xmalloc(npages); memset(seen, 0, npages);

    for (uint64_t i = 0; i < nref; i++) {
        uint32_t p = ID(seq[i]);
        if (m.frame_of[p] != NONE) continue;
        r->misses++; mark(r, i);
        if (!seen[p]) { seen[p] = 1; r->compulsory++; }
        uint32_t f = mem_alloc(&m);
        if (f == NONE) {
            uint32_t victim = q[qh]; qh = (qh + 1) % nframes; qn--;
            mem_evict(&m, m.frame_of[victim]);
            r->evictions++;
            f = mem_alloc(&m);
        }
        mem_put(&m, f, p);
        q[qt] = p; qt = (qt + 1) % nframes; qn++;
    }
    (void)qn;
    free(q); free(seen); mem_done(&m);
}

/* --- random -------------------------------------------------------------- */
static uint64_t rng_s = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{ rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return (uint32_t)(rng_s >> 32); }

static void run_rand(struct result *r, uint32_t nframes)
{
    struct mem m; mem_init(&m, nframes, npages);
    uint8_t *seen = xmalloc(npages); memset(seen, 0, npages);
    for (uint64_t i = 0; i < nref; i++) {
        uint32_t p = ID(seq[i]);
        if (m.frame_of[p] != NONE) continue;
        r->misses++; mark(r, i);
        if (!seen[p]) { seen[p] = 1; r->compulsory++; }
        uint32_t f = mem_alloc(&m);
        if (f == NONE) { mem_evict(&m, rnd() % nframes); r->evictions++; f = mem_alloc(&m); }
        mem_put(&m, f, p);
    }
    free(seen); mem_done(&m);
}

/* --- the clock, as c/kernel/mm/reclaim.c runs it -------------------------
 *
 * `batch` selects between the two shapes:
 *   0  demand: evict exactly one frame at the moment one is needed. This
 *      isolates the replacement POLICY, which is the thing a learned model
 *      would replace.
 *   1  watermarks: reclaim_on_alloc()'s rule -- do nothing while free >= low,
 *      and when free falls below it sweep until free reaches high. That is
 *      what the kernel ships, and the difference between the two rows is the
 *      cost of the batching rather than of the policy. */
static void run_clock(struct result *r, uint32_t nframes, int batch)
{
    struct mem m; mem_init(&m, nframes, npages);
    uint8_t *ref = xmalloc(nframes); memset(ref, 0, nframes);
    uint8_t *seen = xmalloc(npages); memset(seen, 0, npages);
    uint32_t hand = 0;
    /* reclaim_init(): low = total/32, high = total/16, floored so that a tiny
     * simulated machine still has a cushion of at least one frame. */
    uint32_t low  = nframes / 32; if (batch && low  < 1) low  = 1;
    uint32_t high = nframes / 16; if (batch && high <= low) high = low + 1;

    for (uint64_t i = 0; i < nref; i++) {
        uint32_t p = ID(seq[i]);
        if (m.frame_of[p] != NONE) { ref[m.frame_of[p]] = 1; continue; }
        r->misses++; mark(r, i);
        if (!seen[p]) { seen[p] = 1; r->compulsory++; }

        /* The trigger, then the allocation, in reclaim_on_alloc()'s order. */
        uint32_t want = 0;
        if (batch) { if (m.free < low) want = high - m.free; }
        else       { if (m.free == 0)  want = 1; }

        uint32_t freed = 0;
        /* RECLAIM_ALLOC_BUDGET, scaled: the kernel's 16384 is 2x a 32 MiB
         * machine and a fraction of a big one, so it is expressed here as a
         * multiple of memory rather than as a constant that would mean
         * something different at every simulated size. */
        uint64_t budget = (uint64_t)nframes * 2;
        while (freed < want && budget--) {
            uint32_t f = hand; hand = (hand + 1 >= nframes) ? 0 : hand + 1;
            r->scanned++;
            if (m.page_at[f] == NONE) continue;    /* skip_unmapped: no chain */
            if (ref[f]) { ref[f] = 0; continue; }  /* the second chance */
            mem_evict(&m, f);
            r->evictions++;
            freed++;
        }

        uint32_t f = mem_alloc(&m);
        if (f == NONE) {
            /* fault_frame(): the allocation failed, so force a pass and ask
             * again. Unbounded here for the same reason it is bounded on the
             * machine only by a budget -- it must not return empty-handed
             * while anything is reclaimable. */
            while (m.free == 0) {
                uint32_t g = hand; hand = (hand + 1 >= nframes) ? 0 : hand + 1;
                r->scanned++;
                if (m.page_at[g] == NONE) continue;
                if (ref[g]) { ref[g] = 0; continue; }
                mem_evict(&m, g); r->evictions++;
            }
            f = mem_alloc(&m);
        }
        mem_put(&m, f, p);
        ref[f] = 1;
    }
    free(ref); free(seen); mem_done(&m);
}

/* --------------------------------------------------------- the driver --- */

static uint32_t *build_next(void)
{
    uint32_t *next = xmalloc((size_t)nref * 4);
    uint32_t *last = xmalloc((size_t)npages * 4);
    for (uint32_t i = 0; i < npages; i++) last[i] = INF;
    for (uint64_t i = nref; i-- > 0; ) {
        uint32_t p = ID(seq[i]);
        next[i] = last[p];
        last[p] = (uint32_t)i;
    }
    free(last);
    return next;
}

struct result R[8];
static int nR;

static struct result *add(const char *name, int shape)
{
    struct result *r = &R[nR++];
    memset(r, 0, sizeof *r);
    r->name = name;
    if (shape) { r->missbits = xmalloc((nref + 7) / 8); memset(r->missbits, 0, (nref + 7) / 8); }
    return r;
}

static void report(uint32_t nframes, int csv)
{
    uint64_t optcap = R[0].misses - R[0].compulsory;
    if (csv) {
        for (int i = 0; i < nR; i++)
            printf("csv,%u,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   nframes, R[i].name, R[i].misses, R[i].compulsory,
                   R[i].misses - R[i].compulsory, R[i].evictions);
        return;
    }
    printf("  frames=%-8u refs=%-12" PRIu64 " pages=%-9u  (memory holds %.1f%% "
           "of the footprint)\n", nframes, nref, npages,
           100.0 * nframes / (npages ? npages : 1));
    printf("    %-8s %14s %14s %14s %9s %9s\n",
           "policy", "misses", "compulsory", "capacity", "vs MIN", "miss/ref");
    for (int i = 0; i < nR; i++) {
        uint64_t cap = R[i].misses - R[i].compulsory;
        printf("    %-8s %14" PRIu64 " %14" PRIu64 " %14" PRIu64 " %8.2fx %8.4f\n",
               R[i].name, R[i].misses, R[i].compulsory, cap,
               optcap ? (double)cap / (double)optcap : (cap ? 999.0 : 1.0),
               (double)R[i].misses / (double)nref);
    }
}

/* Where does the clock lose? Not "how much", which the table above gives, but
 * WHEN and ON WHAT -- which is the half that says what a model would need to
 * see. Divided into equal slices of the reference string so a phase change
 * shows up as a slice where the ratio jumps. */
static void shape(uint32_t nframes, int nwin, struct result *a, struct result *b)
{
    if (!a->missbits || !b->missbits) return;
    printf("\n  shape: %s vs %s over %d windows of %" PRIu64 " references "
           "(frames=%u)\n", a->name, b->name, nwin, nref / nwin, nframes);
    printf("    %-6s %12s %12s %10s   %s\n", "window", a->name, b->name, "ratio", "");
    uint64_t w = nref / nwin;
    uint64_t *wa = xmalloc((size_t)nwin * 8), *wb = xmalloc((size_t)nwin * 8), peak = 1;
    for (int k = 0; k < nwin; k++) {
        uint64_t lo = (uint64_t)k * w, hi = (k == nwin - 1) ? nref : lo + w;
        wa[k] = wb[k] = 0;
        for (uint64_t i = lo; i < hi; i++) { wa[k] += got(a, i) != 0; wb[k] += got(b, i) != 0; }
        if (wa[k] > peak) peak = wa[k];
    }
    /* The bar is the ABSOLUTE miss count, scaled to the busiest window, not the
     * ratio. A window with 542 misses at 2.30x and one with 71432 at 3.40x are
     * not comparable problems, and a bar drawn from the ratio makes the first
     * look like the second -- which is precisely the mistake this whole report
     * exists to avoid making about the workloads. */
    for (int k = 0; k < nwin; k++) {
        int bar = (int)(50.0 * (double)wa[k] / (double)peak);
        printf("    %-6d %12" PRIu64 " %12" PRIu64 " %9.2fx   ", k, wa[k], wb[k],
               wb[k] ? (double)wa[k] / (double)wb[k] : 0.0);
        for (int j = 0; j < bar; j++) putchar('#');
        putchar('\n');
    }
    free(wa); free(wb);

    /* Of the references the clock missed and MIN did not, what were they? A
     * code page evicted out from under a running loop and a data page evicted
     * one reference before its reuse are different failures with different
     * fixes, and the totals cannot tell them apart. */
    uint64_t only = 0, only_x = 0, only_w = 0, only_r = 0;
    for (uint64_t i = 0; i < nref; i++) {
        if (!got(a, i) || got(b, i)) continue;
        only++;
        switch (KIND(seq[i])) {
        case MMT_KIND_EXEC:  only_x++; break;
        case MMT_KIND_WRITE: only_w++; break;
        default:             only_r++; break;
        }
    }
    printf("    misses %s took and %s did not: %" PRIu64
           "  (%" PRIu64 " code, %" PRIu64 " written data, %" PRIu64 " read-only data)\n",
           a->name, b->name, only, only_x, only_w, only_r);
}

/* --------------------------------------------------------- the inputs --- */

static void load_trace(const char *path, uint32_t want_space, int top,
                       int no_exec, uint64_t maxrec)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    struct mmt_hdr h;
    if (fread(&h, sizeof h, 1, f) != 1) die("short trace file");
    check_hdr(&h, path);

    /* Pass one: which address space? "top" = the space that touched the most
     * DISTINCT pages, which on every workload here is the one being measured
     * (the shell and the window manager have footprints two orders of
     * magnitude smaller). Counting distinct pages rather than references
     * matters: a tight loop in a small program makes far more references than
     * a big program makes pages. */
    if (top) {
        long start = ftell(f);
        uint64_t *cnt = calloc(1u << 24, 8);
        if (!cnt) die("out of memory counting spaces");
        static uint8_t *bits;
        bits = calloc(1u << 24, 1);
        if (!bits) die("out of memory counting spaces");
        struct mmt_rec buf[8192];
        size_t n;
        while ((n = fread(buf, sizeof buf[0], 8192, f)) > 0)
            for (size_t i = 0; i < n; i++) {
                uint64_t k = ((uint64_t)MMT_SPACE(buf[i]) << 40) | MMT_VPN(buf[i]);
                uint32_t hh = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40);
                if (bits[hh]) continue;         /* approximate distinct count */
                bits[hh] = 1;
                cnt[MMT_SPACE(buf[i])]++;
            }
        uint64_t best = 0;
        for (uint32_t s = 1; s < (1u << 24); s++) if (cnt[s] > best) { best = cnt[s]; want_space = s; }
        fprintf(stderr, "mmsim: busiest address space is cr3>>12=0x%x "
                        "(~%" PRIu64 " distinct pages)\n", want_space, best);
        free(cnt); free(bits);
        fseek(f, start, SEEK_SET);
    }

    ht_init(1u << 20);
    uint64_t cap = 1u << 20;
    seq = xmalloc(cap * 4);

    struct mmt_rec buf[8192];
    size_t n;
    while ((n = fread(buf, sizeof buf[0], 8192, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            unsigned kind = MMT_KIND(buf[i]);
            if (no_exec && kind == MMT_KIND_EXEC) continue;
            if (want_space != 0xFFFFFFFFu && MMT_SPACE(buf[i]) != want_space) continue;
            uint64_t key = ((uint64_t)MMT_SPACE(buf[i]) << 40) | MMT_VPN(buf[i]);
            uint32_t id = intern(key);
            if (nref == cap) { cap *= 2; seq = realloc(seq, cap * 4); if (!seq) die("out of memory"); }
            seq[nref++] = id | (kind << 30);
            if (maxrec && nref >= maxrec) goto done;
        }
    }
done:
    fclose(f);
    /* Collapsing again after the filters: dropping instruction fetches or
     * another address space's records can leave two adjacent references to the
     * same page, and a duplicate is not a reference. */
    uint64_t o = 0;
    for (uint64_t i = 0; i < nref; i++)
        if (!o || ID(seq[o - 1]) != ID(seq[i])) seq[o++] = seq[i];
        else if (KIND(seq[i]) == MMT_KIND_WRITE) seq[o - 1] = ID(seq[o - 1]) | (MMT_KIND_WRITE << 30);
    nref = o;
}

/* What address spaces are in this trace at all?
 *
 * Worth being able to ask directly. A trace is of a whole running machine --
 * the window manager, the shell, whatever the harness typed -- and picking
 * "the busiest space" without ever looking at the list is how a measurement
 * ends up being of /bin/sh. Distinct pages, not references: a tight loop in a
 * tiny program makes far more references than a big program makes pages. */
static void list_spaces(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    struct mmt_hdr h;
    if (fread(&h, sizeof h, 1, f) != 1) die("short trace file");
    check_hdr(&h, path);

    uint64_t *pages = calloc(1u << 24, 8), *refs = calloc(1u << 24, 8);
    uint8_t  *bits  = calloc(1u << 24, 1);
    if (!pages || !refs || !bits) die("out of memory");
    struct mmt_rec buf[8192];
    size_t n;
    uint64_t total = 0;
    while ((n = fread(buf, sizeof buf[0], 8192, f)) > 0)
        for (size_t i = 0; i < n; i++) {
            uint32_t s = MMT_SPACE(buf[i]);
            refs[s]++; total++;
            uint64_t k = ((uint64_t)s << 40) | MMT_VPN(buf[i]);
            uint32_t hh = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40);
            if (!bits[hh]) { bits[hh] = 1; pages[s]++; }
        }
    fclose(f);
    printf("%s: %" PRIu64 " records\n", path, total);
    printf("  %-12s %14s %12s %12s\n", "cr3>>12", "references", "pages", "footprint");
    for (int k = 0; k < 16; k++) {
        uint32_t best = 0; uint64_t bp = 0;
        for (uint32_t s = 0; s < (1u << 24); s++) if (pages[s] > bp) { bp = pages[s]; best = s; }
        if (!bp) break;
        printf("  0x%-10x %14" PRIu64 " %12" PRIu64 " %9.1f MiB\n",
               best, refs[best], pages[best], pages[best] * 4096.0 / (1024 * 1024));
        pages[best] = 0;
    }
    free(pages); free(refs); free(bits);
}

/* The controls. Generated here rather than run on the machine because the
 * point of a control is that its answer is known in advance: these two have
 * textbook answers, and a simulator that does not reproduce them is broken in
 * a way no amount of real-workload data would reveal. */
static void synth(const char *spec, uint32_t nframes)
{
    char kind[32] = "seq";
    uint64_t pages = nframes + 1, refs = 0, loops = 20, hot = 0;
    char *s = strdup(spec), *tok = strtok(s, ",");
    if (tok) snprintf(kind, sizeof kind, "%s", tok);
    while ((tok = strtok(NULL, ","))) {
        if (!strncmp(tok, "pages=", 6)) pages = strtoull(tok + 6, NULL, 0);
        else if (!strncmp(tok, "refs=", 5)) refs = strtoull(tok + 5, NULL, 0);
        else if (!strncmp(tok, "loops=", 6)) loops = strtoull(tok + 6, NULL, 0);
        else if (!strncmp(tok, "hot=", 4)) hot = strtoull(tok + 4, NULL, 0);
    }
    ht_init(pages * 2 + 16);
    if (!refs) refs = pages * loops;
    seq = xmalloc(refs * 4);
    npages = 0;
    for (uint64_t i = 0; i < pages; i++) intern(i);
    npages = (uint32_t)pages;

    if (!strcmp(kind, "seq")) {
        for (uint64_t i = 0; i < refs; i++) seq[i] = (uint32_t)(i % pages);
    } else if (!strcmp(kind, "rand")) {
        for (uint64_t i = 0; i < refs; i++) seq[i] = (uint32_t)(rnd() % pages);
    } else if (!strcmp(kind, "phase")) {
        /* Two working sets, alternating. The case a clock is supposed to be
         * bad at and a model is supposed to be good at. */
        uint64_t half = pages / 2, per = refs / 8;
        for (uint64_t i = 0; i < refs; i++) {
            uint64_t ph = (i / (per ? per : 1)) & 1;
            seq[i] = (uint32_t)(ph * half + (rnd() % (half ? half : 1)));
        }
    } else if (!strcmp(kind, "hotcold")) {
        /* A small hot set inside a big scan: the pattern where LRU and clock
         * genuinely differ from each other, because clock's sweep order is
         * allocation order and not recency. */
        if (!hot) hot = pages / 10 ? pages / 10 : 1;
        for (uint64_t i = 0; i < refs; i++)
            seq[i] = (uint32_t)((i & 1) ? (rnd() % hot) : (hot + rnd() % (pages - hot)));
    } else {
        die("unknown --synth kind (seq, rand, phase, hotcold)");
    }
    nref = refs;
    /* Same rule as the trace loader: adjacent duplicates are not references. */
    uint64_t o = 0;
    for (uint64_t i = 0; i < nref; i++) if (!o || ID(seq[o - 1]) != ID(seq[i])) seq[o++] = seq[i];
    nref = o;
    free(s);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: mmsim [--trace F | --synth SPEC] --frames N[,N...] [options]\n"
        "  --space 0xN   only this address space (cr3>>12)\n"
        "  --space top   the space that touched the most distinct pages (default for traces)\n"
        "  --space all   every space together\n"
        "  --no-exec     drop instruction fetches (a data-only reference string)\n"
        "  --max N       stop after N records\n"
        "  --windows K   print a K-window shape comparison of clock against MIN\n"
        "  --csv         machine-readable rows\n"
        "  --synth seq|rand|phase|hotcold[,pages=,refs=,loops=,hot=]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *tracef = NULL, *synthspec = NULL, *framespec = NULL, *fracspec = NULL;
    uint32_t space = 0xFFFFFFFFu;
    int top = 0, no_exec = 0, nwin = 0, csv = 0, all = 0;
    uint64_t maxrec = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(), (char *)NULL))
        if      (!strcmp(a, "--trace"))   tracef = NEXT();
        else if (!strcmp(a, "--synth"))   synthspec = NEXT();
        else if (!strcmp(a, "--frames"))  framespec = NEXT();
        else if (!strcmp(a, "--frac"))    fracspec = NEXT();
        else if (!strcmp(a, "--max"))     maxrec = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--windows")) nwin = atoi(NEXT());
        else if (!strcmp(a, "--no-exec")) no_exec = 1;
        else if (!strcmp(a, "--csv"))     csv = 1;
        else if (!strcmp(a, "--spaces"))  { list_spaces(NEXT()); return 0; }
        else if (!strcmp(a, "--space")) {
            const char *v = NEXT();
            if (!strcmp(v, "top")) top = 1;
            else if (!strcmp(v, "all")) { space = 0xFFFFFFFFu; all = 1; }
            else space = (uint32_t)strtoul(v, NULL, 0);
        } else usage();
        #undef NEXT
    }
    if (!tracef == !synthspec) usage();
    if (!framespec && !fracspec) usage();

    /* Frame counts first: --synth seq with no pages= is defined relative to
     * the first one (N+1 pages through N frames), which is the control. */
    uint32_t frames[32]; int nf = 0;
    if (framespec) {
        char *s = strdup(framespec), *t = strtok(s, ",");
        while (t && nf < 32) { frames[nf++] = (uint32_t)strtoul(t, NULL, 0); t = strtok(NULL, ","); }
        free(s);
        if (!nf) usage();
    } else {
        frames[nf++] = 1024;    /* a placeholder for --synth's default sizing */
    }

    if (tracef) {
        if (space == 0xFFFFFFFFu && !top && !all) top = 1;   /* the sane default */
        load_trace(tracef, space, top, no_exec, maxrec);
        printf("trace %s: %" PRIu64 " references over %u distinct pages "
               "(%.1f MiB of footprint)\n",
               tracef, nref, npages, npages * 4096.0 / (1024 * 1024));
    } else {
        synth(synthspec, frames[0]);
        printf("synthetic %s: %" PRIu64 " references over %u distinct pages\n",
               synthspec, nref, npages);
    }
    if (!nref) die("the reference string is empty -- wrong --space, or the trace is boot-only");

    /* --frac is the form to use on a real trace. "512 frames" means something
     * different for a 3 MiB workload and a 90 MiB one, and the only regime
     * where a replacement policy matters at all is memory somewhat smaller
     * than the working set -- which is a fraction, not a constant. */
    if (fracspec) {
        nf = 0;
        char *s = strdup(fracspec), *t = strtok(s, ",");
        while (t && nf < 32) {
            double pc = atof(t);
            uint32_t n = (uint32_t)(npages * pc / 100.0);
            if (n < 1) n = 1;
            frames[nf++] = n;
            t = strtok(NULL, ",");
        }
        free(s);
        if (!nf) usage();
    }

    uint32_t *next = build_next();

    for (int k = 0; k < nf; k++) {
        uint32_t n = frames[k];
        if (!n) die("--frames 0");
        nR = 0;
        int want_shape = nwin > 0;
        run_opt  (add("opt", want_shape),  n, next);
        run_lru  (add("lru", 0),           n);
        run_fifo (add("fifo", 0),          n);
        run_rand (add("rand", 0),          n);
        run_clock(add("clock", want_shape), n, 0);
        run_clock(add("clockwm", 0),        n, 1);
        printf("\n");
        report(n, csv);
        if (want_shape) shape(n, nwin, &R[4], &R[0]);
        for (int i = 0; i < nR; i++) free(R[i].missbits);
    }
    free(next);
    return 0;
}
