#ifndef LOGIT_SMP_H
#define LOGIT_SMP_H
#include <stdint.h>
#include "smp_topology.h"

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */
void smp_present_ipi(void);   /* vector-241 handler body: copy this core's present band */
int  smp_irq_via_apic(void);  /* 1 once device IRQs are routed through the I/O APIC */
void smp_mark_sched_ready(void); /* BSP: release parked APs into the scheduler */
int  smp_cpu_topology_get(int index, struct smp_topology_record *out);
void smp_topology_get_summary(struct smp_topology_summary *out);

#ifdef LOGIT_RAPTOR_SMP_QEMU_TEST
void smp_test_timer_interrupt(void);
void smp_test_ipi_interrupt(void);
#endif

#endif
