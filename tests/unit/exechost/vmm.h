#ifndef EXECHOST_VMM_H
#define EXECHOST_VMM_H
/* The slice of c/kernel/mm/vmm.h the loader uses. Deliberately a SLICE: if the
 * loader starts needing more of the memory manager, this test stops compiling,
 * which is the notification you want. */
#include <stdint.h>
#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
uint64_t *vmm_pte(uint64_t cr3, uint64_t virt);
#endif
