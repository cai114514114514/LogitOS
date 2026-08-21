#ifndef LOGIT_OOM_H
#define LOGIT_OOM_H

#include <stdint.h>

/* THE OUT-OF-MEMORY KILLER: choosing who dies, instead of letting the machine
 * choose for us.
 *
 * ===========================================================================
 * THE BUG THIS EXISTS TO FIX, STATED AS A SENTENCE
 *
 * c/kernel/mm/fault.c's fault_frame() asks the allocator for a frame, forces a
 * reclaim pass when that fails, and then returns 0. A 0 there means the fault
 * declines, and c/kernel/cpu/interrupts.c terminates the faulting process.
 *
 * So the process that DIED was whoever touched memory NEXT. That is not a
 * policy, it is a race: on a machine whose memory has been taken by one large
 * program, the thing most likely to fault next is the shell, the window
 * manager's client, or the small program the user just started -- anything but
 * the program that took the memory. The kernel had containment (one process
 * dies, the machine survives) and no selection.
 *
 * This file is the selection. Nothing else about the failure path changes: the
 * victim still dies through the ordinary teardown, on its own stack, and the
 * kernel still survives.
 *
 * ===========================================================================
 * IT RUNS WHEN ALLOCATION HAS ALREADY FAILED, SO IT MAY NOT ALLOCATE
 *
 * The same rule reclaim.h states for the write-out path, and for the same
 * reason: a killer that kmallocs is a killer that fails exactly when it is
 * needed. Everything this file touches is static storage sized at compile time:
 *
 *   the per-task scratch table   OOM_MAXTASK entries of `struct oom_task`,
 *                                one .bss array (see oom.c for the byte count
 *                                measured with nm);
 *   the resident-set tally       one uint32_t per table entry, in the same
 *                                array -- NOT a map keyed by cr3, which would
 *                                need to grow;
 *   the page walk               reclaim's reverse map, already built at
 *                                pmm_init time and never grown.
 *
 * There is no kmalloc, no pmm_alloc and no stack recursion anywhere below.
 * It does not draw on pmm_alloc_reserve() either: the reserve is 32 frames kept
 * for the swap-IN fault (pmm.h), and spending it here would take the escape
 * hatch away from the one path that has no alternative. The killer needs no
 * frames at all -- it reads a table and sets a flag.
 *
 * ===========================================================================
 * CHOOSING THE VICTIM
 *
 * THE METRIC IS RESIDENT SET, and the reverse map gives it honestly. Not
 * "reserved bytes" (vma_reserved_bytes) -- an mmap of 4 GiB that was never
 * touched costs nothing and freeing it frees nothing, so a reservation-based
 * killer picks a process whose death does not help. Not the heap's own idea of
 * how much it has, which no process outside the kernel keeps. rmap.h already
 * maintains, for every physical frame, the list of user PTEs pointing at it; so
 * ONE sweep of the reverse map attributes every resident user frame on the
 * machine to the address space that has it. That is the number that says how
 * much memory comes back if this process dies.
 *
 * A frame mapped by two address spaces (fork, or a shared segment) counts once
 * for EACH -- which is what "resident set" means everywhere else, and is the
 * conservative direction here: it over-states what a kill recovers rather than
 * under-stating it, and the over-statement is exactly the sharing that makes
 * the recovery smaller. Said out loud because it is the one place this number
 * is not the number of frames that will actually come back.
 *
 * THE OBVIOUS POLICY IS "KILL THE BIGGEST", AND ON THIS MACHINE THAT IS WRONG
 * IN ONE SPECIFIC WAY. The desktop peaks at 229 MiB of 511, and the largest
 * single consumer is normally the browser or an inference run -- i.e. the thing
 * the user is looking at. A killer that always takes the maximum takes the
 * user's window every time a background program exhausts memory.
 *
 * But the correction has a hard limit, and it is worth stating before the rule:
 * THE KILL HAS TO END THE SHORTAGE. Killing something that frees too little is
 * the classic OOM-killer failure -- the machine is out of memory again a
 * millisecond later, having lost a program for nothing, and now does it again.
 * So "spare the window" may never be bought at the price of "and OOM again
 * immediately".
 *
 * The rule that satisfies both:
 *
 *     Prefer the largest HEADLESS process, if it alone frees enough.
 *     Otherwise take the largest eligible process, window or not.
 *
 * "Enough" is not a number invented here: it is reclaim_high(), the watermark
 * reclaim itself stops at, which is already this kernel's own written-down
 * definition of "enough free memory to run" (6% of RAM; see reclaim_init).
 * Deriving it means the two mechanisms cannot drift apart, and means this file
 * contains no magic constant that a bigger machine would invalidate.
 *
 * Consequences worth knowing, because they are deliberate:
 *   - a 200 MiB browser against a 60 MiB build: the browser is taken. Correct,
 *     because killing the build would not end the shortage and the next fault
 *     would kill the browser anyway, having also lost the build.
 *   - a 200 MiB inference run against a 120 MiB browser: the inference run is
 *     taken, which is both the largest AND headless.
 *   - a 180 MiB headless hog against a 12 MiB browser: the hog is taken even
 *     though "biggest" and "headless" happen to agree; the gate distinguishes
 *     the two by putting the hog SECOND in the table, so a policy that merely
 *     picks the first or the newest cannot pass by coincidence.
 *
 * ===========================================================================
 * WHO IS NEVER A VICTIM
 *
 * Structurally, and using the process table's OWN rule rather than a second
 * copy of it (c/kernel/exec/proc.c, above proc_kill()):
 *
 *   the console shell   NO PARENT AND NO WINDOW. wm_run() proc_spawns /bin/sh
 *                       on the serial console and is the only caller in the
 *                       tree that creates a proc with ppid == 0 and gui == NULL.
 *                       It is NOT pid 1 -- the desktop opens Finder first -- so
 *                       a hard-coded number would protect the wrong process,
 *                       which is a bug proc.c already found and fixed once.
 *   the window manager  is not a proc at all. It is a kernel thread, so it is
 *                       not in the table and cannot be named. Nothing here has
 *                       to remember to exclude it.
 *   a zombie            has already exited; its frames are the reaper's, and
 *                       "killing" it frees nothing.
 *   an already-marked   see the next section.
 *   process
 *
 * ===========================================================================
 * KILLING IS A MARK, NOT A TEARDOWN -- AND THAT IS WHY IT IS SAFE HERE
 *
 * proc.c's kill sets a flag and lets the victim run proc_exit() on itself at
 * its next kernel entry. That is what makes an OOM killer callable from the
 * page-fault handler and from inside kmalloc: this file never touches another
 * thread's address space, stack or file table. It takes one spinlock, sets one
 * flag, and returns.
 *
 * The cost is that the memory does not come back at the instant of the
 * decision, which shapes the fault path's use of it:
 *
 *   THE VICTIM IS THE FAULTING PROCESS   decline the fault, as before. The
 *                                        process dies immediately, on its own
 *                                        stack, through the path that already
 *                                        existed. This is the common case: the
 *                                        program taking the memory is the
 *                                        program faulting for more.
 *   THE VICTIM IS SOMEBODY ELSE          the faulting process is INNOCENT and
 *                                        killing it is the bug being fixed. So
 *                                        the fault waits, bounded, for the mark
 *                                        to be claimed, and retries once. If
 *                                        the wait expires it declines exactly
 *                                        as before -- strictly no worse than
 *                                        the old behaviour, plus one correct
 *                                        kill.
 *
 * BEFORE ANY OF THAT: THE DEAD ARE ASKED FIRST.
 *
 * A process that has already exited keeps its whole address space on this
 * machine until a reaper collects it, and the two reapers are a parent that may
 * never call waitpid() and a window-manager loop that only takes ORPHANS
 * (proc_exit(), c/kernel/exec/proc.c). So the state "160 MiB held by a program
 * that is not running" is reachable and durable, and it is reachable most
 * easily by the killer's own previous victim.
 *
 * Killing a live program to recover memory that belongs to a dead one is the
 * worst move available, so oom_kill() strips the zombies first
 * (oom_task_reap_dead()) and, if that produced frames, chooses nobody and
 * returns OOM_REAPED. The counters keep the two apart: `reaps` and
 * `reaped_frames` against `kills`.
 *
 * ONE KILL AT A TIME. If a mark is already outstanding, no second victim is
 * chosen: the shortage already has an answer in flight, and choosing again
 * would kill a second program for a shortage the first kill is about to end.
 * (Real OOM killers get this wrong and it is called an OOM storm.) The
 * outstanding victim is reported and waited for instead.
 *
 * ===========================================================================
 * WHAT IT DID ON THE MACHINE, 2026-08-20 (make test-oom-os)
 *
 * 320 MiB, no swap device, a 200 MiB hog resident and a 96 MiB innocent that
 * waited for the shortage and then asked for memory:
 *
 *   [oom] pmm_alloc refused (1 so far); free=128 reserve=128
 *   [oom] page fault: out of memory -- 128 frames free of 81885, 4 processes,
 *         3 eligible
 *   [oom] victim: pid 6 "as" rss=51453 frames (205812 KiB) -- the largest
 *         process on the machine
 *   [oom] pid 4 survived: 52869 frames free after 2 parks
 *   OOMSMALL-OK 24576 pages written and read back
 *
 * Read the third and fourth lines together, because they are the entire point:
 * the process that DIED held 205 MiB, and the process that lived is the one
 * that faulted. Before this file the second of those was the one that died.
 * `[mm] audit: 0 inconsistencies` afterwards, and the counters were
 * kills=1 self=0 gui=0 novictim=0 saved=1 expired=0 reaps=1 reapedframes=52741
 * faultretry=1 faultsaved=1.
 *
 * `reapedframes=52741` is the zombie tier doing the work described above: the
 * hog was a background job of the serial console's shell, which never reaps,
 * so those 206 MiB came back because the fault path took them, not because
 * anybody collected the corpse. Two parks -- about 20 ms.
 *
 * ===========================================================================
 * A PROCESS THAT VANISHES WITH NO RECORD IS INDISTINGUISHABLE FROM A CRASH
 *
 * So every decision prints, on the serial console, WHO, HOW BIG, and WHY --
 * including the runner-up that was spared and the reason it was spared. And
 * every decision is counted, readable from ring 3 without a debugger through
 * SYS_MEMINFO's diagnostic door (c/kernel/mm/mmsys.c, MMCTL_OOM = 5) and on the
 * existing one-line [mmstat] record, so a harness asserts on numbers rather
 * than on the absence of a string.
 */

