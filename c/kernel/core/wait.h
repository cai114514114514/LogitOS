#ifndef LOGIT_WAIT_H
#define LOGIT_WAIT_H

#include <stdint.h>
#include "spinlock.h"
#include "sched.h"

/* ===========================================================================
 * M27 -- waiting without spinning.
 *
 * Before this file every "wait" in the kernel was a spin or a poll: net_poll()
 * pumped from the window manager's main loop, device I/O run "interrupts on but
 * non-preemptible", blocking reads calling bkl_hlt_wait() in a loop. None of
 * those are waits; they are the absence of one.
 *
 * The stack is:
 *
 *   sched_block_self_unlock()/sched_wake()      park + unpark      (sched.c)
 *     -> waitq                                  the queue          (this file)
 *          -> mutex / semaphore / condvar / rwlock
 *          -> workqueue                                            (work.c)
 *
 * LOCKING DISCIPLINE (the same three rules for every primitive here)
 *
 *  1. Lock order is always  <the caller's lock>  ->  wq->lock  ->  g_sched_lock.
 *     A waker takes wq->lock and calls sched_wake() inside it; a sleeper takes
 *     wq->lock and hands it to sched_block_self_unlock(), which takes
 *     g_sched_lock inside it. Nothing here ever takes a lock in the other order,
 *     so there is no AB-BA cycle to find.
 *
 *  2. The condition MUST be evaluated under the SAME lock that is handed to the
 *     sleep. That is what closes the lost wakeup (see the long comment in
 *     sched.h). wait_event() enforces it by construction: it takes wq->lock and
 *     evaluates `cond` with the lock held. If your condition is guarded by a
 *     different lock, use a condvar (cv_wait releases the caller's mutex at the
 *     one point where it is safe) -- do not hand-roll it.
 *
 *  3. Blocking is a THREAD-context operation. Never from an interrupt handler,
 *     never while holding a spinlock other than the one handed to the sleep,
 *     never from the idle thread. Waking is safe from anywhere, including
 *     interrupt context -- that asymmetry is the whole point of work.c.
 *
 * WHAT STILL DEPENDS ON THE BKL: a sleeper drops the BKL across the switch and
 * re-acquires it on resume, exactly like schedule(). So these primitives are
 * usable today from any BKL-holding kernel context, and they are already correct
 * for a BKL-free one -- they never consult g_bkl themselves, only the park/unpark
 * pair does, in one place.
 * =========================================================================== */

struct waiter {
    struct thread *thread;
    struct waiter *next;
    int            woken;      /* set by the waker under wq->lock */
    int            queued;
};

struct waitq {
    spinlock_t     lock;
    struct waiter *head;       /* FIFO: wake-one takes the longest waiter */
    struct waiter *tail;
    unsigned long  wakes;      /* diagnostics */
};

#define WAITQ_INIT { SPINLOCK_INIT, 0, 0, 0 }

/* Initialise a queue in FRESH memory. It assigns a whole new lock, so calling
 * it on a queue anything might be using resets that lock's ticket and serving
 * to zero -- and a core queued on it right then holds a number that will never
 * be served again, which is an unbounded spin on a lock that reads as free. To
 * recycle a queue whose owner is going away, waitq_wake_all() instead: the
 * sleepers re-test their predicate and leave. */
void waitq_init(struct waitq *q);

/* Enqueue the current thread. Caller MUST already hold q->lock. */
void waitq_enqueue(struct waitq *q, struct waiter *w);
/* Remove `w` if still queued. Caller MUST already hold q->lock. */
void waitq_dequeue(struct waitq *q, struct waiter *w);

/* Wake the longest-waiting thread / every waiter. Take q->lock themselves, so
 * they are callable from interrupt context and from any core. Return the number
 * of threads actually unparked. */
int waitq_wake_one(struct waitq *q);
int waitq_wake_all(struct waitq *q);

/* Sleep until `cond` becomes true. `cond` is re-evaluated under q->lock on every
 * pass, which is rule 2 above: a waker that makes `cond` true must then take
 * q->lock (waitq_wake_*) and therefore cannot slip between our test and our
 * park. Spurious wakes are handled by the loop, so this is safe to use with a
 * queue several unrelated conditions share. */
