#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "pmm.h"
#include "mmhost.h"      /* mm_p2v: identity in the kernel, an offset on the host */
#include "spinlock.h"
#include "kprintf.h"

/* M25 P1: kheap is peeled out from under the BKL -- kmalloc/kfree take their own
 * lock so they are safe to call from BKL-free kernel paths running concurrently
 * on other cores. irqsave: kmalloc may be reached from IRQ context, and a core
 * must not be preempted while holding this lock (yield-while-holding-spinlock).
 * Lock order is BKL -> kheap_lock -> pmm_lock (grow() calls pmm under kheap_lock);
 * nothing takes them in the reverse order, so no deadlock. */
static spinlock_t kheap_lock = SPINLOCK_INIT;

/* ===========================================================================
 * The kernel heap: size-class free lists over arenas of contiguous frames,
 * with SPLITTING and COALESCING.
 *
 * WHY IT IS SHAPED LIKE THIS -- the bug this replaced.
 *
 * The previous allocator bump-allocated within an arena and kept freed blocks
 * on nine singly-linked size-class lists, WHOLE: a request was served by the
 * first free block big enough, and the excess went with it. Every payload over
 * 2048 bytes lands in bin 8, the catch-all, so "big enough" spanned four orders
 * of magnitude in one list. A 4 KiB pipe buffer would be served out of the
 * 2.8 MiB window surface a closed app had just returned, and would hold all
 * 2.8 MiB for as long as it lived. The next window found nothing that fit, and
 * grow() took another 4 MiB of frames from the PMM.
 *
 * That is the shape of the reported fault. kheap NEVER returns frames to the
 * PMM -- an arena is permanent -- so every one of those grows is physical
 * memory gone for the rest of the boot. And because nothing is leaked in the
 * frame allocator's sense, pmm's counters stay balanced, pmm_audit() stays
 * clean and pmm_bugs() stays 0 the entire time. Free memory falls and every
 * memory invariant the kernel checks says everything is fine.
 *
 * WHY SPLITTING ALONE IS NOT THE FIX, which is the part worth writing down:
 * carving the remainder off a reused block and putting it back makes the
 * measured leak WORSE, not better (tests/unit/leak_kheap_test.c measured
 * 2.7 MB per open/close cycle with splitting and no merging). Splitting chops
 * the big blocks into pieces, nothing ever puts them back together, and the
 * next window-sized request finds a heap that is 90% free and has no single
 * block big enough. So the two halves are one fix: split on allocation,
 * COALESCE on free.
 *
 * THE STRUCTURE that coalescing needs, and which the old one did not have:
 * blocks TILE their arena. Each block knows the payload size of the block
 * physically before it (prev_size) and whether it is the last in its arena
 * (F_LAST), so both neighbours are reachable in O(1) from a block being freed.
 * The header stays 16 bytes -- the free-list links live in the payload of a
 * free block, which is always at least 16 bytes -- so nothing about alignment
 * or per-allocation overhead changed.
 *
 * Blocks never merge across an arena boundary: the first block in an arena has
 * prev_size == 0 (impossible for a real block, whose payload is >= 16) and the
 * last carries F_LAST.
 *
 * Free lists are DOUBLY linked because coalescing has to remove an arbitrary
 * block from the middle of one; with the old singly-linked lists that would be
 * an O(n) walk on the free path.
 * =========================================================================== */

#define ALIGN16(x)    (((x) + 15) & ~((size_t)15))
#define ARENA_FRAMES  1024                 /* 4 MiB per arena */

/* Size classes: bin i covers payloads up to (16 << i) bytes, for i in
 * [0, NUM_BINS-2]; the last bin is the catch-all for oversized blocks.
 *   bin[0]=16  bin[1]=32 ... bin[7]=2048  bin[8]=oversized                 */
#define NUM_BINS      9

/* Smallest remainder worth carving off a reused block. Below this the split
 * would produce a 16-byte header guarding a payload nobody can use, so the
 * block is handed over whole and the excess is counted as waste -- honestly,
 * rather than hidden. Must be >= MIN_PAYLOAD. */
#define MIN_PAYLOAD   16                   /* a free block's payload holds two pointers */
#define SPLIT_MIN     64

/* Flags in the low bits of header.size. Payload sizes are ALIGN16, so the low
 * four bits are always zero and are free to carry state. */
#define F_FREE        0x1u
#define F_LAST        0x2u                 /* no block follows this one in its arena */
#define SIZE_MASK     (~(size_t)0xF)

struct header {                            /* 16 bytes: payload stays 16-aligned */
    size_t size;                           /* payload bytes | F_FREE | F_LAST */
    size_t prev_size;                      /* payload bytes of the physically previous
                                            * block; 0 == first block in its arena */
};

