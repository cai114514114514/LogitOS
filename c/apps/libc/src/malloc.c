#include <stddef.h>
#include <stdint.h>

/* mini-libc allocator: segregated free lists over one static arena.
 *
 * WHY IT WAS REWRITTEN (2026-08). The old allocator was a first-fit walk of the
 * physical block chain STARTING AT THE ARENA BASE ON EVERY CALL -- no free list,
 * no size classes, not even a rotating cursor. With N live blocks, allocation
 * N+1 walked N headers, so "build N objects and hold them" cost O(N^2). Measured
 * on the host with the real file, N blocks of 24-56 bytes all kept live:
 *
 *      N        old        ratio per doubling
 *     12500     183 ms          -
 *     25000     760 ms        x4.15
 *     50000    3232 ms        x4.25
 *    100000   12706 ms        x3.93
 *    200000   54810 ms        x4.31          <- x4 per doubling == quadratic
 *
 * QuickJS allocates every JS object through here and a real page holds tens of
 * thousands live, so this -- not emulation -- was the wall. See tests/unit/
 * malloc_test.c (`make test-malloc`), which asserts the SCALING, not a duration.
 *
 * DESIGN. Boundary-tagged blocks + 56 segregated free lists:
 *   - bins 0..31 are EXACT sizes (payload 16,32,...,512), so the small-object
 *     case that dominates a JS heap is "pop the head", no search at all;
 *   - bins 32.. are power-of-two ranges (bin = 23+floor(log2 size)), searched
 *     with a bounded scan before falling up to a strictly larger bin, which by
 *     construction always fits;
 *   - `binmap` is a 64-bit "this bin is non-empty" bitmap, so finding the
 *     smallest usable bin is one mask + ctz rather than a walk of 56 heads.
 * free() is O(1): it coalesces with BOTH physical neighbours (the old code
 * coalesced forward only, and only when a malloc scan happened to pass by) and
 * pushes the result on a bin. Nothing anywhere walks the whole heap.
 *
 * NEVER FAULT. The old code's one defence was "a block's computed next must stay
 * inside the arena". Free lists make this harder, because a freed block's links
 * live in its payload, where a use-after-free writes. Three things replace it:
 *   1. every link is a 32-bit OFFSET into the arena, never a pointer. It is
 *      turned into an address only by hdr_at(), which rejects anything
 *      misaligned or past the arena end -- so a corrupted link can at worst
 *      point at another spot INSIDE the arena, which is always mapped. There is
 *      no value it can hold that produces a wild pointer.
 *   2. every header carries a tag + checksum over its own fields, so a header
 *      clobbered by an overflow from the block before it is detected rather than
 *      believed. That also makes double-free and free() of an interior pointer
 *      no-ops instead of list corruption.
 *   3. when a check does fail, rebuild() re-derives every bin from a physical
 *      walk (itself bounds-checked); if even that cannot complete, the heap
 *      latches `broken` and malloc returns NULL forever. Degraded, never a jump
 *      through a wild pointer. List walks are also step-capped, so a link cycle
 *      terminates instead of spinning.
 */

#ifndef ARENA_SIZE
#define ARENA_SIZE (24u * 1024u * 1024u)   /* 24 MiB JS heap */
#endif
#define HDR    16u
#define MINPAY 16u                  /* smallest payload: also holds the 2 links */
#define NIL    0xFFFFFFFFu

/* Offsets are uint32 and the checksum arithmetic assumes it. */
_Static_assert(ARENA_SIZE > 4096u && ARENA_SIZE <= 0x80000000u,
               "arena must fit a 32-bit offset");

static unsigned char arena[ARENA_SIZE] __attribute__((aligned(16)));
static int inited;
static int broken;                  /* latched on unrecoverable corruption */

/* high-water mark of live bytes (payload only) -- lets the browser report how
 * much heap a page really needed (github.com's 3 MiB of CSS blew the 24 MiB
 * arena mid-parse and the tail of the stylesheet silently vanished). */
size_t malloc_peak;
static size_t malloc_cur;

#define TAG_USED 0x55534544u        /* 'USED' */
#define TAG_FREE 0x46524545u        /* 'FREE' */

/* 16 bytes, so payloads stay 16-aligned. `prev` is the PREVIOUS physical
 * block's payload size (0 = first block) -- that is the boundary tag that makes
 * backward coalescing O(1) without a per-block footer. */
struct hdr { uint32_t size, prev, tag, chk; };

