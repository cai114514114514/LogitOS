#include <stdint.h>
#include <stddef.h>
#include "uthread.h"
#include "sched.h"
#include "proc.h"
#include "spinlock.h"
#include "usercopy.h"
#include "vmm.h"
#include "mm.h"           /* mm_syscall(SYS_MUNMAP, ...): a thread stack is an mmap */
#include "pit.h"
#include "kprintf.h"
#include "logit_abi.h"
/* Path-qualified for the reason spelled out at the top of c/kernel/exec/
 * syscall.c: mini-libc ships a sys/wait.h and the bare form has resolved to the
 * wrong file before. */
#include "kernel/core/wait.h"

/* See uthread.h for the model. This file is the thread TABLE, the join/detach
 * lifecycle, and the futex. */

#define NUT 128     /* descriptors machine-wide. LOGIT_THREADS_MAX (64) bounds one
                     * PROCESS; this bounds the machine, and is deliberately larger
                     * than NPROC (32) but far short of 32*64 -- a table sized for
                     * every process to be maximally threaded would be 6 KiB of
                     * mostly-idle memory on a machine whose whole desktop is 229
                     * MiB. Running out is reported as THR_E_FULL, not as a
                     * mysterious failure. */

#define UT_FREE   0
#define UT_LIVE   1
#define UT_EXITED 2   /* ended; the descriptor is held only for a joiner */

struct uthread {
    int      state;
    int      tid;              /* scheduler thread id -- the handle userland holds */
    int      pid;              /* owning process */
    int      detached;
    int      joining;          /* a joiner has claimed this thread; a second is refused */
    int      main;             /* the thread the process started with */
    uint64_t retval;
    uint64_t stack_base;       /* mmap region to release when the thread ends... */
    uint64_t stack_len;        /* ...0 = the stack is the caller's to free */
};

static struct uthread g_ut[NUT];
static spinlock_t     g_ut_lock = SPINLOCK_INIT;

/* "This process is exiting and its threads do not know it yet", one entry per
 * process, plus the exit code the first caller chose. A parallel array rather
 * than a field in struct proc for the reason proc.c's g_killmark gives: proc.h
 * is not this line's header to change, and an array indexed by pid is exactly
 * as correct provided it is cleared when a pid is claimed -- which it is, in
 * ut_exiting_clear() from proc_exit()'s teardown branch.
 *
 * g_exit_armed is the gate: the number of processes currently marked. It exists
 * so the check on the syscall path costs one load of a global and one
 * never-taken branch on a machine where nothing is exiting. */
static int  g_exit_pid[NPROC];
static int  g_exit_code[NPROC];
static volatile unsigned long g_exit_armed;

/* Where a joiner waits. ONE queue for every joiner in the machine, not one per
 * descriptor, for the reason proc.c's g_child_wq gives: the predicate ("the
 * thread I named has exited") is per-waiter and must be re-tested on every wake
 * regardless, so a shared queue costs a spurious table scan and saves a struct
 * per descriptor.
 *
 * LOCK ORDER: g_join_wq.lock -> g_ut_lock, because the predicate is evaluated
 * inside wait_event() with the queue lock held. Every waker therefore RELEASES
 * g_ut_lock before it wakes; nothing ever holds both in the other order. */
static struct waitq g_join_wq = WAITQ_INIT;

static unsigned long g_created, g_reaped, g_futex_waits, g_futex_wakes;

/* --- the table, all under g_ut_lock ------------------------------------- */

static struct uthread *ut_by_tid_locked(int tid)
{
    for (int i = 0; i < NUT; i++)
        if (g_ut[i].state != UT_FREE && g_ut[i].tid == tid) return &g_ut[i];
    return NULL;
}

static struct uthread *ut_alloc_locked(void)
{
    for (int i = 0; i < NUT; i++)
        if (g_ut[i].state == UT_FREE) {
            struct uthread *u = &g_ut[i];
            u->state = UT_LIVE; u->tid = -1; u->pid = 0;
            u->detached = 0; u->joining = 0; u->main = 0;
            u->retval = 0; u->stack_base = 0; u->stack_len = 0;
            return u;
        }
    return NULL;
}

static void ut_free_locked(struct uthread *u)
{
    u->state = UT_FREE;
    u->tid = -1;
    g_reaped++;
}

