/* poll() -- the core. Read kpoll.h first: it states the two contracts a
 * pollable backend must meet and why the hook, not the syscall, is the design.
 *
 * THIS FILE KNOWS NOTHING ABOUT DESCRIPTORS. It takes an array of `struct
 * pollsrc` -- an object plus the function that can answer for it -- and does
 * three things with them: register on every wait queue they name, read their
 * readiness, and sleep if none of them had any. The fd table, the user pointer,
 * the copy in and out and the eventfd/timerfd backends live in kpollsys.c and
 * file.c, on the other side of that boundary.
 *
 * THE BOUNDARY IS THERE FOR THE GATE, and that is worth saying plainly rather
 * than presenting it as taste. file.c cannot be compiled for the host: it needs
 * kheap, vfs, serial, percpu and the big kernel lock. A poll core reachable
 * only through file.c would be testable only by booting QEMU, which means the
 * one property that matters here -- that an event arriving between the
 * readiness check and the sleep is not lost -- would be tested by hoping the
 * race happens rather than by causing it. tests/unit/poll_test.c drives THIS
 * file, unmodified, with a model object whose readiness function injects the
 * event at exactly the instruction where it hurts. See the header of that file.
 */

#include <stdint.h>
#include "kpoll.h"
#include "sched.h"
#include "pit.h"

/* EINTR. Declared weak here rather than by including ksignal.h, for the same
 * reason file.c declares the lsock hooks weak: this TU is linked into a host
 * test binary that has no signal delivery at all, and a hard reference would
 * make it fail to link over a facility it never uses. NULL means "this build
 * cannot be interrupted", which is the truth in that build. */
int ksig_interrupted(void) __attribute__((weak));

/* --------------------------------------------------------------------------
 * Registration.
 *
 * Every entry is enqueued with waitq_enqueue_hook(), which is what makes the
 * waker publish into `pt->triggered` under `pt->lock` before it unparks us.
 * The enqueue happens under the QUEUE's lock, so a waker can never observe a
 * half-initialised waiter -- there is no window in which the waiter is on the
 * list with a NULL hook.
 * ------------------------------------------------------------------------ */

void poll_wait(struct poll_table *pt, struct waitq *q)
{
    if (!pt || !q) return;                 /* NULL pt = "just report your mask" */
    if (pt->n >= POLL_MAXWAIT) { pt->overflow = 1; return; }
    struct poll_ent *e = &pt->e[pt->n];
    e->q = q;
    uint64_t f = spin_lock_irqsave(&q->lock);
    waitq_enqueue_hook(q, &e->w, &pt->lock, &pt->triggered);
    spin_unlock_irqrestore(&q->lock, f);
    pt->n++;
}

/* Take every registration back off its queue. Idempotent per entry:
 * waitq_dequeue() does nothing to a waiter a waker already popped, and it is
 * called under the same q->lock the waker holds -- which is also what makes it
 * safe for `pt` to be a stack frame that is about to go away. */
static void poll_unregister(struct poll_table *pt)
{
    for (int i = 0; i < pt->n; i++) {
        struct waitq *q = pt->e[i].q;
        uint64_t f = spin_lock_irqsave(&q->lock);
        waitq_dequeue(q, &pt->e[i].w);
        pt->e[i].w.xlock = 0;
        pt->e[i].w.xflag = 0;
        spin_unlock_irqrestore(&q->lock, f);
    }
    pt->n = 0;
}

/* --------------------------------------------------------------------------
 * The wait.
 *
 * One pass is: publish "not triggered", register on everything, read everyone's
 * readiness, and -- only if nothing was ready -- park. A wake, a timeout or a
 * signal starts the next pass.
 *
 * WHY `triggered` CAN BE RESET AT THE TOP OF EACH PASS without losing an event.
 * Anything that set it did so AFTER publishing the state that caused it (a
 * pipe's write puts the bytes in the ring and only then calls waitq_wake_all).
 * So an event whose flag is cleared here is an event whose state the scan two
 * lines below will see. The flag is a wakeup, not a queue of them; it never has
 * to count.
 *
 * WHY THE RESET IS UNDER pt->lock. It is a plain int written by wakers on other
 * cores. Clearing it outside the lock would be a torn read-modify-write against
 * a waker in flight, and -- worse -- would let the compiler sink the store past
 * the registration below, which is the one ordering this file depends on.
 * ------------------------------------------------------------------------ */

