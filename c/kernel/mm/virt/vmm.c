#include "mmguard.h"
#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "vma.h"
#include "rmap.h"
#include "swap.h"
#include "mmhost.h"
#include "kprintf.h"
#include "../../../../include/weaksym.h"   /* the weak tlb_flush_all below */

/* WEAK, and not an #include: vmm.c is compiled host-side by make test-mm with
 * no kernel cpu headers on its include path -- the same reason kheap.c reaches
 * its core index through a weak hook. Absent, the call is skipped, which is
 * right: a host test has no other core to shoot down. */
void tlb_flush_all(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(tlb_flush_all);

#include "mmguard.inc"

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

void *memset(void *, int, size_t);     /* lib/string.c */
void *memcpy(void *, const void *, size_t);

static inline void invlpg(uint64_t addr) { mm_invlpg(addr); }

/* Return the next-level table for `idx`, allocating and linking it if absent.
 * Physical frames live in the identity-mapped low region, so a frame's
 * physical address is directly usable as a pointer (mm_p2v). */
static uint64_t *next_table(uint64_t *table, int idx, int user)
{
    if (!(table[idx] & PRESENT)) {
#ifdef MM_KERNEL_MAP_TEST
        void mm_host_table_absent(uint64_t *, int, int);
        mm_host_table_absent(table,idx,user);
#endif
        uint64_t frame = pmm_alloc();
        if (!frame) return NULL;                 /* OOM: frame 0 is reserved; never install it */
        memset(mm_p2v(frame), 0, 4096);
        table[idx] = frame | PRESENT | WRITABLE | (user ? USER : 0);
    } else {
        if (table[idx] & 0x80)                  /* PS: a large-page leaf, not a table --
                                                 * refuse to descend (boot.asm maps 0-1 GiB
                                                 * with 2 MiB pages; treating a PD leaf as a
                                                 * PT pointer would corrupt that page). */
            return NULL;
        /* Previously every walk set USER, even for a kernel mapping. Preserve
         * existing user paths, but never grant USER to a supervisor-only
         * subtree (especially the high physmap) while mapping kernel memory. */
        if (user) table[idx] |= USER;
    }
    return (uint64_t *)mm_p2v(table[idx] & MM_PTE_ADDR);
}

/* THE ONE PLACE A LEAF PTE CHANGES.
 *
 * Everything that maps or unmaps a page in this kernel funnels through here, and
 * that is what makes the reverse map (rmap.h) able to be right. A reverse map is
 * only as good as its worst-maintained update site, so there is exactly one
 * update site: if a PTE changes anywhere else, rmap_audit() catches it, and if
 * it changes here, the bookkeeping is unavoidable.
 *
 * Three transitions are handled, in this order, because the old entry must be
 * accounted for before it is overwritten:
 *
 *   old was a present user page  -> forget the mapping (the frame's REFERENCE is
 *                                   the caller's business, exactly as before --
 *                                   vmm_map_page over a live mapping leaked one
 *                                   before this change and still does; what it
 *                                   no longer does is leave a stale reverse-map
 *                                   entry, which would be a real eviction bug
 *                                   rather than a leak).
 *   old was a swap entry         -> drop the slot's reference. Without this a
 *                                   process that unmaps swapped-out memory
 *                                   leaks a swap slot per page, which is a leak
 *                                   nothing on the machine could see.
 *   new is a present user page   -> record the mapping.
 *
 * `entry` is the COMPLETE new PTE value, not a frame plus flags, so a swap
 * entry (present bit clear) can be installed through the same path as a normal
 * mapping. */
static void set_leaf(uint64_t cr3, uint64_t *pt, uint64_t virt, uint64_t entry)
{
    uint64_t *slotp = &pt[(virt >> 12) & 0x1FF];
    uint64_t old = *slotp;

    if ((old & (PRESENT | USER)) == (PRESENT | USER))
        rmap_remove(old & MM_PTE_ADDR, cr3, virt);
    else if (vmm_pte_is_swap(old))
        swap_slot_put(vmm_pte_swap_slot(old));

    *slotp = entry;

    if ((entry & (PRESENT | USER)) == (PRESENT | USER))
        rmap_add(entry & MM_PTE_ADDR, cr3, virt);
}

/* The kernel PML4 is canonical. Each process has a private low PDPT to
 * isolate PDPT[1], so adding a shared root after process creation also needs
 * publication into those copies. Registry publication/retirement and physical
 * root release use this same owner; no AS lock is acquired in this walk. */
static void publish_kernel_root(uint64_t virt, uint64_t *kpml4)
{
    unsigned l=(unsigned)((virt>>39)&511),q=(unsigned)((virt>>30)&511);
    uint64_t entry;
    if (l==0) {
        uint64_t *kpdpt=mm_p2v(kpml4[0]&MM_PTE_ADDR);
        entry=kpdpt[q]&~(uint64_t)USER;
        __atomic_store_n(&kpdpt[q],entry,__ATOMIC_RELEASE);
    } else {
        entry=kpml4[l]&~(uint64_t)USER;
        __atomic_store_n(&kpml4[l],entry,__ATOMIC_RELEASE);
    }
    for (unsigned i=0;i<256;i++) {
        uint64_t cr3=__atomic_load_n(&mm_live_spaces[i],__ATOMIC_ACQUIRE);
        if (!cr3) continue;
        uint64_t *root=mm_p2v(cr3&MM_PTE_ADDR);
        if (l==0) {
            uint64_t *pdpt=mm_p2v(root[0]&MM_PTE_ADDR);
            __atomic_store_n(&pdpt[q],entry,__ATOMIC_RELEASE);
        } else __atomic_store_n(&root[l],entry,__ATOMIC_RELEASE);
    }
}

/* Caller owns the selected AS or shared-kernel guard. Returns whether a leaf
 * changed, so range callers pay one shootdown after all publications.
 * Correction: a newly allocated shared root also counts as a change, even if
 * a later table allocation fails. Publish valid, zero-filled partial trees on
 * that failure path: otherwise a retry sees an existing canonical root and
 * never repairs the missing roots in processes which predate the allocation. */
static int map_page_locked(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags, int user)
{
    uint64_t *pml4=mm_p2v(cr3&MM_PTE_ADDR);
    unsigned l=(unsigned)((virt>>39)&511),q=(unsigned)((virt>>30)&511);
    uint64_t old_root=pml4[l];
    int changed=0;
    if (!user && l==0 && (old_root&PRESENT))
        old_root=((uint64_t *)mm_p2v(old_root&MM_PTE_ADDR))[q];
    uint64_t *pdpt=next_table(pml4,(int)l,user); if (!pdpt) goto out;
    uint64_t *pd=next_table(pdpt,(int)q,user); if (!pd) goto out;
    uint64_t *pt=next_table(pd,(int)((virt>>21)&511),user); if (!pt) goto out;
    uint64_t entry=(phys&MM_PTE_ADDR)|flags|PRESENT;
    uint64_t old=pt[(virt>>12)&511];
    if (old!=entry) { set_leaf(cr3,pt,virt,entry); changed=1; }
out:
#ifdef MM_NO_PARTIAL_KERNEL_PUBLISH
    if (!user && !changed) return 0; /* negative control: omit OOM publication */
#endif
    if (!user) {
        uint64_t new_root=pml4[l];
        if (l==0 && (new_root&PRESENT))
            new_root=((uint64_t *)mm_p2v(new_root&MM_PTE_ADDR))[q];
        if ((new_root&PRESENT) && old_root!=new_root) {
            publish_kernel_root(virt,pml4);
            changed=1;
        }
    }
    return changed;
}
static int kernel_map_address(uint64_t virt)
{
    /* Only canonical, supervisor regions. In particular never overwrite the
     * private legacy PDPT[1] or any wide user PML4 root during publication. */
    uint64_t top=virt>>47;
    return (top==0 || top==0x1ffff) && !mm_user_addr(virt);
}
static void kernel_map_finish(int changed)
{
    /* A new root or permission replacement must be visible before a caller
     * uses its MMIO alias. The sender retains the kernel owner until every
     * CPU confirms, and tlb_flush_all fail-stops on a missing ACK. */
    if (changed && LOGIT_HAVE(tlb_flush_all)) tlb_flush_all();
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (flags&USER) {
        if (!mm_user_addr(virt)) return;
        uint64_t cr3=mm_read_cr3(); MM_GUARD(cr3);
        map_page_locked(cr3,virt,phys,flags,1);
        invlpg(virt);
    } else {
        if (!kernel_map_address(virt)) return;
        MM_KERNEL_GUARD;
        int changed=map_page_locked(vmm_kernel_cr3(),virt,phys,flags,0);
        invlpg(virt);
        kernel_map_finish(changed);
    }
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)
{
    if (size==0 || size>UINT64_MAX-0xFFF || virt>UINT64_MAX-size-0xFFF) return;
    uint64_t end=(virt+size+0xFFF)&~(uint64_t)0xFFF;
    virt&=~(uint64_t)0xFFF;phys&=~(uint64_t)0xFFF;
    if (flags&USER) {
        uint64_t cr3=mm_read_cr3(); MM_GUARD(cr3);
        for (;virt<end;virt+=4096,phys+=4096) {
            if (!mm_user_addr(virt)) continue;
            map_page_locked(cr3,virt,phys,flags,1);invlpg(virt);
        }
    } else {
        MM_KERNEL_GUARD; int changed=0;
        uint64_t cr3=vmm_kernel_cr3();
        for (;virt<end;virt+=4096,phys+=4096) {
            if (!kernel_map_address(virt)) continue;
            changed|=map_page_locked(cr3,virt,phys,flags,0);invlpg(virt);
        }
        kernel_map_finish(changed);
    }
}

/* --- per-process address spaces --- */

/* The user region (where every app's image + stack lives, 0x40000000..) sits
 * under PML4[0], PDPT[1]. A per-process space keeps the kernel's PML4[0] but
 * swaps in a *private* PDPT so each app's PDPT[1] sub-tree is isolated, while
 * still sharing the kernel's other PDPT entries (identity low mem, framebuffer). */
#define USER_PML4_IDX 0
#define USER_PDPT_IDX 1
/* Correction: in addition to the legacy subtree above, PML4[2..255]
 * contains private user trees. PML4[1] stays kernel-reserved; indices >=256
 * retain the kernel's supervisor mappings, including its physical map. */
#define WIDE_PML4_FIRST ((int)(MM_USER_WIDE_BASE >> 39))
#define WIDE_PML4_END   ((int)(MM_USER_WIDE_END >> 39))

static uint64_t g_kernel_cr3;
/* Statistics, not state: every writer (this file's clone, fault.c's resolve,
 * the two unmap paths) runs under the BKL today -- SYS_FORK is not in
 * syscall_is_bkl_free() and faults always take the BKL -- so plain increments
 * are correct. If fork is ever made BKL-free these need the same treatment the
 * PTE edits below would: a per-address-space lock. */
_Atomic uint64_t g_mm_cow_pages;                 /* PTEs currently carrying VMM_PTE_COW (mm.h) */
/* Legacy host API keeps per-host-thread results; kernel callers pass outputs
 * to vmm_clone_user_counted, so sleeping/migration cannot mix two forks. */
#ifdef MM_HOSTTEST
static _Thread_local uint64_t g_clone_shared, g_clone_copied;
#endif

uint64_t mm_cow_pages(void) { return g_mm_cow_pages; }

uint64_t vmm_kernel_cr3(void)
{
    if (!g_kernel_cr3)
        g_kernel_cr3 = mm_read_cr3();
    return g_kernel_cr3 & MM_PTE_ADDR;
}

/* ------------------------------------------------- WHO IS RUNNING WHAT --
 *
 * THE STALE-TLB PROBLEM, AND WHY RECLAIM CANNOT IGNORE IT.
 *
 * Everything that unmapped a page before this line ran either on the address
 * space it was unmapping (invlpg is enough) or on a space whose threads had
 * already left (vmm_free_space says so in as many words). Reclaim is the first
 * thing in this kernel that takes a page away from a process that is RUNNING,
 * on ANOTHER CORE, right now. Core 0 clears the PTE and frees the frame; core 1
 * is in ring 3 with a TLB entry that still translates that address to it; the
 * allocator hands the frame to somebody else; core 1 writes into it. Silent
 * corruption, and the reclaim counters would look perfect.
 *
 * The obvious fix is a shootdown IPI, and it is exactly the thing that must not
 * be done here: c/kernel/cpu/tlb.h states that tlb_flush_all() deadlocks if the
 * caller holds the BKL, because a core spinning for the BKL does so with IF=0
 * and can never acknowledge the IPI. Reclaim runs under the BKL.
 *
 * So instead of flushing other cores, reclaim simply does not touch what they
 * are using. That needs one fact nothing exported: which address space each core
 * currently has loaded. This is the right file to answer it -- after boot, every
 * CR3 load in the kernel goes through vmm_switch() (sched.c's three, exec.c's
 * pair, wm.c's launch), so keeping the answer up to date is four lines here
 * rather than an accessor added to somebody else's scheduler.
 *
 * THE ORDER IS THE WHOLE CORRECTNESS ARGUMENT. A switch has to leave BOTH the
 * space being left and the space being entered protected across the CR3 write,
 * because in between, this core holds TLB entries for the old one and is about
 * to hold them for the new:
 *
 *      prev = cur;  cur = next;      both are now published
 *      write CR3                     the CPU drops every non-global TLB entry
 *      prev = 0                      the old space is genuinely gone from here
 *
 * Publish the new one late and there is a window where another core evicts
 * pages this core is about to cache; drop the old one early and there is a
 * window where it evicts pages this core still has cached. Doing both around
 * the write closes both. x86 stores are ordered, so the compiler barrier is all
 * that is needed to keep the sequence intact. */
#ifndef MM_HOSTTEST
#include "percpu.h"
static uint64_t g_cpu_cur[PERCPU_MAXCPU];
static uint64_t g_cpu_prev[PERCPU_MAXCPU];

void vmm_switch(uint64_t cr3)
{
    int i = this_cpu()->index;
    if (i >= 0 && i < PERCPU_MAXCPU) {
        __atomic_store_n(&g_cpu_prev[i],__atomic_load_n(&g_cpu_cur[i],__ATOMIC_RELAXED),__ATOMIC_RELEASE);
        __atomic_store_n(&g_cpu_cur[i],cr3 & MM_PTE_ADDR,__ATOMIC_RELEASE);
        __asm__ volatile ("" ::: "memory");
        mm_write_cr3(cr3);
        __asm__ volatile ("" ::: "memory");
        __atomic_store_n(&g_cpu_prev[i],0,__ATOMIC_RELEASE);
        return;
    }
    mm_write_cr3(cr3);
}

int vmm_space_busy_elsewhere(uint64_t cr3)
{
    uint64_t want = cr3 & MM_PTE_ADDR;
    if (!want) return 0;
    int me = this_cpu()->index;
    for (int i = 0; i < PERCPU_MAXCPU; i++) {
        if (i == me) continue;          /* our own TLB is handled by invlpg */
        if (__atomic_load_n(&g_cpu_cur[i],__ATOMIC_ACQUIRE) == want || __atomic_load_n(&g_cpu_prev[i],__ATOMIC_ACQUIRE) == want) return 1;
    }
    return 0;
}
#else
/* The host tests are single-threaded: there is no other core to hold a stale
 * translation, so nothing is ever busy elsewhere. */
void vmm_switch(uint64_t cr3) { mm_write_cr3(cr3); }
int  vmm_space_busy_elsewhere(uint64_t cr3) { (void)cr3; return 0; }
#endif

uint64_t vmm_new_space(void)
{
    uint64_t pml4=pmm_alloc(),pdpt=pmm_alloc();
    if (!pml4 || !pdpt) { if (pml4) pmm_free(pml4); if (pdpt) pmm_free(pdpt); return 0; }
    {
        MM_KERNEL_GUARD;
        uint64_t kcr3=vmm_kernel_cr3();
        uint64_t *kpml4=mm_p2v(kcr3);
        uint64_t *kpdpt=mm_p2v(kpml4[USER_PML4_IDX]&MM_PTE_ADDR);
        /* Private roots become visible together, while kernel-root publishers
         * cannot race the copy. Wide user trees never come from the kernel. */
        memcpy(mm_p2v(pml4),kpml4,4096);
        for (int l=WIDE_PML4_FIRST;l<WIDE_PML4_END;l++)
            ((uint64_t *)mm_p2v(pml4))[l]=0;
        memcpy(mm_p2v(pdpt),kpdpt,4096);
        ((uint64_t *)mm_p2v(pdpt))[USER_PDPT_IDX]=0;
        ((uint64_t *)mm_p2v(pml4))[USER_PML4_IDX]=pdpt|PRESENT|WRITABLE|USER;
        if (!mm_space_publish(pml4)) { pmm_free(pdpt);pmm_free(pml4);return 0; }
    }
    /* No kernel -> blocking-AS edge: this takes the new space's AS guard. */
    vma_space_new(pml4);
    return pml4;
}

/* Explicit-space user mappings own that AS. Supervisor mappings still belong
 * to the canonical shared tree, even if a driver runs in a process context. */
void vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (flags&USER) {
        if (!mm_user_addr(virt)) return;
        MM_GUARD(cr3);map_page_locked(cr3,virt,phys,flags,1);
    } else {
        if (!kernel_map_address(virt)) return;
        MM_KERNEL_GUARD;
        int changed=map_page_locked(vmm_kernel_cr3(),virt,phys,flags,0);
        kernel_map_finish(changed);
    }
}

