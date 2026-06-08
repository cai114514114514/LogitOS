#ifndef AETHER_TLB_H
#define AETHER_TLB_H

/* M25 P2: cross-core TLB shootdown. When a core changes or frees a mapping that
 * another core may have cached, broadcast an IPI so every other online core
 * drops its stale TLB (full CR3 reload). The initiator flushes itself too and
 * waits for all others to acknowledge before returning, so a freed frame is not
 * reused while a stale translation to it could still live in another core's TLB.
 *
 * The IPI handler (tlb_ipi) is BKL-free. **Deadlock constraint:** do NOT call
 * tlb_flush_all() while holding the BKL -- a core spinning to acquire the BKL
 * does so with IF=0 (spin_lock_irqsave), so it cannot take the shootdown IPI,
 * never acks, and the BKL-holding initiator waits forever. Call it only from a
 * context where every other core can service an IPI (IF=1): i.e. from a bkl-free
 * subsystem that owns its own fine-grained lock, NOT from the BKL-serialized
 * free/unmap paths. (This is why vmm_free_space does not call it -- and under the
 * current BKL it isn't needed; see vmm_free_space.) Wired in P3+ when a peeled,
 * bkl-free subsystem first remaps/frees a frame another core may have cached. */

void tlb_flush_all(void);   /* initiator: flush self + all other cores; wait for ack */
void tlb_ipi(void);         /* IPI-240 handler (BKL-free), called from interrupt_handler */

#endif /* AETHER_TLB_H */
