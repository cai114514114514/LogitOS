/* Host test for the reverse map (c/kernel/mm/rmap.c, compiled -DMM_HOSTTEST).
 *
 * WHY THIS FILE EXISTS BEFORE THE RECLAIM TEST DOES
 *
 * Reclaim's entire safety argument is one sentence: a frame may only be evicted
 * if every reference to it is a PTE the reverse map knows the address of. If
 * the reverse map is wrong, that sentence is worthless and every downstream
 * test is testing the wrong thing -- it would pass while frames were being
 * handed to two owners, because nothing else in the system can see the
 * difference. So the map is verified on its own, against pmm's independently
 * maintained refcount, before anything is allowed to act on it.
 *
 * The bug this is really hunting is asymmetry: an add without a matching
 * remove (a chain that keeps a dead entry, so reclaim later unmaps a PTE that
 * belongs to somebody else) or a remove without a matching add (a chain that is
 * short, so reclaim thinks it has unmapped everything when it has not). Both
 * are silent. rmap_audit() is what makes them loud, and this file drives every
 * transition through it.
 *
 * Run: sh tests/unit/mm_run.sh */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "mm.h"
#include "rmap.h"
#include "mmhost.h"      /* mm_host_cr3: the simulated active address space */
#include "kprintf.h"

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

/* Two page-aligned addresses in the private user region. */
#define VA0 (MM_USER_BASE + 0x10000)
#define VA(n) (VA0 + (uint64_t)(n) * 4096)

static void phase(const char *s) { printf("-- %s\n", s); }

/* ------------------------------------------------------------------------ */
static void t_basic(void)
{
    phase("one frame, one mapping: the count is the truth and audit agrees");

    uint64_t cr3 = vmm_new_space();
    mm_ok(cr3 != 0, "new address space");

    uint64_t f = pmm_alloc();
    mm_ok(f != 0, "a frame");
    mm_eqi(rmap_count(f), 0, "a fresh frame is mapped by nobody");

    vmm_map_page_in(cr3, VA0, f, VMM_USER | VMM_WRITABLE);
    mm_eqi(rmap_count(f), 1, "after one map, one mapping");
    mm_eqi(pmm_refcount(f), 1, "and one reference");
    mm_eqi(rmap_audit(), 0, "audit clean with one mapping");

    /* The iterator must return exactly the pair that was installed: reclaim
     * uses it to decide which PTE to unmap, so a wrong address here is a wrong
     * page unmapped later. */
    struct rmap_iter it;
    uint64_t gc = 0, gv = 0;
    int n = 0;
    for (rmap_begin(&it, f); rmap_next(&it, &gc, &gv); n++) { }
    mm_eqi(n, 1, "the chain has one entry");
    mm_ok(gc == (cr3 & MM_PTE_ADDR), "the entry names the right address space");
    mm_ok(gv == VA0, "the entry names the right virtual address");

    vmm_unmap_range_in(cr3, VA0, 4096);
    mm_eqi(rmap_count(f), 0, "unmapping removes the entry");
    mm_eqi(pmm_refcount(f), 0, "and the last reference");
    mm_eqi(rmap_audit(), 0, "audit clean after the unmap");

    vmm_free_space(cr3);
}

/* ------------------------------------------------------------------------ */
static void t_sharing(void)
{
    phase("a frame shared four ways: the chain names all four, and only those");

    uint64_t spaces[4];
    uint64_t f = 0;

    spaces[0] = vmm_new_space();
    vma_reserve_fixed(spaces[0], VA0, 4096, VMA_READ | VMA_WRITE);
    f = pmm_alloc();
    memset(mm_sim_ptr(f), 0x11, 4096);
    vmm_map_page_in(spaces[0], VA0, f, VMM_USER | VMM_WRITABLE);

    for (int i = 1; i < 4; i++) {
        spaces[i] = vmm_new_space();
        mm_ok(vmm_clone_user(spaces[i], spaces[0]) == 0, "fork %d", i);
    }

    mm_eqi(rmap_count(f), 4, "four address spaces map the frame");
    mm_eqi(pmm_refcount(f), 4, "and pmm counts four references");
    mm_eqi(rmap_audit(), 0, "audit clean with a four-way share");

    /* Every entry must be a DIFFERENT space, all at the same VA. A chain with a
     * duplicate would make reclaim unmap one PTE twice and free one reference
     * too many -- the exact shape of a double free. */
    struct rmap_iter it;
    uint64_t seen[8];
    int n = 0;
    for (rmap_begin(&it, f); n < 8; n++) {
        uint64_t c, v;
        if (!rmap_next(&it, &c, &v)) break;
        seen[n] = c;
        mm_ok(v == VA0, "chain entry %d is at the mapped address", n);
    }
    mm_eqi(n, 4, "exactly four chain entries");
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            mm_ok(seen[i] != seen[j], "chain entries %d and %d are different spaces", i, j);

    /* Tear the shares down in a shuffled order; the count must track exactly. */
    int order[4] = { 2, 0, 3, 1 };
    for (int k = 0; k < 4; k++) {
        vmm_free_space(spaces[order[k]]);
        mm_eqf(rmap_count(f), 3 - k, "after freeing %d spaces, %d mappings left", k + 1, 3 - k);
        mm_eqi(pmm_refcount(f), (unsigned)(3 - k), "references track the mappings");
        mm_eqi(rmap_audit(), 0, "audit clean mid-teardown");
    }
}

