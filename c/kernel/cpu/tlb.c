/* M25 P2: cross-core TLB shootdown (see tlb.h). */
#include <stdint.h>
#include "tlb.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"
#ifndef LOGIT_TLB_HOST
#include "io.h"
#else
/* Architecture operations and observation only; the transaction and ticket
 * lock below remain the production implementations in pthread tests. */
void tlb_host_flush_self(int request);
void tlb_host_pause(void);
void tlb_host_begin(int cpu, int peers);
void tlb_host_end(int cpu, int ack);
void tlb_host_emergency_putc(char c);
_Noreturn void tlb_host_failstop(void);
#endif

#ifndef TLB_WAIT_SPINS
#define TLB_WAIT_SPINS 50000000L
#endif

#define TLB_IPI_VEC 240          /* the retired present-IPI vector, repurposed */

static void flush_self(int request);   /* defined below; tlb_service uses it first */

/* The request/ack transaction has one publisher at a time. Waiters poll
 * tlb_service through spin_lock, so simultaneous initiators still ACK the
 * holder's request even with interrupts masked. */
static spinlock_t g_tlb_send = SPINLOCK_INIT;
static volatile int g_tlb_ack;   /* receivers bump this after flushing */

/* WHY A FLAG AND NOT JUST AN IPI.
 *
 * A core waiting for the BKL waits in spin_lock_irqsave -- with INTERRUPTS OFF
 * -- so the shootdown IPI cannot reach it, it never acks, and the initiator
 * waits for an acknowledgement that can never come. That is why tlb_flush_all
 * gave up after a bounded spin, and why it is still not wired into
 * vmm_free_space: the whole mechanism was unusable from under the lock that
 * every unmap path holds.
 *
 * The IPI is not the only way to be told. The initiator now RECORDS the request
 * in a per-core flag before sending it, and the spin loop in spinlock.c polls
 * that flag while it waits. A core that cannot take an interrupt can still read
 * a byte. Whichever arrives first -- the interrupt or the poll -- claims the
 * request atomically, so the ack is counted exactly once.
 *
 * This is the same shape as the nanosleep wedge fixed in 601b8926e: an IF=0
 * spin that blocks the very thing it is waiting for. There, the fix was to stop
 * spinning; here it is to make the spin do the work. */
static volatile unsigned char g_tlb_req[PERCPU_MAXCPU];
static volatile unsigned long g_tlb_late;   /* acks that never came, ever */

/* Serve a pending shootdown for THIS core, if there is one. Safe from anywhere
 * -- it reloads CR3 and touches no lock -- which is what lets the spin loop
 * call it. Returns 1 if it did something. */
int tlb_service(void)
{
    /* Nothing to serve before the other cores exist, and this runs from inside
     * spin_lock -- the earliest code in the kernel. Checking the core count
     * first keeps this_cpu() out of the boot path entirely, where the per-CPU
     * area is not yet what it will be. */
    if (smp_cpu_count() <= 1) return 0;
    int me = this_cpu()->index;
    if (me < 0 || me >= PERCPU_MAXCPU) return 0;
    /* Claim atomically: the IPI handler may be running this on another path.
     * Exactly one of them gets the 1, so exactly one ack is counted. */
    if (!__atomic_exchange_n(&g_tlb_req[me], 0, __ATOMIC_ACQUIRE)) return 0;
    flush_self(1);
    __atomic_fetch_add(&g_tlb_ack, 1, __ATOMIC_RELEASE);
    return 1;
}

unsigned long tlb_late_count(void) { return __atomic_load_n(&g_tlb_late, __ATOMIC_RELAXED); }

static void flush_self(int request)
{
#ifdef LOGIT_TLB_HOST
    tlb_host_flush_self(request);
#else
    (void)request;
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");   /* reload -> drop non-global TLB */
#endif
}

/* No console/serial lock on this path: the missing CPU may own it. These
 * bounded raw UART writes can lose diagnostics on wedged hardware, but cannot
 * allow unmap to return and reuse a page with an outstanding translation. */
