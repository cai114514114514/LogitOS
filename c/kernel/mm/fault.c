#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "rmap.h"
#include "pcache.h"
#include "shm.h"
#include "reclaim.h"
#include "oom.h"
#include "mmhost.h"
#include "kprintf.h"

/* WEAK, and the reason is about the test tree rather than about the kernel.
 * c/kernel/mm/oom.c reads the PROCESS TABLE, so it only makes sense where there
 * is one; tests/unit/mm_run.sh and tests/unit/leak_run.sh both compile this
 * file over a simulated physical memory with no processes at all, and their two
 * source lists are required to be identical to each other by a comment in both.
 * A hard call would make "the fault path asks the killer" a link error in a
 * dozen host suites that are about something else -- so the hook is weak, the
 * kernel always provides it (C_SRC globs c/kernel/mm), and a harness that does
 * not link it gets exactly the behaviour that existed before this line. The
 * same trick, for the same reason, as proc.c's sock_close_owner. */
int oom_fault_retry(void) __attribute__((weak));

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
/* Set by fault_frame() when a fault is about to be declined FOR WANT OF MEMORY,
 * which is a different event from every other decline in this file: a decline
 * for a bad address is the process's own doing and killing it is correct, while
 * a decline for want of memory kills whoever happened to touch a page next.
 * Only mm_fault_in() reads it, and it clears it first, so it never carries
 * across faults. */
static int g_oom_decline;
static uint64_t g_oom_retry, g_oom_saved;

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
static uint64_t g_cyc_copy, g_cyc_reuse, g_cyc_anon, g_cyc_swapin, g_cyc_file;
static uint64_t g_swapin, g_file;
static uint64_t g_shm, g_cyc_shm;

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
                      int pte_cow, int pte_user, int vma_prot, int pte_swap,
                      int vma_file, int vma_shm)
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

    /* A file-backed reservation. LAST, and after the swap check rather than
     * before it, which is the ordering rule the swap case already states and
     * which this case has to fit without weakening:
     *
     *   the swap marker is checked FIRST because a swap entry is self-evidently
     *   ours -- the kernel wrote it -- and because the pages that most need to
     *   come back have no VMA at all. Nothing here changes that. A file page
     *   that was SWAPPED (it can be: an invalidated-but-still-mapped page loses
     *   its cache entry and then reclaim can only swap it) has the marker in its
     *   PTE, so it is caught above and read back from the slot, NOT re-read from
     *   the file -- which is the right answer, because by then the file's bytes
     *   have changed and the process is entitled to the ones it was given.
     *
     * The file case is therefore purely a refinement of MM_FAULT_ANON: same
     * conditions, and the only difference is where the page's contents come
     * from. `vma_file` is the VMA saying it has a backing file. */
    if (vma_file)
        return MM_FAULT_FILE;
    /* A shared segment, on the same footing as the file case and for the same
     * reason: the conditions above have already decided the fault is ours and
     * that the access is permitted; this only says where the bytes live. Last,
     * after the swap marker, so a shared page that somehow reached a swap slot
     * comes back from the slot rather than being silently re-derived -- though
     * it cannot: shm.h's refcount arrangement makes a segment frame fail
     * reclaim's eligibility test, so no swap entry for one can exist. The
     * ordering costs nothing and does not depend on that being true. */
    if (vma_shm)
        return MM_FAULT_SHM;
    return MM_FAULT_ANON;
}

/* ------------------------------------------------------- acting on it --- */

static uint64_t fault_frame(void);