int poll_core(struct pollsrc *src, int n, int timeout_ms)
{
    struct poll_table pt;
    spinlock_t z = SPINLOCK_INIT;
    pt.lock = z;
    pt.triggered = 0;
    pt.n = 0;
    pt.overflow = 0;

    uint64_t dl = 0;
    int have_dl = timeout_ms > 0;
    if (have_dl) dl = wait_deadline_ms((unsigned)timeout_ms);

    for (;;) {
        uint64_t f = spin_lock_irqsave(&pt.lock);
        pt.triggered = 0;
        spin_unlock_irqrestore(&pt.lock, f);
        pt.n = 0;
        pt.overflow = 0;

        int ready = 0;

#ifdef POLL_NO_PREREGISTER
        /* THE NEGATIVE CONTROL, and the only thing it changes is the ORDER.
         *
         * Read everyone's readiness with no registration (pt = NULL), decide
         * from that whether to sleep, and register afterwards. Every mask is
         * still computed from real state, every queue is still registered on
         * before the park, and every ordinary case still passes -- which is
         * exactly why this bug ships: it is invisible unless an event lands in
         * the window between the two loops. tests/unit/poll_test.c puts one
         * there deliberately. Built by `make test-poll-negctl`. */
        for (int i = 0; i < n; i++) {
            src[i].revents = 0;
            if (!src[i].ready) { src[i].revents = LPOLLNVAL; ready++; continue; }
            short m = src[i].ready(src[i].obj, 0);
            m &= (short)(src[i].events | LPOLLERR | LPOLLHUP | LPOLLNVAL);
            if (m) { src[i].revents = m; ready++; }
        }
        if (!ready)
            for (int i = 0; i < n; i++)
                if (src[i].ready) (void)src[i].ready(src[i].obj, &pt);
#else
        for (int i = 0; i < n; i++) {
            src[i].revents = 0;
            /* A source with no answering function is not a source: file.c uses
             * this for a descriptor that is not open, and LPOLLNVAL is an
             * ANSWER -- the caller is told the fd is dead instead of being
             * parked forever on something that can never become ready. */
            if (!src[i].ready) { src[i].revents = LPOLLNVAL; ready++; continue; }

            /* The backend calls poll_wait() BEFORE it reads its own state.
             * That order is the correctness argument; see kpoll.h. */
            short m = src[i].ready(src[i].obj, &pt);

            /* ERR, HUP and NVAL are reported whether or not they were asked
             * for -- POSIX, and the reason a poll loop over a closed pipe
             * terminates instead of spinning on a request that can never be
             * satisfied. */
            m &= (short)(src[i].events | LPOLLERR | LPOLLHUP | LPOLLNVAL);
            if (m) { src[i].revents = m; ready++; }
        }
#endif

        if (ready) { poll_unregister(&pt); return ready; }

        /* A registration table that overflowed means somebody is about to sleep
         * without being on a queue that can wake them. Refuse, loudly. */
        if (pt.overflow) { poll_unregister(&pt); return POLL_E_NOMEM; }

        if (timeout_ms == 0) { poll_unregister(&pt); return 0; }
        if (have_dl && wait_deadline_passed(dl)) { poll_unregister(&pt); return 0; }

        /* EINTR is checked AFTER the scan, on purpose: a signal that arrives
         * alongside real readiness must not throw the readiness away. POSIX
         * agrees, and the alternative loses data on a fd the caller will not be
         * told to re-read. */
        if (ksig_interrupted && ksig_interrupted()) {
            poll_unregister(&pt);
            return SIG_E_INTR;
        }

        f = spin_lock_irqsave(&pt.lock);
        if (pt.triggered) {
            /* Something fired while we were scanning. Do not park -- go round
             * and look again. This branch IS the lost wakeup, closed. */
            spin_unlock_irqrestore(&pt.lock, f);
        } else if (have_dl) {
            sched_block_self_unlock_until(&pt.lock, f, dl);
        } else {
            sched_block_self_unlock(&pt.lock, f);
        }
        poll_unregister(&pt);
    }
}
