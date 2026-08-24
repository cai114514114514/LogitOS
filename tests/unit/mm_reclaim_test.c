/* Host test for reclaim and swap (c/kernel/mm/{reclaim,swap,rmap,vmm,fault,
 * pmm,vma}.c compiled -DMM_HOSTTEST over a simulated physical memory and a
 * simulated block device -- the real clock, the real eviction, the real slot
 * allocator, the real page-table edits).
 *
 * WHAT IS ACTUALLY BEING DEFENDED HERE
 *
 * Reclaim has one job and two ways to do it catastrophically. It can take a
 * page away from somebody who still needs it -- which is a crash, and at least
 * announces itself. Or it can give the page BACK WRONG: different bytes, no
 * crash, no log line, discovered hours later as a program behaving strangely.
 * The second is the one this file is built around, which is why every page in
 * it carries a pattern that depends on BOTH the page index and the offset
 * inside the page. Zeroes would pass a swapped-in-wrong-slot bug. A constant
 * per page would pass an off-by-one within the page. A pattern that is a
 * function of (page, offset) fails both, loudly, at the first wrong byte.
 *
 * The order of the cases follows the order of the risk:
 *
 *   1. the classifier -- a swap entry must never be mistaken for an untouched
 *      anonymous page, because the recovery for one is "read the disk" and for
 *      the other is "fill with zeroes";
 *   2. the cheap tier -- a page that can be reconstructed must not be written;
 *   3. the round trip -- byte-for-byte, under distinct patterns;
 *   4. pinning -- what may never be evicted, proven by trying;
 *   5. the clock -- a page in use must survive a pass;
 *   6. sharing and fork -- the cases where one page has several owners;
 *   7. failure -- a write that fails must leave the process exactly as it was;
 *   8. exhaustion -- reclaim must work when there is no memory left, since that
 *      is the only condition it exists for.
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
#include "swap.h"
#include "reclaim.h"
#include "pcache.h"
#include "mmhost.h"      /* mm_host_cr3: the simulated active address space */
#include "kprintf.h"

/* mm_fault_classify grew a `vma_file` argument when the page cache landed. This
 * file is the SWAP file -- every case here is about telling a swap entry apart
 * from an anonymous fill, and none of them is file-backed -- so the flag is
 * wrapped to 0 once here rather than appended to seven call sites, exactly as
 * tests/unit/mm_vmm_test.c wraps the swap flag itself for the same reason.
 * Note the shape: this macro passes `swap` THROUGH, because distinguishing it
 * is what the file is for; it only fixes the argument this file has no opinion
 * about. A file-backed case belongs in tests/unit/mm_pcache_test.c, beside the
 * code that produces one. */
#define mm_fault_classify(cr2, err, p, cow, u, prot, swap) \
        mm_fault_classify((cr2), (err), (p), (cow), (u), (prot), (swap), 0, 0)

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4

#define PF_P 0x01
#define PF_W 0x02
#define PF_U 0x04

#define VBASE (MM_USER_BASE + 0x100000)
#define VA(n) (VBASE + (uint64_t)(n) * 4096)

static void phase(const char *s) { printf("-- %s\n", s); }

/* Device errors caused ON PURPOSE by t_write_failure. Recorded so the final
 * "no unexplained device errors" assertion means what it says: without this it
 * would either be a lie (counting the deliberate ones) or have to be dropped. */
static uint64_t g_deliberate_io_errors;

/* The pattern. A function of the page index AND the byte offset, so:
 *   - a page swapped in from the wrong slot has the wrong page index baked in
 *     and mismatches at byte 0;
 *   - a transfer that is off by one byte, one sector or one page mismatches
 *     everywhere;
 *   - a page that was silently zero-filled instead of read back mismatches at
 *     byte 0 for every page except one that would legitimately be zero.
 * The multiplier is odd so no byte position ever collapses to a constant. */
static uint8_t pat(int page, int off)
{
    return (uint8_t)((page * 131 + off * 17 + (off >> 5) * 7 + 0x5B) & 0xFF);
}

static void fill_pattern(void *p, int page)
{
    uint8_t *b = p;
    for (int i = 0; i < 4096; i++) b[i] = pat(page, i);
}

/* Returns the offset of the first wrong byte, or -1 if the page is right. */
static int check_pattern(const void *p, int page)
{
    const uint8_t *b = p;
    for (int i = 0; i < 4096; i++)
        if (b[i] != pat(page, i)) return i;
    return -1;
}

/* Build an address space with `n` anonymous pages, materialised through the
 * REAL fault path (so they carry VMM_PTE_ANON exactly as a running process's
 * mmap pages do). Returns the cr3. */
static uint64_t make_space(int n)
{
    uint64_t cr3 = vmm_new_space();
    if (!cr3) return 0;
    if (vma_reserve_fixed(cr3, VBASE, (uint64_t)n * 4096, VMA_READ | VMA_WRITE) < 0) return 0;
    mm_host_cr3 = cr3;
    for (int i = 0; i < n; i++)
        if (!mm_fault_in(cr3, VA(i), PF_W | PF_U)) return 0;
    return cr3;
}

static uint64_t frame_at(uint64_t cr3, uint64_t va)
{
    uint64_t *p = vmm_pte(cr3, va);
    return (p && (*p & PRESENT)) ? (*p & MM_PTE_ADDR) : 0;
}