/* Must be >= NPROC (c/kernel/exec/proc.h). The adapter in proc.c has the
 * _Static_assert; it is stated as a number here so this header does not have to
 * include an exec header into c/kernel/mm. */
#define OOM_MAXTASK 32

/* Who ran out. Recorded per kill so "the browser died" can be told from "a
 * driver could not get a DMA ring". */
#define OOM_SRC_FAULT 0     /* a user page fault could not get a frame */
#define OOM_SRC_KHEAP 1     /* kmalloc could not grow the kernel heap */
/* There is deliberately no OOM_SRC_PMM. A draft had one, for oom_alloc_fail(),
 * and nothing ever set it: that function records and does not choose a victim
 * (see the argument at its definition), so a "source" for it would have been a
 * constant with no producer -- the shape this tree treats as a defect rather
 * than as harmless spare capacity. The frame allocator's refusals are counted
 * by pmm.c, which already had a counter for them. */

/* What oom_kill() decided. */
#define OOM_NO_VICTIM    0  /* nothing eligible: the caller's old behaviour stands */
#define OOM_VICTIM_SELF  1  /* the calling process was chosen -- it should decline
                             * and die, which is the pre-existing path */
#define OOM_VICTIM_OTHER 2  /* somebody else is marked; frames are coming */
#define OOM_REAPED       3  /* NOBODY was killed: memory came back from processes
                             * that had already exited. See the zombie tier in
                             * oom.c -- the caller retries and no program dies. */

