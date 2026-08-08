#ifndef LOGIT_MM_H
#define LOGIT_MM_H

#include <stdint.h>

/* The page-fault half of memory management.
 *
 * Copy-on-write and demand paging are DEFINED by what happens on a #PF, so
 * this header is the contract between c/kernel/mm/ and the trap path.
 *
 * ------------------------------------------------------------------------
 * WIRING (one line, in a file this line does not own -- see the commit
 * message and the report). c/kernel/cpu/interrupts.c, at the top of the
 * `if (r->vector < 32)` exception block, BEFORE the ring-3 kill:
 *
 *     #include "mm.h"
 *     ...
 *     if (r->vector < 32) {
 *         if (r->vector == 14 && mm_fault(mm_cr2(), r->error_code, r->rip))
 *             goto done;          // copy-on-write / demand page: retry the insn
 *         if (r->cs & 3) { ... existing ring-3 kill ... }
 *         panic_exception(r);
 *     }
 *
 * Everything mm_fault() does not claim returns 0 and falls through to exactly
 * the behaviour that exists today: a ring-3 fault kills that app alone, a
 * ring-0 fault panics. Fault containment therefore cannot regress by adding
 * the hook -- only by mm_fault() wrongly claiming a fault, which is what the
 * decision table in fault.c and its host test exist to prevent.
 *
 * Until the hook is wired, MM_COW_DEFAULT below MUST stay 0: with no handler,
 * the first write to a shared page kills the process. The boot report says
 * which mode is live, out loud, on every boot.
 * ------------------------------------------------------------------------ */

/* Flip to 1 in the same commit that adds the interrupts.c hook above. */
#define MM_COW_DEFAULT 1

/* The private user region (PML4[0]/PDPT[1]); see elf.c, which rejects any
 * program segment outside it. mm only ever claims faults inside this range. */
#define MM_USER_BASE 0x40000000ull
#define MM_USER_END  0x80000000ull

/* Where mmap() hands out address space: the top of the user region, growing
 * down, well clear of every app's image + BSS + stack. The highest link base is
 * 0x50000000 (CLI programs) with a 64 MiB image+stack window above it, and the
 * browser's 96 MiB BSS arena tops out well below 0x60000000. */
#define MM_MMAP_BASE 0x60000000ull
#define MM_MMAP_TOP  0x7F000000ull      /* 16 MiB of guard below MM_USER_END */

static inline uint64_t mm_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* Resolve a page fault. Returns 1 if the fault was handled and the faulting
 * instruction should be retried, 0 if this is a genuine fault the caller must
 * handle as before (kill the process / panic).
 *
 * `err` is the CPU's page-fault error code:
 *   bit 0 P    0 = the page was not present, 1 = a protection violation
 *   bit 1 W    1 = the access was a write
 *   bit 2 U    1 = the access came from ring 3
 *   bit 3 RSVD 1 = a reserved bit was set in a page-table entry
 *   bit 4 I    1 = instruction fetch */
int mm_fault(uint64_t cr2, uint64_t err, uint64_t rip);

/* Same, against an explicit address space, for the kernel-writes-user-memory
 * path (usercopy.c) rather than an actual trap. `err` uses the same bits. */
int mm_fault_in(uint64_t cr3, uint64_t va, uint64_t err);

/* The decision half of mm_fault, split out so it can be tested exhaustively
 * without a page table: what KIND of fault is this? */
enum mm_fault_kind {
    MM_FAULT_NONE = 0,      /* not ours: kill the process / panic, as today */
    MM_FAULT_COW,           /* a write to a copy-on-write page */
    MM_FAULT_ANON,          /* first touch of an anonymous (mmap) page */
    MM_FAULT_SWAP,          /* the page is on the swap device; read it back */
};
/* `pte_swap` is 1 when the entry is a swap entry (see vmm.h). It is checked
 * FIRST and independently of the VMA, because a swap entry is self-evidently
 * ours -- the kernel wrote it -- and because pages that have no VMA at all (an
 * ELF image's text, which elf_load maps directly) must still be able to come
 * back. Getting this order wrong is silent data loss: a swapped page that falls
 * through to the anonymous case is refilled with zeroes. */
int mm_fault_classify(uint64_t cr2, uint64_t err, int pte_present,
                      int pte_cow, int pte_user, int vma_prot, int pte_swap);

void     mm_set_cow(int on);
int      mm_cow_enabled(void);

/* Counters, all monotonic since boot. */
uint64_t mm_cow_faults(void);      /* write faults resolved by copying */
uint64_t mm_cow_reuse(void);       /* write faults resolved WITHOUT copying (sole owner) */
uint64_t mm_anon_faults(void);     /* first-touch anonymous pages filled */
uint64_t mm_fault_declined(void);  /* faults mm did not claim (genuine faults) */
uint64_t mm_swapin_faults(void);   /* faults resolved by reading the swap device */
uint64_t mm_cow_pages(void);       /* pages currently mapped copy-on-write */

/* mm-internal: leaf PTEs currently carrying VMM_PTE_COW. Raised by the clone
 * (vmm.c), lowered by the fault that resolves one and by every unmap, so the
 * three files that move it share the counter directly rather than through an
 * accessor that would hide which of them is out of step. */
extern uint64_t g_mm_cow_pages;

/* One line of mm accounting on the console. `tag` says what prompted it. */
void mm_report(const char *tag);

/* SYS_MMAP / SYS_MUNMAP / SYS_MEMINFO, handled in c/kernel/mm/mmsys.c. The
 * dispatcher (c/kernel/exec/syscall.c, not this line's file) forwards those
 * three numbers here; see the header comment in mmsys.c for the exact case. */
long mm_syscall(long num, long a, long b, long c);

#endif /* LOGIT_MM_H */