static int do_cow(uint64_t cr3, uint64_t page, uint64_t *pte, int active)
{
    uint64_t e = *pte;
    uint64_t old = e & MM_PTE_ADDR;
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
        /* MM_PTE_FLAGS, not 0xFFF. The permissions carried onto the copy
         * include NX, which lives in bit 63; with `e & 0xFFF` a copy-on-write
         * copy of a no-execute page came back EXECUTABLE, so every forked
         * process would get its stack's execute permission back the first time
         * it wrote to it. That is not a crash, it is the protection quietly
         * coming undone, which is why it is spelled out here. */
        *pte = nf | ((e & MM_PTE_FLAGS) & ~VMM_PTE_COW) | PRESENT | WRITABLE;
        /* This PTE now points somewhere else, so the reverse map has to be told
         * about BOTH ends of the move. Editing the PTE by hand here rather than
         * going through vmm.c's set_leaf() is why: a page-fault handler wants
         * the entry it already has a pointer to, not another four-level walk.
         * The price is these two lines, and rmap_audit() is what proves they
         * are not forgotten. Note (e & MM_PTE_FLAGS) carries VMM_PTE_ANON
         * across, so a copy of an anonymous page is still reclaimable by the
         * drop tier. */
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
    if (reclaim_emergency(64)) f = pmm_alloc();
    /* WHY THE KILLER IS NOT CALLED FROM HERE, one frame short of it.
     *
     * The obvious place to ask for a victim is right on this line, and it is
     * the wrong one: oom_fault_retry() PARKS (it drops the big kernel lock, see
     * oom.c), and do_cow()'s caller is holding a `uint64_t *pte` obtained by
     * walking the page tables before the call. Another thread of the same
     * process can fault on that page while this one is parked, so the entry
     * behind that pointer may be a different entry by the time this returns --
     * and do_cow() would then install its copy over somebody else's, leaking a
     * frame and handing this process a stale page.
     *
     * So this function only RECORDS that the decline was for want of memory,
     * and mm_fault_in() -- which holds no pointer into the tables -- does the
     * kill and re-walks the fault from the top. */
    if (!f) g_oom_decline = 1;
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
    /* NX FROM THE VMA, which is the second of the two gaps c/kernel/exec/exec.c
     * names above setup_cli_stack(): a CLI program's stack is a reservation
     * whose first few pages exec maps eagerly (no-execute) and whose remaining
     * ~250 pages arrive HERE. Mapping those without NX would make the deep part
     * of every stack executable -- and the deep part is exactly where a
     * recursive parser puts its buffers -- so a probe that jumps into a local
     * array would be blocked or not depending on how far down the stack it ran.
     * The reservation already carries the answer: mmap's PROT_EXEC becomes
     * VMA_EXEC (mmsys.c) and exec reserves the stack VMA_READ|VMA_WRITE. Note
     * this is the one place mm ORIGINATES bit 63 rather than carrying it, and
     * it is safe to do unconditionally: with EFER.NXE clear bit 63 would be a
     * reserved bit, but long.asm/ap_trampoline.asm set NXE before paging on
     * every core, and the host test has no MMU to object. */
    vmm_map_page_in(cr3, page, f,
                    VMM_USER | VMM_PTE_ANON |
                    ((prot & VMA_WRITE) ? VMM_WRITABLE : 0) |
                    ((prot & VMA_EXEC)  ? 0 : MM_PTE_NX));
    if (active) mm_invlpg(page);
    g_anon++;
    return 1;
}

/* THE PAGE CACHE'S FAULT. Beside do_anon(), and deliberately the same shape:
 * a page that is legitimately absent is made present, and the ONLY difference
 * is where its bytes come from -- zeroes there, a file here.
 *
 * The one thing worth reading slowly is the reference. pcache_get() hands back
 * a frame the CACHE holds a reference on and does not take one for us; this PTE
 * is a second, independent reference, so pmm_ref() before the mapping. Getting
 * that wrong in either direction is exactly the class of bug rmap.h is written
 * around: one too few and the frame is freed while this PTE still points at it,
 * one too many and reclaim can never take the page back because refcount and
 * rmap count stop agreeing.
 *
 * READ-ONLY, ALWAYS. VMA_WRITE never reaches this PTE, and it cannot: a
 * writable file page would be a dirty page, and there is no writeback in this
 * line (see pcache.h). mmsys.c refuses a writable file mapping OUT LOUD rather
 * than quietly handing back a private copy, so a write fault here is a genuine
 * protection fault and stays one -- the classifier declines it and the process
 * dies, which is what a write to a read-only mapping is supposed to do. */
