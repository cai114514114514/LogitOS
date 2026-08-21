/* Host test for the out-of-memory killer (c/kernel/mm/oom.c, -DMM_HOSTTEST).
 *
 * WHAT IS ACTUALLY BEING GATED, because "an OOM killer works" is not a claim
 * anything can check. Three separate things, and they fail differently:
 *
 *   1. THE MEASUREMENT.  Resident set, read out of the real reverse map, over
 *      real address spaces built with vmm_map_page_in(). If this number is
 *      wrong every policy decision below is decided by a coin.
 *   2. THE CHOICE.       Given resident sets, which process. This is the whole
 *      design (see oom.h) and it is where the negative control aims.
 *   3. THE REFUSALS.     init is never chosen; a second victim is never chosen
 *      while a mark is outstanding; nothing is chosen when nothing is worth
 *      choosing. These are survival properties -- they must hold under EVERY
 *      policy, including a wrong one, which is why the control must not redden
 *      them.
 *
 * THE CASES ARE BUILT SO THAT COINCIDENCE CANNOT PASS THEM. The hog is never
 * the first entry in the table and never the last, and never the highest pid
 * unless the case is specifically about that -- so "pick the first", "pick the
 * last" and "pick the newest" each fail somewhere. The negative control
 * (-DOOM_KILL_NEWEST) is one of those three, wired to a -D so it can be watched
 * failing rather than argued about.
 *
 * Run: sh tests/unit/oom_run.sh */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "mm.h"
#include "rmap.h"
#include "reclaim.h"
#include "oom.h"
#include "mmhost.h"
#include "kprintf.h"

#define VA0 (MM_USER_BASE + 0x10000)

static void phase(const char *s) { printf("-- %s\n", s); }

/* ------------------------------------------------------------------------
 * 1. THE MEASUREMENT: does the resident set come out of the reverse map right?
 *
 * No fixture here -- real address spaces, real mappings, and oom_rss_frames()
 * walking the real rmap chains. A killer that mis-measures picks at random
 * however good its policy is.
 */
