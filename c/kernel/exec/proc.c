#include <stdint.h>
#include <stddef.h>
#include "proc.h"
#include "file.h"
#include "sched.h"
#include "vmm.h"
#include "mm.h"
#include "pmm.h"
#include "pit.h"
#include "kprintf.h"
#include "spinlock.h"
#include "usercopy.h"    /* SYS_PROCS copies the table out to ring 3 */
#include "logit_abi.h"   /* struct logit_procinfo, LOGIT_KILL_* */
/* Path-qualified: see the note in syscall.c -- mini-libc's sys/wait.h sorts
 * first in INCDIRS and silently wins the bare form. */
#include "kernel/core/wait.h"   /* M27: a parent waits for a child, it does not poll */
#include "uthread.h"            /* M30: a process is a set of threads */
#include "ksignal.h"            /* M31: signals -- lifecycle, SIGCHLD, EINTR, kill */
#include "ptrace.h"             /* a dying process is either end of a trace link */

void wm_app_exit(void);   /* wm.c: mark the current proc's window dead */
/* net/core/sock.c: release the non-blocking sockets this process owns. Weak so
 * the process model does not hard-depend on the network stack being linked. */
void sock_close_owner(int pid) __attribute__((weak));

/* M25 P2: the process table is peeled out from under the BKL so SYS_FORK can run
 * BKL-free (concurrent worker spawn). g_proc_lock guards the procs[] table + the
 * next_pid counter -- the slot scan/claim/free and the state transitions. Held for
 * tiny critical sections only: vmm_free_space, file_close and schedule() are kept
 * OUTSIDE it (lock order BKL -> g_proc_lock -> g_file_lock -> g_sched_lock ->
 * g_kheap_lock -> g_pmm_lock; nothing under g_proc_lock calls vmm/kheap, so the
 * order never reverses). irqsave: reachable from fault-context proc_exit and held
 * with the timer live on a BKL-free path. */
static spinlock_t g_proc_lock = SPINLOCK_INIT;

static struct proc procs[NPROC];
static int next_pid = 1;

/* "This process has been killed and does not know it yet", one byte per PCB
 * slot, parallel to procs[] and guarded by the same lock.
 *
 * A field in struct proc would read better, and it is not one because proc.h is
 * not this change's to edit. A parallel array indexed identically is exactly as
 * correct -- it is cleared in alloc_proc(), the single place a slot is claimed,
 * so a recycled pid can never inherit the previous tenant's death sentence.
 *
 * g_kill_pending is the gate: the number of marks outstanding. It exists so the
 * check on the syscall path costs one load of a global and one never-taken
 * branch on a machine where nobody is being killed -- the same discipline
 * kprof's disabled spans use. It is not a lock and does not need to be: a stale
 * read costs one syscall of latency before the victim notices, never a missed
 * kill, because proc_kill_check() re-reads the mark under g_proc_lock. */
static unsigned char g_killmark[NPROC];
static volatile unsigned long g_kill_pending;

/* Where a parent waits for a child to die.
 *
 * One queue for every waiter, not one per process: the predicate ("a child of
 * MINE is a zombie") is per-waiter and has to be re-tested on every wake
 * regardless, so a shared queue costs a spurious scan of a 32-slot table and
 * saves a struct per PCB plus the question of which queue an exiting orphan
 * should signal. NPROC is 32 and waiters are single digits.
 *
 * Lock order: g_child_wq.lock -> g_proc_lock, because the predicate is
 * evaluated inside wait_event() with the queue lock held. proc_exit() therefore
 * RELEASES g_proc_lock before it wakes -- the waker never holds both, so the
 * order can never reverse. */
static struct waitq g_child_wq = WAITQ_INIT;

/* Does `ppid` have a reapable child matching `want` (-1 = any)? The predicate,
 * split out so wait_event() can re-evaluate it and proc_waitpid's claim loop
 * stays the single place that mutates anything. */
static int have_zombie(int ppid, int want)
{
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    int yes = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state != PROC_ZOMBIE || c->ppid != ppid) continue;
        if (want != -1 && c->pid != want) continue;
        yes = 1;
        break;
    }
    spin_unlock_irqrestore(&g_proc_lock, fl);
    return yes;
}

/* --- fork accounting ------------------------------------------------------
 * "fork is faster now" is not a claim anybody can check; "fork of the shell
 * copied 0 of its 331 pages, in 1.1 Mcycles instead of 47" is. These are the
 * numbers the copy-on-write work is judged on, so the kernel keeps them itself
 * rather than leaving them to be inferred from a stopwatch.
 *
 * TSC rather than timer_ticks(): the PIT runs at 100 Hz, so a whole fork
 * rounds to 0 or 1 ticks and the difference being measured is invisible. The
 * TSC has no calibrated frequency here, so cycles are reported as cycles --
 * a ratio between two builds on the same host is exactly what is wanted. */
static uint64_t g_forks, g_fork_cycles, g_fork_shared, g_fork_copied;
static uint64_t g_rep_ms, g_rep_forks;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Prototype here rather than in proc.h: this line owns proc.c but not proc.h.
 * It belongs in the header the day a caller outside this file needs it (a
 * meminfo syscall would); until then a local prototype keeps the change inside
 * the file this line is allowed to change. */
void proc_fork_stats(uint64_t *forks, uint64_t *cycles, uint64_t *shared, uint64_t *copied);

void proc_fork_stats(uint64_t *forks, uint64_t *cycles, uint64_t *shared, uint64_t *copied)
{
    if (forks)  *forks  = g_forks;
    if (cycles) *cycles = g_fork_cycles;
    if (shared) *shared = g_fork_shared;
    if (copied) *copied = g_fork_copied;
}

/* An idle desktop stays silent; a shell running commands leaves a continuous
 * trace of the free-frame count, which is the only way a one-frame-per-fork
 * leak is ever noticed -- it is invisible in any single test and fatal in an
 * hour.
 *
 * Sampled from two places, both deliberate:
 *   - proc_waitpid(), immediately AFTER the child's address space is freed.
 *     That is the only moment at which the free-frame count is comparable
 *     between samples: taken with a child alive it is short by that child's
 *     private pages (~256 for the shell), which is 6x the size of the leak
 *     being looked for and would bury it.
 *   - the WM loop, on a 5 s timer, so a slow trickle from GUI apps (which are
 *     reaped by proc_reap, not waited for) still leaves a trail.
 * Rate-limited either way: every 8 forks, or every 5 seconds. */
