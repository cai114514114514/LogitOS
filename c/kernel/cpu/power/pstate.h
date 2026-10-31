#ifndef LOGIT_PSTATE_H
#define LOGIT_PSTATE_H
int pstate_init(void);
void pstate_set_ratio(unsigned cpu, unsigned ratio);
/* Per-CPU timer integration: load is 0..100 percent. Must execute locally
 * with preemption disabled, as must init and the local setter. Remote setters
 * publish requests; the target consumes them here (no remote WRMSR fiction). */
void pstate_tick(unsigned load);
#endif
