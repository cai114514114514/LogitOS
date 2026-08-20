/* Host test for mprotect: c/kernel/mm/vma.c vma_protect() + c/kernel/mm/vmm.c
 * vmm_protect_range_in(), compiled -DMM_HOSTTEST alongside the rest of
 * c/kernel/mm over a simulated physical memory.
 *
 * WHY IT NEEDS ITS OWN FILE. mprotect is two halves that fail in different
 * ways, and every interesting bug in it is SILENT:
 *
 *  1. A PROTECTION THAT DID NOT TAKE. Taking write away from a page and having
 *     the write still succeed is not a crash and not a wrong number -- it is a
 *     program that keeps working until the day something depended on the page
 *     being read-only. The one way it happens here is specific and is the
 *     negative control below: mm_fault_classify()'s present-page branch answers
 *     MM_FAULT_COW on the PTE's copy-on-write bit ALONE and never consults the
 *     VMA, so a copy-on-write page that keeps its marker across an mprotect
 *     serves the very write the caller just forbade, by copying it.
 *
 *  2. A FRAME NOBODY CAN SEE ANY MORE. A resident page made PROT_NONE cannot
 *     stay present (there is no P=1 encoding that refuses a ring-3 READ), and
 *     it cannot be thrown away (a later mprotect back must return the same
 *     bytes). So it becomes VMM_PTE_NOACCESS: not present, frame retained,
 *     still referenced -- and therefore invisible to every loop in vmm.c that
 *     decides what to free by testing (PRESENT|USER). munmap, exit and fork
 *     each had to learn about it, and each of them failing is a leak or a
 *     use-after-free that shows up somewhere else, later.
 *
 *  3. A HALF-APPLIED CALL. vma_protect refuses an unreserved page, a writable
 *     file area and a split it has no slot for -- all three BEFORE mutating
 *     anything. "It failed and changed nothing" is asserted by reading the
 *     protections back, not inferred from the return code.
 *
 * The contents assertions are byte patterns that depend on BOTH the page and
 * the offset within it, for the reason mm_reclaim_test.c argues: zeroes would
 * pass a wrong-page read and a per-page constant would pass a wrong-offset one.
 *
 * Run: sh tests/unit/mm_run.sh   (Makefile: test-mm) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "mm.h"
#include "rmap.h"
#include "pcache.h"
#include "mmhost.h"      /* mm_host_cr3: the simulated active address space */
#include "kprintf.h"     /* mm_log_quiet: swallow the audit's own output */

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

#define PF_P 0x01
#define PF_W 0x02
#define PF_U 0x04

static void phase(const char *s) { printf("-- %s\n", s); }

static uint64_t pte_of(uint64_t cr3, uint64_t va)
{
    uint64_t *p = vmm_pte(cr3, va);
    return p ? *p : 0;
}

static uint8_t pat(uint64_t va, int off)
{
    return (uint8_t)(((va >> 12) * 61 + off * 7 + (off >> 6) * 29 + 0x33) & 0xFF);
}

static void fill(uint64_t cr3, uint64_t va)
{
    uint8_t *p = mm_sim_ptr(pte_of(cr3, va) & MM_PTE_ADDR);
    for (int i = 0; i < 4096; i++) p[i] = pat(va, i);
}