/* Install a COMPLETE, not-present PTE (a swap entry) in `cr3`. The clone path
 * needs it: a forked child inherits its parent's swapped-out pages as swap
 * entries, and those have no frame to hand to vmm_map_page_in. */
void vmm_map_raw_in(uint64_t cr3, uint64_t virt, uint64_t entry)
{
    MM_GUARD(cr3);
    if (!mm_user_addr(virt)) return;
    uint64_t *pml4 = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    uint64_t *pdpt = next_table(pml4, (virt >> 39) & 0x1FF, 1);   if (!pdpt) return;
    uint64_t *pd   = next_table(pdpt, (virt >> 30) & 0x1FF, 1);   if (!pd)   return;
    uint64_t *pt   = next_table(pd,   (virt >> 21) & 0x1FF, 1);   if (!pt)   return;

    set_leaf(cr3, pt, virt, entry);
}

/* Walk to the leaf PTE without allocating anything. NULL if any level is
 * absent, or if a level is a 2 MiB/1 GiB leaf (boot.asm's identity map). */
uint64_t *vmm_pte(uint64_t cr3, uint64_t virt)
{
    uint64_t *t = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    int shift[3] = { 39, 30, 21 };
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = t[(virt >> shift[lvl]) & 0x1FF];
        if (!(e & PRESENT)) return NULL;
        if (e & 0x80) return NULL;              /* large page: not a table */
        t = (uint64_t *)mm_p2v(e & MM_PTE_ADDR);
    }
    return &t[(virt >> 12) & 0x1FF];
}

