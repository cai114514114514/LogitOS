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

/* --------------------------------------------------- PTE ADDRESS MASKING --
 *
 * THE ONE MASK, and the bug it closes.
 *
 * Every file under c/kernel/mm/ used to turn a page-table entry back into a
 * physical frame with `e & ~(uint64_t)0xFFF`. That clears the twelve flag bits
 * and KEEPS EVERYTHING ABOVE THE ADDRESS FIELD. The architectural address field
 * of a 4 KiB PTE is bits 12..51; bits 52..62 are available-to-software and bit
 * 63 is NX. So the old mask is correct only for as long as the kernel never
 * sets any of those bits -- and the moment it sets NX, `e & ~0xFFF` yields
 * 0x8000000000nnn000, a physical address eight exabytes up, which then reaches
 * pmm_ref() / pmm_free() / pmm_refcount() (indexed by frame number) and
 * rmap_add() / rmap_remove(). That is not a crash at the mask; it is a
 * corrupted refcount table and a #GP several operations later, in a file
 * nobody edited. It was measured before this change and it is exactly why
 * cpu_prot_nx_usable() used to return 0.
 *
 * MM_PTE_ADDR is therefore the ONLY thing that may extract a frame from a PTE,
 * and it is used for CR3 as well -- CR3's PML4 base is the same bits 12..51,
 * and a mask that is right for one is right for the other.
 *
 * MM_PTE_FLAGS is its complement, and it exists for the other half of the bug:
 * code that carries permissions from one entry to another (the copy-on-write
 * copy in fault.c, the shared PTE the fork clone installs in the child) used
 * `e & 0xFFF`, which SILENTLY DROPS NX. That direction does not corrupt
 * anything -- it quietly re-opens the protection instead, which is worse to
 * find. Carry flags with MM_PTE_FLAGS and both halves stay right.
 *
 * NOT for page-aligning a VIRTUAL address or a length: `va & ~0xFFF` is
 * correct there and MM_PTE_ADDR would truncate a kernel-half address. Those
 * sites are deliberately left alone. */
#define MM_PTE_ADDR  0x000FFFFFFFFFF000ull
#define MM_PTE_FLAGS (~MM_PTE_ADDR)

/* Bit 63 of a leaf entry: no-execute. The same bit as c/kernel/cpu/prot.h's
 * PTE_NX, spelled again here because c/kernel/mm/ is also compiled for the host
 * test, which has no prot.h and no EFER -- so mm may never depend on that
 * header. mm does not DECIDE whether NX is used (elf.c and exec.c do, through
 * cpu_prot_nx_usable()); it only has to carry the bit correctly through fork,
 * copy-on-write, eviction and swap-in, which is what the two masks above are
 * for. The one place mm originates the bit is do_anon(), which honours the
 * VMA's own VMA_EXEC. */
#define MM_PTE_NX    (1ull << 63)

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
    MM_FAULT_FILE,          /* first touch of a FILE-backed page; read it from
                             * the page cache (c/kernel/mm/pcache.c) */
    MM_FAULT_SHM,           /* first touch of a page in a SHARED segment; take a
                             * reference on the frame the segment already holds
                             * (c/kernel/mm/shm.c) */
};
/* MM_FAULT_FILE is APPENDED rather than placed beside MM_FAULT_ANON, which is
 * where it belongs semantically -- fault.c's own comment calls it "purely a
 * refinement of MM_FAULT_ANON: same conditions, and the only difference is
 * where the page's contents come from". Appending anyway, because these values
 * reach tests/unit/mm_*.c as expectations and renumbering an existing kind to
 * make an enum read nicely is how a test starts asserting the wrong thing while
 * still passing. */
/* `pte_swap` is 1 when the entry is a swap entry (see vmm.h). It is checked
 * FIRST and independently of the VMA, because a swap entry is self-evidently
 * ours -- the kernel wrote it -- and because pages that have no VMA at all (an
 * ELF image's text, which elf_load maps directly) must still be able to come
 * back. Getting this order wrong is silent data loss: a swapped page that falls
 * through to the anonymous case is refilled with zeroes. */
/* `vma_file` is 1 when the VMA covering this address names a backing file. It
 * is the LAST term consulted, after the swap marker and after the copy-on-write
 * and present tests, because it distinguishes only where an absent page's bytes
 * come from -- not whether the fault is ours. A file page that was SWAPPED (it
 * can be: an invalidated-but-still-mapped page loses its cache entry, and then
 * reclaim can only swap it) carries the marker and is caught first, so it comes
 * back from the slot and NOT from the file -- correct, because by then the
 * file's bytes may have moved on and the process is entitled to the ones it was
 * given. */
/* `vma_shm` is 1 when the VMA covering this address names a shared segment. It
 * sits beside `vma_file` and is consulted at the same point for the same
 * reason: like the file case it distinguishes only WHERE an absent page's bytes
 * come from, not whether the fault is ours. The two are mutually exclusive
 * (vma.h) and `vma_file` is asked first, so a caller that somehow passed both
 * gets the file answer rather than an undefined one.
 *
 * Unlike the file case, a shared page can be written: the segment IS the
 * storage, so there is nothing to write back and no dirty state to track. The
 * write permission comes from the VMA and is already tested two clauses above,
 * which is why this needs no clause of its own for it. */
int mm_fault_classify(uint64_t cr2, uint64_t err, int pte_present,
                      int pte_cow, int pte_user, int vma_prot, int pte_swap,
                      int vma_file, int vma_shm);

void     mm_set_cow(int on);
int      mm_cow_enabled(void);

/* Counters, all monotonic since boot. */
uint64_t mm_cow_faults(void);      /* write faults resolved by copying */
uint64_t mm_cow_reuse(void);       /* write faults resolved WITHOUT copying (sole owner) */
uint64_t mm_anon_faults(void);     /* first-touch anonymous pages filled */
uint64_t mm_fault_declined(void);  /* faults mm did not claim (genuine faults) */
uint64_t mm_swapin_faults(void);   /* faults resolved by reading the swap device */
uint64_t mm_oom_retries(void);     /* faults that ran out of memory and asked the
                                    * out-of-memory killer for one more chance */
uint64_t mm_oom_saved(void);       /* ...and the ones that then SUCCEEDED, i.e.
                                    * processes that would have died before
                                    * c/kernel/mm/oom.c existed */
uint64_t mm_shm_faults(void);      /* first touches of a shared-segment page */
uint64_t mm_cow_pages(void);       /* pages currently mapped copy-on-write */

/* mm-internal: leaf PTEs currently carrying VMM_PTE_COW. Raised by the clone
 * (vmm.c), lowered by the fault that resolves one and by every unmap, so the
 * three files that move it share the counter directly rather than through an
 * accessor that would hide which of them is out of step. */
extern uint64_t g_mm_cow_pages;

/* One line of mm accounting on the console. `tag` says what prompted it. */
void mm_report(const char *tag);

/* SYS_MMAP / SYS_MMAP_FILE / SYS_MUNMAP / SYS_MEMINFO, handled in
 * c/kernel/mm/mmsys.c. The dispatcher (c/kernel/exec/syscall.c, not this
 * line's file) forwards those numbers here; see the header comment in
 * mmsys.c for the exact case. */
long mm_syscall(long num, long a, long b, long c);

#endif /* LOGIT_MM_H */
