/* ===========================================================================
 * M31 -- THE FRAME. The second entry into user code, and the way back.
 *
 * This is the file that makes signals something other than a bitmask. Two
 * functions, and between them they are the whole mechanism:
 *
 *   ksig_deliver()    at a return-to-ring-3 boundary, pick a pending unblocked
 *                     signal, build a frame on the USER stack holding the
 *                     interrupted machine state, and rewrite the frame that is
 *                     about to be iretq'd so it enters the handler instead.
 *   ksig_sigreturn()  put every bit of that back.
 *
 * WHAT THE FRAME MUST CARRY, and the one that is easy to leave out.
 *
 * The general registers and rip/rflags/rsp are obvious -- a handler is ordinary
 * C and will clobber all of them. The blocked mask is nearly as obvious. THE
 * FPU/SSE STATE IS NOT, and leaving it out produces a kernel where every test
 * anyone would think to write still passes.
 *
 * c/boot/isr.asm FXSAVEs on every kernel entry and FXRSTORs before every iretq,
 * and its comment says why: the kernel is built with -msse2, so C code in the
 * kernel may clobber XMM0-15 and MXCSR of whatever it interrupted. A signal
 * handler is ring-3 C compiled the same way. So if the frame does not carry the
 * 512 bytes, a handler that so much as adds two doubles silently overwrites the
 * XMM registers of the computation it interrupted -- and the computation
 * continues, and produces a wrong number, and nothing anywhere reports an
 * error. `kill -TERM` works. The shell works. A handler that sets a flag works.
 * A program doing arithmetic gets the wrong answer, sometimes.
 *
 * That is why the negative control for this whole line is not "remove signals"
 * but -DSIGNAL_NO_FPU: deliver correctly, and leave the FXSAVE area out. The
 * suite has to catch it. SIGQ_FPUSAVED exists so the test can also see, from
 * ring 3, that the kernel it is running on is the one that saves.
 *
 * WHERE THE 512 BYTES COME FROM. Not from an FXSAVE issued here -- by the time
 * this runs, kernel C code has already had the XMM registers. They come from
 * the area isr_common filled on entry, which is the interrupted program's own
 * state, handed down as `fxarea`. Symmetrically, sigreturn writes back INTO
 * that area rather than issuing FXRSTOR, so isr_common's own FXRSTOR on the way
 * out is what installs it. One save and one restore, in the place that already
 * does both.
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include "ksignal.h"
#include "ksig_int.h"
#include "interrupts.h"
#include "proc.h"
#include "sched.h"
#include "kprintf.h"
#include "usercopy.h"
#include "logit_abi.h"

#define SIGBIT(n) (1ull << (n))
#define SIG_UNMASKABLE (SIGBIT(LOGIT_SIGKILL) | SIGBIT(LOGIT_SIGSTOP))

#define FXAREA_BYTES 512

/* Which RFLAGS bits ring 3 is allowed to bring back through SYS_SIGRETURN:
 * CF PF AF ZF SF DF OF -- the arithmetic flags and the direction flag, and
 * nothing else. Everything a program has any business restoring is in there.
 *
 * What is deliberately NOT: IOPL (bits 12-13, port access), IF (bit 9, which is
 * forced ON below because returning to ring 3 with interrupts disabled hangs
 * the core), TF (bit 8, single-step), NT, RF, VM, AC, and the ID bit. The
 * sigctx is on a user stack the process can write, so this mask is the only
 * thing between "a program corrupted its own signal frame" and "a program
 * granted itself I/O privilege". */
#define RFLAGS_USER_MASK 0x0CD5ull
#define RFLAGS_ALWAYS    0x0202ull   /* IF + the reserved bit 1 */

/* MXCSR has reserved bits, and FXRSTOR with any of them set is a #GP -- in the
 * kernel, on the way out of an interrupt, which is a panic and not a signal.
 * The frame came from a user stack, so it is untrusted; mask it to the bits
 * every x86-64 defines. Offset 24 into the FXSAVE area is fixed by the ISA. */