/* Lookup for sparse range operations. If an intermediate table is absent,
 * skip its entire coverage; the caller's for-loop adds the final 4 KiB step.
 * This keeps munmap/mprotect of a multi-TiB untouched reservation bounded by
 * allocated tables. Nonpresent leaf encodings (swap/PROT_NONE) are NOT skipped. */
static uint64_t *range_pte(uint64_t cr3, uint64_t *addr)
{
    uint64_t *t = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    static const int shift[] = {39, 30, 21};
    for (int level = 0; level < 3; level++) {
        uint64_t e = t[(*addr >> shift[level]) & 511];
        if (!(e & PRESENT) || (e & 0x80)) {
            uint64_t span = 1ull << shift[level];
            *addr = (*addr & ~(span - 1)) + span - 4096;
            return NULL;
        }
        t = (uint64_t *)mm_p2v(e & MM_PTE_ADDR);
    }
    return &t[(*addr >> 12) & 511];
}

/* ---------------------------------------------------------------- fork --
 *
 * COPY-ON-WRITE. Both spaces are pointed at the same frames, mapped read-only
 * with VMM_PTE_COW set in BOTH; the frame's refcount goes up by one. Nothing
 * is copied until somebody writes (fault.c).
 *
 * THE REFCOUNT ARGUMENT -- the invariant this whole design rests on:
 *
 *     refcount(f) == the number of leaf PTEs, across all address spaces, that
 *                    point at f.
 *
 * It holds because there are exactly five places a user PTE is created or
 * destroyed, and each moves the count by exactly one in the same direction:
 *
 *   create   pmm_alloc()          -> count 1, and the caller installs exactly
 *                                    one PTE (elf_load, setup_cli_stack,
 *                                    wm_launch, the anon fault, the COW copy).
 *   create   this function        -> pmm_ref() +1 per PTE it installs in dst.
 *   destroy  vmm_free_user()      -> pmm_free() once per present user PTE.
 *   destroy  vmm_unmap_range_in() -> pmm_free() once per PTE it clears.
 *   replace  the COW fault        -> installs a NEW frame in this space's PTE
 *                                    (count 1) and pmm_free()s the old one,
 *                                    which is this space's single reference.
 *
 * Two things make that argument airtight rather than merely plausible:
 *   (a) within ONE address space a frame is never mapped at two addresses --
 *       every caller above maps a freshly allocated frame at one VA -- so
 *       "one PTE per space" and "one reference per space" are the same
 *       statement, and freeing a space by walking its PTEs cannot double-count.
 *   (b) pmm_free() is a DECREMENT, not a free: the frame returns to the pool
 *       only at zero, and a decrement below zero is refused and reported
 *       (pmm.c mm_bug) rather than silently freeing a live frame.
 * The host test drives every one of those transitions and re-derives the whole
 * table afterwards (tests/unit/mm_vmm_test.c).
 *
 * FAILURE PATH. On OOM the caller frees dst, which drops every reference this
 * function took. The src PTEs already made read-only stay that way: that is
 * SAFE, not a leak -- the parent's next write takes a COW fault, finds itself
 * the sole owner (count 1) and simply gets WRITABLE back without copying.
 *
 * TLB. src is the running process's own space and a process has exactly one
 * thread, so no other core can have these translations cached. We invalidate
 * on this core as we go, and reload CR3 at the end if src is active. */
