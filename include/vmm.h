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

#endif /* AQUA_VMM_H */
