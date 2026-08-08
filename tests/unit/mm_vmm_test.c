/* Host test for copy-on-write fork, demand paging and the fault-decision
 * table (c/kernel/mm/{vmm,fault,vma,pmm}.c compiled with -DMM_HOSTTEST over a
 * simulated physical memory -- the real page-table walk, the real refcounting,
 * the real classifier).
 *
 * Three things are being defended, in order of how badly they fail:
 *
 *  1. FAULT CONTAINMENT. mm_fault_classify() decides which faults mm claims.
 *     Every fault it wrongly claims is a bug it silently papers over -- a null
 *     dereference that quietly gets a zero page instead of killing the
 *     process. So the classifier is enumerated exhaustively, and the cases
 *     that MUST still kill are asserted as loudly as the ones that must not.
 *
 *  2. THE REFCOUNT. A frame freed while another space still maps it is
 *     corruption that surfaces later, elsewhere, in unrelated code. Every
 *     transition is asserted by reading the count back, and the free-frame
 *     total is compared against a baseline after every phase and after a long
 *     stress loop, so a one-frame-per-fork error cannot hide.
 *
 *  3. THE DIRTY COW SHAPE. A kernel write to a user page must resolve the
 *     copy-on-write FIRST, and the other holder of the frame must not see the
 *     write. That is asserted directly, on the frame contents.
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
#include "kprintf.h"

/* The classifier grew a seventh argument, `pte_swap`, when reclaim landed: a
 * swapped-out page is not-present for the same reason an untouched mmap page
 * is, and telling them apart is the difference between reading the page back
 * and silently zeroing it. Every case in THIS file is a non-swap case, so it is
 * wrapped rather than having ", 0" appended to forty call sites; the swap cases
 * are enumerated in tests/unit/mm_reclaim_test.c, next to the code that makes
 * the entries. */
#define mm_fault_classify(cr2, err, p, cow, u, prot) \
        mm_fault_classify((cr2), (err), (p), (cow), (u), (prot), 0)

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

#define PF_P 0x01
#define PF_W 0x02
#define PF_U 0x04
#define PF_R 0x08
#define PF_I 0x10

extern uint64_t mm_host_cr3;

/* A process image: a few pages at the base of the user region. */
#define IMG_VA   0x40000000ull
#define IMG_PAGES 24

static void phase(const char *n) { printf("-- %s\n", n); }

static void audit_clean(const char *where)
{
    mm_log_quiet(1);
    int e = pmm_audit();
    mm_log_quiet(0);
    mm_ok(e == 0, "%s: pmm_audit reported %d inconsistencies", where, e);
}

/* Build the "kernel" address space the way boot.asm + kmain leave it: a PML4
 * whose entry 0 points at a PDPT whose entry 1 (the private user region) is
 * empty. vmm_new_space() clones this shape. */
static uint64_t make_kernel_space(void)
{
    uint64_t pml4 = pmm_alloc(), pdpt = pmm_alloc();
    memset(mm_sim_ptr(pml4), 0, 4096);
    memset(mm_sim_ptr(pdpt), 0, 4096);
    ((uint64_t *)mm_sim_ptr(pml4))[0] = pdpt | PRESENT | WRITABLE | USER;
    mm_host_cr3 = pml4;
    return pml4;
}

/* Give `cr3` an image of `n` writable pages at IMG_VA, page k filled with the
 * byte (k+1), exactly as elf_load would. */
static void make_image(uint64_t cr3, int n)
{
    for (int k = 0; k < n; k++) {
        uint64_t f = pmm_alloc();
        memset(mm_sim_ptr(f), k + 1, 4096);
        vmm_map_page_in(cr3, IMG_VA + (uint64_t)k * 4096, f, VMM_USER | VMM_WRITABLE);
    }
}

static uint64_t pte_of(uint64_t cr3, uint64_t va)
{
    uint64_t *p = vmm_pte(cr3, va);
    return p ? *p : 0;
}