#define wait_event(q, cond)                                                    \
    do {                                                                       \
        struct waitq *__q = (q);                                               \
        uint64_t __f = spin_lock_irqsave(&__q->lock);                          \
        while (!(cond)) {                                                      \
            struct waiter __w;                                                 \
            waitq_enqueue(__q, &__w);                                          \
            sched_block_self_unlock(&__q->lock, __f);                          \
            __f = spin_lock_irqsave(&__q->lock);                               \
            waitq_dequeue(__q, &__w);                                          \
        }                                                                      \
        spin_unlock_irqrestore(&__q->lock, __f);                               \
    } while (0)

/* Same, but gives up after `ms` milliseconds. Sets `okv` to 1 if `cond` held on
 * return, 0 on timeout. */
#define wait_event_timeout(q, cond, ms, okv)                                   \
    do {                                                                       \
        struct waitq *__q = (q);                                               \
        uint64_t __dl = wait_deadline_ms(ms);                                  \
        uint64_t __f = spin_lock_irqsave(&__q->lock);                          \
        (okv) = 1;                                                             \
        while (!(cond)) {                                                      \
            struct waiter __w;                                                 \
            if (wait_deadline_passed(__dl)) { (okv) = 0; break; }              \
            waitq_enqueue(__q, &__w);                                          \
            sched_block_self_unlock_until(&__q->lock, __f, __dl);              \
            __f = spin_lock_irqsave(&__q->lock);                               \
            waitq_dequeue(__q, &__w);                                          \
        }                                                                      \
        spin_unlock_irqrestore(&__q->lock, __f);                               \
    } while (0)

uint64_t wait_deadline_ms(unsigned ms);       /* timer_ticks() + ms, rounded up */
int      wait_deadline_passed(uint64_t dl);
void     sched_sleep_ms(unsigned ms);         /* park for `ms`; zero slices meanwhile */

/* --- mutex: a sleeping lock. Use it where a critical section can be long or can
 * itself block (I/O, allocation). spinlock_t stays the right answer for short
 * sections and for anything an interrupt handler touches. --- */
struct mutex {
    struct waitq   wq;
    struct thread *owner;      /* NULL = free */
    int            locked;
};
#define MUTEX_INIT { WAITQ_INIT, 0, 0 }
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
int  mutex_trylock(struct mutex *m);          /* 1 = acquired, never sleeps */
void mutex_unlock(struct mutex *m);

/* --- counting semaphore: the natural shape for "a device produced N things". --- */
struct semaphore {
    struct waitq wq;
    int          count;
};
#define SEMAPHORE_INIT(n) { WAITQ_INIT, (n) }
void semaphore_init(struct semaphore *s, int count);
void sem_wait(struct semaphore *s);                     /* P */
int  sem_wait_timeout(struct semaphore *s, unsigned ms);/* 1 = got it, 0 = timeout */
int  sem_trywait(struct semaphore *s);
void sem_post(struct semaphore *s);                     /* V -- interrupt-safe */

/* --- condition variable, over a mutex. cv_wait releases `m` at the one point
 * where releasing it cannot lose a signal: after the caller is enqueued on the
 * cv and while the cv's own lock is still held, so a cv_signal from the thread
 * that just took `m` blocks on the cv lock until the sleeper is parked. --- */
struct condvar { struct waitq wq; };
#define CONDVAR_INIT { WAITQ_INIT }
void condvar_init(struct condvar *c);
void cv_wait(struct condvar *c, struct mutex *m);
int  cv_wait_timeout(struct condvar *c, struct mutex *m, unsigned ms);
void cv_signal(struct condvar *c);
void cv_broadcast(struct condvar *c);

/* --- reader/writer lock. Writer-preferring: a waiting writer stops new readers,
 * so a steady stream of readers cannot starve it.
 *
 * ONE waitqueue on purpose. Readers and writers wait on different predicates but
 * both predicates read the same counters, and rule 2 says the predicate must be
 * evaluated under the lock handed to the sleep -- two queues would mean two
 * locks and a predicate visible to neither. Both sides park on q and re-test
 * under q->lock; every state change does wake_all. The cost is a thundering herd
 * proportional to the number of waiters, which for a kernel rwlock is small. --- */
struct rwlock {
    struct waitq q;            /* q.lock guards every field below */
    int          readers;      /* active readers */
    int          writer;       /* 1 while a writer holds it */
    int          waiting_w;    /* writers waiting (new readers defer to them) */
};
#define RWLOCK_INIT { WAITQ_INIT, 0, 0, 0 }
void rwlock_init(struct rwlock *l);
void read_lock_sleep(struct rwlock *l);
void read_unlock_sleep(struct rwlock *l);
void write_lock_sleep(struct rwlock *l);
void write_unlock_sleep(struct rwlock *l);

#endif /* LOGIT_WAIT_H */
