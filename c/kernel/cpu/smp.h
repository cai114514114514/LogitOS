#ifndef AETHER_SMP_H
#define AETHER_SMP_H
#include <stdint.h>

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */
void smp_present_ipi(void);   /* vector-241 handler body: copy this core's present band */
int  smp_irq_via_apic(void);  /* 1 once device IRQs are routed through the I/O APIC */
void smp_mark_sched_ready(void); /* BSP: release parked APs into the scheduler */

#endif
