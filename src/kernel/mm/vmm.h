#ifndef AQUA_VMM_H
#define AQUA_VMM_H

#include <stdint.h>

#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
#define VMM_NOCACHE  (0x8 | 0x10)   /* PWT | PCD */

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

/* Validate that a user buffer is mapped in `cr3` for read/write access. */
int vmm_user_range_ok(uint64_t cr3, const void *ptr, uint64_t len, int write);

/* fork(): eager-copy the private user subtree of `src_cr3` into `dst_cr3`.
 * Returns 0 on success, -1 on OOM (caller must vmm_free_space(dst) + fail). */
int vmm_clone_user(uint64_t dst_cr3, uint64_t src_cr3);

/* execve(): free the user subtree but keep the (empty) address space alive. */
void vmm_free_user(uint64_t cr3);

/* exit(): tear down an entire address space made by vmm_new_space (not the active CR3). */
void vmm_free_space(uint64_t cr3);

/* Load CR3 (switch the active address space). */
void vmm_switch(uint64_t cr3);

#endif /* AQUA_VMM_H */
