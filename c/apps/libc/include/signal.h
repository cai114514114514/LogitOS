#ifndef _SIGNAL_H
#define _SIGNAL_H

/* SIGNALS ON LOGITOS -- what is real, and what is still not.
 *
 * This header used to open by saying the kernel had no signal delivery, and
 * that was true: nothing could cause a handler registered here to run.
 * c/kernel/exec/ksignal.c and ksigframe.c are that mechanism now. A handler
 * installed here really is entered on a frame the kernel pushes onto this
 * process's stack, with the interrupted registers AND the FPU/SSE state saved,
 * and sigreturn restores them exactly. So:
 *
 *   - a SIGSEGV/SIGBUS/SIGFPE/SIGILL handler runs on a real ring-3 fault
 *     (default action is still terminate, so a program that installs nothing
 *     dies exactly as it did);
 *   - kill() reaches another process;
 *   - SIGCHLD arrives when a child exits, SIGPIPE when a write finds no
 *     reader, SIGALRM from alarm(), SIGINT from Ctrl+C on the console;
 *   - sigprocmask really blocks, sigpending really reports, sigsuspend really
 *     waits;
 *   - a blocking syscall interrupted by a delivered handler returns EINTR, or
 *     is restarted if the handler was installed with SA_RESTART.
 *
 * WHAT IS STILL NOT HERE, stated so that nobody has to find out by testing:
 *
 *   - NO SIGINFO. SA_SIGINFO is accepted and ignored: a three-argument handler
 *     is called with a NULL siginfo_t* and a `struct logit_sigctx *` as its
 *     third argument, so it can read the machine state but not si_code or
 *     si_addr. There is no sigqueue().
 *   - NO ALTERNATE STACK. sigaltstack() does not exist, so a SIGSEGV caused by
 *     stack exhaustion cannot be handled -- there is nowhere to put the frame.
 *   - NO PER-THREAD MASKS AND NO THREAD-DIRECTED SIGNALS. One pending set and
 *     one mask per PROCESS; a pending signal is taken by whichever thread next
 *     returns to ring 3, which POSIX permits for a process-directed signal.
 *     pthread_kill() still returns ENOSYS.
 *   - NO JOB CONTROL. There are no sessions and no process groups, so
 *     kill(0, sig) and kill(-pgid, sig) are not process-group sends, and the
 *     tty has one foreground pid (whoever last read it) rather than a
 *     foreground group. SIGSTOP/SIGCONT do stop and continue a process.
 *   - NO SIGCHLD DETAIL. It says a child changed state; waitpid() says which. */

typedef int sig_atomic_t;
typedef unsigned long sigset_t;

typedef void (*__sighandler_t)(int);
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)
#define SIG_HOLD ((__sighandler_t)2)

/* Linux/x86-64 numbering, for the same reason as <errno.h>. */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGIOT   SIGABRT
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO   29
#define SIGPOLL SIGIO
#define SIGPWR  30
#define SIGSYS  31
#define NSIG    32

/* sigprocmask() how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    union {
        __sighandler_t sa_handler;
        void (*sa_sigaction)(int, void *, void *);
    } __sa;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define sa_handler   __sa.sa_handler
#define sa_sigaction __sa.sa_sigaction

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

__sighandler_t signal(int, __sighandler_t);
int raise(int);
int kill(int, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
unsigned alarm(unsigned);

#endif /* _SIGNAL_H */