static int holds(uint64_t cr3, uint64_t va)
{
    uint64_t e = pte_of(cr3, va);
    if (!(e & MM_PTE_ADDR)) return 0;
    const uint8_t *p = mm_sim_ptr(e & MM_PTE_ADDR);
    for (int i = 0; i < 4096; i++) if (p[i] != pat(va, i)) return 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * 1. vma_protect's arithmetic: splitting, coverage, and the refusals.
 * ------------------------------------------------------------------------ */
static void test_vma_arith(void)
{
    phase("vma_protect: splits at the edges, and three refusals that change nothing");

    uint64_t sp = vmm_new_space();
    mm_host_cr3 = sp;

    uint64_t len = 16 * 4096;
    uint64_t a = vma_reserve(sp, 0, len, VMA_READ | VMA_WRITE);
    mm_ok(a != 0, "a 16-page reservation");
    mm_eqi(vma_count(sp), 1, "one area to start with");

    /* --- the middle, which is the case that needs BOTH splits ------------ */
    mm_eqi(vma_protect(sp, a + 4 * 4096, 4 * 4096, VMA_READ), 0, "protect four pages out of the middle");
    mm_eqi(vma_count(sp), 3, "one area became three");
    mm_eqi(vma_prot_at(sp, a),                VMA_READ | VMA_WRITE, "the part before is untouched");
    mm_eqi(vma_prot_at(sp, a + 4 * 4096),     VMA_READ,             "the protected part changed");
    mm_eqi(vma_prot_at(sp, a + 7 * 4096),     VMA_READ,             "...to its last page");
    mm_eqi(vma_prot_at(sp, a + 8 * 4096),     VMA_READ | VMA_WRITE, "the part after is untouched");
    mm_eqi(vma_reserved_bytes(sp), len, "and not one byte stopped being reserved");

    /* --- PROT_NONE is a REAL value here, unlike in vma_reserve ----------- */
    mm_eqi(vma_protect(sp, a + 4 * 4096, 4096, 0), 0, "PROT_NONE is accepted");
    mm_eqi(vma_prot_at(sp, a + 4 * 4096), 0, "and stored as 0, NOT floored to VMA_READ");
    mm_eqi(vma_reserved_bytes(sp), len,
           "a PROT_NONE page is still RESERVED -- which is the whole difference "
           "between this and munmap");
    /* The reservation is what stops the guard being handed out again. */
    uint64_t other = vma_reserve(sp, a + 4 * 4096, 4096, VMA_READ | VMA_WRITE);
    mm_ok(other != a + 4 * 4096,
          "a later reservation hinting at the guard page is placed ELSEWHERE (%p)",
          (void *)other);
    mm_eqi(vma_release(sp, other, 4096), 0, "(clean that one up)");

    /* --- refusal 1: a hole in the range ---------------------------------- */
    {
        uint64_t b = vma_reserve(sp, 0, 4 * 4096, VMA_READ | VMA_WRITE);
        mm_ok(b != 0, "a second, separate reservation");
        /* A range starting inside `b` and running past its end. Whatever is
         * beyond it is not reserved by this space. */
        uint32_t was = vma_prot_at(sp, b);
        mm_eqi(vma_protect(sp, b, 4 * 4096 + 4096, VMA_READ), VMA_E_NOMEM,
               "a range that runs off the end of a mapping is REFUSED");
        mm_eqi(vma_prot_at(sp, b), was,
               "...and the pages that WERE mapped are unchanged -- the refusal is "
               "all-or-nothing, not 'as far as it got'");
        mm_eqi(vma_release(sp, b, 4 * 4096), 0, "(clean up)");
    }

    /* --- refusal 2: a range outside the user region ---------------------- */
    mm_eqi(vma_protect(sp, 0x1000, 4096, VMA_READ), VMA_E_RANGE, "a low wild address is refused");
    mm_eqi(vma_protect(sp, a, 0, VMA_READ), VMA_E_RANGE, "a zero length is refused");
    mm_eqi(vma_protect(sp, ~(uint64_t)0 - 8, 4096, VMA_READ), VMA_E_RANGE,
           "a length that wraps the address space is refused");


    /* --- refusal 3: out of slots ----------------------------------------- */
    {
        /* Fill the table, then ask for a protect that needs a split. */
        int made = 0;
        while (vma_count(sp) < VMA_MAXAREA) {
            if (!vma_reserve(sp, 0, 4096, VMA_READ | VMA_WRITE)) break;
            made++;
        }
        mm_eqf(vma_count(sp), VMA_MAXAREA, "the area table is full (%d made)", made);
        uint32_t was = vma_prot_at(sp, a);
        mm_eqi(vma_protect(sp, a + 4096, 4096, VMA_READ), VMA_E_NOMEM,
               "a protect that needs a split it cannot have is REFUSED");
        mm_eqi(vma_prot_at(sp, a), was, "...having changed nothing");
        mm_eqi(vma_count(sp), VMA_MAXAREA, "...and having created no area");
    }

    mm_host_cr3 = 0;
    vmm_free_space(sp);
}

/* ------------------------------------------------------------------------
 * 1b. A FILE-BACKED area may never be made writable, and the refusal has to
 *     belong to the MECHANISM rather than to the one caller that remembers it
 *     -- the same argument vma_reserve_file_fixed() makes about itself. There
 *     is no writeback anywhere in this kernel, so a writable file PTE is a
 *     dirty page nothing can ever clean.
 * ------------------------------------------------------------------------ */
#define SIMPAGES 8
static uint8_t simfile[SIMPAGES][4096];

static int sim_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{
    if (strcmp(path, "/t/ro") != 0) return -1;
    *dev = 3; *ino = 9; *size = (uint64_t)SIMPAGES * 4096;
    return 0;
}
static long sim_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    if (strcmp(path, "/t/ro") != 0) return -1;
    uint64_t page = off / 4096;
    if (page >= SIMPAGES) return 0;
    if (len > 4096) len = 4096;
    memcpy(dst, simfile[page], (size_t)len);
    return (long)len;
}
static const struct pcache_ops sim_ops = { sim_stat, sim_read, 0 };

