/* M25 P2: cross-core TLB shootdown (see tlb.h). */
#include <stdint.h>
#include "tlb.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"

#define TLB_IPI_VEC 240          /* the retired present-IPI vector, repurposed */

static void flush_self(void);   /* defined below; tlb_service uses it first */

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
    if (!__sync_lock_test_and_set(&g_tlb_req[me], 0)) return 0;
    flush_self();
    __sync_fetch_and_add(&g_tlb_ack, 1);
    return 1;
}

unsigned long tlb_late_count(void) { return g_tlb_late; }

static void flush_self(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");   /* reload -> drop non-global TLB */
}

void tlb_flush_all(void)
{
    flush_self();
    int n = smp_cpu_count();
    if (n <= 1) return;                      /* uniprocessor: self-flush is enough */
    int me = this_cpu()->index;
    g_tlb_ack = 0;
    int others = 0;
    /* Flags BEFORE interrupts, and a barrier between: a core that takes the IPI
     * the instant it is sent must find the flag already set, or it acks nothing
     * and the initiator waits for it. */
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        g_tlb_req[i] = 1;
        others++;
    }
    __sync_synchronize();
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        lapic_send_ipi((uint8_t)g_cpus[i].lapic_id, TLB_IPI_VEC);
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
    for (volatile long spin = 0; spin < 50000000L && g_tlb_ack < others; spin++)
        __asm__ volatile ("pause");
    if (g_tlb_ack < others) {
        __sync_fetch_and_add(&g_tlb_late, (unsigned long)(others - g_tlb_ack));
        for (int i = 0; i < n; i++) if (i != me) g_tlb_req[i] = 0;   /* drop the stale request */
    }
}

void tlb_ipi(void)
{
    /* Through the same claim as the poll, so an IPI that races the spin loop
     * does not ack twice and let the initiator through one core early. */
    tlb_service();
}
