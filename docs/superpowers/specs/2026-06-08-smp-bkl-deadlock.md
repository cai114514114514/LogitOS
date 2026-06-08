# M25 P0 hardening — the -smp 4 BKL ↔ g_sched_lock ABBA deadlock

Status: **FIXED** (2026-06-08). Root cause + fix below. Was: real, reproducible,
NOT the QEMU MTTCG artifact (reproduced in single-thread TCG too).

## THE FIX (root cause)

First-run thread hand-built frames in `sched.c` set RFLAGS = **0x202 (IF=1)** for
the ring3_bootstrap / kthread_bootstrap entries. When `context_switch` (switch.asm)
`popfq`s that frame to first-run the thread, IF becomes 1 *while the bootstrap
still holds g_sched_lock* (it releases it a few instructions later). A timer IRQ in
that window enters `interrupt_handler` and does `spin_lock_irqsave(&g_bkl)` --
acquiring g_bkl while holding g_sched_lock, the reverse of the BKL→sched order --
which ABBA-deadlocks against a core that holds g_bkl and is waiting for g_sched_lock
in `schedule()`. First-run threads happen on every app launch, so GUI churn hit it.

Fix: hand-built frames now set RFLAGS = **0x002 (IF=0)** (like fork_ret already
did). Each bootstrap re-enables IF at its own controlled point AFTER releasing
g_sched_lock (kthread_bootstrap: `sti` after taking g_bkl; ring3_bootstrap: the
iretq's RFLAGS=0x202). Plus `context_switch` returns IF=0 defensively (a `cli`
after popfq). Verified: -smp 4 (both `-accel tcg` and `tcg,thread=multi`) survives
1800+ ops of a motion/drag/click/key + dock-launch storm that previously wedged all
cores by op ~19-150; g_bkl ticket/serving stays healthy; UI stays usable.

The original investigation notes are kept below for reference.

---

Original status when OPEN: real, reproducible; NOT the QEMU MTTCG artifact. Single-core
was already stable (the WM input-vs-render freeze fixed in commit ffd3b90).

## Symptom

Under `-smp 4`, heavy GUI interaction (opening apps / dragging / clicking the
dock while threads churn) hard-freezes the whole system: all cores end up spinning
in `spin_lock`, none makes progress. Reproduces under both `-accel tcg,thread=multi`
AND plain `-accel tcg` (single host thread, correct atomics) — so it is a genuine
lock-ordering bug, not the QEMU-MTTCG-on-Apple-Silicon mis-emulation.

## Evidence (captured live via QMP at a confirmed freeze)

- `g_bkl` ticket−serving == 4, **stable** across many reads (a real deadlock,
  4 cores in the lock's lifecycle), `g_bkl_owner` == 2.
- Per-core wedge (recorded by instrumenting `spin_lock` to dump the lock + caller
  after N spins, read back via QMP):
  - cpu0/cpu1/cpu3: spinning on **g_bkl**, caller = **interrupt_handler:69**
    (`spin_lock_irqsave(&g_bkl)` on kernel entry).
  - cpu2: holds g_bkl (owner=2), spinning on **g_sched_lock**, caller =
    **schedule():226** (`spin_lock_irqsave(&g_sched_lock)`).

## Root cause (the cycle)

Classic ABBA between the BKL and the scheduler-ring lock:

- cpu2 holds **g_bkl**, is in `schedule()` waiting for **g_sched_lock** (the
  correct order: BKL → sched).
- The **g_sched_lock holder** is sitting in **interrupt_handler:69** waiting for
  **g_bkl** — i.e. it is holding g_sched_lock and trying to acquire g_bkl, the
  REVERSE order. That can only happen if a timer/IRQ fired (IF=1) while the core
  held g_sched_lock: the IRQ enters `interrupt_handler`, sees `nested==false`
  (g_bkl was dropped), and does `spin_lock_irqsave(&g_bkl)` → reverse order →
  cycle with cpu2.

The window is `schedule()`'s BKL hand-off: it acquires g_sched_lock (226), DROPS
g_bkl (282) while still holding g_sched_lock, `context_switch`es (283), and the
incoming side releases g_sched_lock (286) then re-acquires g_bkl (287). Anywhere
in `[drop g_bkl .. release g_sched_lock]` that runs with IF=1 is exposed.

## Partial fix already in tree (commit with this doc)

`src/boot/switch.asm`: `context_switch` now returns with **IF=0** (a `cli` after
the `popfq` that restored the incoming thread's RFLAGS — a first-run thread's
hand-built frame has IF=1, which would otherwise leak into the post-switch window
while g_sched_lock is still held). Every resume site re-enables IF at its own
controlled point (schedule's `if(flags&IF) sti`, kthread_bootstrap's sti, the
ring-3 iretq). This is a correct invariant and **delayed** the deadlock
substantially (froze at ~op 19 without it, ~op 150–600 with it) but did NOT
eliminate it — there is at least one more IF=1-while-holding-g_sched_lock window.

## Candidate next steps

1. Audit EVERY site that can run with IF=1 while g_sched_lock is held (the whole
   `[226 .. 286]` span across the context_switch, all bootstraps, and the AP idle
   bring-up `sched_become_idle`/`ap_entry`). Add a reliable g_sched_lock owner +
   an assertion in interrupt_handler ("about to take g_bkl while holding
   g_sched_lock") to pinpoint the remaining window (the throwaway g_sched_owner
   probe used here was unreliable because the lock is acquired by one thread and
   released by another across the switch).
2. Consider removing the hazard structurally: do NOT drop g_bkl while holding
   g_sched_lock — e.g. hand the BKL across context_switch without a window, or
   make the scheduler not require g_bkl at all (P4 per-CPU runqueues would remove
   the single g_sched_lock contention entirely).

## Mitigation in place

`make run`/`debug` now default to `-smp 1` (stable). Use
`make run QEMU_SMP="-smp 4 -accel tcg,thread=multi"` for SMP work. Tests still
exercise -smp 4 via their own scripts (they pass: the deadlock needs GUI churn).
