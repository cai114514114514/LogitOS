#ifndef LOGIT_KIRQ_H
#define LOGIT_KIRQ_H

#include <stdint.h>

/* Save-and-clear / restore the interrupt flag.
 *
 * This is one header rather than two lines inlined into klog.c so that the
 * host unit test can SHADOW it (tests/unit/klogstub/kirq.h, on the include
 * path ahead of this one -- the same trick tests/unit/kheapstub uses). `cli`
 * is privileged: a ring-3 test process executing it takes a #GP, so the real
 * ring-0 instructions have to be replaceable to test the ring logic natively.
 *
 * This is NOT a lock. It excludes only THIS core's interrupt handlers, which
 * is exactly what a per-CPU buffer needs; cross-core exclusion is the ring
 * lock's job. */
static inline uint64_t kirq_save(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}

static inline void kirq_restore(uint64_t f)
{
    __asm__ volatile ("pushq %0\n\tpopfq" :: "r"(f) : "memory", "cc");
}

#endif /* LOGIT_KIRQ_H */