static void t_rss_is_real(void)
{
    phase("resident set, from the real reverse map over real address spaces");

    uint64_t big = vmm_new_space();
    uint64_t small = vmm_new_space();
    mm_ok(big && small, "two address spaces");

    mm_eqi((int)oom_rss_frames(big), 0, "a space with no mappings has no resident set");

    for (int i = 0; i < 40; i++) {
        uint64_t f = pmm_alloc();
        mm_ok(f != 0, "frame %d for the big space", i);
        vmm_map_page_in(big, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }
    for (int i = 0; i < 7; i++) {
        uint64_t f = pmm_alloc();
        vmm_map_page_in(small, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }

    mm_eqi((int)oom_rss_frames(big), 40, "40 mapped pages read back as 40 frames");
    mm_eqi((int)oom_rss_frames(small), 7, "and the other space is counted separately");
    mm_eqi(rmap_audit(), 0, "the reverse map is consistent while we read it");

    /* A SHARED frame counts for BOTH, which is what "resident set" means
     * everywhere else and is the conservative direction here (oom.h). Asserted
     * rather than assumed, because the alternative -- counting it once -- would
     * make two forked processes each look half their real size. */
    uint64_t shared = pmm_alloc();
    vmm_map_page_in(big,   VA0 + 0x100000, shared, VMM_USER);
    vmm_map_page_in(small, VA0 + 0x100000, shared, VMM_USER);
    mm_eqi((int)oom_rss_frames(big), 41, "a shared frame counts for the first space");
    mm_eqi((int)oom_rss_frames(small), 8, "and for the second one too");

    vmm_free_space(big);
    vmm_free_space(small);
    mm_eqi(rmap_audit(), 0, "audit still clean after both spaces go");
}

/* The same question one level up: the SWEEP that attributes every frame on the
 * machine in one pass, driven through the policy rather than through
 * oom_rss_frames(). tally_rss() is static, so it is reached the way the kernel
 * reaches it -- by asking for a victim and seeing who was chosen. */
static void t_sweep_feeds_the_choice(void)
{
    phase("the one-pass sweep feeds the policy (no declared numbers anywhere)");

    uint64_t a = vmm_new_space(), b = vmm_new_space(), c = vmm_new_space();
    for (int i = 0; i < 70; i++) {   /* pid 11: middling */
        uint64_t f = pmm_alloc();
        vmm_map_page_in(a, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }
    for (int i = 0; i < 300; i++) {  /* pid 12: the hog, and NOT the newest */
        uint64_t f = pmm_alloc();
        vmm_map_page_in(b, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }
    for (int i = 0; i < 65; i++) {   /* pid 13: newest, small */
        uint64_t f = pmm_alloc();
        vmm_map_page_in(c, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }

    oom_test_reset();
    oom_test_add(10, 0,  0, 1, "sh");        /* init: no space, immune */
    oom_test_add(11, a,  0, 0, "middling");
    oom_test_add(12, b,  0, 0, "hog");
    oom_test_add(13, c,  0, 0, "newest");
    oom_test_set_self(13);
    /* No oom_test_set_rss() ANYWHERE in this case -- that is the point. The
     * numbers come from the reverse map or this case proves nothing. */

    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d, OOM_VICTIM_OTHER, "a victim other than the caller was chosen");
    mm_eqi(oom_last_pid(), 12, "the sweep found the hog (pid 12), measured not declared");
    mm_eqi((int)oom_last_frames(), 300, "and reported its real resident set");
    mm_eqi(oom_test_nkilled(), 1, "exactly one process was marked");

    vmm_free_space(a); vmm_free_space(b); vmm_free_space(c);
}

/* ------------------------------------------------------------------------
 * 2. THE CHOICE. From here on the resident sets are DECLARED, so a case can
 * state "a 180 MiB process" in one line instead of mapping 46,080 pages. The
 * sweep that produces them for real is gated above and on the machine.
 */

/* Table shape used by most cases below. Note the ORDER: the biggest is never
 * first and never last. */
static void table_std(void)
{
    oom_test_reset();
    oom_test_add(1, 0x100000, 0, 1, "sh");        /* init -- immune */
    oom_test_add(2, 0x200000, 1, 0, "finder");    /* a window */
    oom_test_add(3, 0x300000, 0, 0, "build");     /* headless */
    oom_test_add(4, 0x400000, 1, 0, "browser");   /* a window */
    oom_test_add(5, 0x500000, 0, 0, "tiny");
}

static void t_largest_headless_wins(void)
{
    phase("the largest HEADLESS process, when it alone ends the shortage");

    table_std();
    oom_test_set_rss(1, 900);
    oom_test_set_rss(2, 1200);
    oom_test_set_rss(3, 20000);      /* the hog: headless, biggest, and third */
    oom_test_set_rss(4, 30000);      /* bigger still -- but it is the window */
    oom_test_set_rss(5, 100);
    oom_test_set_self(5);

    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d, OOM_VICTIM_OTHER, "somebody other than the caller");
    mm_eqi(oom_last_pid(), 3, "the headless hog is taken, not the bigger browser");
    mm_eqi((int)oom_kills_gui(), 0, "no window was closed");
}

static void t_window_taken_when_sparing_it_would_not_help(void)
{
    phase("...but the window IS taken when sparing it would not end the shortage");

    /* The headless candidate is real but small: killing it frees less than the
     * watermark wants, so the machine would be out of memory again immediately,
     * having lost a program for nothing. oom.h calls this the hard limit on the
     * whole "spare the window" rule, and this is the case that holds it. */
    table_std();
    oom_test_set_rss(1, 900);
    oom_test_set_rss(2, 1200);
    oom_test_set_rss(3, 200);        /* headless, but nowhere near enough */
    oom_test_set_rss(4, 40000);      /* the window, and the only real answer */
    oom_test_set_rss(5, 100);
    oom_test_set_self(5);

    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d, OOM_VICTIM_OTHER, "a victim was chosen");
    mm_eqi(oom_last_pid(), 4, "the window is taken, because nothing else is enough");
    mm_eqi((int)oom_kills_gui(), 1, "and it is recorded as a windowed kill");
}

static void t_self_is_chosen_when_self_is_the_hog(void)
{
    phase("the common case: the program taking the memory is the one faulting");

    table_std();
    oom_test_set_rss(1, 900);
    oom_test_set_rss(2, 1200);
    oom_test_set_rss(3, 50000);
    oom_test_set_rss(4, 1000);
    oom_test_set_rss(5, 100);
    oom_test_set_self(3);            /* the hog itself faults */

    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d, OOM_VICTIM_SELF, "the caller is told it is the victim");
    mm_eqi(oom_last_pid(), 3, "and it is the hog");
    mm_eqi((int)oom_kills_self(), 1, "counted as a self-kill");
    /* This matters because of what the fault path does with it: decline, and
     * die on its own stack through the path that already existed. No wait, no
     * retry, nothing new. */
}

/* ------------------------------------------------------------------------
 * 3. THE REFUSALS -- survival properties. EVERY assertion in this section must
 * survive the negative control too.
 */
static void t_never_init(void)
{
    phase("init is never the victim, however big it gets");

    oom_test_reset();
    oom_test_add(1, 0x100000, 0, 1, "sh");      /* no parent, no window */
    oom_test_add(2, 0x200000, 0, 0, "small");
    oom_test_set_rss(1, 100000);                /* by far the biggest AND lowest pid */
    oom_test_set_rss(2, 5000);
    oom_test_set_self(2);

    int d = oom_kill(OOM_SRC_FAULT);
    mm_ok(oom_last_pid() != 1, "the console shell was not chosen");
    mm_eqi(oom_last_pid(), 2, "the only eligible process was");
    mm_eqi(d, OOM_VICTIM_SELF, "which happens to be the caller");
    for (int i = 0; i < oom_test_nkilled(); i++)
        mm_ok(oom_test_killed(i) != 1, "init was never even marked");
}

static void t_no_victim_when_nobody_holds_anything(void)
{
    phase("nothing eligible: the caller's old behaviour stands");

    oom_test_reset();
    oom_test_add(1, 0x100000, 0, 1, "sh");
    oom_test_add(2, 0x200000, 0, 0, "tiny");
    oom_test_set_rss(1, 90000);
    oom_test_set_rss(2, 8);                     /* below OOM_MIN_VICTIM_FRAMES */
    oom_test_set_self(2);

    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d, OOM_NO_VICTIM, "no victim");
    mm_eqi(oom_test_nkilled(), 0, "and nothing was marked");
    mm_eqi((int)oom_no_victim(), 1, "counted, so it is not silent");
    /* A killer that takes a 32 KiB process to satisfy a 200 MiB shortage has
     * lost a program and changed nothing. */
}

