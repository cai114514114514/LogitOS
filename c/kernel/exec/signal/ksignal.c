/* ===========================================================================
 * M31 -- signals: the table, the pending set, and everything that is not the
 * frame. The frame lives in c/kernel/exec/signal/ksigframe.c.
 *
 * Read the header comment in ksignal.h first for where delivery happens and
 * why there is only one such place.
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include "ksignal.h"
#include "ksig_int.h"
#include "proc.h"
#include "sched.h"
#include "uthread.h"
#include "serial.h"
#include "pit.h"
#include "kprintf.h"
#include "usercopy.h"
#include "logit_abi.h"
/* Path-qualified for the reason file.c gives: mini-libc ships
 * c/apps/libc/include/sys/wait.h and INCDIRS is one flat sorted list. */
#include "kernel/sync/wait.h"   /* the console's wait queue -- poll() needs one */
#include "file.h"               /* file_timerfd_tick(): the tick IS timerfd's clock */

spinlock_t   g_sig_lock = SPINLOCK_INIT;
struct sigst g_sig[NPROC];

volatile uint64_t g_sig_delivered, g_sig_returned, g_sig_posted;
volatile uint64_t g_sig_defaulted, g_sig_dropped, g_sig_fpusaved;

/* THE GATE. The number of processes holding at least one undelivered signal.
 * Every kernel exit to ring 3 tests this, so on a machine where nothing has
 * been signalled the whole feature costs one relaxed load and a branch that is
 * never taken -- the same discipline proc_kill_armed() and uthread_exit_armed()
 * already use, and the reason this could be put on the interrupt return path at
 * all rather than only on the syscall gate. */
static volatile unsigned long g_sig_armed;

/* Alarms are gated separately: the timer tick consults this, not the table. */
static volatile unsigned long g_alarm_armed;

/* ---------------------------------------------------------------------------
 * Default actions. The table is spelled out rather than derived because it is
 * the part a reader will want to check against POSIX line by line.
 * ------------------------------------------------------------------------- */
int ksig_default_action(int signo)
{
    switch (signo) {
    case LOGIT_SIGCHLD: case LOGIT_SIGURG: case LOGIT_SIGWINCH:
        return DFL_IGN;
    case LOGIT_SIGCONT:
        return DFL_CONT;
    case LOGIT_SIGSTOP: case LOGIT_SIGTSTP:
    case LOGIT_SIGTTIN: case LOGIT_SIGTTOU:
        return DFL_STOP;
    default:
        return DFL_TERM;   /* including SIGKILL, SIGINT, SIGTERM, SIGSEGV, ... */
    }
}

#define SIGBIT(n) (1ull << (n))
/* The two that can be neither caught nor blocked. Kept as a mask so every place
 * that has to make the exception says it the same way. */
#define SIG_UNMASKABLE (SIGBIT(LOGIT_SIGKILL) | SIGBIT(LOGIT_SIGSTOP))

/* Which signals, right now, would do nothing at all if delivered. Recomputed at
 * the one place a disposition changes so that ksig_interrupted() -- which runs
 * unlocked, inside waitqueue predicates -- can answer from a single word.
 *
 * A signal is "ignored" if its handler is SIG_IGN, or if it is SIG_DFL and the
 * default action is to ignore. SIGKILL and SIGSTOP are never ignored no matter
 * what the table says, because sigaction refuses to change them. */
void ksig_recalc_ignored(struct sigst *s)
{
    uint64_t ig = 0;
    for (int n = 1; n < KSIG_NSIG; n++) {
        if (SIGBIT(n) & SIG_UNMASKABLE) continue;
        if (s->handler[n] == 1) { ig |= SIGBIT(n); continue; }              /* SIG_IGN */
        if (s->handler[n] == 0 && ksig_default_action(n) == DFL_IGN)
            ig |= SIGBIT(n);
    }
    s->ignored = ig;
}

/* pending is also the gate's input, so it moves only through these two. */
void ksig_set_pending(struct sigst *s, uint64_t bits)
{
    if (!bits) return;
    if (!s->pending) __atomic_fetch_add(&g_sig_armed, 1, __ATOMIC_RELAXED);
    s->pending |= bits;
}