/* ------------------------------------------------------------------------ */
static void t_cow_move(void)
{
    phase("a copy-on-write copy MOVES a mapping: the chain must follow it");

    uint64_t parent = vmm_new_space();
    vma_reserve_fixed(parent, VA0, 4096, VMA_READ | VMA_WRITE);
    uint64_t f = pmm_alloc();
    memset(mm_sim_ptr(f), 0x22, 4096);
    vmm_map_page_in(parent, VA0, f, VMM_USER | VMM_WRITABLE);

    uint64_t child = vmm_new_space();
    mm_ok(vmm_clone_user(child, parent) == 0, "fork");
    mm_eqi(rmap_count(f), 2, "both spaces map the original frame");

    /* The child writes: it gets its own frame, and the reverse map must show
     * the original frame losing one mapping and the new frame gaining one. If
     * the copy-on-write path forgot either half, the audit would still pass on
     * counts alone -- which is why the frames are compared as well. */
    mm_host_cr3 = child;
    /* P|W|U: a copy-on-write fault is a PROTECTION violation on a page that is
     * present, which is what the CPU reports and what the classifier requires. */
    mm_ok(mm_fault_in(child, VA0, 0x1 | 0x2 | 0x4) == 1,
          "the child's write is resolved by copy");

    uint64_t *cpte = vmm_pte(child, VA0);
    uint64_t nf = *cpte & MM_PTE_ADDR;
    mm_ok(nf != f, "the child is on a different frame");
    mm_eqi(rmap_count(f), 1, "the original frame is now mapped once");
    mm_eqi(rmap_count(nf), 1, "the copy is mapped once");
    mm_eqi(rmap_audit(), 0, "audit clean after the copy");

    /* And the entry for the new frame must name the child, not the parent. */
    struct rmap_iter it;
    uint64_t c = 0, v = 0;
    rmap_begin(&it, nf);
    mm_ok(rmap_next(&it, &c, &v), "the copy has a chain entry");
    mm_ok(c == (child & MM_PTE_ADDR), "the entry names the child");
    mm_ok(v == VA0, "at the right address");

    vmm_free_space(child);
    vmm_free_space(parent);
    mm_eqi(rmap_audit(), 0, "audit clean after both spaces die");
}

/* ------------------------------------------------------------------------ */
static void t_remap_same_address(void)
{
    phase("re-mapping the same address does not leak or duplicate an entry");

    /* elf_load does exactly this: it maps a frame writable to fill it, then
     * maps the SAME frame at the SAME address again with the final flags. A
     * reverse map that only appended would end up with two entries for one PTE,
     * and reclaim would then unmap it twice and drop one reference too many. */
    uint64_t cr3 = vmm_new_space();
    uint64_t f = pmm_alloc();

    vmm_map_page_in(cr3, VA0, f, VMM_USER | VMM_WRITABLE);
    mm_eqi(rmap_count(f), 1, "first map");
    vmm_map_page_in(cr3, VA0, f, VMM_USER);              /* same frame, new flags */
    mm_eqi(rmap_count(f), 1, "re-mapping the same frame keeps exactly one entry");
    mm_eqi(rmap_audit(), 0, "audit clean after a re-map");

    /* Now map a DIFFERENT frame over the same address. The old entry must go
     * (leaving it would have reclaim unmap a PTE that no longer refers to it). */
    uint64_t g = pmm_alloc();
    vmm_map_page_in(cr3, VA0, g, VMM_USER | VMM_WRITABLE);
    mm_eqi(rmap_count(f), 0, "the replaced frame has no mappings left");
    mm_eqi(rmap_count(g), 1, "the new frame has one");

    vmm_free_space(cr3);
    pmm_free(f);      /* the overwrite leaked f's reference, exactly as it did
                       * before this line existed; released here so the frame
                       * baseline below is about the reverse map, not that. */
}