/* ======================================================================== */
static void t_classify(void)
{
    phase("the classifier: a swap entry is never an untouched anonymous page");

    uint64_t va = VBASE;

    /* The whole reason MM_FAULT_SWAP is checked before MM_FAULT_ANON. Both are
     * P=0 faults inside a readable+writable reservation; only the PTE tells
     * them apart, and getting it wrong refills a swapped page with zeroes. */
    mm_eqi(mm_fault_classify(va, PF_U, 0, 0, 0, VMA_READ | VMA_WRITE, 1), MM_FAULT_SWAP,
           "a read fault on a swap entry is a swap-in");
    mm_eqi(mm_fault_classify(va, PF_W | PF_U, 0, 0, 0, VMA_READ | VMA_WRITE, 1), MM_FAULT_SWAP,
           "a write fault on a swap entry is a swap-in");
    mm_eqi(mm_fault_classify(va, PF_U, 0, 0, 0, VMA_READ | VMA_WRITE, 0), MM_FAULT_ANON,
           "the SAME fault without the swap bit is an anonymous fill");

    /* A swapped page with no VMA at all must still come back: an ELF image's
     * text and data are mapped directly by elf_load and never reserved. This is
     * the case that would kill every process whose code got evicted. */
    mm_eqi(mm_fault_classify(va, PF_U, 0, 0, 0, 0, 1), MM_FAULT_SWAP,
           "a swap entry outside any VMA still faults back in");
    mm_eqi(mm_fault_classify(va, PF_U, 0, 0, 0, 0, 0), MM_FAULT_NONE,
           "but an ordinary absent page outside any VMA still kills the process");

    /* Disagreement between the hardware and the tables is never "handled". */
    mm_eqi(mm_fault_classify(va, PF_P | PF_W | PF_U, 1, 0, 1, VMA_READ | VMA_WRITE, 1),
           MM_FAULT_NONE,
           "a protection fault on something we think is swapped is refused");
    mm_eqi(mm_fault_classify(0x1000, PF_U, 0, 0, 0, VMA_READ, 1), MM_FAULT_NONE,
           "a swap-looking entry outside the user region is still refused");

    /* And the encoding round-trips. */
    uint64_t e = VMM_PTE_SWAP | (12345ull << VMM_SWAP_SHIFT) | VMM_SWAP_W | VMM_SWAP_ANON;
    mm_ok(vmm_pte_is_swap(e), "a swap entry is recognised");
    mm_eqi((long long)vmm_pte_swap_slot(e), 12345, "the slot survives the encoding");
    mm_ok(!vmm_pte_is_swap(0), "a zeroed PTE is NOT a swap entry");
    mm_ok(!vmm_pte_is_swap(0x1000 | PRESENT | USER | WRITABLE),
          "an ordinary present PTE is not a swap entry");
}

/* ======================================================================== */
static void t_drop_tier(void)
{
    phase("the cheap tier: a reconstructible page is dropped, not written");

    uint64_t base_w = mm_sim_swap_writes();
    uint64_t cr3 = make_space(32);
    mm_ok(cr3 != 0, "a space with 32 anonymous pages");

    /* Every page is still zero: do_anon zero-fills, and nothing has written. */
    uint64_t free_before = pmm_free_frames();
    uint64_t dropped_before = reclaim_dropped();
    uint64_t swapped_before = reclaim_swapped();

    /* Two passes: the first sweep clears accessed bits, the second evicts. The
     * pages have no accessed bits set here (the simulator has no CPU to set
     * them), so one call is enough -- but ask for more than exists so the whole
     * candidate set is visited. */
    uint64_t got = reclaim_frames(32);

    mm_ok(got >= 32, "reclaim freed at least the 32 pages (%llu)", (unsigned long long)got);
    mm_ok(reclaim_dropped() - dropped_before >= 32,
          "and freed them through the DROP tier (%llu dropped)",
          (unsigned long long)(reclaim_dropped() - dropped_before));
    mm_eqi((long long)(reclaim_swapped() - swapped_before), 0,
           "nothing was written to the swap device");
    mm_eqi((long long)(mm_sim_swap_writes() - base_w), 0,
           "the device saw no writes at all");
    mm_ok(pmm_free_frames() > free_before, "free memory went up");

    /* The mappings are simply gone: not swap entries, nothing. */
    uint64_t *pte = vmm_pte(cr3, VA(0));
    mm_ok(pte && *pte == 0, "a dropped page's PTE is empty, not a swap entry");

    /* And it comes back, correct, through the ordinary anonymous fault. That is
     * what "reconstructible" means, and it is asserted rather than assumed. */
    mm_host_cr3 = cr3;
    mm_ok(mm_fault_in(cr3, VA(0), PF_W | PF_U) == 1, "a dropped page faults back in");
    uint64_t f = frame_at(cr3, VA(0));
    mm_ok(f != 0, "and is present again");
    const uint8_t *b = mm_sim_ptr(f);
    int bad = 0;
    for (int i = 0; i < 4096; i++) if (b[i]) { bad = i + 1; break; }
    mm_eqi(bad, 0, "reconstructed as zeroes, which is what it was");

    mm_eqi(rmap_audit(), 0, "reverse map clean after the drop tier");
    vmm_free_space(cr3);
}