#define MXCSR_OFF  24
#define MXCSR_MASK 0x0000FFBFu

static int lowest_bit(uint64_t m)
{
    for (int i = 1; i < KSIG_NSIG; i++) if (m & SIGBIT(i)) return i;
    return 0;
}

/* --------------------------------------------------------------------------
 * Building the frame.
 *
 * LAYOUT, from the top of the user stack down, and the alignment is ABI rather
 * than taste:
 *
 *     old rsp
 *     ...128 bytes...                  the SysV red zone. Skipped, not reused:
 *                                      a leaf function below the interrupted rip
 *                                      may have live data there.
 *     [FXAREA_BYTES]                   16-aligned (FXSAVE/FXRSTOR require it)
 *     [struct logit_sigctx]            16-aligned, size a multiple of 16
 *     [restorer]                       8 bytes -- the handler's `ret` target
 *     new rsp -------------------------^
 *
 * new rsp % 16 == 8, which is exactly what the SysV ABI guarantees a function
 * on entry (rsp was 16-aligned at the `call`, which then pushed 8). Getting it
 * wrong is not subtle in its consequences and is very subtle in its cause: the
 * handler's own `movaps` to a stack local #GPs.
 * ------------------------------------------------------------------------ */
struct frame_plan {
    uint64_t rsp;        /* the handler's rsp: where the restorer address goes */
    uint64_t ctx;        /* struct logit_sigctx */
    uint64_t fp;         /* FXSAVE area */
    uint64_t low, len;   /* the whole span, for one user_range_ok */
};

static int plan_frame(uint64_t user_rsp, struct frame_plan *fp)
{
    uint64_t top = user_rsp - 128;                 /* red zone */
    top &= ~(uint64_t)15;
    if (top > user_rsp) return 0;                  /* wrapped: a nonsense rsp */

    uint64_t fx  = (top - FXAREA_BYTES) & ~(uint64_t)15;
    if (fx > top) return 0;
    uint64_t ctx = (fx - sizeof(struct logit_sigctx)) & ~(uint64_t)15;
    if (ctx > fx) return 0;
    uint64_t rsp = ctx - 8;
    if (rsp > ctx) return 0;

    fp->rsp = rsp; fp->ctx = ctx; fp->fp = fx;
    fp->low = rsp; fp->len = top - rsp;
    return 1;
}

/* Everything the disposition says, snapshotted under the lock so the frame can
 * be written with the lock DROPPED. Writing user memory here can demand-fault
 * (usercopy resolves copy-on-write and anonymous pages before it returns), and
 * taking a page fault with an irqsave spinlock held is the thing proc_list()
 * drops g_proc_lock to avoid. Same rule, same reason. */
struct disp {
    uint64_t handler, mask, restorer, oldmask;
    uint32_t flags;
    uint64_t cr2, err, trapno;
};

static void fill_ctx(struct logit_sigctx *c, const struct registers *r,
                     int signo, const struct disp *d, uint64_t fpaddr)
{
    c->r15 = r->r15; c->r14 = r->r14; c->r13 = r->r13; c->r12 = r->r12;
    c->r11 = r->r11; c->r10 = r->r10; c->r9  = r->r9;  c->r8  = r->r8;
    c->rbp = r->rbp; c->rdi = r->rdi; c->rsi = r->rsi; c->rdx = r->rdx;
    c->rcx = r->rcx; c->rbx = r->rbx; c->rax = r->rax;
    c->rip = r->rip; c->rflags = r->rflags; c->rsp = r->rsp;
    c->oldmask = d->oldmask;
    c->signo = (uint64_t)signo;
    c->err = d->err; c->trapno = d->trapno; c->cr2 = d->cr2;
    c->fpstate = fpaddr;
}