void ksig_clear_pending(struct sigst *s, uint64_t bits)
{
    if (!s->pending) return;
    s->pending &= ~bits;
    if (!s->pending && g_sig_armed) __atomic_fetch_sub(&g_sig_armed, 1, __ATOMIC_RELAXED);
}

int ksig_armed(void) { return __atomic_load_n(&g_sig_armed, __ATOMIC_RELAXED) != 0; }

struct sigst *ksig_find_locked(int pid)
{
    if (pid <= 0) return NULL;
    for (int i = 0; i < NPROC; i++)
        if (g_sig[i].pid == pid) return &g_sig[i];
    return NULL;
}

/* At most the adopted thread table plus one lazy main thread per PCB can
 * fault. Keep the budget derived from their owning tables, not an unrelated
 * CPU count: a fault can remain pending across a context switch. */
static struct ksig_fault_record g_faults[UTHREAD_TABLE_MAX + NPROC];
static void fault_clear_locked(int pid)
{
    for (unsigned i = 0; i < sizeof g_faults / sizeof g_faults[0]; i++)
        if (g_faults[i].signo && g_faults[i].pid == pid) {
            g_faults[i].signo = 0;
            __atomic_fetch_sub(&g_sig_armed, 1, __ATOMIC_RELAXED);
        }
}
int ksig_take_fault_locked(struct ksig_fault_record *out)
{
    int tid = sched_current_tid();
    for (unsigned i = 0; i < sizeof g_faults / sizeof g_faults[0]; i++)
        if (g_faults[i].signo
#ifndef BKL_NEGCTL_SIGNAL_OWNER
            && g_faults[i].tid == tid
#endif
        ) {
            *out = g_faults[i];
            g_faults[i].signo = 0;
            __atomic_fetch_sub(&g_sig_armed, 1, __ATOMIC_RELAXED);
            return out->signo;
        }
    return 0;
}

/* --------------------------------------------------------------------------
 * Lifecycle. Exactly two call sites, both in proc.c, both at the moment the PCB
 * itself is claimed or released -- which is what keeps a table keyed by pid as
 * correct as a field in struct proc would have been.
 * ------------------------------------------------------------------------ */
static void reset_locked(struct sigst *s, int pid)
{
    s->pid = pid;
    for (int n = 0; n < KSIG_NSIG; n++) {
        s->handler[n] = 0; s->hmask[n] = 0; s->restorer[n] = 0; s->hflags[n] = 0;
    }
    if (s->pending) { s->pending = 0; if (g_sig_armed) __atomic_fetch_sub(&g_sig_armed, 1, __ATOMIC_RELAXED); }
    s->blocked = 0;
    s->stopped = 0;
    if (s->alarm_at && g_alarm_armed) __atomic_fetch_sub(&g_alarm_armed, 1, __ATOMIC_RELAXED);
    s->alarm_at = 0;
    s->suspend_mask = 0; s->in_suspend = 0;
    s->fault_cr2 = 0; s->fault_err = 0; s->fault_trapno = 0;
    ksig_recalc_ignored(s);
}

void ksig_proc_init(int pid)
{
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(pid);          /* a recycled pid: reuse the slot */
    if (!s) for (int i = 0; i < NPROC; i++) if (!g_sig[i].pid) { s = &g_sig[i]; break; }
    if (s) reset_locked(s, pid);
    spin_unlock_irqrestore(&g_sig_lock, f);
}

/* fork: POSIX. The child inherits the dispositions and the blocked mask, and
 * inherits NOTHING pending -- a signal raised at the parent was raised at the
 * parent. The alarm is not inherited either. */
void ksig_proc_fork(int child, int parent)
{
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *c = ksig_find_locked(child), *p = ksig_find_locked(parent);
    if (c && p) {
        for (int n = 0; n < KSIG_NSIG; n++) {
            c->handler[n] = p->handler[n]; c->hmask[n] = p->hmask[n];
            c->restorer[n] = p->restorer[n]; c->hflags[n] = p->hflags[n];
        }
        c->blocked = p->blocked;
        c->ignored = p->ignored;
    }
    spin_unlock_irqrestore(&g_sig_lock, f);
}

/* execve: POSIX again, and the distinction matters. A CAUGHT signal goes back
 * to SIG_DFL, because the handler's address is in an image that no longer
 * exists -- keeping it would be a jump into whatever the new program put there.
 * An IGNORED signal STAYS ignored, which is what lets a shell start a child
 * with SIGINT ignored. The blocked mask is inherited across exec unchanged. */