/* ======================================================================== */
static void t_round_trip(void)
{
    phase("the round trip: distinct per-page patterns, byte for byte");

    const int N = 48;
    uint64_t cr3 = make_space(N);
    mm_ok(cr3 != 0, "a space with %d anonymous pages", N);

    /* Give every page contents that no other page could produce. */
    uint64_t frames[64];
    for (int i = 0; i < N; i++) {
        frames[i] = frame_at(cr3, VA(i));
        mm_ok(frames[i] != 0, "page %d is present before eviction", i);
        fill_pattern(mm_sim_ptr(frames[i]), i);
    }

    uint64_t sw_before = reclaim_swapped();
    uint64_t drop_before = reclaim_dropped();
    uint64_t got = reclaim_frames(N);
    mm_ok(got >= (uint64_t)N, "reclaim evicted all %d pages (%llu)", N, (unsigned long long)got);
    mm_eqi((long long)(reclaim_dropped() - drop_before), 0,
           "none went through the drop tier -- they all have data");
    mm_ok(reclaim_swapped() - sw_before >= (uint64_t)N,
          "all %d went to the swap device", N);

    /* Every PTE is now a swap entry, and every slot is distinct. Two pages
     * sharing a slot is the bug that a per-page pattern is designed to catch,
     * but catching it here, at the slot level, says WHY. */
    uint64_t slots[64];
    for (int i = 0; i < N; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        mm_ok(pte && vmm_pte_is_swap(*pte), "page %d is now a swap entry", i);
        slots[i] = vmm_pte_swap_slot(*pte);
        mm_ok(slots[i] != SWAP_NOSLOT, "page %d has a real slot", i);
    }
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++)
            if (slots[i] == slots[j])
                mm_ok(0, "pages %d and %d were given the SAME slot %llu",
                      i, j, (unsigned long long)slots[i]);

    /* The bytes on the device are the bytes that were in memory. Checked
     * against the raw device, before any swap-in, so a read-side bug cannot
     * cancel out a write-side one. */
    for (int i = 0; i < N; i++) {
        int off = check_pattern(mm_sim_swap_slot_bytes(slots[i]), i);
        if (off >= 0)
            mm_ok(0, "slot %llu holds the wrong bytes for page %d at offset %d",
                  (unsigned long long)slots[i], i, off);
    }
    mm_ok(1, "every slot on the device holds its own page's bytes");

    /* Now fault them all back, in an order that is not the order they were
     * written: a swap-in that quietly depends on sequential access would pass a
     * forward walk and fail this. */
    mm_host_cr3 = cr3;
    int order[64];
    for (int i = 0; i < N; i++) order[i] = (i * 29 + 7) % N;   /* 29 is coprime with 48 */
    int wrong = 0, missing = 0;
    for (int k = 0; k < N; k++) {
        int i = order[k];
        if (!mm_fault_in(cr3, VA(i), PF_U)) { missing++; continue; }
        uint64_t f = frame_at(cr3, VA(i));
        if (!f) { missing++; continue; }
        int off = check_pattern(mm_sim_ptr(f), i);
        if (off >= 0) {
            if (wrong < 4)
                printf("   page %d: first wrong byte at %d (got %02x, want %02x)\n",
                       i, off, ((uint8_t *)mm_sim_ptr(f))[off], pat(i, off));
            wrong++;
        }
    }
    mm_eqi(missing, 0, "every swapped page faulted back in");
    mm_eqi(wrong, 0, "every swapped page came back byte-identical");

    /* Faulting a page in releases its slot: a swap area that only ever fills up
     * is a swap area that stops working after one pass over memory. */
    mm_eqi((long long)swap_slots_used(), 0, "every slot was released on swap-in");

    /* And the pages are writable again, since they were writable before. */
    uint64_t *pte = vmm_pte(cr3, VA(3));
    mm_ok(pte && (*pte & WRITABLE), "a writable page comes back writable");
    mm_ok(pte && (*pte & USER), "and user-accessible");

    mm_eqi(rmap_audit(), 0, "reverse map clean after a full round trip");
    mm_eqi((long long)pmm_bugs(), 0, "no frame-allocator invariant violations");
    vmm_free_space(cr3);
}

/* ======================================================================== */
static void t_pinning(void)
{
    phase("pinning: what must never be evicted, proven by trying hard");

    const int N = 24;
    uint64_t cr3 = make_space(N);
    mm_ok(cr3 != 0, "a space with %d pages", N);

    for (int i = 0; i < N; i++)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), i);

    /* Pin three of them, as the kernel does around a usercopy window or a
     * device transfer into user memory. */
    const int pinned[3] = { 2, 11, 23 };
    uint64_t pf[3];
    for (int k = 0; k < 3; k++) {
        pf[k] = frame_at(cr3, VA(pinned[k]));
        pmm_pin(pf[k]);
    }
    mm_eqi((long long)pmm_pins_live(), 3, "three frames are pinned");

    /* Nested pins must not be cancelled by one unpin: two overlapping windows
     * into the same page is the case that would otherwise unprotect it early. */
    pmm_pin(pf[1]);
    pmm_unpin(pf[1]);
    mm_eqi((long long)pmm_pincount(pf[1]), 1, "a nested pin survives one unpin");

    /* Now lean on reclaim: ask for far more than is available, several times,
     * so the hand goes all the way round repeatedly. If the pin were advisory
     * this could not survive it. */
    uint64_t skipped_before = reclaim_skip_pinned();
    for (int round = 0; round < 4; round++) reclaim_frames(N * 4);

    for (int k = 0; k < 3; k++) {
        uint64_t *pte = vmm_pte(cr3, VA(pinned[k]));
        mm_ok(pte && (*pte & PRESENT), "pinned page %d is still mapped", pinned[k]);
        mm_ok(pte && (*pte & MM_PTE_ADDR) == pf[k],
              "pinned page %d is still on its ORIGINAL frame", pinned[k]);
        int off = check_pattern(mm_sim_ptr(pf[k]), pinned[k]);
        mm_eqf(off, -1, "pinned page %d still holds its own bytes", pinned[k]);
    }
    mm_ok(reclaim_skip_pinned() > skipped_before,
          "reclaim reached those frames and refused them (%llu skips)",
          (unsigned long long)(reclaim_skip_pinned() - skipped_before));

    /* The unpinned ones went, which is what makes the above a real test rather
     * than a test of a reclaim that did nothing at all. */
    int evicted = 0;
    for (int i = 0; i < N; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        if (pte && !(*pte & PRESENT)) evicted++;
    }
    mm_ok(evicted >= N - 3 - 2, "the unpinned pages WERE evicted (%d of %d)", evicted, N);

    for (int k = 0; k < 3; k++) pmm_unpin(pf[k]);
    mm_eqi((long long)pmm_pins_live(), 0, "unpinning is balanced");
    mm_eqi((long long)pmm_bugs(), 0, "no unbalanced-unpin complaints");

    /* Once unpinned they are ordinary candidates again. */
    uint64_t got = reclaim_frames(8);
    mm_ok(got > 0, "unpinned pages become evictable again");

    mm_eqi(rmap_audit(), 0, "reverse map clean");
    vmm_free_space(cr3);
}