int vmm_clone_user_counted(uint64_t dst_cr3, uint64_t src_cr3,
                           uint64_t *shared, uint64_t *copied)
{
    MM_PAIR(dst_cr3, src_cr3);
    uint64_t clone_shared = 0, clone_copied = 0;
    if (shared) *shared=0;
    if (copied) *copied=0;

    uint64_t *spml4 = (uint64_t *)mm_p2v(src_cr3 & MM_PTE_ADDR);
    int cow = mm_cow_enabled();
    int active = ((mm_read_cr3() & MM_PTE_ADDR) == (src_cr3 & MM_PTE_ADDR));
    if (vma_space_clone(dst_cr3, src_cr3) != 0) return -1;

    /* Walk allocated trees, not virtual pages: a 1 TiB reservation may contain
     * only two resident pages. The old PDPT[1]-only traversal silently lost
     * everything above 1 TiB on fork. The control reinstates exactly that loss. */
    for (int l = 0; l < WIDE_PML4_END; l++) {
        if (l != USER_PML4_IDX && l < WIDE_PML4_FIRST) continue;
#ifdef WIDEVA_SKIP_CLONE
        if (l >= WIDE_PML4_FIRST) continue;
#endif
        if (!(spml4[l] & PRESENT)) continue;
        uint64_t *spdpt = (uint64_t *)mm_p2v(spml4[l] & MM_PTE_ADDR);
        int first = l == USER_PML4_IDX ? USER_PDPT_IDX : 0;
        int limit = l == USER_PML4_IDX ? USER_PDPT_IDX + 1 : 512;
        for (int q = first; q < limit; q++) {
            uint64_t pde = spdpt[q];
            if (!(pde & PRESENT)) continue;
            if (pde & 0x80) goto clone_failed; /* user huge pages are not supported */
            uint64_t *spd = (uint64_t *)mm_p2v(pde & MM_PTE_ADDR);
            for (int i = 0; i < 512; i++) {
                if (!(spd[i] & PRESENT)) continue;
                if (spd[i] & 0x80) goto clone_failed;
                uint64_t *spt = (uint64_t *)mm_p2v(spd[i] & MM_PTE_ADDR);
                for (int j = 0; j < 512; j++) {
                    uint64_t e = spt[j];
                    uint64_t va = ((uint64_t)l << 39) | ((uint64_t)q << 30) |
                                  ((uint64_t)i << 21) | ((uint64_t)j << 12);

                    if (!vmm_pte_is_swap(e) && !vmm_pte_is_noaccess(e) &&
                        (e & (PRESENT | USER)) != (PRESENT | USER)) continue;
                    uint64_t *dt = (uint64_t *)mm_p2v(dst_cr3 & MM_PTE_ADDR);
                    dt = next_table(dt, l, 1); if (!dt) goto clone_failed;
                    dt = next_table(dt, q, 1); if (!dt) goto clone_failed;
                    dt = next_table(dt, i, 1); if (!dt) goto clone_failed;

                    /* A page the parent has swapped out. The child inherits the SLOT,
                     * not a frame: both PTEs point at the same bytes on the device and
                     * the slot's reference count says so. Whichever side faults first
                     * reads it back into a private frame (see swap.h -- the sharing is
                     * not restored). Missing this case would have the child inherit an
                     * empty address at that page, i.e. silently lose the parent's
                     * memory across a fork, which is precisely the kind of bug that
                     * only appears once swap is under real pressure. */
                    if (vmm_pte_is_swap(e)) {
                        swap_slot_ref(vmm_pte_swap_slot(e));
                        vmm_map_raw_in(dst_cr3, va, e);
                        clone_shared++;
                        continue;
                    }

                    /* A PROT_NONE page. The child inherits the RESERVATION and the
                     * bytes behind it, exactly as it inherits any other page -- the
                     * VMA came across in vma_space_clone above with prot 0, so the
                     * child's guard is a guard too. Installed WHOLE and given a
                     * reference, which is the same two moves the shared case below
                     * makes; what it does NOT get is a COW marker, because a page
                     * neither side may touch cannot be written by either side, and
                     * whichever side later mprotects it back to writable re-derives
                     * the sharing from the refcount then (vmm_protect_range_in).
                     * Skipping this case would silently drop the page from the child
                     * and leak the parent's reference -- the same failure the swap
                     * case above exists to prevent, one encoding along. */
                    if (vmm_pte_is_noaccess(e)) {
                        if (pmm_ref(e & MM_PTE_ADDR) == 0) {
                            vmm_map_raw_in(dst_cr3, va, e);
                            clone_shared++;
                        } else {
                            /* Refcount saturated: pmm_ref's contract is that the frame
                             * must then be COPIED, never shared. Same fallback the
                             * ordinary path takes below, with the entry rebuilt around
                             * the new frame rather than installed whole. */
                            uint64_t nf = pmm_alloc_any();
                            if (!nf) goto clone_failed;
                            memcpy(mm_p2v(nf), mm_p2v(e & MM_PTE_ADDR), 4096);
                            vmm_map_raw_in(dst_cr3, va, nf | (e & MM_PTE_FLAGS));
                            clone_copied++;
                        }
                        continue;
                    }

                    if ((e & (PRESENT | USER)) != (PRESENT | USER)) continue;
                    uint64_t frame = e & MM_PTE_ADDR;

#ifndef SHM_FORK_COPY
                    /* A MAP_SHARED page (c/kernel/mm/shm.h). THE ONE CASE THAT MUST NOT
                     * BECOME COPY-ON-WRITE.
                     *
                     * A shared region survives fork as THE SAME MEMORY. The child gets
                     * the parent's entry verbatim -- still writable, still marked SHM,
                     * NOT marked COW -- plus one pmm reference, which is the same two
                     * moves the copy-on-write branch below makes and the same two the
                     * swap and file branches above make. What it does not do is drop
                     * WRITABLE, and that omission is the whole feature.
                     *
                     * Getting this wrong is the quietest bug in this file. Fall through
                     * to the branch below and everything still WORKS: the child gets the
                     * right bytes, the right permissions, the right protections, and the
                     * refcounts all balance. The first write on either side then
                     * privatises the page and the two processes stop communicating --
                     * no error, no fault, no log line, and nothing in either address
                     * space that looks wrong to an audit. Both halves of a producer/
                     * consumer pair simply talk to themselves.
                     *
                     * -DSHM_FORK_COPY is exactly that: this branch compiled out, so a
                     * shared page takes the ordinary COW path. It is the PLAUSIBLE wrong
                     * implementation rather than the feature removed -- it is what
                     * adding shared memory and not touching fork looks like -- and
                     * tests/unit/mm_shm_test.c is required to fail against it.
                     *
                     * REFCOUNT SATURATION IS A FAILURE HERE, not a reason to copy. Every
                     * other branch in this loop answers a saturated refcount by copying
                     * the frame, which is correct when the sharing is an optimisation
                     * (COW) or a cache (a file page). For a shared segment the sharing
                     * IS the semantics, so a copy would be a silent wrong answer, and
                     * fork must fail instead: the caller frees dst and reports ENOMEM,
                     * which is a fact the program can act on. */
                    if (e & VMM_PTE_SHM) {
                        if (pmm_ref(frame) != 0) goto clone_failed;
                        vmm_map_raw_in(dst_cr3, va, e);
                        clone_shared++;
                        continue;
                    }
#endif

                    if (cow && pmm_ref(frame) == 0) {
                        /* Share. Drop WRITABLE in both and mark both copy-on-write, so
                         * whichever side writes first takes the fault. A page that was
                         * ALREADY read-only keeps its flags (it may be read-only
                         * because of an earlier fork -- then it is still COW and both
                         * new holders inherit that -- or genuinely read-only, in which
                         * case neither side may ever write it and no COW is needed). */
                        uint64_t shared_e = e;
                        if (e & WRITABLE) {
                            shared_e = (e & ~(uint64_t)WRITABLE) | VMM_PTE_COW;
                            spt[j] = shared_e;
                            g_mm_cow_pages += 2;           /* both spaces now hold a COW PTE */
                            if (active) invlpg(va);
                        } else if (e & VMM_PTE_COW) {
                            g_mm_cow_pages += 1;           /* src already counted; dst is new */
                        }
                        /* The child's entry is the parent's entry, INSTALLED WHOLE.
                         * This used to be vmm_map_page_in(dst, va, shared_e,
                         * shared_e & 0xFFF) -- frame from the top, flags from the
                         * bottom -- which worked only because the old ~0xFFF mask
                         * carried bit 63 across inside the "frame". Now that the mask
                         * is right, splitting the entry and re-assembling it would
                         * drop NX (and every bit 52-62) on every forked page: a child
                         * would silently get an executable stack its parent did not
                         * have. Installing the entry verbatim cannot lose a bit, and
                         * it is what the code meant -- shared_e already has PRESENT
                         * (checked above), so the two are otherwise identical. */
#ifdef VMM_FORK_REASSEMBLE
                        /* NEGATIVE CONTROL (tests/unit/mm_run.sh): the entry taken
                         * apart and put back together -- frame from the top, flags
                         * from the bottom twelve bits -- which is what this line said
                         * before the NX comment above was written. It loses every bit
                         * 52..63, so the child's copy of a file-backed text page comes
                         * back without MM_PTE_NX; and it is the plausible wrong
                         * version rather than the feature switched off, because the
                         * child still gets the right FRAME and the program still runs.
                         * mm_forkfile_test requires this build to fail, and to fail on
                         * the bit-for-bit assertion and not on the refcounts. */
                        vmm_map_page_in(dst_cr3, va, shared_e & MM_PTE_ADDR, shared_e & 0xFFF);
#else
                        vmm_map_raw_in(dst_cr3, va, shared_e);
#endif
                        clone_shared++;
                        continue;
                    }

                    /* No COW (disabled), or the refcount saturated and the frame may
                     * not be shared: copy it, exactly as the eager clone always did.
                     *
                     * THE FLAGS ARE CARRIED, not rebuilt from WRITABLE alone, and that
                     * changed when file-backed text landed. This line used to say
                     * `VMM_USER | ((e & WRITABLE) ? VMM_WRITABLE : 0)`, which drops
                     * MM_PTE_NX -- a forked child silently got an executable stack its
                     * parent did not have -- and, now that a program's text can be a
                     * page-cache page, also drops VMM_PTE_FILE: the child would hold a
                     * private anonymous frame that says it is neither anonymous nor
                     * file-backed, so reclaim can neither drop it (try_drop_cached
                     * demands VMM_PTE_FILE on every PTE) nor swap it (try_drop demands
                     * VMM_PTE_ANON), and the page becomes permanently unreclaimable.
                     * The expression is do_cow()'s (fault.c), for the same reason: a
                     * private copy has the original's protections and only its sharing
                     * changes.
                     *
                     * TWO BITS ARE CLEARED, and the second is the one worth arguing.
                     * VMM_PTE_COW, because nothing is shared here. And VMM_PTE_FILE,
                     * because this frame is NOT the page cache's -- it is a private
                     * copy of what the cache held, and a PTE claiming FILE over a
                     * frame pcache_holds() says nothing about is the exact
                     * disagreement reclaim.c declines to act on. The copy is left
                     * marked neither FILE nor ANON, which makes it unreclaimable and
                     * is precisely what every page copied down this path has been
                     * since the path was written; do_cow() may carry FILE across
                     * because it can never see a file PTE (they are read-only and not
                     * COW, so a write to one is a genuine protection fault the
                     * classifier declines), and this loop sees every PTE there is. */
                    uint64_t nf = pmm_alloc_any();
                    if (!nf) goto clone_failed;       /* OOM: caller must vmm_free_space(dst) + fail the fork */
                    memcpy(mm_p2v(nf), mm_p2v(frame), 4096);
                    vmm_map_page_in(dst_cr3, va, nf,
                                    (e & MM_PTE_FLAGS) &
                                        ~(uint64_t)(VMM_PTE_COW | VMM_PTE_FILE));
                    clone_copied++;
                }
            }
        }
    }
    if (active) vmm_switch(mm_read_cr3());      /* flush the stale writable entries */
    if (shared) *shared=clone_shared;
    if (copied) *copied=clone_copied;
    return 0;
clone_failed:
    /* Earlier leaves may already have made the source COW; even a failed fork
     * must flush those permissions before returning to the source process. */
    if (active) vmm_switch(mm_read_cr3());
    return -1;
}