/* Push. Returns 1 on success (r has been rewritten to enter the handler), 0 if
 * the user stack could not hold a frame -- in which case the caller kills the
 * process, because there is nowhere to report the failure to. */
static int push_frame(struct registers *r, void *fxarea, int signo,
                      const struct disp *d, uint64_t sysnr)
{
    struct frame_plan fp;
    if (!plan_frame(r->rsp, &fp)) return 0;
    if (!user_range_ok((void *)fp.low, fp.len, 1)) return 0;

    struct logit_sigctx c;
    uint64_t fpaddr = 0;
#ifndef SIGNAL_NO_FPU
    /* THE NEGATIVE CONTROL'S SEAM. With SIGNAL_NO_FPU the frame is built
     * without this and fpstate stays 0; sigreturn then restores nothing, the
     * handler's XMM state survives into the interrupted computation, and every
     * test that does not do floating-point arithmetic across a signal still
     * passes. See tests/unit/signal_test.c. */
    if (user_copy_to((void *)fp.fp, fxarea, FXAREA_BYTES) < 0) return 0;
    fpaddr = fp.fp;
    g_sig_fpusaved++;
#else
    (void)fxarea;
#endif

    fill_ctx(&c, r, signo, d, fpaddr);

    /* SA_RESTART, applied to the SAVED context rather than to the live frame,
     * which is the only place it can be applied: the syscall has already run
     * and returned SIG_E_INTR, and "restart" means the state sigreturn puts
     * back must re-execute it. So rewind the saved rip over the two-byte
     * `int $0x80` and put the syscall NUMBER back in the saved rax (which
     * currently holds the result). The argument registers were never touched.
     *
     * Only when the call actually reported an interruption -- a syscall that
     * completed is not restarted, and one that failed for its own reasons keeps
     * its error. */
    if (sysnr && r->vector == 128 && (long)r->rax == SIG_E_INTR &&
        (d->flags & LOGIT_SA_RESTART)) {
        c.rax = sysnr;
        c.rip = r->rip - 2;          /* CD 80 */
    }

    if (user_copy_to((void *)fp.ctx, &c, sizeof c) < 0) return 0;
    if (user_copy_to((void *)fp.rsp, &d->restorer, sizeof d->restorer) < 0) return 0;

    /* Enter the handler. rdi = signo, rsi = 0 (there is no siginfo_t here),
     * rdx = the sigctx -- so a three-argument SA_SIGINFO handler can at least
     * read the machine state, which is the useful half. */
    r->rsp = fp.rsp;
    r->rip = d->handler;
    r->rdi = (uint64_t)signo;
    r->rsi = 0;
    r->rdx = fp.ctx;
    r->rax = 0;
    /* DF cleared: the SysV ABI says a function is entered with the direction
     * flag clear, and a handler entered with it set corrupts every string
     * operation the compiler emits. TF cleared so a signal cannot arrive
     * single-stepping. */
    r->rflags &= ~(0x400ull | 0x100ull);
    return 1;
}

/* --------------------------------------------------------------------------
 * Delivery.
 * ------------------------------------------------------------------------ */