static void test_file_area(void)
{
    phase("a file-backed area: read-only protections change, writable is refused");

    for (int p = 0; p < SIMPAGES; p++)
        for (int i = 0; i < 4096; i++)
            simfile[p][i] = (uint8_t)((p * 61 + i * 7 + 0x33) & 0xFF);
    /* The cache has to exist before a handle can be opened in it; it is init'd
     * here rather than in main() because this is the only phase that uses it. */
    pcache_init(pmm_total_frames());
    pcache_set_ops(&sim_ops);

    uint64_t sp = vmm_new_space();
    mm_host_cr3 = sp;

    int fh = pcache_file_open("/t/ro");
    mm_ok(fh >= 0, "the simulated file is cacheable");
    uint64_t a = vma_reserve_file(sp, 0, 4 * 4096, VMA_READ, fh, 0);
    mm_ok(a != 0, "a four-page file-backed reservation");
    pcache_file_put(fh);            /* the AREA owns the reference now */

    mm_eqi(mm_fault_in(sp, a + 0x20, PF_U), 1, "a page faults in from the file");
    mm_ok(pte_of(sp, a) & VMM_PTE_FILE, "and its PTE says it is a page-cache page");

    /* THE REFUSAL. */
    mm_eqi(vma_protect(sp, a, 4096, VMA_READ | VMA_WRITE), VMA_E_ACCES,
           "making a file-backed page WRITABLE is refused OUT LOUD");
    mm_eqi(vma_prot_at(sp, a), VMA_READ, "...and its protection did not change");
    mm_ok(!(pte_of(sp, a) & WRITABLE), "...and neither did its PTE");

    /* A read-only change IS allowed, and must keep the page a cache page --
     * lose VMM_PTE_FILE and reclaim can neither drop it nor swap it, and the
     * frame becomes permanently unreclaimable. */
    mm_eqi(vma_protect(sp, a, 4096, VMA_READ | VMA_EXEC), 0, "read+execute is allowed");
    mm_eqi(vmm_protect_range_in(sp, a, 4096, VMA_READ | VMA_EXEC), 1, "the PTE was rewritten");
    mm_ok(pte_of(sp, a) & VMM_PTE_FILE, "and it is STILL marked as a page-cache page");
    /* !! ON A BIT-63 TEST, and it is not decoration: mm_ok takes an `int`, so
     * `pte & MM_PTE_NX` -- which is 1<<63 -- truncates to 0 and the assertion
     * reads FALSE whatever the PTE says. The negated form below happened to be
     * safe (`!` already yields 0/1) and the positive one was not, which is how
     * a correct kernel produced one failing line here. */
    mm_ok(!(pte_of(sp, a) & MM_PTE_NX), "and it is executable now");
    mm_eqi(vma_protect(sp, a, 4096, VMA_READ), 0, "back to read-only");
    vmm_protect_range_in(sp, a, 4096, VMA_READ);
    mm_ok(!!(pte_of(sp, a) & MM_PTE_NX), "and no-execute came back");
    mm_ok(pte_of(sp, a) & VMM_PTE_FILE, "still a page-cache page");

    mm_host_cr3 = 0;
    vmm_free_space(sp);
    mm_eqi(pcache_audit(), 0, "every cache entry still names an allocated frame");
    pcache_set_ops(0);
}

/* ------------------------------------------------------------------------
 * 2. An UNTOUCHED PROT_NONE page: the guard page, which is the only case
 *    pthread_create actually produces. Nothing is resident, so this is
 *    entirely about what the fault path does with a prot of 0.
 * ------------------------------------------------------------------------ */