/* Choose and mark a victim. Never allocates, never blocks, never tears anything
 * down. Returns one of OOM_*; the victim's pid is oom_last_pid(). */
int oom_kill(int source);

/* THE FAULT PATH'S WHOLE USE OF THIS FILE.
 *
 * Call when a user page fault could not obtain a frame after reclaim. Returns 1
 * if the caller should RETRY the fault (a victim other than the caller was
 * marked and memory has appeared), 0 if it should decline as it always has.
 *
 * May block -- it parks with bkl_hlt_wait(), which drops the big kernel lock,
 * exactly as the swap path already does from this same fault context. That is
 * why the retry re-walks the page tables from the top rather than reusing a
 * PTE pointer taken before the wait. */
int oom_fault_retry(void);

/* kmalloc's failure path: it grew the heap, the PMM refused, and kmalloc is
 * about to return NULL. Called with NO heap lock held. */
void oom_kheap_fail(uint64_t bytes);

/* pmm_alloc() returned 0. RECORDS ONLY -- see oom.c for why this one does not
 * kill. Called with pmm_lock released. */
void oom_alloc_fail(void);

/* Resident user frames attributed to one address space, from the reverse map.
 * O(total_frames + rmap nodes). Exposed for the report and for the tests. */
uint64_t oom_rss_frames(uint64_t cr3);