void ksig_proc_exec(int pid)
{
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(pid);
    if (s) {
        for (int n = 0; n < KSIG_NSIG; n++) {
            if (s->handler[n] != 1) s->handler[n] = 0;   /* caught -> default */
            s->hmask[n] = 0; s->restorer[n] = 0; s->hflags[n] = 0;
        }
        ksig_recalc_ignored(s);
    }
    spin_unlock_irqrestore(&g_sig_lock, f);
}

void ksig_proc_free(int pid)
{
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(pid);
    fault_clear_locked(pid);
    if (s) { reset_locked(s, 0); }
    spin_unlock_irqrestore(&g_sig_lock, f);
}

/* --------------------------------------------------------------------------
 * Raising.
 * ------------------------------------------------------------------------ */
int ksig_post(int pid, int signo)
{
    if (signo < 0 || signo >= KSIG_NSIG) return SIG_E_ARG;

    int wake_tid = -1;
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(pid);
    if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_SRCH; }
    if (signo == 0) { spin_unlock_irqrestore(&g_sig_lock, f); return 0; }  /* existence probe */

    /* SIGCONT and the stop signals are each other's negation, and the
     * cancellation happens AT POST rather than at delivery: a process sent
     * SIGSTOP then SIGCONT before either is delivered must end up running, and
     * if both merely sat in the pending set the order they came out in would
     * decide -- which is the wrong thing for that to depend on. */
    if (signo == LOGIT_SIGCONT) {
        ksig_clear_pending(s, SIGBIT(LOGIT_SIGSTOP) | SIGBIT(LOGIT_SIGTSTP) |
                              SIGBIT(LOGIT_SIGTTIN) | SIGBIT(LOGIT_SIGTTOU));
        s->stopped = 0;
    } else if (ksig_default_action(signo) == DFL_STOP) {
        ksig_clear_pending(s, SIGBIT(LOGIT_SIGCONT));
    }

    ksig_set_pending(s, SIGBIT(signo));
    __atomic_fetch_add(&g_sig_posted, 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_sig_lock, f);

    /* Wake the target so a PARKED thread returns from its wait and reaches a
     * return-to-ring-3 boundary, where ksig_deliver() runs. The wake is
     * spurious as far as the sleeper's own predicate goes, which is fine: every
     * sleep site in this kernel is a `while (!cond)` loop by rule, and the ones
     * that must report EINTR test ksig_interrupted() in that condition.
     *
     * ONLY THE MAIN THREAD, and this is the documented limit of the thread
     * story rather than an oversight: struct proc carries one tid, uthread.c
     * exposes no enumerator, and a process-directed signal is allowed to go to
     * one thread. A signal aimed at a process whose main thread is parked and
     * whose worker is spinning in ring 3 is still delivered -- the worker's next
     * timer interrupt is a return-to-ring-3 boundary too.
     * Correction: all adopted live threads are also woken now, so an exited
     * main thread cannot strand a signal behind sleeping workers. */
    {
        struct proc snap;
        if (proc_snapshot(pid, &snap)) wake_tid = snap.tid;
    }
    if (wake_tid >= 0) sched_wake_id(wake_tid);
    uthread_wake_process(pid);
    return 0;
}

int ksig_post_current(int signo)
{
    struct proc *p = proc_current();
    return p ? ksig_post(p->pid, signo) : SIG_E_SRCH;
}

/* Historically this was an unlocked volatile hint. Once syscalls run in
 * parallel the predicate must sample one consistent disposition/mask/pending
 * state. Signal publication never wakes while holding g_sig_lock, so a wait
 * queue may take this short lock without reversing the wake order. */
int ksig_interrupted(void)
{
    if (uthread_exit_pending()) return 1;
    if (!__atomic_load_n(&g_sig_armed, __ATOMIC_RELAXED)) return 0;
    struct proc *p = proc_current();
    if (!p) return 0;
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    int yes = s && ((s->pending & (~s->blocked | SIG_UNMASKABLE) & ~s->ignored) != 0);
    spin_unlock_irqrestore(&g_sig_lock, f);
    return yes;
}