static void t_one_kill_at_a_time(void)
{
    phase("a mark already outstanding IS the answer -- no second victim");

    table_std();
    oom_test_set_rss(1, 900);
    oom_test_set_rss(2, 1200);
    oom_test_set_rss(3, 20000);
    oom_test_set_rss(4, 30000);
    oom_test_set_rss(5, 100);
    oom_test_set_self(5);

    oom_kill(OOM_SRC_FAULT);
    int first = oom_last_pid();
    mm_eqi(oom_test_nkilled(), 1, "one kill for the first shortage");

    /* The shortage has not gone away yet -- the victim has not reached its next
     * kernel entry. A killer that chooses again here kills a second program for
     * a shortage the first kill is about to end; real ones get this wrong and it
     * is called an OOM storm. */
    int d = oom_kill(OOM_SRC_FAULT);
    mm_eqi(oom_test_nkilled(), 1, "the second shortage marked NOBODY new");
    mm_eqi(oom_last_pid(), first, "it reports the outstanding victim instead");
    mm_eqi(d, OOM_VICTIM_OTHER, "and tells the caller to wait for it");
}

static void t_kheap_source_is_recorded(void)
{
    phase("who ran out is recorded: the kernel heap is not a page fault");

    table_std();
    oom_test_set_rss(1, 900);
    oom_test_set_rss(2, 1200);
    oom_test_set_rss(3, 20000);
    oom_test_set_rss(4, 1000);
    oom_test_set_rss(5, 100);
    oom_test_set_self(5);

    oom_kheap_fail(4096);
    mm_eqi(oom_last_source(), OOM_SRC_KHEAP, "recorded as a kernel-heap shortage");
    mm_eqi((int)oom_kheap_fails(), 1, "and the refusal itself is counted");
    /* "the browser died" and "a driver could not get a DMA ring" are different
     * findings, and a single kill counter cannot tell them apart. */
}

