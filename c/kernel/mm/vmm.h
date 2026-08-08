#ifndef LOGIT_VMM_H
#define LOGIT_VMM_H

#include <stdint.h>

#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
#define VMM_NOCACHE  (0x8 | 0x10)   /* PWT | PCD */

/* ------------------------------------------------------------- PTE BITS --
 * Bits 9-11 of a PTE are ignored by the CPU and reserved for the OS. This is
 * the whole map of what c/kernel/mm/ uses, written in one place because a page
 * lifetime line and an NX/permissions line editing the same 64 bits from
 * different files is how a PTE flag gets used twice:
 *
 *   bit  1   W        hardware: writable
 *   bit  2   U        hardware: user-accessible
 *   bit  5   A        hardware: set by the CPU on any reference   (reclaim reads)
 *   bit  6   D        hardware: set by the CPU on a write         (reclaim reads)
 *   bit  9   COW      OS: writable-but-shared, resolve on write   (mm owns)
 *   bit 10   ANON     OS: this page was zero-filled on demand and can be
 *                     re-derived by faulting it again             (mm owns)
 *   bit 11   --       OS: FREE
 *   bits 52-62        OS: FREE
 *   bit 63   NX       hardware: no-execute (the user/kernel-separation line)
 *
 * mm claims 9 and 10 and nothing else. */
#define VMM_PTE_COW  (1ull << 9)

/* Zero-fill-on-demand origin. Set by the anonymous fault, carried through fork
 * (the clone copies PTE flags wholesale) and through a copy-on-write copy.
 * Reclaim's cheapest tier needs it: a page may only be dropped and re-derived
 * for free if the code that re-derives it produces the same bytes, and only an
 * anonymous page's re-derivation (zero fill) is known to. An ELF text page's
 * contents came from a file that nothing re-reads, so it must be swapped. */
#define VMM_PTE_ANON (1ull << 10)

/* The hardware reference bits, named. Reclaim samples A to find pages nobody is
 * using, and reads D to know whether a page has been modified. */
#define VMM_PTE_ACCESSED (1ull << 5)
#define VMM_PTE_DIRTY    (1ull << 6)

/* ------------------------------------------------------------ SWAP ENTRY --
 * A page that has been written to the swap device leaves its PTE NOT PRESENT,
 * with the slot number in it. With P=0 the CPU ignores every other bit, so the
 * whole entry is ours -- but bit 0 must stay clear and the entry must be
 * distinguishable from a plain zero PTE (an address that was never mapped), so
 * bit 1 is the marker and slot numbers start at 1.
 *
 *   bit  0    0        not present
 *   bit  1    1        SWAP marker
 *   bit  2    saved W        )  the permissions the page had before it left,
 *   bit  3    saved COW      )  so faulting it back in restores them. They
 *   bit  4    saved ANON     )  cannot stay in their normal positions: bit 1
 *                              (W) is the marker and bit 2 (U) would collide.
 *   bits 12+  slot number
 *   bit  63   NX       left exactly where it is, so it survives untouched */
#define VMM_PTE_SWAP     (1ull << 1)
#define VMM_SWAP_W       (1ull << 2)
#define VMM_SWAP_COW     (1ull << 3)
#define VMM_SWAP_ANON    (1ull << 4)
#define VMM_SWAP_SHIFT   12

static inline int vmm_pte_is_swap(uint64_t e)
{ return !(e & 1) && (e & VMM_PTE_SWAP) != 0; }
static inline uint64_t vmm_pte_swap_slot(uint64_t e)
{ return e >> VMM_SWAP_SHIFT; }

/* Map one 4 KiB page (`virt` -> `phys`) into the active address space,
 * allocating intermediate page tables from the PMM as needed. PRESENT is
 * always set; pass extra flags such as VMM_WRITABLE. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map a contiguous range (rounded out to page boundaries). */
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* --- per-process address spaces --- */