static int ut_proc_count_locked(int pid, int live_only)
{
    int n = 0;
    for (int i = 0; i < NUT; i++) {
        if (g_ut[i].state == UT_FREE || g_ut[i].pid != pid) continue;
        if (live_only && g_ut[i].state != UT_LIVE) continue;
        n++;
    }
    return n;
}

int uthread_proc_live(int pid)
{
    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    int n = ut_proc_count_locked(pid, 1);
    spin_unlock_irqrestore(&g_ut_lock, f);
    return n;
}

/* Adopt the calling thread if it has no descriptor yet.
 *
 * LAZY, and that is the point: wm_launch(), proc_spawn() and execve() all
 * create the first thread of a process, they all live in files this line does
 * not own, and none of them needs to learn that threads exist. The first time
 * a process asks a thread question, its main thread appears in the table with
 * the right pid and stack_base 0 (its stack came from exec.c's reservation, so
 * the kernel must NOT unmap it here -- freeing the whole address space does
 * that, once, at process exit). */
int uthread_self(void)
{
    struct proc *p = proc_current();
    int tid = sched_current_tid();
    if (!p || tid < 0) return -1;

    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    struct uthread *u = ut_by_tid_locked(tid);
    if (!u) {
        u = ut_alloc_locked();
        if (u) {
            u->tid = tid;
            u->pid = p->pid;
            u->main = 1;
            u->detached = 1;      /* nobody joins a main thread */
            g_created++;
        }
    }
    spin_unlock_irqrestore(&g_ut_lock, f);
    return u ? tid : -1;
}

/* --- exit ---------------------------------------------------------------- */

void uthread_release_self(uint64_t retval)
{
    int tid = sched_current_tid();
    uint64_t base = 0, len = 0;
    int wake = 0;

    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    struct uthread *u = ut_by_tid_locked(tid);
    if (u && u->state == UT_LIVE) {
        u->retval = retval;
        u->state  = UT_EXITED;
        base = u->stack_base; len = u->stack_len;
        u->stack_base = 0; u->stack_len = 0;
        if (u->detached) ut_free_locked(u);       /* nobody will ever ask for retval */
        else wake = 1;
    }
    spin_unlock_irqrestore(&g_ut_lock, f);

    /* THE STACK IS FREED HERE, ON PURPOSE, and it is safe for a reason worth
     * writing down: a thread reaching this point is inside a syscall, running
     * on its KERNEL stack, and will never return to ring 3. Its user stack is
     * therefore already dead memory. The alternative -- leaving it for the
     * joiner to unmap -- cannot work for a DETACHED thread, because by
     * definition nobody arrives to do it; and having two rules (joined stacks
     * freed there, detached stacks freed here) is how one of them rots. One
     * rule: whoever gave the kernel a stack_base gets it unmapped by the thread
     * that stops using it.
     *
     * Outside the lock: mm_syscall walks and edits page tables, and g_ut_lock
     * is an irqsave spinlock -- the lock order in this kernel puts mm below
     * everything, and taking it under a leaf lock is how the orders reverse. */
    if (base && len) mm_syscall(SYS_MUNMAP, (long)base, (long)len, 0);

    /* Outside the lock for the reason the g_join_wq comment gives: the joiner
     * holds g_join_wq.lock while it takes g_ut_lock, so waking with g_ut_lock
     * held would close an AB-BA cycle. It cannot be missed -- the joiner is
     * enqueued under the queue lock and stays enqueued until it is parked, so a
     * wake arriving in that window blocks on the queue lock instead of being
     * lost (c/kernel/core/wait.h rule 2). */
    if (wake) waitq_wake_all(&g_join_wq);
}

static void ut_exiting_clear(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (g_exit_pid[i] == pid) {
            g_exit_pid[i] = 0;
            if (g_exit_armed) g_exit_armed--;
        }
}

