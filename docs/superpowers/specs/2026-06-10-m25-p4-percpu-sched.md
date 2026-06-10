# M25 P4 — per-CPU run queues (measured, REVERTED) + parallel present (SHIPPED)

Executes the plan deferred in `2026-06-08-m25-completion-status.md` §P4. Outcome,
honestly: **P4a (per-CPU run queues + work stealing) was fully built, gated, and
REVERTED on the data** — at this kernel's scale the proven global ring is
strictly better; the deferral doc's risk/value prediction was correct and is now
quantified. **P4b (parallel framebuffer present) shipped** on IPI vector 241
with a contention gate. Several durable by-products landed along the way.

## P4a — what was built

Full per-CPU scheduler per the plan: per-core circular ready ring + `rq_lock` +
dead list in `struct cpu`; `g_ring`/`g_sched_lock` deleted; the lock held across
`context_switch` became the switching core's own rq_lock, released by the
incoming thread at `this_cpu()` (migration-correct by construction); placement
by least-loaded core; work stealing via a new `spin_trylock` (no AB-BA by
construction); BKL hand-off and first-run IF discipline preserved. It booted
and ran **correctly in every configuration tried: corruption=0 in ~60 smptest
runs across 7 design iterations, zero scheduler crashes** (the one early crash
was a stale-object build artifact -- see "by-products").

## P4a — why it was reverted (the data)

Gate: `smptest` (4 ring-3 children hammering the BKL-free kheap path), workload
sized to T1≈6s. Old global ring: **T1=6 TN=6, distinct_cpus=4, 5/5 runs —
perfect work conservation**. The per-CPU scheduler across seven successive
balancing designs: TN 8–13s (ratio 1.3–2.2), distinct_cpus 2–4, bimodal and
unstable. Each iteration fixed a real pathology and exposed the next:

1. **Steal pump**: stealing on every dry `schedule()` → busy-looping yielders
   (waitpid shell, SYS_YIELD pollers) migrated threads at ~3k/s; every
   migration costs the next run a CR3 reload = full TLB flush under TCG.
2. **The BKL valve**: damping the steal froze the system — a BKL-holding
   blocking loop's *context switch is the only thing that releases the BKL*;
   with nothing stealable it never switched and three cores starved (QMP:
   g_bkl ticket-serving gap = 4).
3. **Tick-gated stealing deadlocked on its own clock**: the PIT tick advances
   on cpu0 only, and cpu0 was IF-off *waiting for the BKL the steal would have
   released*. Local LAPIC credits failed next: **int 0x80 is an interrupt gate
   — a syscall blocking loop spins with IF=0 and never receives its own tick**.
4. **Park-to-idle** (voluntary yielder with dry ring parks on the idle thread)
   fixed liveness + freed cores (baseline child got a clean core: T1 2s→1s,
   why the workload had to grow), but…
5. **Idle cores ping-ponged parked pollers** (~1000 migrations/s of
   Clock/Finder for zero work) → `parked` flag, steal skips them; then a core
   hosting any parked spinner never reached its steal branch (pick-order
   inversion); then…
6. **Near-synchronous LAPIC ticks**: all cores sampled their neighbours at the
   exact zoo-slice instant — either everyone stole at once (musical chairs,
   ~125 migrations/s at steady state) or, with a victim-state transit filter,
   nobody ever balanced. Staggering the LAPIC periods (+12.5%/core) helped but
   did not close the gap; a wait-age stamp (global switch counter) didn't
   either: children spend ~100ms bursts inside IF-off BKL-free syscalls and
   resist migration sampling entirely.

Conclusion: in a BKL kernel whose blocking model is spin-via-schedule, the
global ring **is** the load balancer, the BKL release valve, and the fairness
engine in one mechanism; per-CPU queues must re-implement all three with
heuristics, and at ~10 threads there is no contention win to pay for it
(`g_sched_lock` is brief and barely contended — the BKL is the serializer).
Revisit only at much higher thread counts, with real blocking/wake queues
(not spin-loops), on real hardware (no TCG TLB-flush-per-CR3 artifact), and
ideally after shrinking the BKL.

## P4b — parallel present (SHIPPED)

`smp.c`: `smp_present_par()` registered via `fb_set_present_par()` when >1 core
is online. Splits a tall rect into one row-band per online core, publishes
bands, IPIs vector **241** (240 belongs to TLB shootdown; new `isr241` stub +
IDT gate — its absence #GP'd (err=0x78a = IDT|241) the first boot), copies its
own band, waits for acks with a bounded spin, then copies any un-acked band
itself (idempotent fallback).

Two hard-won safety rules:
- **Handler is BKL-free** (presenter usually holds the BKL while waiting).
- **Contention gate**: if `g_bkl.ticket - g_bkl.serving > 1`, present solo.
  BKL-queued cores spin IF-off and cannot service the IPI until the presenter
  releases the BKL — every parallel attempt rode the full ack timeout while
  holding the BKL, collapsing the system (boot fine, everything else crawling).
  Parallel present engages exactly when it helps: big composites while other
  cores are idle or in ring 3.

Also kept: the per-core LAPIC period stagger (+12.5%/index) — in-phase
preemption ticks make the cores hit shared locks in lock-step (convoys).

## Gate results (final tree)

- `make test`, `test-shell`, `test-libc`, `test-as-os`: PASS.
- `test-smp`: **5/5 consecutive** — T1=6 TN=6 distinct_cpus=4 corruption=0
  (workload grown 16→48 chunks: the old 2s baseline was at the test's own >=2s
  floor; 1s-granularity RTC made the ratio ±50% noise).
- App-churn soak (`make CHURN=1`, -smp 4, parallel present active): no freeze,
  no heap corruption (see commit log for duration/counters).

## Durable by-products

- **Makefile header-dependency tracking (-MMD -MP + -include \*.d)**: editing
  `struct cpu` in percpu.h rebuilt only git-touched .c files; stale objects
  disagreed on the struct layout and corrupted `g_cpus` at runtime — a
  perfect counterfeit of an SMP scheduler bug (garbage `cpu->current`, #PF in
  `schedule()`).
- `spin_trylock` (ticket-lock cmpxchg try-acquire) — kept for future use.
- Kernel-fault diagnostics: `panic_exception` now prints cr2/cpu/current.
- smptest workload floor fix; QMP RIP+RDI sampling and per-build `nm` symbol
  hygiene (stale-build addresses produced phantom "corrupted lock" reads).
