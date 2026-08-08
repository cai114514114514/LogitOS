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

/* WHERE THE ARENA COMES FROM (2026-08). It used to be `static unsigned char
 * arena[ARENA_SIZE]` -- a plain .bss array. That is why browser.aex shipped a
 * 104.9 MiB .bss of which this array was 96 MiB (91.5%); `nm --size-sort` put
 * the next symbol two orders of magnitude down. And .bss is not free here:
 * elf_load (c/kernel/exec/elf.c) walks [p_vaddr, p_vaddr+p_memsz) and does
 * pmm_alloc() + memset(0) for EVERY page, so the whole array was resident
 * before main() ran, against a measured heap peak of a few MiB on real pages.
 *
 * The history is the argument for not simply picking a smaller number. The
 * arena was 24 MiB; LibCSS parsing github.com's ~3 MiB of stylesheets ran it
 * dry, malloc returned NULL mid-sheet and the page rendered unstyled -- so it
 * was raised to 96 MiB, and the machine paid 96 MiB of RAM to make a ceiling
 * go away. A fixed array can only ever be too small or too expensive.
 *
 * SYS_MMAP reserves ADDRESS SPACE and lets frames appear on first touch, which
 * makes those two failures independent:
 *   - the RESERVATION (ARENA_SIZE) sets how large the heap may ever get. It is
 *     address space, so making it large costs nothing that is not touched.
 *   - the COMMIT BOUND (ARENA_COMMIT, plus a live check against free RAM) sets
 *     how much may actually be occupied, and malloc returns NULL when it is
 *     reached.
 * The bound is not optional, and it is the subtle part. With a static array,
 * running out produced NULL, which every caller here already handles. With
 * demand paging, malloc would hand back an address for memory the machine does
 * not have and the program would die on the TOUCH, in the fault handler, with
 * no way to report anything. Demand paging without a bound converts a clean
 * NULL into a crash. So the bound exists to keep the old failure mode.
 *
 * The arena is still ONE contiguous range and links are still uint32 offsets
 * into it, so none of the safety reasoning above changes. One thing shifts: a
 * corrupted offset used to be guaranteed to land on a mapped page. It still
 * lands inside the reservation, and a first touch of an untouched page there
 * faults in an anonymous zero page rather than faulting the process -- the
 * "never a wild pointer" property is unchanged, it is just backed lazily.
 */

#ifndef ARENA_SIZE
#define ARENA_SIZE (24u * 1024u * 1024u)   /* reservation: how big the heap MAY get */
#endif
#ifndef ARENA_COMMIT
#define ARENA_COMMIT ARENA_SIZE            /* bound: how much may be occupied */
#endif
/* Reservation floor. If the full reservation is refused we halve and retry, so
 * a machine too small for the preferred heap gets a smaller one instead of a
 * program that cannot allocate at all. */
#ifndef ARENA_MIN
#define ARENA_MIN (1u * 1024u * 1024u)
#endif
#define HDR    16u
#define MINPAY 16u                  /* smallest payload: also holds the 2 links */
#define NIL    0xFFFFFFFFu

/* Offsets are uint32 and the checksum arithmetic assumes it. */
_Static_assert(ARENA_SIZE > 4096u && ARENA_SIZE <= 0x80000000u,
               "arena must fit a 32-bit offset");
_Static_assert(ARENA_COMMIT <= ARENA_SIZE, "commit bound cannot exceed the reservation");

static unsigned char *arena;        /* mmap'd at first malloc, never moves */
static uint32_t arena_size;         /* what we actually got (<= ARENA_SIZE) */
static uint32_t arena_limit;        /* commit bound, in arena offset terms */
static uint32_t maxblk;             /* arena_size / (HDR+MINPAY); walk cap */
static int inited;
static int broken;                  /* latched on unrecoverable corruption */

/* Highest arena offset ever occupied by a live block. Because the allocator
 * fills from the base and bins are LIFO, this is what the process has actually
 * made resident -- the number that used to be ARENA_SIZE unconditionally. */
size_t malloc_hwm;
/* Set when the reservation could not be made at all: the one failure a caller
 * cannot distinguish from an ordinary out-of-memory NULL. */
int malloc_arena_failed;

size_t malloc_arena_size(void) { return arena_size; }
size_t malloc_arena_limit(void) { return arena_limit; }

