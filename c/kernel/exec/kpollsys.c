/* The syscall face of poll(): SYS_POLL, SYS_EVENTFD, SYS_TIMERFD.
 *
 * SEPARATE FROM kpoll.c ON PURPOSE, and the reason is the gate rather than
 * tidiness. kpoll.c is the part whose correctness is subtle -- the registration
 * order that closes the lost wakeup -- and it is written so a host binary can
 * link it and provoke that race deliberately. This file cannot be host-linked:
 * it reaches proc.c's fd table, file.c's backends and the user-copy machinery,
 * every one of which needs a kernel. Keeping them in one file would have made
 * the testable half untestable, which is the trade CLAUDE.md's WPT note
 * describes from the other direction -- linking a translation unit is not
 * running it, and a unit that cannot be linked alone cannot be run alone.
 *
 * What lives here is only the translation: user pointer -> kernel array, fd ->
 * struct file, mask -> revents. No readiness logic and no waiting.
 */

#include <stdint.h>
#include <stddef.h>
#include "kpoll.h"
#include "file.h"
#include "proc.h"
#include "usercopy.h"
#include "logit_abi.h"

void *memcpy(void *, const void *, size_t);

/* The bridge from a `struct pollsrc` back to a descriptor. poll_core() knows
 * nothing about files; this is the one line that says a source's object is a
 * struct file and its readiness is file_poll's answer. */
static short src_file_ready(void *obj, struct poll_table *pt)
{
    return file_poll((struct file *)obj, pt);
}

/* --------------------------------------------------------------------------
 * SYS_POLL (fds, nfds, timeout_ms)
 *
 * THE fd TABLE IS SNAPSHOTTED, and that is a decision rather than an
 * optimisation. `struct file *` pointers are resolved once, before the wait; a
 * close() from another thread of the same process during the wait therefore
 * leaves this call holding a pointer whose refcount it did not take.
 *
 * That is safe here and would not be in general, so it is written down: every
 * caller of poll() holds the descriptors it is polling, a close of an fd this
 * process is currently polling is a program bug in any POSIX system (Linux's
 * behaviour is famously unspecified), and the only alternative -- file_dup()
 * on every entry and file_close() on the way out -- would take and drop
 * g_file_lock 2*nfds times per poll call, on the hottest loop a server has.
 * What makes it defensible rather than merely cheap: the descriptions are
 * statically allocated (files[] in file.c), so a stale pointer is a wrong
 * answer, never a fault, and a closed slot reads as F_NONE, which file_poll
 * answers LPOLLNVAL for.
 * ------------------------------------------------------------------------ */
static long sys_poll(long ufds, long nfds, long timeout_ms)
{
    struct logit_pollfd *u = (struct logit_pollfd *)ufds;
    if (nfds < 0 || nfds > LOGIT_POLL_MAX) return POLL_E_ARG;
    if (nfds == 0) {
        /* A poll of nothing is a sleep, and POSIX says so. Kept rather than
         * refused, because it is how a program waits on a timeout with the same
         * call it uses for everything else -- and poll_core() with n = 0 is
         * already exactly that sleep. */
        return poll_core(0, 0, (int)timeout_ms);
    }
    if (!u || !user_range_ok(u, sizeof *u * (uint64_t)nfds, 1)) return POLL_E_FAULT;

    struct proc *p = proc_current();
    if (!p) return POLL_E_ARG;

    struct logit_pollfd kfds[LOGIT_POLL_MAX];
    struct pollsrc      src[LOGIT_POLL_MAX];
    if (user_copy_from(kfds, u, sizeof *u * (uint64_t)nfds) < 0) return POLL_E_FAULT;

    for (long i = 0; i < nfds; i++) {
        src[i].events  = kfds[i].events;
        src[i].revents = 0;
        src[i].obj     = 0;
        src[i].ready   = 0;
        /* A NEGATIVE fd is SKIPPED, not refused: POSIX, and it is how a caller
         * disables one slot of a fixed-size array without rebuilding it. It
         * gets revents 0 and is never counted as ready -- which is why `ready`
         * is left NULL AND `events` is cleared, so it cannot even match an
         * unconditional LPOLLERR/LPOLLHUP. */
        if (kfds[i].fd < 0) { src[i].events = 0; continue; }
        struct file *f = proc_fd_get(p, kfds[i].fd);
        if (!f) continue;              /* ready == NULL -> poll_core says LPOLLNVAL */
        src[i].obj   = f;
        src[i].ready = src_file_ready;
    }

    /* A skipped (negative-fd) slot must not be reported LPOLLNVAL, so it needs
     * to be distinguishable from a bad fd. Both have ready == NULL, so mark the
     * skipped ones after poll_core has written its answers, not before. */
    int rc = poll_core(src, (int)nfds, (int)timeout_ms);
    if (rc < 0) return rc;

    int n = 0;
    for (long i = 0; i < nfds; i++) {
        short rev = src[i].revents;
        if (kfds[i].fd < 0) rev = 0;
        kfds[i].revents = rev;
        if (rev) n++;
    }
    if (user_copy_to(u, kfds, sizeof *u * (uint64_t)nfds) < 0) return POLL_E_FAULT;
    return n;
}