void ksig_deliver(struct registers *r, void *fxarea, uint64_t sysnr)
{
    if (!ksig_armed()) return;
    struct proc *p = proc_current();
    if (!p) return;
    if (!(r->cs & 3)) return;          /* only ever on the way back to ring 3 */

    for (;;) {
        int signo = 0, action = 0, stopped = 0;
        struct disp d;

        uint64_t f = spin_lock_irqsave(&g_sig_lock);
        struct sigst *s = ksig_find_locked(p->pid);
        if (!s) { spin_unlock_irqrestore(&g_sig_lock, f); return; }
        if (s->stopped) { stopped = 1; }
        else {
            /* SIGKILL and SIGSTOP ignore the mask -- that is what "cannot be
             * blocked" means, and it is enforced here as well as at
             * sigprocmask because a mask can also be set by sa_mask. */
            uint64_t deliverable = s->pending & (~s->blocked | SIG_UNMASKABLE);
            signo = lowest_bit(deliverable);
            if (signo) {
                d.handler  = s->handler[signo];
                d.mask     = s->hmask[signo];
                d.restorer = s->restorer[signo];
                d.flags    = s->hflags[signo];
                /* THE MASK THE FRAME CARRIES. Normally the mask in force right
                 * now; inside sigsuspend, the one the caller had BEFORE it --
                 * so that the handler runs under the suspend mask (POSIX) and
                 * returning from it lands back on the original. */
                d.oldmask  = s->in_suspend ? s->suspend_mask : s->blocked;
                d.cr2 = s->fault_cr2; d.err = s->fault_err; d.trapno = s->fault_trapno;
                if (SIGBIT(signo) & SIG_UNMASKABLE) d.handler = 0;   /* uncatchable */
                action = (d.handler > 1) ? -1 : (d.handler == 1 ? DFL_IGN
                                                                : ksig_default_action(signo));
                ksig_clear_pending(s, SIGBIT(signo));
                if (action == -1) {
                    /* Commit the handler's mask NOW, under the same lock that
                     * chose it: a second signal arriving between here and the
                     * frame being written must already see the new mask, or
                     * sa_mask would not be a guarantee. */
                    s->blocked |= d.mask;
                    if (!(d.flags & LOGIT_SA_NODEFER)) s->blocked |= SIGBIT(signo);
                    s->blocked &= ~SIG_UNMASKABLE;
                    if (d.flags & LOGIT_SA_RESETHAND) {
                        s->handler[signo] = 0;
                        ksig_recalc_ignored(s);
                    }
                    s->in_suspend = 0;          /* the frame carries the mask now */
                } else if (action == DFL_STOP) {
                    s->stopped = 1; stopped = 1;
                } else if (action == DFL_CONT) {
                    s->stopped = 0;
                }
                if (s->in_suspend && action != -1) {
                    s->blocked = s->suspend_mask; s->in_suspend = 0;
                }
            }
        }
        spin_unlock_irqrestore(&g_sig_lock, f);

        if (stopped) {
            /* STOPPED. Not parked on a queue: the thing being waited for is
             * "somebody sent SIGCONT", which is a flag in the table above and
             * not an event any existing queue owns, and a halt wakes on any
             * interrupt -- of which there are a hundred a second. Same idiom
             * tty_read() uses at the console prompt, and for the same reason.
             *
             * bkl_hlt_wait() DROPS the big kernel lock, so a stopped process
             * does not stop the machine. What it does cost, said plainly: this
             * thread is still on the run ring and is dispatched once per tick
             * to test one flag. A stopped process is rare and this is ten
             * instructions of it. */
            for (;;) {
                uint64_t f2 = spin_lock_irqsave(&g_sig_lock);
                struct sigst *s2 = ksig_find_locked(p->pid);
                int still = s2 ? s2->stopped : 0;
                /* SIGKILL must reach a stopped process -- that is most of the
                 * point of SIGKILL. Anything unmaskable breaks the loop and is
                 * handled on the next pass round the outer one. */
                if (s2 && (s2->pending & SIG_UNMASKABLE)) still = 0;
                spin_unlock_irqrestore(&g_sig_lock, f2);
                if (!still) break;
                bkl_hlt_wait();
            }
            continue;
        }

        if (!signo) return;

        if (action == -1) {
            if (push_frame(r, fxarea, signo, &d, sysnr)) {
                g_sig_delivered++;
                return;                       /* iretq goes to the handler */
            }
            /* No usable user stack. There is nowhere to report this to and no
             * state to return to, so it is fatal -- which is also what Linux
             * does (a SIGSEGV that cannot be delivered kills). */
            g_sig_dropped++;
            kprintf("[signal] pid %d: no room for a sig %d frame at rsp=%p -- terminating\n",
                    p->pid, signo, (void *)r->rsp);
            proc_exit(128 + signo);           /* never returns */
        }

        g_sig_defaulted++;
        if (action == DFL_IGN || action == DFL_CONT) continue;   /* look for another */

        /* DFL_TERM. The historical exit code for a signalled process, and the
         * one /bin/sh already prints: 128 + the number. */
        kprintf("[signal] pid %d (%s): terminated by signal %d\n",
                p->pid, p->name, signo);
        proc_exit(128 + signo);               /* never returns */
    }
}

