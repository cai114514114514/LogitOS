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

#ifdef MM_HOSTTEST

extern uint64_t mm_host_base;   /* host address that "physical 0" maps to */
extern uint64_t mm_host_kend;   /* the physical address the kernel image ends at */
extern uint64_t mm_host_cr3;    /* the "active" CR3 */

static inline void *mm_p2v(uint64_t phys)     { return (void *)(uintptr_t)(mm_host_base + phys); }
static inline uint64_t mm_kernel_end_phys(void) { return mm_host_kend; }
static inline uint64_t mm_read_cr3(void)      { return mm_host_cr3; }
static inline void mm_write_cr3(uint64_t c)   { mm_host_cr3 = c; }
static inline void mm_invlpg(uint64_t a)      { (void)a; }

#else

extern char _kernel_end[];      /* provided by linker.ld */

static inline void *mm_p2v(uint64_t phys)     { return (void *)(uintptr_t)phys; }
static inline uint64_t mm_kernel_end_phys(void) { return (uint64_t)_kernel_end; }
static inline uint64_t mm_read_cr3(void)
{ uint64_t c; __asm__ volatile ("mov %%cr3, %0" : "=r"(c)); return c; }
static inline void mm_write_cr3(uint64_t c)
{ __asm__ volatile ("mov %0, %%cr3" :: "r"(c) : "memory"); }
static inline void mm_invlpg(uint64_t a)
{ __asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory"); }

#endif

#endif /* LOGIT_MM_HOST_H */
