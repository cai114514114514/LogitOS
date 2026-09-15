#ifndef LOGIT_WAIT_SELFTEST_H
#define LOGIT_WAIT_SELFTEST_H

/* Boot-time proof that the M27 blocking core works under real preemption and
 * real SMP. Spawned from sched_init() as an ordinary ring-0 thread, so boot does
 * not wait for it; it runs alongside the desktop coming up, prints one block of
 * serial output and exits. Every phase is bounded by a deadline, so a broken
 * primitive prints WAITQ_SELFTEST_FAIL instead of hanging the machine. */
void wait_selftest_start(void);

#endif /* LOGIT_WAIT_SELFTEST_H */