static int do_file(uint64_t cr3, uint64_t page, int fh, uint64_t index,
                   uint32_t prot, int active)
{
    /* ONE PAGE IS RETURNED AND SEVERAL MAY HAVE ARRIVED. pcache_get() runs
     * readahead underneath this call (pcache.h's READAHEAD block): if the
     * previous request on this file was for page index-1, it fetches a run of
     * pages in one backend read and installs them, then hands back this one.
     * Nothing here changes -- the extra pages are in the cache with no PTE, so
     * the next fault on them is a HIT and this function maps them one at a
     * time exactly as before.
     *
     * It is written down here because this is where a reader stands when they
     * ask why one fault caused a 132 KiB device read, and because the frames
     * those pages hold are charged to nobody visible from this file: they are
     * rmap 0 / pcache_holds 1 / refcount 1, which reclaim.h's three-term test
     * makes the cheapest eligible page on the machine rather than a leak. */
    uint64_t f = pcache_get(fh, index);
    if (!f)
        return 0;                   /* unreadable, past EOF, or out of memory:
                                     * decline, and the process dies as it would
                                     * for any other address it may not have */

#ifdef MM_NEGCTL_NOSHARE
    /* THE NEGATIVE CONTROL FOR THE SHARING GATE (tests/boot/run-execshare-test.sh).
     *
     * It is the PLAUSIBLE WRONG IMPLEMENTATION, not the feature switched off,
     * and the distinction is the whole reason it is written this way. The page
     * is still fetched THROUGH THE PAGE CACHE, so the read is still
     * demand-paged -- a page nothing touches is still never read off the disk,
     * and the second process still finds the page already resident and pays no
     * device round trip. What changes is one thing only: the process is given
     * a PRIVATE COPY of the cached frame instead of a second reference to it.
     *
     * That is the mistake a reader is most likely to make here, because it is
     * the shape do_cow() twenty lines above uses and it looks safe: every byte
     * the process sees is identical, the permissions are identical, W^X is
     * intact, and nothing can observe the difference from inside ring 3. Every
     * functional assertion in this tree still passes against it. The ONLY
     * thing it loses is the memory, which is the entire point of the feature --
     * so a gate that cannot see this control is a gate measuring "the loader
     * still works" while claiming to measure sharing.
     *
     * The PTE keeps VMM_PTE_FILE, which is the honest encoding rather than a
     * second deliberate error: the page really is file-backed and really is
     * re-derivable by reading the file again. The frame simply is not the
     * cache's, so pcache_holds() is false for it and reclaim's tier-1 cached
     * drop cannot take it -- a real consequence of not sharing, not an extra
     * fault injected to make the control easier to detect.
     *
     * Note what it does NOT break, because the gate has to be built around it:
     * a fork()ed child still shares these frames with its parent through
     * vmm_clone_user's read-only branch. Only two INDEPENDENT execs of the same
     * binary stop sharing. So the gate must launch the same program twice, not
     * fork one process in two. */
    uint64_t priv = pmm_alloc();
    if (!priv) return 0;
    memcpy(mm_p2v(priv), mm_p2v(f), 4096);
    vmm_map_page_in(cr3, page, priv,
                    VMM_USER | VMM_PTE_FILE |
                    ((prot & VMA_EXEC) ? 0 : MM_PTE_NX));
    if (active) mm_invlpg(page);
    g_file++;
    return 1;
#else
    if (pmm_ref(f) < 0)
        return 0;                   /* saturated refcount: refuse to share it,
                                     * exactly as the copy-on-write clone does */
    vmm_map_page_in(cr3, page, f,
                    VMM_USER | VMM_PTE_FILE |
                    ((prot & VMA_EXEC) ? 0 : MM_PTE_NX));
#endif
    if (active) mm_invlpg(page);
    g_file++;
    return 1;
}

