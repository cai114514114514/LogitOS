# M25 (preemptive SMP scheduler) — completion status & P4 plan

Written 2026-06-08 during an autonomous "finish M25" session. Honest status of each
phase, what shipped, and why P4's scheduler rewrite is deferred rather than rushed.

## Status

| Phase | What | State |
|-------|------|-------|
| **P0** | spinlocks + per-CPU + AP scheduler + global run queue + BKL + ctx-switch/BKL hand-off | ✅ committed (fb24a93); -smp4 boot/run deadlock + corruption fixed this session |
| **P1** | peel kheap → `kheap_lock`; first bkl-free path (`SYS_KHEAP_STRESS`) | ✅ committed (08d4a0e) |
| **P2a** | peel pmm → `pmm_lock` | ✅ committed (08d4a0e) |
| **P2b** | TLB shootdown | ✅ **infrastructure** committed (3396432); active wiring deferred — see below |
| **P3** | peel WM/net/fs (or keep BKL if low-contention) + audit cli/busy-flags | ✅ audited; WM/net/fs stay BKL-guarded (low-contention, spec-allowed) — see below |
| **P4** | per-CPU run queues + work-stealing; restore parallel present | ⏸ **deferred** — risk/value rationale + plan below |

**The M25 *correctness* goal is met and solid:** four cores boot and schedule
ring-3 threads in parallel; the hot contention point (the allocators) is peeled to
fine-grained locks; the full suite + `test-smp` (4 cores, corruption=0, genuine
parallelism) pass. P4 is a *scalability refinement*, not a correctness fix.

## P2b — TLB shootdown: why infra-only

`src/kernel/cpu/tlb.c` provides `tlb_flush_all()` (IPI vector 240, BKL-free
handler, wait-for-ack) — correct and ready. It is **not** called from
`vmm_free_space` because:
1. **Deadlock:** that path runs under the BKL, and a core spinning to acquire the
   BKL has IF=0 (`spin_lock_irqsave`) so it can't service the shootdown IPI →
   never acks → the BKL-holding initiator waits forever. (Caught by `test-shell`.)
2. **Not needed under the BKL:** a user space's frames are only TLB-cached by its
   own threads, which have CR3-switched away before reap; kernel mappings are
   grow-only (no stale positive entries).

It is wired in at the first **bkl-free** unmap path (a peeled subsystem owning its
own lock), or once the BKL acquire-spin services pending shootdowns. Constraint
documented in `tlb.h`.

## P3 — cross-core safety audit

A 4-dimension adversarial audit (busy-flags, cli-atomicity, allocator-internal,
shared-IRQ-state) ran against the current model: **the BKL serializes every kernel
entry except the leaf allocators (kheap/pmm), the one bkl-free syscall, and the
TLB IPI.** So WM/net/fs/drivers, all device IRQs, and all other syscalls are
already mutually exclusive — `cli`-for-atomicity and the busy-flags
(`g_net_busy`/`g_virtio_busy`/`ata_busy`/`net_lock`) remain correct because the
BKL still serializes them.

**Decision (spec-allowed):** keep WM/net/fs **BKL-guarded** — they are I/O-bound,
low-contention, and peeling them buys little while adding large risk. The genuine
concurrency (allocators) is already peeled and leaf-locked.

**Audit result (4 dimensions, 10 hazards checked, adversarially verified):** exactly
**1 real** cross-core hazard, severity low — `pmm_free_bytes()` read
`total_frames - used_frames` without `pmm_lock` while the bkl-free allocator path
writes `used_frames` under the lock (the BKL doesn't serialize them since the
writer is bkl-free). Practical impact tiny (a momentarily-stale free-memory figure
in `sysinfo`; an aligned u64 read isn't torn on x86-64), but it's a genuine
unsynchronized shared access — **fixed**: `pmm_free_bytes()` now reads under
`pmm_lock`. Everything else (`g_net_busy`/`g_virtio_busy`/`ata_busy`/`net_lock`,
all `cli`-for-atomicity, device-IRQ state, the `inq` SPSC input ring) is correctly
BKL-covered or properly leaf-locked → **safe**.

## P4 — deferred (rationale + plan)

**Why deferred, not rushed:** replacing the global run queue (`g_ring` +
`g_sched_lock`) with per-CPU run queues + work-stealing rewrites the most delicate
kernel code (the BKL hand-off across `context_switch`, thread migration, the
`dead_threads` reaping). The risk is a *subtle* scheduler race that boots fine and
corrupts only under load — exactly the "Schrödinger bug" class to avoid — and
`test-smp` is a weak gate for it (it has passed even with scheduler bugs; ARM-host
MTTCG is unreliable for x86 SMP). Shipping that unverified while unattended is the
wrong trade: P4 is low-value *now* (few threads; `g_sched_lock` is briefly held and
barely contended; the BKL, not the run-queue lock, is the real serializer) and
high-risk. The current global run queue is correct and proven.

**Plan for a careful (attended) execution:**
1. Add a per-CPU ready queue to `struct cpu` (`rq_head`/`rq_tail` + a per-CPU
   `rq_lock`); keep `g_ring` only as the bootstrap list until migration is proven.
2. `thread_create*` enqueues onto the least-loaded core's rq (or round-robin).
3. `schedule()` pops from `this_cpu()`'s rq; on empty, **work-steal** from another
   core's rq (lock-ordered: always lower-index-first to avoid AB-BA).
4. Keep the existing BKL hand-off + `g_sched_lock`→per-cpu-rq-lock transition
   carefully (the incoming thread still releases the lock post-`context_switch`).
5. **Restore parallel present** as a scheduler-aware job: enqueue framebuffer-band
   present work onto idle cores' rqs (not the raw vector-240 IPI, now used by TLB).
6. Gate: `test` / `test-shell` / `test-as-os` + `test-smp` green across **many**
   repeated -smp4 runs, plus a new load-balance stress (uneven thread load must
   spread across cores). Do NOT ship on a single green run.

Until then the global run queue stays — M25's multi-core correctness is complete.
