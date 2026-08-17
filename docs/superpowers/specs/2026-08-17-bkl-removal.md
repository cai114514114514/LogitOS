# Removing the BKL — the four steps, and what each one is worth

Written 2026-08-17, after a day of measuring rather than arguing. Every number
below is off this machine. Step 1 is done; this document exists so steps 2–4 are
planned against evidence instead of against the shape of the code.

## Where it stands

|                    | before | now    |
|--------------------|--------|--------|
| syscalls per boot  | 3,283,157 | **1,045** |
| BKL acquisitions   | 3,390,115 | **18,282** |
| BKL time waiting   | 6,685 ms | **3,684 ms** |
| BKL time holding   | 4,211 ms | **1,428 ms** |
| contended          | 0% (diluted) | **5%** |

Three changes got there, and **two of them were not the plan**:

1. `SYS_WAIT_EVENT` — 98% of kernel entries were GUI apps polling
   (`SYS_POLL_EVENT` + `SYS_YIELD`). Not contention: traffic.
2. The `nanosleep` livelock — its sub-two-tick tail spun on `schedule()`
   **holding the BKL**, waiting for a clock only the BSP advances, while the
   BSP sat in `spin_lock_irqsave` with interrupts off. Four cores, 13 forks.
3. Per-core magazines in `kheap` — `test-smp` blamed the BKL and was wrong by
   834x. 30.7 M `kheap_lock` acquisitions against the BKL's 36,836.

## What holds it now

```
[kbench] BKL holders: 608 samples, held in 117 (19%)
  wm_run+0x2de              80% of held      <- the compositor
  interrupt_handler+0xc2    16%
  schedule / block_self      1% each
```

**The lock is free about 80% of the time.** It is a bottleneck because of who
holds it and for how long at a stretch, not because it is busy. **No syscall
appears in this list at all**, which is why widening
`syscall_is_bkl_free()` — the obvious next move, and the one this document
replaces — would move nothing.

The M25 spec's P3 decision ("keep WM/net/fs BKL-guarded — they are I/O-bound,
**low-contention**") was made on 2026-06-08 without these numbers. The premise
is false now: the WM is 80%.

## Step 1 — the deadlock class, retired (DONE, 9d3e78b87)

Cross-core TLB shootdown shipped in M25 P2b as infrastructure nobody could
call: a core spinning for the BKL has IF=0, cannot service the IPI, never acks,
and the initiator deadlocks. `tlb_flush_all()` gave up after a bounded spin and
`vmm_free_space` carried a comment saying it must not be used.

**The IPI is not the only way to be told.** The initiator records the request in
a per-core flag before sending it; `spin_lock()`'s wait loop polls that flag.
Whichever arrives first claims it atomically. A core that cannot take an
interrupt can still read a byte.

`[kbench] tlb: 0 shootdown(s) never acked`, and the shootdown is wired into
`vmm_free_space` in the same commit — a mechanism with no caller is not a fix.

This had to be first: every later peel creates BKL-free unmap paths, and each
one is unsafe until a shootdown can reach a blocked core.

## Step 2 — peel the compositor (80% of held time)

**Smaller than it looks, and the reason is a structural fact worth checking
before designing anything:** `win_apply_size()` is the only code that
reallocates a window surface, and it runs on the WM thread, in the same loop
iteration, BEFORE `wm_render()`. So the use-after-free that would normally make
"composite without the global lock" a rewrite **cannot happen between the
snapshot and the blit**. What is left is smaller:

| what the frame touches | who else touches it | answer |
|---|---|---|
| `order[]` / `norder`, window geometry | app syscalls (create/destroy/resize) | a window-list lock, held only to SNAPSHOT geometry, released before the pixels |
| `w->surf.px` CONTENTS | app syscalls, constantly | tearing, not corruption. `WM_MIDFRAME_GUARD` / `rect_blocked` / `perf_torn` already exist for exactly this |
| `w->surf.px` POINTER | WM thread only (see above) | nothing needed, but assert it |
| the glyph cache | any `SYS_GUI_TEXT` | its own lock — the one genuinely new piece |
| the back buffer | WM only | nothing |

