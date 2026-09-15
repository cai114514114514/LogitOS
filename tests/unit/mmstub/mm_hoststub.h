/* The symbols c/kernel/mm reaches for OUTSIDE c/kernel/mm, supplied for the
 * host suites. See mm_hoststub.c for why they cannot simply be absent here,
 * which is what both call sites say they expect.
 */
#ifndef MMSTUB_HOSTSTUB_H
#define MMSTUB_HOSTSTUB_H

/* c/kernel/cpu/tlb.c on the machine; vmm.c calls it from vmm_free_space() and
 * from vmm_protect_range(). */
void tlb_flush_all(void);

/* How many times vmm.c asked for a shootdown since the last reset. A host test
 * that changes an unmap or an mprotect can assert on this: on the host it is
 * the only observable the call has, because there is no second TLB to flush. */
unsigned long mm_host_tlb_flushes(void);
void          mm_host_tlb_reset(void);

/* c/kernel/cpu/smp/percpu.c on the machine; kheap.c's magazine layer calls it. */
int kheap_cpu_index(void);

#endif
