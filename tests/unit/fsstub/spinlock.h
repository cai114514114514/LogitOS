#ifndef LOGIT_FS_SIM_SPINLOCK_H
#define LOGIT_FS_SIM_SPINLOCK_H

/* The kernel spinlock, stubbed to nothing for the LogitFS host tests.
 *
 * WHY A NO-OP AND NOT A REAL LOCK, because "the stub is a no-op" is the kind of
 * sentence that later reads as laziness:
 *
 *  - These binaries are SINGLE-THREADED. tests/unit/fs_{crash,fsck,bulkread}
 *    and statmeta drive one operation at a time against a simulated device, so
 *    a real lock would be taken and released by the only thread there is and
 *    could never be contended. It would measure nothing and it would change
 *    nothing -- which is exactly the property the gate needs: every one of
 *    those suites must report the SAME COUNT after this change as before, and
 *    a no-op guarantees that by construction rather than by hoping.
 *
 *  - A held-flag assertion would be actively WRONG here. fs_sim.h simulates a
 *    power cut with longjmp() out of the middle of an operation -- no
 *    unwinding, no cleanup, by design, because that is what losing power does
 *    to a call stack. A lock taken on the way in is never released on the way
 *    out, so any state-carrying stub would report a deadlock on the next mount
 *    for every one of test-fs-crash's 1,744 cuts. On the machine the same event
 *    discards the lock along with the rest of RAM, which is what the no-op
 *    models.
 *
 * So: the host gates prove the JOURNAL is unchanged, and they are honest about
 * not proving anything about the lock. What proves the lock is the kernel
 * build and the boot gates (test-durability, test-fscrash, test-barrier).
 *
 * Included only via -Itests/unit/fsstub. The kernel build resolves the same
 * name to c/kernel/cpu/spinlock.h through INCDIRS; the basename is unique in
 * the tree apart from this file, which is never on a kernel include path. */

#include <stdint.h>

typedef struct {
    unsigned int  ticket;
    unsigned int  serving;
    unsigned long owner_ra;
    int           owner_cpu;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0, 0, -1 }

static inline void     spin_lock(spinlock_t *l)                  { (void)l; }
static inline void     spin_unlock(spinlock_t *l)                { (void)l; }
static inline int      spin_trylock(spinlock_t *l)               { (void)l; return 1; }
static inline uint64_t spin_lock_irqsave(spinlock_t *l)          { (void)l; return 0; }
static inline void     spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{ (void)l; (void)f; }

#endif /* LOGIT_FS_SIM_SPINLOCK_H */