/* Physical address of the kernel's boot PML4 (the shared address space). */
uint64_t vmm_kernel_cr3(void);

/* Build a new address space: a fresh PML4 that shares the kernel's top-level
 * entries (identity-mapped low memory, framebuffer MMIO) but gets its own
 * private user region. Returns the new PML4 physical address, or 0 on failure. */
uint64_t vmm_new_space(void);

/* Map one 4 KiB page into the address space rooted at PML4 physical `cr3`
 * (rather than the active one), so an app's space can be populated before the
 * switch. The private user PDPT is copied-on-first-use off the shared kernel
 * PDPT so kernel/framebuffer stay mapped. */
void vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);

/* Install a complete, NOT-present PTE (i.e. a swap entry) in `cr3`. mm-internal:
 * the fork clone uses it to give a child its parent's swapped-out pages. */
void vmm_map_raw_in(uint64_t cr3, uint64_t virt, uint64_t entry);

/* Validate that a user buffer is mapped in `cr3` for read/write access.
 * PURE CHECK: it never changes a mapping, so a copy-on-write page reads as
 * NOT writable. Callers that are about to write must use
 * vmm_user_range_fault_in() instead -- see usercopy.c. */
int vmm_user_range_ok(uint64_t cr3, const void *ptr, uint64_t len, int write);

/* Same range check, but RESOLVES anything that is legitimately missing first:
 * a copy-on-write page is unshared, an untouched anonymous (mmap) page is
 * filled. After this returns 1 the whole range is present, and writable if
 * `write`, so the caller's plain memcpy cannot fault. Returns 0 if the range is
 * not valid for the process, in which case nothing was changed. */
int vmm_user_range_fault_in(uint64_t cr3, const void *ptr, uint64_t len, int write);

/* Locate the leaf PTE for `virt` in `cr3` WITHOUT allocating page tables.
 * Returns a pointer to the entry, or NULL if any level is absent or is a large
 * page. mm-internal (fault.c), exported only because it lives in vmm.c. */
uint64_t *vmm_pte(uint64_t cr3, uint64_t virt);

/* fork(): clone the private user subtree of `src_cr3` into `dst_cr3`. With
 * copy-on-write enabled (mm.h) no data page is copied: both spaces are pointed
 * at the same frames, read-only + VMM_PTE_COW, and the frames' refcounts go up.
 * Returns 0 on success, -1 on OOM (caller must vmm_free_space(dst) + fail). */
int vmm_clone_user(uint64_t dst_cr3, uint64_t src_cr3);

/* Pages shared / copied by the most recent vmm_clone_user, for the fork
 * accounting. Reset at the start of each clone. */
void vmm_clone_stats(uint64_t *shared, uint64_t *copied);

/* Unmap [virt, virt+len) in `cr3`, dropping a reference on each frame that was
 * present. Returns the number of pages actually unmapped. */
uint64_t vmm_unmap_range_in(uint64_t cr3, uint64_t virt, uint64_t len);

/* execve(): free the user subtree but keep the (empty) address space alive. */
void vmm_free_user(uint64_t cr3);

/* exit(): tear down an entire address space made by vmm_new_space (not the active CR3). */
void vmm_free_space(uint64_t cr3);

/* Load CR3 (switch the active address space). Also records which space this
 * core is running, for vmm_space_busy_elsewhere() below. */
void vmm_switch(uint64_t cr3);

/* Is `cr3` loaded on some OTHER core right now (or was, until a CR3 write that
 * has not completed)? Reclaim must not unmap a page belonging to a space
 * another core is executing: it cannot shoot down that core's TLB (tlb.h --
 * doing so from under the BKL deadlocks), so it leaves those spaces alone
 * instead. See the long comment above vmm_switch() for why the answer is
 * maintained here and why the update order is what it is. */
int vmm_space_busy_elsewhere(uint64_t cr3);

#endif /* LOGIT_VMM_H */