/* --- reserving the arena ----------------------------------------------------
 * Two backends, chosen by __STDC_HOSTED__ (the discriminator this tree already
 * uses -- see c/apps/as/as_ll.c): the freestanding build is the real one and
 * goes straight to int 0x80, the hosted build exists so the unit tests measure
 * the same allocator over the same kind of memory.
 *
 * ARENA_NO_MMAP is the NEGATIVE CONTROL: it puts the static array back. A build
 * with it defined must fail the occupancy assertion in tests/unit/arena_mem_test.c,
 * because otherwise that assertion is not testing anything.
 */
#ifdef ARENA_NO_MMAP
static unsigned char arena_static[ARENA_SIZE] __attribute__((aligned(16)));
static void *arena_map(unsigned long n) { return n <= ARENA_SIZE ? arena_static : 0; }
static void arena_report_fail(void) { }
__attribute__((unused)) static unsigned long arena_free_bytes(void) { return ~0ul; }

#elif __STDC_HOSTED__
#include <sys/mman.h>
#include <unistd.h>
static void *arena_map(unsigned long n)
{
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
static void arena_report_fail(void)
{
    static const char m[] = "malloc: could not reserve a heap\n";
    ssize_t r = write(2, m, sizeof m - 1); (void)r;
}
/* The host is not the machine this bound is for, and querying it portably is
 * more trouble than it is worth; ARENA_COMMIT alone bounds the host build. */
__attribute__((unused)) static unsigned long arena_free_bytes(void) { return ~0ul; }

#else
/* SYS_MMAP / SYS_WRITE / SYS_MEMINFO. The numbers are duplicated rather than
 * pulled from logit_abi.h because this TU is the allocator every other TU
 * depends on, and it must not acquire an include that might itself allocate. */
static long arena_sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

static void *arena_map(unsigned long n)
{ return (void *)arena_sys(92 /*SYS_MMAP*/, (long)n, 0x1 | 0x2 /*R|W*/, 0); }

static void arena_report_fail(void)
{
    static const char m[] = "malloc: could not reserve a heap\n";
    arena_sys(1 /*SYS_WRITE*/, 2, (long)m, (long)(sizeof m - 1));
}

/* Free physical memory, so the bound can be the machine's rather than a number
 * compiled in months ago. Only the first two fields are read, but the struct
 * must be the ABI's size or the kernel writes past this. */
__attribute__((unused)) static unsigned long arena_free_bytes(void)
{
    unsigned long long mi[13];
    for (unsigned i = 0; i < 13; i++) mi[i] = 0;
    if (arena_sys(94 /*SYS_MEMINFO*/, (long)mi, 0, 0) != 0) return ~0ul;
    if (!mi[0]) return ~0ul;                       /* frame_bytes unset: no opinion */
    return (unsigned long)(mi[2] * mi[0]);         /* frames_free * frame_bytes */
}
#endif

/* Keep this much physical memory out of the browser's reach. The kernel, the
 * window manager and every other process live in it, and the swap line's
 * reclaim should be a backstop for pressure this process did not cause -- not
 * the thing that keeps its own heap from killing the machine. */
#ifndef ARENA_RAM_RESERVE
#define ARENA_RAM_RESERVE (24u * 1024u * 1024u)
#endif

static void arena_reserve(void)
{
    unsigned long want = ARENA_SIZE;
    while (want >= ARENA_MIN) {
        void *p = arena_map(want);
        if (p) {
            arena = (unsigned char *)p;
            arena_size = (uint32_t)want;
            arena_limit = ARENA_COMMIT < want ? (uint32_t)ARENA_COMMIT : (uint32_t)want;
            maxblk = arena_size / (HDR + MINPAY);
            return;
        }
        want /= 2;
    }
    malloc_arena_failed = 1;
    arena_report_fail();
}

/* Would occupying up to `end` be more than the machine can back? Consulted only
 * when the high-water mark actually moves past a megabyte boundary, so the
 * syscall is amortised over ~256 pages of growth rather than paid per malloc. */
#ifndef ARENA_NO_BOUND
static uint32_t committed_ok_upto;
#endif

static int commit_ok(uint32_t end)
{
#ifdef ARENA_NO_BOUND
    /* NEGATIVE CONTROL: the bound removed. Demand-paged memory is then handed
     * out with nothing checking that the machine can back it, which is the
     * failure this whole mechanism exists to prevent. tests/unit/arena_mem_test.c
     * requires a build with this defined to FAIL. */
    (void)end; return 1;
#else
    if (end > arena_limit) return 0;             /* the compiled-in ceiling */
    if (end <= malloc_hwm) return 1;             /* reusing space already occupied */
    if (end <= committed_ok_upto) return 1;      /* already cleared this far */

    unsigned long grow = (unsigned long)(end - malloc_hwm);
    unsigned long freeb = arena_free_bytes();
    if (freeb != ~0ul && freeb < grow + ARENA_RAM_RESERVE) return 0;

    /* Clear a megabyte beyond what was asked, so the next ~256 pages of growth
     * cost no syscall. Over-committing by that much is immaterial against
     * ARENA_RAM_RESERVE, which is an order of magnitude larger. */
    uint32_t ahead = end + (1u << 20);
    committed_ok_upto = ahead < end ? end : ahead;   /* no wrap */
    return 1;
#endif
}

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
    /* arena_size is a runtime value now, so the bound has to survive being asked
     * before there is an arena: with arena_size 0, `off > arena_size - HDR`
     * underflows to a comparison against ~0 and lets everything through onto a
     * NULL base. Every caller today is downstream of a successful heap_init, so
     * this is a guard against a future one, not a live bug. */
    if (!arena || arena_size < HDR) return NULL;
    if (off > arena_size - HDR) return NULL;
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
    if (no > (uint64_t)arena_size - HDR) return NULL;
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

/* An arena can hold at most `maxblk` blocks (set with arena_size at reservation
 * time); every free-list walk is capped by it, so a corrupted link that closes
 * a cycle cannot hang the allocator. */

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
    for (unsigned n = 0; n <= maxblk; n++) {
        struct hdr *h = hdr_at(off);
        if (!h || !ok(h)) break;
        if (h->size == 0) { in_rebuild = 0; return; }      /* sentinel: walk complete */
        if (h->tag == TAG_FREE) bin_push(h);
        uint64_t no = (uint64_t)off + HDR + h->size;
        if (no > (uint64_t)arena_size - HDR) break;
        off = (uint32_t)no;
    }
    broken = 1;                            /* the walk never reached the sentinel */
    in_rebuild = 0;
}

