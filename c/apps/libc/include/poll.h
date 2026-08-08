#ifndef _POLL_H
#define _POLL_H

/* poll()/select() over a kernel that has NO generic fd-readiness primitive
 * (no FIONREAD-equivalent, no wait-for-any-of-these-fds syscall). What is
 * real and what is not, precisely:
 *
 *   REGULAR FILES AND DIRECTORIES are always reported readable and
 *   writable, and that is not an approximation: c/kernel/exec/file.c's F_VFS
 *   holds the whole file in a kmalloc buffer, so a read or write on one never
 *   blocks and never has "not yet" to report.
 *
 *   PIPES AND THE TTY (F_PIPE / F_TTY) cannot be answered honestly. Their
 *   ONLY readiness signal is what <unistd.h> already documents: open the fd
 *   O_NONBLOCK (SYS_SETNB) and let read()/write() return -1/EAGAIN when
 *   there is nothing to do -- and that signal is destructive to peek with
 *   (a "peek" read that succeeds has just consumed the byte a real read call
 *   would need). So poll()/select() report these fds NEVER READY rather than
 *   guessing, and:
 *     - a call with timeout 0 (a non-blocking probe) returns immediately, 0
 *       fds ready, which is always a SAFE answer (it costs the caller a
 *       spurious retry, never a lost wakeup or a lie);
 *     - a call with timeout >= 0 sleeps the requested time and then reports
 *       0 ready, which is honest ("nothing became ready that we could
 *       detect") even though it may be wrong (something WAS ready) --
 *       stated here so a caller relying on prompt pipe wakeups through this
 *       interface knows to poll its pipe directly with O_NONBLOCK instead;
 *     - a call with an INFINITE timeout (-1) over a set that contains a
 *       pipe/tty fd is refused outright (errno ENOSYS) rather than blocking
 *       forever with no way to ever return.
 *   A kernel fix that would close this gap: a syscall that reports bytes
 *   queued in a pipe/tty ring without consuming them (see the libc inventory
 *   report, bucket C). */

typedef unsigned long nfds_t;

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x080

struct pollfd { int fd; short events; short revents; };

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* _POLL_H */