/* ------------------------------------------------------------------------
 * 4. THE DEAD ARE ASKED FIRST. Real frames, really given back, through the same
 * vmm_free_user() the machine calls -- see oom_task_reap_dead() in
 * c/kernel/exec/proc.c for why a zombie holding 160 MiB is reachable here.
 */
static void t_zombie_memory_before_anyone_dies(void)
{
    phase("a dead process's frames are taken back before a live one is killed");

    uint64_t dead = vmm_new_space(), live = vmm_new_space();
    for (int i = 0; i < 120; i++) {
        uint64_t f = pmm_alloc();
        vmm_map_page_in(dead, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }
    for (int i = 0; i < 500; i++) {
        uint64_t f = pmm_alloc();
        vmm_map_page_in(live, VA0 + (uint64_t)i * 4096, f, VMM_USER | VMM_WRITABLE);
    }

    oom_test_reset();
    oom_test_add(1, 0x100000, 0, 1, "sh");
    oom_test_add(7, dead, 0, 0, "exited");
    oom_test_add(8, live, 0, 0, "hog");
    oom_test_set_self(8);
    oom_test_set_zombie(7, 1);          /* it has exited; nobody has reaped it */

    uint64_t free_before = pmm_free_frames();
    int d = oom_kill(OOM_SRC_FAULT);

    mm_eqi(d, OOM_REAPED, "the shortage was answered by the already-dead");
    mm_eqi(oom_test_nkilled(), 0, "NOBODY was killed");
    /* 122, NOT 120, and the two extra are the finding rather than a rounding
     * error: vmm_free_user() gives back the dead process's PAGE TABLES as well
     * as its pages -- 120 mapped pages sit under one PT (512 pages each) hanging
     * off one PD, so 120 + 1 + 1. Written as 120 first, measured as 122, and
     * corrected here rather than loosened to >=, because the exact number is
     * what would catch a leak of exactly one table frame per reap -- which is
     * the mistake this operation is most likely to make. */
    mm_eqi((int)(pmm_free_frames() - free_before), 122,
           "all 120 of the dead process's pages came back, plus its PT and PD");
    mm_eqi((int)oom_reaps(), 1, "counted as a reap, not as a kill");
    mm_eqi((int)oom_reaped_frames(), 122, "with the frames recorded");
    mm_eqi((int)oom_kills(), 0, "the kill counter is untouched");

    /* And with the dead already stripped, the NEXT shortage does kill -- so the
     * reap tier is a first answer, not a way of never answering. */
    int d2 = oom_kill(OOM_SRC_FAULT);
    mm_eqi(d2, OOM_VICTIM_SELF, "the second shortage falls through to a real victim");
    mm_eqi(oom_last_pid(), 8, "which is the hog");

    vmm_free_space(dead);
    vmm_free_space(live);
    mm_eqi(rmap_audit(), 0, "the reverse map survived the early free");
}

/* ------------------------------------------------------------------------ */
int main(void)
{
    mm_sim_init(64);                    /* MiB */
    mm_sim_kernel_space();
    mm_ok(rmap_ready(), "the reverse map came up -- every number below needs it");
    printf("=== oom killer: %d frames of simulated memory ===\n",
           (int)pmm_total_frames());
    /* The watermark the "does it free enough" test is derived from. Set
     * explicitly rather than inherited, so the cases below state a number that
     * is checkable against reclaim_high() instead of one that moves when
     * reclaim's init changes. */
    reclaim_set_watermarks(512, 1024);

    t_rss_is_real();
    t_sweep_feeds_the_choice();
    t_largest_headless_wins();
    t_window_taken_when_sparing_it_would_not_help();
    t_self_is_chosen_when_self_is_the_hog();
    t_never_init();
    t_no_victim_when_nobody_holds_anything();
    t_one_kill_at_a_time();
    t_kheap_source_is_recorded();
    t_zombie_memory_before_anyone_dies();

    oom_report("end of test");
    mm_sim_done();
    return mm_summary("mm_oom_test");
}