static void test_guard(void)
{
    phase("the guard page: an untouched PROT_NONE page faults, its neighbour does not");

    uint64_t sp = vmm_new_space();
    mm_host_cr3 = sp;
    uint64_t free0 = pmm_free_frames();

    uint64_t len = 8 * 4096;
    uint64_t a = vma_reserve(sp, 0, len, VMA_READ | VMA_WRITE);
    mm_eqi(vma_protect(sp, a, 4096, 0), 0, "the lowest page of the mapping is PROT_NONE");

    /* THE ASSERTION THE WHOLE FEATURE EXISTS FOR. */
    mm_eqi(mm_fault_in(sp, a + 0x800, PF_W | PF_U), 0,
           "a write to the guard page is DECLINED -- the process dies here, at "
           "this address, instead of walking on");
    mm_eqi(mm_fault_in(sp, a + 0x800, PF_U), 0, "a READ of it is declined too");
    mm_eqi(pte_of(sp, a), 0, "and nothing was mapped there");
    mm_eqi(pmm_free_frames(), free0, "a guard page costs NO frame at all");

    /* The page above it is ordinary stack. */
    mm_eqi(mm_fault_in(sp, a + 4096 + 0x40, PF_W | PF_U), 1,
           "the page immediately above the guard is filled normally");
    mm_ok(pte_of(sp, a + 4096) & WRITABLE, "and it is writable");

    mm_host_cr3 = 0;
    vmm_free_space(sp);
}

/* ------------------------------------------------------------------------
 * 3. A RESIDENT page made PROT_NONE and brought back. This is where the
 *    VMM_PTE_NOACCESS encoding earns its place -- or fails to.
 * ------------------------------------------------------------------------ */
static void test_resident(void)
{
    phase("a resident page: PROT_NONE keeps the frame AND the bytes");

    uint64_t sp = vmm_new_space();
    mm_host_cr3 = sp;

    uint64_t a = vma_reserve(sp, 0, 4 * 4096, VMA_READ | VMA_WRITE);
    for (int k = 0; k < 4; k++) {
        mm_eqf(mm_fault_in(sp, a + (uint64_t)k * 4096, PF_W | PF_U), 1, "page %d faulted in", k);
        fill(sp, a + (uint64_t)k * 4096);
    }
    uint64_t frame1 = pte_of(sp, a + 4096) & MM_PTE_ADDR;
    unsigned rc1 = pmm_refcount(frame1);
    uint64_t free_before = pmm_free_frames();

    mm_eqi(vma_protect(sp, a + 4096, 4096, 0), 0, "protect page 1 PROT_NONE");
    uint64_t n = vmm_protect_range_in(sp, a + 4096, 4096, 0);
    mm_eqi(n, 1, "one resident PTE was rewritten");

    uint64_t e = pte_of(sp, a + 4096);
    mm_ok(!(e & PRESENT), "the PTE is NOT PRESENT, which is what makes the access fault");
    mm_ok(vmm_pte_is_noaccess(e), "and it is marked NOACCESS");
    mm_ok(!vmm_pte_is_swap(e), "and it can NOT be mistaken for a swap entry");
    mm_eqi((long long)(e & MM_PTE_ADDR), (long long)frame1, "the frame is still in it");
    mm_eqi(pmm_refcount(frame1), rc1, "and still referenced exactly as before");
    mm_eqi(pmm_free_frames(), free_before, "no frame was freed and none allocated");
    mm_eqi(rmap_count(frame1), 0,
           "it has no reverse-map entry, because there is no user mapping -- which "
           "makes it un-evictable, the safe direction (rmap may know FEWER "
           "mappings than pmm counts references, never more)");

    mm_eqi(mm_fault_in(sp, a + 4096 + 0x10, PF_U), 0, "a read of it is declined");
    mm_eqi(mm_fault_in(sp, a + 4096 + 0x10, PF_W | PF_U), 0, "a write of it is declined");

    /* THE ASSERTION THAT SEPARATES THIS FROM "just unmap the page". An
     * implementation that dropped the frame would pass everything above and
     * fail here with a page of zeroes -- silently, since zeroes are what an
     * anonymous fault legitimately produces. */
    mm_eqi(vma_protect(sp, a + 4096, 4096, VMA_READ | VMA_WRITE), 0, "protect it back");
    mm_eqi(vmm_protect_range_in(sp, a + 4096, 4096, VMA_READ | VMA_WRITE), 1, "one PTE back");
    e = pte_of(sp, a + 4096);
    mm_ok(e & PRESENT, "present again");
    mm_ok(e & USER, "and reachable from ring 3");
    mm_ok(e & WRITABLE, "and writable, since this space is the frame's only holder");
    mm_eqi((long long)(e & MM_PTE_ADDR), (long long)frame1, "THE SAME frame");
    mm_ok(holds(sp, a + 4096), "and every byte of it survived the round trip");
    mm_eqi(rmap_count(frame1), 1, "the reverse-map entry came back");

    /* The neighbours were never involved. */
    mm_ok(holds(sp, a), "page 0 is untouched");
    mm_ok(holds(sp, a + 2 * 4096), "page 2 is untouched");

    /* --- and the frame comes back on munmap ------------------------------ */
    mm_eqi(vma_protect(sp, a + 4096, 4096, 0), 0, "PROT_NONE again");
    vmm_protect_range_in(sp, a + 4096, 4096, 0);
    uint64_t f0 = pmm_free_frames();
    mm_eqi(vmm_unmap_range_in(sp, a + 4096, 4096), 1,
           "munmap counts the PROT_NONE page as unmapped");
    mm_eqi(pmm_free_frames(), f0 + 1,
           "AND GIVES ITS FRAME BACK -- a loop that only frees (PRESENT|USER) "
           "PTEs would leak one frame per guard page, invisibly");
    mm_eqi(pte_of(sp, a + 4096), 0, "the PTE is cleared");

    mm_host_cr3 = 0;
    vmm_free_space(sp);
}

