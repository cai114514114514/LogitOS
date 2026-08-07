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
int  mm_summary(const char *suite);   /* prints; returns process exit status */

/* --- simulated physical memory ------------------------------------------ */
/* Build `mib` MiB of simulated RAM, plant a Multiboot2 info block in it and run
 * the real pmm_init over it. Returns the info-block physical address. */
uint64_t mm_sim_init(unsigned mib);
void     mm_sim_done(void);

/* Physical address -> host pointer, for reading a frame's contents back. */
void    *mm_sim_ptr(uint64_t phys);

#endif