static void heap_init(void)
{
    inited = 1;                            /* before anything that could re-enter */
    arena_reserve();
    if (!arena) { broken = 1; return; }    /* malloc returns NULL from here on */

    for (unsigned i = 0; i < NBIN; i++) bins[i] = NIL;
    binmap = 0;

    struct hdr *h = (struct hdr *)arena;
    h->size = arena_size - 2 * HDR;        /* one big free block, ending AT the sentinel */
    h->prev = 0;
    h->tag  = TAG_FREE;
    seal(h);

    struct hdr *end = (struct hdr *)(arena + arena_size - HDR);
    end->size = 0;                         /* sentinel: size 0 marks the end */
    end->prev = h->size;
    end->tag  = TAG_USED;                  /* never coalesced into */
    seal(end);
    /* Note: that write touches the LAST page of the reservation, so a heap that
     * has allocated nothing still costs two pages rather than one. It is the
     * only page above the high-water mark this allocator ever touches, and
     * keeping the chain terminated at the true end of the arena is what lets
     * arena_limit be raised later without re-mapping anything. */

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

static void *malloc_nl(size_t n)
{
    if (!inited) heap_init();
    if (broken) return NULL;
    if (n == 0) n = 1;
    if (n > arena_size) return NULL;              /* also kills the align16 overflow */

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
            h = bin_scan(c, need, maxblk, &corrupt);
        }
    }
    if (!h) return NULL;

    /* THE BOUND. Checked while h is still on its bin, so refusing costs nothing
     * and leaks nothing. Taking this block would occupy the arena out to
     * `endoff`; if that is past the ceiling, or past what the machine can still
     * back with physical frames, return NULL now -- the failure every caller
     * here already handles -- rather than hand back an address that kills the
     * process when it is written to. */
    uint32_t endoff = off_of(h) + HDR + need;
    if (!commit_ok(endoff)) return NULL;

    if (bin_remove(h) != 0) return NULL;          /* corrupt list; already rebuilt */

    h->tag = TAG_USED;
    seal(h);
    trim(h, need);

    if (endoff > malloc_hwm) malloc_hwm = endoff;
    malloc_cur += h->size;
    if (malloc_cur > malloc_peak) malloc_peak = malloc_cur;
    return (unsigned char *)h + HDR;
}

/* --- free ------------------------------------------------------------------- */

