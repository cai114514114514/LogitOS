/* Host unit test for the M27 wait queues and sleeping locks
 * (c/kernel/core/wait.c, compiled against the stub headers in waitstub/).
 *
 * WHAT IS AND IS NOT PROVED HERE
 * ------------------------------
 * wait.c is portable C over two scheduler calls, so the queue logic, the FIFO
 * discipline, and the five state machines built on it all run natively -- under
 * real pthreads, so the contention is real. The park/unpark pair itself cannot
 * run on the host (it context-switches a kernel stack and hands off the BKL);
 * waitstub/sched.h supplies a faithful stand-in whose ONLY guarantee is the
 * kernel's ordering contract, with no wakeup token behind it. So a wakeup
 * delivered before a thread parks is genuinely lost in this harness, and any
 * test that passes here passes because wait.c ordered things correctly.
 *
 * THE NEGATIVE CONTROL
 * --------------------
 * Two of them, and both must fail for the suite to be trustworthy:
 *
 *   -DWAIT_NEGCTRL   inverts one line of the harness's park: the caller's lock
 *                    is released BEFORE the thread is marked parked, with the
 *                    window widened so the race is deterministic instead of
 *                    rare. This is the classic lost wakeup, and every phase that
 *                    depends on a wakeup surviving it must then FAIL (19 of the
 *                    41 checks, as of this writing).
 *   naive_cv_wait()  (always compiled) is a condition-variable wait written the
 *                    obvious wrong way -- drop the mutex first, then enqueue.
 *                    Phase 7 asserts that it loses signals AND that the real
 *                    cv_wait, in the same binary against the same producer,
 *                    loses none.
 *
 * Run: make test-wait  (or tests/unit/run-wait-tests.sh, which also runs the
 * negative control and requires it to fail).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "wait.h"
#include "pit.h"

/* --- the stub spinlock: the kernel's ticket algorithm, minus cli/sti (which
 * fault in ring 3) and with sched_yield in place of the `pause` hint. --- */
void spin_lock(spinlock_t *l)
{
    unsigned int my = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->serving, __ATOMIC_SEQ_CST) != my) sched_yield();
}
void spin_unlock(spinlock_t *l) { __atomic_fetch_add(&l->serving, 1, __ATOMIC_SEQ_CST); }
int spin_trylock(spinlock_t *l)
{
    unsigned int s = __atomic_load_n(&l->serving, __ATOMIC_SEQ_CST), e = s;
    return __atomic_compare_exchange_n(&l->ticket, &e, s + 1, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
uint64_t spin_lock_irqsave(spinlock_t *l) { spin_lock(l); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; spin_unlock(l); }

static int failures, checks;
#define CHECK(cond, ...) do {                                                  \
        checks++;                                                              \
        if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__);       \
                       printf("   (%s:%d)\n", __FILE__, __LINE__); }           \
    } while (0)

/* ------------------------------------------------------------------ clock -- */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t g_base_ns;
uint64_t timer_ticks(void) { return (now_ns() - g_base_ns) / (1000000000ull / TIMER_HZ); }
uint64_t timer_ms(void)    { return (now_ns() - g_base_ns) / 1000000ull; }

/* ------------------------------------------------- the park/unpark stand-in */
struct thread {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             parked;
    unsigned long   slices;      /* times this thread was unparked */
    int             id;
};

static __thread struct thread *g_self;
struct thread *sched_current_thread(void) { return g_self; }
unsigned long sched_slices_of(struct thread *t) { return t ? t->slices : 0; }

