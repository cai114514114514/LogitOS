#ifndef LOGIT_SPINLOCK_H
#define LOGIT_SPINLOCK_H
#include <stdint.h>
/* host-test stub. The ring lock is a LEAF lock -- it takes nothing else -- so
 * a single-threaded host test can replace it with a counter and still exercise
 * every line of the ring logic. What the stub does verify is that the log path
 * never nests it: the test asserts the depth never exceeds 1. */
typedef struct { unsigned int ticket, serving; } spinlock_t;
#define SPINLOCK_INIT { 0, 0 }
uint64_t spin_lock_irqsave(spinlock_t *l);
void     spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);
void     spin_lock(spinlock_t *l);
void     spin_unlock(spinlock_t *l);
#endif
