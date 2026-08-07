/* Signals. Read the header comment in <signal.h> first -- it states exactly
 * which of this is real and which is a documented degenerate case. Summary:
 * raise() genuinely delivers (it is synchronous by definition); nothing else
 * ever delivers, because the kernel has no signal mechanism. */
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static __sighandler_t handlers[NSIG];
static sigset_t blocked;                 /* see sigprocmask below */

__sighandler_t signal(int sig, __sighandler_t fn)
{
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return SIG_ERR; }
    /* SIGKILL and SIGSTOP are uncatchable everywhere; refuse them so that code
     * checking for SIG_ERR behaves the same here as on any other system. */
    if (sig == SIGKILL || sig == SIGSTOP) { errno = EINVAL; return SIG_ERR; }
    __sighandler_t old = handlers[sig];
    handlers[sig] = fn;
    return old ? old : SIG_DFL;
}

/* REAL. C11 7.14.2.1: raise() sends the signal to the executing program. With
 * one thread and no kernel queue, calling the handler directly IS that.
 * SIG_DFL for a fatal signal terminates the process with the conventional
 * 128+signo status; SIG_IGN returns. */
int raise(int sig)
{
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    __sighandler_t fn = handlers[sig];
    if (fn == SIG_IGN) return 0;
    if (fn && fn != SIG_DFL && fn != SIG_ERR) {
        /* C says the handler is reset to SIG_DFL before a signal() handler runs
         * (the "unreliable" semantics signal() is specified with). */
        handlers[sig] = SIG_DFL;
        fn(sig);
        return 0;
    }
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
        return 0;                                  /* default action: ignore */
    default:
        _exit(128 + sig);
    }
}

/* DEGENERATE: there is no way for one process to interrupt another, so a kill()
 * to anyone else fails rather than pretending to have been delivered. kill() to
 * self is exactly raise(). sig 0 is the "does this process exist" probe. */
int kill(int pid, int sig)
{
    if (sig < 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    if (pid == getpid()) return sig ? raise(sig) : 0;
    errno = ENOSYS;
    return -1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; }
    if (old) {
        static struct sigaction tmp;
        tmp.sa_handler = handlers[sig] ? handlers[sig] : SIG_DFL;
        tmp.sa_mask = 0; tmp.sa_flags = 0;
        *old = tmp;
    }
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP) { errno = EINVAL; return -1; }
        handlers[sig] = act->sa_handler;
    }
    return 0;
}

int sigemptyset(sigset_t *s) { if (!s) { errno = EINVAL; return -1; } *s = 0; return 0; }
int sigfillset(sigset_t *s)  { if (!s) { errno = EINVAL; return -1; } *s = ~0UL; return 0; }
int sigaddset(sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } *s |= 1UL << sig; return 0; }
int sigdelset(sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } *s &= ~(1UL << sig); return 0; }
int sigismember(const sigset_t *s, int sig)
{ if (!s || sig <= 0 || sig >= NSIG) { errno = EINVAL; return -1; } return (*s >> sig) & 1; }

/* DEGENERATE, and harmlessly so: the mask is stored and returned faithfully,
 * and nothing consults it -- because nothing can be delivered for it to block.
 * Code that brackets a critical section with sigprocmask is already correct on
 * a system with no asynchronous signals; failing the call instead would break
 * that code for no gain. */
int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    if (old) *old = blocked;
    if (!set) return 0;
    switch (how) {
    case SIG_BLOCK:   blocked |= *set; break;
    case SIG_UNBLOCK: blocked &= ~*set; break;
    case SIG_SETMASK: blocked = *set; break;
    default: errno = EINVAL; return -1;
    }
    return 0;
}

/* DEGENERATE: there are no interval timers, so no SIGALRM can ever arrive.
 * Returns 0 ("no previous alarm was pending"), which is true. */
unsigned alarm(unsigned seconds) { (void)seconds; return 0; }

/* assert() lands here when NDEBUG is not defined; see <assert.h>. Going through
 * raise(SIGABRT) rather than straight to _exit is what lets a program install a
 * SIGABRT handler and get one last look at its own state. */
void __assert_fail(const char *expr, const char *file, int line, const char *fn)
{
    fprintf(stderr, "%s:%d: %s: Assertion `%s' failed.\n", file, line, fn ? fn : "?", expr);
    abort();
}
