#include <stdint.h>
#include <stddef.h>
#include "reclaim.h"
#include "rmap.h"
#include "swap.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "mm.h"
#include "mmhost.h"
#include "kprintf.h"

/* See reclaim.h for the design and the arguments. This file is the machine. */

#define PRESENT  0x1
#define WRITABLE 0x2
#define USER     0x4
/* NX is MM_PTE_NX (mm.h): one definition, because this file both PRESERVES it
 * into a swap entry and restores it on the way back in. */

void *memset(void *, int, size_t);

#ifdef MM_HOSTTEST
static inline uint64_t rc_cyc(void) { return 0; }
#else
static inline uint64_t rc_cyc(void)
{ uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo; }
#endif

static int      rc_on = 1;
static int      rc_inited;
static int      rc_in_pass;              /* reentrancy guard: see reclaim_on_alloc */
static uint64_t rc_hand;
static uint64_t rc_low, rc_high;

static uint64_t c_runs, c_scanned, c_dropped, c_swapped, c_second;
static uint64_t c_skip_unmapped, c_skip_pinned, c_skip_partial, c_skip_wide;
static uint64_t c_noslot, c_io, c_bugs, c_cycles, c_skip_busy, c_backoffs;
static uint64_t rc_backoff;   /* allocations to skip after a fruitless pass */
static uint64_t c_swapin, c_swapin_fail;

uint64_t reclaim_runs(void)          { return c_runs; }
uint64_t reclaim_scanned(void)       { return c_scanned; }
uint64_t reclaim_dropped(void)       { return c_dropped; }
uint64_t reclaim_swapped(void)       { return c_swapped; }
uint64_t reclaim_second_chance(void) { return c_second; }
uint64_t reclaim_skip_unmapped(void) { return c_skip_unmapped; }
uint64_t reclaim_skip_pinned(void)   { return c_skip_pinned; }
uint64_t reclaim_skip_partial(void)  { return c_skip_partial; }
uint64_t reclaim_skip_wide(void)     { return c_skip_wide; }
uint64_t reclaim_skip_busy(void)     { return c_skip_busy; }
uint64_t reclaim_backoffs(void)      { return c_backoffs; }
uint64_t reclaim_fail_noslot(void)   { return c_noslot; }
uint64_t reclaim_fail_io(void)       { return c_io; }
uint64_t reclaim_bugs(void)          { return c_bugs; }
uint64_t reclaim_cycles(void)        { return c_cycles; }
uint64_t reclaim_swapins(void)       { return c_swapin; }
uint64_t reclaim_swapin_fail(void)   { return c_swapin_fail; }

void reclaim_set_enabled(int on) { rc_on = on ? 1 : 0; }
int  reclaim_enabled(void)       { return rc_on && rmap_ready(); }
uint64_t reclaim_low(void)       { return rc_low; }
uint64_t reclaim_high(void)      { return rc_high; }

void reclaim_set_watermarks(uint64_t low, uint64_t high)
{
    rc_low = low;
    rc_high = (high > low) ? high : low + 1;
}

void reclaim_init(void)
{
    if (rc_inited) return;
    rc_inited = 1;
    uint64_t total = pmm_total_frames();
    /* 3% / 6% of RAM: on 512 MiB that is 15 MiB / 31 MiB. Low enough that a
     * healthy machine (peak 229 of 511 MiB) never triggers a pass, high enough
     * that the first pass has room to work in rather than starting from
     * nothing. `high` is the stop point, so one pass leaves real headroom
     * instead of being re-entered by the very next allocation. */
    reclaim_set_watermarks(total / 32, total / 16);
    swap_init();
    kprintf("[reclaim] watermarks: start below %d frames (%d MiB free), "
            "stop at %d (%d MiB); swap %s\n",
            (int)rc_low, (int)(rc_low * FRAME_SIZE / (1024 * 1024)),
            (int)rc_high, (int)(rc_high * FRAME_SIZE / (1024 * 1024)),
            swap_ready() ? swap_dev_name() : "OFF (drop tier only)");
}

