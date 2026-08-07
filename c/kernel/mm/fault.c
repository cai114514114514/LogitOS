#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "mmhost.h"
#include "kprintf.h"

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

void mm_set_cow(int on) { g_cow_on = on ? 1 : 0; }
int  mm_cow_enabled(void) { return g_cow_on; }

uint64_t mm_cow_faults(void)     { return g_cow_copies; }
uint64_t mm_cow_reuse(void)      { return g_cow_reuse; }
uint64_t mm_anon_faults(void)    { return g_anon; }
uint64_t mm_fault_declined(void) { return g_declined; }

/* ------------------------------------------------------- the decision --- */
int mm_fault_classify(uint64_t cr2, uint64_t err, int pte_present,
                      int pte_cow, int pte_user, int vma_prot)
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
        uint64_t nf = pmm_alloc();
        if (!nf)
            return 0;               /* out of memory: decline. The process dies,
                                     * the kernel does not. */
        memcpy(mm_p2v(nf), mm_p2v(old), 4096);
        *pte = nf | ((e & 0xFFF) & ~VMM_PTE_COW) | PRESENT | WRITABLE;
        pmm_free(old);              /* drop THIS space's reference, no more */
        g_cow_copies++;
    }
    g_mm_cow_pages--;
    if (active) mm_invlpg(page);
    (void)cr3;
    return 1;
}

static int do_anon(uint64_t cr3, uint64_t page, uint32_t prot, int active)
{
    uint64_t f = pmm_alloc();
    if (!f)
        return 0;
    memset(mm_p2v(f), 0, 4096);     /* anonymous memory reads as zero, always:
                                     * handing over a frame with the previous
                                     * owner's bytes in it is a disclosure bug */
    vmm_map_page_in(cr3, page, f, VMM_USER | ((prot & VMA_WRITE) ? VMM_WRITABLE : 0));
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
    uint32_t prot = vma_prot_at(cr3, page);
    int active = ((mm_read_cr3() & ~(uint64_t)0xFFF) == (cr3 & ~(uint64_t)0xFFF));

    switch (mm_fault_classify(va, err, present, cow, user, (int)prot)) {
    case MM_FAULT_COW:
        if (do_cow(cr3, page, pte, active)) return 1;
        break;
    case MM_FAULT_ANON:
        if (do_anon(cr3, page, prot, active)) return 1;
        break;
    default:
        break;
    }
    g_declined++;
    return 0;
}

int mm_fault(uint64_t cr2, uint64_t err, uint64_t rip)
{
    (void)rip;
    return mm_fault_in(mm_read_cr3() & ~(uint64_t)0xFFF, cr2, err);
}

/* ------------------------------------------------------------ report --- */
void mm_report(const char *tag)
{
    pmm_report(tag);
    kprintf("[mm] %s: cow=%s, %d pages shared copy-on-write; faults: %d copied, "
            "%d reused, %d anon, %d declined; %d address spaces with areas\n",
            tag ? tag : "-",
            g_cow_on ? "on" : "off (page-fault hook not wired -- see c/kernel/mm/mm.h)",
            (int)g_mm_cow_pages, (int)g_cow_copies, (int)g_cow_reuse,
            (int)g_anon, (int)g_declined, vma_spaces_live());
}
