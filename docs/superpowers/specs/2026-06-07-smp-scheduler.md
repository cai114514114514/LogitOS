---
title: M25 — Preemptive SMP scheduler (the road from BKL to fine-grained locking)
status: spec
date: 2026-06-07
---

# M25 — Preemptive SMP scheduler

## Goal

Make Aether run threads on **multiple cores at once**. Today the APs only do a
parallel framebuffer present (`ap_entry` = `for(;;) sti;hlt`, woken by vector-240
IPIs); **all real threads run on the BSP**, and the scheduler/`current`/TSS are
single globals. This milestone turns the APs into full scheduling cores so that
ring-3 (and eventually kernel) work runs truly in parallel.

This is the **A-layer prerequisite** for Java-style multi-threaded AetherScript
(the B-layer, a later milestone): real parallelism for compute-bound code, which
is exactly the thing Python's GIL denies. AetherScript already uses mark-sweep GC
(not refcounting), so it has no GIL-equivalent baked in — the only blocker to
parallelism is this OS-level scheduler.

## The core engineering truth (why this is phased, not "two options")

A preemptive SMP scheduler's hard part is not "schedule on the APs" — it is that
the moment two cores can be in the kernel simultaneously, **every shared kernel
structure that was written single-core (kmalloc, pmm, the WM compositor, the net
stack, the fs) is exposed to data races.** "cli for atomicity" no longer works:
`cli` only masks interrupts on the *local* core.

There are not two parallel choices (BKL vs fine-grained). **Fine-grained locking
IS the incremental dismantling of the BKL.** You cannot have both at once. The
only sane order is:

1. A **Big Kernel Lock (BKL)** so multi-core boots correctly on day one with a
   single lock, and ring-3 already runs in parallel (the anti-GIL payoff).
2. Then peel hot subsystems out from under the BKL one at a time, each getting its
   own lock, with the test suite green at every step.

Jumping straight to full fine-grained locking is a flag-day: every subsystem must
be made SMP-safe *before* SMP can boot at all, with no working intermediate and
unbounded risk. So this spec commits to the **whole arc P0→P4** (captured here so
it is not forgotten) but **executes it incrementally**, BKL first.

## Architecture

### Spinlocks (new primitive — `src/kernel/cpu/spinlock.{h,c}`)
A ticket spinlock with acquire/release memory barriers (`__atomic` seq-cst).
Two layers:
- `spin_lock(l)` / `spin_unlock(l)` — bare, for sections never entered from an IRQ.
- `spin_lock_irqsave(l)` / `spin_unlock_irqrestore(l, flags)` — saves+clears IF,
  for any lock taken in both thread and IRQ context (most of them). Required
  because a core holding a lock must not be interrupted into code that re-takes it
  (self-deadlock).
Held-lock invariant: never sleep/yield while holding a spinlock except the
scheduler's own documented hand-off (below).

### Per-CPU state (`src/kernel/cpu/percpu.{h,c}`)
```
struct cpu {
    int       index;          /* 0 = BSP */
    uint32_t  lapic_id;
    struct thread *current;   /* the thread running on THIS core (was a global) */
    struct thread *idle;      /* this core's idle thread (hlt loop) */
    struct tss tss;           /* this core's TSS -> its own ring-0 rsp0 */
    int       in_kernel;      /* depth, for BKL/preemption accounting */
};
static struct cpu cpus[MAXCPU];
```
The running core finds its own `struct cpu` via its LAPIC id (`this_cpu()`).
(Phase-later optimization: stash the `struct cpu*` in `%gs` via `MSR_GS_BASE` +
`swapgs` so `this_cpu()` is one instruction; start with the LAPIC-id lookup.)
`current` and `tss` move from single globals into `struct cpu`; `tss_set_rsp0`,
`sched_current_*` become per-core.

### AP scheduler entry (`src/kernel/cpu/smp.c`)
`ap_entry` changes from the hlt-loop to: load this core's GDT + its own TSS,
`lapic_init` + `lapic_timer_init` (per-core preemption tick), create this core's
idle thread, then enter `schedule()`. Each AP now pulls runnable threads and
preempts on its own LAPIC timer.

### Run queue
- **P0:** ONE global run queue, protected by a scheduler spinlock. `schedule()`
  takes the lock, picks the next runnable thread (skipping ones marked
  running-on-another-core), switches to it. A thread is owned by at most one core
  at a time (a `running` flag set under the lock).
- **P4:** per-CPU run queues + work-stealing (a core whose queue is empty steals
  from the busiest), replacing the global queue + its single lock.

### The Big Kernel Lock (P0)
A single `bkl` spinlock taken on **every entry into kernel code**:
- `int 0x80` syscall dispatch (`syscall_dispatch`): acquire on entry, release
  before `iretq` to ring 3.
- Every IRQ/exception handler (`isr_common` C side): acquire on entry, release on
  exit. (The timer IRQ that calls `schedule()` is special — see hand-off.)
- Kernel threads (WM, init) run holding the BKL while in kernel code; they drop it
  only when blocking/yielding.