/* A SHARED SEGMENT'S FAULT. Beside do_file(), deliberately the same shape, and
 * the only difference is that the backing object is a c/kernel/mm/shm.h segment
 * instead of the page cache -- so there is no device read, no miss, and no way
 * for this to fail for want of memory: shm_create() allocated every frame up
 * front (shm.h says why the fault path may not allocate).
 *
 * THE REFERENCE IS THE WHOLE OF IT, and it is do_file()'s sentence word for
 * word because it is the same sentence: shm_frame() hands back a frame the
 * SEGMENT holds a reference on and does not take one for us; this PTE is a
 * second, independent reference, so pmm_ref() before the mapping. One too few
 * and the frame is freed while this PTE still points at it; one too many and
 * the arithmetic reclaim uses stops meaning anything.
 *
 * WRITABLE, unlike do_file(), and that is the feature rather than an oversight.
 * A file page may not be written because there is no writeback; a segment page
 * may, because the frame IS the storage and a write is exactly what the other
 * process is waiting to see. The permission comes from the VMA, so a segment
 * mapped read-only stays read-only.
 *
 * NEITHER ANON NOR FILE IS SET, and vmm.h's VMM_PTE_SHM comment argues why at
 * length: both of those bits promise reclaim that the page can be re-derived by
 * faulting it again, and both re-derivations produce a PRIVATE frame. Marking a
 * shared page ANON would make it droppable the moment it was all zeroes -- which
 * is what a segment looks like before its first write. */
static int do_shm(uint64_t cr3, uint64_t page, int sh, uint64_t index,
                  uint32_t prot, int active)
{
    uint64_t f = shm_frame(sh, index);
    if (!f)
        return 0;                   /* past the end of the segment, or the
                                     * segment is gone: decline, and the process
                                     * dies as it would for any other address it
                                     * may not have */
    if (pmm_ref(f) < 0)
        return 0;                   /* saturated refcount. Refuse rather than
                                     * copy: for a shared page the sharing IS
                                     * the semantics, so a private copy would be
                                     * a silent wrong answer (vmm.c's clone
                                     * makes the identical argument). */
    vmm_map_page_in(cr3, page, f,
                    VMM_USER | VMM_PTE_SHM |
                    ((prot & VMA_WRITE) ? VMM_WRITABLE : 0) |
                    ((prot & VMA_EXEC)  ? 0 : MM_PTE_NX));
    if (active) mm_invlpg(page);
    g_shm++;
    return 1;
}

