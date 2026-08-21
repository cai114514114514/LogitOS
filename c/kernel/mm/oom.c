#include <stdint.h>
#include <stddef.h>
#include "oom.h"
#include "pmm.h"
#include "rmap.h"
#include "reclaim.h"
#include "mm.h"
#include "mmhost.h"
#include "kprintf.h"

#ifndef MM_HOSTTEST
#include "sched.h"     /* bkl_hlt_wait(): wait WITHOUT holding the big kernel lock */
#include "percpu.h"    /* this_cpu()->in_kernel: do we hold it in the first place? */
#endif

/* See oom.h for the design and the arguments. This file is the machine. */

/* --------------------------------------------------------------- policy --
 *
 * OOM_KILL_NEWEST is the NEGATIVE CONTROL (tests/unit/mm_run.sh). It replaces
 * the whole selection below with "the highest pid", which is a policy somebody
 * could genuinely propose -- the newest process is the one that just asked for
 * memory, so blame it -- and which is wrong here for a reason the control makes
 * visible: on this machine the newest process is usually the small one the user
 * just started, and the hog was started first. It must redden exactly the
 * which-process assertions and none of the survival ones. */

/* THE WAIT, in parks. Each park is bkl_hlt_wait(): drop the BKL, halt until the
 * next interrupt, re-take it. The timer runs at 100 Hz, so a park is at most
 * 10 ms and usually much less (any interrupt wakes it). 128 is therefore a
 * ceiling of about a second on a completely idle machine and far less in
 * practice -- long enough for a victim to reach its next kernel entry (a GUI
 * app syscalls every frame, a shell constantly, and a faulting program on its
 * next fault), short enough that a victim which is NOT going to reach one soon
 * does not hang the innocent process in the kernel.
 *
 * Expiry is not a failure to be hidden: it falls back to exactly the behaviour
 * that existed before this file, and is counted (oom_waits_expired) so "the
 * innocent process died anyway" is a number rather than a mystery.
 *
 * MEASURED on the machine (make test-oom-os, 2026-08-20): the one real rescue
 * in that run took TWO parks -- "[oom] pid 4 survived: 52869 frames free after
 * 2 parks" -- against this budget of 128, and expired=0. So the ceiling is two
 * orders of magnitude clear of the case it is sized for, which is the right
 * side to be wrong on: too small silently reintroduces the bug for a victim
 * that is merely slow. */
#ifndef OOM_WAIT_PARKS
#define OOM_WAIT_PARKS 128
#endif

/* A candidate must hold at least this many resident frames to be worth killing:
 * 64 frames = 256 KiB. Below it the kill cannot even satisfy the burst of
 * faults that produced the shortage, so it would lose a program and change
 * nothing. Not derived from the watermark like "enough" is -- this is a floor
 * on "is there anything here at all", and on a 64 MiB machine the watermark is
 * 512 frames, which would refuse every candidate a small machine has. */
#ifndef OOM_MIN_VICTIM_FRAMES
#define OOM_MIN_VICTIM_FRAMES 64
#endif

/* The scratch table. .bss, sized at compile time, never grown: this code runs
 * when allocation has already failed (oom.h).
 *
 * MEASURED, `nm -S build/kernel.elf`, 2026-08-20:
 *   g_task  0x800 = 2,048 B   (so sizeof(struct oom_task) is 64, not the 56 an
 *                              earlier draft of this comment asserted -- the
 *                              char name[32] pads the struct out)
 *   g_rss   0x080 =   128 B
 *   total          = 2,176 B of a 511 MiB machine.
 * The wrong figure sat here claiming to have been measured, which is worse than
 * no figure: it is the shape this tree's own notes warn about most. */
static struct oom_task g_task[OOM_MAXTASK];
static uint32_t        g_rss[OOM_MAXTASK];
static int             g_ntask;