static void park(spinlock_t *outer, uint64_t flags, const struct timespec *abs, int *timed_out)
{
    struct thread *t = g_self;
    if (timed_out) *timed_out = 0;
#ifdef WAIT_NEGCTRL
    /* NEGATIVE CONTROL: release the caller's lock before becoming parked, and
     * widen the window so the race is not a matter of luck. Everything else is
     * identical. This is the one-line inversion the kernel's block_self()
     * exists to avoid. */
    spin_unlock(outer);
    (void)flags;
    usleep(2000);
    pthread_mutex_lock(&t->m);
    t->parked = 1;
#else
    /* FAITHFUL: mark parked, THEN release the caller's lock, while still holding
     * the per-thread lock a waker has to take. A waker that acquires `outer`
     * from here on cannot proceed past sched_wake() until we are fully parked --
     * which is exactly what the kernel achieves by holding g_sched_lock across
     * context_switch. */
    pthread_mutex_lock(&t->m);
    t->parked = 1;
    spin_unlock(outer);
    (void)flags;
#endif
    while (t->parked) {
        if (abs) {
            if (pthread_cond_timedwait(&t->c, &t->m, abs) == ETIMEDOUT) {
                t->parked = 0;
                if (timed_out) *timed_out = 1;
                break;
            }
        } else {
            pthread_cond_wait(&t->c, &t->m);
        }
    }
    pthread_mutex_unlock(&t->m);
}

void sched_block_self_unlock(spinlock_t *outer, uint64_t flags)
{
    park(outer, flags, NULL, NULL);
}

int sched_block_self_unlock_until(spinlock_t *outer, uint64_t flags, uint64_t deadline)
{
    /* deadline is in timer_ticks(); convert back to an absolute host time. */
    uint64_t ticks_now = timer_ticks();
    uint64_t remain_ms = (deadline > ticks_now)
                       ? (deadline - ticks_now) * (1000 / TIMER_HZ) : 0;
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec  += (time_t)(remain_ms / 1000);
    abs.tv_nsec += (long)((remain_ms % 1000) * 1000000L);
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    int to = 0;
    park(outer, flags, &abs, &to);
    return !to;
}

int sched_wake(struct thread *t)
{
    int did;
    if (!t) return 0;
    pthread_mutex_lock(&t->m);
    did = t->parked;
    if (did) { t->parked = 0; t->slices++; pthread_cond_signal(&t->c); }
    pthread_mutex_unlock(&t->m);
    return did;
}

/* ------------------------------------------------------------ thread helper */
/* Slots are never REUSED, only consumed. The negative-control build leaves
 * threads permanently parked on purpose; recycling a slot would re-initialise a
 * struct thread out from under a live sleeper and crash the control run before
 * it could report the failure it exists to report. */
#define MAXTH 64
static struct thread g_threads[MAXTH];
static pthread_t     g_pth[MAXTH];
static int           g_slot;        /* next free slot */
static int           g_first;       /* first slot of the current batch */
#define g_nth (g_slot - g_first)

struct spawn { void (*fn)(void *); void *arg; int id; };
static void *trampoline(void *p)
{
    struct spawn *s = p;
    g_self = &g_threads[s->id];
    s->fn(s->arg);
    return NULL;
}
static void spawn(void (*fn)(void *), void *arg)
{
    int id = g_slot++;
    struct spawn *s = malloc(sizeof *s);
    g_threads[id].id = id;
    g_threads[id].parked = 0;
    g_threads[id].slices = 0;
    pthread_mutex_init(&g_threads[id].m, NULL);
    pthread_cond_init(&g_threads[id].c, NULL);
    s->fn = fn; s->arg = arg; s->id = id;
    pthread_create(&g_pth[id], NULL, trampoline, s);
}
/* Join with a deadline: a lost wakeup shows up as a thread that never returns,
 * and a hung test says nothing. Returns 0 if every thread finished. */
static int join_all_timeout(int ms)
{
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += ms / 1000;
    abs.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    int stuck = 0;
    for (int i = g_first; i < g_slot; i++)
        if (pthread_timedjoin_np(g_pth[i], NULL, &abs) != 0) stuck++;
    /* Stuck threads are LEFT ALONE -- not detached. pthread_detach after a
     * failed timedjoin trips an assertion inside ThreadSanitizer's interceptor,
     * which aborted the negative-control run before it could print its verdict.
     * They stay parked until the process exits, which is all this needs. */
    g_first = g_slot;
    return stuck;
}

/* =========================================================== phase 1: queue */
static struct waitq  q1 = WAITQ_INIT;
static int           q1_parked;
static int           q1_order[8], q1_norder;

