#include "mmguard.h"
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
#include "pcache.h"
#include "oom.h"

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

/* A bounded allocation slice is not a complete unsuccessful clock round.
 * Two revolutions allow one to clear accessed bits and the next to evict;
 * declaring OOM after two slices would kill with untouched candidates farther
 * round the hand on a large machine. This is a scan bound, not a hardware
 * timeout: 128 MiB / 4 KiB * 2 = 65,536 frames in the host pressure case.
 * No watermark, reserve or page-cache policy is changed by this limit.
 * Reference: Linux v6.12 mm/vmscan.c, wakeup_kswapd(), also stops waking a
 * hopeless scanner after kswapd_failures reaches MAX_RECLAIM_RETRIES:
 * https://github.com/torvalds/linux/blob/v6.12/mm/vmscan.c
 * The two-revolution value here comes from this clock's second chance, not
 * Linux's LRU retry count. Model only; not verified on X79-VG4 hardware. */
#define RECLAIM_STALL_ROUNDS 2u
static uint64_t rc_unproductive;

/* THE STALL WORD. rc_unproductive holds, in one atomic word, the episode's
 * count of fruitless frames scanned and the state of the one scanner, so every
 * decision about the episode is made on a single read and committed by one CAS:
 *   bit 63      RC_STALL_SCANNING  a scan is between its start and its publish
 *   bit 62      RC_STALL_STALE     the episode ended while that scan ran
 *   bits 0..61  fruitless frames scanned in this episode
 *
 * Corrections, kept because they explain the shape. The first version stored
 * the count alone and ended an episode with a plain store, so a scan that had
 * finished two revolutions before an OOM decision added them back after it.
 * The second tagged the count with a 16-bit generation; that still let the
 * watermark path decide on two separate reads (the count, then a separate
 * scanner flag) while a scan published in between, and a generation bumped
 * once per end could wrap after 65,536 ends during one scan. A STALE bit has
 * neither problem: it is set in the same CAS that ends the episode, and only
 * the scan it condemns clears it. Model only; not verified on X79-VG4. */
#define RC_STALL_SCANNING   (1ull << 63)
#define RC_STALL_STALE      (1ull << 62)
#define RC_STALL_COUNT_MASK (RC_STALL_STALE - 1)

/* Why an episode ends; the watermark mutation control below needs to know. */
#define RC_END_DECISION  0   /* an OOM decision (reclaim_stall_reset) */
#define RC_END_WATERMARK 1   /* free memory at or above low */
#define RC_END_DISABLED  2   /* reclaim switched off, or found off */

#ifdef MM_HOSTTEST
/* Host seams for tests/hw/oom/pressure.c. reclaim_test_before_account runs
 * between a scan and its publish, where another core's end can land;
 * reclaim_test_in_end runs between rc_episode_end()'s read and its CAS, where
 * a scan can publish. Both are one-shot and unset outside the cases using them. */
void (*reclaim_test_before_account)(void);
void (*reclaim_test_in_end)(void);
#endif

/* End the current episode: zero the count and, if a scan is running, mark it
 * stale so its publish is discarded. Nothing to do when nothing is counted and
 * no scan runs -- the common case above low costs one load. A CAS that fails
 * because a scan started or published meanwhile re-reads and decides again. */