static int page_is(uint64_t cr3, uint64_t va, int byte)
{
    uint64_t e = pte_of(cr3, va);
    if (!(e & PRESENT)) return 0;
    const uint8_t *p = mm_sim_ptr(e & ~(uint64_t)0xFFF);
    for (int i = 0; i < 4096; i++) if (p[i] != (uint8_t)byte) return 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * 1. The fault-decision table, exhaustively.
 * ------------------------------------------------------------------------ */
static void test_classify(void)
{
    phase("fault classification: which faults are ours, and which must still kill");

    const uint64_t inuser = IMG_VA + 0x1234;

    /* --- the two we claim ------------------------------------------------ */
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W | PF_U, 1, 1, 1, 0), MM_FAULT_COW,
           "ring-3 write to a present copy-on-write page");
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W, 1, 1, 1, 0), MM_FAULT_COW,
           "RING-0 write to a copy-on-write page is claimed too (usercopy)");
    mm_eqi(mm_fault_classify(inuser, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE),
           MM_FAULT_ANON, "write to an untouched anonymous reservation");
    mm_eqi(mm_fault_classify(inuser, PF_U, 0, 0, 0, VMA_READ), MM_FAULT_ANON,
           "read of an untouched anonymous reservation");

    /* --- everything else must fall through to the existing kill path ----- */
    mm_eqi(mm_fault_classify(0, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE), MM_FAULT_NONE,
           "NULL dereference is NOT ours (it must keep killing the process)");
    mm_eqi(mm_fault_classify(0x1000, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE), MM_FAULT_NONE,
           "a low wild pointer is not ours");
    mm_eqi(mm_fault_classify(MM_USER_BASE - 1, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE),
           MM_FAULT_NONE, "one byte below the user region is not ours");
    mm_eqi(mm_fault_classify(MM_USER_END, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE),
           MM_FAULT_NONE, "the first address above the user region is not ours");
    mm_eqi(mm_fault_classify(0xFFFF800000001000ull, PF_W, 1, 1, 1, 0), MM_FAULT_NONE,
           "a kernel address is never ours, even if it looks copy-on-write");

    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W | PF_U | PF_R, 1, 1, 1, 0), MM_FAULT_NONE,
           "a reserved-bit fault is a malformed page table, never handled");

    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W | PF_U, 1, 0, 1, 0), MM_FAULT_NONE,
           "write to a genuinely read-only present page still kills");
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_U, 1, 1, 1, 0), MM_FAULT_NONE,
           "a READ fault on a present page is not a copy-on-write fault");
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_U | PF_I, 1, 1, 1, 0), MM_FAULT_NONE,
           "an instruction fetch fault is not a copy-on-write fault");
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W | PF_U, 1, 1, 0, 0), MM_FAULT_NONE,
           "a kernel-only page (no USER in the PTE) is never unshared for ring 3");
    mm_eqi(mm_fault_classify(inuser, PF_P | PF_W | PF_U, 0, 1, 1, 0), MM_FAULT_NONE,
           "error code says present, page table says absent: refuse");

    mm_eqi(mm_fault_classify(inuser, PF_W | PF_U, 0, 0, 0, 0), MM_FAULT_NONE,
           "absent page with NO reservation still kills (stack overflow, wild pointer)");
    mm_eqi(mm_fault_classify(inuser, PF_W | PF_U, 0, 0, 0, VMA_READ), MM_FAULT_NONE,
           "write to a read-only reservation still kills");
    mm_eqi(mm_fault_classify(inuser, PF_U, 1, 0, 1, VMA_READ), MM_FAULT_NONE,
           "absent-page error code with a present PTE: refuse");

    /* The whole 5-bit error-code space against a plain present writable page
     * that is NOT copy-on-write: nothing may be claimed. */
    for (unsigned err = 0; err < 32; err++) {
        int k = mm_fault_classify(inuser, err, 1, 0, 1, 0);
        mm_ok(k == MM_FAULT_NONE,
              "err=%02x on a present non-COW page must not be claimed (got %d)", err, k);
    }
}

/* ------------------------------------------------------------------------
 * 2. Copy-on-write fork.
 * ------------------------------------------------------------------------ */