/* --------------------------------------------------------------------------
 * The way back.
 *
 * At the moment the restorer executes `int $0x80`, the handler's `ret` has
 * already popped the restorer address, so the user rsp saved in this entry's
 * frame points AT the sigctx. That is the whole address calculation: there is
 * no cookie and no per-process "a frame is outstanding" flag, because the frame
 * is self-describing and the restore is validated field by field below.
 * ------------------------------------------------------------------------ */
void ksig_sigreturn(struct registers *r, void *fxarea)
{
    struct proc *p = proc_current();
    struct logit_sigctx c;

    if (!p || !(r->cs & 3)) { r->rax = (uint64_t)(long)SIG_E_ARG; return; }
    if (!user_range_ok((const void *)r->rsp, sizeof c, 0) ||
        user_copy_from(&c, (const void *)r->rsp, sizeof c) < 0) {
        g_sig_dropped++;
        kprintf("[signal] pid %d: bad sigreturn frame at %p -- terminating\n",
                p->pid, (void *)r->rsp);
        proc_exit(128 + LOGIT_SIGSEGV);       /* never returns */
    }
    if (c.signo == 0 || c.signo >= KSIG_NSIG) {
        r->rax = (uint64_t)(long)SIG_E_ARG;   /* not a frame this kernel pushed */
        return;
    }

    /* The FPU/SSE state goes back into the area c/boot/isr.asm will FXRSTOR on
     * the way out -- so the one FXRSTOR that already exists installs it, rather
     * than this file issuing a second one that the first would then undo.
     *
     * MXCSR is masked because the frame lives on a stack the process can write,
     * and FXRSTOR with a reserved MXCSR bit set is a #GP taken in the kernel on
     * the iretq path, i.e. a panic. An untrusted 512 bytes is exactly the sort
     * of input that has to be sanitised at the boundary rather than trusted
     * because we wrote it a moment ago. */
    if (c.fpstate) {
        if (user_range_ok((const void *)c.fpstate, FXAREA_BYTES, 0) &&
            user_copy_from(fxarea, (const void *)c.fpstate, FXAREA_BYTES) >= 0) {
            uint32_t *mx = (uint32_t *)((uint8_t *)fxarea + MXCSR_OFF);
            *mx &= MXCSR_MASK;
        }
    }

    r->r15 = c.r15; r->r14 = c.r14; r->r13 = c.r13; r->r12 = c.r12;
    r->r11 = c.r11; r->r10 = c.r10; r->r9  = c.r9;  r->r8  = c.r8;
    r->rbp = c.rbp; r->rdi = c.rdi; r->rsi = c.rsi; r->rdx = c.rdx;
    r->rcx = c.rcx; r->rbx = c.rbx; r->rax = c.rax;
    r->rip = c.rip;
    r->rsp = c.rsp;
    r->rflags = (c.rflags & RFLAGS_USER_MASK) | RFLAGS_ALWAYS;
    /* cs and ss are NOT restored from the frame. They are still the ring-3
     * selectors this entry came in with, and taking them from user memory is
     * how a process would ask to be resumed in ring 0. */

    uint64_t f = spin_lock_irqsave(&g_sig_lock);
    struct sigst *s = ksig_find_locked(p->pid);
    if (s) {
        s->blocked = c.oldmask & ~SIG_UNMASKABLE;
        s->in_suspend = 0;
    }
    spin_unlock_irqrestore(&g_sig_lock, f);

    g_sig_returned++;
}
