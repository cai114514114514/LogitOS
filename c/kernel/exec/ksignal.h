#ifndef LOGIT_KSIGNAL_H
#define LOGIT_KSIGNAL_H

#include <stdint.h>

/* ===========================================================================
 * M31 -- signal delivery.
 *
 * THE NAME. This header is ksignal.h and not signal.h, and that is not a style
 * choice: mini-libc already ships c/apps/libc/include/signal.h, INCDIRS is one
 * flat sorted list, and c/apps/libc/include sorts before c/kernel/exec -- so a
 * kernel "signal.h" would be silently shadowed by the userland one and the
 * failure would surface in some other file entirely. CLAUDE.md records that
 * exact trap happening twice. This is the third time it was avoided instead.
 *
 * WHAT THIS IS. The kernel side of signals: a per-process disposition table, a
 * pending set, a blocked mask, and -- the part that is not a syscall -- the
 * code that builds a second entry into ring 3 on the user's own stack.
 *
 * WHERE DELIVERY HAPPENS, and why there is only one place. A signal frame can
 * only be pushed at a moment when the kernel is about to iretq into ring 3,
 * because that is the only moment at which a complete, consistent user register
 * state exists to save. That moment is the tail of interrupt_handler() in
 * c/kernel/cpu/interrupts.c, and it is the tail of EVERY kernel entry: syscall
 * return, timer tick, page fault, IRQ. So ksig_deliver() is called from exactly
 * one site and covers all of them -- including a ring-3 compute loop that never
 * makes a syscall, which the timer reaches within one tick. (That is strictly
 * better than the SYS_KILL mark this work sits beside, which by its own comment
 * cannot reach such a loop at all.)
 *
 * STATE IS KEYED BY PID, in a table here, rather than being fields in struct
 * proc. struct proc.h belongs to the process line; a parallel table keyed by
 * the pid is exactly as correct as long as it is created and destroyed at the
 * same two points the PCB is, which is what ksig_proc_init/_free are for and
 * why they are called from alloc_proc() and proc_exit() and nowhere else.
 * g_killmark[] in proc.c is the same pattern for the same reason.
 * =========================================================================== */

struct registers;   /* interrupts.h */

/* --- lifecycle: one call each from the two places a PCB is claimed/released. */
void ksig_proc_init(int pid);                 /* fresh process: everything default */
void ksig_proc_fork(int child, int parent);   /* handlers + mask inherited, pending NOT */
void ksig_proc_exec(int pid);                 /* execve: caught -> default, ignored stays */
void ksig_proc_free(int pid);

/* --- raising ------------------------------------------------------------- */
/* Post `signo` to `pid`. Returns 0, or a SIG_E_*. signo 0 is the POSIX
 * existence probe: it validates the target and posts nothing. Safe to call from
 * interrupt context (it wakes, it does not deliver). */
int  ksig_post(int pid, int signo);
/* Post to the calling process. Used by the in-kernel raisers (SIGPIPE). */
int  ksig_post_current(int signo);

/* --- the gate ------------------------------------------------------------ */
/* Non-zero if ANY process has an undelivered signal. One relaxed load; this is
 * what keeps the cost of this feature on a quiet machine to a not-taken branch,
 * the same discipline proc_kill_armed() and uthread_exit_armed() use. */
int  ksig_armed(void);

/* Does the CALLING process have a signal that would run a handler or kill it?
 * The EINTR predicate: a blocking wait consults it and returns SIG_E_INTR.
 * Deliberately LOCK-FREE -- it reads the pending/blocked words directly. It is
 * a hint, and a stale read costs one more pass round a wait loop, never a
 * missed signal; making it take a lock would mean taking one inside a
 * wait_event() condition, i.e. under a waitqueue lock, and inverting the lock
 * order this kernel is built on. */
int  ksig_interrupted(void);

/* --- delivery ------------------------------------------------------------ */
/* THE hook, called from the tail of interrupt_handler with the frame that is
 * about to be iretq'd and the 512-byte FXSAVE area c/boot/isr.asm filled on
 * entry. May rewrite `r` to enter a handler, may block (SIGSTOP), and may not
 * return at all (a default action that terminates).
 *
 * `sysnr` is the syscall number this entry carried (0 if it was not a syscall),
 * kept by the caller because r->rax holds the RESULT by the time we get here.
 * It is what makes SA_RESTART possible: restarting means putting the number
 * back and rewinding rip over the two-byte `int $0x80`. */
void ksig_deliver(struct registers *r, void *fxarea, uint64_t sysnr);

/* SYS_SIGRETURN. Intercepted in interrupt_handler AHEAD of the syscall
 * dispatcher, and it has to be: restoring a frame means writing the register
 * set that is about to be iretq'd and the FXSAVE area c/boot/isr.asm will
 * FXRSTOR, and a syscall body can reach neither. Rewrites `r` and `fxarea` in
 * place; on an unusable frame it terminates the process rather than returning
 * to an address the process cannot have meant. */
void ksig_sigreturn(struct registers *r, void *fxarea);

/* A ring-3 fault. Returns 1 if a handler will take it (the caller must then
 * fall through to ksig_deliver), 0 if the process should die as it always did.
 * A BLOCKED synchronous fault returns 0 too: POSIX says the behaviour is
 * undefined and every real kernel kills, because returning to the faulting
 * instruction with the signal blocked is an infinite loop. */
int  ksig_fault(int signo, uint64_t cr2, uint64_t err, uint64_t vector);

/* --- the periodic half --------------------------------------------------- */
/* Expire alarms and drain the console. Called from the timer IRQ (100 Hz).
 * Gated internally so a machine with no alarm and an idle console pays one
 * load and a not-taken branch, plus one UART status read. */
void ksig_tick(void);

/* The console byte source. c/kernel/exec/file.c's tty_read calls this instead
 * of serial_getc(): ^C has to be seen even when NOBODY is reading the console
 * (the shell is in waitpid, the child is computing), so the timer drains the
 * UART, turns ^C into a SIGINT and queues everything else here. */
int  ksig_tty_getc(void);
/* The tty's single foreground pid -- see the honest note about sessions in
 * include/abi/logit_abi.h. Set by whoever reads the console. */
void ksig_tty_set_fg(int pid);
/* "The caller is now the foreground process." Resolves proc_current() HERE
 * rather than at the call site so that c/kernel/exec/file.c needs no new
 * include: it is linked into host test binaries, and pulling proc.h (and
 * through it interrupts.h) into it would be a build-shape change for the sake
 * of one field read. */
void ksig_tty_claim_fg(void);

/* --- syscalls ------------------------------------------------------------ */
long ksig_syscall(long num, long a, long b, long c);
/* SYS_KILL's signal half, called from proc.c's proc_syscall so that the ONE
 * refusal rule for the protected console process stays in the one file that
 * knows it. */
int  ksig_kill(int pid, int signo);

#endif /* LOGIT_KSIGNAL_H */