int vmm_clone_user(uint64_t dst_cr3,uint64_t src_cr3)
{
#ifdef MM_HOSTTEST
    return vmm_clone_user_counted(dst_cr3,src_cr3,&g_clone_shared,&g_clone_copied);
#else
    return vmm_clone_user_counted(dst_cr3,src_cr3,0,0);
#endif
}
#ifdef MM_HOSTTEST
void vmm_clone_stats(uint64_t *shared,uint64_t *copied)
{ if (shared) *shared=g_clone_shared; if (copied) *copied=g_clone_copied; }
#endif

/* How many frames this call may hold, with their PTEs already cleared, before it
 * has to stop and let the other cores catch up. See the header below: it is a
 * correctness unit rather than a tuning knob, and 64 costs 512 bytes of the
 * 32 KiB kernel stack.
 *
 * Overridable so that claim is CHECKABLE rather than asserted: the result of an
 * unmap -- frames returned, refcounts, reverse map, swap slots -- must not
 * depend on where the batch boundaries fall. Measured at 1, 3, 64 and 4096 over
 * a 256-page unmap: identical, byte for byte, in every count. A batch of 1 is
 * one shootdown per page, which is the SLOW correct answer, not a wrong one. */
#ifndef UNMAP_HOLD
#define UNMAP_HOLD 64
#endif
/* 0 would be a zero-length array and a store past it on the first frame -- the
 * one value of this knob that is not merely slow. */
_Static_assert(UNMAP_HOLD >= 1, "UNMAP_HOLD must hold at least one frame");

/* Hand back the frames this pass has unmapped -- but not before every core that
 * could still translate them has dropped the entry. This is the ordering the
 * whole fix is: a flush AFTER pmm_free closes nothing, because the window is
 * between the two.
 *
 * The shootdown is skipped entirely unless a sibling thread is running this
 * same CR3 on another core right now, which is every single-threaded program on
 * this machine: one read of g_cpu_cur[] per drain and no IPI. */
static void unmap_drain(uint64_t cr3, const uint64_t *hold, unsigned *nh)
{
    if (!*nh) return;
    if (LOGIT_HAVE(tlb_flush_all) && vmm_space_busy_elsewhere(cr3)) tlb_flush_all();
    for (unsigned i = 0; i < *nh; i++) pmm_free(hold[i]);
    *nh = 0;
}

/* Drop one reference per present PTE in [virt, virt+len) and clear the entries.
 * The page tables themselves are left in place (they are per-space and are
 * reclaimed wholesale by vmm_free_user).
 *
 * THE FRAME IS NOT HANDED BACK UNTIL EVERY CORE HAS DROPPED ITS TRANSLATION,
 * and until 2026-08-28 it was handed back first. This loop read `*pte = 0;
 * pmm_free(frame); if (active) invlpg(a);` and then returned -- no cross-core
 * shootdown, no generation bump, and even on THIS core the frame was back in
 * the allocator two instructions before the TLB entry naming it was gone.
 *
 * That was harmless while an address space had exactly one thread. It stopped
 * being harmless at M30: SYS_THREAD_CREATE gives siblings ONE CR3, so a sibling
 * can be in ring 3 on another core holding a cached WRITABLE translation to a
 * page this call is unmapping. pmm_free() then hands that frame to whoever asks
 * next -- a page table, a kheap arena, a DMA ring -- and the sibling keeps
 * writing to it. Ring 3 writing ring 0's memory, with every counter on the
 * machine reading correct. c/kernel/sched/uthread.c:197 reported it from the
 * outside, declined to patch it from there, and named both the site and the
 * ordering: flush inside this function, BEFORE the frames are released.
 *
 * WHY tlb_flush_all() IS CALLABLE HERE, when the thread-exit path took it out
 * again. Both of uthread.c's objections were about the mechanism as it stood
 * then, and both have since been answered in c/kernel/cpu/:
 *
 *   - "it cannot be called holding the BKL, because a core spinning for the BKL
 *     does so with IF=0 and can never acknowledge the IPI." True until tlb.c
 *     started RECORDING the request in a per-core flag before sending the IPI
 *     and spin_lock()'s wait loop started polling that flag (spinlock.c:54). A
 *     core that cannot take an interrupt can still read a byte.
 *     vmm_protect_range_in() forty lines below and vmm_free_space() both
 *     already call it from under the BKL, for exactly this reason.
 *   - "it gives up SILENTLY after fifty million pauses, and that spin was the
 *     dominant cost of ending a thread." The silence is gone (tlb_late_count()
 *     counts what was never acked), and the cost went with the first objection:
 *     the spin only ran to its bound when the un-acked core was one that could
 *     never answer. What is left is charged only when a sibling is ACTUALLY in
 *     this space -- `vmm_space_busy_elsewhere` is the same gate mprotect uses.
 *
 * The generation counter in sched.c is NOT replaced by this and stays where it
 * is: it is the backstop for a core that never acks at all. What this closes is
 * the window the counter cannot -- the one tick between the frames going back to
 * the PMM and that core reaching schedule().
 *
 * ONCE PER 64 FRAMES, not once per page and not once for the whole range. Once
 * per page is an IPI broadcast per 4 KiB; once for the range needs a list as
 * long as the range, and this path allocates nothing -- it is reached from
 * munmap, which is reached from thread exit, and an unmap that can fail to
 * allocate is an unmap that can fail. A fixed 512 bytes of stack bounds it
 * instead. The batch is a CORRECTNESS unit: every frame in `hold` has had its
 * PTE cleared and has not been given away, so the one flush that precedes the
 * drain covers all of them, whatever the batch size is.
 *
 * A held frame is briefly rmap_count < pmm_refcount -- rmap_remove has run and
 * pmm_free has not. That is the safe direction: reclaim evicts only when the two
 * are EQUAL, so a held frame is temporarily unreclaimable rather than wrongly
 * reclaimable. (Nothing else runs anyway; every caller of this holds the BKL,
 * SYS_KHEAP_STRESS being the one entry on syscall_is_bkl_free's allow-list.) */
