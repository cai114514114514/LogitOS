#ifndef LOGIT_WORK_H
#define LOGIT_WORK_H

#include <stdint.h>

/* ===========================================================================
 * M27 deferred work -- the other half of "waiting without spinning".
 *
 * An interrupt handler cannot block, cannot allocate, and must be short. Until
 * now this kernel had no way for one to say "finish this later": a handler
 * either did everything itself inside the IRQ (e1000_irq drains the whole RX
 * ring in interrupt context) or it did nothing and left the work for the window
 * manager's main loop to notice on its next pass (net_poll). Those look like
 * two different designs but they are the same missing mechanism.
 *
 * Three tiers, cheapest first:
 *
 *   softirq   - a numbered handler run at the tail of the interrupt that raised
 *               it, still in interrupt context, still holding the BKL. Cannot
 *               sleep. For work that must happen soon and is bounded: draining a
 *               ring, completing a descriptor.
 *   tasklet   - a one-shot item run on the softirq tier. Never runs concurrently
 *               with itself even on different cores, so its handler needs no
 *               lock of its own. Cannot sleep.
 *   work item - run by the `kworker` kernel THREAD. MAY SLEEP: it can take a
 *               mutex, wait on a semaphore, do blocking I/O. This is the tier an
 *               interrupt hands work to when the work needs to wait for
 *               something.
 *
 * All three take caller-allocated items and never allocate, because the caller
 * is often an interrupt handler.
 * =========================================================================== */

/* --- softirq ------------------------------------------------------------- */
#define SOFTIRQ_TASKLET   0
#define SOFTIRQ_NET       1     /* reserved for the net line: raise from the NIC ISR */
#define SOFTIRQ_BLOCK     2     /* reserved for the block lines */
#define NR_SOFTIRQ        4

void softirq_register(int nr, void (*fn)(void));
void softirq_raise(int nr);        /* interrupt-safe, lock-free, idempotent */
void softirq_run_pending(void);    /* called by interrupts.c on kernel exit */
unsigned long softirq_runs(void);

/* --- tasklet ------------------------------------------------------------- */
struct tasklet {
    void (*fn)(void *);
    void  *arg;
    struct tasklet *next;
    int    queued;
    int    running;
};
void tasklet_init(struct tasklet *t, void (*fn)(void *), void *arg);
void tasklet_schedule(struct tasklet *t);   /* interrupt-safe; a second schedule
                                             * while queued is a no-op */

/* --- workqueue ----------------------------------------------------------- */
struct work {
    void (*fn)(void *);
    void  *arg;
    struct work *next;
    int    queued;
};
void work_init(void);                       /* creates the kworker thread */
void work_item_init(struct work *w, void (*fn)(void *), void *arg);
int  work_queue(struct work *w);            /* interrupt-safe. 1 = queued,
                                             * 0 = already queued */
unsigned long work_items_run(void);

#endif /* LOGIT_WORK_H */