/* --------------------------------------------------------------------------
 * The fault hook. Returns 1 if a handler will take it, so that
 * c/kernel/cpu/irq/interrupts.c can fall through to ksig_deliver() instead of
 * killing the process -- and 0 in every other case, which is what keeps the
 * desktop-survives-a-ring-3-fault property exactly as it was.
 * ------------------------------------------------------------------------ */
int ksig_fault(int signo, uint64_t cr2, uint64_t err, uint64_t vector)
{
    if (signo <= 0 || signo >= KSIG_NSIG) return 0;

    struct proc *p = proc_current();
    if (!p) return 0;

    int take = 0;
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (s) {
        uint64_t bit = SIGBIT(signo);
        /* A caught, unblocked fault signal is taken. Everything else falls
         * through to the historical kill:
         *   - no handler: the default action for SIGSEGV/SIGBUS/SIGFPE/SIGILL
         *     is terminate, which is what already happens.
         *   - SIG_IGN on a fault: POSIX leaves it undefined and every real
         *     kernel kills, because returning to the faulting instruction with
         *     the fault ignored re-faults forever.
         *   - BLOCKED: same infinite loop, same answer.
         * Getting this wrong in the permissive direction would not look like a
         * bug, it would look like the machine hanging. */
        if (s->handler[signo] > 1 && !(s->blocked & bit)) {
            /* Previously these three words and the pending bit belonged to
             * s: another CPU could consume both and build its own stack frame
             * with this thread's fault address. Bind the record to the tid. */
            int tid = sched_current_tid();
            struct ksig_fault_record *dst = NULL;
            for (unsigned i = 0; i < sizeof g_faults / sizeof g_faults[0]; i++)
                if (g_faults[i].signo && g_faults[i].tid == tid) { dst = &g_faults[i]; break; }
            if (!dst) for (unsigned i = 0; i < sizeof g_faults / sizeof g_faults[0]; i++)
                if (!g_faults[i].signo) { dst = &g_faults[i]; break; }
            if (dst) {
                if (!dst->signo) __atomic_fetch_add(&g_sig_armed, 1, __ATOMIC_RELAXED);
                *dst = (struct ksig_fault_record){tid, p->pid, signo, cr2, err, vector};
                take = 1;
            }
        }
    }
    spin_unlock_irqrestore(&g_sig_lock, f);
    if (take) __atomic_fetch_add(&g_sig_posted, 1, __ATOMIC_RELAXED);
    return take;
}

/* --------------------------------------------------------------------------
 * THE CONSOLE, and the honest shape of Ctrl+C on this machine.
 *
 * There is no serial receive interrupt here (c/kernel/cpu/irq/interrupts.c wires
 * IRQ 1 and IRQ 12 and nothing else), so the only thing that ever looked at the
 * UART was tty_read()'s own poll -- which means that while a child process ran,
 * NOBODY was reading, and a ^C simply sat in the receive register until the
 * child finished and the shell asked for the next line. A Ctrl+C that arrives
 * after the program you wanted to stop has exited is not a Ctrl+C.
 *
 * So the 100 Hz timer drains the UART instead. A ^C becomes a SIGINT to the
 * foreground pid; every other byte goes into this ring, and tty_read() takes it
 * from here rather than from the port. Two consumers of one register is a race
 * (historically both under the BKL; now serialized by g_tty_lock), and one consumer
 * with a queue is not -- so this is also the simpler arrangement, not only the
 * one that works.
 *
 * Latency is one tick, 10 ms.
 *
 * THE FOREGROUND PID is whichever process most recently blocked reading the
 * console. That is not job control and is not pretending to be: historically
 * there were no sessions or process groups here. Correction (2026-09-15): PTY
 * sessions/groups now exist, but this serial-console path deliberately does
 * not borrow their policy or claim to be a controlling terminal. It gets the common case right -- the
 * shell reads, so the shell is foreground; the shell then forks a child and
 * waits, and the SIGINT goes to the shell, which is exactly where a shell wants
 * it. What it does NOT do is deliver to the child, so `sleep 100` is not
 * interruptible by ^C until /bin/sh forwards it. Stated, not hidden.
 * ------------------------------------------------------------------------ */
