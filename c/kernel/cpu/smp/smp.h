#ifndef LOGIT_SMP_H
#define LOGIT_SMP_H
#include <stdint.h>
#include "smp_topology.h"

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */
void smp_present_ipi(void);   /* vector-241 handler body: copy this core's present band */
void smp_kick_others(void);   /* vector 243 to every other online core: leave hlt, schedule */
int  smp_irq_via_apic(void);  /* 1 once device IRQs are routed through the I/O APIC */
void smp_mark_sched_ready(void); /* BSP: release parked APs into the scheduler */
int  smp_cpu_topology_get(int index, struct smp_topology_record *out);
void smp_topology_get_summary(struct smp_topology_summary *out);

/* Counters of the IPI-parallel present (vector 241), read by the HUD. They
 * answer whether the other cores service the IPI inside the ack wait: a
 * timeout means the presenter waited the full bound and copied the band
 * itself. miss[] counts, per core, the bands still pending at a timeout. */
#define SMP_PRESENT_STATS_CORES 32
struct smp_present_stats {
    uint64_t calls;               /* tall presents routed through the IPI path */
    uint64_t fast;                /* every band acked inside the wait */
    uint64_t timeouts;            /* the wait expired with a band pending */
    uint64_t fallback_bands;      /* bands the presenter copied after a timeout */
    uint64_t wait_cycles_total;   /* TSC cycles spent waiting for acks */
    uint64_t wait_cycles_max;     /* longest single wait, TSC cycles */
    uint64_t tsc_hz;              /* converts the cycle counts to time; 0 if unknown */
    uint64_t miss[SMP_PRESENT_STATS_CORES];
    uint64_t missing_mask;        /* bit n set: core n ever missed a deadline */
    int      disabled;            /* 1 once the path unregistered itself */
    uint64_t disable_timeouts;    /* timeouts seen when it was disabled */
    uint64_t disable_calls;       /* presents seen when it was disabled */
};
void smp_present_stats(struct smp_present_stats *out);

#ifdef LOGIT_RAPTOR_SMP_QEMU_TEST
void smp_test_timer_interrupt(void);
void smp_test_ipi_interrupt(void);
#endif

#endif
