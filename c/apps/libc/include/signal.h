#ifndef _SIGNAL_H
#define _SIGNAL_H

/* SIGNALS ON LOGITOS -- READ THIS BEFORE RELYING ON ANY OF IT.
 *
 * The kernel has no signal delivery. There is no mechanism by which a timer,
 * another process, or a fault can cause a handler registered here to run. What
 * this header gives you is exactly two things, and they are honest ones:
 *
 *   1. Every C program that MENTIONS signals compiles and links. That is most
 *      of the interesting ones: a `signal(SIGPIPE, SIG_IGN)` at the top of
 *      main, a SIGINT handler that sets a flag, an atexit-style cleanup path.
 *
 *   2. raise() genuinely works, because raise() is synchronous -- C defines it
 *      as sending the signal to the executing program, and a direct call to the
 *      installed handler is a correct implementation of that. abort() goes
 *      through raise(SIGABRT), so a program that installs a SIGABRT handler to
 *      dump state before dying gets it.
 *
 * What does NOT happen: nothing arrives asynchronously. A handler installed for
 * SIGSEGV never runs -- a ring-3 fault kills the process in the kernel (see
 * c/kernel/interrupts.c) without consulting this table. kill() to another
 * process returns -1/ENOSYS rather than pretending. sigprocmask and the
 * sigset_t operations manipulate a mask that nothing consults; they return
 * success because there is nothing that could fail, and because code that
 * blocks signals around a critical section is already correct on a system with
 * no signals. Each is marked at its definition in signal.c. */

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
unsigned alarm(unsigned);

#endif /* _SIGNAL_H */