/* --------------------------------------------------------- the candidate --
 *
 * The eligibility test, and the reason it is one expression rather than a
 * policy: every clause below is a way for a frame to have a reference that
 * reclaim cannot see and therefore cannot remove. */
/* THE NEGATIVE CONTROLS.
 *
 * An assertion nobody has watched fail is not a known-failing assertion. Both
 * of the clauses below are the ONLY thing standing between reclaim and a
 * specific, nameable corruption, so each has a build that removes it and a test
 * that is REQUIRED to fail in that build:
 *
 *   -DRECLAIM_NO_PIN_CHECK    reclaim ignores pmm_pincount(). A frame the
 *                             kernel is holding a pointer into gets evicted
 *                             underneath it. tests/unit/mm_reclaim_test.c's
 *                             pinning case must fail.
 *
 *   -DRECLAIM_NO_ZERO_CHECK   the cheap tier drops any anonymous page instead
 *                             of only a page that is currently all zeroes. A
 *                             page full of a process's data is then thrown away
 *                             and comes back as zeroes on the next fault. That
 *                             is the classic swap corruption -- no crash, no
 *                             log line, just different bytes -- and the
 *                             round-trip integrity case must fail on it.
 *
 * The point of naming them is that both clauses read like defensive paranoia
 * and neither is. Each is load-bearing, and the failing build is the proof. */
static int candidate(uint64_t f, unsigned *nmap)
{
    uint64_t phys = f * FRAME_SIZE;
    /* The cheap test first, and it is the one that answers for almost every
     * frame: page tables, heap arenas and the kernel image have no chain, and
     * on a 512 MiB machine they are most of the sweep. */
    if (!rmap_mapped(phys)) { c_skip_unmapped++; return 0; }
    unsigned n = rmap_count(phys);
    if (n == 0) { c_skip_unmapped++; return 0; }        /* raced away; not ours */
    if (rmap_incomplete(phys)) { c_skip_partial++; return 0; }
    unsigned rc = pmm_refcount(phys);
    if (rc == 0 || n != rc) {
        /* Either pmm thinks the frame is free while PTEs point at it (a bug
         * elsewhere, already reported by rmap_audit), or somebody holds a
         * reference that is not one of the PTEs we know -- the kernel is using
         * this user page. Both mean: not ours to take. */
        c_skip_partial++;
        return 0;
    }
#ifndef RECLAIM_NO_PIN_CHECK
    if (pmm_pincount(phys)) { c_skip_pinned++; return 0; }
#endif
    if (n > RECLAIM_MAX_SHARERS) { c_skip_wide++; return 0; }
    *nmap = n;
    return 1;
}

/* Every PTE that maps `f`, resolved and checked. Returns the count, or -1 if
 * the reverse map and the page tables disagree -- which must never happen and
 * is the one thing that would make eviction unsafe, so it aborts loudly rather
 * than evicting what it can find. */
static int gather(uint64_t f, uint64_t *cr3s, uint64_t *vas, uint64_t **ptes, int max)
{
    uint64_t phys = f * FRAME_SIZE;
    struct rmap_iter it;
    int n = 0;
    for (rmap_begin(&it, phys); n < max; ) {
        uint64_t cr3, va;
        if (!rmap_next(&it, &cr3, &va)) break;
        uint64_t *pte = vmm_pte(cr3, va);
        if (!pte || !(*pte & PRESENT) || !(*pte & USER) ||
            (*pte & MM_PTE_ADDR) != phys) {
            kprintf("[reclaim] BUG: rmap says cr3 %p va %p maps frame %p; "
                    "the page table says %p\n", (void *)cr3, (void *)va,
                    (void *)phys, (void *)(pte ? *pte : 0));
            c_bugs++;
            return -1;
        }
        /* Another core is executing this address space and has, or is about to
         * have, a TLB entry for this page. We cannot shoot it down from here
         * (tlb.h: a shootdown IPI under the BKL deadlocks), so the frame is not
         * ours to take. Reported as "busy" rather than folded into one of the
         * skip counters, because it is the only skip whose rate depends on how
         * many cores are running rather than on what the memory looks like. */
        if (vmm_space_busy_elsewhere(cr3)) { c_skip_busy++; return -2; }
        cr3s[n] = cr3; vas[n] = va; ptes[n] = pte; n++;
    }
    return n;
}