static uint64_t c_kills, c_kills_self, c_kills_gui, c_novictim;
static uint64_t c_saved, c_wait_expired, c_kheapfail;
static uint64_t c_reaps, c_reaped_frames;
static int      g_last_pid, g_last_source = -1;
static uint64_t g_last_frames;
/* Rate limit for the pmm/kheap refusal lines. A machine deep in a shortage can
 * refuse thousands of allocations a second and a per-refusal kprintf would BE
 * the hang it is reporting. The first one is always printed, then every 256th:
 * the first is what names the moment, the rest are a rhythm. */
static uint64_t g_next_alloc_log = 1, g_next_kheap_log = 1;

uint64_t oom_kills(void)         { return c_kills; }
uint64_t oom_kills_self(void)    { return c_kills_self; }
uint64_t oom_kills_gui(void)     { return c_kills_gui; }
uint64_t oom_no_victim(void)     { return c_novictim; }
uint64_t oom_saved(void)         { return c_saved; }
uint64_t oom_waits_expired(void) { return c_wait_expired; }
uint64_t oom_alloc_fails(void)   { return pmm_alloc_failures(); }  /* the allocator's own count, not a copy */
uint64_t oom_kheap_fails(void)   { return c_kheapfail; }
uint64_t oom_reaps(void)         { return c_reaps; }
uint64_t oom_reaped_frames(void) { return c_reaped_frames; }
int      oom_last_pid(void)      { return g_last_pid; }
uint64_t oom_last_frames(void)   { return g_last_frames; }
int      oom_last_source(void)   { return g_last_source; }

static const char *src_name(int s)
{
    switch (s) {
    case OOM_SRC_FAULT: return "page fault";
    case OOM_SRC_KHEAP: return "kernel heap";
    default:            return "?";
    }
}

/* ------------------------------------------------------- resident sets --
 *
 * ONE sweep of the reverse map, attributing every mapped user frame to the
 * address space that has it, rather than one sweep per candidate. The
 * difference matters: the sweep is O(total_frames) -- 131,072 iterations on a
 * 512 MiB machine -- and this runs on the failure path of a page fault, where a
 * factor of NPROC would turn a diagnosis into a stall.
 *
 * The chain walk is rmap.h's iterator, whose contract says it is safe under the
 * big kernel lock because a user PTE is only ever installed or destroyed by
 * kernel code that also holds it. That is why gather() is only ever called with
 * the BKL held -- see the check in oom_kill(). */
#ifdef MM_HOSTTEST
static int tally_rss_host(void);           /* the fixture, at the bottom */
#endif

static void tally_rss(void)
{
    for (int i = 0; i < g_ntask; i++) g_rss[i] = 0;
#ifdef MM_HOSTTEST
    /* The fixture answers ONLY if a case declared a resident set. Falling
     * through to the real sweep otherwise is what lets one host case build two
     * genuine address spaces with vmm_map_page_in() and put the REAL reverse-map
     * walk in front of the REAL policy -- without it, everything below this line
     * would be device-only code, and the host suite would be gating a table of
     * numbers somebody typed. */
    if (tally_rss_host()) return;
#endif
    if (!rmap_ready()) return;

    uint64_t total = pmm_total_frames();
    for (uint64_t f = 1; f < total; f++) {
        if (!rmap_mapped(f * FRAME_SIZE)) continue;     /* one unlocked load; see rmap.h */
        struct rmap_iter it;
        uint64_t cr3, va;
        for (rmap_begin(&it, f * FRAME_SIZE); rmap_next(&it, &cr3, &va); ) {
            for (int i = 0; i < g_ntask; i++)
                if ((g_task[i].cr3 & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR)) {
                    g_rss[i]++;
                    break;
                }
        }
    }
}

uint64_t oom_rss_frames(uint64_t cr3)
{
    if (!rmap_ready() || !cr3) return 0;
    uint64_t n = 0, total = pmm_total_frames();
    for (uint64_t f = 1; f < total; f++) {
        if (!rmap_mapped(f * FRAME_SIZE)) continue;
        struct rmap_iter it;
        uint64_t c, va;
        for (rmap_begin(&it, f * FRAME_SIZE); rmap_next(&it, &c, &va); )
            if ((c & MM_PTE_ADDR) == (cr3 & MM_PTE_ADDR)) { n++; break; }
    }
    return n;
}