#define FORK_REPORT_EVERY 8

static void fork_report_tick(void)
{
    uint64_t now = timer_ms();
    if (g_forks == g_rep_forks) return;
    if (g_forks - g_rep_forks < FORK_REPORT_EVERY && g_rep_ms && now - g_rep_ms < 5000) return;
    g_rep_ms = now ? now : 1;
    g_rep_forks = g_forks;
    /* `live` is what makes two samples comparable. The free-frame count on its
     * own is not: taken while one more process exists it is short by that
     * process's pages (~256 for a shell), which is far bigger than the leak
     * being watched for and would bury it. Reading it lets a reader -- or a
     * test -- compare only samples taken with the same set of processes
     * alive. */
    int live = 0;
    for (int i = 0; i < NPROC; i++) if (procs[i].state != PROC_FREE) live++;

    kprintf("[mm] fork: cow=%s, %d forks, %d pages shared, %d copied, %d kcycles/fork; "
            "%d frames free, %d shared, %d bugs, %d live\n",
            mm_cow_enabled() ? "on" : "off",
            (int)g_forks, (int)g_fork_shared, (int)g_fork_copied,
            (int)(g_forks ? g_fork_cycles / g_forks / 1000 : 0),
            (int)pmm_free_frames(), (int)pmm_shared_frames(), (int)pmm_bugs(), live);
}

void proc_init(void)
{
    for (int i = 0; i < NPROC; i++) { procs[i].state = PROC_FREE; procs[i].pid = 0; }
}

struct proc *proc_current(void) { return (struct proc *)sched_current_data(); }

struct proc *proc_by_pid(int pid)
{
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    struct proc *ret = NULL;
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != PROC_FREE && procs[i].pid == pid) { ret = &procs[i]; break; }
    spin_unlock_irqrestore(&g_proc_lock, fl);
    return ret;
}

static struct proc *alloc_proc(void)
{
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    struct proc *ret = NULL;
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state == PROC_FREE) {
            struct proc *p = &procs[i];
            for (int f = 0; f < NFD; f++) p->fd[f] = NULL;
            g_killmark[i] = 0;                 /* a recycled slot never inherits a kill mark */
            p->state = PROC_RUNNING;           /* claim the slot atomically under the lock */
            p->pid = next_pid++;
            p->ppid = 0; p->exit_code = 0; p->tid = -1; p->cr3 = 0; p->gui = NULL;
            p->cwd[0] = '/'; p->cwd[1] = 0; p->name[0] = 0;
            /* M28: a recycled slot must not inherit the previous tenant's
             * grant any more than it inherits its kill mark or its cwd --
             * this is the one place a slot is claimed, so it is the one place
             * that can make that promise. DENY, not CAP_ALL: proc_create()'s
             * caller decides what a new process is trusted with, explicitly,
             * every time (proc_spawn() sets CAP_ALL for the console shell;
             * SYS_CAP_SPAWN sets the ceiling-checked request; wm_launch, a
             * file this line does not own, currently sets nothing at all --
             * see `not_done`). A default of CAP_ALL here would make that
             * silent and would make EVERY untouched creation site "the" root
             * of trust instead of the one that is supposed to be. */
            p->caps = 0; p->fs_prefix[0] = 0;
            ret = p;
            break;
        }
    spin_unlock_irqrestore(&g_proc_lock, fl);
    /* M31: give the new pid a clean signal state -- every disposition SIG_DFL,
     * nothing pending, nothing blocked. Here rather than inside the critical
     * section because it takes g_sig_lock, and this is the one place a slot is
     * claimed, so a recycled pid can no more inherit the previous tenant's
     * handlers than it can inherit its kill mark. */
    if (ret) ksig_proc_init(ret->pid);
    return ret;
}

static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

struct proc *proc_create(uint64_t cr3, void *gui, const char *name, int ppid)
{
    struct proc *p = alloc_proc();
    if (!p) return NULL;
    p->cr3 = cr3; p->gui = gui; p->ppid = ppid;
    scopy(p->name, name, sizeof p->name);
    return p;
}

int proc_fd_alloc(struct proc *p, struct file *f)
{
    if (!p || !f) return -1;
    for (int i = 0; i < NFD; i++)
        if (!p->fd[i]) { p->fd[i] = f; return i; }
    return -1;
}

struct file *proc_fd_get(struct proc *p, int fd)
{
    if (!p || fd < 0 || fd >= NFD) return NULL;
    return p->fd[fd];
}

/* Resolve `in` to an absolute canonical path against p->cwd, collapsing "."/"..".
 * Output is "/a/b/c" (root is "/"). Bounded by `max`. */
void proc_resolve(struct proc *p, const char *in, char *out, int max)
{
    char src[256]; int n = 0;
    if (in[0] != '/') {
        for (const char *d = p->cwd; *d && n < 255; d++) src[n++] = *d;
        if (n == 0 || src[n - 1] != '/') { if (n < 255) src[n++] = '/'; }
    }
    for (const char *s = in; *s && n < 255; s++) src[n++] = *s;
    src[n] = 0;

    /* 128 covers the most components a 255-char path can hold ("/x" each = >=2
     * chars), so a valid path is never silently truncated to a wrong shorter one. */
    const char *comp[128]; int clen[128], top = 0, i = 0;
    while (src[i]) {
        while (src[i] == '/') i++;
        if (!src[i]) break;
        const char *start = &src[i]; int len = 0;
        while (src[i] && src[i] != '/') { i++; len++; }
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { if (top > 0) top--; continue; }
        if (top < 128) { comp[top] = start; clen[top] = len; top++; }
    }
    int oi = 0;
    if (top == 0) { if (max > 1) { out[0] = '/'; out[1] = 0; } else if (max > 0) out[0] = 0; return; }
    for (int t = 0; t < top; t++) {
        if (oi < max - 1) out[oi++] = '/';
        for (int k = 0; k < clen[t] && oi < max - 1; k++) out[oi++] = comp[t][k];
    }
    out[oi] = 0;
}

