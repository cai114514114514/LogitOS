/* ptrace: see c/kernel/exec/ptrace.h for what is here, what is deliberately
 * not, and what the permission rules are actually worth on this machine.
 *
 * The whole file is a table keyed by the TRACEE's pid, plus two hooks inside a
 * stop that already existed. Nothing here schedules, blocks a thread, or
 * touches the signal state machine beyond posting SIGSTOP/SIGCONT through the
 * ordinary ksig_post() -- which is the reason the feature is small enough to
 * be trusted. */

#include <stdint.h>
#include "ptrace.h"
#include "interrupts.h"
#include "proc.h"
#include "sched.h"
#include "ksignal.h"
#include "usercopy.h"
#include "vmm.h"
#include "spinlock.h"
#include "kprintf.h"
#include "vfs_cred.h"
#include "vfs_meta.h"
#include "logit_abi.h"
#include "coredump.h"    /* CORE_R15..CORE_GS: ONE register order, not two */

/* One link per traced process. NPROC of them, because a tracee is a process
 * and there cannot be more traced processes than processes. Static, like every
 * other per-pid table in this directory (g_killmark in proc.c, the signal
 * table in ksignal.c) and for the same reason: it is read from the stop path,
 * where an allocator call would put kheap_lock under a thread that is parking. */
struct link {
    int used;
    int tracee, tracer;
    int stopped;                 /* the tracee is parked and `regs` is valid   */
    int setregs;                 /* a SETREGS is waiting to be applied         */
    struct registers regs;
};
static struct link g_link[NPROC];
/* LOCK ORDER: g_pt_lock -> g_sig_lock, one direction only, and it is checked
 * rather than intended:
 *   - ptrace_proc_free() holds this and calls ksig_post(), which takes
 *     ksignal.c's g_sig_lock. That is the ONLY nesting in this file.
 *   - the reverse cannot happen. Both hooks are called from ksig_deliver()'s
 *     stop, which RELEASES g_sig_lock before it reaches them (ksigframe.c
 *     unlocks, then tests `stopped`), so a thread taking g_pt_lock there holds
 *     nothing else.
 *   - proc_exit() releases g_proc_lock before calling ptrace_proc_free(), so
 *     that one is not underneath either.
 * irqsave, because ptrace_note_stop() runs on the return-to-ring-3 path with
 * the timer live. */
static spinlock_t  g_pt_lock = SPINLOCK_INIT;
/* The gate. Zero on a machine where nothing is traced, which is every machine
 * almost all of the time, so the two hooks in the stop path cost one relaxed
 * load and a not-taken branch -- the discipline ksig_armed() and
 * proc_kill_armed() already set in this directory. */
static volatile unsigned long g_links_live;

static struct link *find_locked(int tracee)
{
    for (int i = 0; i < NPROC; i++)
        if (g_link[i].used && g_link[i].tracee == tracee) return &g_link[i];
    return 0;
}

/* --------------------------------------------------------------- the hooks */
void ptrace_note_stop(int pid, const struct registers *r)
{
    if (!g_links_live) return;
    uint64_t f = spin_lock_irqsave(&g_pt_lock);
    struct link *l = find_locked(pid);
    if (l) { l->regs = *r; l->stopped = 1; l->setregs = 0; }
    spin_unlock_irqrestore(&g_pt_lock, f);
}

void ptrace_note_resume(int pid, struct registers *r)
{
    if (!g_links_live) return;
    uint64_t f = spin_lock_irqsave(&g_pt_lock);
    struct link *l = find_locked(pid);
    if (l) {
        if (l->setregs) {
            /* WHAT SETREGS IS ALLOWED TO CHANGE, and this is the security
             * boundary of the whole feature. rip/rsp/rbp/rflags and the
             * general registers are the tracee's business. cs and ss ARE NOT:
             * writing a ring-0 selector into cs and returning through iretq is
             * a privilege escalation, and it would be one written by the
             * process being debugged as easily as by the debugger. They are
             * restored from the frame that was saved, not taken from the
             * tracer. RFLAGS keeps its own restriction below for the same
             * reason -- IOPL and the interrupt flag are not the tracee's to
             * set. */
            uint64_t cs = r->cs, ss = r->ss, oldfl = r->rflags;
            *r = l->regs;
            r->cs = cs; r->ss = ss;
            /* Only the arithmetic/direction flags, ID and the four the ABI
             * lets user code touch. Everything else -- IF, IOPL, NT, TF, RF,
             * VM, AC -- comes from the frame that was interrupted. TF is in
             * that list because single-step is NOT implemented (see the
             * header): letting a tracer set it would arm a debug exception
             * this kernel turns into a SIGTRAP with nobody to receive it. */
            const uint64_t USER_FLAGS = 0x0000000000000CD5ull;  /* CF PF AF ZF SF DF OF */
            r->rflags = (oldfl & ~USER_FLAGS) | (l->regs.rflags & USER_FLAGS);
            l->setregs = 0;
        }
        l->stopped = 0;
    }
    spin_unlock_irqrestore(&g_pt_lock, f);
}

