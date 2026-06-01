#ifndef AQUA_SCHED_H
#define AQUA_SCHED_H

#include <stdint.h>

/* Cooperative-capable, preemptive round-robin kernel thread scheduler. */

void sched_init(void);                                  /* adopt the boot context as "main" */
void thread_create(void (*entry)(void), const char *name);
int  thread_create_user(const char *name, uint64_t entry, uint64_t ustack, void *data, uint64_t cr3);
void thread_exit(void);                                 /* end the current thread */
void schedule(void);                                    /* switch to the next ready thread */

unsigned long sched_switches(void);                     /* total context switches so far */
void *sched_current_data(void);                         /* current thread's payload */

#endif /* AQUA_SCHED_H */