/* ===========================================================================
 * M28: is (req_caps, req_prefix) an ALLOWED narrowing of (cur_caps,
 * cur_prefix)? This is D1's ceiling test
 * (docs/superpowers/specs/2026-08-14-m28-capabilities.md): "a child's set
 * must be a subset of the caller's. Never a superset. The kernel checks the
 * inclusion and nothing else." Two independent conditions, BOTH required:
 *
 *   BITS.   req_caps must be a bitwise subset of cur_caps -- (req_caps &
 *           ~cur_caps) == 0. A caller without CAP_NET can never hand a child
 *           CAP_NET, no matter what it asks for.
 *
 *   PREFIX. req_prefix must be at least as specific as cur_prefix: either
 *           cur_prefix is "" (the caller is itself unscoped, so any request
 *           narrows it, including ""), or req_prefix begins with cur_prefix
 *           ON A PATH-COMPONENT BOUNDARY. That last clause is not a nicety --
 *           a caller scoped to "/usr" must be able to request "/usr/bin" but
 *           MUST NOT be able to request "/usrland": a shared BYTE prefix is
 *           not a shared DIRECTORY unless the match ends the component
 *           (exactly at cur_prefix's end, or the next byte is '/'). A plain
 *           strncmp-style test here would have silently accepted the sibling
 *           path as a "narrowing" -- the entire ceiling would have been a
 *           string trick away from being no ceiling at all.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO (spec D6). This is a comparison between
 * two prefixes the CALLER supplies (one of which it already holds) -- it says
 * nothing about whether a REAL path, once resolved, can escape either one
 * through a symlink. proc_resolve() (above) is lexical only and symlink-blind,
 * and SYS_SYMLINK stores its target completely unresolved by design, so a
 * scoped process can plant its own escape hatch. That question can only be
 * answered where a path is actually resolved to bytes on disk, which is
 * c/fs/vfs.c's resolve() -- a file this line does not own. What THIS function
 * guarantees is narrower but still real and necessary: struct proc's OWN
 * fs_prefix bookkeeping can never record a WIDER scope than a process already
 * has, at the one moment (SYS_CAP_SPAWN) it can change at all. See `not_done`
 * for the exact c/fs change the full guarantee still needs. */
int proc_cap_subset(unsigned long req_caps, const char *req_prefix,
                     unsigned long cur_caps, const char *cur_prefix)
{
    if (req_caps & ~cur_caps) return 0;
    if (!cur_prefix || !cur_prefix[0]) return 1;   /* caller unscoped: any request narrows it */
    if (!req_prefix) return 0;                     /* no string can "extend" a NULL one */
    int i = 0;
    for (; cur_prefix[i]; i++)
        if (req_prefix[i] != cur_prefix[i]) return 0;   /* diverges before cur_prefix ends */
    return req_prefix[i] == 0 || req_prefix[i] == '/';  /* component boundary, not a byte match */
}

long proc_fork(struct registers *r)
{
    struct proc *parent = proc_current();
    if (!parent) return -1;

    uint64_t t0 = rdtsc();
    uint64_t space = vmm_new_space();
    if (!space) { kprintf("[fork] vmm_new_space failed\n"); return -1; }
    if (vmm_clone_user(space, parent->cr3) < 0) {   /* OOM mid-clone: don't run a partial child */
        kprintf("[fork] clone_user failed\n");
        vmm_free_space(space);
        return -1;
    }
    {   /* Charge the address-space clone only: the fd table and the child
         * thread cost the same before and after, and mixing them in would
         * hide the thing being measured. */
        uint64_t shared = 0, copied = 0;
        vmm_clone_stats(&shared, &copied);
        g_forks++;
        g_fork_cycles += rdtsc() - t0;
        g_fork_shared += shared;
        g_fork_copied += copied;
    }

    struct proc *child = alloc_proc();
    if (!child) { kprintf("[fork] proc table full\n"); vmm_free_space(space); return -1; }
    child->cr3  = space;
    child->ppid = parent->pid;
    child->gui  = NULL;                      /* a forked child has no window */
    scopy(child->name, parent->name, sizeof child->name);
    scopy(child->cwd, parent->cwd, sizeof child->cwd);
    /* M28 D1 item 2: fork inherits the parent's capability set UNCHANGED --
     * not attenuated, not widened. Plain SYS_FORK is not a narrowing event,
     * the same way plain SYS_EXECVE is not (see proc_execve() in exec.c): a
     * process that wants to hand a CHILD less than it holds itself uses
     * SYS_CAP_SPAWN instead of SYS_FORK, which does the fork-and-narrow
     * together and refuses outright rather than create a child and hope
     * something narrows it before it becomes reachable. */
    child->caps = parent->caps;
    scopy(child->fs_prefix, parent->fs_prefix, sizeof child->fs_prefix);
    /* POSIX: the child inherits the dispositions and the blocked mask, and
     * inherits NOTHING pending -- a signal raised at the parent was raised at
     * the parent, and duplicating it would mean a Ctrl+C during a fork killing
     * two processes. */
    ksig_proc_fork(child->pid, parent->pid);
    for (int i = 0; i < NFD; i++) {
        child->fd[i] = parent->fd[i];
        if (child->fd[i]) file_dup(child->fd[i]);
    }

    /* M30, and this is POSIX's rule rather than a shortcut: ONLY THE CALLING
     * THREAD EXISTS IN THE CHILD. thread_fork() builds exactly one thread, and
     * the child's descriptor table starts empty -- uthread_self() adopts that
     * thread the first time the child asks a thread question, so a child that
     * never threads costs nothing.
     *
     * THE HAZARD, said out loud because it is real and it is not fixable here:
     * a lock held by a thread that did NOT survive the fork is locked forever
     * in the child, with no owner to release it. That includes mini-libc's
     * malloc lock (c/apps/libc/src/malloc.c). It is why POSIX limits a forked
     * child of a threaded parent to async-signal-safe calls until it execs, and
     * that limit applies here unchanged. The one thing this kernel does do
     * about it is refuse execve from a multi-threaded process outright (see the
     * SYS_EXECVE case in syscall.c), so the surviving path is fork-then-exec
     * from a single-threaded process, which is what /bin/sh does. */
    child->tid = thread_fork(child->name, r, child, space);
    if (child->tid < 0) {                    /* OOM building the child kstack/thread */
        kprintf("[fork] thread_fork failed\n");
        /* Undo the fork instead of leaking the PCB slot + dup'd fds + address space
         * (and falsely returning a pid for a child that will never run). */
        for (int i = 0; i < NFD; i++)
            if (child->fd[i]) { file_close(child->fd[i]); child->fd[i] = NULL; }
        vmm_free_space(space);
        child->state = PROC_FREE; child->pid = 0; child->cr3 = 0;
        return -1;
    }
    return child->pid;                       /* parent sees the child's pid */
}

