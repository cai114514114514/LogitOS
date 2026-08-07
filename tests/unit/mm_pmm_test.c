/* Host test for the physical frame allocator's REFERENCE COUNTS
 * (c/kernel/mm/pmm.c compiled with -DMM_HOSTTEST over a simulated physical
 * memory -- the real algorithm, not a model of it).
 *
 * What is being defended here:
 *   - a frame freed while another address space still maps it is silent
 *     corruption that surfaces much later somewhere unrelated,
 *   - a frame never freed is a leak that only shows under load,
 *   - and neither is visible in the commit that caused it.
 * So every refcount transition is asserted explicitly, the counters are
 * re-derived from the table (pmm_audit) after every phase, and the torture
 * loop at the end runs long enough that a one-frame-per-iteration error is
 * unmissable rather than plausible noise.
 *
 * Run: make test-mm  (or sh tests/unit/mm_run.sh) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "kprintf.h"

#define SIM_MIB 64

static uint64_t base_free;

static void phase(const char *name) { printf("-- %s\n", name); }

/* Every phase ends here: the counters the kernel keeps incrementally must
 * equal what a full walk of the refcount table says they are. */
static void audit_clean(const char *where)
{
    mm_log_quiet(1);
    int errs = pmm_audit();
    mm_log_quiet(0);
    mm_ok(errs == 0, "%s: pmm_audit reported %d inconsistencies", where, errs);
}

