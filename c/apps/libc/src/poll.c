/* <poll.h>, <sys/select.h>, <sys/eventfd.h>, <sys/timerfd.h> -- all four over
 * SYS_POLL / SYS_EVENTFD / SYS_TIMERFD.
 *
 * WHAT THIS FILE USED TO BE, because the change is the point of it. There was
 * no fd-readiness syscall, so poll() SEEKED each descriptor: an fd that
 * accepted lseek(SEEK_CUR) is a regular file and was reported ready, and every
 * pipe, tty and socket was reported NEVER READY -- with an infinite timeout
 * over such a set refused outright (ENOSYS) because it would otherwise have
 * blocked forever with no way to return. That was the honest implementation of
 * a kernel that could not answer the question. It is deleted, not layered over:
 * a fallback path would be a second answer to "is this fd ready", and the whole
 * value of SYS_POLL is that there is exactly one.
 *
 * select() STAYS IN LIBC, and that is a deliberate ABI decision argued at
 * SYS_POLL in include/abi/logit_abi.h: fd_set is a userland bitmap whose width
 * is a compile-time constant of THIS library, and putting it in the kernel
 * would freeze FD_SETSIZE there forever. It is a translation to poll() and
 * back, and it costs the kernel nothing.
 */

#include <poll.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include "logit_abi.h"

static long psys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* The kernel's negative codes are a small closed set, so the mapping is a
 * switch and not a table lookup with a default that hides a new one. Anything
 * unrecognised becomes EINVAL and returns -1, which is still a refusal -- never
 * a success. */
static int poll_errno(long rc)
{
    switch (rc) {
    case POLL_E_ARG:   errno = EINVAL; break;
    case POLL_E_FAULT: errno = EFAULT; break;
    case POLL_E_NOMEM: errno = ENOMEM; break;
    case SIG_E_INTR:   errno = EINTR;  break;
    default:           errno = EINVAL; break;
    }
    return -1;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    if (!fds && nfds) { errno = EFAULT; return -1; }
    if (nfds > (nfds_t)LOGIT_POLL_MAX) { errno = EINVAL; return -1; }
    /* struct pollfd and struct logit_pollfd are the same three fields in the
     * same order, so this is a pass-through and not a copy loop. If that ever
     * stops being true this cast is where it breaks, loudly, at the first
     * wrong revents -- which is why the ABI struct was defined to match POSIX's
     * rather than being given a layout of its own. */
    long rc = psys(SYS_POLL, (long)fds, (long)nfds, (long)timeout_ms);
    if (rc < 0) return poll_errno(rc);
    return (int)rc;
}

/* --- select(), as a translation to poll() ---------------------------------
 *
 * THE EXCEPT SET IS ALWAYS EMPTIED AND NEVER FILLED, and that is the truthful
 * answer rather than an omission: an "exceptional condition" in select(2) means
 * out-of-band data, and there is none anywhere in this kernel (POLLPRI is never
 * set -- the ABI says so). Reporting an fd in exceptfds because it had an error
 * would be a different and wrong claim; POLLERR belongs in readfds/writefds,
 * where it makes the following read() or write() return the error.
 *
 * A TIMEOUT of NULL means block forever, which is now expressible -- the old
 * implementation refused it with ENOSYS because it had nothing to block on. */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }

    struct pollfd pf[LOGIT_POLL_MAX];
    int map[LOGIT_POLL_MAX];
    int n = 0;

    for (int fd = 0; fd < nfds; fd++) {
        int want_r = readfds  && FD_ISSET(fd, readfds);
        int want_w = writefds && FD_ISSET(fd, writefds);
        if (!want_r && !want_w) continue;
        if (n >= LOGIT_POLL_MAX) { errno = EINVAL; return -1; }
        pf[n].fd      = fd;
        pf[n].events  = (short)((want_r ? POLLIN : 0) | (want_w ? POLLOUT : 0));
        pf[n].revents = 0;
        map[n] = fd;
        n++;
    }

    int tmo = -1;
    if (timeout) {
        long ms = (long)timeout->tv_sec * 1000 + (long)timeout->tv_usec / 1000;
        /* Round a sub-millisecond but non-zero timeout UP to 1 ms. Rounding it
         * to 0 would turn a short wait into a pure probe, which is the one
         * conversion error a caller cannot see: the call returns instantly and
         * "nothing was ready" is a legal answer. */
        if (ms == 0 && (timeout->tv_sec || timeout->tv_usec)) ms = 1;
        if (ms > INT_MAX) ms = INT_MAX;
        tmo = (int)ms;
    }

    int rc = poll(pf, (nfds_t)n, tmo);

    /* The sets are rewritten only AFTER the wait, and only on success. On -1
     * they are left exactly as the caller passed them, which is what POSIX
     * requires and what lets a caller retry an EINTR without rebuilding them. */
    if (rc < 0) return -1;
    if (readfds)   FD_ZERO(readfds);
    if (writefds)  FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);
    if (rc == 0) return 0;

    int count = 0;
    for (int i = 0; i < n; i++) {
        short r = pf[i].revents;
        if (!r) continue;
        /* POLLERR and POLLHUP are reported to BOTH sides the caller asked
         * about. select() has no way to say "this fd is broken" other than
         * marking it ready, and the read() or write() that follows is what
         * delivers the actual error. */
        int err = (r & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (readfds && (pf[i].events & POLLIN) && ((r & POLLIN) || err))
            { FD_SET(map[i], readfds); count++; }
        if (writefds && (pf[i].events & POLLOUT) && ((r & POLLOUT) || err))
            { FD_SET(map[i], writefds); count++; }
    }
    return count;
}

