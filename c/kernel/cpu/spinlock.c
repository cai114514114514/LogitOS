#include "spinlock.h"
#include "percpu.h"      /* this_cpu (BKL owner tracking) */
#include "kbench.h"      /* g_kb_stat: BKL wait/hold accounting, off by default */

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

/* BKL ACCOUNTING (kbench.h). "The big kernel lock is the concurrency model" is
 * a design statement; "the four cores spent N ms waiting for it and M ms
 * holding it, and P% of acquisitions had to wait" is the thing that decides
 * whether removing it is the next piece of work. Nothing else in the tree could
 * answer that, so the lock counts itself.
 *
 * Only g_bkl, and only when armed: the disabled path adds one pointer compare
 * (which spin_lock already made below, so it is a reorder rather than a new
 * instruction) and one load of a global. No rdtsc, no call. */
static inline void bkl_acquired(int idx, uint64_t t0, int waited)
{
    if (idx < 0 || idx >= KB_MAXCPU) return;
    uint64_t t1 = kb_rdtsc();
    g_kb[idx].bkl_acq++;
    g_kb[idx].bkl_contended += (uint64_t)(waited != 0);
    g_kb[idx].bkl_wait += t1 - t0;
    g_kb[idx].bkl_t0 = t1;
}

void spin_lock(spinlock_t *l)
{
    unsigned int my = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_SEQ_CST);
    int stat = (l == &g_bkl) && g_kb_stat;
    int waited = 0;
    uint64_t t0 = stat ? kb_rdtsc() : 0;
    while (__atomic_load_n(&l->serving, __ATOMIC_SEQ_CST) != my) {
        waited = 1;
        __asm__ volatile ("pause");
    }
    if (l == &g_bkl) {
        int idx = this_cpu()->index;
        g_bkl_owner = idx;
        if (stat) bkl_acquired(idx, t0, waited);
    }
}

void spin_unlock(spinlock_t *l)
{
    if (l == &g_bkl) {
        int idx = g_bkl_owner;
        /* bkl_t0 == 0 means the accounting was armed AFTER this acquisition, so
         * there is no start timestamp to subtract. Skipping it loses one sample
         * per core at arm time; using it would credit the lock with every cycle
         * since boot. */
        if (g_kb_stat && idx >= 0 && idx < KB_MAXCPU && g_kb[idx].bkl_t0) {
            g_kb[idx].bkl_hold += kb_rdtsc() - g_kb[idx].bkl_t0;
            g_kb[idx].bkl_t0 = 0;
        }
        g_bkl_owner = -1;
    }
    __atomic_fetch_add(&l->serving, 1, __ATOMIC_SEQ_CST);
}

/* Ticket-lock trylock: the lock is free iff ticket==serving; claim it by
 * advancing ticket from s to s+1 atomically. If any other core grabbed a ticket
 * between our load and the cmpxchg, the cmpxchg fails and we report busy --
 * we never wait, and a failed attempt takes no ticket (no queue pollution). */
int spin_trylock(spinlock_t *l)
{
    unsigned int s = __atomic_load_n(&l->serving, __ATOMIC_SEQ_CST);
    unsigned int expect = s;
    if (!__atomic_compare_exchange_n(&l->ticket, &expect, s + 1, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    if (l == &g_bkl) {
        int idx = this_cpu()->index;
        g_bkl_owner = idx;
        if (g_kb_stat) bkl_acquired(idx, kb_rdtsc(), 0);   /* never waits: wait == 0 */
    }
    return 1;
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