#define TTYQ_SZ 1024
static char          g_ttyq[TTYQ_SZ];
static volatile int  g_ttyq_head, g_ttyq_tail;
static volatile int  g_tty_fg;
/* Nobody sleeps on this today -- tty_read() uses sched_poll_wait and
 * halts, which is a different mechanism and is left alone deliberately: it also
 * has to cover the between-ticks UART fallback, and rewriting it is not what
 * poll() needed. This queue exists for poll() registrations, and it is woken by
 * the ONE producer of bytes into the ring below. */
static struct waitq  g_tty_wq = WAITQ_INIT;
/* IRQ producer and concurrent console readers share both the ring and UART.
 * Post signals only after dropping this lock: signal wakeups may take other
 * wait queues, and no device lock is held across that chain. */
static spinlock_t g_tty_lock = SPINLOCK_INIT;

struct waitq *ksig_tty_waitq(void) { return &g_tty_wq; }
int           ksig_tty_avail(void) { return g_ttyq_tail != g_ttyq_head; }

void ksig_tty_set_fg(int pid) { __atomic_store_n(&g_tty_fg, pid, __ATOMIC_RELAXED); }

void ksig_tty_claim_fg(void)
{
    struct proc *p = proc_current();
    if (p) ksig_tty_set_fg(p->pid);
}

static int ttyq_full(void)
{
    return ((g_ttyq_head + 1) % TTYQ_SZ) == g_ttyq_tail;
}

static void ttyq_push(char c)
{
    if (ttyq_full()) return;
    g_ttyq[g_ttyq_head] = c;
    g_ttyq_head = (g_ttyq_head + 1) % TTYQ_SZ;
}

int ksig_tty_getc(void)
{
    uint64_t fl = spin_lock_irqsave(&g_tty_lock);
    int c;
    if (g_ttyq_tail != g_ttyq_head) {
        c = (unsigned char)g_ttyq[g_ttyq_tail];
        g_ttyq_tail = (g_ttyq_tail + 1) % TTYQ_SZ;
    } else c = serial_getc();
    int fg = g_tty_fg;
    spin_unlock_irqrestore(&g_tty_lock, fl);
    if (c == 3) { if (fg > 0) ksig_post(fg, LOGIT_SIGINT); return -1; }
    return c;
}

/* --------------------------------------------------------------------------
 * The periodic half: alarms and the console drain. Called from the timer IRQ.
 * ------------------------------------------------------------------------ */
void ksig_tick(void)
{
    /* Console first: it is unconditional (one UART status read) because the
     * whole point is to see a ^C when nothing else is looking.
     *
     * STOP AT A FULL QUEUE RATHER THAN DROPPING, and this is not a detail --
     * it is a bug this drain caused and the mm harness found. Before this
     * existed, unread input stayed in the UART, where the host side's own
     * buffering held it until the guest asked. Draining it here unconditionally
     * moves that backlog into a fixed ring, and a ring that fills starts
     * throwing away bytes that the host believes it delivered -- which is a
     * test harness typing a command that never arrives. Leaving the byte in the
     * device is the flow control; the queue is only ever a place to put what
     * has already been taken out. */
    int pushed = 0, interrupt_fg = 0;
    uint64_t tf = spin_lock_irqsave(&g_tty_lock);
    for (int i = 0; i < 16 && !ttyq_full(); i++) {   /* bounded: never spin in an IRQ */
        int c = serial_getc();
        if (c < 0) break;
        if (c == 3) {
            int fg = g_tty_fg;
            if (fg > 0) interrupt_fg = fg;
            continue;                            /* ^C is consumed, not echoed */
        }
        ttyq_push((char)c);
        pushed = 1;
    }
    /* Announce the bytes to anybody polling the console. The wake is AFTER the
     * pushes, never between them: a poller woken mid-drain would read the ring,
     * find one byte, and be told nothing more had arrived -- correct but
     * needlessly chatty. waitq_wake_all is interrupt-safe by design
     * (c/kernel/sync/wait.h rule 3), which is what makes this legal here. */
    spin_unlock_irqrestore(&g_tty_lock, tf);
    if (interrupt_fg) ksig_post(interrupt_fg, LOGIT_SIGINT);
    if (pushed) waitq_wake_all(&g_tty_wq);

    /* timerfd. Same argument for putting it here as for the console drain: this
     * is already the one function the 100 Hz tick calls that is allowed to do
     * bookkeeping, and file_timerfd_tick() returns after a single load on any
     * machine with no timerfd open -- which is every machine that does not use
     * one. It is NOT gated by g_alarm_armed below: an alarm(2) and a timerfd
     * are unrelated facilities, and hanging one off the other's guard is how a
     * feature silently stops working when the other is unused. */
    file_timerfd_tick();

    if (!g_alarm_armed) return;

    uint64_t now = timer_ms();
    int fire[NPROC], nf = 0;
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    for (int i = 0; i < NPROC; i++) {
        if (!g_sig[i].pid || !g_sig[i].alarm_at) continue;
        if (now < g_sig[i].alarm_at) continue;
        g_sig[i].alarm_at = 0;
        if (g_alarm_armed) __atomic_fetch_sub(&g_alarm_armed, 1, __ATOMIC_RELAXED);
        if (nf < NPROC) fire[nf++] = g_sig[i].pid;
    }
    spin_unlock_irqrestore(&g_sig_lock, f);

    /* Posting takes the same lock and wakes threads; do it outside. */
    for (int i = 0; i < nf; i++) ksig_post(fire[i], LOGIT_SIGALRM);
}

