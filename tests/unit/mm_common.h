/* Shared scaffolding for the c/kernel/mm host tests (make test-mm).
 *
 * The point of these tests: the memory-management failure modes are silent and
 * delayed. A missed decrement is an out-of-memory hours later; an extra
 * decrement corrupts whatever code is handed the frame next. Neither shows up
 * in QEMU as anything you can attribute. So the real pmm.c / vmm.c / fault.c
 * are compiled (with -DMM_HOSTTEST, see c/kernel/mm/mmhost.h) against a
 * simulated physical memory, where every refcount, every PTE and every frame's
 * contents can be read back and asserted on. */
#ifndef MM_COMMON_H
#define MM_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* --- test harness ------------------------------------------------------- */
extern int mm_checks, mm_fails;

void mm_ok(int cond, const char *fmt, ...);
void mm_eqi(long long got, long long want, const char *what);
/* Same, with a formatted label -- for assertions inside a loop, where the index
 * is the only thing that says WHICH one failed. */
void mm_eqf(long long got, long long want, const char *fmt, ...);
int  mm_summary(const char *suite);   /* prints; returns process exit status */

/* --- simulated physical memory ------------------------------------------ */
/* Build `mib` MiB of simulated RAM, plant a Multiboot2 info block in it and run
 * the real pmm_init over it. Returns the info-block physical address. */
uint64_t mm_sim_init(unsigned mib);
void     mm_sim_done(void);

/* Physical address -> host pointer, for reading a frame's contents back. */
void    *mm_sim_ptr(uint64_t phys);

/* Build the "kernel" address space the way boot.asm + kmain leave it: a PML4
 * whose entry 0 points at a PDPT whose entry 1 (the private user region) is
 * empty. vmm_new_space() clones that shape, so any test that calls it needs
 * this first. Sets mm_host_cr3 and returns the PML4. */
uint64_t mm_sim_kernel_space(void);

/* --- simulated swap device ----------------------------------------------
 * c/kernel/mm/swap.c is compiled with the two calls that touch hardware
 * replaced by these (see the MM_HOSTTEST block at the top of that file).
 * Everything above them -- the slot allocator, the refcounting, the header,
 * the queueing -- is the code the kernel runs.
 *
 * `mib` of 0 means "no device", which is the case reclaim has to survive: a
 * machine with no swap must still run its drop tier. */
void     mm_sim_swap(unsigned mib);
void     mm_sim_swap_free(void);
/* Make every transfer fail from now on, so the write-failure path (which must
 * put the page back exactly as it was) can be driven on purpose. */
void     mm_sim_swap_fail(int on);
uint64_t mm_sim_swap_reads(void);
uint64_t mm_sim_swap_writes(void);
/* Read a slot's raw bytes without going through swap.c, so a test can check
 * what actually landed on the "device". */
void    *mm_sim_swap_slot_bytes(uint64_t slot);

#endif