/* lives in a FREE block's payload (needs 8 of the 16 minimum bytes) */
struct fnode { uint32_t next, prev; };

#define NSMALL 32u                  /* exact-size bins: payload 16..512 */
#define NBIN   56u                  /* <= 64: binmap is one uint64_t */
static uint32_t bins[NBIN];
static uint64_t binmap;

/* --- header plumbing, all of it bounds-checked ------------------------------ */

static uint32_t off_of(const struct hdr *h) { return (uint32_t)((const unsigned char *)h - arena); }

static struct hdr *hdr_at(uint32_t off)
{
    if (off & 15u) return NULL;
    if (off > ARENA_SIZE - HDR) return NULL;
    return (struct hdr *)(arena + off);
}

static uint32_t chk_of(const struct hdr *h)
{
    return h->size ^ (h->prev * 2654435761u) ^ h->tag ^ 0x9E3779B9u;
}
static void seal(struct hdr *h) { h->chk = chk_of(h); }
static int  ok(const struct hdr *h)
{
    return h && (h->tag == TAG_USED || h->tag == TAG_FREE) && h->chk == chk_of(h);
}
static struct fnode *fn(struct hdr *h) { return (struct fnode *)((unsigned char *)h + HDR); }

static struct hdr *next_hdr(const struct hdr *h)
{
    uint64_t no = (uint64_t)off_of(h) + HDR + h->size;
    if (no > (uint64_t)ARENA_SIZE - HDR) return NULL;
    return hdr_at((uint32_t)no);
}

static struct hdr *prev_hdr(const struct hdr *h)
{
    if (!h->prev) return NULL;
    uint64_t back = (uint64_t)h->prev + HDR;
    if (back > off_of(h)) return NULL;
    struct hdr *p = hdr_at(off_of(h) - (uint32_t)back);
    if (!p || !ok(p) || p->size != h->prev) return NULL;   /* boundary tag must agree */
    return p;
}

static unsigned bin_of(uint32_t size)
{
    if (size <= NSMALL * 16u) return size / 16u - 1u;      /* size >= 16, exact bin */
    unsigned c = 23u + (31u - (unsigned)__builtin_clz(size));
    return c >= NBIN ? NBIN - 1u : c;
}

/* An arena can hold at most this many blocks; every free-list walk is capped by
 * it, so a corrupted link that closes a cycle cannot hang the allocator. */
#define MAXBLK (ARENA_SIZE / (HDR + MINPAY))

/* --- bins ------------------------------------------------------------------- */

static void rebuild(void);
static int in_rebuild;

/* h must already be a finalised FREE block (size/prev/tag/chk written). */
static void bin_push(struct hdr *h)
{
    unsigned c = bin_of(h->size);
    uint32_t old = bins[c];
    if (old != NIL) {
        struct hdr *oh = hdr_at(old);
        if (!oh || !ok(oh) || oh->tag != TAG_FREE) {
            if (in_rebuild) { broken = 1; return; }
            rebuild();                     /* picks h up: it is already FREE */
            return;
        }
        fn(oh)->prev = off_of(h);
    }
    fn(h)->next = old;
    fn(h)->prev = NIL;
    bins[c] = off_of(h);
    binmap |= 1ull << c;
}

/* 0 = unlinked and the caller now owns h; 1 = corruption, the bins were rebuilt
 * and h is STILL listed, so the caller must not touch it. */
static int bin_remove(struct hdr *h)
{
    unsigned c = bin_of(h->size);
    uint32_t nx = fn(h)->next, pv = fn(h)->prev;
    struct hdr *nh = NULL, *ph = NULL;

    if (nx != NIL) {
        nh = hdr_at(nx);
        if (!nh || !ok(nh) || nh->tag != TAG_FREE) { rebuild(); return 1; }
    }
    if (pv != NIL) {
        ph = hdr_at(pv);
        if (!ph || !ok(ph) || ph->tag != TAG_FREE) { rebuild(); return 1; }
    } else if (bins[c] != off_of(h)) {
        rebuild(); return 1;               /* claims to be the head but is not */
    }

    if (ph) fn(ph)->next = nx;
    else {
        bins[c] = nx;
        if (nx == NIL) binmap &= ~(1ull << c);
    }
    if (nh) fn(nh)->prev = pv;
    return 0;
}

/* Re-derive every bin from a physical walk. Only called when a check failed, so
 * cost does not matter; not faulting does. */