void proc_exit(int code)
{
    struct proc *p = proc_current();
    if (p) {
        /* ===================================================================
         * M30: a process is a SET of threads now, so "exit" has two halves.
         *
         * The old body below is the second half -- close the fds, release the
         * sockets, become a zombie, wake the parent -- and every line of it is
         * wrong to run while a sibling thread is still executing in this
         * address space: it would close fds out from under it and hand its page
         * tables to proc_waitpid() to free.
         *
         * So the rule is THE LAST THREAD OUT DOES THE TEARDOWN, and it is
         * derived rather than assumed -- not "the main thread", because POSIX
         * says a process ends when its last thread does, whichever that is.
         *
         *   1. uthread_proc_kill() marks the process and wakes its parked
         *      threads. A sibling dies at its next kernel entry, through
         *      uthread_exit_check() at the syscall gate. That is the SAME
         *      mark-and-check shape proc_kill() uses, for the same reason it
         *      gives: a thread may be anywhere, and tearing it down from
         *      another thread's context pulls the stack out from under it.
         *   2. This thread releases its own descriptor (waking any joiner).
         *   3. If a sibling is still live, this thread simply LEAVES. The
         *      process stays alive, holding its address space, until the last
         *      one arrives here.
         *
         * WHAT THIS COSTS, stated rather than hidden: a sibling in a pure
         * compute loop that never enters the kernel keeps the process alive
         * until it does. Closing that needs the same check on the timer
         * interrupt's return-to-ring-3 path, in c/kernel/cpu/interrupts.c,
         * which is another line's file -- exactly the gap proc_kill() already
         * documents for a killed process, now reachable one more way.
         * =================================================================== */
        code = uthread_proc_kill(p->pid, code);
        uthread_release_self(0);
        if (uthread_proc_live(p->pid) > 0)
            thread_exit();               /* a sibling lives on; never returns */
        uthread_proc_reap(p->pid);       /* free this process's descriptors + its mark */

        /* Close fds BEFORE marking zombie (file_close takes g_file_lock/kheap, which
         * must not nest under g_proc_lock). While still RUNNING with no fds, a waiter
         * sees RUNNING and keeps waiting -- no premature reap. */
        for (int i = 0; i < NFD; i++)
            if (p->fd[i]) { file_close(p->fd[i]); p->fd[i] = NULL; }
        /* Sockets are a separate table from the fds, so they need their own
         * sweep. Without it a tab that dies mid-load (window closed, page
         * faulted) strands its connections until something else needs the slot
         * -- the same class of leak the g_net_busy watchdog exists to catch on
         * the blocking path, except here it can simply be closed properly. */
        if (sock_close_owner) sock_close_owner(p->pid);
        /* Stored RAW -- the plain code passed to proc_exit(), not the POSIX
         * (code<<8)|signo wait-status encoding c/apps/libc/include/sys/wait.h's
         * WIFEXITED/WEXITSTATUS macros expect. Verified deliberate, not fixed
         * here, because the raw convention is the one actually load-bearing in
         * this tree today: c/apps/logit.h's sys_waitpid() -- used directly by
         * c/apps/coreutils/sh.c for `$?`, plus entropy.c/smptest.c/studio.c --
         * hands this value straight through with no decode, and sh.c's own
         * comment (job_running(), sh.c) already argues from "there is no
         * WNOHANG" to a workaround built on that raw value. SYS_WAITPID has one
         * wire format for every caller (the kernel cannot tell a mini-libc
         * program from a logit.h one at the syscall gate), so shifting it here
         * would fix mini-libc's WIFEXITED/WEXITSTATUS -- today unused by
         * anything in the tree, per grep -- by silently breaking sh.c's `$?`
         * for every real command a user types (exit 1 would read back 256).
         * The encode belongs where the two conventions can be kept apart: mini-
         * libc's own waitpid()/wait() (c/apps/libc/src/io.c), translating the
         * kernel's raw code into the POSIX status word for ITS callers only.
         * That file is outside this change's ownership; this comment is the
         * record for whoever picks it up. */
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        p->exit_code = code;
        p->state = PROC_ZOMBIE;
        int ppid = p->ppid;
        spin_unlock_irqrestore(&g_proc_lock, fl);

        /* M31: SIGCHLD. AFTER the zombie is visible, so a parent whose handler
         * calls waitpid() finds the child already reapable rather than racing
         * the state change it was told about.
         *
         * Its default action is IGNORE, which is why this can be unconditional:
         * a parent that has not asked for SIGCHLD sees no change at all, and
         * the existing waitpid() path is untouched. A parent that HAS asked can
         * now reap asynchronously instead of blocking or polling, which is the
         * thing a shell could not do before.
         *
         * ppid 0 (a GUI app, whose "parent" is the window manager, which is a
         * kernel thread and not a process) posts to nobody; ksig_post answers
         * SIG_E_SRCH and nothing happens. */
        if (ppid > 0) ksig_post(ppid, LOGIT_SIGCHLD);

        /* This process will never run again, so its signal state is dead with
         * it -- released here rather than at reap, so that a pid recycled
         * before the zombie is collected cannot find the old handlers. */
        ksig_proc_free(p->pid);
        /* And any ptrace link at EITHER end, for the same reason and released
         * at the same point: a recycled pid must not inherit the right to read
         * somebody's memory, and a tracee whose tracer has just died must not
         * be left parked in the stop loop with nobody to continue it.
         * c/kernel/exec/ptrace.c handles both cases. */
        ptrace_proc_free(p->pid);
        /* AFTER the unlock, and before thread_exit() (which never returns).
         * Outside the lock because the waiter holds g_child_wq.lock while it
         * takes g_proc_lock, so a waker holding g_proc_lock here would close an
         * AB-BA cycle. The waiter cannot miss this: it is enqueued under
         * g_child_wq.lock and stays enqueued until it is parked, so a wake that
         * arrives in that window blocks on the queue lock rather than being
         * lost. */
        waitq_wake_all(&g_child_wq);
    }
    wm_app_exit();        /* if this proc owns a window, mark it dead (no-op otherwise) */
    thread_exit();        /* leaves the ring; never returns. Address space freed by
                           * proc_waitpid (parent) or proc_reap (orphan/GUI). */
}

