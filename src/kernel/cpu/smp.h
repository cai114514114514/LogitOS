#ifndef AQUA_SMP_H
#define AQUA_SMP_H
#include <stdint.h>

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */
void smp_ipi_work(void);      /* vector-240 handler body: do this CPU's present band */

#endif
