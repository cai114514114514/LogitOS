#ifndef LOGIT_RMAP_H
#define LOGIT_RMAP_H

#include <stdint.h>

/* REVERSE MAPPING: who maps this frame?
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE IS THE FOUNDATION AND NOT A DETAIL
 *
 * Reclaim is "take a physical frame away from whoever has it". The forward
 * direction is easy -- a page table answers "what frame is behind this virtual
 * address" -- but reclaim needs the other direction, and the kernel had no way
 * to ask it. pmm.c's refcount says how MANY leaf PTEs point at a frame; it
 * cannot say WHICH, and you cannot unmap a PTE you cannot find. A reclaim path
 * that evicts a frame while one stale PTE still points at it does not fail: it
 * silently hands one process's page to another, which is the worst class of
 * memory bug this tree has a name for.
 *
 * So the rule is stated first and everything else is built to satisfy it:
 *
 *     A frame may be evicted ONLY if every reference to it is a user PTE this
 *     table knows the address of.
 *
 * Written as a check, that is  rmap_count(f) == pmm_refcount(f)  with the count
 * nonzero and the chain not truncated. It is deliberately the SAME number from
 * two independent structures. pmm's refcount is moved by pmm_alloc/ref/free;
 * the rmap chain is moved by the four places a user PTE is installed or torn
 * down. If they ever disagree about a frame, that frame is not evicted -- so a
 * bug in either one costs memory (a frame nothing will reclaim) and never
 * correctness (a frame two owners share). rmap_audit() re-derives the
 * comparison over every frame and is what the tests assert on.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS ALSO THE PIN DISCIPLINE
 *
 * The same rule pins everything that must never be evicted, without a list of
 * exceptions anybody has to remember to update:
 *
 *   page-table frames      allocated by next_table(), never mapped into a user
 *                          address space, so they have refcount >= 1 and NO
 *                          rmap entry -> rmap_count(0) != refcount(>=1) -> never
 *                          a candidate.
 *   kheap arenas           same: pmm_alloc_contig, no user PTE.
 *   the rmap tables        same, and allocated before anything can be evicted.
 *   DMA buffers            same, as long as they are kernel memory (they are:
 *                          every driver here allocates its rings from kheap).
 *   the kernel image,
 *   the multiboot block,
 *   frame 0                reserved at boot, never rmap'd.
 *
 * The exceptions this does NOT cover are user pages the kernel is *temporarily*
 * holding a pointer into -- a usercopy window, a device mid-transfer into user
 * memory. Those have a correct refcount and a complete rmap chain, so they look
 * evictable and are not. That is what pmm_pin()/pmm_unpin() exist for; see
 * pmm.h. Two mechanisms because there are genuinely two situations: "the kernel
 * owns this frame" (structural, permanent, answered here) and "the kernel is
 * busy with this frame right now" (dynamic, temporary, answered by a pin count).
 *
 * ---------------------------------------------------------------------------
 * SHAPE AND COST
 *
 * A chain per frame: head[] indexed by frame number, nodes in one pre-allocated
 * pool. Three parallel uint32_t arrays rather than an array of structs, so a
 * node is exactly 12 bytes with no padding.
 *
 *   head        4 bytes / frame                       512 KiB on 512 MiB
 *   nodes       12 bytes each, 1.5 nodes / frame      2.25 MiB on 512 MiB
 *   incomplete  1 bit / frame                          16 KiB
 *                                                     -----------------
 *                                                     ~2.8 MiB = 0.54% of RAM
 *
 * 1.5 nodes per frame rather than 1.0 because sharing makes PTEs outnumber
 * frames: a fork of a 100 MiB process doubles the PTE count without allocating
 * a frame. The pool is sized once, at init, from the PMM -- reclaim must be
 * able to run when there is no memory left, so it may not allocate, and a table
 * it grows on demand is a table that fails exactly when it is needed.
 *
 * POOL EXHAUSTION IS SAFE BY CONSTRUCTION. If a node cannot be had, the frame
 * is flagged INCOMPLETE and is never evicted again for the life of the boot.
 * The cost is a frame that reclaim will not consider; the alternative (dropping
 * the entry) would be a frame reclaim evicts while a PTE it never saw still
 * points at it. Overflows are counted and reported, so "reclaim got less
 * effective" is a number rather than a mystery.
 *
 * ---------------------------------------------------------------------------
 * LOCKING. rmap_lock (irqsave) guards head/next/vpn/cr3pfn. It is taken UNDER
 * the BKL and never takes another lock, so it is a leaf: nothing can deadlock
 * against it. The iterator is the one exception -- it walks a chain across
 * several calls without holding the lock -- and it is documented at
 * rmap_begin(). */