/* WNOHANG, matching c/apps/libc/include/sys/wait.h's #define WNOHANG 1 (this
 * file cannot include that header -- see the comment on proc_waitpid() in
 * proc.h). The one caller found by grepping the tree for it,
 * c/apps/as/as_port.c's as_proc_drop(), is explicit about why: it is a GC
 * finalizer, and "block until this child exits" would freeze the interpreter
 * on a live child's own timeline, which its own comment calls "a worse
 * failure than a zombie". Before this, SYS_WAITPID never read its options
 * argument at all (syscall.c), so that call blocked anyway -- silently, the
 * exact failure the finalizer was written to avoid. */
#define PROC_WNOHANG 1

/* Reap one zombie child of the current process. With options==0, blocks
 * (cooperatively) until a matching child is a zombie -- pid == -1 waits for
 * any child. With WNOHANG set, never blocks: returns 0 if a matching child
 * exists but none has exited yet (POSIX), same as it always has otherwise.
 * Any other bit in `options` is refused with SIG_E_NOSYS rather than
 * silently ignored -- a caller that asks for an option this kernel does not
 * implement (WUNTRACED; job control does not exist here) gets a loud error,
 * not a call that quietly behaves as if it had not asked. */
long proc_waitpid(int pid, int *status, int options)
{
    struct proc *self = proc_current();
    if (!self) return -1;
    if (options & ~PROC_WNOHANG) return SIG_E_NOSYS;

    for (;;) {
        int have_child = 0, rpid = -1, code = 0; uint64_t freed_cr3 = 0;
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        for (int i = 0; i < NPROC; i++) {
            struct proc *c = &procs[i];
            if (c->state == PROC_FREE || c->ppid != self->pid) continue;
            if (pid != -1 && c->pid != pid) continue;
            have_child = 1;
            if (c->state == PROC_ZOMBIE) {       /* claim it under the lock */
                rpid = c->pid; code = c->exit_code; freed_cr3 = c->cr3;
                c->state = PROC_FREE; c->pid = 0; c->cr3 = 0;
                break;
            }
        }
        spin_unlock_irqrestore(&g_proc_lock, fl);
        if (rpid != -1) {                         /* freed the address space OUTSIDE the lock */
            if (freed_cr3) vmm_free_space(freed_cr3);
            fork_report_tick();                   /* sample with the child gone -- see above */
            if (status) *status = code;
            return rpid;
        }
        if (!have_child) return -1;
        if (options & PROC_WNOHANG) {   /* asked not to block: say so, don't park */
            if (status) *status = 0;
            return 0;
        }
        /* M31: EINTR, and this is the first place in the kernel that has one.
         *
         * A blocking wait cut short by a signal must be distinguishable from
         * one that failed, or a shell cannot tell "the user pressed Ctrl+C"
         * from "there is no such child" -- and -1 already means the latter
         * here. So it returns SIG_E_INTR, which mini-libc turns into errno
         * EINTR, and a handler with SA_RESTART gets the call re-issued instead
         * (the restart is done to the SAVED context; see ksigframe.c).
         *
         * Tested BEFORE the park, not after: the wake that a signal causes is
         * spurious as far as this predicate goes, so the loop would re-park and
         * the check would never be reached if it were on the other side. */
        if (ksig_interrupted()) return SIG_E_INTR;
        /* PARK, do not poll. bkl_hlt_wait() re-dispatched this thread on every
         * interrupt -- ~100 times a second for the whole life of every command
         * /bin/sh runs -- and each pass re-acquired the global kernel lock (which
         * on four cores means spinning with interrupts off if somebody else has
         * it) only to walk a 32-slot table and halt again. Parked, the thread is
         * off the run ring and takes the BKL zero times until the child exits.
         *
         * The timeout is a backstop, not the mechanism: proc_exit() wakes this
         * queue directly, so 200 ms is what a MISSED wake would cost rather than
         * what a normal one does. A wait that can only be ended by a wake is one
         * bug away from a hung shell, and this shell is the machine's console. */
#ifdef KBENCH_NEGCTL
        bkl_hlt_wait();      /* the old poll; tests/boot/run-kbench.sh must FAIL */
#else
        int woke = 0;
        wait_event_timeout(&g_child_wq, have_zombie(self->pid, pid), 200, woke);
        (void)woke;
#endif
    }
}

/* Free zombies that nobody will waitpid() for: GUI apps (ppid 0, "parent" is the
 * WM) and orphans whose parent slot is already gone. Called from the WM loop. */
void proc_reap(void)
{
    fork_report_tick();
    for (int i = 0; i < NPROC; i++) {
        uint64_t freed_cr3 = 0;
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        struct proc *p = &procs[i];
        if (p->state == PROC_ZOMBIE) {
            int orphan = (p->ppid == 0);
            if (!orphan) {                      /* parent-alive check inline (avoid re-locking proc_by_pid) */
                int found = 0;
                for (int j = 0; j < NPROC; j++)
                    if (procs[j].state != PROC_FREE && procs[j].pid == p->ppid) { found = 1; break; }
                orphan = !found;
            }
            if (orphan) {                        /* no live waiter -> claim + free outside the lock */
                freed_cr3 = p->cr3;
                p->state = PROC_FREE; p->pid = 0; p->cr3 = 0;
            }
        }
        spin_unlock_irqrestore(&g_proc_lock, fl);
        if (freed_cr3) vmm_free_space(freed_cr3);
    }
}

/* ===========================================================================
 * The process table as data (SYS_PROCS), and ending a process (SYS_KILL).
 *
 * Both live here, behind one proc_syscall() entry point, for the reason
 * mmsys.c gives for mm_syscall(): the dispatcher in c/kernel/exec/syscall.c
 * gets a four-line forwarding case instead of a body somebody else has to
 * review, and the argument checking sits next to the table it is checking.
 * ===========================================================================*/