static void q1_waiter(void *arg)
{
    long me = (long)arg;
    struct waiter w;
    uint64_t f = spin_lock_irqsave(&q1.lock);
    waitq_enqueue(&q1, &w);
    /* Atomics on the bookkeeping counters, not because the lock is missing but
     * because the MAIN thread polls them without it -- and under ThreadSanitizer
     * an unsynchronised read is a race whether or not the value is benign. */
    __atomic_fetch_add(&q1_parked, 1, __ATOMIC_SEQ_CST);
    sched_block_self_unlock(&q1.lock, f);
    f = spin_lock_irqsave(&q1.lock);
    waitq_dequeue(&q1, &w);
    int slot = __atomic_fetch_add(&q1_norder, 1, __ATOMIC_SEQ_CST);
    if (slot < 8) q1_order[slot] = (int)me;
    spin_unlock_irqrestore(&q1.lock, f);
}

static void phase1_queue_order(void)
{
    /* Enqueue four waiters in a known order (one at a time, so the FIFO
     * position is not a matter of scheduling luck), then wake them one by one
     * and check that they come back in the order they arrived. */
    waitq_init(&q1);
    q1_parked = 0; q1_norder = 0;
    for (long i = 0; i < 4; i++) {
        spawn(q1_waiter, (void *)i);
        for (int spin = 0; spin < 20000; spin++) {
            if (__atomic_load_n(&q1_parked, __ATOMIC_SEQ_CST) == i + 1) break;
            usleep(100);
        }
    }
    int np = __atomic_load_n(&q1_parked, __ATOMIC_SEQ_CST);
    CHECK(np == 4, "phase1: not all four waiters enqueued (%d)", np);

    for (int i = 0; i < 4; i++) {
        int n = waitq_wake_one(&q1);
        CHECK(n == 1, "phase1: wake_one unparked %d threads, expected 1", n);
        usleep(20000);
        int no = __atomic_load_n(&q1_norder, __ATOMIC_SEQ_CST);
        CHECK(no == i + 1, "phase1: wake_one woke %d threads, expected %d",
              no, i + 1);
    }
    CHECK(join_all_timeout(2000) == 0, "phase1: a waiter never returned");
    int fifo = 1;
    for (int i = 0; i < 4; i++) if (q1_order[i] != i) fifo = 0;
    CHECK(fifo, "phase1: wake order was %d,%d,%d,%d, expected FIFO 0,1,2,3",
          q1_order[0], q1_order[1], q1_order[2], q1_order[3]);
}

/* ======================================================== phase 2: wake_all */
static struct waitq q2 = WAITQ_INIT;
static int          q2_parked, q2_woke;

static void q2_waiter(void *arg)
{
    (void)arg;
    struct waiter w;
    uint64_t f = spin_lock_irqsave(&q2.lock);
    waitq_enqueue(&q2, &w);
    __atomic_fetch_add(&q2_parked, 1, __ATOMIC_SEQ_CST);
    sched_block_self_unlock(&q2.lock, f);
    f = spin_lock_irqsave(&q2.lock);
    waitq_dequeue(&q2, &w);
    __atomic_fetch_add(&q2_woke, 1, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&q2.lock, f);
}

static void phase2_wake_all(void)
{
    waitq_init(&q2);
    q2_parked = 0; q2_woke = 0;
    for (int i = 0; i < 6; i++) spawn(q2_waiter, NULL);
    for (int spin = 0; spin < 20000 &&
         __atomic_load_n(&q2_parked, __ATOMIC_SEQ_CST) < 6; spin++) usleep(100);
    int p2 = __atomic_load_n(&q2_parked, __ATOMIC_SEQ_CST);
    CHECK(p2 == 6, "phase2: only %d of 6 parked", p2);
    CHECK(waitq_wake_one(&q2) == 1, "phase2: wake_one did not take exactly one");
    int n = waitq_wake_all(&q2);
    CHECK(n == 5, "phase2: wake_all unparked %d, expected the remaining 5", n);
    CHECK(join_all_timeout(2000) == 0, "phase2: wake_all left a waiter parked");
    CHECK(q2_woke == 6, "phase2: %d of 6 waiters returned", q2_woke);
    CHECK(waitq_wake_one(&q2) == 0, "phase2: wake on an empty queue reported a wake");
    CHECK(waitq_wake_all(&q2) == 0, "phase2: wake_all on an empty queue reported a wake");
}

