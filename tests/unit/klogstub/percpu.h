#ifndef LOGIT_PERCPU_H
#define LOGIT_PERCPU_H
#include <stdint.h>
/* host-test stub: `which CPU am I` is a variable the test drives, which is how
 * a single-threaded process can play two cores interleaving mid-line. */
#define PERCPU_MAXCPU 8
struct cpu { int index; };
struct cpu *this_cpu(void);
void klogtest_set_cpu(int i);
#endif