uint64_t vmm_unmap_range_in(uint64_t cr3, uint64_t virt, uint64_t len)
{
    MM_GUARD(cr3);
    uint64_t start = virt & ~(uint64_t)0xFFF;
    if (!mm_user_range(virt, len)) return 0;
    uint64_t end = (virt + len + 0xFFF) & ~(uint64_t)0xFFF;
    int active = ((mm_read_cr3() & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR));
    uint64_t n = 0;
    uint64_t hold[UNMAP_HOLD];
    unsigned nh = 0;

    for (uint64_t a = start; a < end; a += 4096) {
        uint64_t *pte = range_pte(cr3, &a);
        if (!pte) continue;
        uint64_t e = *pte;
        /* A swapped-out page still occupies something -- a slot rather than a
         * frame -- so munmap has to release that too, or every unmap of
         * swapped-out memory leaks swap capacity invisibly. Nothing is held: a
         * swap entry has P=0, so no core can have a TLB entry for it, and a
         * swap slot is not a frame the PMM can hand to anyone. */
        if (vmm_pte_is_swap(e)) {
            *pte = 0;
            if (active) invlpg(a);
            swap_slot_put(vmm_pte_swap_slot(e));
            n++;
            continue;
        }
        /* A PROT_NONE page is not present and has no reverse-map entry, but its
         * frame is STILL REFERENCED (vmm.h). It has to be released here for the
         * same reason the swap slot above does: the reference is invisible to
         * every other path, so an unmap that skipped it would leak a frame per
         * guard page with nothing on the machine able to see it.
         *
         * It goes through `hold` like any other frame even though P=0 means no
         * core should still be caching it -- that "should" is an inference from
         * vmm_protect_range_in having shot the page down when it made it
         * PROT_NONE, and an inference is not what a frame handed back to the
         * allocator should rest on when the uniform path is free. */
        if (vmm_pte_is_noaccess(e)) {
            *pte = 0;
            if (active) invlpg(a);
            hold[nh++] = e & MM_PTE_ADDR;
            if (nh == UNMAP_HOLD) unmap_drain(cr3, hold, &nh);
            n++;
            continue;
        }
        if ((e & (PRESENT | USER)) != (PRESENT | USER)) continue;
        if (e & VMM_PTE_COW) g_mm_cow_pages--;
        *pte = 0;
        if (active) invlpg(a);           /* our own TLB first: the frame is still ours */
        rmap_remove(e & MM_PTE_ADDR, cr3, a);
        hold[nh++] = e & MM_PTE_ADDR;
        if (nh == UNMAP_HOLD) unmap_drain(cr3, hold, &nh);
        n++;
    }
    unmap_drain(cr3, hold, &nh);
    return n;
}

/* THE RESIDENT HALF OF mprotect. vma_protect() (vma.c) changes what a range
 * MEANS; this changes what the pages already in it PERMIT. Two halves rather
 * than one because they fail differently: the VMA edit can run out of table
 * slots and must then change nothing at all, while this one cannot fail --
 * it allocates nothing, frees nothing and only ever rewrites entries that are
 * already there.
 *
 * Three PTE shapes are met and each has one right answer:
 *
 *   ABSENT (or a swap entry)   nothing to do. The VMA carries the new
 *                              protection and the fault that eventually fills
 *                              the page reads it from there (fault.c do_anon /
 *                              do_file / reclaim_swapin all take `prot` from
 *                              the VMA). Touching a swap entry here would mean
 *                              reading a page back in order to change a bit
 *                              nobody has asked for yet.
 *   RESIDENT, new prot != 0    rebuilt below.
 *   RESIDENT, new prot == 0    made VMM_PTE_NOACCESS (see vmm.h): not present,
 *                              frame retained, contents retained.
 *
 * COPY-ON-WRITE IS RE-DERIVED FROM THE REFCOUNT, NOT CARRIED ACROSS. This is
 * the one place in the file where that is true and it is forced:
 *
 *   - taking WRITE AWAY from a COW page cannot simply clear W and keep the COW
 *     bit, because mm_fault_classify()'s present-page branch answers
 *     MM_FAULT_COW on `pte_cow` alone and never consults the VMA -- so the
 *     write it is supposed to refuse would be served by copying the page. The
 *     bit has to go.
 *   - and once it is gone, GIVING write back cannot just set W, because the
 *     frame may still be shared with a forked sibling; setting W would let two
 *     processes write one frame.
 *
 * So the rule is symmetric and stateless: writable is granted as WRITABLE when
 * this space is the frame's only holder, and as COW when it is not. That is
 * exactly the invariant vmm_clone_user establishes, re-established from the
 * same number it uses. It costs one pmm_refcount() per writable page.
 *
 * WHY c/kernel/exec/load/elf.c's PASS 2 IS NOT REWRITTEN ONTO THIS, since it is the
 * same operation by hand and the obvious thing to do with a new function is to
 * find its existing open-coded copies. Two reasons, and the second is a bug
 * waiting to happen rather than a matter of taste:
 *
 *   - elf.c derives each page's protection from page_prot(), a UNION over the
 *     program headers, so its answer varies page by page inside one range.
 *     This takes ONE `prot` for the whole range, because that is what an
 *     mprotect ABI means. Driving it from here would need a callback per page,
 *     which is elf.c's loop with extra steps.
 *   - the copy-on-write rule above would be WRONG there. A file-backed text
 *     page at load time has refcount 2 -- the mapping and the page cache's own
 *     reference (pcache.h) -- so "refcount > 1 means share it copy-on-write"
 *     would mark a program's writable .data copy-on-write against a frame
 *     nobody else can write, on every exec. That rule is correct here only
 *     because vma_protect() has already refused VMA_WRITE over a file area, so
 *     no page this function makes writable can be a cache page. elf.c has no
 *     VMA in the eager case and therefore no such guarantee. */