/* ================================= phase 3: semaphore, the lost-wakeup hunt */
#define NPROD 4
#define NCONS 4
#define NITEM 2000
static struct semaphore s3;
static struct mutex     m3;
static int              s3_consumed;

static void s3_producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < NITEM; i++) {
        sem_post(&s3);
        if ((i & 63) == 0) usleep(50);   /* let consumers actually park */
    }
}
static void s3_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        if (!sem_wait_timeout(&s3, 2000)) return;      /* starved = lost wakeup */
        mutex_lock(&m3);
        /* Only count real items. The last consumer posts NCONS release tokens
         * below so its peers can leave; counting those would push the total
         * past the number produced and make a correct run look wrong. */
        if (s3_consumed < NPROD * NITEM) s3_consumed++;
        int done = (s3_consumed >= NPROD * NITEM);
        mutex_unlock(&m3);
        if (done) { /* release everyone else */
            for (int i = 0; i < NCONS; i++) sem_post(&s3);
            return;
        }
    }
}

static void phase3_semaphore(void)
{
    semaphore_init(&s3, 0);
    mutex_init(&m3);
    s3_consumed = 0;
    for (int i = 0; i < NCONS; i++) spawn(s3_consumer, NULL);
    for (int i = 0; i < NPROD; i++) spawn(s3_producer, NULL);
    CHECK(join_all_timeout(15000) == 0,
          "phase3: a consumer never returned -- A WAKEUP WAS LOST");
    CHECK(s3_consumed == NPROD * NITEM,
          "phase3: consumed %d of %d items", s3_consumed, NPROD * NITEM);

    /* sem_trywait must never invent a token. */
    semaphore_init(&s3, 2);
    CHECK(sem_trywait(&s3) == 1 && sem_trywait(&s3) == 1 && sem_trywait(&s3) == 0,
          "phase3: sem_trywait miscounted");
    /* A timed wait on an empty semaphore must time out, not hang or succeed. */
    uint64_t t0 = timer_ms();
    CHECK(sem_wait_timeout(&s3, 60) == 0, "phase3: timed wait on an empty sem succeeded");
    CHECK(timer_ms() - t0 >= 50, "phase3: timed wait returned early");
}

/* ================================================= phase 4: mutex exclusion */
static struct mutex m4;
static long         m4_shared;
static int          m4_overlap;      /* set if two threads were inside at once */
static volatile int m4_inside;

static void m4_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        mutex_lock(&m4);
        if (__atomic_fetch_add(&m4_inside, 1, __ATOMIC_SEQ_CST) != 0) m4_overlap = 1;
        long v = m4_shared;          /* deliberately non-atomic */
        m4_shared = v + 1;
        __atomic_fetch_sub(&m4_inside, 1, __ATOMIC_SEQ_CST);
        mutex_unlock(&m4);
    }
}

static void phase4_mutex(void)
{
    mutex_init(&m4);
    m4_shared = 0; m4_overlap = 0; m4_inside = 0;
    for (int i = 0; i < 6; i++) spawn(m4_worker, NULL);
    CHECK(join_all_timeout(20000) == 0, "phase4: a mutex worker never returned");
    CHECK(!m4_overlap, "phase4: two threads were inside the mutex at once");
    CHECK(m4_shared == 6L * 20000, "phase4: counter is %ld, expected %d",
          m4_shared, 6 * 20000);
    CHECK(mutex_trylock(&m4) == 1, "phase4: trylock failed on a free mutex");
    CHECK(mutex_trylock(&m4) == 0, "phase4: trylock succeeded on a held mutex");
    mutex_unlock(&m4);
    CHECK(mutex_trylock(&m4) == 1, "phase4: mutex was not released");
    mutex_unlock(&m4);
}