/* ------------------------------------------------------------------------
 * 4. Copy-on-write across an mprotect. The negative control's target.
 * ------------------------------------------------------------------------ */
static void test_cow(void)
{
    phase("copy-on-write is RE-DERIVED from the refcount, not carried");

    uint64_t parent = vmm_new_space();
    mm_host_cr3 = parent;

    uint64_t a = vma_reserve(parent, 0, 2 * 4096, VMA_READ | VMA_WRITE);
    mm_eqi(mm_fault_in(parent, a, PF_W | PF_U), 1, "a page in the parent");
    fill(parent, a);
    uint64_t frame = pte_of(parent, a) & MM_PTE_ADDR;

    uint64_t child = vmm_new_space();
    mm_eqi(vmm_clone_user(child, parent), 0, "fork");
    mm_eqi(pmm_refcount(frame), 2, "one frame, two references");
    mm_ok(pte_of(parent, a) & VMM_PTE_COW, "the parent's PTE is copy-on-write");
    mm_ok(!(pte_of(parent, a) & WRITABLE), "and not writable");

    /* --- take write away ------------------------------------------------- */
    mm_eqi(vma_protect(parent, a, 4096, VMA_READ), 0, "parent mprotects it read-only");
    mm_eqi(vmm_protect_range_in(parent, a, 4096, VMA_READ), 1, "the PTE was rewritten");
    mm_ok(!(pte_of(parent, a) & VMM_PTE_COW),
          "the copy-on-write marker is GONE -- it has to be: the classifier's "
          "present-page branch answers MM_FAULT_COW on that bit alone and never "
          "looks at the VMA");

    /* THE ASSERTION THE CONTROL BREAKS. */
    mm_eqi(mm_fault_in(parent, a, PF_P | PF_W | PF_U), 0,
           "a WRITE to the now-read-only page is DECLINED, not served by copying");
    mm_eqi(pmm_refcount(frame), 2, "and nothing was copied");
    mm_ok(holds(parent, a), "the parent's bytes are still its bytes");

    /* The child is unaffected: its own PTE still says copy-on-write. */
    mm_ok(pte_of(child, a) & VMM_PTE_COW, "the child's PTE is untouched");
    mm_eqi(mm_fault_in(child, a, PF_P | PF_W | PF_U), 1, "and the CHILD may still write");
    mm_eqi(pmm_refcount(frame), 1, "which copied, leaving the parent sole owner");

    /* --- give write back, now that the parent is the only holder --------- */
    mm_eqi(vma_protect(parent, a, 4096, VMA_READ | VMA_WRITE), 0, "parent mprotects it writable");
    mm_eqi(vmm_protect_range_in(parent, a, 4096, VMA_READ | VMA_WRITE), 1, "the PTE was rewritten");
    mm_ok(pte_of(parent, a) & WRITABLE,
          "and it is WRITABLE outright, not copy-on-write, because refcount is 1");
    mm_ok(!(pte_of(parent, a) & VMM_PTE_COW), "no marker");
    mm_ok(holds(parent, a), "still its bytes");

    mm_host_cr3 = 0;
    vmm_free_space(child);
    vmm_free_space(parent);
}