static int fault_once(uint64_t cr3, uint64_t va, uint64_t err)
{
    if (!cr3) { g_declined++; return 0; }
    uint64_t page = va & ~(uint64_t)0xFFF;
    uint64_t *pte = vmm_pte(cr3, page);
    int present = pte && (*pte & PRESENT);
    int cow     = pte && (*pte & VMM_PTE_COW);
    int user    = pte && (*pte & USER);
    int swapped = pte && vmm_pte_is_swap(*pte);
    int fh = -1;
    uint64_t findex = 0;
    uint32_t fprot = 0;
    int has_file = vma_file_at(cr3, page, &fh, &findex, &fprot);
    int sh = -1;
    uint64_t sindex = 0;
    uint32_t sprot = 0;
    /* Asked only when the area is not file-backed. The two are mutually
     * exclusive (vma.h), so a second scan when the first already answered would
     * be a table walk under vma_lock that cannot change the outcome -- and the
     * fault path is the one place in this file where that cost is paid on every
     * page of every process. */
    int has_shm = has_file ? 0 : vma_shm_at(cr3, page, &sh, &sindex, &sprot);
    uint32_t prot = has_file ? fprot : (has_shm ? sprot : vma_prot_at(cr3, page));
    int active = ((mm_read_cr3() & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR));

    uint64_t t0 = fault_cyc();
    switch (mm_fault_classify(va, err, present, cow, user, (int)prot, swapped,
                              has_file, has_shm)) {
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
    case MM_FAULT_FILE:
        /* Timed separately from anon for the same reason swap-in is: a cache
         * HIT is a pmm_ref and a PTE write, a cache MISS is a device read, and
         * an average over both says nothing about either. The split between
         * them is pcache_hits()/pcache_misses(), reported next to this. */
        if (do_file(cr3, page, fh, findex, prot, active)) {
            g_cyc_file += fault_cyc() - t0;
            return 1;
        }
        break;
    case MM_FAULT_SHM:
        /* Timed separately from the file case for the reason that one is timed
         * separately from anon: this path has no device read and no allocation
         * at all, so averaging it into either would hide that a shared fault is
         * the cheapest non-trivial fault on the machine. */
        if (do_shm(cr3, page, sh, sindex, prot, active)) {
            g_cyc_shm += fault_cyc() - t0;
            return 1;
        }
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

/* THE ONE LINE THIS WHOLE FILE USED TO GET WRONG.
 *
 * fault_once() returning 0 ends a process. Until the killer existed there was
 * nothing else to do about it, and the consequence was recorded honestly in
 * do_cow() -- "the process dies, the kernel does not" -- but the process that
 * died was whoever touched memory NEXT, which on a machine one program has
 * emptied is almost never that program. It is the shell, or the app the user
 * just opened, or the compositor's client.
 *
 * So a decline FOR WANT OF MEMORY (and only that: g_oom_decline, set at the one
 * allocation site) now gets one more chance. oom_kill() either takes back the
 * memory of a process that has already exited, or marks the largest eligible
 * live one, and this retries the fault FROM THE TOP -- a second full walk of
 * the page tables, because oom_fault_retry() can park and drop the big kernel
 * lock, so nothing read before it may be reused after it. The retry is why the
 * body above is a separate function.
 *
 * ONE retry, not a loop. If the fault fails again the machine is out of memory
 * in a way one kill did not fix, and spinning here would hold this process in
 * the kernel while it happened. The behaviour then is exactly what it was
 * before this existed: decline, and the process dies. */
int mm_fault_in(uint64_t cr3, uint64_t va, uint64_t err)
{
    g_oom_decline = 0;
    int r = fault_once(cr3, va, err);
    if (r || !g_oom_decline) return r;

    if (!oom_fault_retry) return 0;     /* no killer linked: the old behaviour */
    g_oom_retry++;
    if (!oom_fault_retry()) return 0;   /* we were the victim, or nothing helped */

    g_oom_decline = 0;
    r = fault_once(cr3, va, err);
    if (r) g_oom_saved++;
    return r;
}

uint64_t mm_oom_retries(void) { return g_oom_retry; }
uint64_t mm_oom_saved(void)   { return g_oom_saved; }

uint64_t mm_swapin_faults(void) { return g_swapin; }
uint64_t mm_file_faults(void)   { return g_file; }
uint64_t mm_shm_faults(void)    { return g_shm; }

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
    return mm_fault_in(mm_read_cr3() & MM_PTE_ADDR, cr2, err);
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
    kprintf("[mm] %s: file faults %d (%d kcycles total / %d each)\n",
            tag ? tag : "-", (int)g_file, (int)(g_cyc_file / 1000),
            (int)(g_file ? g_cyc_file / g_file : 0));
    kprintf("[mm] %s: shm faults %d (%d kcycles total / %d each)\n",
            tag ? tag : "-", (int)g_shm, (int)(g_cyc_shm / 1000),
            (int)(g_shm ? g_cyc_shm / g_shm : 0));
    pcache_report(tag);
    shm_report(tag);
    reclaim_report(tag);
}