/* --- counters. A killer nobody has watched run is not a killer. --------- */
uint64_t oom_kills(void);          /* victims marked */
uint64_t oom_kills_self(void);     /* ...of which the caller's own process */
uint64_t oom_kills_gui(void);      /* ...of which owned a window */
uint64_t oom_no_victim(void);      /* asked, and nothing was eligible */
uint64_t oom_saved(void);          /* faults that survived because somebody ELSE
                                    * was killed -- the number this whole file
                                    * exists to make nonzero */
uint64_t oom_waits_expired(void);  /* ...and the ones where the wait ran out */
uint64_t oom_alloc_fails(void);    /* pmm_alloc() refusals seen */
uint64_t oom_kheap_fails(void);    /* kmalloc refusals seen */
uint64_t oom_reaps(void);          /* shortages answered by a dead process's
                                    * memory instead of by killing a live one */
uint64_t oom_reaped_frames(void);  /* ...and the frames that came back that way */
int      oom_last_pid(void);
uint64_t oom_last_frames(void);
int      oom_last_source(void);

void oom_report(const char *tag);      /* prose, on the serial console */
void oom_stats(void);                  /* the one-line [oomstat] record */

/* --- the process-table seam --------------------------------------------
 * c/kernel/mm must not include c/kernel/exec/proc.h (mm is underneath exec:
 * the fault path is reached from the scheduler, and pulling the process table's
 * header down here would make every mm host test link the world). So the table
 * is read through these three, implemented by c/kernel/exec/proc.c on the
 * machine and by the host test's own fixture under -DMM_HOSTTEST. */
struct oom_task {
    int      pid;
    uint64_t cr3;
    int      gui;      /* owns a window */
    int      immune;   /* proc.c's own protected-process rule */
    int      dying;    /* a kill is already marked against it */
    char     name[32];
};

/* Slot `idx` of the process table, 0 <= idx < OOM_MAXTASK. Returns 1 and fills
 * `out` for a live process, 0 for an empty or zombie slot. */
int  oom_task_at(int idx, struct oom_task *out);
/* Mark `pid` for death. Returns 0 on success. */
int  oom_task_kill(int pid);
/* The pid of the process running on this core, or 0 for a kernel thread. */
int  oom_task_self(void);
/* Release the user pages of every process that has already exited but whose
 * address space no reaper has collected yet. Returns the number of zombie
 * spaces visited; the FRAMES are measured by the caller. See the long comment
 * above the implementation in c/kernel/exec/proc.c -- on this machine a
 * background job killed from the serial console is a zombie holding every frame
 * it took, for an unbounded time, which would make "kill the biggest process"
 * free nothing at all. */
int  oom_task_reap_dead(void);

#ifdef MM_HOSTTEST
/* The fixture the host test drives the policy with. Never compiled into the
 * kernel. */
void oom_test_reset(void);
int  oom_test_add(int pid, uint64_t cr3, int gui, int immune, const char *name);
void oom_test_set_self(int pid);
/* Declare a process's resident set instead of building page tables for it.
 * If NOTHING declares one, the sweep runs for real over the reverse map -- see
 * tally_rss() -- so one host case can put two real address spaces in front of
 * the real policy, and the rest can state a 180 MiB process in one line. */
void oom_test_set_rss(int pid, uint64_t frames);
int  oom_test_nkilled(void);
int  oom_test_killed(int n);
void oom_test_set_zombie(int pid, int z);  /* the reap tier's fixture */
int  oom_test_nreaped(void);
#endif

#endif /* LOGIT_OOM_H */
