/* Wide virtual addresses are a reservation contract, not a RAM stress test.
 * A 64 MiB simulated machine reserves TiB, touches separate PML4/PDPT/PT
 * branches, forks them, and checks exact bytes/refcounts on both sides. The
 * WIDEVA_SKIP_CLONE control retains the legacy-only fork to prove that merely
 * accepting high mmap addresses cannot satisfy this test. Real MM sources,
 * including the reverse map and swap, are linked by wideva_run.py. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mm_common.h"
#include "mm.h"
#include "mmhost.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "rmap.h"
#include "swap.h"

static uint64_t entry(uint64_t space, uint64_t va)
{
    uint64_t *p = vmm_pte(space, va);
    return p ? *p : 0;
}

static void range_contract(void)
{
    uint64_t a, b;
    mm_ok(mm_user_range(MM_USER_BASE, MM_USER_END - MM_USER_BASE), "whole legacy window");
    mm_ok(mm_user_range(MM_USER_WIDE_BASE, MM_USER_WIDE_END - MM_USER_WIDE_BASE), "whole wide window");
    mm_ok(!mm_user_range(MM_USER_BASE, MM_USER_WIDE_BASE - MM_USER_BASE + 4096), "union rejects gap crossing");
    mm_ok(!mm_user_range(MM_USER_WIDE_END - 4096, 4097), "reject noncanonical upper crossing");
    mm_ok(!mm_user_range(MM_USER_WIDE_BASE, UINT64_MAX), "reject length overflow");
    mm_ok(!mm_user_addr(MM_USER_END), "legacy end is excluded");
    mm_ok(!mm_user_addr(MM_USER_WIDE_BASE - 1), "gap ends before wide base");
    mm_ok(!mm_user_addr(MM_USER_WIDE_END), "wide end is excluded");
    mm_ok(!mm_user_addr(0xffff800000000000ull), "physmap is not user");
    mm_ok(vma_range(MM_USER_WIDE_END - 1, 1, &a, &b) == 0 && a == MM_USER_WIDE_END - 4096 && b == MM_USER_WIDE_END, "round final byte without overflowing");
    mm_ok(vma_range(MM_USER_END - 1, 2, &a, &b) < 0, "rounded range cannot cross kernel gap");
}

static void reservations(void)
{
    uint64_t free0 = pmm_free_frames(), sp = vmm_new_space();
    uint64_t legacy = vma_reserve(sp, 0, 4096, VMA_READ | VMA_WRITE);
    mm_eqi(legacy, MM_MMAP_BASE, "small unhinted allocation keeps legacy base");
    uint64_t before = pmm_free_frames();
    uint64_t big = vma_reserve(sp, 0, 1ull << 40, VMA_READ | VMA_WRITE);
    mm_eqi(big, MM_USER_WIDE_BASE, "TiB allocation selects wide window");
    mm_eqi(pmm_free_frames(), before, "TiB reservation allocates no physical pages");
    uint64_t sparse_frame = pmm_alloc();
    vmm_map_page_in(sp, big + (1ull << 40) - 4096, sparse_frame,
                    VMM_USER | VMM_WRITABLE | VMM_PTE_ANON);
    uint64_t hinted = vma_reserve(sp, 1ull << 44, 4096, VMA_READ);
    mm_eqi(hinted, 1ull << 44, "explicit 16 TiB hint is honored");
    uint64_t clash = vma_reserve(sp, hinted, 4096, VMA_READ);
    mm_ok(clash >= MM_USER_WIDE_BASE && clash != hinted, "occupied wide hint stays in wide window");
    mm_eqi(vma_protect(sp, big, 1ull << 40, VMA_READ), 0, "protect sparse TiB VMA");
    mm_eqi(vmm_protect_range_in(sp, big, 1ull << 40, VMA_READ), 1, "protect sparse TiB reaches final resident page");
    mm_eqi(vmm_unmap_range_in(sp, big, 1ull << 40), 1, "unmap sparse TiB finds the final resident page");
    mm_eqi(vma_release(sp, big, 1ull << 40), 0, "release sparse TiB VMA");
    mm_ok(vma_reserve_fixed(sp, MM_USER_END, 4096, VMA_READ) < 0, "fixed gap mapping denied");
    vmm_free_space(sp);
    mm_eqi(pmm_free_frames(), free0, "reservation space releases all tables");
}

static void mappings(uint64_t kernel)
{
    const uint64_t va[] = { MM_USER_BASE, MM_USER_WIDE_BASE,
        MM_USER_WIDE_BASE + (1ull << 30) + (1ull << 21),
        (3ull << 39) + 4096, (1ull << 44) + 4096, MM_USER_WIDE_END - 4096 };
    uint64_t free0 = pmm_free_frames();
    uint64_t sp = vmm_new_space(), child = vmm_new_space();
    for (unsigned i = 0; i < sizeof va / sizeof va[0]; i++) {
        uint64_t f = pmm_alloc();
        memset(mm_sim_ptr(f), (int)i + 33, 4096);
        mm_eqi(vma_reserve_fixed(sp, va[i], 4096, VMA_READ | VMA_WRITE), 0, "reserve across page table levels");
        vmm_map_page_in(sp, va[i], f, VMM_USER | VMM_WRITABLE | VMM_PTE_ANON | MM_PTE_NX);
        mm_ok(vmm_user_range_ok(sp, (void *)(uintptr_t)va[i], 4096, 1), "mapped VA passes user walk");
        struct rmap_iter it; uint64_t rc = 0, rv = 0;
        rmap_begin(&it, f);
        mm_ok(rmap_next(&it, &rc, &rv) && rc == sp && rv == va[i], "rmap stores full 64-bit absolute VPN");
    }
    /* Protect one resident high page before fork: its bytes must survive even
     * though neither present nor USER is set on the stored guard PTE. */
    uint64_t guard = va[2];
    mm_eqi(vma_protect(sp, guard, 4096, 0), 0, "wide guard VMA");
    mm_eqi(vmm_protect_range_in(sp, guard, 4096, 0), 1, "wide guard resident PTE");
    mm_ok(!vmm_user_range_ok(sp, (void *)(uintptr_t)guard, 1, 0), "guard rejects read");
    uint64_t sv = va[3] + 4096, slot = swap_alloc_slot();
    mm_ok(slot != SWAP_NOSLOT, "swap slot allocated");
    vmm_map_raw_in(sp, sv, ((slot << VMM_SWAP_SHIFT) | VMM_PTE_SWAP | VMM_SWAP_ANON | MM_PTE_NX));
    mm_set_cow(1);
    mm_eqi(vmm_clone_user(child, sp), 0, "clone user trees");
    for (unsigned i = 0; i < sizeof va / sizeof va[0]; i++) {
        uint64_t pe = entry(sp, va[i]), ce = entry(child, va[i]);
        mm_ok(ce != 0 && ce == pe, "wide fork preserves leaf %u and all flags", i);
        if (!ce) continue; /* the negative control must fail assertions, not crash */
        mm_eqi(pmm_refcount(ce & MM_PTE_ADDR), 2, "fork increments high frame references");
        mm_eqi(*(unsigned char *)mm_sim_ptr(ce & MM_PTE_ADDR), i + 33, "fork preserves exact high page bytes");
    }
    mm_ok(vmm_pte_is_swap(entry(child, sv)), "wide fork preserves swap leaf");
    mm_eqi(swap_slot_refs(slot), 2, "wide swap slot gets child reference");
    mm_eqi(rmap_audit(), 0, "reverse map agrees after wide fork");
    /* Resolve a child write through the real fault path, including full VA
     * lookup; legacy and high siblings must stay unchanged. */
    uint64_t cv = va[4];
    if (entry(child, cv)) {
        mm_eqi(vmm_user_range_fault_in(child, (void *)(uintptr_t)cv, 1, 1), 1, "wide usercopy resolves COW");
        uint64_t ce = entry(child, cv), pe = entry(sp, cv);
        mm_ok((ce & MM_PTE_ADDR) != (pe & MM_PTE_ADDR), "wide child gets private COW frame");
        *(unsigned char *)mm_sim_ptr(ce & MM_PTE_ADDR) = 99;
        mm_eqi(*(unsigned char *)mm_sim_ptr(pe & MM_PTE_ADDR), 37, "wide parent data remains unchanged");
    }
    mm_host_cr3 = kernel;
    vmm_free_space(child);
    mm_eqi(swap_slot_refs(slot), 1, "child teardown releases wide swap reference");
    mm_eqi(vma_protect(sp, guard, 4096, VMA_READ | VMA_WRITE), 0, "restore wide guard VMA");
    mm_eqi(vmm_protect_range_in(sp, guard, 4096, VMA_READ | VMA_WRITE), 1, "restore wide guard PTE");
    mm_eqi(*(unsigned char *)mm_sim_ptr(entry(sp, guard) & MM_PTE_ADDR), 35, "guard retains bytes");
    vmm_free_user(sp);
    mm_ok(!entry(sp, va[1]) && !entry(sp, va[5]), "exec-style teardown clears all wide PML4 trees");
    mm_eqi(swap_slot_refs(slot), 0, "teardown releases final wide swap reference");
    mm_ok(vma_reserve(sp, va[4], 4096, VMA_READ) == va[4], "cleared address space remains reusable");
    vmm_free_space(sp);
    mm_eqi(mm_cow_pages(), 0, "all COW counts released");
    mm_eqi(rmap_nodes_used(), 0, "all reverse mappings released");
    mm_eqi(pmm_free_frames(), free0, "all wide data and table frames reclaimed exactly");
}

