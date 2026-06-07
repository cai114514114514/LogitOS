#ifndef AQUA_SCHED_H
#define AQUA_SCHED_H

#include <stdint.h>

/* Cooperative-capable, preemptive round-robin kernel thread scheduler. */

struct registers;   /* interrupts.h */

void sched_init(void);                                  /* adopt the boot context as "main" */
void thread_create(void (*entry)(void), const char *name);
int  thread_create_user(const char *name, uint64_t entry, uint64_t ustack, void *data, uint64_t cr3);
/* Create a child thread that resumes from the parent's int 0x80 frame `r`
 * (returning 0 in the child) in address space `cr3`. Used by fork(). */
int  thread_fork(const char *name, struct registers *r, void *data, uint64_t cr3);
void thread_exit(void);                                 /* end the current thread */
void schedule(void);                                    /* switch to the next ready thread */
void thread_create_idle(int idx);                       /* SMP: build core `idx`'s idle thread */
void sched_become_idle(void);                           /* SMP: AP enters its idle loop (never returns) */
void sched_unlock_new_thread(void);                     /* new ring-3/fork first-run: drop g_sched_lock + BKL */
void kthread_bootstrap(void);                           /* kernel-thread first-run trampoline */

unsigned long sched_switches(void);                     /* total context switches so far */
void *sched_current_data(void);                         /* current thread's payload */
uint64_t sched_current_cr3(void);                        /* active thread address space */

#endif /* AQUA_SCHED_H */