/* --- eventfd --------------------------------------------------------------
 * eventfd_read/eventfd_write are glibc's helpers, and they are here because a
 * program that uses them expects a partial transfer to be IMPOSSIBLE, not
 * merely unlikely: the kernel refuses anything shorter than 8 bytes rather
 * than truncating, so these either move the whole counter or fail. */
int eventfd(unsigned int initval, int flags)
{
    long rc = psys(SYS_EVENTFD, (long)(unsigned long)initval, (long)flags, 0);
    if (rc < 0) return poll_errno(rc);
    return (int)rc;
}

int eventfd_read(int fd, eventfd_t *value)
{
    eventfd_t v = 0;
    long n = read(fd, &v, sizeof v);
    if (n != (long)sizeof v) { if (n >= 0) errno = EINVAL; return -1; }
    if (value) *value = v;
    return 0;
}

int eventfd_write(int fd, eventfd_t value)
{
    long n = write(fd, &value, sizeof value);
    if (n != (long)sizeof value) { if (n >= 0) errno = EINVAL; return -1; }
    return 0;
}

/* --- timerfd -------------------------------------------------------------- */

int timerfd_create(int clockid, int flags)
{
    /* CLOCK_REALTIME is REFUSED rather than quietly served by the monotonic
     * clock: the whole difference between them is whether the timer survives
     * the wall clock being set, and substituting one for the other is how every
     * armed timer fires at once the first time the RTC is read. */
    if (clockid != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    long rc = psys(SYS_TIMERFD, -1, 0, (long)(flags & TFD_NONBLOCK));
    if (rc < 0) return poll_errno(rc);
    return (int)rc;
}

/* ms, rounded UP, with anything non-zero becoming at least 1. A timer asked for
 * and never fired is a worse failure than one that fires a tick late, and the
 * kernel rounds up to a tick again on top of this. */
static long ts_ms(const struct timespec *t)
{
    if (!t) return 0;
    long ms = (long)t->tv_sec * 1000 + (t->tv_nsec + 999999L) / 1000000L;
    if (ms == 0 && (t->tv_sec || t->tv_nsec)) ms = 1;
    return ms;
}

int timerfd_settime(int fd, int flags, const struct itimerspec *newv,
                    struct itimerspec *oldv)
{
    if (!newv) { errno = EFAULT; return -1; }
    /* TFD_TIMER_ABSTIME would mean "it_value is a point on the clock, not a
     * duration". Refused rather than treated as relative: the two differ by
     * however long the machine has been up, so accepting it would arm every
     * absolute timer days late. */
    if (flags & TFD_TIMER_ABSTIME) { errno = EINVAL; return -1; }
    /* oldv would have to report the REMAINING time, which needs a kernel call
     * that does not exist (there is no timerfd_gettime here). Refusing is the
     * only honest answer: filling it with zeroes would tell a caller its timer
     * was disarmed. */
    if (oldv) { errno = ENOSYS; return -1; }

    struct logit_itimer it;
    it.value_ms    = ts_ms(&newv->it_value);
    it.interval_ms = ts_ms(&newv->it_interval);
    long rc = psys(SYS_TIMERFD, fd, (long)&it, 0);
    if (rc < 0) return poll_errno(rc);
    return 0;
}
