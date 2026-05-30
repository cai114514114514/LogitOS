#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"

#define PRESENT  0x1
#define WRITABLE 0x2

void *memset(void *, int, size_t);     /* lib/string.c */

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Return the next-level table for `idx`, allocating and linking it if absent.
 * Physical frames live in the identity-mapped low region, so a frame's
 * physical address is directly usable as a pointer. */
static uint64_t *next_table(uint64_t *table, int idx)
{
    if (!(table[idx] & PRESENT)) {
        uint64_t frame = pmm_alloc();
        memset((void *)frame, 0, 4096);
        table[idx] = frame | PRESENT | WRITABLE;
    }
    return (uint64_t *)(table[idx] & ~(uint64_t)0xFFF);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint64_t *pml4 = (uint64_t *)(cr3 & ~(uint64_t)0xFFF);
    uint64_t *pdpt = next_table(pml4, (virt >> 39) & 0x1FF);
    uint64_t *pd   = next_table(pdpt, (virt >> 30) & 0x1FF);
    uint64_t *pt   = next_table(pd,   (virt >> 21) & 0x1FF);

    pt[(virt >> 12) & 0x1FF] = (phys & ~(uint64_t)0xFFF) | flags | PRESENT;
    invlpg(virt);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)
{
    uint64_t end = (virt + size + 0xFFF) & ~(uint64_t)0xFFF;
    virt &= ~(uint64_t)0xFFF;
    phys &= ~(uint64_t)0xFFF;
    for (; virt < end; virt += 4096, phys += 4096)
        vmm_map_page(virt, phys, flags);
}