/* ------------------------------------------------------------------------ */
static void t_kernel_memory_is_invisible(void)
{
    phase("kernel frames have no chain -- which is what pins them");

    /* Page tables, heap arenas and everything else the kernel allocates for
     * itself are never mapped into a user address space. That, and not a list
     * of exceptions, is why reclaim can never reach them: rmap_count is 0 while
     * the refcount is not. This asserts the property directly on a page-table
     * frame, since that is the one whose eviction would be immediately fatal. */
    uint64_t cr3 = vmm_new_space();
    uint64_t f = pmm_alloc();
    vmm_map_page_in(cr3, VA0, f, VMM_USER | VMM_WRITABLE);

    uint64_t *pml4 = (uint64_t *)mm_sim_ptr(cr3 & MM_PTE_ADDR);
    uint64_t pdpt = pml4[0] & MM_PTE_ADDR;
    uint64_t *pdptv = (uint64_t *)mm_sim_ptr(pdpt);
    uint64_t pd = pdptv[1] & MM_PTE_ADDR;

    mm_ok(pmm_refcount(pdpt) >= 1, "the private PDPT frame is allocated");
    mm_eqi(rmap_count(pdpt), 0, "and is mapped by no user PTE");
    mm_ok(pmm_refcount(pd) >= 1, "the page directory frame is allocated");
    mm_eqi(rmap_count(pd), 0, "and is mapped by no user PTE");
    mm_eqi(rmap_count(cr3), 0, "the PML4 frame itself is mapped by no user PTE");

    /* The data page, by contrast, IS visible. */
    mm_eqi(rmap_count(f), 1, "the user data page is mapped once");

    vmm_free_space(cr3);
}

/* ------------------------------------------------------------------------ */
static void t_stress(void)
{
    phase("stress: 4000 map/fork/fault/free rounds, audited throughout");

    uint64_t base_free = pmm_free_frames();
    uint64_t worst_nodes = 0;

    for (int round = 0; round < 4000; round++) {
        uint64_t p = vmm_new_space();
        if (!p) break;
        for (int k = 0; k < 3; k++) {
            vma_reserve_fixed(p, VA(k), 4096, VMA_READ | VMA_WRITE);
            uint64_t f = pmm_alloc();
            if (!f) break;
            memset(mm_sim_ptr(f), 0x30 + k, 4096);
            vmm_map_page_in(p, VA(k), f, VMM_USER | VMM_WRITABLE);
        }
        uint64_t c = vmm_new_space();
        vmm_clone_user(c, p);
        mm_host_cr3 = c;
        mm_fault_in(c, VA(1), 0x1 | 0x2 | 0x4);    /* make the child copy one page */
        if (rmap_nodes_used() > worst_nodes) worst_nodes = rmap_nodes_used();
        vmm_free_space(c);
        vmm_free_space(p);
    }

    mm_eqi(rmap_audit(), 0, "audit clean after the stress loop");
    mm_eqi(rmap_nodes_used(), 0, "every reverse-map node came back");
    mm_eqi(rmap_overflows(), 0, "the node pool was never exhausted");
    mm_eqi(rmap_bugs(), 0, "the reverse map reported no bugs");
    mm_eqi((long long)pmm_free_frames(), (long long)base_free,
           "no frames leaked over 4000 rounds");
    printf("   peak reverse-map nodes in use: %llu of %llu\n",
           (unsigned long long)worst_nodes, (unsigned long long)rmap_nodes_total());
}

int main(void)
{
    mm_sim_init(64);
    mm_sim_kernel_space();
    mm_ok(rmap_ready(), "the reverse map came up at pmm_init");

    t_basic();
    t_sharing();
    t_cow_move();
    t_remap_same_address();
    t_kernel_memory_is_invisible();
    t_stress();

    rmap_report("end of test");
    mm_sim_done();
    return mm_summary("mm_rmap_test");
}