/* ---------------------------------------------------------- the choice -- */

static int under_bkl(void)
{
#ifdef MM_HOSTTEST
    return 1;                       /* the host fixture is single-threaded */
#else
    return this_cpu()->in_kernel;
#endif
}

static void park(void)
{
#ifdef MM_HOSTTEST
    /* No scheduler here, so nothing can run and claim the mark. The host test
     * gates the POLICY; the machine gates the wait (tests/boot/run-oom-test.sh). */
#else
    if (this_cpu()->in_kernel) bkl_hlt_wait();
    else __asm__ volatile ("pause");
#endif
}

/* Fill g_task from the process table, then g_rss from the reverse map. Returns
 * the number of live processes found. */
static int gather(void)
{
    g_ntask = 0;
    for (int i = 0; i < OOM_MAXTASK; i++) {
        struct oom_task t;
        if (!oom_task_at(i, &t)) continue;
        g_task[g_ntask++] = t;
    }
    tally_rss();
    return g_ntask;
}

/* RE-ENTRANCY, and it is not a theoretical worry -- it is reachable through the
 * kmalloc hook specifically.
 *
 * c/kernel/cpu/interrupts.c:156 takes the big kernel lock only when the entry is
 * NOT nested, and leaves `in_kernel` at 1 for the nested one. So an interrupt
 * arriving on a core that is already inside this function passes under_bkl()
 * quite legitimately, and kmalloc is reachable from IRQ context by design (the
 * comment at the top of kheap.c says so). Without this flag such an interrupt
 * could run oom_task_reap_dead() while the outer call is halfway through it --
 * two walks of one dying address space, and vmm_free_user() is idempotent
 * ACROSS calls, not DURING one.
 *
 * A plain int and not an atomic: every caller holds the BKL (checked directly
 * above), so the only concurrency this can see is a nested interrupt on this
 * same core, which cannot land between the read and the write of an
 * uninterruptible instruction pair the compiler emits here. Two cores cannot
 * both be here, because only one holds the lock. */
static int g_busy;

/* The body, so the guard above is one place and cannot be returned around. */
static int oom_choose(int source, int self);

int oom_kill(int source)
{
    int self = oom_task_self();

    /* THE SWEEP NEEDS THE BIG KERNEL LOCK (rmap.h's iterator contract), and
     * pmm_alloc() is reachable from the BKL-free syscall (syscall_is_bkl_free,
     * SYS_KHEAP_STRESS). Refusing here rather than walking a chain another core
     * may be editing costs one diagnostic and never costs a corrupted walk;
     * the alternative -- taking rmap_lock across the whole sweep -- would hold
     * a leaf lock for 131,072 iterations on the failure path of every
     * allocation, which is worse than not choosing. */
    if (!under_bkl()) {
        c_novictim++;
        kprintf("[oom] %s: out of memory on a lock-free path -- no victim chosen "
                "(the reverse map may not be walked here)\n", src_name(source));
        return OOM_NO_VICTIM;
    }

    if (g_busy) {
        /* Already choosing on this core, one frame down the stack. Say so and
         * decline: the shortage that interrupted us has an answer in flight. */
        c_novictim++;
        kprintf("[oom] %s: re-entered from an interrupt while already choosing "
                "-- declined\n", src_name(source));
        return OOM_NO_VICTIM;
    }
    g_busy = 1;
    int decision = oom_choose(source, self);
    g_busy = 0;
    return decision;
}