static int page_is_zero(uint64_t f)
{
    const uint64_t *p = (const uint64_t *)mm_p2v(f * FRAME_SIZE);
    for (int i = 0; i < FRAME_SIZE / 8; i++)
        if (p[i]) return 0;
    return 1;
}

static void flush_if_active(uint64_t cr3, uint64_t va)
{
    if ((mm_read_cr3() & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR))
        mm_invlpg(va);
}

/* --- TIER 1: drop. No device, no slot, no I/O. ---------------------------- */
static int try_drop(uint64_t f, uint64_t *cr3s, uint64_t *vas, uint64_t **ptes, int n)
{
    /* Re-derivable only if the fault that re-derives it produces these exact
     * bytes. That needs BOTH halves: the page must be anonymous (so the fault
     * path zero-fills it rather than killing the process) in every space that
     * maps it, and it must currently BE zero. */
    for (int i = 0; i < n; i++)
        if (!(*ptes[i] & VMM_PTE_ANON)) return 0;
#ifdef RECLAIM_NO_ZERO_CHECK
    (void)page_is_zero;     /* the negative control: drop it regardless */
#else
    if (!page_is_zero(f)) return 0;
#endif

    for (int i = 0; i < n; i++) {
        *ptes[i] = 0;
        rmap_remove(f * FRAME_SIZE, cr3s[i], vas[i]);
        flush_if_active(cr3s[i], vas[i]);
        pmm_free(f * FRAME_SIZE);          /* one reference per PTE; the last frees */
    }
    c_dropped++;
    return 1;
}

/* --- TIER 2: swap out. ---------------------------------------------------- */
static uint64_t swap_entry(uint64_t old, uint64_t slot)
{
    /* The other end of the bound documented in vmm.h: the slot field is 40
     * bits, and the bits above it are not spare -- bit 63 is NX. Masking here
     * and masking in vmm_pte_swap_slot() are the same statement made at both
     * ends; try_swap() refuses a slot that would not survive the round trip,
     * so the mask can never actually lose one. */
    uint64_t e = VMM_PTE_SWAP | ((slot & VMM_SWAP_MAX) << VMM_SWAP_SHIFT);
    if (old & WRITABLE)     e |= VMM_SWAP_W;
    if (old & VMM_PTE_COW)  e |= VMM_SWAP_COW;
    if (old & VMM_PTE_ANON) e |= VMM_SWAP_ANON;
    e |= (old & MM_PTE_NX);                   /* left in place; see vmm.h */
    return e;
}

/* ORDER, and why it is this order.
 *
 * The PTEs become swap entries BEFORE the write, not after. That looks backwards
 * -- it publishes a pointer to a slot whose contents are not there yet -- and it
 * is the only ordering that is safe once swap_write_page() is allowed to give up
 * the BKL while it waits for the device.
 *
 *   PTEs first:  a fault arriving mid-write finds a swap entry, calls
 *                swap_read_page() for that slot, and QUEUES on the same
 *                one-transfer-at-a-time flag the write holds. It therefore
 *                cannot read the slot until the write has finished. The
 *                serialisation that makes the device correct also makes this
 *                ordering correct, and it needs no separate writeback state.
 *
 *   Write first: a fault arriving mid-write finds a present, writable PTE and
 *                modifies the page under the transfer. The bytes on the device
 *                are then a mixture of before and after, and the process gets
 *                back a page it never had.
 *
 * The frame is pinned across the whole operation so that nothing else can
 * reclaim or reuse it while it is still the source of an in-flight write, and
 * it is freed only after the write has been acknowledged. If the write fails,
 * every PTE is put back exactly as it was -- the frame is still held, so the
 * page is not lost; the process just keeps its memory and reclaim counts a
 * failure. */