int uthread_proc_kill(int pid, int code)
{
    int wake[NUT], nw = 0, eff = code;

    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    int slot = -1;
    for (int i = 0; i < NPROC; i++) if (g_exit_pid[i] == pid) { slot = i; break; }
    if (slot >= 0) {
        /* Already marked: the FIRST exit code wins. A sibling that arrives here
         * through uthread_exit_check() passes 0 because it has no opinion, and
         * letting that overwrite the code the exiting thread actually chose is
         * how `sh` reports 0 for a command that failed. */
        eff = g_exit_code[slot];
    } else {
        for (int i = 0; i < NPROC; i++)
            if (!g_exit_pid[i]) {
                g_exit_pid[i] = pid; g_exit_code[i] = code; g_exit_armed++;
                break;
            }
    }
    /* Snapshot the tids to wake; do the waking outside the lock (sched_wake_id
     * takes g_sched_lock, and the order here must stay g_ut_lock -> nothing). */
    int me = sched_current_tid();
    for (int i = 0; i < NUT && nw < NUT; i++)
        if (g_ut[i].state == UT_LIVE && g_ut[i].pid == pid && g_ut[i].tid != me)
            wake[nw++] = g_ut[i].tid;
    spin_unlock_irqrestore(&g_ut_lock, f);

    /* A parked sibling is woken so that it RETURNS from its wait and reaches
     * the syscall gate, where uthread_exit_check() ends it. The wake itself is
     * spurious as far as the sibling's own predicate is concerned -- which is
     * fine, because every sleep site in this kernel is a `while (!cond)` loop
     * by rule. What this does NOT reach is a sibling spinning in ring 3 with no
     * syscall in it; that one dies at its next kernel entry, exactly as a
     * SYS_KILL victim does, and for the same reason (see proc_kill()). */
    for (int i = 0; i < nw; i++) sched_wake_id(wake[i]);
    if (nw) waitq_wake_all(&g_join_wq);
    return eff;
}

void uthread_proc_reap(int pid)
{
    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    for (int i = 0; i < NUT; i++)
        if (g_ut[i].state != UT_FREE && g_ut[i].pid == pid) ut_free_locked(&g_ut[i]);
    ut_exiting_clear(pid);
    spin_unlock_irqrestore(&g_ut_lock, f);
}

int uthread_exit_armed(void) { return g_exit_armed != 0; }

void uthread_exit_check(void)
{
    struct proc *p = proc_current();
    if (!p) return;

    int doomed = 0;
    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    for (int i = 0; i < NPROC; i++)
        if (g_exit_pid[i] == p->pid) { doomed = 1; break; }
    spin_unlock_irqrestore(&g_ut_lock, f);
    if (!doomed) return;

    /* proc_exit() is the single teardown path, exactly as it is for SYS_KILL.
     * It re-derives "am I the last thread" itself, so a thread arriving here
     * either ends quietly or performs the whole process teardown -- never both,
     * and never neither. */
    proc_exit(0);                       /* never returns */
}

/* --- create -------------------------------------------------------------- */

static long ut_create(const struct logit_thread_spec *uspec)
{
    struct proc *p = proc_current();
    struct logit_thread_spec s;

    if (!p) return THR_E_ARG;
    if (uthread_self() < 0) return THR_E_ARG;          /* adopt the caller first */
    if (user_copy_from(&s, uspec, sizeof s) < 0) return THR_E_ARG;

    if (!s.entry || !s.stack_top) return THR_E_ARG;
    if (!user_range_ok((const void *)s.entry, 1, 0)) return THR_E_ARG;
    if (s.stack_base) {
        if (s.stack_len < LOGIT_THREAD_STACK_MIN ||
            s.stack_len > LOGIT_THREAD_STACK_MAX) return THR_E_ARG;
        if (s.stack_top <= s.stack_base ||
            s.stack_top > s.stack_base + s.stack_len) return THR_E_ARG;
    }

    /* THE INITIAL STACK, and why two words are written into it.
     *
     * A new ring-3 thread is entered by c/boot/enter_user.asm's IRETQ, and that
     * file belongs to the boot line -- so the register state the thread starts
     * with is not this line's to choose. It is also not empty: enter_user does
     * `mov fs, ax`, which ZEROES the %fs base as its last act before the IRETQ.
     * So the kernel cannot hand the thread its TLS pointer in a register and
     * cannot pre-load it in the MSR either -- anything set before that
     * instruction is destroyed by it.
     *
     * The stack survives. So `arg` and `tls` are written at the top of the new
     * thread's stack and the ring-3 trampoline (__logit_thread_entry in
     * c/apps/libc/src/pthread_entry.asm) reads them from there: it issues
     * SYS_SET_TLS with the second word as its very first instruction, then
     * calls the C body with the first. Ten instructions in userland instead of
     * an edit to shared boot assembly.
     *
     * rsp is 16-aligned here rather than the (16n - 8) that thread_create()'s
     * long comment demands, because the trampoline is ASSEMBLY and realigns
     * before its own `call` -- no C prologue ever runs on this frame, which is
     * the same reason that comment gives for ring3_bootstrap being exempt. */
    uint64_t rsp = (s.stack_top & ~(uint64_t)0xF) - 16;
    uint64_t words[2] = { s.arg, s.tls };
    if (user_copy_to((void *)rsp, words, sizeof words) < 0) return THR_E_ARG;

    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    if (ut_proc_count_locked(p->pid, 0) >= LOGIT_THREADS_MAX) {
        spin_unlock_irqrestore(&g_ut_lock, f);
        return THR_E_FULL;
    }
    struct uthread *u = ut_alloc_locked();
    if (!u) { spin_unlock_irqrestore(&g_ut_lock, f); return THR_E_FULL; }
    u->pid = p->pid;
    u->stack_base = s.stack_base;
    u->stack_len  = s.stack_len;
    spin_unlock_irqrestore(&g_ut_lock, f);

    /* Same struct proc, same cr3 -- that is what makes this a thread and not a
     * process. thread_create_user() splices it onto the shared run ring, so it
     * can be picked by ANY core, including one that is not this one; the
     * descriptor is already in the table with its stack recorded, so a thread
     * that starts and exits before this function returns still cleans up. */
    int tid = thread_create_user(p->name, s.entry, rsp, p, p->cr3);
    if (tid < 0) {
        uint64_t f2 = spin_lock_irqsave(&g_ut_lock);
        ut_free_locked(u);
        spin_unlock_irqrestore(&g_ut_lock, f2);
        return THR_E_NOMEM;
    }

    uint64_t f3 = spin_lock_irqsave(&g_ut_lock);
    u->tid = tid;
    g_created++;
    spin_unlock_irqrestore(&g_ut_lock, f3);
    return tid;
}