static void eager_clone(void)
{
    uint64_t before = pmm_free_frames();
    uint64_t sp = vmm_new_space(), child = vmm_new_space();
    uint64_t va = (1ull << 44) + 8192, frame = pmm_alloc_any();
    memset(mm_sim_ptr(frame), 0x5a, 4096);
    vmm_map_page_in(sp, va, frame, VMM_USER | VMM_WRITABLE | MM_PTE_NX);
    mm_set_cow(0);
    mm_eqi(vmm_clone_user(child, sp), 0, "eager fork with no legacy mappings");
    uint64_t ce = entry(child, va);
    mm_ok(ce != 0 && (ce & MM_PTE_ADDR) != frame, "wide fork preserves eager high page by copying");
    if (ce) {
        mm_ok((ce & (VMM_WRITABLE | MM_PTE_NX)) == (VMM_WRITABLE | MM_PTE_NX), "eager copy preserves permissions");
        mm_ok(memcmp(mm_sim_ptr(frame), mm_sim_ptr(ce & MM_PTE_ADDR), 4096) == 0, "eager high page has exact original bytes");
    }
    vmm_free_space(child);
    vmm_free_space(sp);
    mm_set_cow(1);
    mm_eqi(pmm_free_frames(), before, "eager fork frees all high tables and data");
}