static void rebuild(void)
{
    if (in_rebuild) { broken = 1; return; }
    in_rebuild = 1;
    for (unsigned i = 0; i < NBIN; i++) bins[i] = NIL;
    binmap = 0;

    uint32_t off = 0;
    for (unsigned n = 0; n <= MAXBLK; n++) {
        struct hdr *h = hdr_at(off);
        if (!h || !ok(h)) break;
        if (h->size == 0) { in_rebuild = 0; return; }      /* sentinel: walk complete */
        if (h->tag == TAG_FREE) bin_push(h);
        uint64_t no = (uint64_t)off + HDR + h->size;
        if (no > (uint64_t)ARENA_SIZE - HDR) break;
        off = (uint32_t)no;
    }
    broken = 1;                            /* the walk never reached the sentinel */
    in_rebuild = 0;
}

static void heap_init(void)
{
    for (unsigned i = 0; i < NBIN; i++) bins[i] = NIL;
    binmap = 0;

    struct hdr *h = (struct hdr *)arena;
    h->size = ARENA_SIZE - 2 * HDR;        /* one big free block, ending AT the sentinel */
    h->prev = 0;
    h->tag  = TAG_FREE;
    seal(h);

    struct hdr *end = (struct hdr *)(arena + ARENA_SIZE - HDR);
    end->size = 0;                         /* sentinel: size 0 marks the end */
    end->prev = h->size;
    end->tag  = TAG_USED;                  /* never coalesced into */
    seal(end);

    inited = 1;
    bin_push(h);
}

static size_t align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

/* --- split / trim ----------------------------------------------------------- */

/* h is USED, sealed, off every list, of size >= need. Give the tail back.
 * The tail is kept marked USED until it is pushed, so that any rebuild()
 * triggered in between sees a physically consistent arena and does not list a
 * block we are about to list ourselves. */
static void trim(struct hdr *h, uint32_t need)
{
    uint32_t total = h->size;
    if (total - need < HDR + MINPAY) return;

    struct hdr *r = hdr_at(off_of(h) + HDR + need);
    if (!r) return;
    h->size = need;
    seal(h);

    r->size = total - need - HDR;
    r->prev = need;
    r->tag  = TAG_USED;
    seal(r);

    struct hdr *after = next_hdr(r);
    if (after && ok(after)) {
        after->prev = r->size;
        seal(after);
        if (after->tag == TAG_FREE && bin_remove(after) == 0) {   /* absorb it */
            r->size += HDR + after->size;
            seal(r);
            struct hdr *a2 = next_hdr(r);
            if (a2 && ok(a2)) { a2->prev = r->size; seal(a2); }
        }
    }
    r->tag = TAG_FREE;
    seal(r);
    bin_push(r);
}

/* --- malloc ----------------------------------------------------------------- */

/* First fitting block in bin c, scanning at most `cap` nodes. NULL = none
 * found (or the list was corrupt, in which case the bins were rebuilt). */
static struct hdr *bin_scan(unsigned c, uint32_t need, unsigned cap, int *corrupt)
{
    uint32_t off = bins[c];
    for (unsigned n = 0; off != NIL && n < cap; n++) {
        struct hdr *h = hdr_at(off);
        if (!h || !ok(h) || h->tag != TAG_FREE) { rebuild(); *corrupt = 1; return NULL; }
        if (h->size >= need) return h;
        off = fn(h)->next;
    }
    return NULL;
}

void *malloc(size_t n)
{
    if (!inited) heap_init();
    if (broken) return NULL;
    if (n == 0) n = 1;
    if (n > ARENA_SIZE) return NULL;              /* also kills the align16 overflow */

    uint32_t need = (uint32_t)align16(n);
    unsigned c = bin_of(need);
    int corrupt = 0;
    struct hdr *h = NULL;

    if (c < NSMALL) {
        /* exact-size bin: the head, if any, fits by construction */
        if (bins[c] != NIL) {
            h = hdr_at(bins[c]);
            if (!h || !ok(h) || h->tag != TAG_FREE) { rebuild(); h = NULL; corrupt = 1; }
        }
    } else {
        h = bin_scan(c, need, 24, &corrupt);      /* bounded: keeps malloc O(1) */
    }

    if (!h && !corrupt) {
        /* smallest strictly larger non-empty bin -- everything in it fits */
        uint64_t mask = (c + 1u >= 64u) ? 0 : (binmap & ~((1ull << (c + 1u)) - 1ull));
        if (mask) {
            unsigned c2 = (unsigned)__builtin_ctzll(mask);
            h = hdr_at(bins[c2]);
            if (!h || !ok(h) || h->tag != TAG_FREE || h->size < need) {
                rebuild(); h = NULL; corrupt = 1;
            }
        } else if (c >= NSMALL) {
            /* last gasp before NULL: the home bin holds the only candidates, so
             * pay for the full walk rather than fail with memory in hand. */
            h = bin_scan(c, need, MAXBLK, &corrupt);
        }
    }
    if (!h) return NULL;
    if (bin_remove(h) != 0) return NULL;          /* corrupt list; already rebuilt */

    h->tag = TAG_USED;
    seal(h);
    trim(h, need);

    malloc_cur += h->size;
    if (malloc_cur > malloc_peak) malloc_peak = malloc_cur;
    return (unsigned char *)h + HDR;
}