/* --------------------------------------------------------------------------
 * SYS_KILL's signal half. Lives here so the REFUSAL rule -- the protected
 * console process -- has one home, and so proc.c's proc_syscall keeps its
 * existing shape.
 * ------------------------------------------------------------------------ */
int ksig_kill(int pid, int signo)
{
    if (pid <= 0) return SIG_E_ARG;
    if (signo < 0 || signo >= KSIG_NSIG) return SIG_E_ARG;

    /* The SAME protected process the destroy path refuses (no parent and no
     * window = the shell wm_run spawned on the serial console; see the long
     * comment above proc_kill()), but the rule is NARROWER here, and the
     * narrowing is the point: init may be SIGNALLED, it may not be ENDED. A
     * console that cannot be sent SIGINT is a console with no Ctrl+C, which is
     * the whole reason this work exists -- so refusing every signal to it would
     * defeat the feature in the name of protecting it.
     *
     * What is refused is exactly the fatal outcome: a signal whose DEFAULT
     * action would terminate, sent to the protected process, which has not
     * installed a handler for it. With a handler installed the process has
     * asked for it and gets it; SIGINT with a handler is precisely the case
     * that must work. */
    if (signo > 0 && ksig_default_action(signo) == DFL_TERM) {
        struct proc snap;
        if (proc_snapshot(pid, &snap) && !snap.gui && snap.ppid == 0) {
            int caught = 0;
            uint64_t f = spin_lock_irqsave(&g_sig_lock);
            struct sigst *s = ksig_find_locked(pid);
            if (s && s->handler[signo] > 1) caught = 1;
            spin_unlock_irqrestore(&g_sig_lock, f);
            if (!caught) return SIG_E_PERM;
        }
    }
    return ksig_post(pid, signo);
}

/* --------------------------------------------------------------------------
 * The syscalls.
 * ------------------------------------------------------------------------ */
static long sys_sigaction(int signo, uint64_t uact, uint64_t uold)
{
    if (signo <= 0 || signo >= KSIG_NSIG) return SIG_E_ARG;
    if (SIGBIT(signo) & SIG_UNMASKABLE) return SIG_E_ARG;   /* uncatchable, and stays so */

    struct proc *p = proc_current();
    if (!p) return SIG_E_SRCH;

    struct logit_sigaction act, old;
    if (uact) {
        if (!user_range_ok((const void *)uact, sizeof act, 0)) return SIG_E_ARG;
        if (user_copy_from(&act, (const void *)uact, sizeof act) < 0) return SIG_E_ARG;
        /* A real handler MUST come with a restorer -- see the long note in
         * include/abi/logit_abi.h. SIG_DFL and SIG_IGN need none. */
        if (act.handler > 1 && !act.restorer) return SIG_E_ARG;
    }
    if (uold && !user_range_ok((void *)uold, sizeof old, 1)) return SIG_E_ARG;

    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_SRCH; }
    old.handler  = s->handler[signo];
    old.mask     = s->hmask[signo];
    old.flags    = s->hflags[signo];
    old.restorer = s->restorer[signo];
    if (uact) {
        s->handler[signo]  = act.handler;
        s->hmask[signo]    = act.mask & ~SIG_UNMASKABLE;
        s->hflags[signo]   = (uint32_t)act.flags;
        s->restorer[signo] = act.restorer;
        /* Installing SIG_IGN discards what is already pending, which is what
         * makes `signal(SIGPIPE, SIG_IGN)` at the top of main actually mean
         * "and I never want to hear about it". */
        if (act.handler == 1) ksig_clear_pending(s, SIGBIT(signo));
        ksig_recalc_ignored(s);
    }
    spin_unlock_irqrestore(&g_sig_lock, f);

    if (uold) user_copy_to((void *)uold, &old, sizeof old);
    return 0;
}