static int try_swap(uint64_t f, uint64_t *cr3s, uint64_t *vas, uint64_t **ptes, int n)
{
    if (!swap_ready()) return 0;

    uint64_t slot = swap_alloc_slot();
    if (slot == SWAP_NOSLOT) { c_noslot++; return 0; }
    if (slot > VMM_SWAP_MAX) {                  /* cannot be encoded; see vmm.h */
        swap_slot_put(slot);
        c_noslot++;
        return 0;
    }
    for (int i = 1; i < n; i++) swap_slot_ref(slot);    /* one reference per PTE */

    uint64_t saved[RECLAIM_MAX_SHARERS];
    uint64_t phys = f * FRAME_SIZE;

    pmm_pin(phys);
    for (int i = 0; i < n; i++) {
        saved[i] = *ptes[i];
        *ptes[i] = swap_entry(saved[i], slot);
        flush_if_active(cr3s[i], vas[i]);
    }

    if (swap_write_page(slot, mm_p2v(phys)) != 0) {
        for (int i = 0; i < n; i++) {
            *ptes[i] = saved[i];
            flush_if_active(cr3s[i], vas[i]);
        }
        for (int i = 0; i < n; i++) swap_slot_put(slot);
        pmm_unpin(phys);
        c_io++;
        return 0;
    }

    /* Committed. Drop the reverse-map entries and the references; the frame
     * goes back to the allocator as the last one lands. */
    pmm_unpin(phys);
    for (int i = 0; i < n; i++) {
        rmap_remove(phys, cr3s[i], vas[i]);
        pmm_free(phys);
    }
    c_swapped++;
    return 1;
}

/* ------------------------------------------------------------- the hand -- */

/* HOW MUCH SCANNING ONE CALL MAY DO, and why there has to be a limit.
 *
 * The first version of this had one budget: two full sweeps of the machine, on
 * the reasoning that the first sweep clears accessed bits and the second finds
 * what is genuinely idle. That is right for a call made by hand. It is
 * catastrophic for the call made from pmm_alloc(), because under sustained
 * pressure free memory sits just below the watermark and EVERY allocation
 * starts another two-sweep pass -- 98000 frames examined to hand out one 4 KiB
 * page. The machine stops being disk-bound and becomes scan-bound, which looks
 * exactly like a hang and was one: a workload whose swap traffic was only 187
 * pages spent minutes not finishing.
 *
 * So the two callers get two budgets. An explicit reclaim_frames() still sweeps
 * as far as it needs to, because its caller asked for a specific number of
 * frames and wants them. The allocation path gets a bounded slice: the clock
 * hand is persistent, so successive allocations continue the sweep where the
 * last one stopped, and the total scanning work becomes proportional to the
 * number of allocations rather than to allocations times the size of memory.
 *
 * The slice is generous enough to find something on a machine with a normal
 * amount of reclaimable memory (4096 frames is 16 MiB of scan) and small enough
 * that the worst case -- nothing reclaimable at all -- costs a bounded amount
 * per allocation instead of a sweep. */
#define RECLAIM_ALLOC_BUDGET 16384

static uint64_t reclaim_scan(uint64_t want, uint64_t budget)
{
    if (!reclaim_enabled() || want == 0) return 0;

    uint64_t total = pmm_total_frames();
    if (!total) return 0;

    uint64_t t0 = rc_cyc();
    uint64_t freed = 0;
    if (budget == 0 || budget > total * 2) budget = total * 2;

    c_runs++;
    while (freed < want && budget--) {
        uint64_t f = rc_hand;
        rc_hand = (rc_hand + 1 >= total) ? 0 : rc_hand + 1;
        c_scanned++;

        unsigned nmap = 0;
        if (!candidate(f, &nmap)) continue;

        uint64_t cr3s[RECLAIM_MAX_SHARERS], vas[RECLAIM_MAX_SHARERS];
        uint64_t *ptes[RECLAIM_MAX_SHARERS];
        int n = gather(f, cr3s, vas, ptes, RECLAIM_MAX_SHARERS);
        if (n <= 0) continue;

        /* SECOND CHANCE. The accessed bits of every PTE that maps this frame,
         * OR-ed: one sharer touching it is enough to keep it. Clearing them all
         * is the cost of the chance, and the next time the hand comes round the
         * answer means "not touched since the last sweep". */
        int referenced = 0;
        for (int i = 0; i < n; i++) if (*ptes[i] & VMM_PTE_ACCESSED) referenced = 1;
        if (referenced) {
            for (int i = 0; i < n; i++) {
                *ptes[i] &= ~VMM_PTE_ACCESSED;
                flush_if_active(cr3s[i], vas[i]);
            }
            c_second++;
            continue;
        }

        if (try_drop(f, cr3s, vas, ptes, n)) { freed++; continue; }
        if (try_swap(f, cr3s, vas, ptes, n)) { freed++; continue; }
    }

    c_cycles += rc_cyc() - t0;
    return freed;
}