/* ======================================================================== */
static void t_clock(void)
{
    phase("the clock: a page in use survives the pass that takes an idle one");

    const int N = 16;
    uint64_t cr3 = make_space(N);
    for (int i = 0; i < N; i++)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), i);

    /* Mark half the pages referenced, as the CPU would on a real machine. The
     * simulator has no CPU to set accessed bits, so the test sets them -- what
     * is under test is what reclaim DOES with the bit, which is the part that
     * can be wrong. */
    for (int i = 0; i < N; i += 2)
        *vmm_pte(cr3, VA(i)) |= VMM_PTE_ACCESSED;

    uint64_t second_before = reclaim_second_chance();
    uint64_t total = pmm_total_frames();

    /* ONE sweep of the hand. Ask for one frame more than the idle pages can
     * supply, so the pass definitely visits everything. */
    reclaim_frames(N / 2);

    mm_ok(reclaim_second_chance() > second_before,
          "referenced pages were given a second chance (%llu)",
          (unsigned long long)(reclaim_second_chance() - second_before));

    int ref_gone = 0, idle_gone = 0;
    for (int i = 0; i < N; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        int present = pte && (*pte & PRESENT);
        if (i % 2 == 0) { if (!present) ref_gone++; }
        else            { if (!present) idle_gone++; }
    }
    mm_eqi(ref_gone, 0, "no referenced page was evicted in the first pass");
    mm_ok(idle_gone > 0, "idle pages were (%d of %d)", idle_gone, N / 2);

    /* The second chance is spent, not permanent: the accessed bits were cleared
     * by the pass, so a page that is not touched again loses next time round. */
    for (int i = 0; i < N; i += 2)
        mm_ok(!(*vmm_pte(cr3, VA(i)) & VMM_PTE_ACCESSED),
              "page %d's accessed bit was cleared by the sweep", i);

    reclaim_frames(N);
    int now_gone = 0;
    for (int i = 0; i < N; i += 2)
        if (!(*vmm_pte(cr3, VA(i)) & PRESENT)) now_gone++;
    mm_ok(now_gone > 0, "a page that was not touched again lost its chance (%d)", now_gone);

    (void)total;
    vmm_free_space(cr3);
    mm_eqi(rmap_audit(), 0, "reverse map clean");
}

/* ======================================================================== */
static void t_shared_and_fork(void)
{
    phase("sharing: a shared page evicts once, and a fork inherits the slot");

    uint64_t parent = make_space(4);
    mm_ok(parent != 0, "a parent with 4 pages");
    for (int i = 0; i < 4; i++)
        fill_pattern(mm_sim_ptr(frame_at(parent, VA(i))), 100 + i);

    uint64_t child = vmm_new_space();
    mm_ok(vmm_clone_user(child, parent) == 0, "fork");

    uint64_t f0 = frame_at(parent, VA(0));
    mm_eqi(rmap_count(f0), 2, "the page is mapped by both spaces");
    mm_eqi(pmm_refcount(f0), 2, "and referenced twice");

    /* Evict. ONE frame goes; BOTH PTEs must become swap entries pointing at the
     * same slot, and the slot's reference count must say two. Getting this
     * wrong frees the slot when the first sharer faults, and the second sharer
     * then reads a slot that has been handed to another page. */
    uint64_t used_before = pmm_used_frames();
    uint64_t got = reclaim_frames(4);
    mm_ok(got > 0, "reclaim evicted shared pages (%llu)", (unsigned long long)got);
    mm_ok(pmm_used_frames() < used_before, "frames were actually returned");

    uint64_t *pp = vmm_pte(parent, VA(0)), *cp = vmm_pte(child, VA(0));
    mm_ok(pp && vmm_pte_is_swap(*pp), "the parent's PTE is a swap entry");
    mm_ok(cp && vmm_pte_is_swap(*cp), "the child's PTE is a swap entry");
    mm_ok(vmm_pte_swap_slot(*pp) == vmm_pte_swap_slot(*cp),
          "both point at the SAME slot");
    mm_eqi((long long)swap_slot_refs(vmm_pte_swap_slot(*pp)), 2,
           "and the slot is referenced twice");

    /* Both sharers fault it back, one after the other. Each must get the right
     * bytes -- the second one especially, since by then the first has already
     * dropped its reference to the slot. */
    uint64_t slot = vmm_pte_swap_slot(*pp);
    mm_host_cr3 = parent;
    mm_ok(mm_fault_in(parent, VA(0), PF_U) == 1, "the parent faults it back");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(parent, VA(0))), 100),
           -1, "the parent got the right bytes");
    mm_eqi((long long)swap_slot_refs(slot), 1, "one reference to the slot is left");

    mm_host_cr3 = child;
    mm_ok(mm_fault_in(child, VA(0), PF_U) == 1, "the child faults it back");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(child, VA(0))), 100),
           -1, "the child got the right bytes too");
    mm_eqi((long long)swap_slot_refs(slot), 0, "the slot is free again");

    mm_ok(frame_at(parent, VA(0)) != frame_at(child, VA(0)),
          "they came back on separate frames (the documented loss of sharing)");

    /* A fork of a process that has swapped-out pages must give the child those
     * pages, not a hole. This is the case that only shows up under pressure. */
    phase("fork with swapped-out pages: the child inherits the slots");
    mm_host_cr3 = parent;
    reclaim_frames(8);                       /* push the parent back out */
    uint64_t *sp = vmm_pte(parent, VA(1));
    mm_ok(sp && vmm_pte_is_swap(*sp), "the parent has a swapped page at VA(1)");
    uint64_t s1 = vmm_pte_swap_slot(*sp);
    unsigned refs_before = swap_slot_refs(s1);

    uint64_t child2 = vmm_new_space();
    mm_ok(vmm_clone_user(child2, parent) == 0, "fork a process with swapped pages");
    uint64_t *c2 = vmm_pte(child2, VA(1));
    mm_ok(c2 && vmm_pte_is_swap(*c2), "the child inherited a swap entry, not a hole");
    mm_eqi((long long)vmm_pte_swap_slot(*c2), (long long)s1, "pointing at the same slot");
    mm_eqi((long long)swap_slot_refs(s1), (long long)refs_before + 1,
           "and the slot's reference count went up");

    mm_host_cr3 = child2;
    mm_ok(mm_fault_in(child2, VA(1), PF_U) == 1, "the child can fault it in");
    mm_eqi(check_pattern(mm_sim_ptr(frame_at(child2, VA(1))), 101),
           -1, "with the parent's bytes");

    /* Tearing down a space with swapped pages must release their slots, or the
     * swap area leaks capacity that nothing on the machine could account for. */
    phase("a dying process releases its swap slots");
    uint64_t slots_used = swap_slots_used();
    vmm_free_space(child2);
    vmm_free_space(child);
    vmm_free_space(parent);
    mm_ok(swap_slots_used() < slots_used || slots_used == 0,
          "freeing the spaces released their slots (%llu -> %llu)",
          (unsigned long long)slots_used, (unsigned long long)swap_slots_used());
    mm_eqi((long long)swap_slots_used(), 0, "no slot was leaked");
    mm_eqi(rmap_audit(), 0, "reverse map clean");
}

