#ifndef LOGIT_PMM_H
#define LOGIT_PMM_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_SIZE 4096

/* A frame's reference count saturates here instead of wrapping. See pmm.c. */
#define PMM_REF_MAX 0xFFFFu

/* Initialise the physical frame allocator from the Multiboot2 info block. */
void pmm_init(uint64_t mb_info_addr);

/* Allocate / free a single 4 KiB physical frame. Returns 0 on failure.
 * A fresh frame comes back with refcount 1. pmm_free DECREMENTS: the frame
 * returns to the free pool only when the last reference goes. */
uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys_addr);

/* Allocate `n` contiguous frames; returns the base physical address or 0. */
uint64_t pmm_alloc_contig(size_t n);

/* Take an additional reference on an already-allocated frame (copy-on-write
 * fork maps one frame into two address spaces). Returns 0, or -1 if the count
 * is saturated -- in which case the caller MUST NOT share the frame and must
 * copy it instead. Never wraps. */
int      pmm_ref(uint64_t phys_addr);
unsigned pmm_refcount(uint64_t phys_addr);

/* --- accounting. Memory bugs are diagnosed with numbers or not at all. --- */
uint64_t pmm_total_bytes(void);   /* usable RAM reported by firmware */
uint64_t pmm_free_bytes(void);    /* currently free */
uint64_t pmm_total_frames(void);
uint64_t pmm_used_frames(void);   /* frames with refcount >= 1 */
uint64_t pmm_free_frames(void);
uint64_t pmm_shared_frames(void); /* frames with refcount >= 2 */
uint64_t pmm_pinned_frames(void); /* frames whose count saturated: never freeable */
uint64_t pmm_refs_total(void);    /* sum of every frame's refcount */
uint64_t pmm_bugs(void);          /* invariant violations detected so far */

/* Frame poisoning (guardrail: turn "corruption somewhere, someday" into
 * "assertion, here, now"). Level 0 = off, 1 = poison+verify the first
 * PMM_POISON_HEAD bytes of every freed frame (the default: one cache line, so
 * it is always on), 2 = the whole 4 KiB (debug/tests). */
#define PMM_POISON_HEAD 64
void pmm_set_poison(int level);
int  pmm_poison_level(void);

/* O(total_frames) consistency walk: re-derives used/shared/pinned/refs from the
 * refcount table and compares them with the running counters, and checks that
 * "bitmap bit set" and "refcount >= 1" agree for every frame. Returns the
 * number of inconsistencies (0 = healthy) and prints each one. */
int  pmm_audit(void);

/* One line of frame accounting on the serial console. */
void pmm_report(const char *tag);

/* The self-triggering leak trace (see pmm.c). The allocator announces each new
 * all-time low in free frames, at most once per `frames` step -- so a busy
 * system goes quiet and a leaking one never does. Default step 256 frames
 * (1 MiB). pmm_low_free_frames() is the mark itself, for a test that wants the
 * number rather than the log. */
void     pmm_watch_step(uint64_t frames);
uint64_t pmm_low_free_frames(void);

#endif /* LOGIT_PMM_H */
