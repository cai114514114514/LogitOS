#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "rmap.h"
#include "reclaim.h"
#include "mmhost.h"
#include "kprintf.h"

void reclaim_late_init(void);
#ifndef MM_HOSTTEST
#include "kheap.h"
#endif

/* The page-fault half of memory management: what a #PF MEANS.
 *
 * Two halves on purpose:
 *   mm_fault_classify()  -- pure. Given the error code, the address, three
 *                           bits of PTE state and the VMA's protection, decide
 *                           what kind of fault this is. No side effects, so
 *                           the whole table can be enumerated in a host test,
 *                           including every case that must still KILL the
 *                           process. Fault containment is a property of this
 *                           function, and it is tested as one.
 *   mm_fault_in()        -- acts on that decision.
 *
 * The rule the classifier is built around: claim a fault ONLY when we can name
 * why it is legitimate. Everything else returns MM_FAULT_NONE and falls
 * through to the behaviour that exists today -- a ring-3 fault kills that
 * process alone; a ring-0 fault panics. A handler that guesses is a handler
 * that turns a null-pointer bug into a silently allocated zero page. */

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

/* Page-fault error code bits. */
#define PF_P     0x01   /* 0 = page not present, 1 = protection violation */
#define PF_W     0x02   /* the access was a write */
#define PF_U     0x04   /* the access came from ring 3 */
#define PF_RSVD  0x08   /* a reserved bit was set in a page-table entry */
#define PF_I     0x10   /* instruction fetch */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

static int g_cow_on = MM_COW_DEFAULT;
static uint64_t g_cow_copies, g_cow_reuse, g_anon, g_declined;

/* WHAT A FAULT COSTS. Demand paging and copy-on-write moved work OFF fork and
 * ONTO the page fault -- fork of /bin/sh went from 258 pages copied to 0. That
 * is only a win if the faults that replaced them are cheaper than the copies
 * they saved, and nothing in the tree could say. So the fault path times
 * itself, per class, and mm_report() prints it.
 *
 * rdtsc rather than the ns clock for the same reason proc.c uses it: a fault is
 * microseconds and the 100 Hz tick cannot see one. Local rather than kbench.h's
 * because this file is also compiled for the host test, which puts neither
 * c/kernel/sched nor an x86 on the include path. */
#ifdef MM_HOSTTEST
static inline uint64_t fault_cyc(void) { return 0; }
#else
static inline uint64_t fault_cyc(void)
{ uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo; }
#endif
static uint64_t g_cyc_copy, g_cyc_reuse, g_cyc_anon, g_cyc_swapin;
static uint64_t g_swapin;

void mm_fault_cost(uint64_t *copy_cyc, uint64_t *reuse_cyc, uint64_t *anon_cyc);
void mm_fault_cost(uint64_t *copy_cyc, uint64_t *reuse_cyc, uint64_t *anon_cyc)
{
    if (copy_cyc)  *copy_cyc  = g_cyc_copy;
    if (reuse_cyc) *reuse_cyc = g_cyc_reuse;
    if (anon_cyc)  *anon_cyc  = g_cyc_anon;
}

void mm_set_cow(int on) { g_cow_on = on ? 1 : 0; }
int  mm_cow_enabled(void) { return g_cow_on; }

uint64_t mm_cow_faults(void)     { return g_cow_copies; }
uint64_t mm_cow_reuse(void)      { return g_cow_reuse; }
uint64_t mm_anon_faults(void)    { return g_anon; }
uint64_t mm_fault_declined(void) { return g_declined; }

/* ------------------------------------------------------- the decision --- */
int mm_fault_classify(uint64_t cr2, uint64_t err, int pte_present,
                      int pte_cow, int pte_user, int vma_prot, int pte_swap)
{
    /* A reserved bit set in a page-table entry means the tables themselves are
     * malformed. That is a kernel bug and must never be "handled". */
    if (err & PF_RSVD)
        return MM_FAULT_NONE;

    /* mm owns the private user region and nothing else. A fault at a kernel
     * address, at a NULL dereference, or anywhere below the user base is
     * somebody's bug, and staying out of it is what keeps a null-pointer
     * dereference fatal instead of quietly allocating a page at address 0. */
    if (cr2 < MM_USER_BASE || cr2 >= MM_USER_END)
        return MM_FAULT_NONE;

    /* A swap entry, before anything else looks at the VMA.
     *
     * This entry was written by reclaim, so its presence is proof that the page
     * is legitimate -- more proof than a VMA could offer, since the pages that
     * most need to come back (an ELF image's text and data) never had a VMA in
     * the first place. It is also checked before the anonymous case on purpose:
     * a swap entry has P=0 and would otherwise be indistinguishable from an
     * untouched mmap page and refilled with zeroes, which is not a crash but
     * silent corruption of a running program.
     *
     * The error code must agree that the page is absent. If the CPU reports a
     * protection violation (P=1) on an entry we believe is swapped, the tables
     * and the hardware disagree and nothing here should act on either. */
    if (pte_swap) {
        if (err & PF_P) return MM_FAULT_NONE;
        return MM_FAULT_SWAP;
    }

    if (err & PF_P) {
        /* The page IS present: this is a permission violation. */
        if (!(err & PF_W))
            return MM_FAULT_NONE;   /* a read or an instruction fetch that the
                                     * PTE's own flags refused (no USER, or NX);
                                     * copy-on-write has nothing to say about it */
        if (!pte_present || !pte_user)
            return MM_FAULT_NONE;   /* the tables disagree with the error code,
                                     * or the page is kernel-only: refuse */
        if (pte_cow)
            return MM_FAULT_COW;    /* the one case we exist for */
        return MM_FAULT_NONE;       /* a genuine write to a read-only page */
    }

    /* The page is NOT present. */
    if (pte_present)
        return MM_FAULT_NONE;       /* the CPU saw P=0 and we see P=1. Nothing
                                     * can legitimately have changed the PTE in
                                     * between (a process has one thread, and
                                     * only its own thread edits its PTEs), so
                                     * this is corruption, not a stale TLB. */
    if (!(vma_prot & VMA_READ))
        return MM_FAULT_NONE;       /* nothing reserved this address: a wild
                                     * pointer, or a stack that ran off its end */
    if ((err & PF_W) && !(vma_prot & VMA_WRITE))
        return MM_FAULT_NONE;       /* write to a read-only reservation */
    return MM_FAULT_ANON;
}

