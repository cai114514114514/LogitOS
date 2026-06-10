/* Host-test stub: single-threaded test, locks are no-ops. */
#ifndef KHEAPSTUB_SPINLOCK_H
#define KHEAPSTUB_SPINLOCK_H

#include <stdint.h>

typedef int spinlock_t;
#define SPINLOCK_INIT 0

static inline uint64_t spin_lock_irqsave(spinlock_t *l)            { (void)l; return 0; }
static inline void     spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }

#endif