/* --- join / detach ------------------------------------------------------- */

/* The join predicate, split out so wait_event_timeout() can re-evaluate it.
 * Takes g_ut_lock inside g_join_wq.lock, which is the documented order. */
static int join_ready(int tid)
{
    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    struct uthread *u = ut_by_tid_locked(tid);
    int done = (!u || u->state == UT_EXITED);
    spin_unlock_irqrestore(&g_ut_lock, f);
    return done;
}

static long ut_join(int tid, uint64_t *uret)
{
    struct proc *p = proc_current();
    if (!p) return THR_E_ARG;
    if (uthread_self() < 0) return THR_E_ARG;
    if (tid == sched_current_tid()) return THR_E_DEADLK;

    /* Claim the join under the lock BEFORE waiting. Two threads joining one
     * thread is a program bug POSIX calls undefined; refusing the second is
     * better than racing to deliver one return value twice. */
    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    struct uthread *u = ut_by_tid_locked(tid);
    if (!u || u->pid != p->pid) { spin_unlock_irqrestore(&g_ut_lock, f); return THR_E_SRCH; }
    if (u->detached || u->joining) { spin_unlock_irqrestore(&g_ut_lock, f); return THR_E_INVAL; }
    u->joining = 1;
    spin_unlock_irqrestore(&g_ut_lock, f);

    for (;;) {
        uint64_t retval = 0; int have = 0;
        f = spin_lock_irqsave(&g_ut_lock);
        u = ut_by_tid_locked(tid);
        if (!u) { spin_unlock_irqrestore(&g_ut_lock, f); return THR_E_SRCH; }
        if (u->state == UT_EXITED) {
            retval = u->retval;
            ut_free_locked(u);            /* the join IS the reap */
            have = 1;
        }
        spin_unlock_irqrestore(&g_ut_lock, f);

        if (have) {
            if (uret && user_copy_to(uret, &retval, sizeof retval) < 0) return THR_E_ARG;
            return 0;
        }
        /* PARK, do not poll -- the whole argument is in proc_waitpid(). The
         * 200 ms is a backstop for a MISSED wake, not the mechanism:
         * uthread_release_self() wakes this queue directly. It is also what
         * lets a joiner notice that its own process has been marked exiting,
         * which is checked at the top of the next pass through
         * uthread_exit_check() at the syscall gate. */
        int woke = 0;
        wait_event_timeout(&g_join_wq, join_ready(tid), 200, woke);
        (void)woke;
    }
}