/* --- what this can and cannot report --------------------------------------
 * Everything below is read straight out of struct proc. There is no CPU
 * percentage, no resident-memory figure, no I/O and no network column, and
 * that is a statement about the kernel rather than about the reporting:
 *
 *   CPU.       sched.c gives each thread a `slices` counter -- DISPATCHES, not
 *              time -- and `struct thread` is opaque with no lookup by id, so a
 *              proc cannot reach the thread its own ->tid names. Per-process
 *              CPU time needs an accumulator across context_switch, in sched.c.
 *   MEMORY.    pmm refcounts every frame and rmap knows which PTEs point at it,
 *              but nothing sums either per address space. The one per-cr3
 *              number that does exist, vma_reserved_bytes(), counts mmap
 *              reservations -- and no GUI app on this machine calls mmap, so it
 *              reads 0 for every one of them. A resident-set figure needs a
 *              page-table walk in c/kernel/mm/.
 *   DISK/NET.  Not accounted at all. (sock.c knows the owning pid of a socket,
 *              which would give a per-process socket COUNT, but it exposes no
 *              accessor and is another line's file.)
 *
 * Each of those is a small change in a file this one does not own, so each is
 * left undone and said out loud rather than approximated. */
static int proc_list(struct logit_procinfo *out, int max)
{
    struct proc *self = proc_current();
    int selfpid = self ? self->pid : -1;
    int n = 0;

    for (int i = 0; i < NPROC && n < max; i++) {
        struct logit_procinfo e;
        int have = 0;

        /* Snapshot ONE slot under the lock, then copy it out with the lock
         * dropped. user_copy_to() can fault (the caller's buffer is ordinary
         * user memory and may need faulting in), and taking a page fault with
         * g_proc_lock held would deadlock against any fault path that wants the
         * process table. The cost is that the answer is a sample rather than an
         * instant -- which is what every process listing on every system is,
         * and why the table is re-read each refresh rather than cached. */
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        struct proc *p = &procs[i];
        if (p->state != PROC_FREE) {
            e.pid   = p->pid;
            e.ppid  = p->ppid;
            e.state = (p->state == PROC_ZOMBIE) ? LOGIT_PROC_ZOMBIE : LOGIT_PROC_RUNNING;
            e.tid   = p->tid;
            e.flags = 0;
            if (p->gui)            e.flags |= LOGIT_PROC_GUI;
            if (g_killmark[i])     e.flags |= LOGIT_PROC_DYING;
            if (p->pid == selfpid) e.flags |= LOGIT_PROC_SELF;
            /* The SAME predicate proc_kill() refuses on, evaluated here so a UI
             * never has to guess it. See the comment above proc_kill(). */
            if (!p->gui && p->ppid == 0) e.flags |= LOGIT_PROC_PROTECTED;
            e.nfds = 0;
            for (int f = 0; f < NFD; f++) if (p->fd[f]) e.nfds++;
            scopy(e.name, p->name, (int)sizeof e.name);
            scopy(e.cwd,  p->cwd,  (int)sizeof e.cwd);
            have = 1;
        }
        spin_unlock_irqrestore(&g_proc_lock, fl);

        if (!have) continue;
        if (user_copy_to(&out[n], &e, sizeof e) < 0) return -1;
        n++;
    }
    return n;
}

/* --- killing a process ----------------------------------------------------
 * This marks; it does not reach in and tear the victim down where it stands,
 * and the difference is the whole safety argument.
 *
 * A running process is a thread that may be anywhere: in ring 3, or inside a
 * syscall holding the big kernel lock, or parked on a wait queue with a buffer
 * half written. Freeing its address space from another thread's context would
 * pull page tables out from under whatever it is doing. Every existing
 * termination path in this kernel avoids that the same way -- proc_exit() is
 * only ever called BY the dying process, on its own stack: the ring-3 fault
 * handler in interrupts.c calls it from the faulting thread's trap frame, and
 * the window close button does it by sending EV_CLOSE and letting the app call
 * app_exit() itself.
 *
 * So a kill sets a mark, and the victim runs proc_exit() on itself at its next
 * kernel entry (proc_kill_check(), from the syscall gate). That reuses the
 * whole existing, already-correct teardown -- fds closed, sockets released,
 * wm_app_exit() marking the window dead so it leaves the screen, zombie state,
 * address space freed by the reaper -- instead of writing a second copy of it
 * that would have to be kept in step with the first.
 *
 * WHAT THAT COSTS, stated rather than hidden: a process is killed at its next
 * SYSCALL. GUI apps call poll_event() every frame and a shell syscalls
 * constantly, so in practice this is one frame. Two cases are slower, and
 * neither is silently wrong:
 *   - a pure compute loop that never enters the kernel is not killed until it
 *     does. Closing that needs the same check on the timer-interrupt return
 *     path, in c/kernel/cpu/interrupts.c.
 *   - a thread PARKED on a wait queue (a shell blocked in waitpid) is not
 *     running to notice. Waking it needs sched_wake(), which takes a
 *     struct thread * that nothing outside sched.c can obtain.
 * Both files belong to other lines. The mark is durable, so in both cases the
 * kill still happens -- later, not never -- and the table shows the process as
 * DYING in the meantime rather than pretending it is gone.
 *
 * REFUSALS, and how the protected process is IDENTIFIED. The one process that
 * must survive is the shell wm_run() spawns on the serial console -- init here,
 * and the machine's console. It is NOT pid 1: the desktop opens Finder first,
 * so pid 1 is an ordinary GUI app that a task manager should absolutely be
 * allowed to end (Windows lets you kill Explorer). Hard-coding a number would
 * have protected the wrong process, which is exactly the bug a screenshot of
 * the running table caught.
 *
 * The rule used instead is structural: NO PARENT AND NO WINDOW. proc_spawn() is
 * called from exactly one place in the tree (wm.c, for /bin/sh) and is the only
 * thing that ever creates a proc with gui == NULL and ppid == 0 -- a GUI app has
 * a window, and a shell's children carry its pid as their parent. So the test
 * names init without a magic number and without a field to keep in step.
 *
 * The compositor is not in this table at all (it is a kernel thread, not a
 * proc), so it can never be named as a target in the first place. */
static long proc_kill(int pid)
{
    if (pid <= 0) return LOGIT_KILL_PROTECTED;

    long rc = LOGIT_KILL_ENOENT;
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state == PROC_FREE || p->pid != pid) continue;
        if (!p->gui && p->ppid == 0) { rc = LOGIT_KILL_PROTECTED; break; }
        if (p->state == PROC_ZOMBIE) { rc = LOGIT_KILL_ZOMBIE; break; }
        if (!g_killmark[i]) { g_killmark[i] = 1; g_kill_pending++; }
        rc = LOGIT_KILL_OK;
        break;
    }
    spin_unlock_irqrestore(&g_proc_lock, fl);

    if (rc == LOGIT_KILL_OK)
        kprintf("[proc] kill: pid %d marked\n", pid);
    return rc;
}

