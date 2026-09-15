#ifndef LOGIT_MM_HOST_H
#define LOGIT_MM_HOST_H

/* The one testability seam in c/kernel/mm/.
 *
 * pmm.c/vmm.c/fault.c are the files where a bug is silent and delayed (a frame
 * freed while another address space still maps it corrupts something else,
 * later, somewhere unrelated). That is exactly the code that has to be exercised
 * by a test, and it cannot be exercised in QEMU alone: on a real boot there is
 * no way to ask "is the refcount on frame 0x31000 what it should be after those
 * 40 forks?" without a debugger.
 *
 * Everything that makes those files x86-and-kernel-only funnels through here:
 *   - a physical address used as a pointer (the kernel identity-maps the low
 *     1 GiB, so phys == virt; on the host it is an offset into one big arena),
 *   - the address the kernel image ends at (a linker symbol on the kernel),
 *   - CR3 and invlpg.
 * Compile the same .c files with -DMM_HOSTTEST and they run on the host over a
 * simulated physical memory, with the real algorithms untouched.
 *
 * The kernel build takes the #else branch, which is exactly the code that was
 * inline in those files before, so the seam costs the kernel nothing. */

#include <stdint.h>
#include <stddef.h>
#include "physmap.h"

#ifdef MM_HOSTTEST

extern uint64_t mm_host_base;   /* host address that "physical 0" maps to */
extern uint64_t mm_host_kend;   /* the physical address the kernel image ends at */
#ifdef MM_CONCURRENT
extern _Thread_local uint64_t mm_host_cr3;
#else
extern uint64_t mm_host_cr3;
#endif    /* the "active" CR3 */

static inline void *mm_p2v(uint64_t phys)     { return (void *)(uintptr_t)(mm_host_base + phys); }
static inline void *mm_physmap_ptr(uint64_t phys)
{ return phys < PHYSMAP_SIZE ? mm_p2v(phys) : NULL; }
/* The simulated arena remains a physical-offset mapping; its virtual address
 * is deliberately unrelated to either guest alias. */
static inline uint64_t mm_v2p(const void *ptr)
{
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    return va >= mm_host_base && va - mm_host_base < PHYSMAP_SIZE
        ? va - mm_host_base : MM_PHYS_INVALID;
}
static inline uint64_t mm_kernel_end_phys(void) { return mm_host_kend; }
static inline uint64_t mm_read_cr3(void)      { return mm_host_cr3; }
static inline void mm_write_cr3(uint64_t c)   { mm_host_cr3 = c; }
static inline void mm_invlpg(uint64_t a)      { (void)a; }

#else

extern char _kernel_end[];      /* provided by linker.ld */

/* Correction (2026-09-09), beside the old identity-only contract above:
 * low RAM stays identity-mapped for existing heap/DMA callers, while high RAM
 * is reachable only through the supervisor direct map built by pmm_init. A
 * physical address is still the value kept in PTEs, CR3 and DMA descriptors. */
static inline void *mm_p2v(uint64_t phys)
{
    if (phys >= PHYSMAP_SIZE) return NULL;
    return (void *)(uintptr_t)(phys < PMM_LOW_LIMIT ? phys : PHYSMAP_BASE + phys);
}
/* DMA allocations use the independent alias even when physical RAM is low.
 * Arithmetic only: callers validate the backing interval with pmm_is_ram. */
static inline void *mm_physmap_ptr(uint64_t phys)
{ return phys < PHYSMAP_SIZE ? (void *)(uintptr_t)(PHYSMAP_BASE + phys) : NULL; }
/* Translate only a kernel RAM alias, not an arbitrary/user virtual pointer.
 * Callers must already own RAM in that alias; this arithmetic is not a DMA
 * reachability or physical-contiguity test. Holes have no high mapping. */
static inline uint64_t mm_v2p(const void *ptr)
{
    uint64_t va = (uint64_t)(uintptr_t)ptr;
    if (va < PMM_LOW_LIMIT) return va;
    if (va >= PHYSMAP_BASE && va - PHYSMAP_BASE < PHYSMAP_SIZE)
        return va - PHYSMAP_BASE;
    return MM_PHYS_INVALID;
}
static inline uint64_t mm_kernel_end_phys(void) { return (uint64_t)_kernel_end; }
static inline uint64_t mm_read_cr3(void)
{ uint64_t c; __asm__ volatile ("mov %%cr3, %0" : "=r"(c)); return c; }
static inline void mm_write_cr3(uint64_t c)
{ __asm__ volatile ("mov %0, %%cr3" :: "r"(c) : "memory"); }
static inline void mm_invlpg(uint64_t a)
{ __asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory"); }

#endif

#endif /* LOGIT_MM_HOST_H */
