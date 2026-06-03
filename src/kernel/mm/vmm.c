#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

void *memset(void *, int, size_t);     /* lib/string.c */
void *memcpy(void *, const void *, size_t);

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
        table[idx] = frame | PRESENT | WRITABLE | USER;
    } else {
        /* Keep the path user-reachable; leaf PTE flags still gate access, so
         * kernel-only pages (no USER on their final entry) stay protected. */
        table[idx] |= USER;
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

/* --- per-process address spaces --- */

/* The user region (where every app's image + stack lives, 0x40000000..) sits
 * under PML4[0], PDPT[1]. A per-process space keeps the kernel's PML4[0] but
 * swaps in a *private* PDPT so each app's PDPT[1] sub-tree is isolated, while
 * still sharing the kernel's other PDPT entries (identity low mem, framebuffer). */
#define USER_PML4_IDX 0
#define USER_PDPT_IDX 1

static uint64_t g_kernel_cr3;

uint64_t vmm_kernel_cr3(void)
{
    if (!g_kernel_cr3)
        __asm__ volatile ("mov %%cr3, %0" : "=r"(g_kernel_cr3));
    return g_kernel_cr3 & ~(uint64_t)0xFFF;
}

void vmm_switch(uint64_t cr3)
{
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

uint64_t vmm_new_space(void)
{
    uint64_t kcr3 = vmm_kernel_cr3();
    uint64_t *kpml4 = (uint64_t *)kcr3;
    uint64_t *kpdpt = (uint64_t *)(kpml4[USER_PML4_IDX] & ~(uint64_t)0xFFF);

    uint64_t pml4 = pmm_alloc();
    uint64_t pdpt = pmm_alloc();
    if (!pml4 || !pdpt) return 0;

    /* Copy the kernel PML4 wholesale: every region stays mapped by default. */
    memcpy((void *)pml4, (void *)kcr3, 4096);
    /* Copy the kernel's low PDPT, then give this space its own PDPT so its
     * user sub-tree (PDPT[1]) can diverge without touching the kernel's. */
    memcpy((void *)pdpt, (void *)kpdpt, 4096);
    ((uint64_t *)pdpt)[USER_PDPT_IDX] = 0;          /* private, populated lazily */
    ((uint64_t *)pml4)[USER_PML4_IDX] = pdpt | PRESENT | WRITABLE | USER;
    return pml4;
}

/* Like next_table() but walks the table tree rooted at an explicit PML4. */
void vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t *)(cr3 & ~(uint64_t)0xFFF);
    uint64_t *pdpt = next_table(pml4, (virt >> 39) & 0x1FF);
    uint64_t *pd   = next_table(pdpt, (virt >> 30) & 0x1FF);
    uint64_t *pt   = next_table(pd,   (virt >> 21) & 0x1FF);

    pt[(virt >> 12) & 0x1FF] = (phys & ~(uint64_t)0xFFF) | flags | PRESENT;
    /* No invlpg: this space is not active while being populated. */
}

static int user_page_ok(uint64_t cr3, uint64_t virt, int write)
{
    uint64_t *pml4 = (uint64_t *)(cr3 & ~(uint64_t)0xFFF);
    uint64_t e = pml4[(virt >> 39) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER)) return 0;
    uint64_t *pdpt = (uint64_t *)(e & ~(uint64_t)0xFFF);
    e = pdpt[(virt >> 30) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER)) return 0;
    uint64_t *pd = (uint64_t *)(e & ~(uint64_t)0xFFF);
    e = pd[(virt >> 21) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER)) return 0;
    uint64_t *pt = (uint64_t *)(e & ~(uint64_t)0xFFF);
    e = pt[(virt >> 12) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER)) return 0;
    if (write && !(e & WRITABLE)) return 0;
    return 1;
}

int vmm_user_range_ok(uint64_t cr3, const void *ptr, uint64_t len, int write)
{
    if (!ptr || !cr3) return 0;
    if (len == 0) return 1;
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + len - 1;
    if (end < start) return 0;
    if ((start >> 47) != 0 || (end >> 47) != 0) return 0;
    for (uint64_t p = start & ~(uint64_t)0xFFF;; p += 0x1000) {
        if (!user_page_ok(cr3, p, write)) return 0;
        if (p >= (end & ~(uint64_t)0xFFF)) break;
    }
    return 1;
}