/* Called from the syscall gate in syscall.c, but ONLY when g_kill_pending says
 * some mark is outstanding -- so the ordinary cost of this feature on the
 * syscall path is a load and a not-taken branch.
 *
 * Does not return if the current process is the one marked: proc_exit() ends in
 * thread_exit(). Returns normally otherwise, including for every process that
 * is not the victim. */
void proc_kill_check(void)
{
    struct proc *self = proc_current();
    if (!self) return;

    int doomed = 0;
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    for (int i = 0; i < NPROC; i++)
        if (&procs[i] == self && g_killmark[i]) {
            g_killmark[i] = 0;                  /* claim it: exactly one exit per mark */
            if (g_kill_pending) g_kill_pending--;
            doomed = 1;
            break;
        }
    spin_unlock_irqrestore(&g_proc_lock, fl);

    if (doomed) {
        kprintf("[proc] kill: pid %d exiting\n", self->pid);
        proc_exit(137);      /* 128 + SIGKILL, the conventional code. Never returns. */
    }
}

/* The gate itself, inlined by syscall.c's caller into one load + one branch. */
int proc_kill_armed(void) { return g_kill_pending != 0; }

long proc_syscall(long num, long a, long b, long c)
{
    (void)c;
    switch (num) {
    case SYS_PROCS: {
        struct logit_procinfo *buf = (struct logit_procinfo *)(uint64_t)a;
        int max = (int)b;
        if (max <= 0 || max > NPROC) max = NPROC;
        if (!buf || !user_range_ok(buf, (uint64_t)max * sizeof *buf, 1)) return -1;
        return proc_list(buf, max);
    }
    case SYS_KILL:
        /* M31: two calls behind one number, split by a FLAG in the third
         * argument rather than by a value in the second.
         *
         * Without the flag this is the historical call and it is bit-for-bit
         * what it always was: (pid, ignored, 0) = destroy that process, through
         * the deferred mark below. c/apps/gui/monitor.c passes exactly that and
         * is untouched.
         *
         * With LOGIT_KILL_SIGNAL it is POSIX kill(2): `b` is the signal number,
         * and 0 is the existence probe rather than "no signal". A flag and not
         * "sig != 0 means signal" precisely because sig 0 has its own meaning
         * and the task manager's zero must not be mistaken for it. */
        if ((c & LOGIT_KILL_SIGNAL))
            return ksig_kill((int)a, (int)b);
        return proc_kill((int)a);

    /* M28: (buf, max, 0) -> the CALLING process's own current CAP_* bitmap.
     * Read-only introspection of a process's own state, so unlike everything
     * else in this file it never refuses -- there is no ceiling to check
     * against yourself. Copies out fs_prefix the same bounded-loop way
     * SYS_GETCWD copies out cwd elsewhere in this file: `a`/`b` are already
     * the calling process's OWN registers (this runs with ITS address space
     * active), so a direct write through a range-checked pointer needs no
     * extra copy helper. See the long comment on SYS_CAP_QUERY in
     * include/abi/logit_abi.h for why this call exists at all. */
    case SYS_CAP_QUERY: {
        struct proc *p = proc_current();
        if (!p) return 0;                 /* a kernel thread holds nothing to report */
        char *buf = (char *)a; int max = (int)b;
        if (buf && max > 0 && user_range_ok(buf, (uint64_t)max, 1)) {
            int i = 0;
            for (; i < max - 1 && p->fs_prefix[i]; i++) buf[i] = p->fs_prefix[i];
            buf[i] = 0;
        }
        return (long)p->caps;
    }
    default:
        return -1;
    }
}

/* ===========================================================================
 * /proc's source, for the process-table half of it.
 *
 * These three functions are DECLARED in c/fs/procfs.h and implemented here,
 * the same way vfs_cred_pid() is declared in c/fs/vfs.h and implemented in
 * c/fs/vfs_cred.c: the fact lives here, behind this file's lock, so the
 * accessor belongs here too. c/fs/procfs.c includes no kernel header at all
 * and reaches the process table through nothing but these -- which is what
 * lets the whole namespace, its pid parsing and its lifetime rules run in a
 * host unit test against a table the test controls.
 *
 * procfs.h costs this file nothing to include: it depends on <stdint.h> and
 * on no kernel header, deliberately.
 *
 * WHY A COPY AND NOT A `struct proc *`. proc_by_pid() already hands out a
 * pointer, and /proc must never hold one -- see the lifetime argument in
 * procfs.h. A snapshot taken under g_proc_lock and returned by value cannot
 * dangle and cannot tear; a pointer read after the lock is dropped can do
 * both, and the window is exactly the one a process exiting on another core
 * occupies.
 *
 * NOT A SECOND ACCOUNTING. Every field is read straight out of struct proc,
 * as proc_list() (SYS_PROCS) does thirty lines above -- the two are two
 * readers of one table and neither computes anything the other does not. What
 * they do not share is a struct: logit_procinfo is a published ABI copied to
 * user memory, and widening or narrowing it to serve /proc would change what
 * c/apps/gui/monitor.c is compiled against. Two callers, two shapes, one
 * table, no third copy of the facts.
 * ======================================================================== */
/* Included here, beside its only three users, rather than in the block at the
 * top of this file: this is a self-contained appendix, and the top of proc.c
 * is edited by several lines at once. */
#include "procfs.h"

int procfs_src_pids(int *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    for (int i = 0; i < NPROC && n < max; i++)
        if (procs[i].state != PROC_FREE) out[n++] = procs[i].pid;
    spin_unlock_irqrestore(&g_proc_lock, fl);
    return n;
}

int procfs_src_task(int pid, struct procfs_task *out)
{
    if (!out || pid <= 0) return 0;
    int found = 0;
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state == PROC_FREE || p->pid != pid) continue;
        out->pid   = p->pid;
        out->ppid  = p->ppid;
        out->tid   = p->tid;
        out->state = (p->state == PROC_ZOMBIE) ? PROCFS_ZOMBIE : PROCFS_RUN;
        out->gui   = p->gui ? 1 : 0;
        out->dying = g_killmark[i] ? 1 : 0;
        out->caps  = p->caps;
        out->cr3   = p->cr3;
        out->nfd_max = NFD;
        out->nfds  = 0;
        for (int f = 0; f < NFD; f++) if (p->fd[f]) out->nfds++;
        scopy(out->name, p->name, (int)sizeof out->name);
        scopy(out->cwd,  p->cwd,  (int)sizeof out->cwd);
        scopy(out->fs_prefix, p->fs_prefix, (int)sizeof out->fs_prefix);
        found = 1;
        break;
    }
    spin_unlock_irqrestore(&g_proc_lock, fl);
    return found;
}

