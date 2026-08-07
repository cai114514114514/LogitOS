/* M27 wait/wake self-test -- runs at boot, on the real scheduler, under -smp.
 *
 * This class of bug does not appear on one core or without contention, so
 * nothing here is a mock: these are real kernel threads, really preempted by the
 * timer, really parked and really woken from other cores.
 *
 * Six phases, each with a deadline so a broken primitive FAILS rather than
 * hangs:
 *   1  wake-one wakes exactly one waiter, and it is the longest-waiting one
 *   2  wake-all wakes every remaining waiter
 *   3  semaphore producer/consumer across cores -- the lost-wakeup hunt: every
 *      post must be matched by exactly one wait, N times, with no item stranded
 *   4  mutex mutual exclusion: N threads x M non-atomic increments must total
 *      exactly N*M
 *   5  IRQ -> softirq -> tasklet -> workqueue -> a work item that SLEEPS
 *   6  the measurement: the same 200 ms wait expressed as a spin and as a sleep,
 *      reported as dispatch counts
 *
 * Prints WAITQ_SELFTEST_OK / WAITQ_SELFTEST_FAIL on serial.
 */

#include <stddef.h>
#include "wait_selftest.h"
#include "wait.h"
#include "work.h"
#include "sched.h"
#include "spinlock.h"
#include "kprintf.h"
#include "pit.h"

#define NWAIT   4          /* phase 1/2 waiters */
#define NPROD   3          /* phase 3 producers */
#define NCONS   3          /* phase 3 consumers */
#define NITEM   64         /* items per producer */
#define NMUTEX  4          /* phase 4 threads */
#define MITER   250        /* increments per phase-4 thread */
#define MEAS_MS 200        /* phase 6 wait length */

/* ---- phase 1/2: raw waitqueue ---- */
static struct waitq  p12_q      = WAITQ_INIT;
static volatile int  p12_parked;              /* waiters currently enqueued */
static volatile int  p12_woke;                /* waiters that have returned */
static volatile int  p12_order[NWAIT];        /* wake order, by waiter index */
static volatile int  p12_norder;
static volatile int  p12_next_idx;

/* ---- phase 3: semaphore ---- */
static struct semaphore p3_items = SEMAPHORE_INIT(0);
static struct mutex     p3_mtx   = MUTEX_INIT;
static volatile int     p3_consumed;
static volatile int     p3_prod_done;
static volatile int     p3_cons_done;

/* ---- phase 4: mutex ---- */
static struct mutex  p4_mtx = MUTEX_INIT;
static volatile long p4_shared;
static volatile int  p4_done;

/* ---- phase 5: deferred work ---- */
static struct tasklet   p5_tasklet;
static struct work      p5_work;
static struct semaphore p5_done = SEMAPHORE_INIT(0);
static volatile int     p5_slept;

/* ---- phase 6: measurement ---- */
static struct semaphore p6_go     = SEMAPHORE_INIT(0);
static volatile int     p6_flag;
static volatile unsigned long p6_spin_iters;
static struct thread   *p6_spin_thread;
static struct thread   *p6_sleep_thread;
static volatile unsigned long p6_sleep_slices_0, p6_sleep_slices_1;
static volatile unsigned long p6_spin_slices_0, p6_spin_slices_1;
static volatile int     p6_sleep_ready, p6_spin_ready, p6_done;

static int g_fail;
static void check(int ok, const char *what)
{
    if (!ok) { g_fail++; kprintf("[waitq] FAIL: %s\n", what); }
}

/* Spin-free bounded wait on a plain volatile flag, used only by the coordinator
 * to sequence phases. Sleeps in 10 ms steps, so it costs no scheduler time. */
static int await_flag(volatile int *v, int want, unsigned ms)
{
    uint64_t dl = wait_deadline_ms(ms);
    while (*v < want) {
        if (wait_deadline_passed(dl)) return 0;
        sched_sleep_ms(5);
    }
    return 1;
}

/* ------------------------------------------------------------------ phase 1/2 */
static void p12_waiter(void)
{
    struct waiter w;
    int me = __atomic_fetch_add(&p12_next_idx, 1, __ATOMIC_SEQ_CST);
    /* Stagger so the enqueue order is deterministic and FIFO is testable. */
    sched_sleep_ms(20 * (unsigned)(me + 1));

    uint64_t f = spin_lock_irqsave(&p12_q.lock);
    waitq_enqueue(&p12_q, &w);
    p12_parked++;
    /* The park itself: `p12_q.lock` is handed to the scheduler and released only
     * once this thread is off the run ring -- so a waitq_wake_one() racing us
     * here waits on g_sched_lock and cannot miss us. */
    sched_block_self_unlock(&p12_q.lock, f);
    f = spin_lock_irqsave(&p12_q.lock);
    waitq_dequeue(&p12_q, &w);
    if (p12_norder < NWAIT) p12_order[p12_norder++] = me;
    p12_woke++;
    spin_unlock_irqrestore(&p12_q.lock, f);
}