static void kernel_isolation(uint64_t kernel)
{
    const uint64_t low = 0xc0000000ull, high = 0xffff800000000000ull;
    uint64_t lf = pmm_alloc(), hf = pmm_alloc();
    vmm_map_page_in(kernel, low, lf, VMM_WRITABLE);
    vmm_map_page_in(kernel, high, hf, VMM_WRITABLE);
    uint64_t *kp = mm_sim_ptr(kernel);
    mm_ok(!(kp[256] & VMM_USER), "physmap PML4 remains supervisor");
    uint64_t sp = vmm_new_space();
    mm_eqi(entry(sp, low), entry(kernel, low), "legacy kernel mapping is shared");
    mm_eqi(entry(sp, high), entry(kernel, high), "high kernel mapping is shared");
    mm_ok(!vmm_user_range_ok(sp, (void *)(uintptr_t)low, 1, 0), "user cannot address shared low kernel region");
    mm_ok(!vmm_user_range_ok(sp, (void *)(uintptr_t)high, 1, 0), "user cannot address physmap");
    vmm_map_page_in(sp, high, lf, VMM_USER | VMM_WRITABLE);
    mm_eqi(entry(sp, high) & MM_PTE_ADDR, hf, "user map cannot replace shared physmap");
    vmm_map_raw_in(sp, high, lf | VMM_USER | 1);
    mm_eqi(entry(sp, high) & MM_PTE_ADDR, hf, "raw map cannot replace shared physmap");
    mm_eqi(vmm_protect_range_in(sp, high, 4096, VMA_READ | VMA_WRITE), 0, "mprotect rejects physmap");
    vmm_free_space(sp);
    mm_eqi(pmm_refcount(lf), 1, "shared low kernel frame survives user teardown");
    mm_eqi(pmm_refcount(hf), 1, "shared physmap frame survives user teardown");
}

int main(void)
{
    mm_sim_init(64);
    uint64_t kernel = mm_sim_kernel_space();
    vmm_kernel_cr3();
    mm_sim_swap(4);
    mm_ok(swap_init() == 0, "swap initialized");
    range_contract();
    reservations();
    mappings(kernel);
    eager_clone();
    kernel_isolation(kernel);
    mm_eqi(pmm_audit(), 0, "final PMM audit");
    mm_sim_swap_free();
    mm_sim_done();
    return mm_summary("wideva_test");
}
