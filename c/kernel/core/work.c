/* M27 deferred work: softirqs, tasklets and a sleeping workqueue.
 * See work.h for what each tier is for. This file is deliberately small; the
 * interesting property is not the queues but WHERE they run.
 */

#include <stddef.h>
#include "work.h"
#include "wait.h"
#include "sched.h"
#include "spinlock.h"
#include "percpu.h"
#include "kprintf.h"

/* --- softirq -------------------------------------------------------------
 * Pending bits are PER-CPU and live here rather than in struct cpu: percpu.h's
 * struct cpu is compiled into a dozen other translation units, and the Makefile
 * header already records what a stale object with a different struct cpu layout
 * costs (the M25 P4 g_cpus skew). A separate array has no such coupling. */
static void (*g_softirq[NR_SOFTIRQ])(void);
static volatile unsigned int g_pending[PERCPU_MAXCPU];
static volatile int g_in_softirq[PERCPU_MAXCPU];
static volatile unsigned long g_softirq_runs;

void softirq_register(int nr, void (*fn)(void))
{
    if (nr >= 0 && nr < NR_SOFTIRQ) g_softirq[nr] = fn;
}

/* Lock-free on purpose: an ISR must be able to raise without taking anything.
 * The bit is set on THIS core, and this core is the one that will run it on the
 * way out of the interrupt. */
void softirq_raise(int nr)
{
    if (nr < 0 || nr >= NR_SOFTIRQ) return;
    __atomic_fetch_or(&g_pending[this_cpu()->index], 1u << nr, __ATOMIC_SEQ_CST);
}

/* Run this core's pending softirqs. Called from interrupts.c just before the BKL
 * is released, i.e. still in interrupt context with the BKL held. Handlers must
 * not sleep -- there is a live interrupt frame under us.
 *
 * Bounded to MAXROUNDS: a handler that re-raises itself must not be able to hold
 * a core in the interrupt tail forever. Leftover work stays pending and runs on
 * the next interrupt (the timer guarantees one within 10 ms). */
void softirq_run_pending(void)
{
    int cpu = this_cpu()->index;
    if (g_in_softirq[cpu]) return;               /* no re-entry from a nested IRQ */
    g_in_softirq[cpu] = 1;
    for (int round = 0; round < 8; round++) {
        unsigned int p = __atomic_exchange_n(&g_pending[cpu], 0, __ATOMIC_SEQ_CST);
        if (!p) break;
        for (int i = 0; i < NR_SOFTIRQ; i++)
            if ((p & (1u << i)) && g_softirq[i]) { g_softirq[i](); g_softirq_runs++; }
    }
    g_in_softirq[cpu] = 0;
}

unsigned long softirq_runs(void) { return g_softirq_runs; }

/* --- tasklet -------------------------------------------------------------
 * One global list rather than per-CPU: the serialization guarantee ("a tasklet
 * never runs concurrently with itself") is what its users rely on, and a global
 * list plus a `running` flag gives it on every core for free. */
static spinlock_t     g_tasklet_lock = SPINLOCK_INIT;
static struct tasklet *g_tasklets;      /* FIFO head */
static struct tasklet *g_tasklets_tail;

void tasklet_init(struct tasklet *t, void (*fn)(void *), void *arg)
{
    t->fn = fn; t->arg = arg; t->next = NULL; t->queued = 0; t->running = 0;
}

void tasklet_schedule(struct tasklet *t)
{
    uint64_t f = spin_lock_irqsave(&g_tasklet_lock);
    if (!t->queued) {
        t->queued = 1;
        t->next = NULL;
        if (g_tasklets_tail) g_tasklets_tail->next = t; else g_tasklets = t;
        g_tasklets_tail = t;
    }
    spin_unlock_irqrestore(&g_tasklet_lock, f);
    softirq_raise(SOFTIRQ_TASKLET);
}

static void tasklet_action(void)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&g_tasklet_lock);
        struct tasklet *t = g_tasklets;
        /* Skip (and re-queue behind) a tasklet another core is already running:
         * that is the serialization guarantee. */
        while (t && t->running) t = t->next;
        if (!t) { spin_unlock_irqrestore(&g_tasklet_lock, f); return; }
        /* unlink t */
        if (g_tasklets == t) {
            g_tasklets = t->next;
            if (!g_tasklets) g_tasklets_tail = NULL;
        } else {
            struct tasklet *p = g_tasklets;
            while (p && p->next != t) p = p->next;
            if (p) { p->next = t->next; if (g_tasklets_tail == t) g_tasklets_tail = p; }
        }
        t->next = NULL;
        t->queued = 0;
        t->running = 1;
        spin_unlock_irqrestore(&g_tasklet_lock, f);

        t->fn(t->arg);

        f = spin_lock_irqsave(&g_tasklet_lock);
        t->running = 0;
        spin_unlock_irqrestore(&g_tasklet_lock, f);
    }
}

/* --- workqueue ------------------------------------------------------------
 * The tier that may sleep, because it runs on a real thread. work_q.lock guards
 * the list AND is the lock kworker's wait_event() evaluates its condition under
 * -- rule 2 of the locking discipline, which is what makes "queue an item from
 * an ISR" unable to lose the wakeup that goes with it. */
static struct waitq   work_q = WAITQ_INIT;
static struct work   *work_head;
static struct work   *work_tail;
static volatile unsigned long g_work_run;

void work_item_init(struct work *w, void (*fn)(void *), void *arg)
{
    w->fn = fn; w->arg = arg; w->next = NULL; w->queued = 0;
}

int work_queue(struct work *w)
{
    int added = 0;
    uint64_t f = spin_lock_irqsave(&work_q.lock);
    if (!w->queued) {
        w->queued = 1;
        w->next = NULL;
        if (work_tail) work_tail->next = w; else work_head = w;
        work_tail = w;
        added = 1;
    }
    spin_unlock_irqrestore(&work_q.lock, f);
    if (added) waitq_wake_one(&work_q);
    return added;
}

unsigned long work_items_run(void) { return g_work_run; }

static void kworker_main(void)
{
    for (;;) {
        struct work *w = NULL;
        /* Park until there is something to do. Between items the kworker holds
         * nothing, so an item is free to sleep. */
        wait_event(&work_q, work_head != NULL);
        {
            uint64_t f = spin_lock_irqsave(&work_q.lock);
            w = work_head;
            if (w) {
                work_head = w->next;
                if (!work_head) work_tail = NULL;
                w->next = NULL;
                w->queued = 0;
            }
            spin_unlock_irqrestore(&work_q.lock, f);
        }
        if (w && w->fn) { w->fn(w->arg); g_work_run++; }
    }
}

void work_init(void)
{
    waitq_init(&work_q);
    softirq_register(SOFTIRQ_TASKLET, tasklet_action);
    thread_create(kworker_main, "kworker");
}