uint64_t reclaim_frames(uint64_t want)
{
    /* No budget: sweep as far as it takes. This is the entry point for a caller
     * that asked for a number of frames, not the one on the allocation path. */
    return reclaim_scan(want, 0);
}

uint64_t reclaim_emergency(uint64_t want)
{
    return reclaim_scan(want, RECLAIM_ALLOC_BUDGET);
}

/* ------------------------------------------------------------- the trigger -- */

/* Called from pmm_alloc BEFORE it takes pmm_lock, so everything reclaim does
 * (pmm_free, pmm_refcount, pmm_pin) can take that lock normally.
 *
 * rc_in_pass is not an optimisation. reclaim must not allocate -- reclaim.h
 * says why -- and this guard is what turns "must not" into "cannot": if any
 * path inside a pass ever reaches pmm_alloc, the nested call serves the request
 * as it always did instead of starting a second pass on top of the first. A
 * counter would be a nicer diagnostic; a guard is the thing that keeps a bug
 * from being a hang. */
void reclaim_on_alloc(void)
{
    if (!rc_on || rc_in_pass || !rmap_ready()) return;
    uint64_t free_now = pmm_free_frames();
    if (free_now >= rc_low) return;

    /* BACKOFF, and the line it must not cross.
     *
     * A pass that found nothing means there is nothing to find, and the next
     * allocation a microsecond later will find nothing either; without a
     * backoff, a machine whose memory is all genuinely in use spends its
     * remaining life scanning.
     *
     * But a backoff that applies when memory is nearly GONE is not a
     * throttle, it is a way to kill processes. The first version of this backed
     * off unconditionally for 512 allocations, and under real pressure that was
     * long enough for the allocator to empty: pmm_alloc() returned 0, the
     * anonymous fault could not get a frame, and the workload died with
     * "app exception ... err=6" while tens of thousands of reclaimable pages sat
     * there. The backoff is therefore only honoured while there is still a
     * cushion; below half the low watermark every allocation gets a real
     * attempt, however fruitless the last one was. */
    if (rc_backoff && free_now > rc_low / 2) { rc_backoff--; return; }
    rc_backoff = 0;

    rc_in_pass = 1;
    uint64_t want = (rc_high > free_now) ? rc_high - free_now : 1;
    uint64_t got = reclaim_scan(want, RECLAIM_ALLOC_BUDGET);
    rc_in_pass = 0;
    if (!got) { rc_backoff = 512; c_backoffs++; }

    if (!got && free_now < rc_low / 4) {
        /* Nothing left to take, and memory is nearly gone. Say so once per
         * pass: a machine that is about to start failing allocations should
         * announce it rather than let the caller discover it as a null return. */
        kprintf("[reclaim] %d frames free and nothing reclaimable "
                "(scanned %d, %d dropped / %d swapped so far)\n",
                (int)free_now, (int)c_scanned, (int)c_dropped, (int)c_swapped);
    }
}