/* A free block's links live in its own payload (dlmalloc's trick). That is why
 * MIN_PAYLOAD is 16 and why the header did not have to grow to 32 bytes to get
 * coalescing -- per-allocation overhead is unchanged at 16 bytes. */
struct fnode {
    struct fnode *next, *prev;
};

static struct fnode *bins[NUM_BINS] = { NULL };

/* Accounting (kheap.h). All mutated under kheap_lock.
 *
 * st_req vs st_served is the over-allocation metric, and it is CUMULATIVE
 * rather than live for a reason worth stating: there is nowhere to put a live
 * block's requested size. The header is 16 bytes and both words are load-
 * bearing (size+flags, prev_size), and the payload belongs to the caller.
 * Widening the header to 32 bytes to record a debugging counter would double
 * the overhead of every small kernel allocation. Cumulative totals need no
 * per-block storage at all and answer the same question -- "of the bytes this
 * allocator has handed out, how many did anybody ask for" -- which is exactly
 * what a whole-block-reuse allocator gets catastrophically wrong. */
static unsigned long long st_arena, st_live, st_free;
static unsigned long long st_req, st_served;
static unsigned long long st_allocs, st_frees, st_grows, st_splits, st_split_bytes;
static unsigned long long st_merges, st_live_blocks;

/* Fail-safe bound on a single free-list walk. No real bin ever holds anywhere
 * near this many free blocks; a walk that exceeds it means the list has been
 * corrupted into a cycle (a stray write smashed a node's `next`). Because the
 * walk runs under kheap_lock with interrupts OFF, an actual cycle would spin the
 * core forever -- hanging the WHOLE system, not just the faulting task. We refuse
 * to do that: on detection we drop the corrupt bin's list (leaking those blocks)
 * and keep running, so a heap-corrupting bug degrades to a leak, never a freeze. */
#define FREELIST_WALK_MAX 1000000UL

/* --- header helpers ------------------------------------------------------ */

static inline size_t blk_size(const struct header *h) { return h->size & SIZE_MASK; }
static inline int    blk_free(const struct header *h) { return (h->size & F_FREE) != 0; }
static inline int    blk_last(const struct header *h) { return (h->size & F_LAST) != 0; }
static inline void   blk_set_size(struct header *h, size_t n)
{ h->size = n | (h->size & ~SIZE_MASK); }

static inline struct header *blk_next(struct header *h)
{ return blk_last(h) ? NULL : (struct header *)((uint8_t *)(h + 1) + blk_size(h)); }

static inline struct header *blk_prev(struct header *h)
{
    if (h->prev_size == 0) return NULL;    /* first block in its arena */
    return (struct header *)((uint8_t *)h - h->prev_size - sizeof(struct header));
}

static inline struct fnode *blk_node(struct header *h) { return (struct fnode *)(h + 1); }
static inline struct header *node_blk(struct fnode *n) { return (struct header *)n - 1; }

/* Map a (16-aligned) size to its bin: min(ceil_log2(size/16), NUM_BINS-1),
 * computed in O(1). size is always >= 16 here, so size/16 >= 1. */
static int bin_index(size_t size)
{
    size_t units = size >> 4;              /* size / 16, >= 1 */
    /* ceil_log2(units): 0 for units==1, else 64 - clz(units-1). */
    int idx = (units <= 1) ? 0 : (64 - __builtin_clzll(units - 1));
    if (idx >= NUM_BINS)
        idx = NUM_BINS - 1;
    return idx;
}

/* --- free-list membership ------------------------------------------------ */

static void bin_push(struct header *h)
{
    int b = bin_index(blk_size(h));
    struct fnode *n = blk_node(h);
    n->prev = NULL;
    n->next = bins[b];
    if (bins[b]) bins[b]->prev = n;
    bins[b] = n;
    h->size |= F_FREE;
    st_free += blk_size(h);
}

static void bin_remove(struct header *h)
{
    int b = bin_index(blk_size(h));
    struct fnode *n = blk_node(h);
    if (n->prev) n->prev->next = n->next;
    else         bins[b] = n->next;
    if (n->next) n->next->prev = n->prev;
    h->size &= ~(size_t)F_FREE;
    st_free -= blk_size(h);
}

/* --- arenas -------------------------------------------------------------- */

