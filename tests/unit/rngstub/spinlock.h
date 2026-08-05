#ifndef LOGIT_SPINLOCK_H
#define LOGIT_SPINLOCK_H
#include <stdint.h>
/* Host-side no-op stub: the unit test is single-threaded. */
typedef struct { int locked; int unused; } spinlock_t;
#define SPINLOCK_INIT { 0, 0 }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{ (void)l; (void)flags; }
#endif