/* Bring reclaim up. Called from the fault path on the first fault taken FROM
 * RING 3, which is the one moment that is unambiguously safe: user code was
 * executing, so no kernel operation is half-finished on this thread and the
 * block layer (which swap_init reads and writes through) is idle and fully
 * enumerated. Doing it from pmm_init instead would run before there are any
 * block devices; doing it from kmain would need an edit to a file this line
 * does not own. */
void reclaim_late_init(void);
void reclaim_late_init(void)
{
    if (rc_inited) return;
    reclaim_init();
}

/* --------------------------------------------------------------- swap in -- */

int reclaim_swapin(uint64_t cr3, uint64_t va, uint64_t *pte, int active)
{
    uint64_t e = *pte;
    if (!vmm_pte_is_swap(e)) return 0;
    uint64_t slot = vmm_pte_swap_slot(e);
    if (!swap_ready() || slot == SWAP_NOSLOT) { c_swapin_fail++; return 0; }

    /* The one allocation on any reclaim-related path, and the reason pmm keeps
     * a reserve: a machine deep in a thrash must still be able to fault a page
     * back in, or it can never make progress out of the thrash.
     *
     * The reserve is a cushion, not a supply. A read-back pass over a working
     * set larger than memory faults pages in faster than the watermark notices,
     * and once the cushion is spent a failed swap-in KILLS THE PROCESS -- which
     * is what happened: a workload that had already survived being evicted died
     * reading itself back, with tens of thousands of droppable pages resident.
     * So an empty reserve forces a pass and asks again, exactly as the anonymous
     * fault does. Only after that is "out of memory" the truth rather than a
     * scheduling accident. */
    uint64_t f = pmm_alloc_reserve();
    if (!f) {
        reclaim_emergency(64);
        f = pmm_alloc_reserve();
    }
    if (!f) { c_swapin_fail++; return 0; }

    if (swap_read_page(slot, mm_p2v(f)) != 0) {
        pmm_free(f);
        c_swapin_fail++;
        return 0;
    }

    uint64_t flags = PRESENT | USER;
    /* A page that was writable, or that was writable-but-shared (COW), comes
     * back writable and PRIVATE. The sharing is not restored: each sharer that
     * faults gets its own copy of identical bytes. See swap.h -- the contents
     * are right, the memory is not shared any more. */
    if (e & (VMM_SWAP_W | VMM_SWAP_COW)) flags |= WRITABLE;
    if (e & VMM_SWAP_ANON) flags |= VMM_PTE_ANON;
    flags |= (e & MM_PTE_NX);

    *pte = (f & MM_PTE_ADDR) | flags;
    rmap_add(f, cr3, va);
    swap_slot_put(slot);
    if (active) mm_invlpg(va);
    c_swapin++;
    return 1;
}

/* ---------------------------------------------------------------- report -- */

void reclaim_report(const char *tag)
{
    const char *t = tag ? tag : "-";
    if (!rmap_ready()) {
        kprintf("[reclaim] %s: DISABLED (no reverse map)\n", t);
        return;
    }
    uint64_t evicted = c_dropped + c_swapped;
    kprintf("[reclaim] %s: %s, %d passes, %d frames scanned, "
            "%d evicted (%d dropped free + %d swapped), %d second chances\n",
            t, rc_on ? "on" : "off", (int)c_runs, (int)c_scanned,
            (int)evicted, (int)c_dropped, (int)c_swapped, (int)c_second);
    kprintf("[reclaim] %s: skipped %d unmapped / %d pinned / %d partially-known / "
            "%d too-shared / %d running elsewhere; failed %d no-slot / %d io; %d bugs\n",
            t, (int)c_skip_unmapped, (int)c_skip_pinned, (int)c_skip_partial,
            (int)c_skip_wide, (int)c_skip_busy, (int)c_noslot, (int)c_io, (int)c_bugs);
    kprintf("[reclaim] %s: %d swap-ins (%d failed); %d kcycles scanning "
            "(%d per frame evicted)\n",
            t, (int)c_swapin, (int)c_swapin_fail, (int)(c_cycles / 1000),
            (int)(evicted ? c_cycles / evicted : 0));
    rmap_report(t);
    swap_report(t);
}