/* --- installing a freshly made description as a descriptor -----------------
 * Shared by eventfd and timerfd. On failure the description is closed rather
 * than leaked, and the fd slot is unhooked BEFORE the close for the reason
 * SYS_PIPE states in syscall.c: file_close drops the last reference, the slot
 * becomes reusable immediately, and a dangling p->fd[] entry would later close
 * whoever inherits the slot. */
static long install_fd(struct proc *p, struct file *f)
{
    if (!f) return POLL_E_NOMEM;
    int fd = proc_fd_alloc(p, f);
    if (fd < 0) { file_close(f); return POLL_E_NOMEM; }
    return fd;
}

long poll_syscall(long nr, long a, long b, long c)
{
    struct proc *p = proc_current();

    switch (nr) {
    case SYS_POLL:
        return sys_poll(a, b, c);

    case SYS_EVENTFD: {
        if (!p) return POLL_E_ARG;
        /* `a` is unsigned in the ABI; a negative long here is a caller that
         * passed something that is not an initial count, and silently
         * reinterpreting it as a huge counter would make read() return a number
         * nobody wrote. */
        if (a < 0) return POLL_E_ARG;
        return install_fd(p, file_eventfd((uint64_t)a, (int)b));
    }

    case SYS_TIMERFD: {
        if (!p) return POLL_E_ARG;
        struct logit_itimer it = { 0, 0 };
        const struct logit_itimer *uit = (const struct logit_itimer *)b;
        if (uit) {
            if (!user_range_ok(uit, sizeof it, 0)) return POLL_E_FAULT;
            if (user_copy_from(&it, uit, sizeof it) < 0) return POLL_E_FAULT;
        }
        /* fd < 0 CREATES; fd >= 0 RE-ARMS. One number for both, because there
         * were three left in the range this work was given -- argued in full at
         * SYS_TIMERFD in logit_abi.h. A descriptor is never negative, so the two
         * shapes cannot be confused by a caller with the arguments the wrong way
         * round. */
        if (a < 0) {
            struct file *f = file_timerfd((int)c);
            long fd = install_fd(p, f);
            if (fd < 0) return fd;
            if (uit && it.value_ms &&
                file_timerfd_arm(f, it.value_ms, it.interval_ms) < 0) {
                /* Arming failed after the fd was installed. Undo it whole
                 * rather than hand back a descriptor that will never fire: a
                 * timer that is silently never armed is indistinguishable from
                 * a poll() bug, which is the confusion this whole file is
                 * trying to avoid creating. */
                p->fd[fd] = 0;
                file_close(f);
                return POLL_E_ARG;
            }
            return fd;
        }
        struct file *f = proc_fd_get(p, (int)a);
        if (!f) return POLL_E_ARG;
        if (!uit) return POLL_E_ARG;      /* re-arm with nothing to arm from */
        return file_timerfd_arm(f, it.value_ms, it.interval_ms) < 0
               ? POLL_E_ARG : 0;
    }
    }
    return POLL_E_ARG;
}