static int grow(size_t need)
{
    size_t frames = ARENA_FRAMES;
    while (frames * FRAME_SIZE < need) {
        if (frames > SIZE_MAX / (2 * FRAME_SIZE))   /* doubling would wrap to 0 -> spin forever (holding the lock) */
            return 0;
        frames *= 2;
    }

    uint64_t phys;
#ifdef KHEAP_GROW_FAULT_INJECT
    /* Debug knob (make GROWFI=1): after boot settles, fail every other grow to
     * exercise the contig-allocation-failure path deterministically. */
    static unsigned grow_calls;
    if (++grow_calls > 16 && (grow_calls & 1))
        phys = 0;
    else
#endif
    phys = pmm_alloc_contig(frames);
    if (!phys) {
        kprintf("[kheap] grow: pmm_alloc_contig(%d frames) FAILED\n", (int)frames);
        return 0;
    }

    /* The arena starts life as ONE free block spanning it. There is no bump
     * pointer any more: an un-carved tail and a free block are now the same
     * thing, which is what removed the old "retire the bump area's tail"
     * hand-off entirely -- and with it the failure window where brk still
     * described memory that had already been free-listed (the app-churn heap
     * corruption; see tests/unit/kheap_test.c for that reproduction).
     *
     * Identity-mapped in the kernel, so mm_p2v is a no-op there; on the host
     * test build it is the offset into the simulated RAM, which is the only
     * reason kheap.c can be run under ASan at all (tests/unit/leak_kheap_test.c).
     * Same seam pmm.c and vmm.c already use -- see c/kernel/mm/mmhost.h. */
    uint8_t *base = (uint8_t *)mm_p2v(phys);
    size_t   bytes = frames * FRAME_SIZE;

    struct header *h = (struct header *)base;
    h->size = (bytes - sizeof(struct header)) & SIZE_MASK;
    h->size |= F_LAST;                     /* nothing follows it: arena boundary */
    h->prev_size = 0;                      /* nothing precedes it: arena boundary */
    bin_push(h);

    st_arena += (unsigned long long)bytes;
    st_grows++;
    /* Say so, every time. An arena is frames the PMM will never see again --
     * pmm's own counters stay perfectly balanced and pmm_audit() stays clean
     * while free memory falls, so this line is the only place that growth is
     * attributable. In a settled system it must stop appearing; if it keeps
     * appearing while apps open and close, the kernel heap is the leak. */
    kprintf("[kheap] grow #%d: +%d KiB -> arena %d KiB, live %d KiB, free %d KiB, "
            "over-allocated %d KiB\n",
            (int)st_grows, (int)(bytes / 1024), (int)(st_arena / 1024),
            (int)(st_live / 1024), (int)(st_free / 1024),
            (int)((st_served - st_req) / 1024));
    return 1;
}

/* --- split / coalesce ---------------------------------------------------- */

/* Carve the unused tail of a block about to be handed out back into a free
 * block. `b` is already off the free list and `size` is 16-aligned.
 *
 * Alignment: blk_size(b) and size are both 16-aligned and the header is 16
 * bytes, so the carved block's address and its size are 16-aligned by
 * construction.
 *
 * KHEAP_NO_SPLIT keeps the whole block, which is exactly the old behaviour and
 * one of the two negative controls (tests/unit/leak_run.sh). */
static void split_block(struct header *b, size_t size)
{
#ifndef KHEAP_NO_SPLIT
    size_t have = blk_size(b);
    if (have < size + sizeof(struct header) + SPLIT_MIN)
        return;

    struct header *rest = (struct header *)((uint8_t *)(b + 1) + size);
    size_t rest_size = have - size - sizeof(struct header);

    rest->size = rest_size;                       /* not free yet; bin_push sets F_FREE */
    rest->prev_size = size;
    if (blk_last(b)) { rest->size |= F_LAST; b->size &= ~(size_t)F_LAST; }
    else             { blk_next(rest)->prev_size = rest_size; }

    blk_set_size(b, size);
    bin_push(rest);

    st_splits++;
    st_split_bytes += rest_size;
#else
    (void)b; (void)size;
#endif
}

/* Merge `h` (not on any free list, F_FREE clear) with whichever of its physical
 * neighbours are free, and return the merged block -- still not on a list.
 *
 * This is the half that makes splitting safe. Without it the heap grinds itself
 * into dust: every reuse leaves a smaller remainder, nothing ever recombines,
 * and a window-sized request eventually cannot be served from a heap that is
 * almost entirely free. The measured cost of leaving it out is in
 * tests/unit/leak_kheap_test.c, which runs with KHEAP_NO_COALESCE as its second
 * negative control.
 *
 * The neighbour walk is O(1) in each direction and touches only headers, so
 * this adds a handful of loads to kfree and no scan of anything. */