void ptrace_proc_free(int pid)
{
    if (!g_links_live) return;
    uint64_t f = spin_lock_irqsave(&g_pt_lock);
    for (int i = 0; i < NPROC; i++) {
        if (!g_link[i].used) continue;
        /* Either end. A tracer that exits must not leave its tracee traced by
         * a pid that can be recycled -- that is how a later, unrelated process
         * would inherit the right to read somebody's memory. */
        if (g_link[i].tracee == pid || g_link[i].tracer == pid) {
            int tracee = g_link[i].tracee;
            g_link[i].used = 0;
            if (g_links_live) g_links_live--;
            /* If the TRACER died while the tracee was stopped, the tracee
             * would sit in the stop loop forever with nobody to continue it.
             * Post SIGCONT on the way out. */
            if (tracee != pid) ksig_post(tracee, LOGIT_SIGCONT);
        }
    }
    spin_unlock_irqrestore(&g_pt_lock, f);
}

/* ------------------------------------------------------- address translation
 * Reading another process's memory means reading a page that is not in the
 * CURRENT address space, so it cannot be done with a pointer: the tracee's
 * page table is walked by hand and the frame is reached through the kernel's
 * identity map of low physical memory, which is what c/kernel/mm/swap.c
 * already writes pages out through. That is sound here because this machine
 * boots with the first 1 GiB identity-mapped (c/boot/boot.asm) and has 512 MiB
 * of RAM, so every frame is addressable. If either ever stops being true this
 * function is the thing that breaks, which is why it says so.
 *
 * PRESENT ONLY, never faulted in. A tracer asking about an address the tracee
 * has not touched gets PT_E_FAULT rather than causing the page to be
 * materialised -- reading a program must not change it, and demand-faulting on
 * behalf of a stopped process from a third process's syscall would do exactly
 * that. */
static int xlate(uint64_t cr3, uint64_t va, int write, volatile uint64_t **out)
{
    if (va & 7) return PT_E_ALIGN;
    uint64_t *pte = vmm_pte(cr3, va & ~0xfffull);
    if (!pte) return PT_E_FAULT;
    uint64_t e = *pte;
    if (!(e & 1)) return PT_E_FAULT;             /* absent, or a swap entry    */
    if (!(e & 4)) return PT_E_FAULT;             /* not the process's own page */
    /* A copy-on-write page is mapped READ-ONLY and is shared with somebody
     * else, so this test refuses it without needing to know about COW at all:
     * writing through it would silently change a page another process owns.
     * The tracer is told PT_E_FAULT rather than having the copy forced,
     * because forcing it is a decision about the tracee's memory that belongs
     * to the tracee's own fault path. */
    if (write && !(e & 2)) return PT_E_FAULT;
    uint64_t phys = (e & 0x000ffffffffff000ull) | (va & 0xfff);
    *out = (volatile uint64_t *)(uintptr_t)phys;
    return PT_OK;
}

/* ------------------------------------------------------------ permission */
/* May `me` become the tracer of `target`? Root, or the same uid. See the
 * header for what this is honestly worth on a machine where nothing has
 * dropped privilege. */
static int may_attach(int me, int target)
{
#ifdef PTRACE_NO_OWNER_CHECK
    /* NEGATIVE CONTROL (`make test-ptrace-negctl`): the ownership rules
     * removed, which is the shape a first implementation has -- a pid is
     * treated as a capability, so any process may read any other's registers
     * and memory. Everything else still works, so nothing about the mechanism
     * changes; only the refusals stop happening. */
    (void)me; (void)target; return 1;
#else
    struct vcred a, b;
    if (vfs_cred_get(me, &a) != 0) return 0;
    if (vfs_cred_get(target, &b) != 0) return 0;
    if (a.uid == 0) return 1;
    return a.uid == b.uid;
#endif
}

/* Is the caller the tracer of `pid`, and is it stopped? Returns the link, or
 * fills *err. */
static struct link *claim(int pid, int need_stopped, int *err)
{
    struct proc *me = proc_current();
    if (!me) { *err = PT_E_PERM; return 0; }
    struct link *l = find_locked(pid);
    if (!l) { *err = PT_E_PERM; return 0; }
#ifndef PTRACE_NO_OWNER_CHECK
    if (l->tracer != me->pid) { *err = PT_E_PERM; return 0; }
#endif
    if (need_stopped && !l->stopped) { *err = PT_E_NOTSTOP; return 0; }
    *err = PT_OK;
    return l;
}