/* --- free ------------------------------------------------------------------- */

/* The header of a pointer we handed out, or NULL if it is not one. */
static struct hdr *hdr_of(void *p)
{
    unsigned char *u = (unsigned char *)p;
    if (u < arena + HDR || u >= arena + ARENA_SIZE) return NULL;  /* not ours */
    if ((size_t)(u - arena) & 15u) return NULL;                   /* not a payload start */
    struct hdr *h = (struct hdr *)(u - HDR);
    if (!ok(h) || h->tag != TAG_USED || h->size == 0) return NULL; /* freed, or interior */
    return h;
}

void free(void *p)
{
    if (!p || broken) return;
    struct hdr *h = hdr_of(p);
    if (!h) return;                     /* not ours / double free: never scribble */

    if (malloc_cur >= h->size) malloc_cur -= h->size; else malloc_cur = 0;

    /* Coalesce forward, then backward, writing each merge out as we go: the
     * block stays marked USED throughout, so every intermediate state is a
     * consistent physical chain that rebuild() could walk. */
    struct hdr *nx = next_hdr(h);
    if (nx && ok(nx) && nx->tag == TAG_FREE && nx->size && bin_remove(nx) == 0) {
        h->size += HDR + nx->size;
        seal(h);
        struct hdr *a = next_hdr(h);
        if (a && ok(a)) { a->prev = h->size; seal(a); }
    }

    struct hdr *pv = prev_hdr(h);
    if (pv && pv->tag == TAG_FREE && bin_remove(pv) == 0) {
        pv->size += HDR + h->size;
        pv->tag = TAG_USED;             /* still "not on a list"; flipped below */
        seal(pv);
        struct hdr *a = next_hdr(pv);
        if (a && ok(a)) { a->prev = pv->size; seal(a); }
        h = pv;
    }

    h->tag = TAG_FREE;
    seal(h);
    bin_push(h);
}

size_t malloc_usable_size(void *p)
{
    if (!p) return 0;
    struct hdr *h = hdr_of(p);
    return h ? h->size : 0;
}

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    if (n > ARENA_SIZE) return NULL;

    struct hdr *h = hdr_of(p);
    if (!h) return NULL;                /* not ours: reading its "header" would fault */

    uint32_t need = (uint32_t)align16(n);
    if (h->size >= need) {              /* shrink in place, handing the tail back */
        size_t gave_back = (size_t)(h->size);
        trim(h, need);
        gave_back -= h->size;
        if (malloc_cur >= gave_back) malloc_cur -= gave_back; else malloc_cur = 0;
        return p;
    }

    /* Grow in place by swallowing a free physical successor. QuickJS grows
     * arrays and strings by realloc, and this turns the common "nothing else has
     * been allocated since" case into a header write instead of a copy. */
    struct hdr *nx = next_hdr(h);
    if (nx && ok(nx) && nx->tag == TAG_FREE && nx->size &&
        (uint64_t)h->size + HDR + nx->size >= need && bin_remove(nx) == 0) {
        uint32_t was = h->size;
        h->size += HDR + nx->size;
        seal(h);
        struct hdr *a = next_hdr(h);
        if (a && ok(a)) { a->prev = h->size; seal(a); }
        trim(h, need);
        malloc_cur += (size_t)(h->size - was);
        if (malloc_cur > malloc_peak) malloc_peak = malloc_cur;
        return p;
    }

    uint32_t old = h->size;
    void *np = malloc(n);
    if (np) { memcpy(np, p, old); free(p); }
    return np;
}

void *calloc(size_t a, size_t b)
{
    size_t n = a * b;
    if (a && n / a != b) return NULL;                  /* overflow */
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
