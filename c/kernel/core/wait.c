/* M27 -- wait queues and the sleeping locks built on them.
 *
 * Read the header first: it states the three locking rules every primitive here
 * obeys, and sched.h states why a wakeup cannot be lost. This file contains no
 * cleverness of its own; it is the queue plus five small state machines.
 *
 * Every waiter is a `struct waiter` on the SLEEPING THREAD'S OWN KERNEL STACK.
 * That is not a shortcut, it is the reason none of this allocates: the waiter
 * lives exactly as long as the frame that is blocked, and a thread cannot be on
 * a queue while it is not blocked (waitq_dequeue runs before the frame returns,
 * under the same lock the waker uses).
 */

#include <stddef.h>
#include "wait.h"
#include "pit.h"

void waitq_init(struct waitq *q)
{
    spinlock_t z = SPINLOCK_INIT;
    q->lock = z;
    q->head = NULL;
    q->tail = NULL;
    q->wakes = 0;
}

/* FIFO append. Caller holds q->lock. */
void waitq_enqueue(struct waitq *q, struct waiter *w)
{
    w->thread = sched_current_thread();
    w->next   = NULL;
    w->woken  = 0;
    w->queued = 1;
    if (q->tail) q->tail->next = w; else q->head = w;
    q->tail = w;
}

/* Caller holds q->lock. Idempotent: a waiter the waker already popped has
 * queued == 0 and this does nothing. */
void waitq_dequeue(struct waitq *q, struct waiter *w)
{
    struct waiter **pp = &q->head;
    struct waiter *prev = NULL;
    if (!w->queued) return;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            if (q->tail == w) q->tail = prev;
            break;
        }
        prev = *pp;
        pp = &(*pp)->next;
    }
    w->queued = 0;
    w->next   = NULL;
}

/* Pop the head waiter. Caller holds q->lock. */
static struct waiter *waitq_pop(struct waitq *q)
{
    struct waiter *w = q->head;
    if (!w) return NULL;
    q->head = w->next;
    if (!q->head) q->tail = NULL;
    w->next   = NULL;
    w->queued = 0;
    w->woken  = 1;
    return w;
}

int waitq_wake_one(struct waitq *q)
{
    int n = 0;
    uint64_t f = spin_lock_irqsave(&q->lock);
    struct waiter *w = waitq_pop(q);
    if (w) { n = sched_wake(w->thread); q->wakes++; }
    spin_unlock_irqrestore(&q->lock, f);
    return n;
}

int waitq_wake_all(struct waitq *q)
{
    int n = 0;
    uint64_t f = spin_lock_irqsave(&q->lock);
    struct waiter *w;
    while ((w = waitq_pop(q)) != NULL) { n += sched_wake(w->thread); q->wakes++; }
    spin_unlock_irqrestore(&q->lock, f);
    return n;
}

/* --- deadlines ---------------------------------------------------------- */

/* timer_ticks() runs at TIMER_HZ, so the granularity is 1000/TIMER_HZ ms. Round
 * UP and add one tick: a "10 ms" sleep must never return early, which a
 * truncating conversion plus an about-to-fire tick would make it do. */
uint64_t wait_deadline_ms(unsigned ms)
{
    uint64_t ticks = ((uint64_t)ms * TIMER_HZ + 999) / 1000;
    return timer_ticks() + ticks + 1;
}

int wait_deadline_passed(uint64_t dl) { return (int64_t)(timer_ticks() - dl) >= 0; }

/* Park for `ms`, consuming no scheduler time meanwhile -- the replacement for
 * every `while (timer_ticks() < end) ;` in the tree. */
void sched_sleep_ms(unsigned ms)
{
    static struct waitq idle_q = WAITQ_INIT;   /* nobody ever wakes this queue:
                                                * the deadline is the only exit */
    uint64_t dl = wait_deadline_ms(ms);
    while (!wait_deadline_passed(dl)) {
        struct waiter w;
        uint64_t f = spin_lock_irqsave(&idle_q.lock);
        waitq_enqueue(&idle_q, &w);
        sched_block_self_unlock_until(&idle_q.lock, f, dl);
        f = spin_lock_irqsave(&idle_q.lock);
        waitq_dequeue(&idle_q, &w);
        spin_unlock_irqrestore(&idle_q.lock, f);
    }
}

