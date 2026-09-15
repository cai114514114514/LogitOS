/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_IO_LOCK_H
#define LOGIT_IO_LOCK_H
#include <stdint.h>
/* A small non-sleeping device gate. Ownership is independent of interrupt
 * state: CLI excludes this CPU's IRQ, the acquire/release excludes other CPUs.
 * Callers must not schedule while held. Device polling may enable IRQs only
 * when that device's IRQ path never takes this gate. Waiting also services TLB
 * mailboxes: an IF=0 contender must not strand an unmapping CPU's shootdown.
 * Hosted tests use the identical atomic protocol, with privileged leaves gone. */
typedef struct { unsigned held; } io_lock_t;
#define IO_LOCK_INIT { 0 }
static inline void io_relax(void)
{
#if !__STDC_HOSTED__
    extern int tlb_service(void);
    __asm__ volatile ("pause");
    tlb_service();
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline uint64_t io_irq_save(void)
{
#if !__STDC_HOSTED__
    uint64_t f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f;
#else
    return 0;
#endif
}
static inline void io_irq_restore(uint64_t f)
{
#if !__STDC_HOSTED__
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
#else
    (void)f;
#endif
}
static inline uint64_t io_lock_enter(io_lock_t *l)
{
    uint64_t f = io_irq_save();
#ifndef IO_NO_LOCK
    for (;;) {
        unsigned zero = 0;
        if (__atomic_compare_exchange_n(&l->held, &zero, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        while (__atomic_load_n(&l->held, __ATOMIC_RELAXED)) io_relax();
    }
#else
    (void)l;
#endif
    return f;
}
static inline int io_lock_try(io_lock_t *l)
{
    unsigned zero = 0;
    return __atomic_compare_exchange_n(&l->held, &zero, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static inline void io_lock_leave(io_lock_t *l, uint64_t f)
{
#ifndef IO_NO_LOCK
    __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
#else
    (void)l;
#endif
    io_irq_restore(f);
}
struct io_guard { io_lock_t *lock; uint64_t flags; };
static inline void io_guard_drop(struct io_guard *g)
{ io_lock_leave(g->lock, g->flags); }
#define IO_GUARD(lockptr) \
    struct io_guard io_guard_ __attribute__((cleanup(io_guard_drop))) = \
        { (lockptr), io_lock_enter(lockptr) }
#endif