/* ======================================================================== */
static void t_munmap_releases_slots(void)
{
    phase("munmap of swapped-out memory releases the slots");

    uint64_t cr3 = make_space(8);
    for (int i = 0; i < 8; i++)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 200 + i);
    reclaim_frames(8);

    int swapped = 0;
    for (int i = 0; i < 8; i++)
        if (vmm_pte_is_swap(*vmm_pte(cr3, VA(i)))) swapped++;
    mm_ok(swapped > 0, "%d pages are on the device", swapped);
    mm_eqi((long long)swap_slots_used(), swapped, "and that many slots are in use");

    vmm_unmap_range_in(cr3, VBASE, 8 * 4096);
    mm_eqi((long long)swap_slots_used(), 0, "unmapping released every slot");

    vmm_free_space(cr3);
}

/* ======================================================================== */
static void t_write_failure(void)
{
    phase("a failed write leaves the process exactly as it was");

    uint64_t cr3 = make_space(6);
    uint64_t frames[6];
    for (int i = 0; i < 6; i++) {
        frames[i] = frame_at(cr3, VA(i));
        fill_pattern(mm_sim_ptr(frames[i]), 300 + i);
    }

    uint64_t io_before = reclaim_fail_io();
    uint64_t slots_before = swap_slots_used();
    uint64_t errs_before = swap_io_errors();
    mm_sim_swap_fail(1);
    uint64_t got = reclaim_frames(6);
    mm_sim_swap_fail(0);
    g_deliberate_io_errors += swap_io_errors() - errs_before;

    mm_eqi((long long)got, 0, "nothing was reclaimed when the device refused");
    mm_ok(reclaim_fail_io() > io_before, "and the failure was counted");
    mm_eqi((long long)swap_slots_used(), (long long)slots_before,
           "the slot it had taken was given back");

    for (int i = 0; i < 6; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        mm_ok(pte && (*pte & PRESENT), "page %d is still present", i);
        mm_ok(pte && (*pte & MM_PTE_ADDR) == frames[i],
              "page %d is on its original frame", i);
        mm_eqf(check_pattern(mm_sim_ptr(frames[i]), 300 + i), -1,
               "page %d still holds its own bytes", i);
    }
    mm_eqi(rmap_audit(), 0, "reverse map clean after a failed eviction");

    /* And with the device working again, the same pages evict normally: the
     * failure path did not leave anything in a state that blocks retry. */
    mm_ok(reclaim_frames(6) > 0, "the same pages evict once the device recovers");
    vmm_free_space(cr3);
}

/* ======================================================================== */
static void t_no_swap_device(void)
{
    phase("with NO swap device at all, the drop tier still works");

    /* A machine with no second disk must not lose reclaim entirely: the tier
     * that needs no device is exactly the tier that should keep working. */
    uint64_t cr3 = make_space(16);
    /* Half zero (droppable), half with data (needs a device it does not have). */
    for (int i = 0; i < 16; i += 2)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 400 + i);

    uint64_t drop_before = reclaim_dropped();
    uint64_t swap_before = reclaim_swapped();
    uint64_t got = reclaim_frames(16);

    mm_ok(got > 0, "reclaim still freed %llu frames", (unsigned long long)got);
    mm_ok(reclaim_dropped() > drop_before, "through the drop tier");
    mm_eqi((long long)(reclaim_swapped() - swap_before), 0, "and nothing was swapped");

    for (int i = 0; i < 16; i += 2)
        mm_eqf(check_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 400 + i), -1,
               "page %d with data was left alone", i);

    vmm_free_space(cr3);
    mm_eqi(rmap_audit(), 0, "reverse map clean");
}