/* --- mutex --------------------------------------------------------------- */

void mutex_init(struct mutex *m)
{
    waitq_init(&m->wq);
    m->owner = NULL;
    m->locked = 0;
}

void mutex_lock(struct mutex *m)
{
    uint64_t f = spin_lock_irqsave(&m->wq.lock);
    while (m->locked) {
        struct waiter w;
        waitq_enqueue(&m->wq, &w);
        sched_block_self_unlock(&m->wq.lock, f);
        f = spin_lock_irqsave(&m->wq.lock);
        waitq_dequeue(&m->wq, &w);
    }
    m->locked = 1;
    m->owner  = sched_current_thread();
    spin_unlock_irqrestore(&m->wq.lock, f);
}

int mutex_trylock(struct mutex *m)
{
    int got = 0;
    uint64_t f = spin_lock_irqsave(&m->wq.lock);
    if (!m->locked) { m->locked = 1; m->owner = sched_current_thread(); got = 1; }
    spin_unlock_irqrestore(&m->wq.lock, f);
    return got;
}

/* Hand-off is "wake one, let it race": the woken thread re-tests m->locked under
 * m->wq.lock, so an unrelated thread that grabbed the mutex in between simply
 * puts it back to sleep. No ownership transfer, hence no lost hand-off to lose. */
void mutex_unlock(struct mutex *m)
{
    uint64_t f = spin_lock_irqsave(&m->wq.lock);
    m->locked = 0;
    m->owner  = NULL;
    struct waiter *w = waitq_pop(&m->wq);
    if (w) { sched_wake(w->thread); m->wq.wakes++; }
    spin_unlock_irqrestore(&m->wq.lock, f);
}

/* --- semaphore ----------------------------------------------------------- */

void semaphore_init(struct semaphore *s, int count)
{
    waitq_init(&s->wq);
    s->count = count;
}

void sem_wait(struct semaphore *s)
{
    uint64_t f = spin_lock_irqsave(&s->wq.lock);
    while (s->count <= 0) {
        struct waiter w;
        waitq_enqueue(&s->wq, &w);
        sched_block_self_unlock(&s->wq.lock, f);
        f = spin_lock_irqsave(&s->wq.lock);
        waitq_dequeue(&s->wq, &w);
    }
    s->count--;
    spin_unlock_irqrestore(&s->wq.lock, f);
}

int sem_wait_timeout(struct semaphore *s, unsigned ms)
{
    uint64_t dl = wait_deadline_ms(ms);
    int got = 0;
    uint64_t f = spin_lock_irqsave(&s->wq.lock);
    for (;;) {
        if (s->count > 0) { s->count--; got = 1; break; }
        if (wait_deadline_passed(dl)) break;
        struct waiter w;
        waitq_enqueue(&s->wq, &w);
        sched_block_self_unlock_until(&s->wq.lock, f, dl);
        f = spin_lock_irqsave(&s->wq.lock);
        waitq_dequeue(&s->wq, &w);
    }
    spin_unlock_irqrestore(&s->wq.lock, f);
    return got;
}

int sem_trywait(struct semaphore *s)
{
    int got = 0;
    uint64_t f = spin_lock_irqsave(&s->wq.lock);
    if (s->count > 0) { s->count--; got = 1; }
    spin_unlock_irqrestore(&s->wq.lock, f);
    return got;
}

/* THE interrupt-context primitive: a completion handler calls this and returns.
 * It takes one spinlock, touches two words and unparks at most one thread; it
 * never sleeps and never touches the BKL. This is what a driver ISR should call
 * instead of setting a flag for a poll loop to find. */
void sem_post(struct semaphore *s)
{
    uint64_t f = spin_lock_irqsave(&s->wq.lock);
    s->count++;
    struct waiter *w = waitq_pop(&s->wq);
    if (w) { sched_wake(w->thread); s->wq.wakes++; }
    spin_unlock_irqrestore(&s->wq.lock, f);
}

/* --- condition variable --------------------------------------------------- */

void condvar_init(struct condvar *c) { waitq_init(&c->wq); }