/* ------------------------------------------------------------------------
 * 5. A PROT_NONE page across fork and across exit: the two lifetime paths
 *    that decide what to free by testing (PRESENT|USER) and therefore could
 *    not see this page at all.
 * ------------------------------------------------------------------------ */
static void test_lifetime(void)
{
    phase("a PROT_NONE page survives fork and is released at exit");

    uint64_t free0 = pmm_free_frames();
    uint64_t parent = vmm_new_space();
    mm_host_cr3 = parent;

    uint64_t a = vma_reserve(parent, 0, 2 * 4096, VMA_READ | VMA_WRITE);
    mm_eqi(mm_fault_in(parent, a, PF_W | PF_U), 1, "a resident page");
    fill(parent, a);
    uint64_t frame = pte_of(parent, a) & MM_PTE_ADDR;
    mm_eqi(vma_protect(parent, a, 4096, 0), 0, "made PROT_NONE");
    vmm_protect_range_in(parent, a, 4096, 0);

    uint64_t child = vmm_new_space();
    mm_eqi(vmm_clone_user(child, parent), 0, "fork");
    mm_ok(vmm_pte_is_noaccess(pte_of(child, a)),
          "THE CHILD INHERITED THE GUARD -- skipping this case would drop the "
          "page from the child and leak the parent's reference");
    mm_eqi((long long)(pte_of(child, a) & MM_PTE_ADDR), (long long)frame, "the same frame");
    mm_eqi(pmm_refcount(frame), 2, "and exactly one more reference was taken");
    mm_eqi(mm_fault_in(child, a, PF_W | PF_U), 0, "it is a guard in the child too");

    /* The child re-protects it writable while the frame is still shared. */
    mm_eqi(vma_protect(child, a, 4096, VMA_READ | VMA_WRITE), 0, "the child un-guards it");
    mm_eqi(vmm_protect_range_in(child, a, 4096, VMA_READ | VMA_WRITE), 1, "the PTE came back");
    mm_ok(pte_of(child, a) & VMM_PTE_COW,
          "as COPY-ON-WRITE, not writable -- the frame is still the parent's too, "
          "and setting WRITABLE here would let two processes write one frame");
    mm_ok(!(pte_of(child, a) & WRITABLE), "so the write bit is clear");
    mm_eqi(mm_fault_in(child, a, PF_P | PF_W | PF_U), 1, "the child's first write is served");
    mm_eqi(pmm_refcount(frame), 1, "by copying, leaving the parent sole owner");
    mm_ok(holds(child, a), "and the copy carries the parent's bytes");

    mm_host_cr3 = 0;
    vmm_free_space(child);
    vmm_free_space(parent);
    mm_eqi(pmm_free_frames(), free0,
           "EVERY frame came back at exit, including the one only a NOACCESS PTE "
           "was holding");
    mm_eqi(pmm_bugs(), 0, "no allocator invariant was violated");
    mm_eqi(rmap_bugs(), 0, "the reverse map reported no bugs");
}

int main(void)
{
    mm_sim_init(64);
    /* The "kernel" address space, as boot.asm + kmain leave it: a PML4 whose
     * PDPT entry 1 (the private user region) is empty. vmm_new_space clones it. */
    mm_sim_kernel_space();
    vmm_kernel_cr3();

    test_vma_arith();
    test_file_area();
    test_guard();
    test_resident();
    test_cow();
    test_lifetime();

    mm_log_quiet(1);
    mm_eqi(pmm_audit() + rmap_audit(), 0, "both structures agree at the end");
    mm_log_quiet(0);

    mm_sim_done();
    return mm_summary("mm_protect_test");
}
