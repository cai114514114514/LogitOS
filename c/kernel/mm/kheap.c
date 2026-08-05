#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "pmm.h"
#include "spinlock.h"
#include "kprintf.h"

/* M25 P1: kheap is peeled out from under the BKL -- kmalloc/kfree take their own
 * lock so they are safe to call from BKL-free kernel paths running concurrently
 * on other cores. irqsave: kmalloc may be reached from IRQ context, and a core
 * must not be preempted while holding this lock (yield-while-holding-spinlock).
 * Lock order is BKL -> kheap_lock -> pmm_lock (grow() calls pmm under kheap_lock);
 * nothing takes them in the reverse order, so no deadlock. */
static spinlock_t kheap_lock = SPINLOCK_INIT;

/* A small allocator: bump-allocate within a contiguous arena of physical
 * frames, and keep freed blocks for reuse, segregated into power-of-2 size
 * classes (the classic slab/size-class technique). Each bin holds a singly-
 * linked free list of equal-or-similar-sized blocks, so a fitting block is
 * found in O(1) instead of scanning one global list. No coalescing — adequate
 * for kernel bookkeeping and easy to reason about. */

#define ALIGN16(x)    (((x) + 15) & ~((size_t)15))
#define ARENA_FRAMES  1024                 /* 4 MiB per arena */

/* Size classes: bin i covers payloads up to (16 << i) bytes, for i in
 * [0, NUM_BINS-2]; the last bin is the catch-all for oversized blocks.
 *   bin[0]=16  bin[1]=32 ... bin[7]=2048  bin[8]=oversized                 */
#define NUM_BINS      9

struct header {
    size_t size;            /* payload size */
    struct header *next;    /* only meaningful while on a bin's free list */
};

static struct header *bins[NUM_BINS] = { NULL };
static uint8_t *brk = NULL;
static size_t   brk_left = 0;

/* Fail-safe bound on a single free-list walk. No real bin ever holds anywhere
 * near this many free blocks; a walk that exceeds it means the list has been
 * corrupted into a cycle (a stray write smashed a node's `next`). Because the
 * walk runs under kheap_lock with interrupts OFF, an actual cycle would spin the
 * core forever -- hanging the WHOLE system, not just the faulting task. We refuse
 * to do that: on detection we drop the corrupt bin's list (leaking those blocks)
 * and keep running, so a heap-corrupting bug degrades to a leak, never a freeze. */
#define FREELIST_WALK_MAX 1000000UL

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

    /* Retire the old bump area's tail onto a free list ONLY now that the new
     * arena is in hand. Doing this before the (fallible) arena allocation was
     * the app-churn freeze root cause: on failure brk/brk_left kept describing
     * a block that was already free-listed, so the next small kmalloc bump-
     * allocated memory that a later bin-fit alloc would hand out AGAIN -- two
     * owners for one block, whose writes smashed free-list headers into cycles
     * (see tests/unit/kheap_test.c for the deterministic reproduction). */
    if (brk && brk_left >= sizeof(struct header) + 16) {
        struct header *h = (struct header *)brk;
        h->size = brk_left - sizeof(struct header);
        int b = bin_index(h->size);
        h->next = bins[b];
        bins[b] = h;
    }

    brk = (uint8_t *)phys;                 /* identity-mapped */
    brk_left = frames * FRAME_SIZE;
    return 1;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (size > SIZE_MAX - 15 - sizeof(struct header))   /* ALIGN16/header add would wrap */
        return NULL;
    size = ALIGN16(size);

    void *ret = NULL;
    uint64_t f = spin_lock_irqsave(&kheap_lock);

    /* Reuse a freed block from the matching size class. A block lands in bin
     * bin_index(block->size); since bin_index is monotonic, any block from a
     * higher bin also fits, so we search this bin and up. Within a bin sizes
     * vary, so still confirm block->size >= size before handing it out. */
    for (int i = bin_index(size); i < NUM_BINS; i++) {
        unsigned long walked = 0;
        for (struct header **pp = &bins[i]; *pp; pp = &(*pp)->next) {
            if (++walked > FREELIST_WALK_MAX) {   /* corrupted into a cycle -> fail safe */
                kprintf("[kheap] bin %d free list corrupt (cycle) -- dropping it to stay alive\n", i);
                bins[i] = NULL;
                break;
            }
            if ((*pp)->size >= size) {
                struct header *b = *pp;
                *pp = b->next;
                ret = (void *)(b + 1);
                goto out;
            }
        }
    }

    /* Otherwise bump-allocate, growing the arena if needed (grow() runs under
     * kheap_lock and may take pmm_lock -- order kheap -> pmm). */
    {
        size_t total = sizeof(struct header) + size;
        if (brk_left < total && !grow(total)) { ret = NULL; goto out; }

        struct header *h = (struct header *)brk;
        h->size = size;
        h->next = NULL;
        brk += total;
        brk_left -= total;
        ret = (void *)(h + 1);
    }

out:
    spin_unlock_irqrestore(&kheap_lock, f);
    return ret;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct header *h = (struct header *)ptr - 1;
    int b = bin_index(h->size);            /* reads this block's own header, not shared */
    uint64_t f = spin_lock_irqsave(&kheap_lock);
    h->next = bins[b];
    bins[b] = h;
    spin_unlock_irqrestore(&kheap_lock, f);
}
