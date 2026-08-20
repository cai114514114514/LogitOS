#ifndef LOGIT_KPOLL_H
#define LOGIT_KPOLL_H

/* poll() -- waiting on several descriptors at once.
 *
 * WHY THIS HEADER IS NOT CALLED poll.h, since that is the obvious name and the
 * first thing anybody looks for. mini-libc ships c/apps/libc/include/poll.h,
 * and the Makefile's INCDIRS is one FLAT sorted list built from
 * `find c include -type d` -- c/apps/libc/include sorts before c/kernel/exec,
 * so a kernel file writing #include "poll.h" gets the USERLAND header and then
 * fails somewhere else entirely, on an undeclared kernel function, in a file
 * nobody edited. CLAUDE.md records that exact failure twice (sys/wait.h and
 * sched.h), and c/kernel/exec/file.c carries the path-qualified include that is
 * the scar from the first one. The alternative was to move the userland header
 * into c/apps/libc/include/uonly/, which is the sanctioned fix -- rejected
 * because it changes what every ring-3 program's include path resolves for a
 * POSIX header name, to buy one character in a kernel filename.
 *
 * ---------------------------------------------------------------------------
 * THE HOOK IS THE DESIGN; THE SYSCALL IS THE SMALL PART.
 *
 * The shape that was rejected, by name: a switch over every fd type inside
 * poll(). It works, it is shorter, and it means the socket layer's poll support
 * has to be written by editing THIS file -- so the one subsystem most likely to
 * need it is the one that cannot add it. Linux's answer is poll_wait: a
 * pollable object is asked "register this waiter on whatever queue you would
 * wake, then tell me your readiness right now", and it answers with code that
 * lives beside the object's own state.
 *
 * A backend is therefore one function:
 *
 *     short obj_poll(void *obj, struct poll_table *pt)
 *     {
 *         poll_wait(pt, &obj->wq);           <-- FIRST, unconditionally
 *         return (obj->count ? LPOLLIN : 0) | ...;
 *     }
 *
 * THE ORDER IN THOSE TWO LINES IS THE WHOLE CORRECTNESS ARGUMENT. Registering
 * before reading the state is what makes an event that lands between the read
 * and the sleep impossible to lose: the waker takes the queue's lock, finds the
 * registration already there, and publishes into the poller's own flag (see
 * struct waiter's poll-hook comment in c/kernel/core/wait.h). Reading first and
 * registering after is the bug every poll implementation ships once, and it is
 * what -DPOLL_NO_PREREGISTER builds so that the gate can watch it fail.
 *
 * TWO CONTRACTS A BACKEND MUST MEET, and neither is optional:
 *
 *  1. poll_wait() FIRST, before any readiness state is read. Above.
 *
 *  2. The object must wake its queue with waitq_wake_ALL, never wake_one. A
 *     poller registered on the queue is a legitimate waiter that consumes a
 *     wake_one WITHOUT consuming the data -- so a wake_one that happens to pick
 *     the poller leaves the real reader parked with data waiting for it. Every
 *     queue in this tree already uses wake_all (pipes in file.c, the listener
 *     and rx queues in c/net), so this costs nothing today; it is written down
 *     because the first wake_one added later would be a hang, not a slowdown,
 *     and it would be blamed on poll().
 *
 * THE SOCKET HOOK, for the line that owns c/net/core/lsock.c. Add exactly this
 * to lsock.c and nothing in kpoll.c or file.c changes:
 *
 *     short lsock_file_poll(struct file *f, struct poll_table *pt);
 *
 * file.c already declares it __attribute__((weak)) and calls it for F_SOCK
 * exactly as it already does for lsock_file_read / lsock_file_write, so a build
 * without a network stack links unchanged and answers LPOLLNVAL. What it should
 * return: for a connection, poll_wait() on the rx wait queue (tcp.c's rx_wq)
 * and then LPOLLIN if tcp_available(id), LPOLLHUP once the peer's FIN has been
 * seen and drained, LPOLLOUT while the send path would accept a byte; for a
 * LISTENER, poll_wait() on l->wq and LPOLLIN when l->qn > 0, which is what
 * makes accept() pollable and is the entire point of the exercise. One queue
 * per fd is enough for both shapes -- see POLL_MAXWAIT below for why that
 * matters.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include "spinlock.h"
#include "logit_abi.h"          /* LPOLL*, struct logit_pollfd, POLL_E_* */
#include "kernel/core/wait.h"   /* struct waitq / struct waiter. Path-qualified
                                 * for the reason file.c gives: mini-libc has a
                                 * sys/wait.h and INCDIRS is flat. */