static void test_cow(void)
{
    phase("copy-on-write fork: nothing is copied until somebody writes");
    mm_set_cow(1);

    uint64_t free0 = pmm_free_frames();
    uint64_t parent = vmm_new_space();
    make_image(parent, IMG_PAGES);
    mm_host_cr3 = parent;
    uint64_t after_img = pmm_free_frames();
    mm_ok(free0 - after_img >= IMG_PAGES, "the image cost at least its own pages");

    uint64_t child = vmm_new_space();
    uint64_t before_fork = pmm_free_frames();
    mm_eqi(vmm_clone_user(child, parent), 0, "clone succeeded");

    uint64_t shared = 0, copied = 0;
    vmm_clone_stats(&shared, &copied);
    mm_eqi(shared, IMG_PAGES, "every data page was shared");
    mm_eqi(copied, 0, "NO data page was copied");

    uint64_t after_fork = pmm_free_frames();
    /* The only frames a copy-on-write fork may consume are the child's page
     * tables (one PD + one PT for a 24-page image), never data. */
    mm_ok(before_fork - after_fork <= 2,
          "fork of a %d-page image consumed %d frames (page tables only)",
          IMG_PAGES, (int)(before_fork - after_fork));

    for (int k = 0; k < IMG_PAGES; k++) {
        uint64_t va = IMG_VA + (uint64_t)k * 4096;
        uint64_t pe = pte_of(parent, va), ce = pte_of(child, va);
        mm_eqi(pe & ~(uint64_t)0xFFF, ce & ~(uint64_t)0xFFF, "both spaces map the same frame");
        mm_ok(!(pe & WRITABLE), "parent PTE lost write permission");
        mm_ok(!(ce & WRITABLE), "child PTE is read-only");
        mm_ok(pe & VMM_PTE_COW, "parent PTE is marked copy-on-write");
        mm_ok(ce & VMM_PTE_COW, "child PTE is marked copy-on-write");
        mm_eqi(pmm_refcount(pe & ~(uint64_t)0xFFF), 2, "the frame is referenced twice");
        mm_ok(page_is(child, va, k + 1), "the child sees the parent's data");
    }
    mm_eqi(mm_cow_pages(), 2 * IMG_PAGES, "both sides' PTEs are counted as copy-on-write");
    audit_clean("after fork");

    /* --- the child writes page 3 ---------------------------------------- */
    uint64_t va3 = IMG_VA + 3 * 4096;
    uint64_t frame_before = pte_of(child, va3) & ~(uint64_t)0xFFF;
    uint64_t freeb = pmm_free_frames();
    mm_host_cr3 = child;
    mm_eqi(mm_fault_in(child, va3 + 0x40, PF_P | PF_W | PF_U), 1, "the write fault was handled");
    mm_eqi(pmm_free_frames(), freeb - 1, "resolving it cost exactly one frame");

    uint64_t frame_after = pte_of(child, va3) & ~(uint64_t)0xFFF;
    mm_ok(frame_after != frame_before, "the child got a different frame");
    mm_ok(pte_of(child, va3) & WRITABLE, "the child's page is writable now");
    mm_ok(!(pte_of(child, va3) & VMM_PTE_COW), "and no longer copy-on-write");
    mm_ok(page_is(child, va3, 4), "the copy has the original contents");
    mm_eqi(pmm_refcount(frame_before), 1, "the parent is now the sole owner");
    mm_ok(pte_of(parent, va3) & VMM_PTE_COW, "the parent's PTE is still copy-on-write");
    mm_ok(!(pte_of(parent, va3) & WRITABLE), "...and still read-only until it writes");

    /* The child now really writes; the parent must not see it. */
    memset(mm_sim_ptr(frame_after), 0xEE, 4096);
    mm_ok(page_is(parent, va3, 4), "the parent's page is UNCHANGED by the child's write");

    /* --- the parent writes the same page: sole owner, no copy ----------- */
    freeb = pmm_free_frames();
    uint64_t copies_before = mm_cow_faults(), reuse_before = mm_cow_reuse();
    mm_host_cr3 = parent;
    mm_eqi(mm_fault_in(parent, va3, PF_P | PF_W | PF_U), 1, "the parent's write fault was handled");
    mm_eqi(pmm_free_frames(), freeb, "the sole owner's fault allocated NOTHING");
    mm_eqi(mm_cow_faults(), copies_before, "no copy was made");
    mm_eqi(mm_cow_reuse(), reuse_before + 1, "it was resolved by re-granting write access");
    mm_ok(pte_of(parent, va3) & WRITABLE, "the parent can write again");
    mm_eqi(pte_of(parent, va3) & ~(uint64_t)0xFFF, frame_before, "on the same frame as before");
    audit_clean("after unsharing");

    /* --- tear both down: everything must come back ---------------------- */
    mm_host_cr3 = 0;
    vmm_free_space(child);
    vmm_free_space(parent);
    mm_eqi(pmm_free_frames(), free0, "both address spaces freed EXACTLY what they took");
    mm_eqi(mm_cow_pages(), 0, "no copy-on-write pages are left behind");
    audit_clean("after teardown");
}