/* Unlocked on purpose, and it is not the same question as the two above:
 * sched_current_data() returns the proc THIS core is running, which cannot be
 * freed while it is running -- it is the caller. There is nothing to race
 * against. */
int procfs_src_self(void)
{
    struct proc *p = proc_current();
    return p ? p->pid : 0;
}

/* ======================================================================
 * THE OUT-OF-MEMORY KILLER'S SEAM  (c/kernel/mm/oom.h)
 *
 * c/kernel/mm must not include this header: mm is UNDERNEATH exec -- the fault
 * path is reached from the scheduler, and pulling the process table down into
 * c/kernel/mm would make every mm host test link the world. So the killer
 * declares four functions and this block is the machine's implementation of
 * them, beside the table and its lock, exactly as the /proc seam above is.
 *
 * The include is here rather than at the top of the file for a reason that is
 * about this tree and not about C: several lines are editing proc.c's
 * neighbourhood, and a change that is ONE contiguous append at the end cannot
 * conflict with a change made anywhere above it.
 * ====================================================================== */
#include "oom.h"

/* The killer's scratch table is sized at compile time because it may not
 * allocate (oom.h). If NPROC ever passes it, the sweep would silently ignore
 * the tail of the table -- i.e. quietly stop considering some processes -- so
 * it is a build error instead. */
_Static_assert(OOM_MAXTASK >= NPROC, "oom.h's OOM_MAXTASK must cover NPROC");

int oom_task_at(int idx, struct oom_task *out)
{
    if (idx < 0 || idx >= NPROC || !out) return 0;
    int live = 0;
    uint64_t fl = spin_lock_irqsave(&g_proc_lock);
    struct proc *p = &procs[idx];
    if (p->state == PROC_RUNNING && p->cr3) {
        out->pid    = p->pid;
        out->cr3    = p->cr3;
        out->gui    = p->gui ? 1 : 0;
        /* THE SAME PREDICATE proc_kill() REFUSES ON, evaluated here rather than
         * discovered by trying: the killer has to skip init while CHOOSING, or
         * it picks the console shell, gets refused, and reports "no victim"
         * having never looked at the process actually holding the memory.
         * proc_list() already makes this same argument for the task manager. */
        out->immune = (!p->gui && p->ppid == 0);
        out->dying  = g_killmark[idx] ? 1 : 0;
        scopy(out->name, p->name, (int)sizeof out->name);
        live = 1;
    }
    spin_unlock_irqrestore(&g_proc_lock, fl);
    return live;
}

int oom_task_kill(int pid)
{
    return proc_kill(pid) == LOGIT_KILL_OK ? 0 : -1;
}

int oom_task_self(void)
{
    struct proc *p = proc_current();
    return p ? p->pid : 0;
}

/* THE CHEAPEST VICTIM IS ONE THAT IS ALREADY DEAD.
 *
 * proc_exit() makes a process a zombie and leaves its ADDRESS SPACE intact:
 * "freed by proc_waitpid (parent) or proc_reap (orphan/GUI)". That is right for
 * a machine with memory -- the teardown runs on somebody's stack, not the dying
 * thread's -- and it is a trap for the killer, because the two reapers are:
 *
 *   proc_waitpid   the parent, when it gets around to it. /bin/sh only sweeps
 *                  finished background jobs in its INTERACTIVE loop
 *                  (reap_background(), sh.c) -- the serial console runs the
 *                  non-interactive branch, which never calls it. A background
 *                  job killed there stays a zombie holding every frame it took.
 *   proc_reap      the window manager's loop, and only for ORPHANS.
 *
 * So "kill the biggest process" can free nothing at all, for an unbounded time,
 * on the exact machine that has no memory. The frames of a process that has
 * already exited belong to nobody: taking them back is not a policy decision
 * and needs no victim.
 *
 * vmm_free_user() is the whole operation -- it drops the private user subtree
 * and leaves the PML4/PDPT husk for whichever reaper eventually arrives. IT IS
 * IDEMPOTENT: it clears pdpt[USER_PDPT_IDX] on the way out, so the later
 * vmm_free_space() returns at its second line and then frees the two table
 * frames as it always did. That is what makes this safe to do early rather than
 * a race with the reaper -- the alternative, teaching proc_waitpid() that
 * somebody else may have got there first, would have put a new invariant in
 * the exit path to pay for a memory optimisation.
 *
 * Returns the number of ZOMBIE address spaces it visited, including ones that
 * were already empty -- a second call is a no-op and still counts. The caller
 * measures FRAMES (pmm_free_frames() across the call), because that is the
 * question actually being asked and this number cannot answer it.
 *
 * THE WINDOW, stated rather than hidden: proc_exit() sets PROC_ZOMBIE and only
 * then calls wm_app_exit() and thread_exit(), so the dying thread can still be
 * executing kernel code on this cr3 when this runs. It touches no user memory
 * in that window (its stack is a kernel stack, kmalloc'd, outside this
 * subtree). The pre-existing proc_waitpid() path frees the PML4 FRAME ITSELF in
 * the same window, which is strictly the larger exposure; this is not the place
 * to close either. */
int oom_task_reap_dead(void)
{
    int n = 0;
    for (int i = 0; i < NPROC; i++) {
        uint64_t cr3 = 0;
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        struct proc *p = &procs[i];
        if (p->state == PROC_ZOMBIE && p->cr3) cr3 = p->cr3;
        spin_unlock_irqrestore(&g_proc_lock, fl);
        /* Outside the lock: vmm_free_user() calls into the PMM and the VMA
         * table, and this file's lock order is g_proc_lock -> ... -> g_pmm_lock
         * with "nothing under g_proc_lock calls vmm/kheap" (top of file). The
         * pid stays in the table with its cr3, so the reaper's later
         * vmm_free_space() still runs -- this only empties the space. */
        if (cr3) { vmm_free_user(cr3); n++; }
    }
    return n;
}