/* ===================================================== phase 5: condvar --- */
static struct mutex   m5;
static struct condvar c5;
static int            c5_data, c5_seen;

static void c5_consumer(void *arg)
{
    (void)arg;
    mutex_lock(&m5);
    while (c5_data == 0) cv_wait(&c5, &m5);
    c5_seen++;
    mutex_unlock(&m5);
}

static void phase5_condvar(void)
{
    mutex_init(&m5); condvar_init(&c5);
    c5_data = 0; c5_seen = 0;
    for (int i = 0; i < 4; i++) spawn(c5_consumer, NULL);
    usleep(150000);
    mutex_lock(&m5);
    c5_data = 1;
    mutex_unlock(&m5);
    cv_broadcast(&c5);
    CHECK(join_all_timeout(3000) == 0, "phase5: cv_broadcast left a waiter asleep");
    CHECK(c5_seen == 4, "phase5: %d of 4 waiters woke", c5_seen);

    /* cv_wait_timeout must return 0 (and re-acquire the mutex) when nobody signals. */
    mutex_lock(&m5);
    int sig = cv_wait_timeout(&c5, &m5, 60);
    CHECK(sig == 0, "phase5: cv_wait_timeout claimed a signal that never came");
    CHECK(mutex_trylock(&m5) == 0, "phase5: cv_wait_timeout did not re-take the mutex");
    mutex_unlock(&m5);
}

/* ====================================================== phase 6: rwlock --- */
static struct rwlock rw6;
static volatile int  rw6_readers, rw6_writers;
static int           rw6_violation;
static long          rw6_value;

static void rw6_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3000; i++) {
        read_lock_sleep(&rw6);
        __atomic_fetch_add(&rw6_readers, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&rw6_writers, __ATOMIC_SEQ_CST) != 0) rw6_violation = 1;
        __atomic_fetch_sub(&rw6_readers, 1, __ATOMIC_SEQ_CST);
        read_unlock_sleep(&rw6);
    }
}
static void rw6_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        write_lock_sleep(&rw6);
        if (__atomic_fetch_add(&rw6_writers, 1, __ATOMIC_SEQ_CST) != 0) rw6_violation = 1;
        if (__atomic_load_n(&rw6_readers, __ATOMIC_SEQ_CST) != 0) rw6_violation = 1;
        long v = rw6_value; rw6_value = v + 1;
        __atomic_fetch_sub(&rw6_writers, 1, __ATOMIC_SEQ_CST);
        write_unlock_sleep(&rw6);
    }
}

static void phase6_rwlock(void)
{
    rwlock_init(&rw6);
    rw6_readers = rw6_writers = 0; rw6_violation = 0; rw6_value = 0;
    for (int i = 0; i < 4; i++) spawn(rw6_reader, NULL);
    for (int i = 0; i < 2; i++) spawn(rw6_writer, NULL);
    CHECK(join_all_timeout(30000) == 0, "phase6: an rwlock thread never returned");
    CHECK(!rw6_violation, "phase6: a writer overlapped a reader or another writer");
    CHECK(rw6_value == 2000, "phase6: writer count is %ld, expected 2000", rw6_value);
}

/* ============ phase 7: the in-binary negative control (naive cv_wait) ===== */
/* The obvious wrong way to write cv_wait: drop the mutex first, then enqueue.
 * A signal that lands in the gap finds an empty queue and is lost. The usleep
 * makes the gap wide enough that the outcome is not a matter of luck -- the
 * point is to show this test detects the bug, not to hope it might. */
static void naive_cv_wait(struct condvar *c, struct mutex *m)
{
    struct waiter w;
    mutex_unlock(m);                          /* <-- the bug: gap starts here */
    usleep(30000);
    uint64_t f = spin_lock_irqsave(&c->wq.lock);
    waitq_enqueue(&c->wq, &w);
    sched_block_self_unlock(&c->wq.lock, f);  /* gap ends here */
    f = spin_lock_irqsave(&c->wq.lock);
    waitq_dequeue(&c->wq, &w);
    spin_unlock_irqrestore(&c->wq.lock, f);
    mutex_lock(m);
}