/* ---------------------------------------------------------- the register map
 * ONE order for the register file, shared with the core dump. Written out
 * rather than memcpy'd for the reason coredump.c gives about the same mapping:
 * struct registers is the trap-frame order and this is user_regs_struct's, the
 * two differ, and the difference is invisible in a hex dump. */
static void regs_out(const struct registers *r, uint64_t *g)
{
    g[CORE_R15] = r->r15; g[CORE_R14] = r->r14; g[CORE_R13] = r->r13;
    g[CORE_R12] = r->r12; g[CORE_RBP] = r->rbp; g[CORE_RBX] = r->rbx;
    g[CORE_R11] = r->r11; g[CORE_R10] = r->r10; g[CORE_R9]  = r->r9;
    g[CORE_R8]  = r->r8;  g[CORE_RAX] = r->rax; g[CORE_RCX] = r->rcx;
    g[CORE_RDX] = r->rdx; g[CORE_RSI] = r->rsi; g[CORE_RDI] = r->rdi;
    g[CORE_ORIG_RAX] = (uint64_t)-1;
    g[CORE_RIP] = r->rip; g[CORE_CS] = r->cs; g[CORE_EFLAGS] = r->rflags;
    g[CORE_RSP] = r->rsp; g[CORE_SS] = r->ss;
    g[CORE_FS_BASE] = 0; g[CORE_GS_BASE] = 0;
    g[CORE_DS] = 0; g[CORE_ES] = 0; g[CORE_FS] = 0; g[CORE_GS] = 0;
}
static void regs_in(struct registers *r, const uint64_t *g)
{
    r->r15 = g[CORE_R15]; r->r14 = g[CORE_R14]; r->r13 = g[CORE_R13];
    r->r12 = g[CORE_R12]; r->rbp = g[CORE_RBP]; r->rbx = g[CORE_RBX];
    r->r11 = g[CORE_R11]; r->r10 = g[CORE_R10]; r->r9  = g[CORE_R9];
    r->r8  = g[CORE_R8];  r->rax = g[CORE_RAX]; r->rcx = g[CORE_RCX];
    r->rdx = g[CORE_RDX]; r->rsi = g[CORE_RSI]; r->rdi = g[CORE_RDI];
    r->rip = g[CORE_RIP]; r->rsp = g[CORE_RSP];
    r->rflags = g[CORE_EFLAGS];
    /* cs and ss are NOT taken from the tracer -- see ptrace_note_resume. */
}

/* ----------------------------------------------------------------- ATTACH */
/* How long ATTACH waits for the tracee to reach its stop. The timer runs at
 * 100 Hz and ksig_deliver() is reached at EVERY kernel exit, so one tick is
 * the worst case for a ring-3 loop that makes no syscall at all; 200 gives two
 * seconds, which is a wide margin over a machine under TCG. It is a timeout
 * and not a wait forever because the tracee may be blocked in a driver poll
 * that does not return to ring 3, and a tracer wedged inside a syscall is
 * worse than a tracer told no. */
#define ATTACH_TICKS 200

static long do_attach(int pid)
{
    struct proc *me = proc_current();
    if (!me) return PT_E_PERM;
    if (pid == me->pid) return PT_E_PERM;        /* no self-tracing            */
    if (pid <= 1) return PT_E_PERM;              /* not init                   */
    if (!proc_by_pid(pid)) return PT_E_SRCH;
    if (!may_attach(me->pid, pid)) return PT_E_PERM;

    uint64_t f = spin_lock_irqsave(&g_pt_lock);
    if (find_locked(pid)) { spin_unlock_irqrestore(&g_pt_lock, f); return PT_E_BUSY; }
    struct link *l = 0;
    for (int i = 0; i < NPROC; i++) if (!g_link[i].used) { l = &g_link[i]; break; }
    if (!l) { spin_unlock_irqrestore(&g_pt_lock, f); return PT_E_NOSPACE; }
    l->used = 1; l->tracee = pid; l->tracer = me->pid;
    l->stopped = 0; l->setregs = 0;
    g_links_live++;
    spin_unlock_irqrestore(&g_pt_lock, f);

    /* Through the ordinary path. SIGSTOP is unmaskable (ksignal.c's
     * SIG_UNMASKABLE), so a tracee cannot decline to be stopped by blocking or
     * ignoring it -- which is what makes ATTACH work on an uncooperative
     * program rather than only on one that agreed. */
    ksig_post(pid, LOGIT_SIGSTOP);

    for (int i = 0; i < ATTACH_TICKS; i++) {
        uint64_t f2 = spin_lock_irqsave(&g_pt_lock);
        struct link *l2 = find_locked(pid);
        int done = l2 && l2->stopped;
        int gone = !l2;
        spin_unlock_irqrestore(&g_pt_lock, f2);
        if (gone) return PT_E_SRCH;              /* it exited while we waited  */
        if (done) return PT_OK;
        if (!proc_by_pid(pid)) { ptrace_proc_free(pid); return PT_E_SRCH; }
        /* Drops the BKL, so waiting here does not stop the machine -- the same
         * idiom the stop loop itself and tty_read() use. */
        bkl_hlt_wait();
    }
    /* Did not stop. The link is torn down rather than left half-made, and the
     * tracee is continued -- leaving it SIGSTOPped with no tracer would be a
     * process this call quietly froze. */
    ksig_post(pid, LOGIT_SIGCONT);
    ptrace_proc_free(pid);
    kprintf("[ptrace] pid %d: %d did not reach a stop in %d ticks -- detached\n",
            me->pid, pid, ATTACH_TICKS);
    return PT_E_TIMEOUT;
}

