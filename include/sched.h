#ifndef AQUA_SCHED_H
#define AQUA_SCHED_H

/* Cooperative-capable, preemptive round-robin kernel thread scheduler. */

void sched_init(void);                                  /* adopt the boot context as "main" */
void thread_create(void (*entry)(void), const char *name);
void schedule(void);                                    /* switch to the next ready thread */

#endif /* AQUA_SCHED_H */