static int oom_choose(int source, int self)
{
    /* TIER 0: THE DEAD, BEFORE THE LIVING. A process that has already exited
     * holds its whole address space here until a reaper collects it, and the
     * reapers can be arbitrarily slow (oom.h, and the long comment above
     * oom_task_reap_dead() in c/kernel/exec/proc.c). Frames belonging to a
     * program that is not running are not a policy question: take them, and if
     * that answers the shortage, nobody dies.
     *
     * Measured in FRAMES rather than trusted from the return value, for the
     * reason this tree states about every counter: "how many zombie spaces did
     * I visit" is not "did memory come back", and only one of those is the
     * question. */
    uint64_t before = pmm_free_frames();
    int zn = oom_task_reap_dead();
    uint64_t got = pmm_free_frames() - before;
    if (got) {
        c_reaps++;
        c_reaped_frames += got;
        kprintf("[oom] %s: %d frames (%d KiB) recovered from %d process(es) that "
                "had already exited -- nobody killed\n",
                src_name(source), (int)got, (int)(got * 4), zn);
        return OOM_REAPED;
    }

    int n = gather();
    if (n <= 0) { c_novictim++; return OOM_NO_VICTIM; }

    /* ONE KILL AT A TIME. A mark already outstanding IS the answer to this
     * shortage; choosing a second victim would kill a second program for a
     * shortage the first kill is about to end. See oom.h. */
    for (int i = 0; i < n; i++)
        if (g_task[i].dying && !g_task[i].immune) {
            g_last_pid = g_task[i].pid;
            g_last_frames = g_rss[i];
            g_last_source = source;
            kprintf("[oom] %s: pid %d (%s) is already marked, %d frames "
                    "(%d KiB) -- waiting for it rather than killing a second\n",
                    src_name(source), g_task[i].pid, g_task[i].name,
                    (int)g_rss[i], (int)(g_rss[i] * 4));
            return (g_task[i].pid == self) ? OOM_VICTIM_SELF : OOM_VICTIM_OTHER;
        }

    int best = -1, best_headless = -1, eligible = 0;
    for (int i = 0; i < n; i++) {
        if (g_task[i].immune) continue;
        if (g_rss[i] < OOM_MIN_VICTIM_FRAMES) continue;
        eligible++;
#ifdef OOM_KILL_NEWEST
        /* NEGATIVE CONTROL: the newest process, i.e. the highest pid. */
        if (best < 0 || g_task[i].pid > g_task[best].pid) best = i;
        (void)best_headless;
#else
        if (best < 0 || g_rss[i] > g_rss[best]) best = i;
        if (!g_task[i].gui &&
            (best_headless < 0 || g_rss[i] > g_rss[best_headless])) best_headless = i;
#endif
    }

    if (best < 0) {
        c_novictim++;
        kprintf("[oom] %s: out of memory and NO eligible victim -- %d processes, "
                "none holding %d frames it could give back. The faulting process "
                "dies, as it did before this existed.\n",
                src_name(source), n, OOM_MIN_VICTIM_FRAMES);
        return OOM_NO_VICTIM;
    }

    int chosen = best;
    const char *why = "the largest process on the machine";
#ifndef OOM_KILL_NEWEST
    /* SPARE THE WINDOW IF SPARING IT STILL ENDS THE SHORTAGE. reclaim_high() is
     * this kernel's own definition of enough free memory (reclaim.h), so the
     * threshold is derived rather than invented. */
    uint64_t enough = reclaim_high();
    if (best_headless >= 0 && best_headless != best &&
        g_rss[best_headless] >= enough) {
        chosen = best_headless;
        why = "the largest process WITHOUT a window that alone frees enough";
    }
#endif

    g_last_pid    = g_task[chosen].pid;
    g_last_frames = g_rss[chosen];
    g_last_source = source;

    /* THE RECORD. A process that vanishes with no line is indistinguishable
     * from a crash, so this says who, how big, why, and what was spared. */
    kprintf("[oom] %s: out of memory -- %d frames free of %d, %d processes, "
            "%d eligible\n",
            src_name(source), (int)pmm_free_frames(), (int)pmm_total_frames(),
            n, eligible);
    kprintf("[oom] victim: pid %d \"%s\" rss=%d frames (%d KiB)%s -- %s\n",
            g_task[chosen].pid, g_task[chosen].name, (int)g_rss[chosen],
            (int)(g_rss[chosen] * 4), g_task[chosen].gui ? " [window]" : "", why);
    if (chosen != best)
        kprintf("[oom] spared: pid %d \"%s\" rss=%d frames (%d KiB) [window] -- "
                "bigger, but the victim above frees %d KiB and the watermark "
                "wants %d KiB\n",
                g_task[best].pid, g_task[best].name, (int)g_rss[best],
                (int)(g_rss[best] * 4), (int)(g_rss[chosen] * 4),
                (int)(reclaim_high() * 4));

    if (oom_task_kill(g_task[chosen].pid) != 0) {
        c_novictim++;
        kprintf("[oom] pid %d could not be marked -- it is protected or already "
                "gone; no victim\n", g_task[chosen].pid);
        return OOM_NO_VICTIM;
    }

    c_kills++;
    if (g_task[chosen].gui) c_kills_gui++;
    if (g_task[chosen].pid == self) {
        c_kills_self++;
        return OOM_VICTIM_SELF;
    }
    return OOM_VICTIM_OTHER;
}