/* ======================================================================== */
static void t_exhaustion(void)
{
    phase("reclaim works with the frame allocator EMPTY -- the case it is for");

    /* THE ALLOCATE-TO-FREE DEADLOCK, tested rather than argued. Fill memory
     * completely, then require reclaim to still make progress. If any part of
     * the write-out path allocated -- a bounce buffer, a reverse-map node, a
     * slot table entry -- this is where it would fail, and it would fail
     * exactly when the mechanism is most needed. */
    const int N = 40;
    uint64_t cr3 = make_space(N);
    mm_ok(cr3 != 0, "a space with %d pages holding data", N);
    for (int i = 0; i < N; i++)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 500 + i);

    /* Take every remaining frame, WITH RECLAIM OFF. It has to be off for this
     * to be the test it claims to be: leave it on and the allocation loop
     * itself trips the low watermark, reclaim evicts the victims on the way
     * down, and the explicit call below would find nothing left to do and
     * "pass" having proved nothing. (That reclaim fires unprompted during the
     * loop is a real property and is what the on-device harness measures; here
     * the point is what reclaim can do from a standing start at zero.) */
    reclaim_set_enabled(0);
    static uint64_t held[400000];
    uint64_t nheld = 0;
    for (;;) {
        uint64_t f = pmm_alloc();
        if (!f) break;
        if (nheld < sizeof held / sizeof held[0]) held[nheld++] = f;
        else { pmm_free(f); break; }
    }
    printf("   held %llu frames; %llu free, %llu in reserve\n",
           (unsigned long long)nheld, (unsigned long long)pmm_free_frames(),
           (unsigned long long)pmm_reserve());
    mm_ok(pmm_free_frames() <= pmm_reserve(),
          "memory is exhausted down to the reserve");
    mm_ok(pmm_alloc() == 0, "an ordinary allocation now fails");
    mm_ok(pmm_alloc_failures() > 0, "the exhaustion was real (allocations were refused)");
    reclaim_set_enabled(1);

    uint64_t rmnodes = rmap_nodes_used();
    uint64_t swrites = swap_writes();
    uint64_t got = reclaim_frames(N / 2);
    mm_ok(got > 0, "reclaim freed %llu frames with nothing free to start from",
          (unsigned long long)got);
    mm_ok(rmap_nodes_used() < rmnodes,
          "and RETURNED reverse-map nodes rather than needing new ones");
    mm_ok(swap_writes() > swrites,
          "the write-out path ran at zero free frames without allocating anything");

    /* Now an allocation succeeds again, which is the whole point: the machine
     * that was out of memory is not out of memory any more. */
    uint64_t f = pmm_alloc();
    mm_ok(f != 0, "an ordinary allocation succeeds again after reclaim");
    if (f) pmm_free(f);

    /* THE RESERVE, exercised. Take everything again -- reclaim off, so the
     * frames it just freed go straight back into `held` -- and then fault ONE
     * page in. There is no ordinary memory at all at that point, so the only
     * way it can succeed is the reserve, and succeeding is the difference
     * between a machine that can crawl out of a thrash and one that is stuck in
     * it because it cannot afford the fault that would free something. */
    reclaim_set_enabled(0);
    for (;;) {
        uint64_t g = pmm_alloc();
        if (!g) break;
        if (nheld < sizeof held / sizeof held[0]) held[nheld++] = g;
        else { pmm_free(g); break; }
    }
    mm_ok(pmm_alloc() == 0, "memory is exhausted again");
    uint64_t hits_before = pmm_reserve_hits();
    mm_host_cr3 = cr3;
    int did = 0;
    for (int i = 0; i < N && !did; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        if (!pte || !vmm_pte_is_swap(*pte)) continue;
        did = 1;
        mm_ok(mm_fault_in(cr3, VA(i), PF_U) == 1,
              "a swap-in completes with no ordinary memory left");
        mm_eqf(check_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 500 + i), -1,
               "and page %d came back with the right bytes", i);
    }
    mm_ok(did, "there was a swapped page to fault in");
    mm_ok(pmm_reserve_hits() > hits_before, "and the frame came out of the reserve");
    reclaim_set_enabled(1);

    /* Give the held frames back before faulting the rest in. A swap-in needs a
     * frame, and with every frame still held the only source is the 32-frame
     * reserve -- which is deliberately too small to fault a whole working set
     * back in. That is the reserve behaving correctly (it is the escape hatch
     * for the last few faults, not a pool to run from), so the test releases the
     * artificial pressure rather than asserting the reserve is infinite. */
    for (uint64_t i = 0; i < nheld; i++) pmm_free(held[i]);

    mm_host_cr3 = cr3;
    int wrong = 0, checked = 0;
    for (int i = 0; i < N; i++) {
        uint64_t *pte = vmm_pte(cr3, VA(i));
        if (!pte || !vmm_pte_is_swap(*pte)) continue;
        checked++;
        if (!mm_fault_in(cr3, VA(i), PF_U)) { wrong++; continue; }
        if (check_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 500 + i) >= 0) wrong++;
    }
    mm_ok(checked > 0, "%d pages had been written out under exhaustion", checked);
    mm_eqi(wrong, 0, "all of them came back byte-identical");

    vmm_free_space(cr3);
    mm_eqi(rmap_audit(), 0, "reverse map clean");
    mm_eqi((long long)pmm_bugs(), 0, "no frame-allocator invariant violations");
}

