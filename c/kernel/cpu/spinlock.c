#include "spinlock.h"
#ifdef LOGIT_LOCK_HOST
/* Architecture seam only: host tests execute the production ticket protocol. */
extern int logit_lock_host_cpu(void);
extern void tlb_service(void);
extern void serial_putc(char);
static int lock_cpu(void) { return logit_lock_host_cpu(); }
#else
#include "percpu.h"
#include "tlb.h"
#include "serial.h"
static int lock_cpu(void) { return this_cpu()->index; }
#endif

/* Correction (2026-09-10): the global entry lock is gone. Each
 * caller supplies the lock belonging to the object it protects. */
void spin_lock(spinlock_t *l)
{
    /* Tickets order contenders; observing our serving value is the acquire.
     * There is only one writer of serving while owned, so releasing needs
     * a store, not an extra locked read-modify-write/cache-line round trip. */
    unsigned int my = __atomic_fetch_add(&l->ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != my) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile ("pause");
#else
        __asm__ volatile ("" ::: "memory");
#endif
        /* A core waiting here has IF=0 and cannot take an interrupt -- which is
         * precisely why tlb_flush_all() had to give up on it and why the real
         * TLB shootdown was never wired into vmm_free_space. It can still read
         * a byte. Serving the request from inside the wait is what makes a
         * shootdown reach a core that is blocked on a lock. Costs one per-core
         * byte load per pause when there is nothing owed. */
        tlb_service();
    }
    l->owner_ra  = (unsigned long)__builtin_return_address(0);
    l->owner_cpu = lock_cpu();

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
static void lock_puts(const char *m) { while (m && *m) serial_putc(*m++); }

/* The CPU table now has 32 slots.  Keep this diagnostic lock-free, but print
 * the complete index so a bad release on CPU 30 cannot masquerade as CPU 6. */
static void lock_put_uint(unsigned int value)
{
    char digits[10];
    unsigned int used = 0;
    do {
        digits[used++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && used < sizeof digits);
    while (used) serial_putc(digits[--used]);
}

/* serving is about to pass ticket -- see the call site. */
static void lock_desync(const spinlock_t *l, void *ra)
{
    static volatile int said;
    if (said) return;
    said = 1;
    lock_puts("[lock] BUG: unlock with ticket==serving at ra=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)ra >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    lock_puts(" lock=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)l >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    lock_puts(" -- serving passes ticket; every later acquirer spins forever\r\n");
}

static void lock_bad_release(int me, int owner, void *ra)
{
    static volatile int said;
    if (said) return;                 /* one line: the first one is the cause */
    said = 1;
    lock_puts("[lock] BUG: cpu ");
    lock_put_uint((unsigned int)me);
    lock_puts(" released a lock held by ");
    if (owner < 0) lock_puts("nobody");
    else lock_put_uint((unsigned int)owner);
    lock_puts(", ra=0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)(((unsigned long)ra >> sh) & 15);
        serial_putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
    lock_puts(" -- a later acquirer will wait for a ticket that is never served\r\n");
}

void spin_unlock(spinlock_t *l)
{

    /* EVERY lock, not just the BKL. A ticket lock released by a core that does
     * not hold it advances  past a ticket nobody has, and from then on
     * SOME LATER ACQUIRER WAITS FOR A NUMBER THAT WILL NEVER BE SERVED -- on a
     * lock whose ticket==serving reads as free. That is a freeze with no
     * evidence in it, arbitrarily far from the release that caused it, which is
     * why this check is here and not in a debug build. */
    if (l->owner_cpu != lock_cpu())
        lock_bad_release(lock_cpu(), l->owner_cpu, __builtin_return_address(0));
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
        if (sv == tk) lock_desync(l, __builtin_return_address(0));
    }
    l->owner_cpu = -1;
    __atomic_store_n(&l->serving, __atomic_load_n(&l->serving, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
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
    l->owner_cpu = lock_cpu();

    return 1;
}

uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags;
#ifdef LOGIT_LOCK_HOST
    flags = 0;
#else
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
#endif
    spin_lock(l);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
#ifndef LOGIT_LOCK_HOST
    if (flags & 0x200)              /* restore IF only if the caller had it set */
        __asm__ volatile ("sti");
#else
    (void)flags;
#endif
}