/* One wait-queue registration. `w` is an ordinary waiter carrying the poll
 * hook, so the queue side of this needs no special case at all. */
struct poll_ent {
    struct waitq *q;
    struct waiter w;
};

/* POLL_MAXWAIT IS NFD, AND THAT IS AN ARGUMENT RATHER THAN A ROUND NUMBER.
 * A process cannot have more than NFD = 32 descriptors open (c/kernel/exec/
 * proc.h), so 32 fds is the largest set that can exist, and every backend in
 * this tree registers exactly ONE queue per fd -- a pipe has one queue serving
 * both directions, and file.c argues that choice where the pipe is defined.
 * A backend wanting two queues would overflow this table on a full set, which
 * is why poll_wait() marks the overflow and poll_core() REFUSES (POLL_E_NOMEM)
 * instead of sleeping on a partial registration. Sleeping on a partial
 * registration is exactly the lost wakeup this file exists to prevent; a loud
 * refusal is worse to receive and much better to debug.
 *
 * Cost: 32 * sizeof(struct poll_ent) = 1,536 B on the calling thread's kernel
 * stack, which is 32 KiB. Deliberately on the stack and not kmalloc'd -- the
 * table lives exactly as long as the frame that is blocked, which is the same
 * argument c/kernel/core/wait.c makes for putting `struct waiter` there. */
#define POLL_MAXWAIT LOGIT_POLL_MAX

struct poll_table {
    /* `lock` guards `triggered` and NOTHING ELSE. It is the lock handed to the
     * sleep, so it is also the lock a waker publishes `triggered` under -- rule
     * 2 of c/kernel/core/wait.h, with this table standing in for the object. */
    spinlock_t      lock;
    int             triggered;
    int             n;
    int             overflow;
    struct poll_ent e[POLL_MAXWAIT];
};

/* Called by a backend, FIRST, before it reads any readiness state. A NULL `pt`
 * is a no-op, which is what makes a backend's poll function reusable as a pure
 * "what is your mask right now" probe. */
void poll_wait(struct poll_table *pt, struct waitq *q);

/* One entry of the set poll_core() waits on. `ready` is the backend hook;
 * `obj` is whatever it wants (file.c passes a struct file *). */
struct pollsrc {
    void  *obj;
    short (*ready)(void *obj, struct poll_table *pt);
    short  events;    /* what the caller asked about */
    short  revents;   /* filled in by poll_core */
};

/* Wait until at least one source is ready, the timeout expires, or a signal
 * arrives. Returns the number of sources with a non-zero revents, 0 on timeout,
 * SIG_E_INTR, or POLL_E_NOMEM. timeout_ms < 0 = forever, 0 = probe only.
 *
 * Generic over `struct pollsrc` rather than over `struct file *` so that the
 * host gate can drive the REAL code with a model object -- file.c cannot be
 * compiled for the host (kheap, vfs, serial, percpu, the BKL), and a poll core
 * that could only be tested through file.c could not be tested at all. */
int poll_core(struct pollsrc *src, int n, int timeout_ms);

/* The syscall face: SYS_POLL / SYS_EVENTFD / SYS_TIMERFD. Forwarded whole from
 * syscall.c for the reason mm_syscall() and uthread_syscall() give -- which
 * argument is a user pointer and what it means are facts about this subsystem. */
long poll_syscall(long nr, long a, long b, long c);

#endif /* LOGIT_KPOLL_H */
