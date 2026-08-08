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
            ret = p;
            break;
        }
    spin_unlock_irqrestore(&g_proc_lock, fl);
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
    for (int i = 0; i < NFD; i++) {
        child->fd[i] = parent->fd[i];
        if (child->fd[i]) file_dup(child->fd[i]);
    }

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
        uint64_t fl = spin_lock_irqsave(&g_proc_lock);
        p->exit_code = code;
        p->state = PROC_ZOMBIE;
        spin_unlock_irqrestore(&g_proc_lock, fl);
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

/* Reap one zombie child of the current process. Blocks (cooperatively) until a
 * matching child is a zombie. pid == -1 waits for any child. */
long proc_waitpid(int pid, int *status)
{
    struct proc *self = proc_current();
    if (!self) return -1;

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
        return proc_kill((int)a);
    default:
        return -1;
    }
}
