#ifndef LOGIT_RECLAIM_H
#define LOGIT_RECLAIM_H

#include <stdint.h>

/* RECLAIM: taking a physical frame back from whoever has it.
 *
 * ===========================================================================
 * WHAT THIS IS FOR, STATED WITHOUT EXAGGERATION
 *
 * This machine is not out of memory. The full desktop -- nine apps, the browser
 * on a real page, networking live -- peaks at 229 MiB of 511, and pmm_audit()
 * is clean. Nothing here is firefighting.
 *
 * What the kernel could not do is SURVIVE running out. pmm_alloc() returned 0
 * and every caller failed: the fork failed, the app did not launch, the page
 * fault killed the process. The amount of memory a workload could use was
 * exactly the amount of RAM installed, and there was no mechanism by which a
 * page that nobody had touched in a minute could be persuaded to make room for
 * one that was needed now. That is the thing being added. Swap is where the
 * evicted bytes go; reclaim is the mechanism, and it is the half with the
 * interesting failure modes.
 *
 * ===========================================================================
 * THE TWO TIERS, CHEAPEST FIRST
 *
 * The ordering rule is: a page that can be reconstructed without I/O must be
 * taken before a page that has to be written to a disk. Writing a page you
 * could have thrown away is the single most expensive mistake a reclaim path
 * can make, and it is invisible unless the tiers are separated by construction.
 *
 *   TIER 1 -- DROP. The frame is unmapped and freed. Nothing is written
 *             anywhere. The next fault on it reconstructs it exactly.
 *             Cost: one page-table write per mapping. No device involved.
 *
 *   TIER 2 -- SWAP. The frame's contents are written to a slot on the swap
 *             device, every PTE that mapped it becomes a swap entry, and the
 *             frame is freed. The next fault reads it back.
 *             Cost: a 4 KiB disk write now and a 4 KiB disk read later.
 *
 * WHICH PAGES ARE TIER 1 HERE, AND THE HONEST CAVEAT. On a system with a
 * file-backed page cache, tier 1 is "a clean page that came from a file" -- the
 * file is still on disk, so the copy in RAM is redundant. THIS KERNEL HAS NO
 * FILE-BACKED USER MAPPING. mmap is anonymous only (c/kernel/mm/mmsys.c); an
 * ELF image is read eagerly into anonymous frames by elf_load; a file read
 * through an fd goes into a kmalloc buffer that is not in any user page table.
 * So the classic tier-1 population does not exist to be reclaimed, and saying
 * otherwise would be inventing a result.
 *
 * What DOES exist, and is genuinely the same thing, is the ZERO PAGE. An
 * anonymous page whose 4 KiB are currently all zero can be dropped and
 * re-derived on the next fault by exactly the code that created it in the first
 * place (fault.c do_anon, which zero-fills). It is re-derivable without I/O,
 * which is the entire property tier 1 is about. It is also, on this machine,
 * the population that matters most: browser.aex maps a 105.5 MiB .bss at launch
 * against an actual heap peak of 12.7 MiB, and the difference is ~93 MiB of
 * frames holding zeroes. Tier 1 reclaims that with no device at all.
 *
 * The test for tier 1 is deliberately the CONTENTS, not the dirty bit. "Never
 * written since it was mapped" would also be correct and is cheaper to read,
 * but it misses a page the process itself zeroed, and a 4 KiB comparison costs
 * about a thousandth of the disk write it avoids. The dirty bit is used for
 * what it is actually good for -- telling reclaim which pages are worth
 * sampling -- not as a proxy for content.
 *
 * ===========================================================================
 * CHOOSING A VICTIM: A CLOCK OVER PHYSICAL FRAMES
 *
 * Evicting at random is worse than failing honestly: it takes the page the
 * process is about to touch as readily as the one it will never touch again,
 * and turns a shortage into a thrash. So reclaim needs to know what is being
 * used, and the only evidence the hardware offers is the ACCESSED bit the CPU
 * sets in a PTE on every reference, and the DIRTY bit it sets on every write.
 *
 * The algorithm is a CLOCK (second chance), and the choice is worth defending
 * against the obvious alternative:
 *
 *   An active/inactive LRU pair is more precise, but precision costs list
 *   surgery, and the surgery would be driven by exactly the same accessed-bit
 *   SAMPLING the clock does -- there is no hardware notification on reference,
 *   so a "recently used" list can only be built by periodically reading the
 *   same bits. The lists would therefore add bookkeeping without adding
 *   information. A clock's entire state is one integer (the hand), its sweep IS
 *   the sampling pass, and it degrades gracefully: under light pressure the
 *   hand barely moves, under heavy pressure it approximates LRU.
 *
 * The hand sweeps PHYSICAL frame numbers, not virtual addresses, which is only
 * possible because rmap.h exists. For each frame the hand lands on, the reverse
 * map gives every PTE that points at it; the accessed bits of all of them are
 * OR-ed together. Referenced -> clear them all and move on (the second chance).
 * Not referenced -> it is a candidate. Sweeping physically is the right axis
 * because the thing being reclaimed is a frame: a virtual sweep visits a shared
 * frame once per sharer and has to be told, separately, that it is the same
 * frame.
 *
 * WHAT IT DOES NOT DO, so nobody has to find out the hard way: there is no
 * second hand (no separate clear-ahead pass), so a burst of new allocations is
 * not scan-resistant; there is no per-process fairness, so a large idle process
 * is preferred over a small busy one only to the extent the accessed bits say
 * so; and there is no swap cache, so a page evicted, faulted in and evicted
 * again is written twice even if it was never modified in between.
 *
 * ===========================================================================
 * WHAT MAY NEVER BE EVICTED, AND HOW THAT IS ENFORCED
 *
 * Not by a list of exceptions. A frame is a candidate only if EVERY reference
 * to it is a user PTE whose address the reverse map holds:
 *
 *     rmap_count(f) == pmm_refcount(f),  nonzero, chain not truncated,
 *     and pmm_pincount(f) == 0.
 *
 * Page tables, kheap arenas, DMA rings, the rmap's own tables, the kernel image
 * and the multiboot block all hold references that are not user PTEs, so their
 * rmap count is 0 and their refcount is not -- they fail the test structurally
 * and cannot be reached by any policy change. pmm_pin() covers the other case:
 * a frame that IS user memory and IS fully mapped, but that the kernel or a
 * device is touching right now. See rmap.h for the long form of the argument.
 *
 * ===========================================================================
 * YOU CANNOT ALLOCATE MEMORY IN ORDER TO FREE MEMORY
 *
 * This is where naive swap implementations deadlock, under exactly the pressure
 * they were built for: the write-out path calls kmalloc for a buffer or a
 * descriptor, the allocation fails because memory is exhausted, and the one
 * mechanism that could have fixed the exhaustion is the thing that is stuck.
 *
 * The answer here is that the write-out path ALLOCATES NOTHING. Every structure
 * it touches is sized and taken from the PMM before any pressure can exist:
 *
 *   - the reverse-map node pool          rmap_init(), at pmm_init time;
 *   - the swap slot refcount table       swap_init(), at boot;
 *   - the saved-PTE array                a fixed on-stack array of
 *                                        RECLAIM_MAX_SHARERS entries;
 *   - the page's own bytes               written straight out of the frame,
 *                                        which is identity-mapped, so there is
 *                                        no bounce buffer to allocate.
 *
 * Removing a reverse-map entry RETURNS a node to the free list and never takes
 * one, so even the rmap cannot fail during eviction. This is asserted, not
 * asserted-in-a-comment: the pressure test runs reclaim with the frame
 * allocator at zero free frames and requires it to still make progress.
 *
 * The remaining allocation is on the swap-IN side: a fault that needs a page
 * back needs a frame to put it in. That one is real and unavoidable, and it is
 * why pmm keeps a small RESERVE that only the fault path may draw on
 * (pmm_alloc_reserve). Without it a machine deep in a thrash can be unable to
 * fault anything back in, which is a livelock rather than a deadlock but is
 * just as dead from outside.
 * =========================================================================== */