#define RMAP_NIL 0xFFFFFFFFu

/* Build the tables for `total_frames` frames, taking their storage from the
 * PMM. Call once, from pmm_init, after the bitmap exists and before anything
 * can be reclaimed. Returns 0, or -1 if the storage could not be had (in which
 * case rmap_ready() stays 0 and reclaim is disabled -- the kernel runs exactly
 * as it did before this line, which is the correct degradation). */
int  rmap_init(uint64_t total_frames);
int  rmap_ready(void);

/* Record / forget that the leaf PTE for `va` in the address space rooted at
 * `cr3` points at `phys`. Both are no-ops when rmap is not ready, when `va` is
 * outside the private user region, or when `phys` is out of range -- so the
 * callers in vmm.c and fault.c do not each need those three tests. */
void rmap_add(uint64_t phys, uint64_t cr3, uint64_t va);
void rmap_remove(uint64_t phys, uint64_t cr3, uint64_t va);

/* How many user PTEs this table knows point at `phys`. */
unsigned rmap_count(uint64_t phys);

/* Is there ANY mapping? The clock's fast path.
 *
 * Read WITHOUT the lock: one aligned 32-bit load, and the answer is only ever
 * used to SKIP a frame. A frame that gains its first mapping between this read
 * and the next sweep is simply considered next time round; a frame that loses
 * its last one is re-examined under the lock by rmap_count() before anything is
 * done to it. This exists because the clock visits every frame on the machine
 * -- 131072 of them on a 512 MiB box, the overwhelming majority of which are
 * kernel memory with no chain at all -- and taking a spinlock that many times
 * per sweep is most of what a sweep would cost. */
int rmap_mapped(uint64_t phys);

/* 1 if this frame ever lost an entry to pool exhaustion: the chain is not the
 * whole truth and the frame must never be evicted. */
int rmap_incomplete(uint64_t phys);

/* Walk every PTE that maps `phys`.
 *
 *     struct rmap_iter it; uint64_t cr3, va;
 *     for (rmap_begin(&it, phys); rmap_next(&it, &cr3, &va); )
 *         ...
 *
 * The chain is NOT locked across the walk. That is safe for the one caller that
 * exists (reclaim.c) because it runs under the BKL and a user PTE is only ever
 * installed or destroyed by kernel code that also holds the BKL -- the same
 * argument vmm.c's clone already relies on. If a BKL-free unmap path ever
 * appears, this becomes an rmap_for_each(callback) with the lock held
 * throughout, and the iterator goes away. Said out loud because a reverse map
 * that is walked without a lock is exactly the kind of thing that works until
 * it silently does not. */
struct rmap_iter { uint32_t node; };
void rmap_begin(struct rmap_iter *it, uint64_t phys);
int  rmap_next(struct rmap_iter *it, uint64_t *cr3, uint64_t *va);

/* --- accounting; a reverse map is diagnosed with numbers or not at all --- */
uint64_t rmap_nodes_total(void);
uint64_t rmap_nodes_used(void);
uint64_t rmap_nodes_peak(void);
uint64_t rmap_overflows(void);     /* nodes that could not be had */
uint64_t rmap_frames_incomplete(void);
uint64_t rmap_bugs(void);          /* remove() of an entry that was not there, etc. */

/* THE check: for every frame, rmap_count(f) <= pmm_refcount(f), and every frame
 * with a chain has an allocated frame under it. Prints each violation, returns
 * how many there were. O(total_frames + nodes). */
int  rmap_audit(void);
void rmap_report(const char *tag);

#endif /* LOGIT_RMAP_H */