/* ------------------------------------------------------------------------
 * 3. The eager path is still correct (and is the negative control).
 * ------------------------------------------------------------------------ */
static void test_eager_control(void)
{
    phase("negative control: with copy-on-write OFF, fork copies every page");
    mm_set_cow(0);

    uint64_t free0 = pmm_free_frames();
    uint64_t parent = vmm_new_space();
    make_image(parent, IMG_PAGES);
    mm_host_cr3 = parent;

    uint64_t child = vmm_new_space();
    uint64_t before = pmm_free_frames();
    mm_eqi(vmm_clone_user(child, parent), 0, "eager clone succeeded");
    uint64_t shared = 0, copied = 0;
    vmm_clone_stats(&shared, &copied);

    /* THIS is the assertion that fails without the change and passes with it,
     * from the two sides: the old kernel could only ever produce these
     * numbers, and the copy-on-write phase above could never produce them. */
    mm_eqi(copied, IMG_PAGES, "eager fork copies every page (the old behaviour)");
    mm_eqi(shared, 0, "eager fork shares nothing");
    mm_ok(before - pmm_free_frames() >= IMG_PAGES,
          "eager fork consumed %d frames for a %d-page image",
          (int)(before - pmm_free_frames()), IMG_PAGES);

    for (int k = 0; k < IMG_PAGES; k++) {
        uint64_t va = IMG_VA + (uint64_t)k * 4096;
        mm_ok((pte_of(parent, va) & ~(uint64_t)0xFFF) != (pte_of(child, va) & ~(uint64_t)0xFFF),
              "eager fork gave the child its own frame");
        mm_ok(page_is(child, va, k + 1), "with the right contents");
        mm_eqi(pmm_refcount(pte_of(child, va) & ~(uint64_t)0xFFF), 1, "each frame has one owner");
    }

    mm_host_cr3 = 0;
    vmm_free_space(child);
    vmm_free_space(parent);
    mm_eqi(pmm_free_frames(), free0, "eager fork's frames all came back");
    audit_clean("eager control");
    mm_set_cow(1);
}

/* ------------------------------------------------------------------------
 * 4. Fork chains: three generations sharing one frame.
 * ------------------------------------------------------------------------ */
static void test_chain(void)
{
    phase("fork chains: a frame shared four ways, released in a shuffled order");
    mm_set_cow(1);
    uint64_t free0 = pmm_free_frames();

    uint64_t s[4];
    s[0] = vmm_new_space();
    make_image(s[0], 4);
    mm_host_cr3 = s[0];
    for (int i = 1; i < 4; i++) {
        s[i] = vmm_new_space();
        mm_ok(vmm_clone_user(s[i], s[i - 1]) == 0, "generation %d forked", i);
    }
    uint64_t f0 = pte_of(s[0], IMG_VA) & ~(uint64_t)0xFFF;
    mm_eqi(pmm_refcount(f0), 4, "one frame, four holders");
    for (int i = 0; i < 4; i++)
        mm_ok(pte_of(s[i], IMG_VA) & VMM_PTE_COW, "generation %d is copy-on-write", i);

    /* Generation 2 writes: 4 -> 3, and it gets its own frame. */
    mm_host_cr3 = s[2];
    mm_eqi(mm_fault_in(s[2], IMG_VA, PF_P | PF_W | PF_U), 1, "generation 2 unshared");
    mm_eqi(pmm_refcount(f0), 3, "the shared frame is down to three");

    int order[4] = { 1, 3, 0, 2 };
    mm_host_cr3 = 0;
    for (int i = 0; i < 4; i++) vmm_free_space(s[order[i]]);
    mm_eqi(pmm_free_frames(), free0, "a shuffled teardown returned every frame");
    mm_eqi(mm_cow_pages(), 0, "and left no copy-on-write pages");
    audit_clean("chain");
}