/* The longest sharing chain reclaim will evict in one go. Beyond this the frame
 * is skipped (and counted), because the saved-PTE array is on the stack and a
 * kernel stack is 32 KiB. Sixteen sharers of one frame does not occur in this
 * kernel today: NPROC is small and the sharing that exists comes from fork
 * chains a few deep. */
#define RECLAIM_MAX_SHARERS 16

void reclaim_init(void);

/* Free at least `want` frames if possible; returns how many were actually
 * freed. Never allocates. Safe to call with the frame allocator empty -- that
 * is the case it exists for. */
uint64_t reclaim_frames(uint64_t want);

/* The pressure trigger. pmm_alloc() calls this before serving a request; it
 * runs a reclaim pass when free memory is below the low watermark and does
 * nothing otherwise. Reentrant-safe: a reclaim pass that itself reaches
 * pmm_alloc (it should not, and is asserted not to) will not recurse. */
void reclaim_on_alloc(void);

/* Watermarks, in frames. Reclaim starts when free < low and stops when free >=
 * high. Defaults are set from total RAM in reclaim_init(). */
void     reclaim_set_watermarks(uint64_t low, uint64_t high);
uint64_t reclaim_low(void);
uint64_t reclaim_high(void);

void reclaim_set_enabled(int on);
int  reclaim_enabled(void);

/* --- counters. A reclaim path nobody has watched run is not a reclaim path -- */
uint64_t reclaim_runs(void);          /* passes of the hand */
uint64_t reclaim_scanned(void);       /* frames the hand looked at */
uint64_t reclaim_dropped(void);       /* TIER 1: freed with no I/O */
uint64_t reclaim_swapped(void);       /* TIER 2: written to the swap device */
uint64_t reclaim_second_chance(void); /* referenced, accessed bit cleared instead */
uint64_t reclaim_skip_unmapped(void); /* no reverse-map entry: kernel memory */
uint64_t reclaim_skip_pinned(void);   /* explicitly pinned */
uint64_t reclaim_skip_partial(void);  /* rmap_count != refcount, or chain truncated */
uint64_t reclaim_skip_wide(void);     /* more sharers than RECLAIM_MAX_SHARERS */
uint64_t reclaim_skip_busy(void);     /* the space is running on another core */
uint64_t reclaim_backoffs(void);      /* passes that found nothing and backed off */
uint64_t reclaim_fail_noslot(void);   /* swap area full */
uint64_t reclaim_fail_io(void);       /* the write failed; the page was put back */
uint64_t reclaim_bugs(void);          /* rmap said a PTE was here and it was not */
uint64_t reclaim_cycles(void);

/* Swap-in: the other half, called from the fault path. `pte` is the swap entry.
 * Returns 1 if the page is now present and the instruction may be retried. */
int  reclaim_swapin(uint64_t cr3, uint64_t va, uint64_t *pte, int active);
uint64_t reclaim_swapins(void);
uint64_t reclaim_swapin_fail(void);

void reclaim_report(const char *tag);

#endif /* LOGIT_RECLAIM_H */