/* ------------------------------------------------------- acting on it --- */

static uint64_t fault_frame(void);

static int do_cow(uint64_t cr3, uint64_t page, uint64_t *pte, int active)
{
    uint64_t e = *pte;
    uint64_t old = e & ~(uint64_t)0xFFF;
    unsigned rc = pmm_refcount(old);

    if (rc == 0) {
        /* A live PTE pointing at a frame the allocator thinks is free. Do not
         * "repair" it -- that would hand the process a frame someone else owns.
         * Report and decline, so the process dies and the damage stops here. */
        kprintf("[mm] BUG: copy-on-write PTE at %p points at unallocated frame %p\n",
                (void *)page, (void *)old);
        return 0;
    }

    if (rc == 1) {
        /* Sole owner: everyone else has already copied away. There is nothing
         * to copy -- just give the write permission back. This is why a fork
         * that is never written to costs nothing, and why the parent of a
         * short-lived child gets its pages back one cheap fault at a time. */
        *pte = (e | WRITABLE) & ~VMM_PTE_COW;
        g_cow_reuse++;
    } else {
        uint64_t nf = fault_frame();
        if (!nf)
            return 0;               /* out of memory even after a forced reclaim
                                     * pass: decline. The process dies, the
                                     * kernel does not. */
        memcpy(mm_p2v(nf), mm_p2v(old), 4096);
        *pte = nf | ((e & 0xFFF) & ~VMM_PTE_COW) | PRESENT | WRITABLE;
        /* This PTE now points somewhere else, so the reverse map has to be told
         * about BOTH ends of the move. Editing the PTE by hand here rather than
         * going through vmm.c's set_leaf() is why: a page-fault handler wants
         * the entry it already has a pointer to, not another four-level walk.
         * The price is these two lines, and rmap_audit() is what proves they
         * are not forgotten. Note (e & 0xFFF) carries VMM_PTE_ANON across, so a
         * copy of an anonymous page is still reclaimable by the drop tier. */
        rmap_remove(old, cr3, page);
        rmap_add(nf, cr3, page);
        pmm_free(old);              /* drop THIS space's reference, no more */
        g_cow_copies++;
    }
    g_mm_cow_pages--;
    if (active) mm_invlpg(page);
    return 1;
}

/* A frame, or a real answer that there is none.
 *
 * pmm_alloc() consults reclaim through a watermark heuristic, and a heuristic
 * is allowed to be wrong: it can be in a backoff, or the pressure can have
 * arrived faster than the watermark noticed. An allocation that has actually
 * FAILED is not a heuristic -- it is the fact the heuristic exists to predict.
 * So the fault path asks once more, forcing a pass, before doing the one thing
 * that cannot be undone.
 *
 * This matters because of what "return 0" means here: the process dies. Killing
 * a program for want of a page while tens of thousands of reclaimable pages are
 * sitting in memory is the exact failure this whole line was built to remove,
 * and it is reachable through nothing more exotic than an unlucky moment. */
static uint64_t fault_frame(void)
{
    uint64_t f = pmm_alloc();
    if (f) return f;
    if (reclaim_frames(64)) f = pmm_alloc();
    return f;
}

static int do_anon(uint64_t cr3, uint64_t page, uint32_t prot, int active)
{
    uint64_t f = fault_frame();
    if (!f)
        return 0;
    memset(mm_p2v(f), 0, 4096);     /* anonymous memory reads as zero, always:
                                     * handing over a frame with the previous
                                     * owner's bytes in it is a disclosure bug */
    /* VMM_PTE_ANON records that THIS function is what produces the page's
     * contents. Reclaim's cheapest tier is allowed to throw such a page away
     * and let this code make it again; without the mark it cannot tell a page
     * whose contents are reproducible from one whose contents came from a file
     * nothing re-reads. See vmm.h and reclaim.h. */
    vmm_map_page_in(cr3, page, f,
                    VMM_USER | VMM_PTE_ANON | ((prot & VMA_WRITE) ? VMM_WRITABLE : 0));
    if (active) mm_invlpg(page);
    g_anon++;
    return 1;
}