/* ------------------------------------------------------------------------
 * 5. mmap + demand paging.
 * ------------------------------------------------------------------------ */
static void test_anon(void)
{
    phase("mmap: address space now, frames only when touched");
    uint64_t free0 = pmm_free_frames();
    uint64_t sp = vmm_new_space();
    mm_host_cr3 = sp;

    uint64_t empty = pmm_free_frames();              /* the space itself costs 2 table frames */
    uint64_t len = 4 * 1024 * 1024;                 /* 4 MiB */
    uint64_t a = vma_reserve(sp, 0, len, VMA_READ | VMA_WRITE);
    mm_ok(a >= MM_MMAP_BASE && a + len <= MM_MMAP_TOP, "the reservation is inside the mmap window");
    mm_eqi(pmm_free_frames(), empty, "reserving 4 MiB allocated NOT ONE frame");
    mm_eqi(vma_reserved_bytes(sp), len, "the reservation is recorded");

    /* Touch three pages, spread out. */
    uint64_t touch[3] = { a, a + 512 * 4096, a + len - 4096 };
    for (int i = 0; i < 3; i++) {
        mm_eqi(mm_fault_in(sp, touch[i] + 7, PF_W | PF_U), 1, "first touch was filled");
        uint64_t e = pte_of(sp, touch[i]);
        mm_ok(e & PRESENT, "the page is present now");
        mm_ok(e & WRITABLE, "and writable");
        mm_ok(e & USER, "and reachable from ring 3");
        mm_ok(page_is(sp, touch[i], 0), "anonymous memory reads as ZERO");
    }
    mm_ok(pmm_free_frames() < empty, "touching pages did cost frames");
    mm_ok(empty - pmm_free_frames() < 16,
          "but only %d frames for a 4 MiB region with 3 pages touched",
          (int)(empty - pmm_free_frames()));

    /* An untouched page in the middle is still absent. */
    mm_ok(!(pte_of(sp, a + 4096) & PRESENT), "an untouched page is still not mapped");
    /* ...and reading it faults in a zero page rather than failing. */
    mm_eqi(mm_fault_in(sp, a + 4096, PF_U), 1, "a READ of an untouched page is filled too");

    /* Outside the reservation: still a genuine fault. */
    mm_eqi(mm_fault_in(sp, MM_MMAP_TOP + 0x1000, PF_W | PF_U), 0,
           "a fault outside every reservation is NOT handled");
    mm_eqi(mm_fault_in(sp, 0x8, PF_W | PF_U), 0, "a null dereference is NOT handled");

    /* munmap: the pages that were touched come back, the ones that were not
     * cost nothing to release. */
    uint64_t before = pmm_free_frames();
    uint64_t n = vmm_unmap_range_in(sp, a, len);
    mm_eqi(n, 4, "exactly the four faulted-in pages were unmapped");
    mm_eqi(pmm_free_frames(), before + 4, "and their frames came back");
    mm_eqi(vma_release(sp, a, len), 0, "the reservation was released");
    mm_eqi(vma_reserved_bytes(sp), 0, "nothing is reserved any more");
    mm_eqi(mm_fault_in(sp, a, PF_W | PF_U), 0,
           "after munmap the same address is a genuine fault again");

    mm_host_cr3 = 0;
    vmm_free_space(sp);
    mm_eqi(pmm_free_frames(), free0, "the anonymous space freed everything");
    audit_clean("anon");
}

