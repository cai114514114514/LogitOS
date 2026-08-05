/* M25 P2: cross-core TLB shootdown (see tlb.h). */
#include <stdint.h>
#include "tlb.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"

#define TLB_IPI_VEC 240          /* the retired present-IPI vector, repurposed */

static volatile int g_tlb_ack;   /* receivers bump this after flushing */

static inline void flush_self(void)
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
    __sync_synchronize();
    int others = 0;
    for (int i = 0; i < n; i++) {
        if (i == me) continue;
        lapic_send_ipi((uint8_t)g_cpus[i].lapic_id, TLB_IPI_VEC);
        others++;
    }
    /* Spin until every other core has flushed. BOUNDED: a core spinning to
     * acquire the BKL does so with IF=0 (irqsave) and cannot service the IPI,
     * so an unbounded wait here would deadlock the whole machine the day any
     * caller wires this up under the BKL (the exact failure smp_present_par
     * guards against with its contention gate). An un-acked core simply keeps
     * stale TLB entries until its next CR3 switch flushes them -- safe, since
     * every current use pairs the shootdown with a CR3 reload. */
    for (volatile long spin = 0; spin < 50000000L && g_tlb_ack < others; spin++)
        __asm__ volatile ("pause");
}

void tlb_ipi(void)
{
    flush_self();
    __sync_fetch_and_add(&g_tlb_ack, 1);
}