int mm_fault_in(uint64_t cr3, uint64_t va, uint64_t err)
{
    if (!cr3) { g_declined++; return 0; }
    uint64_t page = va & ~(uint64_t)0xFFF;
    uint64_t *pte = vmm_pte(cr3, page);
    int present = pte && (*pte & PRESENT);
    int cow     = pte && (*pte & VMM_PTE_COW);
    int user    = pte && (*pte & USER);
    int swapped = pte && vmm_pte_is_swap(*pte);
    uint32_t prot = vma_prot_at(cr3, page);
    int active = ((mm_read_cr3() & ~(uint64_t)0xFFF) == (cr3 & ~(uint64_t)0xFFF));

    uint64_t t0 = fault_cyc();
    switch (mm_fault_classify(va, err, present, cow, user, (int)prot, swapped)) {
    case MM_FAULT_COW: {
        uint64_t before = g_cow_copies;
        if (do_cow(cr3, page, pte, active)) {
            /* Split by outcome, not by class: the sole-owner case is a PTE
             * write and the shared case is a 4 KiB memcpy plus an allocation.
             * Averaging them together would hide which one the workload
             * actually hits, which is the only interesting thing about them. */
            if (g_cow_copies != before) g_cyc_copy  += fault_cyc() - t0;
            else                        g_cyc_reuse += fault_cyc() - t0;
            return 1;
        }
        break;
    }
    case MM_FAULT_ANON:
        if (do_anon(cr3, page, prot, active)) { g_cyc_anon += fault_cyc() - t0; return 1; }
        break;
    case MM_FAULT_SWAP:
        /* THE ONE FAULT THAT GOES TO A DISK. Everything above resolves in
         * microseconds out of memory; this one is milliseconds and can give up
         * the CPU in the middle (see swap.c on which of its waits releases the
         * big kernel lock and which does not). Timed separately for exactly
         * that reason -- averaging a disk read into the copy-on-write numbers
         * would make both meaningless. */
        if (reclaim_swapin(cr3, page, pte, active)) {
            g_swapin++;
            g_cyc_swapin += fault_cyc() - t0;
            return 1;
        }
        break;
    default:
        break;
    }
    g_declined++;
    return 0;
}

uint64_t mm_swapin_faults(void) { return g_swapin; }

int mm_fault(uint64_t cr2, uint64_t err, uint64_t rip)
{
    (void)rip;
#ifndef MM_HOSTTEST
    /* Bring reclaim up at the first fault taken FROM RING 3. See
     * reclaim_late_init(): that is the one moment where user code was running,
     * so no kernel operation is half-finished on this thread and the block
     * layer swap_init() needs is enumerated and idle. */
    if (err & PF_U) reclaim_late_init();
#endif
    return mm_fault_in(mm_read_cr3() & ~(uint64_t)0xFFF, cr2, err);
}

/* ------------------------------------------------------------ report --- */
void mm_report(const char *tag)
{
    pmm_report(tag);
    /* The kernel heap is a different allocator with a different failure mode:
     * it takes frames from the PMM and never gives them back, so it can hold
     * megabytes while every PMM invariant is intact. A memory report that shows
     * only frames cannot tell those two apart.
     * Not in the host tests: mm_run.sh links pmm/vmm/fault/vma without kheap.c
     * (that script belongs to another line), and this report is about a running
     * machine anyway. tests/unit/leak_kheap_test.c covers kheap on the host. */
#ifndef MM_HOSTTEST
    kheap_report(tag);
#endif
    kprintf("[mm] %s: cow=%s, %d pages shared copy-on-write; faults: %d copied, "
            "%d reused, %d anon, %d declined; %d address spaces with areas\n",
            tag ? tag : "-",
            g_cow_on ? "on" : "off (page-fault hook not wired -- see c/kernel/mm/mm.h)",
            (int)g_mm_cow_pages, (int)g_cow_copies, (int)g_cow_reuse,
            (int)g_anon, (int)g_declined, vma_spaces_live());
    kprintf("[mm] %s: fault cost (kcycles total / cycles each): "
            "cow-copy %d/%d, cow-reuse %d/%d, anon %d/%d, swap-in %d/%d\n",
            tag ? tag : "-",
            (int)(g_cyc_copy / 1000),   (int)(g_cow_copies ? g_cyc_copy / g_cow_copies : 0),
            (int)(g_cyc_reuse / 1000),  (int)(g_cow_reuse  ? g_cyc_reuse / g_cow_reuse : 0),
            (int)(g_cyc_anon / 1000),   (int)(g_anon       ? g_cyc_anon / g_anon : 0),
            (int)(g_cyc_swapin / 1000), (int)(g_swapin     ? g_cyc_swapin / g_swapin : 0));
    reclaim_report(tag);
}