/* -------------------------------------------------------------------- phase 3 */
static void p3_producer(void)
{
    for (int i = 0; i < NITEM; i++) sem_post(&p3_items);
    __atomic_fetch_add(&p3_prod_done, 1, __ATOMIC_SEQ_CST);
}

static void p3_consumer(void)
{
    int mine = 0;
    for (;;) {
        /* A timeout here is what turns "the test hangs" into "the test fails":
         * a lost wakeup shows up as a consumer that never returns. */
        if (!sem_wait_timeout(&p3_items, 3000)) break;
        mine++;
        mutex_lock(&p3_mtx);
        p3_consumed++;
        int total = p3_consumed;
        mutex_unlock(&p3_mtx);
        if (total >= NPROD * NITEM) break;
    }
    (void)mine;
    __atomic_fetch_add(&p3_cons_done, 1, __ATOMIC_SEQ_CST);
}

/* -------------------------------------------------------------------- phase 4 */
static void p4_worker(void)
{
    for (int i = 0; i < MITER; i++) {
        mutex_lock(&p4_mtx);
        long v = p4_shared;          /* read-modify-write, deliberately not atomic:
                                      * only real mutual exclusion makes the total
                                      * come out exactly right on 4 cores */
        p4_shared = v + 1;
        mutex_unlock(&p4_mtx);
    }
    __atomic_fetch_add(&p4_done, 1, __ATOMIC_SEQ_CST);
}

/* -------------------------------------------------------------------- phase 5 */
static void p5_work_fn(void *arg)
{
    (void)arg;
    /* The property being proved: this runs on a THREAD, so it may sleep. A
     * softirq or tasklet doing this would wedge the interrupt tail. */
    sched_sleep_ms(20);
    p5_slept = 1;
    sem_post(&p5_done);
}

static void p5_tasklet_fn(void *arg)
{
    (void)arg;
    work_queue(&p5_work);            /* softirq context -> sleepable context */
}

/* -------------------------------------------------------------------- phase 6 */
static void p6_spinner(void)
{
    p6_spin_thread = sched_current_thread();
    p6_spin_slices_0 = sched_slices_of(p6_spin_thread);
    p6_spin_ready = 1;
    /* The kernel's PRE-M27 best available wait: drop the BKL, halt until any
     * interrupt, re-acquire, re-test. Correct, and it wakes on every interrupt
     * whether or not it has anything to do with this condition. */
    while (!p6_flag) { p6_spin_iters++; bkl_hlt_wait(); }
    p6_spin_slices_1 = sched_slices_of(p6_spin_thread);
    __atomic_fetch_add(&p6_done, 1, __ATOMIC_SEQ_CST);
}

