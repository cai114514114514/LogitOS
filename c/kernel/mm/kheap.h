#ifndef LOGIT_KHEAP_H
#define LOGIT_KHEAP_H

#include <stddef.h>

/* Kernel dynamic memory. Backed by contiguous physical frames from the PMM
 * (identity-mapped). kmalloc must not be called before pmm_init. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* ------------------------------------------------------------------ stats --
 * The kernel heap is a DIFFERENT allocator from the frame allocator, and the
 * two fail differently: kheap takes frames from pmm and never gives them back,
 * so a block that is never reused costs a permanent frame even though pmm's own
 * counters are perfectly balanced and pmm_audit() is clean. Until these
 * counters existed there was no way to tell "the kernel is holding memory" from
 * "the kernel heap cannot reuse the memory it is holding" -- they look
 * identical in `free frames`.
 *
 *   arena_bytes   frames taken from pmm and mapped into the heap. MONOTONIC:
 *                 this is the number that must stop growing.
 *   live_bytes    block bytes currently handed out and not yet kfree'd. Note
 *                 BLOCK bytes, not requested bytes: a request served from a
 *                 bigger block costs the whole block, and that is the point.
 *   free_bytes    block bytes sitting on the size-class free lists, reusable.
 *   req_bytes     cumulative bytes callers have ASKED for.
 *   served_bytes  cumulative block bytes the allocator SPENT answering them.
 *                 served - req is over-allocation, and it is the mechanism
 *                 behind arena growth: an allocator that reuses blocks whole
 *                 spends a 3 MiB block on a 4 KiB request, and then has no
 *                 3 MiB block left for the next window surface.
 *   grows         arena allocations (each one is memory pmm never sees again)
 *   splits        allocations that carved a remainder back onto a free list
 *   split_bytes   total block bytes returned to the free lists by splitting
 *   merges        frees that coalesced with a physical neighbour              */
struct kheap_stats {
    unsigned long long arena_bytes;
    unsigned long long live_bytes;
    unsigned long long free_bytes;
    unsigned long long req_bytes, served_bytes;
    unsigned long long allocs, frees, grows, splits, split_bytes, merges;
    unsigned long long live_blocks;
};

void kheap_get_stats(struct kheap_stats *out);

/* One line on the console; `tag` says what prompted it. */
void kheap_report(const char *tag);

#endif /* LOGIT_KHEAP_H */
