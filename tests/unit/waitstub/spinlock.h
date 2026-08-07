/* Host-test stub for the kernel spinlock.
 *
 * NOT a no-op like kheapstub's: this test runs the real wait.c under real
 * pthreads, so the lock has to actually exclude. It is the SAME ticket algorithm
 * the kernel uses -- only the two things that cannot run in ring 3 are changed:
 * `cli/sti` (dropped; the flags word is carried as a zero) and the `pause`
 * spin hint (sched_yield instead). Deliberately free of <pthread.h>: this header
 * is pulled in transitively from pthread.h itself, and a cycle there is how the
 * first attempt at this stub failed to compile.
 */
#ifndef WAITSTUB_SPINLOCK_H
#define WAITSTUB_SPINLOCK_H

#include <stdint.h>

typedef struct { unsigned int ticket, serving; } spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

void     spin_lock(spinlock_t *l);
void     spin_unlock(spinlock_t *l);
int      spin_trylock(spinlock_t *l);
uint64_t spin_lock_irqsave(spinlock_t *l);
void     spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);

#endif