static long ut_detach(int tid)
{
    struct proc *p = proc_current();
    if (!p) return THR_E_ARG;
    if (uthread_self() < 0) return THR_E_ARG;

    uint64_t f = spin_lock_irqsave(&g_ut_lock);
    struct uthread *u = ut_by_tid_locked(tid);
    long rc;
    if (!u || u->pid != p->pid)      rc = THR_E_SRCH;
    else if (u->detached || u->joining) rc = THR_E_INVAL;
    else {
        u->detached = 1;
        /* Already finished: the descriptor was being held only for a joiner who
         * is now never coming. Free it here rather than leaving it to a sweep,
         * so "detach frees without a join" is true immediately and a leak test
         * can see it. */
        if (u->state == UT_EXITED) ut_free_locked(u);
        rc = 0;
    }
    spin_unlock_irqrestore(&g_ut_lock, f);
    return rc;
}

/* --- the futex ----------------------------------------------------------- */

/* See the SYS_FUTEX note in include/abi/logit_abi.h for why a futex and not a
 * kernel mutex syscall. This is the kernel half, and it is small on purpose:
 * it does not invent a sleep. It parks on sched_block_self_unlock(), whose
 * lost-wakeup argument (the long comment in sched.h) is what makes the
 * "compare the word, then park" sequence atomic against a waker.
 *
 * THE KEY is (cr3, address). Not a physical frame: nothing on this machine
 * shares memory between address spaces -- there is no MAP_SHARED, no shm, and
 * no file-backed mapping at all (c/kernel/mm/mmsys.c) -- so two processes can
 * never be waiting on the same word, and a virtual key is both correct and
 * immune to the page moving under swap.
 *
 * READING THE USER WORD UNDER THE BUCKET LOCK. The compare has to happen under
 * the same lock the wake takes, or the wake slips between the compare and the
 * park -- that is the whole lost-wakeup shape. But user memory can fault, and a
 * fault taken while holding a spinlock with IF=0 would deadlock. Two things
 * make it safe, and both are load-bearing:
 *   1. user_range_ok() runs FIRST, outside the lock. It does not merely check:
 *      it resolves copy-on-write and fills an untouched anonymous page (see
 *      c/kernel/exec/usercopy.c), so the page is present before the lock is
 *      taken.
 *   2. Nothing can take it away again inside the window. Only this process can
 *      unmap its own memory, and its other threads cannot be in the kernel --
 *      SYS_FUTEX is not in syscall_is_bkl_free(), so this core holds the big
 *      kernel lock. Reclaim (c/kernel/mm/reclaim.c) is likewise a BKL path.
 * The second point is a dependency on the BKL, so it is stated rather than
 * assumed: the day a BKL-free munmap exists, this needs a pinned page.
 */
#define FBUCKETS 64

struct fwaiter {
    struct fwaiter *next;
    uint64_t        key;
    int             tid;
    int             woken;
};

struct fbucket {
    spinlock_t      lock;
    struct fwaiter *head;
};

static struct fbucket g_fb[FBUCKETS];

static unsigned fhash(uint64_t key)
{
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 29;
    return (unsigned)(key % FBUCKETS);
}

static long futex_wait(uint32_t *uaddr, uint32_t val, unsigned timeout_ms)
{
    uint64_t key = (uint64_t)(uintptr_t)uaddr ^ (sched_current_cr3() << 1);
    struct fbucket *b = &g_fb[fhash(key)];
    struct fwaiter w;

    /* Resolve the page BEFORE the lock -- see the long note above. */
    if (!user_range_ok(uaddr, sizeof *uaddr, 0)) return FUTEX_E_ARG;

    uint64_t f = spin_lock_irqsave(&b->lock);
    if (*(volatile uint32_t *)uaddr != val) {
        spin_unlock_irqrestore(&b->lock, f);
        return FUTEX_E_AGAIN;
    }
    w.key = key; w.tid = sched_current_tid(); w.woken = 0;
    w.next = b->head; b->head = &w;
    g_futex_waits++;

    int got;
    if (timeout_ms)
        got = sched_block_self_unlock_until(&b->lock, f, wait_deadline_ms(timeout_ms));
    else { sched_block_self_unlock(&b->lock, f); got = 1; }
    /* `w` lives on this thread's kernel stack, which is exactly why it must be
     * unlinked before this frame goes away -- and why the waker only ever marks
     * it and wakes, never frees it. */
    f = spin_lock_irqsave(&b->lock);
    for (struct fwaiter **pp = &b->head; *pp; pp = &(*pp)->next)
        if (*pp == &w) { *pp = w.next; break; }
    int woken = w.woken;
    spin_unlock_irqrestore(&b->lock, f);

    if (woken) return 0;
    return got ? 0 : FUTEX_E_TIMEDOUT;    /* a wake we did not cause is spurious: 0 */
}