static struct header *coalesce(struct header *h)
{
#ifndef KHEAP_NO_COALESCE
    struct header *n = blk_next(h);
    if (n && blk_free(n)) {
        bin_remove(n);
        blk_set_size(h, blk_size(h) + sizeof(struct header) + blk_size(n));
        if (blk_last(n)) h->size |= F_LAST;
        else             blk_next(h)->prev_size = blk_size(h);
        st_merges++;
    }
    struct header *p = blk_prev(h);
    if (p && blk_free(p)) {
        bin_remove(p);
        blk_set_size(p, blk_size(p) + sizeof(struct header) + blk_size(h));
        if (blk_last(h)) p->size |= F_LAST;
        else             blk_next(p)->prev_size = blk_size(p);
        st_merges++;
        h = p;
    }
#endif
    return h;
}

/* --- the allocator ------------------------------------------------------- */

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (size > SIZE_MAX - 15 - sizeof(struct header))   /* ALIGN16/header add would wrap */
        return NULL;
    size_t req = size;
    size = ALIGN16(size);
    if (size < MIN_PAYLOAD) size = MIN_PAYLOAD;         /* room for the free-list links */

    struct header *b = NULL;
    uint64_t f = spin_lock_irqsave(&kheap_lock);

    for (int attempt = 0; attempt < 2 && !b; attempt++) {
        /* Reuse a free block from the matching size class. A block lands in bin
         * bin_index(block size); since bin_index is monotonic, any block from a
         * higher bin also fits, so we search this bin and up. Within a bin sizes
         * vary, so still confirm the block is big enough before taking it. */
        for (int i = bin_index(size); i < NUM_BINS && !b; i++) {
            unsigned long walked = 0;
            for (struct fnode *n = bins[i]; n; n = n->next) {
                if (++walked > FREELIST_WALK_MAX) {   /* corrupted into a cycle -> fail safe */
                    kprintf("[kheap] bin %d free list corrupt (cycle) -- dropping it to stay alive\n", i);
                    bins[i] = NULL;
                    break;
                }
                if (blk_size(node_blk(n)) >= size) { b = node_blk(n); break; }
            }
        }
        if (b) break;
        /* Nothing fits: take another arena (grow() runs under kheap_lock and
         * may take pmm_lock -- order kheap -> pmm) and look once more. */
        if (attempt == 0 && !grow(sizeof(struct header) + size))
            break;
    }

    void *ret = NULL;
    if (b) {
        bin_remove(b);
        split_block(b, size);              /* the remainder, if any, goes back */
        ret = (void *)(b + 1);
        st_live   += blk_size(b);
        st_req    += req;                  /* what callers asked for, ever */
        st_served += blk_size(b);          /* what the allocator spent on it, ever */
        st_live_blocks++;
        st_allocs++;
    }
    spin_unlock_irqrestore(&kheap_lock, f);
    return ret;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct header *h = (struct header *)ptr - 1;
    uint64_t f = spin_lock_irqsave(&kheap_lock);

    if (blk_free(h)) {
        /* A double free would put one block on a bin twice, and the second
         * allocation out of it would hand the same memory to two owners --
         * which is heap corruption presenting as anything at all, later.
         * Refuse it, loudly, and keep running. */
        kprintf("[kheap] double free of %p (%d bytes) -- refused\n",
                ptr, (int)blk_size(h));
        spin_unlock_irqrestore(&kheap_lock, f);
        return;
    }

    st_live -= blk_size(h);
    if (st_live_blocks) st_live_blocks--;
    st_frees++;

    bin_push(coalesce(h));
    spin_unlock_irqrestore(&kheap_lock, f);
}

void kheap_get_stats(struct kheap_stats *out)
{
    if (!out) return;
    uint64_t f = spin_lock_irqsave(&kheap_lock);
    out->arena_bytes = st_arena;
    out->live_bytes  = st_live;
    out->free_bytes  = st_free;
    out->req_bytes   = st_req;
    out->served_bytes = st_served;
    out->allocs      = st_allocs;
    out->frees       = st_frees;
    out->grows       = st_grows;
    out->splits      = st_splits;
    out->split_bytes = st_split_bytes;
    out->merges      = st_merges;
    out->live_blocks = st_live_blocks;
    spin_unlock_irqrestore(&kheap_lock, f);
}

void kheap_report(const char *tag)
{
    struct kheap_stats s;
    kheap_get_stats(&s);
    kprintf("[kheap] %s: arena %d KiB, live %d KiB (%d blocks), free %d KiB, "
            "over-allocated %d KiB of %d KiB served, "
            "%d allocs, %d frees, %d grows, %d splits, %d merges\n",
            tag ? tag : "-", (int)(s.arena_bytes / 1024), (int)(s.live_bytes / 1024),
            (int)s.live_blocks, (int)(s.free_bytes / 1024),
            (int)((s.served_bytes - s.req_bytes) / 1024), (int)(s.served_bytes / 1024),
            (int)s.allocs, (int)s.frees, (int)s.grows, (int)s.splits, (int)s.merges);
}
