#ifndef LOGIT_SPINLOCK_H
#define LOGIT_SPINLOCK_H

#include <stdint.h>

/* Ticket spinlock with seq-cst acquire/release. Two layers:
 *  - spin_lock/spin_unlock        : bare, for sections never entered from an IRQ.
 *  - spin_lock_irqsave/irqrestore : saves+clears IF, for any lock taken in both
 *    thread and IRQ context (a core holding such a lock must not be interrupted
 *    into code that re-takes it -> self-deadlock). */
typedef struct {
    unsigned int ticket;
    unsigned int serving;
    /* WHO HOLDS IT. A ticket lock records how many are queued and nothing about
     * the holder, so when a machine stops with every core spinning, the one
     * question that matters -- which code took this lock and never gave it back
     * -- has no answer anywhere in the machine's state. Two stores per
     * acquisition buy it: the return address of the caller that got the lock,
     * and the core it got it on (-1 = free).
     *
     * It is always on, not a debug build, because the failure it explains
     * leaves NOTHING else behind: no panic, no fault, no log line. A freeze
     * that can only be read with an instrument the freeze prevents you from
     * adding is a freeze nobody ever fixes. Cost is a store on a path that
     * already does an atomic RMW. */
    unsigned long owner_ra;
    int           owner_cpu;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0, 0, -1 }

void     spin_lock(spinlock_t *l);
void     spin_unlock(spinlock_t *l);
uint64_t spin_lock_irqsave(spinlock_t *l);
void     spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);
int      spin_trylock(spinlock_t *l);   /* 1 = acquired, 0 = busy; never waits.
                                         * M25 P4: lets a core holding its own
                                         * rq_lock probe another's without any
                                         * wait -> no AB-BA by construction. */

extern spinlock_t g_bkl;            /* Big Kernel Lock (M25 P0) */
extern volatile int g_bkl_owner;    /* cpu index holding g_bkl (-1 = free); for nested detection */

#endif /* LOGIT_SPINLOCK_H */
