#ifndef LOGIT_KIRQ_H
#define LOGIT_KIRQ_H
#include <stdint.h>
/* host-test stub for c/kernel/core/kirq.h: `cli` is privileged, so a ring-3
 * test process cannot run the real thing. The test counts these instead and
 * asserts every line append is bracketed by them -- i.e. that the guard is
 * actually applied, not merely present in the source. */
uint64_t kirq_save(void);
void     kirq_restore(uint64_t f);
#endif