/* ======================================================================== */
static void t_thrash(void)
{
    phase("thrash: evict and fault back repeatedly, measuring the rate");

    /* A machine that survives by thrashing should be able to SAY so. This drives
     * a working set larger than what reclaim will leave resident and reports the
     * fault and eviction counts, which is the number an operator needs. */
    const int N = 64;
    uint64_t cr3 = make_space(N);
    for (int i = 0; i < N; i++)
        fill_pattern(mm_sim_ptr(frame_at(cr3, VA(i))), 600 + i);

    uint64_t ev0 = reclaim_dropped() + reclaim_swapped();
    uint64_t in0 = reclaim_swapins();
    int wrong = 0;

    mm_host_cr3 = cr3;
    for (int round = 0; round < 12; round++) {
        reclaim_frames(N / 2);
        for (int i = 0; i < N; i++) {
            uint64_t *pte = vmm_pte(cr3, VA(i));
            if (pte && !(*pte & PRESENT)) {
                if (!mm_fault_in(cr3, VA(i), PF_W | PF_U)) { wrong++; continue; }
            }
            uint64_t f = frame_at(cr3, VA(i));
            /* A page that was DROPPED (it was zero) legitimately comes back
             * zero; a page that was SWAPPED must come back with its pattern. */
            if (f && check_pattern(mm_sim_ptr(f), 600 + i) >= 0) {
                const uint8_t *b = mm_sim_ptr(f);
                int allzero = 1;
                for (int k = 0; k < 4096; k++) if (b[k]) { allzero = 0; break; }
                if (!allzero) wrong++;
                else fill_pattern(mm_sim_ptr(f), 600 + i);   /* re-dirty it */
            }
        }
    }

    uint64_t evicted = reclaim_dropped() + reclaim_swapped() - ev0;
    uint64_t faulted = reclaim_swapins() - in0;
    mm_eqi(wrong, 0, "no page was corrupted over 12 thrash rounds");
    mm_ok(evicted > 0 && faulted > 0, "the loop really thrashed");
    printf("   THRASH: %llu evictions, %llu swap-ins over 12 rounds of a %d-page "
           "working set (%llu KiB moved)\n",
           (unsigned long long)evicted, (unsigned long long)faulted, N,
           (unsigned long long)((swap_writes() + swap_reads()) * 4));

    vmm_free_space(cr3);
    mm_eqi(rmap_audit(), 0, "reverse map clean after thrashing");
}

/* ======================================================================== */
/* pcache.h's refcount decision, from reclaim's side of the hook. This is NOT
 * mm_pcache_test.c's job repeated: that file drives pcache.c on its own and
 * says so explicitly ("reclaim.c never calls pcache_holds()/
 * pcache_forget_frame() -- none of that is this file's problem"). This is the
 * other half -- proving reclaim's eligibility test and drop path actually use
 * those two hooks, including the case NEITHER file alone can build: a frame
 * that fault.c's do_file() mapped AND pcache.c is caching at the same time,
 * which needs vma.c, fault.c and pcache.c all wired together the way the
 * kernel wires them, exactly what this whole test binary links for. */
#define PC_TEST_PAGES 4
static uint8_t pc_backing[PC_TEST_PAGES][4096];

static int pc_test_stat(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size)
{
    if (!path) return -1;
    *dev = 1; *ino = 1; *size = PC_TEST_PAGES * 4096;
    return 0;
}

static long pc_test_read(const char *path, uint64_t off, void *dst, uint64_t len)
{
    (void)path;
    uint64_t page = off / 4096;
    if (page >= PC_TEST_PAGES) return -1;
    if (len > 4096) len = 4096;
    memcpy(dst, pc_backing[page], (size_t)len);
    return (long)len;
}

static const struct pcache_ops pc_test_ops = { pc_test_stat, pc_test_read, 0 };

