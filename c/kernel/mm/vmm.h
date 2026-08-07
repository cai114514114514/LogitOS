#ifndef LOGIT_VMM_H
#define LOGIT_VMM_H

#include <stdint.h>

#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
#define VMM_NOCACHE  (0x8 | 0x10)   /* PWT | PCD */

/* Copy-on-write marker. Bits 9-11 of a PTE are ignored by the CPU and reserved
 * for the OS; bit 9 means "this page is writable as far as the process is
 * concerned, but is mapped read-only because another address space shares the
 * frame". Nothing outside c/kernel/mm/ needs to know it exists. */
#define VMM_PTE_COW  (1ull << 9)

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

/* Load CR3 (switch the active address space). */
void vmm_switch(uint64_t cr3);

#endif /* LOGIT_VMM_H */