static void test_vma_arithmetic(void)
{
    phase("address-range arithmetic, including the overflows an mmap ABI dies of");
    uint64_t s, e;
    mm_eqi(vma_range(MM_MMAP_BASE, 0, &s, &e), -1, "zero length is rejected");
    mm_eqi(vma_range(MM_MMAP_BASE, ~(uint64_t)0, &s, &e), -1, "a length of ~0 is rejected");
    mm_eqi(vma_range(~(uint64_t)0 - 16, 4096, &s, &e), -1, "addr + len wrapping is rejected");
    mm_eqi(vma_range(MM_USER_END - 4096, 8192, &s, &e), -1, "a range leaving the user region is rejected");
    mm_eqi(vma_range(MM_USER_BASE - 1, 8, &s, &e), -1, "a range starting below the user region is rejected");
    mm_eqi(vma_range(MM_MMAP_BASE + 1, 1, &s, &e), 0, "an unaligned 1-byte range is accepted");
    mm_eqi(s, MM_MMAP_BASE, "...rounded down to the page");
    mm_eqi(e, MM_MMAP_BASE + 4096, "...and up to the next page");
    mm_eqi(vma_range(MM_MMAP_BASE + 4095, 2, &s, &e), 0, "a range straddling a page boundary");
    mm_eqi(e - s, 8192, "...covers both pages");

    /* Reservations must never overlap, and a hint must never evict. */
    uint64_t sp = vmm_new_space();
    uint64_t a = vma_reserve(sp, 0, 0x10000, VMA_READ | VMA_WRITE);
    uint64_t b = vma_reserve(sp, a, 0x10000, VMA_READ | VMA_WRITE);
    mm_ok(a && b, "two reservations succeeded");
    mm_ok(b >= a + 0x10000 || b + 0x10000 <= a, "a hint at an occupied address did not overlap it");
    mm_eqi(vma_count(sp), 2, "two areas are recorded");
    /* Punch a hole out of the middle of the first: it must split. */
    mm_eqi(vma_release(sp, a + 0x4000, 0x4000), 0, "a hole was punched");
    mm_eqi(vma_count(sp), 3, "the area split in two");
    mm_eqi(vma_prot_at(sp, a + 0x5000), 0, "the hole is no longer reserved");
    mm_ok(vma_prot_at(sp, a) != 0, "the part before the hole is still reserved");
    mm_ok(vma_prot_at(sp, a + 0x9000) != 0, "the part after the hole is still reserved");
    vmm_free_space(sp);
}

/* ------------------------------------------------------------------------
 * 6. The kernel writing user memory (the Dirty COW shape).
 * ------------------------------------------------------------------------ */
static void test_usercopy_shape(void)
{
    phase("kernel writes to a copy-on-write page: resolve first, and the other holder must not see it");
    uint64_t free0 = pmm_free_frames();
    uint64_t parent = vmm_new_space();
    make_image(parent, 4);
    mm_host_cr3 = parent;
    uint64_t child = vmm_new_space();
    vmm_clone_user(child, parent);
    mm_host_cr3 = child;

    uint64_t va = IMG_VA + 4096;                    /* page 1: filled with 0x02 */
    uint64_t shared_frame = pte_of(child, va) & ~(uint64_t)0xFFF;
    mm_eqi(pmm_refcount(shared_frame), 2, "the page is shared");

    /* The pure check must say "not writable" -- that is exactly the trap. */
    mm_eqi(vmm_user_range_ok(child, (void *)(va + 100), 64, 1), 0,
           "the PURE check correctly reports a copy-on-write page as not writable");
    mm_eqi(vmm_user_range_ok(child, (void *)(va + 100), 64, 0), 1,
           "...but readable");

    /* The fault-in variant makes it genuinely writable first. */
    mm_eqi(vmm_user_range_fault_in(child, (void *)(va + 100), 64, 1), 1,
           "fault-in made the range writable");
    mm_eqi(vmm_user_range_ok(child, (void *)(va + 100), 64, 1), 1,
           "and the pure check now agrees");
    mm_ok(pte_of(child, va) & WRITABLE, "the child's own PTE is writable (not a private alias)");
    mm_eqi(pmm_refcount(shared_frame), 1, "the parent is the sole owner of the old frame");

    /* Now the kernel writes, as a syscall would. */
    uint64_t priv = pte_of(child, va) & ~(uint64_t)0xFFF;
    memset((uint8_t *)mm_sim_ptr(priv) + 100, 0x5A, 64);
    const uint8_t *pp = mm_sim_ptr(shared_frame);
    int parent_untouched = 1;
    for (int i = 0; i < 4096; i++) if (pp[i] != 0x02) parent_untouched = 0;
    mm_ok(parent_untouched, "the PARENT's frame is byte-for-byte unchanged (no Dirty COW)");

    /* A range spanning two pages, one of which is still shared. */
    mm_eqi(vmm_user_range_fault_in(child, (void *)(IMG_VA + 4096 - 8), 16, 1), 1,
           "a range straddling two pages resolves both");
    mm_ok(pte_of(child, IMG_VA) & WRITABLE, "the first page was unshared too");

    /* A bad pointer must still be rejected, and must change nothing. */
    uint64_t f_before = pmm_free_frames();
    mm_eqi(vmm_user_range_fault_in(child, (void *)0x1000, 16, 1), 0,
           "a wild kernel-side pointer is still rejected");
    mm_eqi(vmm_user_range_fault_in(child, (void *)(MM_USER_END - 8), 32, 1), 0,
           "a range running off the end of the user region is rejected");
    mm_eqi(pmm_free_frames(), f_before, "and rejecting them allocated nothing");

    mm_host_cr3 = 0;
    vmm_free_space(child);
    vmm_free_space(parent);
    mm_eqi(pmm_free_frames(), free0, "usercopy phase leaked nothing");
    audit_clean("usercopy");
}

