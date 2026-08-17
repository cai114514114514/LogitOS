#include "spinlock.h"
#include "percpu.h"      /* this_cpu (BKL owner tracking) */
#include "kbench.h"      /* g_kb_stat: BKL wait/hold accounting, off by default */
#include "tlb.h"         /* tlb_service(): a spin with IF=0 must still answer a shootdown */
#include "serial.h"      /* the bad-release detector prints without taking a lock */

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
        /* A core waiting here has IF=0 and cannot take an interrupt -- which is
         * precisely why tlb_flush_all() had to give up on it and why the real
         * TLB shootdown was never wired into vmm_free_space. It can still read
         * a byte. Serving the request from inside the wait is what makes a
         * shootdown reach a core that is blocked on a lock. Costs one per-core
         * byte load per pause when there is nothing owed. */
        tlb_service();
    }
    l->owner_ra  = (unsigned long)__builtin_return_address(0);
    l->owner_cpu = this_cpu()->index;
    if (l == &g_bkl) {
        int idx = l->owner_cpu;
        g_bkl_owner = idx;
        if (stat) bkl_acquired(idx, t0, waited);
    }
}


/* ==========================================================================
 * THE ONE INVARIANT A TICKET LOCK CANNOT ENFORCE FOR ITSELF.
 *
 * `spin_unlock` on a ticket lock is an unconditional `serving++`. It does not
 * and cannot check that the caller is the holder -- and releasing a BKL you do
 * not hold DOES NOT FAIL HERE. It advances `serving` past a ticket nobody is
 * waiting on, so some later acquirer holds a number that will never be served
 * and spins forever. The machine then stops with EVERY CORE IN spin_lock AND
 * NO OWNER, arbitrarily far in time and code from the release that caused it,
 * which is a freeze with no evidence in it at all.
 *
 * One comparison on the BKL release path converts that into a line naming the
 * core and the return address. It is always on because the failure it catches
 * leaves nothing else behind.
 *
 * serial_putc, not kprintf: this runs INSIDE spin_unlock, and a printer that
 * takes a lock could be the second half of the very deadlock it is reporting.
 * serial_putc is lock-free and bounded (it drops the byte on a wedged UART).
 * ========================================================================== */
static void bkl_puts(const char *m) { while (m && *m) serial_putc(*m++); }

/* serving is about to pass ticket -- see the call site. */
static void bkl_desync(const spinlock_t *l, void *ra)
{
    static volatile int said;
    if (said) return;
    said = 1;
    bkl_puts("[bkl] BUG: unlock with ticket==serving at ra=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)ra >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    bkl_puts(" lock=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)l >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    bkl_puts(" -- serving passes ticket; every later acquirer spins forever\r\n");
}

static void bkl_bad_release(int me, int owner, void *ra)
{
    static volatile int said;
    if (said) return;                 /* one line: the first one is the cause */
    said = 1;
    bkl_puts("[bkl] BUG: cpu ");
    serial_putc((char)('0' + (me & 7)));
    bkl_puts(" released a BKL held by ");
    if (owner < 0) bkl_puts("nobody"); else serial_putc((char)('0' + (owner & 7)));
    bkl_puts(", ra=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)ra >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    bkl_puts(" -- a later acquirer will wait for a ticket that is never served\r\n");
}

void spin_unlock(spinlock_t *l)
{
    if (l == &g_bkl) {
        int idx = g_bkl_owner;
        int me  = this_cpu()->index;
        if (idx != me) bkl_bad_release(me, idx, __builtin_return_address(0));
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
    /* EVERY lock, not just the BKL. A ticket lock released by a core that does
     * not hold it advances  past a ticket nobody has, and from then on
     * SOME LATER ACQUIRER WAITS FOR A NUMBER THAT WILL NEVER BE SERVED -- on a
     * lock whose ticket==serving reads as free. That is a freeze with no
     * evidence in it, arbitrarily far from the release that caused it, which is
     * why this check is here and not in a debug build. */
    if (l->owner_cpu != this_cpu()->index)
        bkl_bad_release(this_cpu()->index, l->owner_cpu, __builtin_return_address(0));
    /* AND THE DESYNC ITSELF, caught at the instant it is created. A releaser
     * necessarily holds a ticket -- its own -- so `ticket` is at least one
     * ahead of `serving` here, ALWAYS. If they are equal, this serving++ is
     * about to push serving PAST ticket, and from then on every acquirer waits
     * for a number that is already gone: an unbounded spin on a lock whose
     * ticket==serving reads as free. That is the exact state a wedged -smp 4
     * machine was found in, arbitrarily far in time from the release that did
     * it, so the check belongs here and not in a post-mortem.
     *
     * No false positive is possible: serving is read first, and the only core
     * that may advance serving is the holder, which is this one. Another core
     * taking a ticket only makes ticket LARGER between the two reads. */
    {
        unsigned int sv = __atomic_load_n(&l->serving, __ATOMIC_SEQ_CST);
        unsigned int tk = __atomic_load_n(&l->ticket,  __ATOMIC_SEQ_CST);
        if (sv == tk) bkl_desync(l, __builtin_return_address(0));
    }
    l->owner_cpu = -1;
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
    l->owner_ra  = (unsigned long)__builtin_return_address(0);
    l->owner_cpu = this_cpu()->index;
    if (l == &g_bkl) {
        int idx = l->owner_cpu;
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
