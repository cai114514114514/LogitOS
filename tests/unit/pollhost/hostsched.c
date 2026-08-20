/* The machine underneath the poll host test.
 *
 * WHAT IS REAL IN THIS BUILD AND WHAT IS NOT, because that is the only thing
 * that decides what the gate is worth:
 *
 *   REAL, compiled from the tree, unmodified:
 *       c/kernel/core/wait.c   -- the wait queues, the poll hook, wake_waiter
 *       c/kernel/exec/kpoll.c  -- poll_wait, poll_core, the registration order
 *   REAL, the same algorithm rather than a stand-in:
 *       the ticket spinlock. spin_lock/spin_unlock here are the same
 *       fetch-add/compare loop c/kernel/cpu/spinlock.c runs, over the same
 *       struct, so lock ORDER bugs and missed releases fail here too.
 *   MODELLED:
 *       park and unpark. A kernel thread becomes a pthread and a context switch
 *       becomes a condition variable.
 *
 * THE MODEL IS FAITHFUL IN THE ONE RESPECT THAT MATTERS, and it was written to
 * be. c/kernel/sched/sched.c's wake_locked() does NOTHING to a thread that is
 * not already THREAD_BLOCKED -- there is no pending-wakeup bit anywhere in that
 * file -- which is exactly why a multi-queue sleeper can lose a wakeup at all.
 * hsched_wake() below reproduces that: it returns 0 and changes nothing if the
 * target has not parked yet. A model that quietly remembered the wake would
 * make -DPOLL_NO_PREREGISTER pass, and the gate would be measuring the model.
 *
 * And block_self()'s ordering is reproduced too: `outer` is released only after
 * the caller is marked blocked and while the lock a waker must take (there,
 * g_sched_lock; here, the thread's own mutex) is still held. That is the
 * property the single-queue sleepers in wait.c depend on, so the mutex, the
 * semaphore and the condvar behave here as they do on the machine.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

/* NOT #include <sched.h>. The host include path for this gate carries
 * -Ic/kernel/sched, and an angle-bracket include searches -I directories too --
 * so <sched.h> resolves to the KERNEL's scheduler header and the build fails
 * somewhere else entirely. That is the same flat-INCDIRS trap CLAUDE.md records
 * for sys/wait.h and sched.h, met from the host side, and the kernel header is
 * exactly the one this file needs to see. One declaration is cheaper than
 * reordering the include path around it. */
extern int sched_yield(void);

#include "spinlock.h"
#include "pit.h"

/* --- the ticket spinlock, verbatim in algorithm ------------------------- */

spinlock_t g_bkl = SPINLOCK_INIT;
volatile int g_bkl_owner = -1;

void spin_lock(spinlock_t *l)
{
    unsigned t = __atomic_fetch_add(&l->ticket, 1u, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->serving, __ATOMIC_SEQ_CST) != t) sched_yield();
    l->owner_ra  = (unsigned long)__builtin_return_address(0);
    l->owner_cpu = 0;
}

void spin_unlock(spinlock_t *l)
{
    l->owner_cpu = -1;
    __atomic_fetch_add(&l->serving, 1u, __ATOMIC_SEQ_CST);
}

/* IF does not exist here, so irqsave is the plain lock. Kept as its own pair
 * rather than a #define so that a caller mixing the two still compiles the way
 * the kernel does. */
uint64_t spin_lock_irqsave(spinlock_t *l) { spin_lock(l); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; spin_unlock(l); }
int  spin_trylock(spinlock_t *l)
{
    unsigned s = __atomic_load_n(&l->serving, __ATOMIC_SEQ_CST);
    unsigned t = __atomic_load_n(&l->ticket,  __ATOMIC_SEQ_CST);
    if (s != t) return 0;
    return __atomic_compare_exchange_n(&l->ticket, &t, t + 1, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* --- the clock ---------------------------------------------------------- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t g_t0;

/* TIMER_HZ is 100, so one tick is 10 ms -- the same granularity the machine
 * has, which matters because wait_deadline_ms() rounds up to a tick and adds
 * one. A host clock with millisecond ticks would make every timeout in this
 * test 10x tighter than the real one and would turn a correct implementation
 * into a flaky gate. */
uint64_t timer_ticks(void) { return (mono_ms() - g_t0) * TIMER_HZ / 1000u; }
uint64_t timer_ms(void)    { return mono_ms() - g_t0; }
uint64_t timer_ns_per_tick(void) { return 1000000000ull / TIMER_HZ; }

/* --- threads: park and unpark ------------------------------------------- */

struct thread {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int             blocked;
    int             timed_out;
    const char     *name;
};

static __thread struct thread *g_self;

struct thread *sched_current_thread(void)
{
    if (!g_self) {
        g_self = calloc(1, sizeof *g_self);
        pthread_mutex_init(&g_self->m, NULL);
        pthread_cond_init(&g_self->cv, NULL);
        g_self->name = "anon";
    }
    return g_self;
}

/* The exact semantics of wake_locked(): a thread that is not parked is not
 * woken, and NOTHING is remembered. Returns 1 iff this call did the unparking,
 * as sched_wake() does. */
int sched_wake(struct thread *t)
{
    if (!t) return 0;
    pthread_mutex_lock(&t->m);
    int did = t->blocked;
    if (did) { t->blocked = 0; pthread_cond_signal(&t->cv); }
    pthread_mutex_unlock(&t->m);
    return did;
}

int sched_wake_id(int id) { (void)id; return 0; }

static void hblock(spinlock_t *outer, uint64_t flags, uint64_t deadline, int *timed_out)
{
    struct thread *self = sched_current_thread();
    int to = 0;

    pthread_mutex_lock(&self->m);
    self->blocked = 1;
    /* THE ORDERING. `outer` goes only now: after we are marked blocked and
     * while self->m -- the lock every waker must take -- is still held. A waker
     * that acquires `outer` from here on therefore blocks on self->m until we
     * are inside pthread_cond_wait, and can only ever observe a parked thread.
     * This is block_self()'s comment, one lock renamed. */
    spin_unlock_irqrestore(outer, flags);

    while (self->blocked) {
        if (deadline) {
            /* `deadline` is in ticks of our own clock. Convert back to an
             * absolute wall time for the condvar. */
            uint64_t now_t = timer_ticks();
            long ms = deadline > now_t
                    ? (long)((deadline - now_t) * 1000u / TIMER_HZ) : 0;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += ms / 1000;
            ts.tv_nsec += (ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            int rc = pthread_cond_timedwait(&self->cv, &self->m, &ts);
            if (rc == ETIMEDOUT && self->blocked) { self->blocked = 0; to = 1; }
        } else {
            pthread_cond_wait(&self->cv, &self->m);
        }
    }
    pthread_mutex_unlock(&self->m);
    if (timed_out) *timed_out = to;
}

void sched_block_self_unlock(spinlock_t *outer, uint64_t flags)
{
    hblock(outer, flags, 0, NULL);
}

int sched_block_self_unlock_until(spinlock_t *outer, uint64_t flags, uint64_t deadline)
{
    int to = 0;
    hblock(outer, flags, deadline ? deadline : 1, &to);
    return !to;
}

void hostsched_init(void) { g_t0 = mono_ms(); (void)sched_current_thread(); }