static struct mutex   m7;
static struct condvar c7;
static int            c7_flag, c7_naive;

static void c7_waiter(void *arg)
{
    (void)arg;
    mutex_lock(&m7);
    while (!c7_flag) {
        if (c7_naive) naive_cv_wait(&c7, &m7);
        else          cv_wait(&c7, &m7);
    }
    mutex_unlock(&m7);
}

/* Returns 1 if the waiter was still asleep after the signal (wakeup lost).
 *
 * The stuck waiter is fully released and JOINED before returning. Detaching it
 * instead -- the first version of this -- left the naive run's thread alive
 * inside m7/c7 while the next run re-initialised them, which corrupted the real
 * cv_wait's queue and reported a lost signal that had not happened. */
static int cv_signal_race(int naive)
{
    mutex_init(&m7); condvar_init(&c7);
    c7_flag = 0; c7_naive = naive;
    spawn(c7_waiter, NULL);
    pthread_t th = g_pth[g_slot - 1];
    usleep(5000);                 /* waiter is inside the wait, mid-gap if naive */
    mutex_lock(&m7);
    c7_flag = 1;
    mutex_unlock(&m7);
    cv_signal(&c7);               /* lands in the naive version's gap */

    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += 2;
    int stuck = (pthread_timedjoin_np(th, NULL, &abs) != 0);
    if (stuck) {
        /* c7_flag is already 1, so a broadcast is enough to let it finish. */
        cv_broadcast(&c7);
        pthread_join(th, NULL);
    }
    g_first = g_slot;
    return stuck;
}

static void phase7_negative_control(void)
{
    int lost_naive = cv_signal_race(1);
    CHECK(lost_naive,
          "phase7 NEGATIVE CONTROL DID NOT FIRE: the deliberately wrong cv_wait "
          "kept its wakeup, so this suite cannot detect a lost wakeup");
    int lost_real = cv_signal_race(0);
    CHECK(!lost_real, "phase7: the REAL cv_wait lost a signal");
}

/* ========================================================== phase 8: sleep */
static void phase8_sleep_ms(void)
{
    uint64_t t0 = timer_ms();
    sched_sleep_ms(120);
    uint64_t dt = timer_ms() - t0;
    CHECK(dt >= 110, "phase8: sched_sleep_ms(120) returned after %llums",
          (unsigned long long)dt);
    CHECK(dt < 600, "phase8: sched_sleep_ms(120) overslept (%llums)",
          (unsigned long long)dt);
    /* Rounding: a deadline must never be in the past. */
    CHECK(!wait_deadline_passed(wait_deadline_ms(0)),
          "phase8: a zero-length deadline was already expired");
}

int main(void)
{
    g_base_ns = now_ns();
    static struct thread mainth;
    pthread_mutex_init(&mainth.m, NULL);
    pthread_cond_init(&mainth.c, NULL);
    mainth.id = -1;
    g_self = &mainth;

#ifdef WAIT_NEGCTRL
    printf("wait_test: NEGATIVE CONTROL build -- the park releases the caller's\n"
           "           lock before parking. Phases 3-7 are EXPECTED to fail.\n");
#endif

    phase1_queue_order();
    phase2_wake_all();
    phase3_semaphore();
    phase4_mutex();
    phase5_condvar();
    phase6_rwlock();
    phase7_negative_control();
    phase8_sleep_ms();

    printf("wait_test: %d checks, %d failures\n", checks, failures);
#ifdef WAIT_NEGCTRL
    if (failures == 0) {
        printf("NEGATIVE CONTROL FAILED TO FAIL: inverting the park ordering "
               "changed nothing, so the suite proves nothing.\n");
        return 1;
    }
    printf("negative control behaved as required (%d failures)\n", failures);
    return 0;
#else
    if (failures) return 1;
    printf("wait_test: OK\n");
    return 0;
#endif
}