### The glyph cache, read before designing its lock

`c/kernel/gui/text.c`: 2048 open-addressed entries, 8-probe window, evict on
overflow. Three hazards, and **the sharpest one is not the table**:

1. **`rastbuf` is ONE shared 200x200 scratch buffer.** Two cores rasterising at
   once corrupt each other's glyph. This is the hazard a table lock alone would
   not fix, because it is not in the table.
2. **Eviction `kfree(cache[h0].cov)`** frees a coverage buffer another core may
   be blitting from.
3. **`glyph_get` returns a pointer INTO the table**, and the caller reads
   `e->cov` after it returns -- so a lock held only inside `glyph_get` protects
   nothing that matters.

The shape that answers all three without a new global bottleneck:

- **Allocate the scratch on a MISS instead of sharing one.** kmalloc is cheap
  now (per-core magazines, 64d391255) and a miss is rare by construction -- a
  cache that misses often is a cache that is too small, which is a different
  bug. That deletes hazard 1 outright and removes rasterisation from the
  critical section.
- **The lock then covers only the probe, the evict and the fill** -- tens of
  instructions, not a rasterisation.
- **Hazard 3 is the real design choice.** Either the caller holds the lock
  across its blit (simple, and a text run is short), or entries carry a
  refcount. Prefer the first until a measurement says text contention is real;
  the second is the answer if it is.

Order of work: glyph-cache lock → window-list lock + geometry snapshot →
release the BKL across the pixel pass → measure.

**Gates that already exist and must all hold**: `test-desktop-look` (16 recorded
values, including two known defects at their exact extents), the frame
accounting (`ns=` per composite, `presns=`), the BKL holder profile, and
`test-smp-fork-storm`. A tearing change is a behavioural change: if `perf_torn`
moves, that is the cost and it has to be stated, not discovered.

**What is NOT worth doing, measured:** releasing the BKL around `fb_present()`
buys 5% (16.1 ms composite against 0.81 ms present). Releasing between damage
rectangles buys nothing — a full-screen frame is ONE rectangle, and the
worst-case frame is 34.6 ms.

## Step 3 — the interrupt entry (16%)

`interrupt_handler+0xc2` is the acquire itself, for ordinary device IRQs. Each
driver needs its own lock — and the busy-flags (`g_net_busy`, `g_virtio_busy`,
`ata_busy`, `net_lock`) are correct TODAY only because the BKL still serialises
them, which the M25 P3 audit says in as many words. They change together with
this step or not at all.

## Step 4 — the scheduler stops using the BKL as a hand-off medium

`schedule()`, `block_self()`, `thread_exit()` and the three bootstraps all rely
on "drop before the switch, the incoming thread re-acquires after". That is P0's
design and the last thing standing. When it is gone, the single acquire site at
`interrupts.c:152` is dead code and gets deleted.

## The instruments, because none of this was measurable a day ago

- `spinlock_t` records the acquiring caller's return address and core.
- `kb_bkl_sample()` — who holds the BKL, sampled from the timer tick, which runs
  BEFORE the entry takes the lock so the observer is not a holder.
- `tests/boot/run-smp-freeze-probe.sh` — every core's RIP, the lock each waits
  on, and every lock's ticket/serving/holder at a freeze, twice.
- `tests/boot/qmp_lockdump.py` — the same on a RUNNING machine.
- `tests/boot/run-smp-lockprobe.sh` — every lock's counter across a workload;
  this is what caught `kheap_lock` at 30.7 M against the BKL's 36,836.
- `tests/boot/run-smp-fork-storm.sh` — the multi-core reproducer.
- `/dev/kprof` — and read `tty_read`'s comment before quoting it: a sampling
  interrupt on a halted core records the RIP *after* the `hlt`, so an idle core
  and a spinning one look identical.

**No dates. A condition:** the BKL is gone when `syscall_is_bkl_free()` is
everything and the WM, IRQ and scheduler paths no longer need it. The last P3
decision was made without numbers and guessed wrong about which half mattered;
the numbers exist now.