int main(void)
{
    mm_sim_init(SIM_MIB);
    printf("simulated RAM: %d MiB, %d frames, %d free after boot reservations\n",
           SIM_MIB, (int)pmm_total_frames(), (int)pmm_free_frames());

    /* ---------------------------------------------------------------- */
    phase("boot state");
    mm_eqi(pmm_total_frames(), SIM_MIB * 1024 / 4, "total frames");
    mm_eqi(pmm_used_frames() + pmm_free_frames(), pmm_total_frames(),
           "used + free == total");
    mm_eqi(pmm_refs_total(), pmm_used_frames(), "boot: one reference per used frame");
    mm_eqi(pmm_shared_frames(), 0, "boot: nothing shared");
    mm_eqi(pmm_pinned_frames(), 0, "boot: nothing pinned");
    mm_eqi(pmm_bugs(), 0, "boot: no invariant violations");
    mm_ok(pmm_free_frames() > 0, "boot: some memory is free");
    audit_clean("boot");
    base_free = pmm_free_frames();

    /* ---------------------------------------------------------------- */
    phase("alloc / free is refcount 1 -> 0");
    uint64_t a = pmm_alloc();
    mm_ok(a != 0, "pmm_alloc returned a frame");
    mm_eqi(pmm_refcount(a), 1, "fresh frame has one reference");
    mm_eqi(pmm_free_frames(), base_free - 1, "one frame left the free pool");
    mm_eqi(pmm_shared_frames(), 0, "a private frame is not shared");
    pmm_free(a);
    mm_eqi(pmm_refcount(a), 0, "freed frame has no references");
    mm_eqi(pmm_free_frames(), base_free, "the frame came back");
    audit_clean("alloc/free");

    /* ---------------------------------------------------------------- */
    phase("sharing: the transition table");
    a = pmm_alloc();
    mm_eqi(pmm_ref(a), 0, "pmm_ref succeeds on an allocated frame");
    mm_eqi(pmm_refcount(a), 2, "1 -> 2");
    mm_eqi(pmm_shared_frames(), 1, "1 -> 2 makes the frame shared");
    mm_eqi(pmm_refs_total() - pmm_used_frames(), 1, "one mapping saved");
    mm_eqi(pmm_free_frames(), base_free - 1, "sharing costs no extra frame");

    mm_eqi(pmm_ref(a), 0, "a third reference");
    mm_eqi(pmm_refcount(a), 3, "2 -> 3");
    mm_eqi(pmm_shared_frames(), 1, "3 references is still one shared frame");

    pmm_free(a);
    mm_eqi(pmm_refcount(a), 2, "3 -> 2");
    mm_eqi(pmm_shared_frames(), 1, "still shared at 2");
    mm_eqi(pmm_free_frames(), base_free - 1, "not freed while others hold it");

    pmm_free(a);
    mm_eqi(pmm_refcount(a), 1, "2 -> 1");
    mm_eqi(pmm_shared_frames(), 0, "2 -> 1 stops being shared");
    mm_eqi(pmm_free_frames(), base_free - 1, "still allocated at 1");

    pmm_free(a);
    mm_eqi(pmm_refcount(a), 0, "1 -> 0");
    mm_eqi(pmm_free_frames(), base_free, "the last reference frees it");
    mm_eqi(pmm_bugs(), 0, "no bugs reported for a legal sequence");
    audit_clean("sharing");

    /* ---------------------------------------------------------------- */
    phase("illegal operations are refused and reported, not obeyed");
    mm_log_quiet(1);
    uint64_t bugs0 = pmm_bugs();
    uint64_t used0 = pmm_used_frames();

    a = pmm_alloc();
    pmm_free(a);
    pmm_free(a);                                   /* double free */
    mm_eqi(pmm_bugs(), bugs0 + 1, "double free is reported");
    mm_eqi(pmm_refcount(a), 0, "double free did not push the count negative");
    mm_eqi(pmm_used_frames(), used0, "double free did not change the used count");

    uint64_t b = pmm_alloc();
    pmm_free(b);
    mm_eqi(pmm_ref(b), -1, "pmm_ref on a free frame fails");
    mm_eqi(pmm_bugs(), bugs0 + 2, "reference on a free frame is reported");
    mm_eqi(pmm_refcount(b), 0, "the failed ref did not resurrect the frame");

    /* Unaligned and out-of-range inputs. A free of an address in the middle of
     * a frame must free THAT frame (the callers hold PTE-masked addresses, but
     * an unmasked one must not corrupt a neighbour); an address past the end of
     * RAM must be ignored, not indexed. */
    uint64_t c = pmm_alloc();
    pmm_free(c + 0xFFF);
    mm_eqi(pmm_refcount(c), 0, "free of frame_base+0xFFF frees that frame");
    pmm_free((uint64_t)SIM_MIB * 1024 * 1024 + 0x1000);   /* past the end */
    pmm_free(~(uint64_t)0 & ~(uint64_t)0xFFF);            /* would overflow a naive index */
    mm_eqi(pmm_bugs(), bugs0 + 2, "out-of-range frees are ignored silently");
    mm_log_quiet(0);
    audit_clean("illegal ops");

    /* ---------------------------------------------------------------- */
    phase("refcount overflow saturates (defined), never wraps");
    mm_log_quiet(1);
    a = pmm_alloc();
    int sat_at = 0;
    for (int i = 0; i < (int)PMM_REF_MAX + 16; i++) {
        if (pmm_ref(a) < 0) { sat_at = i; break; }
    }
    mm_log_quiet(0);
    mm_eqi(pmm_refcount(a), PMM_REF_MAX, "count stops at PMM_REF_MAX");
    mm_eqi(sat_at, PMM_REF_MAX - 1, "pmm_ref starts refusing exactly at the ceiling");
    mm_eqi(pmm_pinned_frames(), 1, "the saturated frame is counted as pinned once");
    mm_log_quiet(1);
    for (int i = 0; i < 100; i++) mm_ok(pmm_ref(a) < 0, "further refs keep failing");
    mm_log_quiet(0);
    mm_eqi(pmm_pinned_frames(), 1, "pinned is counted once, not once per attempt");

    uint64_t used_before = pmm_used_frames();
    for (int i = 0; i < 1000; i++) pmm_free(a);
    mm_eqi(pmm_refcount(a), PMM_REF_MAX, "a pinned frame's count never drops");
    mm_eqi(pmm_used_frames(), used_before, "a pinned frame is never returned to the pool");
    mm_ok(pmm_bugs() == bugs0 + 2, "saturation is not reported as a bug (it is defined)");
    audit_clean("saturation");
    /* From here on that one frame is permanently pinned; the free-count
     * baseline moves by exactly 1, and it counts as "shared" forever (its
     * count is PMM_REF_MAX >= 2), so the shared baseline moves too. */
    base_free = pmm_free_frames();
    uint64_t base_shared = pmm_shared_frames();
    mm_eqi(base_shared, 1, "the pinned frame is the only shared frame");

    /* ---------------------------------------------------------------- */
    phase("poison catches use-after-free at the next allocation");
    pmm_set_poison(2);                             /* whole-frame, for the test */
    mm_log_reset();
    uint64_t bugs1 = pmm_bugs();
    uint64_t victim = pmm_alloc();
    memset(mm_sim_ptr(victim), 0x11, 4096);
    pmm_free(victim);
    /* The freed frame must no longer contain the caller's data. */
    mm_ok(((uint8_t *)mm_sim_ptr(victim))[0] != 0x11,
          "a freed frame no longer carries the previous owner's bytes");
    /* Now write through the stale pointer, the way a use-after-free does. */
    ((uint8_t *)mm_sim_ptr(victim))[1234] = 0x42;
    mm_log_quiet(1);
    uint64_t again = pmm_alloc();
    mm_log_quiet(0);
    mm_eqi(again, victim, "the same frame comes back (so the poison is checked)");
    mm_eqi(pmm_bugs(), bugs1 + 1, "the use-after-free is reported at the allocation");
    pmm_free(again);

    /* Level 1 (the always-on default) covers the first cache line. */
    pmm_set_poison(1);
    bugs1 = pmm_bugs();
    victim = pmm_alloc();
    pmm_free(victim);
    ((uint8_t *)mm_sim_ptr(victim))[8] = 0x99;     /* inside the head window */
    mm_log_quiet(1);
    again = pmm_alloc();
    mm_log_quiet(0);
    mm_eqi(pmm_bugs(), bugs1 + 1, "level 1 catches a write to the head of the frame");
    pmm_free(again);

    /* A clean free/alloc cycle must NOT report anything -- no false positives,
     * which matters more than the detection rate for an always-on check. */
    bugs1 = pmm_bugs();
    for (int i = 0; i < 200; i++) { uint64_t f = pmm_alloc(); pmm_free(f); }
    mm_eqi(pmm_bugs(), bugs1, "200 clean alloc/free cycles report nothing");
    audit_clean("poison");
    mm_eqi(pmm_free_frames(), base_free, "poison phase leaked nothing");

    /* ---------------------------------------------------------------- */
    phase("contiguous allocation refcounts every frame it takes");
    uint64_t n = 17;
    uint64_t blk = pmm_alloc_contig((size_t)n);
    mm_ok(blk != 0, "pmm_alloc_contig succeeded");
    for (uint64_t i = 0; i < n; i++)
        mm_eqi(pmm_refcount(blk + i * 4096), 1, "each contig frame has one reference");
    mm_eqi(pmm_free_frames(), base_free - n, "contig took exactly n frames");
    for (uint64_t i = 0; i < n; i++) pmm_free(blk + i * 4096);
    mm_eqi(pmm_free_frames(), base_free, "contig gave back exactly n frames");
    audit_clean("contig");

    /* ---------------------------------------------------------------- */
    /* The leak/double-free discipline check. Randomised but deterministic:
     * allocate, share a varying number of extra references, drop them all in a
     * shuffled order, thousands of times. If ANY path leaks or over-frees a
     * single frame, the free count will not come back to the baseline. */
    phase("torture: 20000 rounds of alloc / share / free in mixed order");
    unsigned rng = 12345;
#define NEXT() (rng = rng * 1103515245u + 12345u, (rng >> 16) & 0x7FFF)
    uint64_t held[64];
    int nheld = 0;
    uint64_t bugs2 = pmm_bugs();
    for (int round = 0; round < 20000; round++) {
        int op = NEXT() % 3;
        if (op == 0 && nheld < 64) {
            uint64_t f = pmm_alloc();
            mm_ok(f != 0, "torture: allocation succeeded");
            held[nheld++] = f;
            int extra = NEXT() % 4;
            for (int k = 0; k < extra; k++) {
                if (pmm_ref(f) == 0) pmm_free(f);   /* take and immediately drop */
            }
        } else if (op == 1 && nheld > 0) {
            int i = NEXT() % nheld;
            /* Share it into "another address space", then release both. */
            if (pmm_ref(held[i]) == 0) {
                mm_ok(pmm_refcount(held[i]) == 2, "torture: shared frame at 2");
                pmm_free(held[i]);
            }
        } else if (nheld > 0) {
            int i = NEXT() % nheld;
            pmm_free(held[i]);
            held[i] = held[--nheld];
        }
    }
    while (nheld > 0) pmm_free(held[--nheld]);
    mm_eqi(pmm_bugs(), bugs2, "torture reported no invariant violations");
    mm_eqi(pmm_shared_frames(), base_shared, "torture left nothing extra shared");
    mm_eqi(pmm_free_frames(), base_free,
           "torture returned EXACTLY to the free-frame baseline");
    audit_clean("torture");

    pmm_report("end of test");
    mm_sim_done();
    return mm_summary("mm_pmm_test");
}
