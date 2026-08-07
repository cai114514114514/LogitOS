/* Host-test stub for c/kernel/cpu/spinlock.h. The mm host tests are
 * single-threaded (they exercise the ALGORITHM, not the SMP protocol), so the
 * locks are no-ops. Shadows the real header via -I order. */
#ifndef MMSTUB_SPINLOCK_H
#define MMSTUB_SPINLOCK_H

#include <stdint.h>

typedef int spinlock_t;
#define SPINLOCK_INIT 0

static inline uint64_t spin_lock_irqsave(spinlock_t *l)                 { (void)l; return 0; }
static inline void     spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }
static inline void     spin_lock(spinlock_t *l)                          { (void)l; }
static inline void     spin_unlock(spinlock_t *l)                        { (void)l; }

#endif
