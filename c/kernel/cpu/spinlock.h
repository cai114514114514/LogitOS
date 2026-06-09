#ifndef AETHER_SPINLOCK_H
#define AETHER_SPINLOCK_H

#include <stdint.h>

/* Ticket spinlock with seq-cst acquire/release. Two layers:
 *  - spin_lock/spin_unlock        : bare, for sections never entered from an IRQ.
 *  - spin_lock_irqsave/irqrestore : saves+clears IF, for any lock taken in both
 *    thread and IRQ context (a core holding such a lock must not be interrupted
 *    into code that re-takes it -> self-deadlock). */
typedef struct {
    unsigned int ticket;
    unsigned int serving;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

void     spin_lock(spinlock_t *l);
void     spin_unlock(spinlock_t *l);
uint64_t spin_lock_irqsave(spinlock_t *l);
void     spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);

extern spinlock_t g_bkl;            /* Big Kernel Lock (M25 P0) */
extern volatile int g_bkl_owner;    /* cpu index holding g_bkl (-1 = free); for nested detection */

#endif /* AETHER_SPINLOCK_H */