static long sys_sigprocmask(int how, uint64_t uset, uint64_t uold)
{
    struct proc *p = proc_current();
    if (!p) return SIG_E_SRCH;
    uint64_t set = 0, old = 0;
    if (uset) {
        if (!user_range_ok((const void *)uset, sizeof set, 0)) return SIG_E_ARG;
        if (user_copy_from(&set, (const void *)uset, sizeof set) < 0) return SIG_E_ARG;
    }
    if (uold && !user_range_ok((void *)uold, sizeof old, 1)) return SIG_E_ARG;

    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_SRCH; }
    old = s->blocked;
    if (uset) {
        /* SIGKILL and SIGSTOP are dropped from the set rather than refused --
         * Linux's behaviour, and the reason is practical: sigfillset() before a
         * critical section is idiomatic and correct code, and failing it would
         * break every program that does it for no gain. */
        uint64_t v = set & ~SIG_UNMASKABLE;
        switch (how) {
        case LOGIT_SIG_BLOCK:   s->blocked |= v; break;
        case LOGIT_SIG_UNBLOCK: s->blocked &= ~v; break;
        case LOGIT_SIG_SETMASK: s->blocked = v; break;
        default: spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_ARG;
        }
        /* Unblocking may have made something deliverable that has been sitting
         * in the pending set; the gate has to notice. It already does -- pending
         * did not change -- but the thread must reach a delivery point, and it
         * is about to: this is a syscall, and the return from it IS one. */
    }
    spin_unlock_irqrestore(&g_sig_lock, f);

    if (uold) user_copy_to((void *)uold, &old, sizeof old);
    return 0;
}

static long sys_sigpending(uint64_t uout)
{
    struct proc *p = proc_current();
    if (!p) return SIG_E_SRCH;
    if (!uout || !user_range_ok((void *)uout, sizeof(uint64_t), 1)) return SIG_E_ARG;
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    uint64_t v = s ? (s->pending & s->blocked) : 0;
    spin_unlock_irqrestore(&g_sig_lock, f);
    user_copy_to((void *)uout, &v, sizeof v);
    return 0;
}

static long sys_alarm(long seconds)
{
    struct proc *p = proc_current();
    if (!p) return SIG_E_SRCH;
    if (seconds < 0) return SIG_E_ARG;

    uint64_t now = timer_ms();
    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_SRCH; }
    long remain = 0;
    if (s->alarm_at) {
        remain = (long)((s->alarm_at > now ? s->alarm_at - now : 0) + 999) / 1000;
        if (g_alarm_armed) __atomic_fetch_sub(&g_alarm_armed, 1, __ATOMIC_RELAXED);
        s->alarm_at = 0;
    }
    if (seconds > 0) {
        s->alarm_at = now + (uint64_t)seconds * 1000ull;
        if (!s->alarm_at) s->alarm_at = 1;      /* 0 means "no alarm"; never store it */
        __atomic_fetch_add(&g_alarm_armed, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&g_sig_lock, f);
    return remain;
}

/* sigsuspend: install `mask`, wait until something that is not in it is
 * delivered, restore. The only successful outcome is SIG_E_INTR, which is what
 * POSIX specifies.
 *
 * The wait is sched_poll_wait() -- drop the big lock, halt, re-acquire -- and not a
 * waitqueue, because the event being waited for is "a signal became
 * deliverable" and that is exactly what ksig_interrupted() answers. A queue
 * would need every raiser to know about it; the halt wakes on any interrupt,
 * and there is one 100 times a second. It is the same idiom tty_read() uses at
 * the console prompt, for the same reason.
 *
 * The mask is restored HERE rather than by the delivery path, so that the
 * handler runs with the suspend mask in force (POSIX) and the pre-suspend mask
 * comes back after it. That is why in_suspend exists: ksigframe.c consults it
 * when it decides what mask to save in the frame. */