static void p6_sleeper(void)
{
    p6_sleep_thread = sched_current_thread();
    p6_sleep_slices_0 = sched_slices_of(p6_sleep_thread);
    p6_sleep_ready = 1;
    sem_wait(&p6_go);                /* parked: off the run ring entirely */
    p6_sleep_slices_1 = sched_slices_of(p6_sleep_thread);
    __atomic_fetch_add(&p6_done, 1, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------ the coordinator */
static void selftest_main(void)
{
    /* Let the APs finish coming up, so everything below really is cross-core. */
    sched_sleep_ms(300);

    /* --- phase 1: wake-one --- */
    for (int i = 0; i < NWAIT; i++) thread_create(p12_waiter, "waitq_w");
    check(await_flag(&p12_parked, NWAIT, 3000), "phase1: waiters never parked");
    int n1 = waitq_wake_one(&p12_q);
    check(n1 == 1, "phase1: wake_one did not unpark exactly one");
    check(await_flag(&p12_woke, 1, 2000), "phase1: woken waiter never ran");
    sched_sleep_ms(30);
    check(p12_woke == 1, "phase1: wake_one woke more than one");
    check(p12_norder >= 1 && p12_order[0] == 0, "phase1: wake_one was not FIFO");

    /* --- phase 2: wake-all --- */
    int n2 = waitq_wake_all(&p12_q);
    check(n2 == NWAIT - 1, "phase2: wake_all count wrong");
    check(await_flag(&p12_woke, NWAIT, 3000), "phase2: wake_all left a waiter parked");

    /* --- phase 3: semaphore across cores --- */
    for (int i = 0; i < NPROD; i++) thread_create(p3_producer, "waitq_p");
    for (int i = 0; i < NCONS; i++) thread_create(p3_consumer, "waitq_c");
    check(await_flag(&p3_prod_done, NPROD, 6000), "phase3: producers stalled");
    check(await_flag(&p3_cons_done, NCONS, 6000), "phase3: a consumer LOST A WAKEUP");
    check(p3_consumed == NPROD * NITEM, "phase3: item count wrong");

    /* --- phase 4: mutex --- */
    for (int i = 0; i < NMUTEX; i++) thread_create(p4_worker, "waitq_m");
    check(await_flag(&p4_done, NMUTEX, 8000), "phase4: mutex workers stalled");
    check(p4_shared == (long)NMUTEX * MITER, "phase4: mutex did not exclude");

    /* --- phase 5: IRQ tail -> tasklet -> workqueue -> sleeping work item --- */
    tasklet_init(&p5_tasklet, p5_tasklet_fn, NULL);
    work_item_init(&p5_work, p5_work_fn, NULL);
    tasklet_schedule(&p5_tasklet);
    check(sem_wait_timeout(&p5_done, 4000), "phase5: deferred work never completed");
    check(p5_slept == 1, "phase5: work item did not run in a sleepable context");

    /* --- phase 6: the measurement ---
     * Both threads wait for the same event for the same length of time; the
     * difference is only HOW they wait. The numbers are sampled TWICE while both
     * are still waiting, MEAS_MS apart, so the comparison is a rate and no
     * capture-versus-park race can contaminate it: a genuinely parked thread's
     * dispatch count cannot move between the two samples, at all, ever. */
    thread_create(p6_spinner, "waitq_spin");
    thread_create(p6_sleeper, "waitq_sleep");
    check(await_flag(&p6_spin_ready, 1, 2000) && await_flag(&p6_sleep_ready, 1, 2000),
          "phase6: measurement threads never started");
    sched_sleep_ms(50);                       /* let both settle into their wait */
    unsigned long spin_a  = sched_slices_of(p6_spin_thread);
    unsigned long sleep_a = sched_slices_of(p6_sleep_thread);
    unsigned long iters_a = p6_spin_iters;
    sched_sleep_ms(MEAS_MS);
    unsigned long spin_b  = sched_slices_of(p6_spin_thread);
    unsigned long sleep_b = sched_slices_of(p6_sleep_thread);
    unsigned long iters_b = p6_spin_iters;
    p6_flag = 1;
    sem_post(&p6_go);
    check(await_flag(&p6_done, 2, 4000), "phase6: measurement threads did not finish");

    unsigned long spin_disp  = spin_b - spin_a;
    unsigned long sleep_disp = sleep_b - sleep_a;
    unsigned long spin_wakes = iters_b - iters_a;
    check(sleep_disp == 0, "phase6: a PARKED thread was still being dispatched");
    /* The baseline's cost is the number of times it WOKE UP TO RE-TEST, not its
     * dispatch count: on a machine with more cores than runnable threads the
     * spinner keeps its own core across every hlt, so it re-tests ~100x/s while
     * being dispatched zero times (observed at -smp 8). Both numbers are
     * printed; only the re-test count is asserted. */
    check(spin_wakes > 0,  "phase6: the spin baseline did not re-test");
    check(p6_sleep_slices_1 - p6_sleep_slices_0 <= 3,
          "phase6: sleeper cost far more than its single wake");

    /* %u/%d only: the console formatter's `l` length modifier is not portable
     * across every checkout of this tree right now, and every number here fits
     * an unsigned int. */
    kprintf("[waitq] %u ms of waiting: bkl_hlt_wait spin re-tested %u times / "
            "%u dispatches; blocked sleeper %u dispatches\n",
            (unsigned)MEAS_MS, (unsigned)spin_wakes, (unsigned)spin_disp,
            (unsigned)sleep_disp);
    kprintf("[waitq] sem items=%d mutex=%d/%d softirqs=%u work=%u blocked_now=%u\n",
            p3_consumed, (int)p4_shared, NMUTEX * MITER,
            (unsigned)softirq_runs(), (unsigned)work_items_run(),
            (unsigned)sched_blocked_count());
    kprintf(g_fail ? "\nWAITQ_SELFTEST_FAIL (%d)\n" : "\nWAITQ_SELFTEST_OK (%d failures)\n",
            g_fail);
}

void wait_selftest_start(void)
{
    thread_create(selftest_main, "waitchk");
}
