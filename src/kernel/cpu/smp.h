#ifndef AQUA_SMP_H
#define AQUA_SMP_H
#include <stdint.h>

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */
void smp_ipi_work(void);      /* vector-240 handler body (P0: no-op, IPI retired) */
int  smp_irq_via_apic(void);  /* 1 once device IRQs are routed through the I/O APIC */
void smp_mark_sched_ready(void); /* BSP: release parked APs into the scheduler */

#endif
