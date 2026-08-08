#ifndef LOGIT_WAIT_STUB_H
#define LOGIT_WAIT_STUB_H

/* Host stub for c/kernel/core/wait.h, so tcp.c's passive-open path compiles
 * into the white-box unit test.
 *
 * The real header is a scheduler interface: it parks a thread on a queue and
 * another context wakes it. On the host there is no scheduler and, more to the
 * point, NOTHING TO WAKE US -- the test IS the peer: it calls tcp_input()
 * itself, synchronously, from the same thread that would be doing the waiting.
 * So a faithful stub of wait_event_timeout is one that evaluates the condition
 * and returns; it cannot sleep, and a sleep here would be a hang, not a wait.
 *
 * That means the host suite tests the LISTENER STATE MACHINE and never the
 * parking. The parking is proved on the device instead (tests/boot/
 * run-httpd-test.sh), which is the only place a second thread exists to prove
 * it with. Said out loud because a stub that quietly no-ops a primitive is the
 * kind of thing that makes a suite look like it covers more than it does. */

#include <stdint.h>

typedef int spinlock_t;
#define SPINLOCK_INIT 0

static inline uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{ (void)l; (void)f; }

struct waiter { int unused; };

struct waitq {
    spinlock_t    lock;
    struct waiter *head, *tail;
    unsigned long wakes;
};
#define WAITQ_INIT { 0, 0, 0, 0 }

static inline void waitq_init(struct waitq *q)
{ q->lock = 0; q->head = q->tail = 0; q->wakes = 0; }

static inline int waitq_wake_one(struct waitq *q) { q->wakes++; return 0; }
static inline int waitq_wake_all(struct waitq *q) { q->wakes++; return 0; }

/* Evaluate once. See the note above: there is no other thread to make the
 * condition true, so looping would loop forever. */
#define wait_event(q, cond)          do { (void)(q); (void)(cond); } while (0)
#define wait_event_timeout(q, cond, ms, okv) \
    do { (void)(q); (void)(ms); (okv) = (cond) ? 1 : 0; } while (0)

#endif /* LOGIT_WAIT_STUB_H */