/* The header of a pointer we handed out, or NULL if it is not one. */
static struct hdr *hdr_of(void *p)
{
    unsigned char *u = (unsigned char *)p;
    if (!arena) return NULL;                                      /* no heap yet */
    if (u < arena + HDR || u >= arena + arena_size) return NULL;  /* not ours */
    if ((size_t)(u - arena) & 15u) return NULL;                   /* not a payload start */
    struct hdr *h = (struct hdr *)(u - HDR);
    if (!ok(h) || h->tag != TAG_USED || h->size == 0) return NULL; /* freed, or interior */
    return h;
}

static void free_nl(void *p)
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

static size_t malloc_usable_size_nl(void *p)
{
    if (!p) return 0;
    struct hdr *h = hdr_of(p);
    return h ? h->size : 0;
}

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

static void *realloc_nl(void *p, size_t n)
{
    if (!p) return malloc_nl(n);
    if (n == 0) { free_nl(p); return NULL; }
    if (n > arena_size) return NULL;

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
        (uint64_t)h->size + HDR + nx->size >= need &&
        commit_ok(off_of(h) + HDR + need) &&      /* same bound as malloc's */
        bin_remove(nx) == 0) {
        uint32_t was = h->size;
        h->size += HDR + nx->size;
        seal(h);
        struct hdr *a = next_hdr(h);
        if (a && ok(a)) { a->prev = h->size; seal(a); }
        trim(h, need);
        uint32_t endoff = off_of(h) + HDR + h->size;
        if (endoff > malloc_hwm) malloc_hwm = endoff;
        malloc_cur += (size_t)(h->size - was);
        if (malloc_cur > malloc_peak) malloc_peak = malloc_cur;
        return p;
    }

    uint32_t old = h->size;
    void *np = malloc_nl(n);
    if (np) { memcpy(np, p, old); free_nl(p); }
    return np;
}

static void *calloc_nl(size_t a, size_t b)
{
    size_t n = a * b;
    if (a && n / a != b) return NULL;                  /* overflow */
    void *p = malloc_nl(n);
    if (p) memset(p, 0, n);
    return p;
}

/* =========================================================================
 * THREAD SAFETY (M30).
 *
 * Everything above this line is the allocator, and none of it is reentrant: a
 * bin head, a binmap bit and two boundary tags are updated as a group, and two
 * threads doing that at once hand one block to both of them. That was fine
 * while a process was one flow of control. It is not fine now, and the failure
 * mode is the worst kind -- not a crash at the allocation, but two live objects
 * at one address, discovered later somewhere unrelated.
 *
 * ONE LOCK OVER THE WHOLE ARENA, not per-bin and not a per-thread cache. The
 * cost of contention here is real but it is bounded by what this allocator is
 * for; the cost of getting a lock-free freelist wrong is a corruption nobody
 * can reproduce. If allocation ever becomes the bottleneck in a threaded
 * program, the answer is per-thread magazines in front of this, not finer
 * locking inside it.
 *
 * THE LOCK IS A WEAK SYMBOL, and that is what keeps this file buildable
 * everywhere it is built today. __libc_lock lives in pthread.c; a link that
 * does not include pthread.c -- the host allocator test (make test-malloc),
 * which compiles this file on its own, and any program that never threads --
 * resolves it to 0 and takes the branch that does nothing. Single-threaded
 * behaviour, byte for byte, with no #ifdef and no second build of this file.
 *
 * It is also why the bodies above were renamed rather than wrapped in-place:
 * realloc() calls malloc() and free(), and a lock taken at the public entry
 * would deadlock on the first realloc that had to move a block. The public
 * functions lock once; the _nl bodies call each other.
 * ========================================================================= */

void __libc_lock(volatile int *v)   __attribute__((__weak__));
void __libc_unlock(volatile int *v) __attribute__((__weak__));

static volatile int heap_lock;

static inline void hlock(void)   { if (__libc_lock)   __libc_lock(&heap_lock); }
static inline void hunlock(void) { if (__libc_unlock) __libc_unlock(&heap_lock); }

void *malloc(size_t n)
{ hlock(); void *r = malloc_nl(n); hunlock(); return r; }

void free(void *p)
{ hlock(); free_nl(p); hunlock(); }

void *realloc(void *p, size_t n)
{ hlock(); void *r = realloc_nl(p, n); hunlock(); return r; }

void *calloc(size_t a, size_t b)
{ hlock(); void *r = calloc_nl(a, b); hunlock(); return r; }

size_t malloc_usable_size(void *p)
{ hlock(); size_t r = malloc_usable_size_nl(p); hunlock(); return r; }