/* ------------------------------------------------------- the fault path -- */

int oom_fault_retry(void)
{
    int d = oom_kill(OOM_SRC_FAULT);
    if (d == OOM_REAPED) {
        /* The frames are already in the allocator -- a dead process's pages
         * were taken back. Retry at once: there is nothing to wait for, and no
         * program died. */
        c_saved++;
        return 1;
    }
    if (d != OOM_VICTIM_OTHER)
        return 0;                   /* we are the victim, or there is none:
                                     * decline exactly as before */

    /* The faulting process is INNOCENT. Wait, bounded, for the victim to reach
     * its next kernel entry and give the frames back. `pmm_reserve()` and not 0
     * is the target because the reserve is not ours to spend (pmm.h) -- a
     * "frame free" that is only the swap-in escape hatch is not a frame this
     * fault may have. */
    uint64_t floor = pmm_reserve();
    for (int i = 0; i < OOM_WAIT_PARKS; i++) {
        if (pmm_free_frames() > floor) {
            c_saved++;
            kprintf("[oom] pid %d survived: %d frames free after %d parks\n",
                    oom_task_self(), (int)pmm_free_frames(), i);
            return 1;
        }
        park();
        /* AND ASK THE DEAD AGAIN, every park. This is not belt-and-braces: the
         * victim reaches its next kernel entry, runs proc_exit() and becomes a
         * ZOMBIE STILL HOLDING EVERY FRAME IT TOOK -- that is what proc_exit()
         * does on this machine -- so without this line the wait would time out
         * beside the very memory the kill just released, and the innocent
         * process would die anyway. The kill is only half of the mechanism; this
         * is the other half. */
        uint64_t b = pmm_free_frames();
        oom_task_reap_dead();
        uint64_t got = pmm_free_frames() - b;
        if (got) { c_reaps++; c_reaped_frames += got; }
    }
    c_wait_expired++;
    kprintf("[oom] pid %d waited %d parks and memory did not come back -- "
            "declining, as before\n", oom_task_self(), OOM_WAIT_PARKS);
    return 0;
}

/* ------------------------------------------------ the other two callers -- */

void oom_kheap_fail(uint64_t bytes)
{
    c_kheapfail++;
    if (c_kheapfail >= g_next_kheap_log) {
        g_next_kheap_log = c_kheapfail + 256;
        kprintf("[oom] kmalloc(%d) refused -- %d refusals so far, %d frames free\n",
                (int)bytes, (int)c_kheapfail, (int)pmm_free_frames());
    }
    /* A kernel allocation failing IS an out-of-memory, and the kill is a mark,
     * so it is safe from here (kmalloc has already released kheap_lock -- see
     * the call site). Unlike the fault path there is nothing to retry: the
     * caller has its own failure handling and this function cannot make its
     * allocation succeed. */
    oom_kill(OOM_SRC_KHEAP);
}

