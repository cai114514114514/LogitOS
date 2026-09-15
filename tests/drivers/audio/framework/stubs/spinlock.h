#ifndef AUDIO_TEST_SPINLOCK_H
#define AUDIO_TEST_SPINLOCK_H
#include <stdint.h>
typedef struct { unsigned held; } spinlock_t;
#define SPINLOCK_INIT {0}
uint64_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);
#endif
