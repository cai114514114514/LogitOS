/* <poll.h> / <sys/select.h>. See <poll.h> for exactly what is real (regular
 * files/dirs: fully accurate) and what is not (pipes/tty: no readiness
 * primitive exists in this kernel, so these never report ready -- see the
 * header for why that is the safe answer rather than a guess). */
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "logit_abi.h"

static long psys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* A seekable fd is a regular file or directory (see the identical technique
 * in isatty(), io.c) -- SEEK_CUR never mutates position or consumes data, so
 * this is a genuinely non-destructive probe. */
static int fd_seekable(int fd) { return psys(SYS_LSEEK, fd, 0, SEEK_CUR) >= 0; }

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    if (!fds && nfds) { errno = EFAULT; return -1; }

    int any_pipe = 0, ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (fd_seekable(fds[i].fd)) {
            if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
        } else {
            any_pipe = 1;   /* pipe/tty: cannot be answered now -- see below */
        }
    }
    if (ready) return ready;
    if (!any_pipe) return 0;                 /* nothing but regular fds, and none were ready to signal */
    if (timeout_ms < 0) { errno = ENOSYS; return -1; }   /* would block forever with no way to detect readiness */
    if (timeout_ms > 0) usleep((unsigned)timeout_ms * 1000u);
    return 0;                                 /* honest "nothing became ready that we could detect" */
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
    fd_set rin, win;
    if (readfds)  rin = *readfds;  else FD_ZERO(&rin);
    if (writefds) win = *writefds; else FD_ZERO(&win);
    if (readfds)  FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    int any_pipe = 0, ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int want_r = FD_ISSET(fd, &rin), want_w = FD_ISSET(fd, &win);
        if (!want_r && !want_w) continue;
        if (fd_seekable(fd)) {
            if (want_r) { FD_SET(fd, readfds);  ready++; }
            if (want_w) { FD_SET(fd, writefds); ready++; }
        } else {
            any_pipe = 1;
        }
    }
    if (ready) return ready;
    if (!any_pipe) return 0;
    if (!timeout) { errno = ENOSYS; return -1; }   /* NULL timeout = block forever; see <poll.h> */
    unsigned ms = (unsigned)timeout->tv_sec * 1000u + (unsigned)timeout->tv_usec / 1000u;
    if (ms) usleep(ms * 1000u);
    return 0;
}