static long futex_wake(uint32_t *uaddr, uint32_t n)
{
    uint64_t key = (uint64_t)(uintptr_t)uaddr ^ (sched_current_cr3() << 1);
    struct fbucket *b = &g_fb[fhash(key)];
    int woke = 0;
    int tids[32], nt = 0;

    uint64_t f = spin_lock_irqsave(&b->lock);
    for (struct fwaiter *w = b->head; w && (uint32_t)woke < n; w = w->next) {
        if (w->key != key || w->woken) continue;
        w->woken = 1;                    /* marked under the bucket lock, so a second
                                          * wake in flight cannot count it twice */
        if (nt < 32) tids[nt++] = w->tid;
        woke++;
    }
    spin_unlock_irqrestore(&b->lock, f);

    /* Unpark outside the bucket lock. It would be legal inside it (the order
     * bucket -> g_sched_lock is the same one waitq_wake_one uses), but holding a
     * lock across an unbounded number of wakes on a broadcast is how a
     * pthread_cond_broadcast with 60 waiters becomes a visible stall. The
     * marking above is what makes this safe to do after the release: a waiter
     * whose `woken` is set will return 0 whether or not it was still parked
     * when we got to it. */
    for (int i = 0; i < nt; i++) sched_wake_id(tids[i]);
    g_futex_wakes += (unsigned)woke;
    return woke;
}

/* --- syscall dispatch ---------------------------------------------------- */

long uthread_syscall(long num, long a, long b, long c)
{
    switch (num) {
    case SYS_THREAD_CREATE:
        return ut_create((const struct logit_thread_spec *)(uint64_t)a);

    case SYS_THREAD_EXIT: {
        uthread_self();
        uthread_release_self((uint64_t)a);
        struct proc *p = proc_current();
        /* THE LAST THREAD OUT RUNS THE PROCESS TEARDOWN. Not "the main thread"
         * -- POSIX is explicit that a process ends when its last thread does,
         * whichever that is, and main() returning is just the common case. */
        if (p && uthread_proc_live(p->pid) == 0) proc_exit(0);   /* never returns */
        thread_exit();                                            /* never returns */
        return 0;
    }

    case SYS_THREAD_JOIN:
        return ut_join((int)a, (uint64_t *)(uint64_t)b);

    case SYS_THREAD_DETACH:
        return ut_detach((int)a);

    case SYS_THREAD_SELF:
        return uthread_self();

    case SYS_SET_TLS: {
        uint64_t v = (uint64_t)a;
        /* A base of 0 is "no TLS" and is always allowed (that is what every
         * thread starts with). Anything else must be memory the caller owns,
         * or a process could point %fs at the kernel and let the compiler's
         * `%fs:`-relative stores do the writing for it. The TCB's self-pointer
         * lives at [fsbase], so one readable word is the minimum that must be
         * true for it to be a TCB at all. */
        if (v && !user_range_ok((const void *)v, 8, 0)) return -1;
        sched_set_fsbase(v);
        return 0;
    }

    case SYS_FUTEX: {
        uint32_t *uaddr = (uint32_t *)(uint64_t)a;
        uint32_t val    = (uint32_t)((uint64_t)b & 0xFFFFFFFFu);
        unsigned tmo    = (unsigned)(((uint64_t)b >> 32) & 0xFFFFFFFFu);
        if (!uaddr || ((uintptr_t)uaddr & 3)) return FUTEX_E_ARG;
        if ((int)c == FUTEX_WAIT) return futex_wait(uaddr, val, tmo);
        if ((int)c == FUTEX_WAKE) return futex_wake(uaddr, val ? val : 1);
        return FUTEX_E_ARG;
    }

    case SYS_THREAD_INFO: {
        struct proc *p = proc_current();
        uint64_t f; long n = 0;
        switch ((int)a) {
        case THRINFO_LIVE:      return p ? uthread_proc_live(p->pid) : -1;
        case THRINFO_SLOTS:
            f = spin_lock_irqsave(&g_ut_lock);
            for (int i = 0; i < NUT; i++) if (g_ut[i].state != UT_FREE) n++;
            spin_unlock_irqrestore(&g_ut_lock, f);
            return n;
        case THRINFO_SLOTS_MAX: return NUT;
        case THRINFO_CREATED:   return (long)g_created;
        case THRINFO_REAPED:    return (long)g_reaped;
        default:                return -1;
        }
    }

    default:
        return -1;
    }
}