static long sys_sigsuspend(uint64_t umask)
{
    struct proc *p = proc_current();
    if (!p) return SIG_E_SRCH;
    uint64_t mask = 0;
    if (umask) {
        if (!user_range_ok((const void *)umask, sizeof mask, 0)) return SIG_E_ARG;
        if (user_copy_from(&mask, (const void *)umask, sizeof mask) < 0) return SIG_E_ARG;
    }
    mask &= ~SIG_UNMASKABLE;

    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return SIG_E_SRCH; }
    s->suspend_mask = s->blocked;
    s->blocked = mask;
    s->in_suspend = 1;
    spin_unlock_irqrestore(&g_sig_lock, f);

    while (!ksig_interrupted())
        sched_poll_wait();

    /* The frame ksig_deliver() is about to push will carry suspend_mask as the
     * mask to restore -- see the in_suspend branch there -- so a handler that
     * returns lands back on the caller's original mask. Restore it here too for
     * the case where nothing is caught (a default action, or the process being
     * killed) and no frame is ever pushed. */
    f = spin_lock_irqsave(&g_sig_lock);
    s = ksig_find_locked(p->pid);
    if (s && s->in_suspend) { s->blocked = s->suspend_mask; s->in_suspend = 0; }
    spin_unlock_irqrestore(&g_sig_lock, f);
    return SIG_E_INTR;
}

static long sys_sigquery(long what)
{
    struct proc *p;
    switch (what) {
    case SIGQ_DELIVERED: return (long)__atomic_load_n(&g_sig_delivered, __ATOMIC_RELAXED);
    case SIGQ_RETURNED:  return (long)__atomic_load_n(&g_sig_returned, __ATOMIC_RELAXED);
    case SIGQ_POSTED:    return (long)__atomic_load_n(&g_sig_posted, __ATOMIC_RELAXED);
    case SIGQ_DEFAULTED: return (long)__atomic_load_n(&g_sig_defaulted, __ATOMIC_RELAXED);
    case SIGQ_DROPPED:   return (long)__atomic_load_n(&g_sig_dropped, __ATOMIC_RELAXED);
    case SIGQ_FPUSAVED:  return (long)__atomic_load_n(&g_sig_fpusaved, __ATOMIC_RELAXED);
    case SIGQ_PENDING:
    case SIGQ_BLOCKED: {
        p = proc_current();
        if (!p) return -1;
        uint64_t f = spin_lock_irqsave(&g_sig_lock);
        struct sigst *s = ksig_find_locked(p->pid);
        long v = s ? (long)(what == SIGQ_PENDING ? s->pending : s->blocked) : -1;
        spin_unlock_irqrestore(&g_sig_lock, f);
        return v;
    }
    default: return -1;
    }
}

long ksig_syscall(long num, long a, long b, long c)
{
    switch (num) {
    case SYS_SIGACTION:   return sys_sigaction((int)a, (uint64_t)b, (uint64_t)c);
    case SYS_SIGPROCMASK: return sys_sigprocmask((int)a, (uint64_t)b, (uint64_t)c);
    case SYS_SIGRETURN:
        /* NOT handled here, and it cannot be: restoring a frame means writing
         * the register set that is about to be iretq'd and the FXSAVE area
         * c/boot/isr.asm will FXRSTOR, and neither is reachable from a syscall
         * body -- only interrupt_handler() holds both. So SYS_SIGRETURN is
         * intercepted there, ahead of the dispatcher, and reaching this line
         * means it was called from somewhere that is not a signal return. */
        return SIG_E_ARG;
    case SYS_SIGPENDING:  return sys_sigpending((uint64_t)a);
    case SYS_ALARM:       return sys_alarm(a);
    case SYS_SIGSUSPEND:  return sys_sigsuspend((uint64_t)a);
    case SYS_SIGQUERY:    return sys_sigquery(a);
    default:              return SIG_E_ARG;
    }
}