uint64_t vmm_protect_range_in(uint64_t cr3, uint64_t virt, uint64_t len, uint32_t prot)
{
    MM_GUARD(cr3);
    uint64_t start = virt & ~(uint64_t)0xFFF;
    if (!mm_user_range(virt, len)) return 0;
    uint64_t end = (virt + len + 0xFFF) & ~(uint64_t)0xFFF;
    int active = ((mm_read_cr3() & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR));
    uint64_t n = 0;

    for (uint64_t a = start; a < end; a += 4096) {
        uint64_t *pte = range_pte(cr3, &a);
        if (!pte) continue;
        uint64_t e = *pte;
        if (vmm_pte_is_swap(e)) continue;              /* see above: the VMA carries it */

        int resident = (e & PRESENT) && (e & USER);
        int noaccess = vmm_pte_is_noaccess(e);
        if (!resident && !noaccess) continue;          /* never touched: the VMA carries it */

        uint64_t frame = e & MM_PTE_ADDR;
        /* Everything mm owns about the page's ORIGIN survives; everything about
         * its permissions is rebuilt. ANON/FILE say where the bytes came from
         * and reclaim needs them either way; COW and W and NX are the answer to
         * this call. */
        uint64_t keep = e & (VMM_PTE_ANON | VMM_PTE_FILE);
        uint64_t ne;

        if (!prot) {
            ne = frame | keep | VMM_PTE_NOACCESS | MM_PTE_NX;   /* P=0, U=0, W=0 */
        } else {
            ne = frame | keep | PRESENT | USER;
#ifdef VMM_PROTECT_KEEP_COW
            /* NEGATIVE CONTROL (tests/unit/mm_run.sh): the copy-on-write marker
             * CARRIED across the protection change instead of re-derived from
             * the refcount -- which is what anybody reads this loop and expects,
             * because every other path in this file preserves the flags it is
             * not changing. It draws a perfectly good picture: the pages are
             * present, the contents are right, the write bit reads exactly as
             * asked, and reclaim is happy. What it does is serve a WRITE to a
             * page the caller just made read-only, by copying it -- because
             * mm_fault_classify's present-page branch answers MM_FAULT_COW on
             * `pte_cow` alone and never looks at the VMA. */
            ne |= (e & VMM_PTE_COW);
            if ((prot & VMA_WRITE) && !(e & VMM_PTE_COW)) ne |= WRITABLE;
#else
            if (prot & VMA_WRITE) {
                if (pmm_refcount(frame) > 1) { ne |= VMM_PTE_COW; }
                else                           ne |= WRITABLE;
            }
#endif
            if (!(prot & VMA_EXEC)) ne |= MM_PTE_NX;
        }

        if (ne == e) continue;                         /* already right: do not churn
                                                        * the reverse map or the TLB */
        /* g_mm_cow_pages is a hand-maintained count of PTEs carrying the bit
         * (vmm_unmap_range_in and vmm_free_user maintain it the same way), so
         * both directions have to be accounted here or the number drifts. */
        if ((e & VMM_PTE_COW) && !(ne & VMM_PTE_COW)) g_mm_cow_pages--;
        if (!(e & VMM_PTE_COW) && (ne & VMM_PTE_COW)) g_mm_cow_pages++;

        /* Through set_leaf, not by hand: it is the one place a leaf changes and
         * therefore the only place the reverse map can be kept right. Going
         * PRESENT|USER -> NOACCESS removes the rmap entry (correct: there is no
         * user mapping any more, only a retained frame), and coming back adds
         * it again. */
        uint64_t *pml4 = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
        uint64_t *pdpt = next_table(pml4, (a >> 39) & 0x1FF, 1);   if (!pdpt) continue;
        uint64_t *pd   = next_table(pdpt, (a >> 30) & 0x1FF, 1);   if (!pd)   continue;
        uint64_t *pt   = next_table(pd,   (a >> 21) & 0x1FF, 1);   if (!pt)   continue;
        set_leaf(cr3, pt, a, ne);
        if (active) invlpg(a);
        n++;
    }

    /* THE OTHER CORES, and this call needs the shootdown more than munmap does.
     *
     * A process is not one thread any more (M30): several threads share one
     * CR3 and can be running on other cores at this instant, each with its own
     * TLB. `invlpg` above reaches only ours. A sibling holding a cached
     * WRITABLE translation for a page just made read-only or PROT_NONE would
     * keep writing to it -- and would do so silently, which is the failure
     * mode this whole call exists to make impossible.
     *
     * Once for the whole range, not once per page: tlb_flush_all() is an IPI
     * broadcast, and a per-page one over an 8 MiB mprotect would be two
     * thousand of them. And only when a sibling is actually running this space
     * -- the single-threaded case is every program on this machine today, and
     * it pays nothing. Safe to call from here for the reason spelled out at
     * vmm_free_space(): tlb_flush_all() records the request in a per-core flag
     * BEFORE it sends the IPI and spin_lock()'s wait loop polls that flag, so a
     * core spinning with IF=0 still answers. */
    if (n && LOGIT_HAVE(tlb_flush_all) && vmm_space_busy_elsewhere(cr3)) tlb_flush_all();
    return n;
}

/* Free every frame + page-table page under the private user subtree (PDPT[1]).
 * Leaves the (now-empty) PDPT[1] slot zeroed so the space can be repopulated
 * (execve). Does NOT touch the shared kernel PDPT entries.
 *
 * With copy-on-write this is still exactly one pmm_free per present user PTE,
 * which is exactly one reference: a shared frame simply has other spaces'
 * references left and stays allocated. */
