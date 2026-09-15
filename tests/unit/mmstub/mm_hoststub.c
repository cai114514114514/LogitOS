/* The two symbols c/kernel/mm reaches for outside c/kernel/mm, for the host
 * suites (tests/unit/mm_run.sh, oom_run.sh, leak_run.sh).
 *
 * WHY THIS FILE EXISTS, because both call sites say it should not:
 *
 *   c/kernel/mm/virt/vmm.c:12   declares tlb_flush_all() weak and explains
 *                          "Absent, the call is skipped, which is right: a host
 *                           test has no other core to shoot down."
 *   c/kernel/mm/phys/kheap.c:365 declares kheap_cpu_index() weak and explains
 *                          "Absent, it answers 0: the host test then exercises
 *                           one magazine."
 *
 * Both are the ELF idiom and both are correct on the kernel target. Neither
 * holds on the documented development host. `weak` on a DECLARATION asks the
 * linker for "resolve to NULL if nobody defines it"; Mach-O spells that
 * `weak_import`, not `weak`, so on macOS/Apple Silicon an undefined weak
 * reference is a hard link error rather than a NULL:
 *
 *     $ make test-mm
 *     === mm_pmm_test ===
 *     Undefined symbols for architecture arm64:
 *       "_tlb_flush_all", referenced from:
 *           _vmm_free_space in vmm-29fca6.o
 *
 * All three mm host scripts died there -- at `ld`, before the first assertion
 * of any suite -- so on this host the two reclaim controls CLAUDE.md cites
 * (RECLAIM_NO_PIN_CHECK, RECLAIM_NO_ZERO_CHECK) and the six others beside them
 * had never been watched to fail at all.
 *
 * Defining the symbols here rather than teaching two kernel files a second
 * spelling keeps the fix inside the test apparatus, which is what owns the
 * host's quirks.
 *
 * tlb_flush_all() is strictly BETTER than the NULL vmm.c asks for, and that is
 * the one deliberate difference in this file. With a definition present
 * `if (tlb_flush_all)` is always true, so the host takes the same branch the
 * machine takes instead of skipping the line, and the request becomes
 * countable: a test that changes an unmap or an mprotect can assert the
 * shootdown was ASKED FOR (mm_host_tlb_flushes()) rather than silently never
 * reaching the code. The body is empty on purpose and is not a simulation --
 * these suites are single-threaded (tests/unit/mmstub/spinlock.h makes the same
 * argument about locks), vmm_space_busy_elsewhere() is hard-wired to 0 in
 * vmm.c's own MM_HOSTTEST branch, and there is no second TLB on the host to be
 * stale. What is under test is that vmm.c DECIDES to flush.
 *
 * kheap_cpu_index() is the opposite: it returns 0, which is exactly what
 * mag_cpu() computes when the symbol is NULL, so the magazine layer behaves
 * identically to the absent case and this file changes nothing about what
 * leak_run.sh measures.
 */
#include "mm_hoststub.h"

static unsigned long g_flushes;

void tlb_flush_all(void) { g_flushes++; }

unsigned long mm_host_tlb_flushes(void) { return g_flushes; }
void          mm_host_tlb_reset(void)   { g_flushes = 0; }

int kheap_cpu_index(void) { return 0; }