static void rc_episode_end(int why)
{
    uint64_t word = __atomic_load_n(&rc_unproductive, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!(word & (RC_STALL_COUNT_MASK | RC_STALL_SCANNING))) {
            return;
        }
        uint64_t next = word & RC_STALL_SCANNING;
        if (next) {
            next |= RC_STALL_STALE;
        }
#ifdef OOM_NEGCTL_WATERMARK_NOT_STALE
        /* Mutation control: the watermark path zeroes the count only. */
        if (why == RC_END_WATERMARK) {
            next = word & (RC_STALL_SCANNING | RC_STALL_STALE);
        }
#else
        (void)why;
#endif
#ifdef MM_HOSTTEST
        if (reclaim_test_in_end) {
            void (*hook)(void) = reclaim_test_in_end;
            reclaim_test_in_end = NULL;
            hook();
        }
#endif
        if (__atomic_compare_exchange_n(&rc_unproductive, &word, next, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
#ifdef OOM_NEGCTL_END_NO_RETRY
        /* Mutation control: act on the stale read and give up. */
        return;
#endif
    }
}

/* Claim the one scan. 0 if another scan is in flight: an allocating caller
 * never waits for reclaim (see reclaim_scan). */
static int rc_scan_begin(void)
{
    uint64_t word = __atomic_load_n(&rc_unproductive, __ATOMIC_RELAXED);
    for (;;) {
        if (word & RC_STALL_SCANNING) {
            return 0;
        }
        if (__atomic_compare_exchange_n(&rc_unproductive, &word, word | RC_STALL_SCANNING,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 1;
        }
    }
}

/* Release the scan and account for it. Progress ends the episode; a stale scan
 * belongs to an episode that already ended and adds nothing; otherwise its
 * fruitless frames are added, capped at the stall limit. */
static void rc_scan_publish(int progress, uint64_t scanned)
{
    uint64_t limit = pmm_total_frames() * RECLAIM_STALL_ROUNDS;
    uint64_t word = __atomic_load_n(&rc_unproductive, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t count = word & RC_STALL_COUNT_MASK;
        uint64_t next = count;
        int stale = (word & RC_STALL_STALE) != 0;
#ifdef OOM_NEGCTL_STALL_IGNORE_STALE
        stale = 0;  /* mutation control: publish across an ended episode */
#endif
        if (progress) {
            next = 0;
        } else if (!stale) {
            uint64_t remaining = count < limit ? limit - count : 0;
            next = count + (scanned < remaining ? scanned : remaining);
        }
        if (__atomic_compare_exchange_n(&rc_unproductive, &word, next, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

/* What is left of the episode's two revolutions. reclaim_scan() caps its
 * budget with it, so the bound is on frames actually scanned, not only on
 * the count published: with a total that is not a whole number of
 * allocation slices, the uncapped slices overran it by up to a slice. */
static uint64_t rc_scan_allowance(void)
{
    uint64_t limit = pmm_total_frames() * RECLAIM_STALL_ROUNDS;
    uint64_t count = __atomic_load_n(&rc_unproductive, __ATOMIC_RELAXED) & RC_STALL_COUNT_MASK;
    return count < limit ? limit - count : 0;
}

/* Fruitless frames scanned in the current episode. */
static uint64_t rc_stall_count(void)
{
    return __atomic_load_n(&rc_unproductive, __ATOMIC_RELAXED) & RC_STALL_COUNT_MASK;
}

/* Report exhaustion only while pressure still exists. An allocation can leave
 * a few hundred free frames and still livelock across repeated empty scans;
 * waiting for pmm_alloc() to return zero never hands that case to OOM.
 * Atomic state lets the scanner publish the evidence without invoking policy
 * under a caller's AS/heap/IRQ locks. Recovery above low starts a new episode. */
int reclaim_stalled(void)
{
    if (!reclaim_enabled()) {
#ifndef OOM_NEGCTL_DISABLE_KEEPS_EPISODE
        rc_episode_end(RC_END_DISABLED);
#endif
        return 0;
    }
    if (pmm_free_frames() >= rc_low) {
        rc_episode_end(RC_END_WATERMARK);
        return 0;
    }
#ifdef OOM_NEGCTL_RECLAIM_UNBOUNDED
    (void)rc_stall_count;  /* the count is kept, only never consulted */
    return 0;  /* old behavior: scan again forever, never hand pressure to OOM */
#else
    uint64_t limit = pmm_total_frames() * RECLAIM_STALL_ROUNDS;
    return limit && rc_stall_count() >= limit;
#endif
}

/* See oom.h: an OOM decision ends the episode like the watermark does. */
void reclaim_stall_reset(void)
{
    rc_episode_end(RC_END_DECISION);
}

#ifdef MM_HOSTTEST
/* Drive the scanner's two steps without a pass, so a case can place a publish
 * exactly between rc_episode_end()'s read and CAS. */
int reclaim_test_scan_begin(void)
{
    return rc_scan_begin();
}

void reclaim_test_scan_publish(uint64_t scanned)
{
    rc_scan_publish(0, scanned);
}
#endif

static uint64_t c_runs, c_scanned, c_dropped, c_swapped, c_second;
static uint64_t c_skip_unmapped, c_skip_pinned, c_skip_partial, c_skip_wide;
static uint64_t c_noslot, c_io, c_bugs, c_cycles, c_skip_busy, c_backoffs;
static uint64_t rc_backoff;   /* allocations to skip after a fruitless pass */
static uint64_t c_swapin, c_swapin_fail;
/* TIER 1's two producers, counted separately -- see pcache.h and the note
 * above try_drop_cached(). c_dropped is still their sum (every increment site
 * bumps exactly one of these two AND c_dropped), so nothing that already reads
 * reclaim_dropped() as "tier 1 total" has to change. */
static uint64_t c_dropped_zero, c_dropped_cache;

/* THE THIRD NUMBER, WATCHED RATHER THAN ASSUMED.
 *
 * candidate() adds pcache_holds(f) to the count that must equal
 * pmm_refcount(f), and pcache.h argues at length that this keeps a cached page
 * evictable instead of structurally pinned. That argument is checkable and was
 * never checked: c_skip_partial lumps together every frame with a reference
 * reclaim cannot account for, so a cached page failing the test looked
 * identical to a kernel buffer, and "the page cache made a page unevictable"
 * -- the exact failure the pin discipline exists to prevent -- would have
 * shown up as a bigger number in a bucket that is nonzero anyway.
 *
 * Split out here, and both halves are needed to read either:
 *   c_seen_cached          frames the sweep met that the cache holds. Without
 *                          it, a zero in the next counter can mean "nothing
 *                          went wrong" or "no cached page was ever swept".
 *   c_skip_partial_cached  ...of which the arithmetic did not add up. This is
 *                          the number that must stay 0. Nonzero means some
 *                          reference is held by neither a PTE nor the cache,
 *                          on a page-cache frame -- i.e. the shared-page case
 *                          this line was asked about, failing.
 *   c_skip_wide_cached     ...and the ones refused for having more sharers
 *                          than gather() can hold. A file page shared by 17
 *                          processes is the one legitimate way a cached page
 *                          is unevictable, and it is a different fact from a
 *                          broken refcount. */
static uint64_t c_seen_cached, c_skip_partial_cached, c_skip_wide_cached;

uint64_t reclaim_runs(void)          { return c_runs; }
uint64_t reclaim_scanned(void)       { return c_scanned; }
uint64_t reclaim_dropped(void)       { return c_dropped; }
uint64_t reclaim_dropped_zero(void)  { return c_dropped_zero; }
uint64_t reclaim_dropped_cache(void) { return c_dropped_cache; }
uint64_t reclaim_swapped(void)       { return c_swapped; }
uint64_t reclaim_second_chance(void) { return c_second; }
uint64_t reclaim_skip_unmapped(void) { return c_skip_unmapped; }
uint64_t reclaim_skip_pinned(void)   { return c_skip_pinned; }
uint64_t reclaim_skip_partial(void)  { return c_skip_partial; }
uint64_t reclaim_seen_cached(void)         { return c_seen_cached; }
uint64_t reclaim_skip_partial_cached(void) { return c_skip_partial_cached; }
uint64_t reclaim_skip_wide_cached(void)    { return c_skip_wide_cached; }
uint64_t reclaim_skip_wide(void)     { return c_skip_wide; }
uint64_t reclaim_skip_busy(void)     { return c_skip_busy; }
uint64_t reclaim_backoffs(void)      { return c_backoffs; }
uint64_t reclaim_fail_noslot(void)   { return c_noslot; }
uint64_t reclaim_fail_io(void)       { return c_io; }
uint64_t reclaim_bugs(void)          { return c_bugs; }
uint64_t reclaim_cycles(void)        { return c_cycles; }
uint64_t reclaim_swapins(void)       { return c_swapin; }
uint64_t reclaim_swapin_fail(void)   { return c_swapin_fail; }

/* Switching reclaim off ends the episode too: the first version only set
 * rc_on, so a count, or a scan in flight, survived an off/on cycle and the
 * next fault could be refused without a fresh check. */
void reclaim_set_enabled(int on)
{
    rc_on = on ? 1 : 0;
#ifndef OOM_NEGCTL_DISABLE_KEEPS_EPISODE
    if (!rc_on) {
        rc_episode_end(RC_END_DISABLED);
    }
#endif
}

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
    int zero=0;
    if (!__atomic_compare_exchange_n(&rc_inited,&zero,1,0,
                              __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
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
    __atomic_store_n(&rc_inited,2,__ATOMIC_RELEASE);
}

/* --------------------------------------------------------- the candidate --
 *
 * The eligibility test, and the reason it is one expression rather than a
 * policy: every clause below is a way for a frame to have a reference that
 * reclaim cannot see and therefore cannot remove.
 *
 * THE THIRD NUMBER. pcache.h's refcount decision (read it before touching
 * this function) adds pcache_holds(f) -- 0 or 1, from a structure maintained
 * independently of both pmm and the reverse map -- to the count that must
 * agree with pmm_refcount(f). A cache entry is a reference with NO leaf PTE
 * behind it, structurally the same shape as a page-table page or a kheap
 * arena, both of which this test already excludes because THEIR rmap count is
 * 0 while their refcount is not. Without the extra term a cached page would
 * fail exactly the same way and be unevictable forever -- pcache.h calls that
 * outcome "a leak with a hash table in front of it" and it is the one thing
 * this function must not do.
 *
 * The property that made the original two-number test worth having is kept
 * exactly: three independently maintained numbers have to agree, and any
 * disagreement STOPS the eviction. Adding a term that could ever make a
 * disagreement look like an agreement would be the bug this whole file exists
 * to prevent, so pcache_holds() is added on both sides of nothing -- it comes
 * in once, into the same equality every other reference has to satisfy. */
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
static int candidate_extra(uint64_t f, unsigned *nmap, int *cached, unsigned extra)
{
    uint64_t phys = f * FRAME_SIZE;
    /* Read once, up front: it is one aligned load (pcache.h), no lock, and
     * every clause below needs it, including the fast-path bailout that used
     * to be the first thing this function did. */
    unsigned held = pcache_holds(phys) ? 1u : 0u;

    /* The cheap test first, and it is the one that answers for almost every
     * frame: page tables, heap arenas and the kernel image have no chain and
     * no cache entry, and on a 512 MiB machine they are most of the sweep.
     * `held` widens this exactly one way: a page the cache holds but NOTHING
     * maps -- read once through pcache_pread(), or every PTE dropped while
     * the file page lived on -- has no rmap chain either, and it must not be
     * turned away here the way ordinary kernel memory is. It is, per
     * pcache.h, the cheapest reclaimable frame on the machine. */
    if (!rmap_mapped(phys) && !held) { c_skip_unmapped++; return 0; }
    if (held) c_seen_cached++;      /* the denominator for the two below */
    unsigned n = rmap_count(phys);
    if (n == 0 && !held) { c_skip_unmapped++; return 0; }   /* raced away; not ours */
    if (rmap_incomplete(phys)) { c_skip_partial++; c_skip_partial_cached += held; return 0; }
    unsigned rc = pmm_refcount(phys);
    if (rc == 0 || n + held + extra != rc) {
        /* Either pmm thinks the frame is free while PTEs (or the cache) point
         * at it (a bug elsewhere, already reported by rmap_audit or
         * pcache_audit), or somebody holds a reference that is neither one of
         * the PTEs we know nor the cache's own -- the kernel is using this
         * user page. Both mean: not ours to take. */
        c_skip_partial++;
        c_skip_partial_cached += held;
        return 0;
    }
#ifndef RECLAIM_NO_PIN_CHECK
    if (pmm_pincount(phys)) { c_skip_pinned++; return 0; }
#endif
    if (n > RECLAIM_MAX_SHARERS) { c_skip_wide++; c_skip_wide_cached += held; return 0; }
    *nmap = n;
    *cached = (int)held;
    return 1;
}

static int candidate(uint64_t f,unsigned *nmap,int *cached)
{ return candidate_extra(f,nmap,cached,0); }

/* A reverse-map snapshot contains values, never pointers into its node pool.
 * Try every owning AS before following a PTE. Failed trylocks unwind without
 * waiting: allocation can enter reclaim while already owning an AS guard. */
struct reclaim_hold {
    struct mm_guard guards[RECLAIM_MAX_SHARERS];
    uint64_t cr3[RECLAIM_MAX_SHARERS], va[RECLAIM_MAX_SHARERS];
    uint64_t *pte[RECLAIM_MAX_SHARERS], original[RECLAIM_MAX_SHARERS];
    int n, locked, frozen, ref, cached;
    uint64_t phys;
};
static void reclaim_release(struct reclaim_hold *h)
{
    if (h->frozen) {
        for (int i=0;i<h->n;i++) {
            uint64_t e=*h->pte[i];
            if ((e & (PRESENT|MM_PTE_ADDR))==(PRESENT|h->phys) &&
                (h->original[i]&WRITABLE)) __atomic_fetch_or(h->pte[i], WRITABLE, __ATOMIC_RELAXED);
        }
        for (int i=0;i<h->n;i++) vmm_flush_space(h->cr3[i]);
    }
    if (h->ref) pmm_free(h->phys);
    while (h->locked) mm_guard_end(&h->guards[--h->locked]);
}
static int gather_hold(uint64_t f,struct reclaim_hold *h)
{
    h->phys=f*FRAME_SIZE;
    h->n=rmap_snapshot(h->phys,h->cr3,h->va,RECLAIM_MAX_SHARERS);
    if (h->n<0) return -1;
    for (int i=0;i<h->n;i++) {
        h->guards[i]=mm_guard_try(h->cr3[i]);
        if (!h->guards[i].held) { c_skip_busy++; return -1; }
        h->locked++;
    }
    /* Nodes may have vanished or moved while we acquired their AS locks.
     * That is ordinary concurrency, not a corruption report. */
    if (rmap_count(h->phys)!=(unsigned)h->n) return -1;
    for (int i=0;i<h->n;i++) {
        if (!mm_space_live(h->cr3[i])) return -1;
        h->pte[i]=vmm_pte(h->cr3[i],h->va[i]);
        if (!h->pte[i] || (*h->pte[i]&(PRESENT|USER|MM_PTE_ADDR))!=
                           (PRESENT|USER|h->phys)) return -1;
        h->original[i]=*h->pte[i];
    }
    if (pmm_ref(h->phys)<0) return -1;
    h->ref=1;
    unsigned n=0; int cache=0;
    if (!candidate_extra(f,&n,&cache,1) || n!=(unsigned)h->n) return -1;
    h->cached=cache;
    return h->n;
}
static void reclaim_freeze(struct reclaim_hold *h)
{
    /* AS locks stop kernel writers, not ring 3. Revoke every writable alias
     * and complete the shootdown BEFORE inspecting zeroes or starting DMA.
     * A fault on these temporary read-only PTEs waits for our AS guard. */
    for (int i=0;i<h->n;i++) {
        h->original[i]=*h->pte[i];
        __atomic_fetch_and(h->pte[i], ~(uint64_t)WRITABLE, __ATOMIC_RELAXED);
    }
    for (int i=0;i<h->n;i++) vmm_flush_space(h->cr3[i]);
    h->frozen=1;
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
    }
    for (int i=0;i<n;i++) vmm_flush_space(cr3s[i]);
    for (int i=0;i<n;i++) pmm_free(f*FRAME_SIZE);
    c_dropped++;
    c_dropped_zero++;
    return 1;
}

/* --- TIER 1, SECOND PRODUCER: drop a page the page cache holds. -----------
 *
 * Beside try_drop(), deliberately the same shape, and the one difference is
 * where the "still reconstructible" proof comes from. try_drop() has to
 * inspect the page's 4096 bytes because an anonymous page's re-derivation
 * (zero-fill) is only correct if the page genuinely IS still zero. A
 * file-backed page needs no such check: its re-derivation is "read the file
 * again", which is correct unconditionally -- that is what pcache_holds(f)
 * being true MEANS, and candidate() already confirmed it against the same
 * refcount every other clause here is trusted for.
 *
 * `n` may be 0. A page read once through pcache_pread() and never mapped has
 * no PTE at all, and the loop below simply does not run -- there is nothing
 * to unmap, only the cache's own reference to drop, which is the cheapest
 * eviction this file can do: no PTE to tear down, no TLB shootdown, no device.
 *
 * Every PTE that DOES map it is required to agree it is a page-cache page
 * (vmm.h: FILE and ANON are mutually exclusive by construction, one set by
 * do_anon(), the other by do_file(), never both). That is not paranoia added
 * for this function -- try_drop() makes the same demand of VMM_PTE_ANON, for
 * the same reason: if a PTE here disagreed with what pcache_holds() says the
 * frame is, dropping it would be reclaim inventing a fact rmap.h says it must
 * never invent, so it declines instead. It should never actually happen: the
 * VMA that produced the PTE and the pcache entry candidate() found both trace
 * back to the same do_file() call. */
static int try_drop_cached(uint64_t f, uint64_t *cr3s, uint64_t *vas, uint64_t **ptes, int n)
{
    for (int i = 0; i < n; i++)
        if (!(*ptes[i] & VMM_PTE_FILE)) return 0;

    uint64_t phys = f * FRAME_SIZE;
    for (int i = 0; i < n; i++) {
        *ptes[i] = 0;
        rmap_remove(phys, cr3s[i], vas[i]);
        flush_if_active(cr3s[i], vas[i]);
    }
    for (int i=0;i<n;i++) vmm_flush_space(cr3s[i]);
    for (int i=0;i<n;i++) pmm_free(phys);
    /* THE HOOK pcache.h asks for: O(1) through pc_of_frame[], called here,
     * under the same big kernel lock that made the eviction decision, exactly
     * as pcache.h's refcount section requires. This is the cache's own
     * reference -- the last one, since candidate() already proved
     * n + held == refcount -- so the frame goes back to the allocator the
     * instant this returns. Skipping this call is the single worst outcome
     * available in this file: the entry would dangle onto a frame the
     * allocator has already handed to somebody else. */
    pcache_forget_frame(phys);
    c_dropped++;
    c_dropped_cache++;
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
static int try_swap(uint64_t f, uint64_t *cr3s, uint64_t *vas, uint64_t **ptes, int n, const uint64_t *original)
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
        saved[i] = original[i];
        *ptes[i] = swap_entry(saved[i], slot);
        flush_if_active(cr3s[i], vas[i]);
    }

    for (int i=0;i<n;i++) vmm_flush_space(cr3s[i]);
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

#ifdef MM_WIDE_VERIFY
/* The test can target only an eligible page in a detached address space. This
 * selects the victim, not its outcome: production gather/try_swap still own
 * every PTE transition, pin, disk write and reference release. */
int reclaim_verify_evict(uint64_t phys);
int reclaim_verify_evict(uint64_t phys)
{
    unsigned count=0;int cached=0;
    if((phys&4095)||!candidate(phys/FRAME_SIZE,&count,&cached)||cached)return 0;
    struct reclaim_hold h __attribute__((cleanup(reclaim_release)))={0};
    int n=gather_hold(phys/FRAME_SIZE,&h);
    if(n<=0)return 0;
    reclaim_freeze(&h);
    return try_swap(phys/FRAME_SIZE,h.cr3,h.va,h.pte,n,h.original);
}
#endif

static uint64_t reclaim_scan_locked(uint64_t want, uint64_t budget)
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
        int cached = 0;
        if (!candidate(f, &nmap, &cached)) continue;

        struct reclaim_hold h __attribute__((cleanup(reclaim_release)))={0};
        int n=gather_hold(f,&h);
        if (n<0) continue;
        cached=h.cached;
        uint64_t *cr3s=h.cr3,*vas=h.va,**ptes=h.pte;
        if (n>0) {
            /* SECOND CHANCE. The accessed bits of every PTE that maps this
             * frame, OR-ed: one sharer touching it is enough to keep it.
             * Clearing them all is the cost of the chance, and the next time
             * the hand comes round the answer means "not touched since the
             * last sweep". A cache-only frame has no PTE and therefore no
             * accessed bit to consult -- there is nothing to sample, so it
             * gets no second chance and is evicted the first time the hand
             * reaches it, which is correct: it costs nothing to re-derive
             * either way. */
            int referenced = 0;
            for (int i = 0; i < n; i++) if (*ptes[i] & VMM_PTE_ACCESSED) referenced = 1;
            if (referenced) {
                for (int i = 0; i < n; i++) {
                    __atomic_fetch_and(ptes[i], ~(uint64_t)VMM_PTE_ACCESSED, __ATOMIC_RELAXED);
                    flush_if_active(cr3s[i], vas[i]);
                }
                c_second++;
                continue;
            }
        }

        reclaim_freeze(&h);
        /* pcache.h's page wins over the anonymous zero page: it is checked
         * first because it needs no 4 KiB comparison to know it is
         * reconstructible, and because a page cannot be both (VMM_PTE_ANON
         * and VMM_PTE_FILE are mutually exclusive; try_drop()'s ANON check
         * would simply decline a cached page and fall through to try_swap(),
         * which would write a page the file already holds -- the exact
         * "writing a page you could have thrown away" mistake reclaim.h's
         * ordering rule exists to prevent). */
        if (cached) {
            if (try_drop_cached(f, cr3s, vas, ptes, n)) { freed++; continue; }
            continue;   /* pcache_forget_frame() cannot fail; nothing else to try */
        }

        if (try_drop(f, cr3s, vas, ptes, n)) { freed++; continue; }
        if (try_swap(f, cr3s, vas, ptes, n, h.original)) { freed++; continue; }
    }

    c_cycles += rc_cyc() - t0;
    return freed;
}

/* One clock hand/scanner. An allocating caller never waits for reclaim:
 * that scanner may itself need this caller's AS/FS lock to finish. The claim
 * is RC_STALL_SCANNING in the stall word (rc_scan_begin). */
static uint64_t reclaim_scan(uint64_t want,uint64_t budget)
{
#ifndef OOM_NEGCTL_DIRECT_STALE_ALLOWANCE
    /* A direct caller (reclaim_frames) need not come through
     * reclaim_on_alloc() or reclaim_stalled(), which end a recovered
     * episode. End it here, before the scan is claimed, so the allowance
     * below belongs to the current episode and not to a spent old one. */
    if (pmm_free_frames() >= rc_low) {
        rc_episode_end(RC_END_WATERMARK);
    }
#endif
    if (!rc_scan_begin()) return 0;
#ifndef OOM_NEGCTL_BUDGET_UNCAPPED
    /* Never scan past the episode's two revolutions; once they are spent
     * there is nothing to look at until the episode ends. */
    uint64_t allowance = rc_scan_allowance();
    if (!allowance) {
        rc_scan_publish(pmm_free_frames() >= rc_low, 0);
        return 0;
    }
    if (!budget || budget > allowance) {
        budget = allowance;
    }
#else
    (void)rc_scan_allowance;  /* mutation control: slices may overrun the bound */
#endif
    uint64_t before = c_scanned;
    uint64_t n=reclaim_scan_locked(want,budget);
#ifdef MM_HOSTTEST
    if (reclaim_test_before_account) {
        void (*hook)(void) = reclaim_test_before_account;
        reclaim_test_before_account = NULL;
        hook();
    }
#endif
    /* Only work actually performed by this scanner counts. A failed scanner
     * trylock is not a fruitless round; another core may be freeing pages.
     * Any freed page resets the consecutive no-progress bound. */
    rc_scan_publish(n || pmm_free_frames() >= rc_low, c_scanned - before);
    return n;
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
    if (!rc_on) {
        /* Off: no episode may carry over to when it is switched back on. */
#ifndef OOM_NEGCTL_DISABLE_KEEPS_EPISODE
        rc_episode_end(RC_END_DISABLED);
#endif
        return;
    }
    if (__atomic_load_n(&rc_inited,__ATOMIC_ACQUIRE)!=2 || !rmap_ready()) return;
    uint64_t free_now = pmm_free_frames();
    if (free_now >= rc_low) {
        rc_episode_end(RC_END_WATERMARK);
        return;
    }
    /* Stop repeating the scan after a complete failed episode. The next user
     * fault hands it to OOM outside its AS guard, even if this allocation can
     * still draw an ordinary frame above the reserve. */
    if (reclaim_stalled()) return;

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
    /* Own the allocation backoff state. Never wait for another allocator. */
    if (__atomic_exchange_n(&rc_in_pass,1,__ATOMIC_ACQUIRE)) return;
    if (rc_backoff && free_now > rc_low / 2) {
        rc_backoff--;
        __atomic_store_n(&rc_in_pass,0,__ATOMIC_RELEASE);
        return;
    }
    rc_backoff = 0;
    uint64_t want = (rc_high > free_now) ? rc_high - free_now : 1;
    uint64_t got = reclaim_scan(want, RECLAIM_ALLOC_BUDGET);
    if (!got) { rc_backoff = 512; c_backoffs++; }
    __atomic_store_n(&rc_in_pass,0,__ATOMIC_RELEASE);

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
    if (__atomic_load_n(&rc_inited,__ATOMIC_ACQUIRE)) return;
    reclaim_init();
}

/* --------------------------------------------------------------- swap in -- */

int reclaim_swapin(uint64_t cr3, uint64_t va, uint64_t *pte, int active)
{
    MM_GUARD(cr3);
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
    /* Swap restores a user payload, not a legacy DMA descriptor. High RAM
     * uses the existing counted block-layer bounce path until DMA migration. */
    uint64_t f = pmm_alloc_reserve_any();
    if (!f) {
        reclaim_emergency(64);
        f = pmm_alloc_reserve_any();
    }
    if (!f) {
#ifdef OOM_NEGCTL_SWAPIN_PLAIN_FAIL
        c_swapin_fail++;
        return 0;
#else
        /* Correction to the old "c_swapin_fail++; return 0": no frame is
         * not a corrupt slot or a failed read. Preserve both PTE and slot and
         * let fault.c retry/choose a victim after releasing this AS guard.
         * Calling OOM here could park with a stale leaf pointer still live.
         * Reference: Linux v6.12 mm/memory.c, do_swap_page(), likewise reports
         * failed swap-in allocation as VM_FAULT_OOM, distinct from read errors:
         * https://github.com/torvalds/linux/blob/v6.12/mm/memory.c */
        return OOM_SWAPIN_NOMEM;
#endif
    }

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
            "%d evicted (%d dropped free [%d zero + %d cache] + %d swapped), "
            "%d second chances\n",
            t, rc_on ? "on" : "off", (int)c_runs, (int)c_scanned,
            (int)evicted, (int)c_dropped, (int)c_dropped_zero,
            (int)c_dropped_cache, (int)c_swapped, (int)c_second);
    kprintf("[reclaim] %s: skipped %d unmapped / %d pinned / %d partially-known / "
            "%d too-shared / %d running elsewhere; failed %d no-slot / %d io; %d bugs\n",
            t, (int)c_skip_unmapped, (int)c_skip_pinned, (int)c_skip_partial,
            (int)c_skip_wide, (int)c_skip_busy, (int)c_noslot, (int)c_io, (int)c_bugs);
    /* THE PAGE CACHE'S SHARE OF THE SWEEP, and the one number in this file
     * that must be zero. See the counter block at the top for why it is
     * printed with its denominator rather than alone. */
    kprintf("[reclaim] %s: page-cache frames met %d, dropped %d, "
            "UNACCOUNTED-FOR REFERENCE %d, too-shared %d\n",
            t, (int)c_seen_cached, (int)c_dropped_cache,
            (int)c_skip_partial_cached, (int)c_skip_wide_cached);
    kprintf("[reclaim] %s: %d swap-ins (%d failed); %d kcycles scanning "
            "(%d per frame evicted)\n",
            t, (int)c_swapin, (int)c_swapin_fail, (int)(c_cycles / 1000),
            (int)(evicted ? c_cycles / evicted : 0));
    rmap_report(t);
    swap_report(t);
}
