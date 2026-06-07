#include "spinlock.h"
#include "percpu.h"      /* this_cpu (BKL owner tracking) */

/* Big Kernel Lock: a single lock taken on every entry into kernel code (P0). */
spinlock_t g_bkl = SPINLOCK_INIT;

/* The CPU index that currently holds g_bkl, or -1. Set the instant the lock is
 * acquired and cleared the instant before it is released -- always from inside
 * the lock primitive, where IF is already 0 (spin_lock_irqsave cli's; the bare
 * re-acquire/release sites cli around themselves). interrupt_handler keys its
 * "am I already in the kernel?" decision off THIS, not a separate in_kernel flag:
 * because the owner tracks true lock ownership (no wide window), a nested IRQ that
 * lands in any in_kernel transition gap still sees owner==me and won't try to
 * re-acquire the BKL this core holds (which would self-deadlock the ticket lock). */
volatile int g_bkl_owner = -1;

void spin_lock(spinlock_t *l)
{
    unsigned int my = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->serving, __ATOMIC_SEQ_CST) != my)
        __asm__ volatile ("pause");
    if (l == &g_bkl) g_bkl_owner = this_cpu()->index;
}

void spin_unlock(spinlock_t *l)
{
    if (l == &g_bkl) g_bkl_owner = -1;
    __atomic_fetch_add(&l->serving, 1, __ATOMIC_SEQ_CST);
}

uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    if (flags & 0x200)              /* restore IF only if the caller had it set */
        __asm__ volatile ("sti");
}
