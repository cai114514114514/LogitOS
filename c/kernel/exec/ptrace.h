#ifndef LOGIT_PTRACE_H
#define LOGIT_PTRACE_H

#include <stdint.h>

/* ===========================================================================
 * ptrace -- one process looking at another one that is still alive.
 *
 * THE HALF OF THE DEBUGGING QUESTION CORE DUMPS DO NOT ANSWER. A dump
 * (c/kernel/exec/coredump.h) says what a program looked like when it DIED. It
 * says nothing about a program that is running and wrong -- spinning, wedged,
 * or about to do something you want to see first. Before this, the only
 * debugger on the machine was `make debug`, which attaches gdb to QEMU and
 * therefore debugs the KERNEL, not the program.
 *
 * -------------------------------------------------------- WHAT IS HERE, ONLY
 * ATTACH, DETACH, GETREGS, SETREGS, PEEKDATA, POKEDATA, CONT.
 *
 * WHAT IS DELIBERATELY NOT HERE, said plainly rather than left to be
 * discovered by a caller that assumed otherwise:
 *
 *   - NO stop-on-fatal-signal. A traced process that faults still dies exactly
 *     as it did before, and still writes its core dump. Catching the fault
 *     instead means the tracer has to be WOKEN and waitpid() has to learn to
 *     report a ptrace stop as distinct from an exit -- a second state machine
 *     across proc.c and ksignal.c, which is where this feature would overrun.
 *     The case it would serve, "look at the program at the moment it crashed",
 *     is the one the core dump already answers on this machine.
 *   - NO single-step. It needs RFLAGS.TF, and the trap that comes back arrives
 *     as vector 1 in the same fault path that turns a debug exception into
 *     SIGTRAP -- so it is entangled with signal delivery in exactly the way
 *     the point above is. Not started rather than half-built.
 *   - NO PTRACE_TRACEME, no syscall tracing, no PEEKUSER. A child that asks to
 *     be traced needs the fork/exec path to hold it; that is the same missing
 *     stop protocol again.
 *
 * ------------------------------------------------------------ HOW IT STOPS
 * IT DOES NOT INVENT A STOP. c/kernel/exec/ksigframe.c's ksig_deliver()
 * already has one: SIGSTOP sets `stopped` and the thread sits in a
 * bkl_hlt_wait() loop at the return-to-ring-3 boundary, which is the ONE place
 * a complete user register frame exists. ATTACH posts SIGSTOP through the
 * ordinary path and the tracee stops there like any other stopped process;
 * this file only adds two calls inside that loop -- one to save the frame on
 * the way in, one to put it back on the way out. Building a second stop state
 * would have meant two things that can disagree about whether a process is
 * running.
 *
 * The consequence to know: ATTACH is not instantaneous. The tracee stops at
 * its next kernel exit, which the 100 Hz timer guarantees within one tick even
 * for a ring-3 loop that makes no syscall. ptrace_attach() therefore waits for
 * it, bounded, with the same bkl_hlt_wait() that does not hold the machine.
 *
 * ------------------------------------------------------------- WHO MAY
 * Two rules, both enforced in ptrace.c and neither of them decoration:
 *   1. The caller must have ATTACHED to this tracee. A pid is not a capability.
 *   2. At ATTACH, the caller must be root or share the target's uid, read
 *      through c/fs/vfs_cred.h -- the same credential vfsctl.c refuses an
 *      unprivileged shell with.
 * A process may not trace itself, pid 1, or a process that is already traced.
 *
 * BE HONEST ABOUT WHAT RULE 2 IS WORTH ON THIS MACHINE TODAY. Every process
 * starts as uid 0 unless /bin/login has run (c/fs/vfs_cred.h: "anything with
 * no current process -- is root"), so on a stock boot the uid test passes for
 * everybody and rule 1 is doing all the work. That is not a reason to leave
 * rule 2 out -- the machine has a login program, a session uid and a stored
 * on-disk mode, so the boundary is real when it is used -- but a reader should
 * not take "there is a uid check" to mean "an unprivileged process cannot read
 * your memory" on a machine where nothing dropped privilege.
 * =========================================================================== */

/* Requests. Deliberately NOT Linux's numbers: this is not Linux's ptrace and a
 * caller that got PTRACE_TRACEME (0) by porting code would be silently handed
 * ATTACH. Starting at 1 makes 0 an error rather than a surprise. */
#define PTRACE_ATTACH   1
#define PTRACE_DETACH   2
#define PTRACE_GETREGS  3   /* arg -> uint64_t[27], the core dump's pr_reg order */
#define PTRACE_SETREGS  4   /* arg -> uint64_t[27]                               */
#define PTRACE_PEEKDATA 5   /* arg -> struct logit_ptrace_word, data written      */
#define PTRACE_POKEDATA 6   /* arg -> struct logit_ptrace_word, data read         */
#define PTRACE_CONT     7

/* Errors. Distinct codes rather than -1 everywhere, because "you are not the
 * tracer" and "it has not stopped yet" call for opposite responses from a
 * tracer and are indistinguishable from a single failure value. */
#define PT_OK            0
#define PT_E_ARG       (-1)   /* unknown request, or a bad argument pointer      */
#define PT_E_SRCH      (-2)   /* no such process                                 */
#define PT_E_PERM      (-3)   /* not the tracer, or not permitted to become one  */
#define PT_E_BUSY      (-4)   /* already traced by somebody else                 */
#define PT_E_NOTSTOP   (-5)   /* attached, but it is not stopped right now       */
#define PT_E_FAULT     (-6)   /* that address is not readable/writable in it     */
#define PT_E_ALIGN     (-7)   /* PEEK/POKE address is not 8-byte aligned         */
#define PT_E_TIMEOUT   (-8)   /* ATTACH: it did not reach a stop in time         */
#define PT_E_NOSPACE   (-9)   /* the link table is full                          */

/* GETREGS/SETREGS hand over 27 registers in the SAME order the core dump's
 * NT_PRSTATUS uses (the CORE_R15..CORE_GS enum in coredump.h, which is
 * <sys/user.h>'s struct user_regs_struct order, diffed against glibc by
 * tests/unit/coredump_test.c). One order for both, so a tracer and a dump
 * reader index registers the same way and /bin/readcore's names apply to
 * both. */
#define PTRACE_NGREG 27

struct registers;   /* c/kernel/cpu/interrupts.h */

/* SYS_PTRACE. `arg` is a user pointer for the requests that take one and is
 * ignored otherwise. Returns PT_OK or a PT_E_*. */
long ptrace_syscall(long req, long pid, long arg);

/* --- the two hooks in ksig_deliver's stop loop --------------------------- */
/* The calling process is about to park as STOPPED, with `r` the frame it would
 * have returned to ring 3 on. Saves it so a tracer can read it. Costs one
 * not-taken branch when nothing is traced. */
void ptrace_note_stop(int pid, const struct registers *r);
/* It is about to resume. Applies a SETREGS the tracer made while it was
 * stopped, and forgets the saved frame -- a stale one would let a later
 * GETREGS report registers from a stop that has already ended. */
void ptrace_note_resume(int pid, struct registers *r);

/* A process is going away: drop any link it is either end of. Called from
 * proc_exit(), which is the one place a PCB is released. */
void ptrace_proc_free(int pid);

#endif /* LOGIT_PTRACE_H */