void oom_alloc_fail(void)
{
    /* NO COUNTER OF ITS OWN. pmm.c already maintains alloc_fails and
     * mmsys.c's [mmstat] line already prints it; a second tally kept here would
     * be a number only this file knows, which is exactly how two subsystems come
     * to disagree about the same machine. The rate limiter therefore runs off
     * the allocator's own count. */
    uint64_t seen = pmm_alloc_failures();
    /* RECORDS ONLY, and the asymmetry with oom_kheap_fail() is deliberate.
     * pmm_alloc() is the bottom of every allocation in the kernel, including
     * ones made from interrupt context and from the BKL-free syscall, and it is
     * also called speculatively by paths that handle a 0 perfectly well
     * (vmm.c's next_table, reclaim's own probes). Killing a process from here
     * would fire on allocations nobody was in trouble over, from contexts where
     * the reverse map may not be walked. The two callers that MEAN it -- the
     * user page fault and kmalloc -- ask for a victim themselves, one layer up,
     * where the failure is known to be terminal. */
    if (seen >= g_next_alloc_log) {
        g_next_alloc_log = seen + 256;
        kprintf("[oom] pmm_alloc refused (%d so far); free=%d reserve=%d\n",
                (int)seen, (int)pmm_free_frames(), (int)pmm_reserve());
    }
}

/* ------------------------------------------------------------- report --- */

void oom_stats(void)
{
    /* One machine-readable line, so a harness greps numbers rather than prose
     * -- the same argument mmsys.c's [mmstat] makes about its own wording. */
    /* TWO "saved" NUMBERS, and they are not the same claim.
     *
     *   saved       this file's: the retry was told memory is available.
     *   faultsaved  fault.c's: the retried fault then actually SUCCEEDED.
     *
     * The second is the one worth asserting on, and it is the smaller: memory
     * can appear and the fault still fail (another core took the frame in
     * between). Printing only the optimistic one would let a harness certify a
     * rescue that did not happen, which is the exact shape of mistake this
     * tree's notes keep finding. Both are here so the gap is visible. */
    kprintf("[oomstat] kills=%d self=%d gui=%d novictim=%d saved=%d "
            "expired=%d allocfail=%d kheapfail=%d reaps=%d reapedframes=%d "
            "faultretry=%d faultsaved=%d "
            "lastpid=%d lastframes=%d lastsrc=%d\n",
            (int)c_kills, (int)c_kills_self, (int)c_kills_gui, (int)c_novictim,
            (int)c_saved, (int)c_wait_expired, (int)pmm_alloc_failures(),
            (int)c_kheapfail, (int)c_reaps, (int)c_reaped_frames,
            (int)mm_oom_retries(), (int)mm_oom_saved(),
            g_last_pid, (int)g_last_frames, g_last_source);
}

void oom_report(const char *tag)
{
    kprintf("[oom] %s: %d kills (%d self, %d windowed), %d asked with no victim, "
            "%d faults saved by killing somebody else, %d waits expired, "
            "%d shortages answered by the already-dead (%d KiB)\n",
            tag ? tag : "-", (int)c_kills, (int)c_kills_self, (int)c_kills_gui,
            (int)c_novictim, (int)c_saved, (int)c_wait_expired,
            (int)c_reaps, (int)(c_reaped_frames * 4));
    if (g_last_pid)
        kprintf("[oom] %s: last victim pid %d, %d frames (%d KiB), from the %s\n",
                tag ? tag : "-", g_last_pid, (int)g_last_frames,
                (int)(g_last_frames * 4), src_name(g_last_source));
}

/* ------------------------------------------------ the host-test fixture -- */
#ifdef MM_HOSTTEST

#include "vmm.h"     /* the fixture's reap does what proc.c's does: vmm_free_user */

static struct oom_task h_task[OOM_MAXTASK];
static uint64_t        h_rss[OOM_MAXTASK];
static int             h_zombie[OOM_MAXTASK];
static int             h_n, h_self, h_rss_set, h_nreaped;
static int             h_killed[OOM_MAXTASK], h_nkilled;