static void t_pcache(void)
{
    phase("the page cache: an ordinary reference, not a pin (pcache.h)");

    /* Bring the cache up once, with a tiny fake one-file backend. Safe here:
     * pcache_init() is idempotent, and every case above this one ran with the
     * cache not yet up -- pcache_holds() answered 0 for everything, exactly as
     * it did before reclaim.c knew this file existed, so nothing already
     * proven above depended on the cache staying off. */
    pcache_init(pmm_total_frames());
    mm_ok(pcache_ready(), "the page cache came up");
    pcache_set_ops(&pc_test_ops);
    for (int i = 0; i < PC_TEST_PAGES; i++) fill_pattern(pc_backing[i], 700 + i);

    int fh = pcache_file_open("/pctest/file");
    mm_ok(fh >= 0, "opened the test file");

    /* (a) + (b): a page the cache holds and that NOTHING ELSE maps -- read
     * once through pcache_get(), never faulted into any address space -- is
     * the cheapest frame on the machine per pcache.h's design note: rmap_count
     * 0, refcount 1, no PTE to tear down. Evicting it must go through the
     * cache-drop path, not be skipped as ordinary "unmapped kernel memory". */
    uint64_t frame = pcache_get(fh, 0);
    mm_ok(frame != 0, "page 0 came into the cache");
    mm_ok(pcache_holds(frame), "pcache_holds() sees it");
    mm_eqi((long long)rmap_count(frame), 0, "and NOTHING maps it -- no PTE at all");
    mm_eqi((long long)pmm_refcount(frame), 1, "one reference: the cache's own");

    /* NOTE on what is NOT asserted here: reclaim_frames(1) sweeps the clock
     * hand from wherever the last call left it, over a 16384-frame machine,
     * until it frees one frame -- so reclaim_skip_unmapped() legitimately
     * climbs by hundreds on ordinary kernel memory (page tables, the rmap and
     * pcache frame tables, ...) along the way. That is not this frame being
     * turned away; it is everything ELSE on the machine being correctly left
     * alone. The claim under test is about THIS frame specifically, and the
     * before/after state below is what proves it: skipped forever looks like
     * "still resident, still refcount 1"; evicted looks like what follows. */
    uint64_t dc_before = reclaim_dropped_cache();
    uint64_t dz_before = reclaim_dropped_zero();
    uint64_t got = reclaim_frames(1);
    mm_ok(got >= 1, "reclaim evicted the cache-only page");
    mm_ok(reclaim_dropped_cache() > dc_before,
          "(b) counted through the CACHE-drop split (%llu)",
          (unsigned long long)(reclaim_dropped_cache() - dc_before));
    mm_eqi((long long)(reclaim_dropped_zero() - dz_before), 0,
           "and not through the zero-page split -- it is a different producer");
    mm_ok(!pcache_holds(frame), "(a) the entry is gone -- pcache_forget_frame() ran");
    mm_eqi((long long)pmm_refcount(frame), 0,
           "and the frame is back on the allocator, not dangling onto memory "
           "somebody else now owns");

    /* Re-fetching the same page must be a genuine MISS: forgetting the frame
     * really dropped the cache's own entry, not just the reclaim bookkeeping
     * beside it. */
    uint64_t miss_before = pcache_misses();
    uint64_t frame2 = pcache_get(fh, 0);
    mm_ok(frame2 != 0, "the page is readable again");
    mm_ok(pcache_misses() > miss_before, "and came back through a real miss");
    mm_eqf(check_pattern(mm_sim_ptr(frame2), 700), -1,
           "with its own bytes, re-read from the backend");

    /* (c): a frame held by BOTH a live PTE and the cache -- built the way
     * production code builds one, a real file-backed VMA faulted through
     * do_file(), not synthesised by hand. A DIFFERENT file page (index 1) from
     * the one above, so this case starts from a clean miss of its own. */
    uint64_t cr3 = vmm_new_space();
    mm_ok(cr3 != 0, "a space for the mapped+cached case");
    uint64_t base = vma_reserve_file(cr3, 0, 4096, VMA_READ, fh, 4096);
    mm_ok(base != 0, "reserved a file-backed page at file offset 4096 (index 1)");
    mm_host_cr3 = cr3;
    mm_ok(mm_fault_in(cr3, base, PF_U) == 1, "faulted it in through do_file()");

    uint64_t f = frame_at(cr3, base);
    mm_ok(f != 0, "the page is present");
    uint64_t *pte = vmm_pte(cr3, base);
    mm_ok(pte && (*pte & VMM_PTE_FILE), "its PTE is marked FILE, not ANON");
    mm_eqf(check_pattern(mm_sim_ptr(f), 701), -1, "and holds file page 1's real bytes");
    mm_ok(pcache_holds(f), "the cache holds this frame too");

    unsigned n = rmap_count(f);
    unsigned rc = pmm_refcount(f);
    mm_eqi((long long)n, 1, "exactly one PTE maps it");
    mm_eqi((long long)rc, 2, "the refcount is two -- the PTE, and the cache");
    /* THE CLAIM THIS CASE EXISTS TO MAKE. rmap_count ALONE disagrees with
     * pmm_refcount -- the pre-pcache eligibility test would have refused this
     * frame forever, counting it skip_partial on every single sweep. Only
     * with pcache_holds(f) folded in do the three independently maintained
     * numbers agree, which is the property reclaim.h says must never be
     * loosened by adding a term that could turn a real disagreement into a
     * false agreement. */
    mm_ok(n != rc, "rmap_count(f) alone does NOT equal pmm_refcount(f) -- "
                    "the two-number test would refuse this frame forever");
    mm_eqi((long long)(n + (pcache_holds(f) ? 1u : 0u)), (long long)rc,
           "but rmap_count(f) + pcache_holds(f) DOES equal pmm_refcount(f)");

    uint64_t dc2_before = reclaim_dropped_cache();
    uint64_t got2 = reclaim_frames(1);
    mm_ok(got2 >= 1, "(c) reclaim evicted the mapped+cached page");
    mm_ok(reclaim_dropped_cache() > dc2_before, "evicted through the cache-drop tier");

    pte = vmm_pte(cr3, base);
    mm_ok(pte && *pte == 0, "the PTE is gone -- cleared, not turned into a swap entry");
    mm_ok(!pcache_holds(f), "the cache entry is gone too");
    mm_eqi((long long)pmm_refcount(f), 0,
           "BOTH references were accounted for -- the frame is fully free, not "
           "leaked and not dangling");

    vmm_free_space(cr3);
    pcache_file_put(fh);
    mm_eqi(rmap_audit(), 0, "reverse map clean after the page-cache cases");
    mm_eqi(pcache_audit(), 0, "page cache clean after the page-cache cases");
}

/* ======================================================================== */
int main(void)
{
    mm_sim_init(64);
    mm_sim_kernel_space();
    /* The allocator's low-water trace announces every new 1 MiB low; the
     * exhaustion case deliberately walks memory all the way down, so it would
     * print sixty lines. Widen the step: what is under test here is reclaim,
     * not the leak trace (which mm_pmm_test covers at its normal setting). */
    pmm_watch_step(1u << 20);

    /* Case ordering note: t_no_swap_device runs with the device absent, so it
     * has to come before swap is brought up. Everything after it has a device. */
    mm_ok(rmap_ready(), "the reverse map is up");
    reclaim_init();                       /* no device yet: swap stays off */
    mm_ok(!swap_ready(), "swap is off when there is no device");

    t_classify();
    t_no_swap_device();
    t_pcache();

    mm_sim_swap(16);                      /* 16 MiB of simulated swap */
    mm_ok(swap_init() == 0, "swap came up on the simulated device");
    mm_ok(swap_ready(), "and reports ready");
    printf("   swap: %llu slots (%llu MiB)\n",
           (unsigned long long)swap_slots_total(),
           (unsigned long long)(swap_slots_total() * 4096 / (1024 * 1024)));

    t_drop_tier();
    t_round_trip();
    t_pinning();
    t_clock();
    t_shared_and_fork();
    t_munmap_releases_slots();
    t_write_failure();
    t_exhaustion();
    t_thrash();

    mm_eqi(reclaim_bugs(), 0, "reclaim never found the reverse map disagreeing with a page table");
    mm_eqi(rmap_bugs(), 0, "the reverse map reported no bugs");
    mm_eqi((long long)(swap_io_errors() - g_deliberate_io_errors), 0,
           "no device errors beyond the ones the failure case caused on purpose");
    printf("   (%llu deliberate device errors, all in the write-failure case)\n",
           (unsigned long long)g_deliberate_io_errors);

    reclaim_report("end of test");
    mm_sim_swap_free();
    mm_sim_done();
    return mm_summary("mm_reclaim_test");
}