/* -------------------------------------------------------------- the syscall */
long ptrace_syscall(long req, long pidl, long arg)
{
    int pid = (int)pidl;

    if (req == PTRACE_ATTACH) return do_attach(pid);

    struct proc *me = proc_current();
    if (!me) return PT_E_PERM;

    if (req == PTRACE_DETACH) {
        int err = PT_OK;
        uint64_t f = spin_lock_irqsave(&g_pt_lock);
        struct link *l = claim(pid, 0, &err);
        int was = l ? 1 : 0;
        if (l) { l->used = 0; if (g_links_live) g_links_live--; }
        spin_unlock_irqrestore(&g_pt_lock, f);
        if (!was) return err;
        ksig_post(pid, LOGIT_SIGCONT);           /* never leave it frozen */
        return PT_OK;
    }

    if (req == PTRACE_CONT) {
        int err = PT_OK;
        uint64_t f = spin_lock_irqsave(&g_pt_lock);
        struct link *l = claim(pid, 1, &err);
        spin_unlock_irqrestore(&g_pt_lock, f);
        if (!l) return err;
        /* The link stays: CONT resumes, DETACH is what gives it up. `stopped`
         * is cleared by ptrace_note_resume() at the tracee's own hand, not
         * here -- clearing it here would let a GETREGS between this call and
         * the tracee actually waking read a frame that is about to change. */
        ksig_post(pid, LOGIT_SIGCONT);
        return PT_OK;
    }

    if (req == PTRACE_GETREGS || req == PTRACE_SETREGS) {
        uint64_t g[PTRACE_NGREG];
        if (req == PTRACE_SETREGS &&
            user_copy_from(g, (const void *)(uintptr_t)arg, sizeof g) != 0)
            return PT_E_ARG;
        int err = PT_OK;
        uint64_t f = spin_lock_irqsave(&g_pt_lock);
        struct link *l = claim(pid, 1, &err);
        if (l) {
            if (req == PTRACE_GETREGS) regs_out(&l->regs, g);
            else { regs_in(&l->regs, g); l->setregs = 1; }
        }
        spin_unlock_irqrestore(&g_pt_lock, f);
        if (!l) return err;
        if (req == PTRACE_GETREGS &&
            user_copy_to((void *)(uintptr_t)arg, g, sizeof g) != 0)
            return PT_E_ARG;
        return PT_OK;
    }

    if (req == PTRACE_PEEKDATA || req == PTRACE_POKEDATA) {
        struct logit_ptrace_word w;
        if (user_copy_from(&w, (const void *)(uintptr_t)arg, sizeof w) != 0)
            return PT_E_ARG;
        int err = PT_OK;
        uint64_t f = spin_lock_irqsave(&g_pt_lock);
        struct link *l = claim(pid, 1, &err);
        spin_unlock_irqrestore(&g_pt_lock, f);
        if (!l) return err;
        /* The tracee is STOPPED, so its address space cannot be torn down
         * under this call: proc_exit() is reached from ring 3 and it is not in
         * ring 3. proc_by_pid() is still re-read, because "stopped" is our
         * bookkeeping and the PCB is the authority. */
        struct proc *t = proc_by_pid(pid);
        if (!t) return PT_E_SRCH;
        volatile uint64_t *p = 0;
        int rc = xlate(t->cr3, w.addr, req == PTRACE_POKEDATA, &p);
        if (rc != PT_OK) return rc;
        if (req == PTRACE_PEEKDATA) {
            w.data = *p;
            if (user_copy_to((void *)(uintptr_t)arg, &w, sizeof w) != 0) return PT_E_ARG;
        } else {
            *p = w.data;
        }
        return PT_OK;
    }

    return PT_E_ARG;
}
