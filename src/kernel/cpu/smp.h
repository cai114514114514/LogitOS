#ifndef AQUA_SMP_H
#define AQUA_SMP_H
#include <stdint.h>

void smp_init(void);          /* detect + bring up application processors */
int  smp_cpu_count(void);     /* CPUs that came online (incl. BSP) */

#endif
