#ifndef _POLL_H
#define _POLL_H

/* poll() over SYS_POLL. See the block at SYS_POLL in include/abi/logit_abi.h
 * for the readiness model -- what LPOLLIN promises, which bits arrive
 * unrequested, and which descriptor types can be answered for.
 *
 * WHAT THIS HEADER USED TO SAY, and why it is worth recording rather than
 * quietly deleting. Until SYS_POLL existed there was no fd-readiness primitive
 * in this kernel at all, so this file implemented poll() by SEEKING each
 * descriptor: a seekable fd is a regular file and was reported ready, and
 * everything else -- every pipe, every tty, every socket -- was reported NEVER
 * READY. It said so in forty lines, honestly, and it ended:
 *
 *     "A kernel fix that would close this gap: a syscall that reports bytes
 *      queued in a pipe/tty ring without consuming them."
 *
 * What landed is not that syscall, and the difference matters: a "bytes
 * queued" call would have made poll() a POLLING LOOP -- ask, sleep, ask again
 * -- which is a wakeup latency and a wasted timeslice per iteration. SYS_POLL
 * parks the caller on the wait queues of the objects themselves, so it is
 * woken BY the write, not by a clock. The old implementation's three documented
 * behaviours are all gone with it: a pipe is no longer permanently unready, a
 * timeout no longer means "sleep and then report nothing", and an infinite
 * timeout over a pipe is no longer refused with ENOSYS.
 *
 * ONE LIMIT REMAINS AND IT IS THE KERNEL'S: nfds may not exceed 32 (NFD, the
 * number of descriptors a process can hold). A larger request is EINVAL, not a
 * truncated answer, because a poll that silently ignored entries would be worse
 * than one that refused. */

typedef unsigned long nfds_t;

/* Must match the LPOLL* block in include/abi/logit_abi.h -- the same convention
 * and the same note as <fcntl.h>'s O_* values. */
#define POLLIN   0x001
#define POLLPRI  0x002       /* never set: no out-of-band data exists here */
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLWRNORM 0x080

struct pollfd { int fd; short events; short revents; };

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* _POLL_H */