static void tlb_raw_char(char c)
{
#ifdef LOGIT_TLB_HOST
    tlb_host_emergency_putc(c);
#else
    for (unsigned i=0;i<100000;i++)
        if (inb(0x3fd)&0x20) { outb(0x3f8,(uint8_t)c); break; }
#endif
}
static void tlb_raw_text(const char *s) { while (*s) tlb_raw_char(*s++); }
static void tlb_raw_hex(unsigned value)
{
    tlb_raw_text("0x");
    for (int shift=28;shift>=0;shift-=4) {
        unsigned digit=(value>>shift)&15;
        tlb_raw_char((char)(digit<10?'0'+digit:'a'+digit-10));
    }
}
static _Noreturn void tlb_failstop(int me, int ack, int expected, int n)
{
    unsigned pending=0;
    for (int i=0;i<n;i++)
        if (__atomic_load_n(&g_tlb_req[i],__ATOMIC_RELAXED)) pending|=1u<<i;
    tlb_raw_text("[tlb] FATAL shootdown timeout cpu="); tlb_raw_hex((unsigned)me);
    tlb_raw_text(" ack="); tlb_raw_hex((unsigned)ack);
    tlb_raw_text(" want="); tlb_raw_hex((unsigned)expected);
    tlb_raw_text(" unclaimed="); tlb_raw_hex(pending);
    tlb_raw_text("; page ownership retained\r\n");
#ifdef LOGIT_TLB_HOST
    tlb_host_failstop();
#else
    for (;;) __asm__ volatile ("cli; hlt");
#endif
}

void tlb_flush_all(void)
{
    flush_self(0);
    int n = smp_cpu_count();
    if (n <= 1) return;                      /* uniprocessor: self-flush is enough */
#ifndef TLB_NO_SEND_LOCK
    uint64_t flags = spin_lock_irqsave(&g_tlb_send);
#else
    (void)&g_tlb_send; uint64_t flags=0; /* named host negative control */
#endif
    int me = this_cpu()->index;
#ifdef LOGIT_TLB_HOST
    tlb_host_begin(me,n-1);
#endif
    __atomic_store_n(&g_tlb_ack,0,__ATOMIC_RELAXED);
    int others = 0;
    /* Flags BEFORE interrupts, and a barrier between: a core that takes the IPI
     * the instant it is sent must find the flag already set, or it acks nothing
     * and the initiator waits for it. */
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        __atomic_store_n(&g_tlb_req[i],1,__ATOMIC_RELEASE);
        others++;
    }
    __sync_synchronize();
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        (void)lapic_send_ipi(g_cpus[i].lapic_id, TLB_IPI_VEC);
    }
    /* Spin until every other core has flushed. BOUNDED: a core spinning to
     * acquire the BKL does so with IF=0 (irqsave) and cannot service the IPI,
     * so an unbounded wait here would deadlock the whole machine the day any
     * caller wires this up under the BKL (the exact failure smp_present_par
     * guards against with its contention gate). An un-acked core simply keeps
     * stale TLB entries until its next CR3 switch flushes them -- safe, since
     * every current use pairs the shootdown with a CR3 reload. */
    /* Still bounded, but for a different reason and with a different ending.
     * The spin-loop poll covers a core waiting for a lock; a core sitting in
     * some other IF=0 region is still unreachable, so the wait cannot be
     * unbounded. What changed is that giving up is no longer SILENT -- an
     * un-acked shootdown means somebody kept a stale entry, and a counter that
     * is normally zero is the difference between knowing that and not. */
    /* Correction to the former counter-only give-up above: a failed ACK
     * transaction now emits a lock-independent diagnostic and never returns. */
    for (long spin=0;spin<TLB_WAIT_SPINS &&
         __atomic_load_n(&g_tlb_ack,__ATOMIC_ACQUIRE)<others;spin++) {
#ifdef LOGIT_TLB_HOST
        tlb_host_pause();
#else
        __asm__ volatile ("pause");
#endif
    }
    int ack=__atomic_load_n(&g_tlb_ack,__ATOMIC_ACQUIRE);
    if (ack != others) {
        if (ack < others)
            __atomic_fetch_add(&g_tlb_late,(unsigned long)(others-ack),__ATOMIC_RELAXED);
        /* An excess ACK is also a broken transaction, not permission to reuse. */
        tlb_failstop(me,ack,others,n);
    }
#ifdef LOGIT_TLB_HOST
    tlb_host_end(me,ack);
#endif
#ifndef TLB_NO_SEND_LOCK
    spin_unlock_irqrestore(&g_tlb_send,flags);
#else
    (void)flags;
#endif
}

void tlb_ipi(void)
{
    /* Through the same claim as the poll, so an IPI that races the spin loop
     * does not ack twice and let the initiator through one core early. */
    tlb_service();
}