Effect: **at most one core executes kernel code at a time; ring-3 runs in
parallel.** A compute-bound as program with two threads doing arithmetic runs on
two cores simultaneously, contending the BKL only on syscalls/GC — near-linear
speedup for the anti-GIL workload.

### Context switch + BKL hand-off (the subtle part)
`schedule()` runs holding a lock (BKL and/or run-queue lock). The classic SMP
dance: the *outgoing* thread switches stacks/CR3 into the *incoming* thread while
the lock is held; the incoming thread is responsible for releasing the lock after
the switch completes (its saved context resumes just after the `context_switch`
call, where it then unlocks). New threads (first run via `ring3_bootstrap` /
`fork_ret`) must unlock the BKL as their first action. TLB: switching CR3 is per
the existing `vmm_switch`; a fresh `current` per core means `schedule()` reseats
`this_cpu()->current`, not the global.

### TLB shootdown (`src/kernel/cpu/tlb.c`, introduced when needed in P2/P3)
When a core changes a mapping another core may have cached (kernel-space changes,
or freeing a user space that ran elsewhere), broadcast an IPI so the other cores
`invlpg`/reload CR3. Per-process *private* user subtrees are mostly contained (a
space runs on one core at a time), so this is mainly needed once kernel-space or
cross-core space reuse appears; documented here, implemented at the phase that
first needs it.

### Framebuffer present (regression to manage)
The current 3×-on-4-cores parallel present uses the APs via IPI. Once APs run the
scheduler they are no longer idle to borrow. **P0 reverts present to BSP-only**
(the WM thread does the whole present) — a perf regression accepted for
correctness. **P4** restores parallelism as a scheduler-aware parallel job
(spread the present rows across idle cores via the run queue, not a raw IPI).

## Phases (committed in full, executed incrementally)

- **P0 — multi-core boots, ring-3 parallel.** Spinlocks + per-CPU state + AP
  scheduler entry + global run queue + BKL + context-switch/BKL hand-off + present
  reverted to BSP-only. Gate: `make test` / `test-shell` / `test-as-os` pass
  under `-smp 4`; a new SMP stress test shows two ring-3 threads running on two
  cores at once (a wall-clock or concurrency-counter proof).
- **P1 — peel kheap.** `kmalloc`/`kfree` get their own lock; remove them from BKL
  coverage. Hottest contention point. Gate: SMP stress (many threads hammering
  kmalloc) + full suite.
- **P2 — peel pmm.** `pmm_alloc`/`pmm_free` own lock; first TLB-shootdown need
  appears here (frame reuse across cores). Gate: SMP fork/exec stress.
- **P3 — peel WM / net / fs.** Each shared subsystem gets its own lock (or stays
  BKL-guarded if measured low-contention). Audit every `cli`-for-atomicity and
  busy-flag (`net_lock`, `g_net_busy`, `ata_busy`, `g_virtio_busy`) for cross-core
  correctness. Gate: full suite + a GUI + net stress under `-smp 4`.
- **P4 — per-CPU run queues + work-stealing; restore parallel present.** Replace
  the global run queue. Gate: load-balance stress (uneven thread load spreads
  across cores) + full suite + the present perf back.

## Risks

- **Context-switch/BKL hand-off** — the #1 SMP bug source; new threads must unlock
  correctly, the outgoing/incoming lock ownership must be exact.
- **`cli` no longer cross-core** — every existing atomicity assumption via `cli`
  must be re-audited; most become BKL-covered, some IRQ handlers need spinlocks.
- **TLB coherence** — kernel-space mapping changes need shootdown IPIs (P2+).
- **Non-determinism** — SMP races are flaky; rely on stress tests + many `-smp 4`
  runs, and keep the (already `-smp 4`) suite green at every phase.
- **Present regression** — accepted in P0, restored in P4.

## Testing

- The existing `make test / test-shell / test-as / test-as-gcstress /
  test-as-os` already run under `-smp 4 -accel tcg,thread=multi` — they must stay
  green at every phase (now exercising real cross-core scheduling, not just
  parallel present).
- **New `make test-smp`**: a kernel/userland stress that spawns N threads across
  cores doing allocation + compute, asserting (a) no corruption (a checksum/
  invariant that only holds if mutual exclusion is correct) and (b) genuine
  concurrency (e.g. a counter or wall-clock speedup only achievable if two cores
  ran ring-3 simultaneously). Run repeatedly to shake out races.

## Scope & the B-layer hooks (noted, not implemented here)

Out of scope: AetherScript thread model (per-thread VM context, stop-the-world GC
across all as threads, `Thread`/`spawn`, monitors/locks/atomics, a memory model)
— that is the **B-layer**, a separate later milestone built on this one. Hooks
left for it: per-CPU `current` makes "all running threads" enumerable for a GC
stop-the-world; the spinlock + IPI infrastructure here is what an IPI-based
stop-the-world GC pause will reuse. Also out of scope: NUMA, CPU hotplug,
priority/real-time scheduling, gang scheduling.
