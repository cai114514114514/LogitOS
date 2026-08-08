/* Signals in mini-libc.
 *
 * THIS FILE USED TO BE A LIE THAT SAID SO. Its old header comment was honest
 * about it -- "the kernel has no signal delivery", raise() called the handler
 * directly because that is a correct implementation of a synchronous signal,
 * kill() to anyone else returned ENOSYS, and sigprocmask manipulated a mask
 * nothing consulted. All of that was the best available answer to a kernel with
 * no mechanism. The mechanism now exists (c/kernel/exec/ksignal.c +
 * ksigframe.c), so every function here is a thin wrapper over it and the
 * table it used to keep is gone: there is exactly one copy of the dispositions
 * and it is the kernel's.
 *
 * THE ONE THING RING 3 STILL HAS TO PROVIDE is the restorer. A handler is an
 * ordinary C function and ends in `ret`, so something must sit at [rsp] that
 * executes SYS_SIGRETURN. Linux keeps those four instructions in the VDSO;
 * there is none here, and the alternatives are executing off the user stack
 * (which the NX bit exists to prevent) or mapping a kernel-owned executable
 * page into every address space. So this file supplies them and fills
 * sa_restorer in on every call, exactly as Linux's own libc did with
 * SA_RESTORER before the VDSO existed. The kernel REFUSES a handler with no
 * restorer rather than delivering to one that would return into nothing, so
 * this is not an optimisation that can be quietly dropped.
 */
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "logit_abi.h"

/* --- the restorer -------------------------------------------------------
 * Four bytes of instruction and one constraint that matters: IT MUST NOT
 * TOUCH THE STACK. The kernel finds the frame it pushed at whatever rsp is
 * when the int lands, and the handler's own `ret` has already popped this
 * address off -- so rsp points exactly at the struct logit_sigctx. A push, a
 * frame pointer, or a call here would move it and the restore would read
 * garbage.
 *
 * Hence raw asm rather than a C function with __attribute__((naked)): the
 * requirement is about the emitted instructions, so they are written out.
 * The syscall number comes from the shared header through the stringify
 * macro, so it cannot drift from include/abi/logit_abi.h. */
#define SIG_STR2(x) #x
#define SIG_STR(x)  SIG_STR2(x)

__asm__(
    ".text\n"
    ".globl __logit_sigrestore\n"
    ".hidden __logit_sigrestore\n"
    "__logit_sigrestore:\n"
    "    movq $" SIG_STR(SYS_SIGRETURN) ", %rax\n"
    "    int $0x80\n"
    /* SYS_SIGRETURN does not return. If it ever does, the frame was rejected
     * and there is nothing sane to resume, so fall into an infinite loop
     * rather than executing whatever follows in memory. */
    "1:  jmp 1b\n"
);
extern void __logit_sigrestore(void);

static long sigsys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* The kernel's SIG_E_* -> errno. SIG_E_INTR is not in here on purpose: it is
 * not an error of these calls, it is the interrupted-syscall return, and the
 * places that can see it map it themselves. */
static int sigerr(long rc)
{
    switch (rc) {
    case SIG_E_SRCH: errno = ESRCH;  break;
    case SIG_E_PERM: errno = EPERM;  break;
    case SIG_E_NOSYS: errno = ENOSYS; break;
    default:         errno = EINVAL; break;
    }
    return -1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; }

    struct logit_sigaction ka, ko;
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP) { errno = EINVAL; return -1; }
        ka.handler  = (unsigned long)(void *)act->sa_handler;
        ka.mask     = (unsigned long)act->sa_mask;
        ka.flags    = (unsigned long)(unsigned)act->sa_flags;
        /* sa_restorer is filled in HERE and the caller's value is ignored.
         * POSIX does not have the field at all, so no portable program sets
         * it, and a program that copied one out of an `old` struct and handed
         * it back would otherwise be installing a pointer it does not
         * understand. There is exactly one correct restorer in this process
         * and this is it. */
        ka.restorer = (unsigned long)(void *)__logit_sigrestore;
    }
    long rc = sigsys(SYS_SIGACTION, sig, act ? (long)&ka : 0, old ? (long)&ko : 0);
    if (rc < 0) return sigerr(rc);
    if (old) {
        old->sa_handler  = (__sighandler_t)(void *)(unsigned long)ko.handler;
        old->sa_mask     = (sigset_t)ko.mask;
        old->sa_flags    = (int)(unsigned)ko.flags;
        old->sa_restorer = (void (*)(void))(void *)(unsigned long)ko.restorer;
    }
    return 0;
}