/* ------------------------------------------------------------------------
 * 7. Stress: fork / touch / exec / exit in a loop.
 * ------------------------------------------------------------------------ */
static void test_stress(void)
{
    phase("stress: 3000 fork / write / execve / exit rounds");
    uint64_t free0 = pmm_free_frames();
    uint64_t bugs0 = pmm_bugs();
    unsigned rng = 987654321u;
#define NEXT() (rng = rng * 1103515245u + 12345u, (rng >> 16) & 0x7FFF)

    uint64_t parent = vmm_new_space();
    make_image(parent, IMG_PAGES);
    mm_host_cr3 = parent;

    for (int round = 0; round < 3000; round++) {
        uint64_t child = vmm_new_space();
        if (vmm_clone_user(child, parent) < 0) { vmm_free_space(child); continue; }

        /* The child touches a random handful of pages, as a shell would. */
        int n = NEXT() % 6;
        for (int i = 0; i < n; i++) {
            uint64_t va = IMG_VA + (uint64_t)(NEXT() % IMG_PAGES) * 4096;
            mm_host_cr3 = child;
            mm_fault_in(child, va + (NEXT() % 4096), PF_P | PF_W | PF_U);
        }
        /* Half the children execve (free the user subtree, load a new image). */
        if (NEXT() & 1) {
            vmm_free_user(child);
            make_image(child, 3 + (int)(NEXT() % 8));
        }
        mm_host_cr3 = parent;
        vmm_free_space(child);

        /* And the parent occasionally writes, so its own pages unshare too. */
        if ((NEXT() & 3) == 0)
            mm_fault_in(parent, IMG_VA + (uint64_t)(NEXT() % IMG_PAGES) * 4096,
                        PF_P | PF_W | PF_U);
    }

    mm_host_cr3 = 0;
    vmm_free_space(parent);
    mm_eqi(pmm_bugs(), bugs0, "3000 rounds reported no invariant violations");
    mm_eqi(mm_cow_pages(), 0, "no copy-on-write pages survived");
    mm_eqi(pmm_free_frames(), free0,
           "free-frame count returned EXACTLY to baseline after 3000 rounds");
    mm_eqi(pmm_shared_frames(), 0, "no frame is left shared");
    audit_clean("stress");
}

int main(void)
{
    mm_sim_init(64);
    make_kernel_space();
    vmm_kernel_cr3();               /* latch it, as kmain does */

    test_classify();
    test_vma_arithmetic();
    test_cow();
    test_eager_control();
    test_chain();
    test_anon();
    test_usercopy_shape();
    test_stress();

    mm_report("end of test");
    mm_sim_done();
    return mm_summary("mm_vmm_test");
}