void oom_test_reset(void)
{
    h_n = 0; h_self = 0; h_nkilled = 0; h_rss_set = 0; h_nreaped = 0;
    for (int i = 0; i < OOM_MAXTASK; i++) { h_rss[i] = 0; h_killed[i] = 0; h_zombie[i] = 0; }
    c_kills = c_kills_self = c_kills_gui = c_novictim = 0;
    c_saved = c_wait_expired = c_kheapfail = 0;
    c_reaps = c_reaped_frames = 0;
    g_last_pid = 0; g_last_frames = 0; g_last_source = -1;
    g_next_alloc_log = g_next_kheap_log = 1;
}

int oom_test_add(int pid, uint64_t cr3, int gui, int immune, const char *name)
{
    if (h_n >= OOM_MAXTASK) return -1;
    struct oom_task *t = &h_task[h_n];
    t->pid = pid; t->cr3 = cr3; t->gui = gui; t->immune = immune; t->dying = 0;
    int i = 0;
    for (; name && name[i] && i < (int)sizeof t->name - 1; i++) t->name[i] = name[i];
    t->name[i] = 0;
    return h_n++;
}

void oom_test_set_self(int pid) { h_self = pid; }

void oom_test_set_rss(int pid, uint64_t frames)
{
    for (int i = 0; i < h_n; i++)
        if (h_task[i].pid == pid) { h_rss[i] = frames; h_rss_set = 1; }
}

void oom_test_set_zombie(int pid, int z)
{
    for (int i = 0; i < h_n; i++) if (h_task[i].pid == pid) h_zombie[i] = z ? 1 : 0;
}

int oom_test_nkilled(void) { return h_nkilled; }
int oom_test_killed(int n) { return (n >= 0 && n < h_nkilled) ? h_killed[n] : -1; }
int oom_test_nreaped(void) { return h_nreaped; }

/* A ZOMBIE IS NOT A LIVE PROCESS, and the fixture has to say so the same way
 * the machine does (proc.c's adapter reports only PROC_RUNNING): if a dead
 * process were still a candidate, the killer would "kill" it, report a victim,
 * free nothing and let the innocent process die -- which is the bug this whole
 * file exists to remove, arrived at from the other side. */
int oom_task_at(int idx, struct oom_task *out)
{
    if (idx < 0 || idx >= h_n || h_zombie[idx]) return 0;
    *out = h_task[idx];
    return 1;
}

/* The fixture's reap is the REAL operation, not a simulation of it: the same
 * vmm_free_user() c/kernel/exec/proc.c calls, on address spaces the test built
 * with vmm_map_page_in(). So the frames the host suite counts coming back are
 * frames that really came back, through the code the machine runs. */
int oom_task_reap_dead(void)
{
    int n = 0;
    for (int i = 0; i < h_n; i++)
        if (h_zombie[i] && h_task[i].cr3) { vmm_free_user(h_task[i].cr3); n++; h_nreaped++; }
    return n;
}

int oom_task_kill(int pid)
{
    for (int i = 0; i < h_n; i++)
        if (h_task[i].pid == pid) {
            if (h_task[i].immune) return -1;
            h_task[i].dying = 1;
            if (h_nkilled < OOM_MAXTASK) h_killed[h_nkilled++] = pid;
            return 0;
        }
    return -1;
}

int oom_task_self(void) { return h_self; }

/* The fixture sets resident sets directly rather than building real page
 * tables: the policy is what this file gates, and a synthetic table lets a case
 * put a 180 MiB process SECOND so "pick the first" cannot pass by coincidence.
 * The real rmap sweep is gated on the machine (tests/boot/run-oom-test.sh),
 * where the address spaces are real. */
static int tally_rss_host(void)
{
    if (!h_rss_set) return 0;              /* nobody declared one: sweep for real */
    for (int i = 0; i < g_ntask; i++) {
        g_rss[i] = 0;
        for (int j = 0; j < h_n; j++)
            if (h_task[j].pid == g_task[i].pid) { g_rss[i] = (uint32_t)h_rss[j]; break; }
    }
    return 1;
}
#endif