/* The whole point of a condvar is this function's ordering. We enqueue on the cv
 * FIRST, then drop the caller's mutex, then park handing over the cv's own lock.
 * A signaller that takes the mutex, sets the condition and calls cv_signal has
 * to acquire c->wq.lock, which we hold until we are parked -- so its signal
 * either arrives before we enqueued (and the caller's `while` re-test sees the
 * condition) or after we are queued and parked. It cannot land in between. */
void cv_wait(struct condvar *c, struct mutex *m)
{
    struct waiter w;
    uint64_t f = spin_lock_irqsave(&c->wq.lock);
    waitq_enqueue(&c->wq, &w);
    mutex_unlock(m);                      /* c->wq.lock -> m->wq.lock, never the reverse */
    sched_block_self_unlock(&c->wq.lock, f);
    f = spin_lock_irqsave(&c->wq.lock);
    waitq_dequeue(&c->wq, &w);
    spin_unlock_irqrestore(&c->wq.lock, f);
    mutex_lock(m);
}

int cv_wait_timeout(struct condvar *c, struct mutex *m, unsigned ms)
{
    struct waiter w;
    uint64_t dl = wait_deadline_ms(ms);
    uint64_t f = spin_lock_irqsave(&c->wq.lock);
    waitq_enqueue(&c->wq, &w);
    mutex_unlock(m);
    int signalled = sched_block_self_unlock_until(&c->wq.lock, f, dl);
    f = spin_lock_irqsave(&c->wq.lock);
    waitq_dequeue(&c->wq, &w);
    spin_unlock_irqrestore(&c->wq.lock, f);
    mutex_lock(m);
    return signalled;
}

void cv_signal(struct condvar *c)    { waitq_wake_one(&c->wq); }
void cv_broadcast(struct condvar *c) { waitq_wake_all(&c->wq); }

/* --- reader/writer lock --------------------------------------------------- */

void rwlock_init(struct rwlock *l)
{
    waitq_init(&l->q);
    l->readers = 0;
    l->writer = 0;
    l->waiting_w = 0;
}

void read_lock_sleep(struct rwlock *l)
{
    uint64_t f = spin_lock_irqsave(&l->q.lock);
    while (l->writer || l->waiting_w) {         /* defer to writers: no starvation */
        struct waiter w;
        waitq_enqueue(&l->q, &w);
        sched_block_self_unlock(&l->q.lock, f);
        f = spin_lock_irqsave(&l->q.lock);
        waitq_dequeue(&l->q, &w);
    }
    l->readers++;
    spin_unlock_irqrestore(&l->q.lock, f);
}

void read_unlock_sleep(struct rwlock *l)
{
    uint64_t f = spin_lock_irqsave(&l->q.lock);
    int last = (--l->readers == 0);
    struct waiter *w, *list = NULL;
    if (last) while ((w = waitq_pop(&l->q)) != NULL) { w->next = list; list = w; }
    spin_unlock_irqrestore(&l->q.lock, f);
    while (list) { struct waiter *n = list->next; sched_wake(list->thread); list = n; }
}

void write_lock_sleep(struct rwlock *l)
{
    uint64_t f = spin_lock_irqsave(&l->q.lock);
    l->waiting_w++;
    while (l->writer || l->readers) {
        struct waiter w;
        waitq_enqueue(&l->q, &w);
        sched_block_self_unlock(&l->q.lock, f);
        f = spin_lock_irqsave(&l->q.lock);
        waitq_dequeue(&l->q, &w);
    }
    l->waiting_w--;
    l->writer = 1;
    spin_unlock_irqrestore(&l->q.lock, f);
}

void write_unlock_sleep(struct rwlock *l)
{
    uint64_t f = spin_lock_irqsave(&l->q.lock);
    l->writer = 0;
    struct waiter *w, *list = NULL;
    while ((w = waitq_pop(&l->q)) != NULL) { w->next = list; list = w; }
    spin_unlock_irqrestore(&l->q.lock, f);
    /* Wake outside the lock: every woken thread immediately wants l->q.lock. */
    while (list) { struct waiter *n = list->next; sched_wake(list->thread); list = n; }
}