/* signal() is the "unreliable" C interface, and the difference from sigaction()
 * is real: C says the handler is reset to SIG_DFL before it runs. That is
 * SA_RESETHAND, so signal() asks for it explicitly rather than being a synonym
 * for sigaction() -- a program that installs with signal() and expects the
 * one-shot behaviour gets it. SA_NODEFER matches the historical System V
 * semantics the name inherits. */
__sighandler_t signal(int sig, __sighandler_t fn)
{
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return SIG_ERR; }
    if (sig == SIGKILL || sig == SIGSTOP) { errno = EINVAL; return SIG_ERR; }
    struct sigaction act, old;
    act.sa_handler = fn;
    act.sa_mask = 0;
    act.sa_flags = SA_RESETHAND | SA_NODEFER;
    act.sa_restorer = 0;
    if (sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int kill(int pid, int sig)
{
    if (sig < 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    /* LOGIT_KILL_SIGNAL is what separates this from the historical
     * "destroy that process" meaning of SYS_KILL, which the task manager
     * still uses and which must keep working unchanged. */
    long rc = sigsys(SYS_KILL, pid, sig, LOGIT_KILL_SIGNAL);
    if (rc < 0) return sigerr(rc);
    return 0;
}

int raise(int sig) { return kill(getpid(), sig); }

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    unsigned long s = set ? (unsigned long)*set : 0, o = 0;
    long rc = sigsys(SYS_SIGPROCMASK, how, set ? (long)&s : 0, old ? (long)&o : 0);
    if (rc < 0) return sigerr(rc);
    if (old) *old = (sigset_t)o;
    return 0;
}

int sigpending(sigset_t *out)
{
    unsigned long v = 0;
    if (!out) { errno = EINVAL; return -1; }
    long rc = sigsys(SYS_SIGPENDING, (long)&v, 0, 0);
    if (rc < 0) return sigerr(rc);
    *out = (sigset_t)v;
    return 0;
}

/* sigsuspend's only successful outcome is -1/EINTR, which is what POSIX
 * specifies and not a failure to implement it. */
int sigsuspend(const sigset_t *mask)
{
    unsigned long m = mask ? (unsigned long)*mask : 0;
    long rc = sigsys(SYS_SIGSUSPEND, mask ? (long)&m : 0, 0, 0);
    if (rc == SIG_E_INTR) { errno = EINTR; return -1; }
    if (rc < 0) return sigerr(rc);
    errno = EINTR;
    return -1;
}

unsigned alarm(unsigned seconds)
{
    long rc = sigsys(SYS_ALARM, (long)seconds, 0, 0);
    return rc < 0 ? 0u : (unsigned)rc;
}

int sigemptyset(sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = 0; return 0; }
int sigfillset(sigset_t *s)  { if (!s) { errno = EINVAL; return -1; } *s = ~0UL; return 0; }
int sigaddset(sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } *s |= 1UL << sig; return 0; }
int sigdelset(sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } *s &= ~(1UL << sig); return 0; }
int sigismember(const sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } return (int)((*s >> sig) & 1); }

/* assert() lands here when NDEBUG is not defined; see <assert.h>. Going through
 * raise(SIGABRT) rather than straight to _exit is what lets a program install a
 * SIGABRT handler and get one last look at its own state -- and now that raise()
 * is a real kill() to self, that actually happens. */
void __assert_fail(const char *expr, const char *file, int line, const char *fn)
{
    fprintf(stderr, "%s:%d: %s: Assertion `%s' failed.\n", file, line, fn ? fn : "?", expr);
    abort();
}