void vmm_free_user(uint64_t cr3)
{
    MM_GUARD(cr3);
    vma_space_clear(cr3);

    uint64_t *pml4 = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    for (int l = 0; l < WIDE_PML4_END; l++) {
        if (l != USER_PML4_IDX && l < WIDE_PML4_FIRST) continue;
        if (!(pml4[l] & PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)mm_p2v(pml4[l] & MM_PTE_ADDR);
        int first = l == USER_PML4_IDX ? USER_PDPT_IDX : 0;
        int limit = l == USER_PML4_IDX ? USER_PDPT_IDX + 1 : 512;
        for (int q = first; q < limit; q++) {
            uint64_t pde = pdpt[q];
            if (!(pde & PRESENT) || (pde & 0x80)) continue;
            uint64_t *pd = (uint64_t *)mm_p2v(pde & MM_PTE_ADDR);
            for (int i = 0; i < 512; i++) {
                if (!(pd[i] & PRESENT) || (pd[i] & 0x80)) continue;
                uint64_t *pt = (uint64_t *)mm_p2v(pd[i] & MM_PTE_ADDR);
                for (int j = 0; j < 512; j++) {
                    uint64_t e = pt[j];
                    uint64_t va = ((uint64_t)l << 39) | ((uint64_t)q << 30) |
                                  ((uint64_t)i << 21) | ((uint64_t)j << 12);
                    if (vmm_pte_is_swap(e)) {
                        pt[j] = 0;
                        swap_slot_put(vmm_pte_swap_slot(e));   /* a dying process releases its swap */
                        continue;
                    }
                    /* A PROT_NONE page: not present, no rmap entry, frame still
                     * referenced (vmm.h). Released here for the same reason the swap
                     * slot above is -- a dying process must give back everything it
                     * holds, and this is the one holding that no other loop can see. */
                    if (vmm_pte_is_noaccess(e)) {
                        pt[j] = 0;
                        pmm_free(e & MM_PTE_ADDR);
                        continue;
                    }
                    if ((e & (PRESENT | USER)) == (PRESENT | USER)) {
                        if (e & VMM_PTE_COW) g_mm_cow_pages--;
                        pt[j] = 0;
                        rmap_remove(e & MM_PTE_ADDR, cr3, va);
                        pmm_free(e & MM_PTE_ADDR);
                    }
                }
                pmm_free(pd[i] & MM_PTE_ADDR);     /* the PT frame */
            }
            pmm_free(pde & MM_PTE_ADDR);           /* the PD frame */
            pdpt[q] = 0;
        }
        /* Legacy PDPT contains shared kernel entries and stays until the
         * address space itself dies. Wide PDPTs are wholly private. */
        if (l >= WIDE_PML4_FIRST) {
            pmm_free(pml4[l] & MM_PTE_ADDR);
            pml4[l] = 0;
        }
    }

    /* If this tore down the ACTIVE user space (execve), stale TLB entries for the
     * old image may still translate its VAs to the just-freed frames (which PMM can
     * now hand to anyone). No PCID here, so a same-value CR3 reload is the only
     * full flush available; vmm_free_space never runs on the active CR3, so it
     * correctly skips this. */
    uint64_t cur = mm_read_cr3();
    if ((cur & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR))
        vmm_switch(cur);
}

/* Tear down an entire address space created by vmm_new_space: the user subtree,
 * the private PDPT frame, and the PML4 frame. The shared kernel tables (other
 * PDPT/PML4 entries) are left untouched. Must not be called on the active CR3. */
void vmm_free_space(uint64_t cr3)
{
    MM_GUARD(cr3);
    if (!cr3) return;
    mm_space_retire(cr3);
    uint64_t *pml4 = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    uint64_t pdpt_e = pml4[USER_PML4_IDX];
    vmm_free_user(cr3);
    vma_space_free(cr3);
    {
        /* Retirement already removed this space from publication. Hold the
         * same owner for physical root release, so no publisher can observe
         * a freed/reused PML4 or private low PDPT. AS -> kernel, never reverse. */
        MM_KERNEL_GUARD;
        if (pdpt_e&PRESENT) pmm_free(pdpt_e&MM_PTE_ADDR);
        pmm_free(cr3&MM_PTE_ADDR);
    }
    /* THE SHOOTDOWN IS WIRED IN NOW, and this comment used to say why it could
     * not be. It said: "a core spinning to acquire the BKL does so with IF=0
     * and cannot service the shootdown IPI, so it never acks and the initiator
     * deadlocks." That was true and it is the reason M25 P2b shipped as
     * infrastructure nobody could call.
     *
     * What changed is that the spin no longer has to take an interrupt to
     * answer: tlb_flush_all() records the request in a per-core flag before it
     * sends the IPI, and spin_lock()'s wait loop polls that flag. A core that
     * cannot be interrupted can still read a byte. tlb_late_count() stays at 0
     * on this path, which is the assertion that the mechanism actually reaches
     * a blocked core rather than timing out politely.
     *
     * It is not redundant now that it works: the old "not needed" argument
     * rested on every thread of the dying space having CR3-switched away
     * first, which is true today and is exactly the kind of invariant that
     * stops being true quietly. */
    if (LOGIT_HAVE(tlb_flush_all)) tlb_flush_all();
}

static int user_page_ok(uint64_t cr3, uint64_t virt, int write)
{
    uint64_t *pml4 = (uint64_t *)mm_p2v(cr3 & MM_PTE_ADDR);
    uint64_t e = pml4[(virt >> 39) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER) || (e & 0x80)) return 0;
    uint64_t *pdpt = (uint64_t *)mm_p2v(e & MM_PTE_ADDR);
    e = pdpt[(virt >> 30) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER) || (e & 0x80)) return 0;
    uint64_t *pd = (uint64_t *)mm_p2v(e & MM_PTE_ADDR);
    e = pd[(virt >> 21) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER) || (e & 0x80)) return 0;
    uint64_t *pt = (uint64_t *)mm_p2v(e & MM_PTE_ADDR);
    e = pt[(virt >> 12) & 0x1FF];
    if ((e & (PRESENT | USER)) != (PRESENT | USER)) return 0;
    if (write && !(e & WRITABLE)) return 0;
    return 1;
}

int vmm_user_range_ok(uint64_t cr3, const void *ptr, uint64_t len, int write)
{
    MM_GUARD(cr3);
    if (!cr3) return 0;
    if (len == 0) return 1;                 /* a zero-length access is valid even at NULL */
    if (!ptr) return 0;
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + len - 1;
    if (end < start) return 0;
    if (!mm_user_range(start, len)) return 0;
    for (uint64_t p = start & ~(uint64_t)0xFFF;; p += 0x1000) {
        if (!user_page_ok(cr3, p, write)) return 0;
        if (p >= (end & ~(uint64_t)0xFFF)) break;
    }
    return 1;
}

/* See vmm.h. This is the kernel-side half of the Dirty COW lesson: a kernel
 * write to a user page must RESOLVE the copy-on-write first, through the same
 * code a userspace write goes through, and must not be allowed to reach a
 * plain memcpy against a read-only mapping. See usercopy.c for the argument
 * about why resolve-then-copy has no window here. */
int vmm_user_range_fault_in(uint64_t cr3, const void *ptr, uint64_t len, int write)
{
    MM_GUARD(cr3);
    if (!cr3) return 0;
    if (len == 0) return 1;
    if (!ptr) return 0;
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + len - 1;
    if (end < start) return 0;
    if (!mm_user_range(start, len)) return 0;

    for (uint64_t p = start & ~(uint64_t)0xFFF;; p += 0x1000) {
        if (!user_page_ok(cr3, p, write)) {
            /* Not usable as-is. Ask the fault path to make it usable; it only
             * says yes for a copy-on-write page or a reserved anonymous page,
             * so a genuinely bad pointer still fails here. */
            uint64_t err = (write ? 0x2 : 0x0) | (user_page_ok(cr3, p, 0) ? 0x1 : 0x0);
            if (!mm_fault_in(cr3, p, err)) return 0;
            if (!user_page_ok(cr3, p, write)) return 0;    /* belt and braces */
        }
        if (p >= (end & ~(uint64_t)0xFFF)) break;
    }
    return 1;
}

/* A pin has an independent reference: pmm_pin prevents reclaim, while the
 * reference prevents explicit munmap from returning the page to the PMM. */
int vmm_pin_user_page(uint64_t cr3,uint64_t va,int write,uint64_t *phys)
{
    MM_GUARD(cr3);
    if (!phys || !mm_user_addr(va) || !mm_space_live(cr3)) return -1;
    if (!vmm_user_range_fault_in(cr3,(void *)(uintptr_t)va,1,write)) return -1;
    uint64_t *pte=vmm_pte(cr3,va);
    if (!pte || !user_page_ok(cr3,va,write)) return -1;
    uint64_t p=*pte & MM_PTE_ADDR;
    if (pmm_ref(p)<0) return -1;
    pmm_pin(p);
    /* Alias copies bypass the hardware PTE walker; account access/dirty here
     * so reclaim observes the same information as a user load/store. */
    *pte |= VMM_PTE_ACCESSED | (write ? VMM_PTE_DIRTY : 0);
    *phys=p;
    return 0;
}
int vmm_copy_in_space(uint64_t cr3,void *kernel,uint64_t va,uint64_t len,int write)
{
    if (!len) return 0;
    if (!kernel || !mm_user_range(va,len)) return -1;
    MM_GUARD(cr3);
    if (!mm_space_live(cr3)) return -1;
    uint8_t *k=kernel;
    while (len) {
        /* Ptrace's existing contract is resident-only. Resolve a present COW
         * write, but never first-touch an absent page or silently read swap. */
        if (!user_page_ok(cr3,va,0)) return -1;
        uint64_t p,n=4096-(va&4095); if (n>len) n=len;
        if (vmm_pin_user_page(cr3,va,write,&p)) return -1;
        void *alias=(uint8_t *)mm_p2v(p)+(va&4095);
        if (write) memcpy(alias,k,(size_t)n); else memcpy(k,alias,(size_t)n);
        pmm_unpin(p); pmm_free(p);
        k+=n; va+=n; len-=n;
    }
    return 0;
}
void vmm_flush_space(uint64_t cr3)
{
    if ((mm_read_cr3() & MM_PTE_ADDR)==(cr3 & MM_PTE_ADDR))
        mm_write_cr3(mm_read_cr3());
    if (LOGIT_HAVE(tlb_flush_all) && vmm_space_busy_elsewhere(cr3)) tlb_flush_all();
}
