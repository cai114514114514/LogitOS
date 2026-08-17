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
| `w->surf.px` POINTER | **also SYS_GUI_CREATE, on the app thread** | safe by an accident that must become a rule -- see below |
| the glyph cache | any `SYS_GUI_TEXT` | its own lock — the one genuinely new piece |
| the back buffer | WM only | nothing |

### The window list: the hazard is the slot claim, not the pointer

The row above began by saying the surface pointer is WM-thread-only, on the
strength of `win_apply_size()` being the only REALLOCATION and running on the WM
thread. Reading `SYS_GUI_CREATE` says otherwise: the first allocation runs on the
APP thread, and it publishes in the wrong order --

```c
w->used = 1; w->kind = WK_APP; w->app = ap;   /* live ... */
...
w->surf.px = kmalloc((size_t)(pxcount * 4));  /* ... and only then given pixels */
```

-- so there is a window in which `used == 1` and `surf.px` is the previous
tenant's freed pointer. The BKL hides it today.

**The compositor survives it by an accident nobody wrote down.** Every render
path walks `order[]` (wm.c:977, :3845, :3912) and never `wins[]` by `used`, and
`raise_win(wi)` -- the only thing that puts a window INTO `order[]` -- is the
last statement of `SYS_GUI_CREATE`, after the buffer exists. So a half-built
window is unreachable from a frame. That is load-bearing and undocumented, which
puts it one refactor away from being untrue. Peeling the compositor is the moment
to write it down as a rule and assert it, not the moment to discover it again.

**The race the BKL is really carrying here is the slot claim:**

```c
for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { wi = i; break; }
```

Two apps calling `SYS_GUI_CREATE` at once scan and claim the same slot, and
nothing else prevents it. So the window-list lock is needed for a reason that has
nothing to do with rendering -- and it must cover the scan and the `used = 1`
together, which is also the cheapest critical section available: a scan of one
byte per window.

This is the same shape as the bug the input-deferral comment (wm.c:2450)
describes -- IRQ handlers mutating `order[]`/`wins[]` under a compositing WM
thread, giving "a torn read yields a garbage window rect and fb_round_rect/fb_put
runs away in a near-infinite pixel loop". That path was fixed by DEFERRING to the
WM thread. The syscall path was never fixed; it was covered by the BKL, and this
step is where it stops being.

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

### ...and then the file says the BKL is its reason, which changes the answer

`text.c:89`, above the shaping scratch:

> The scratch is file scope because the whole UI runs under the big kernel
> lock, and because a 32 KiB kernel stack has no room for it.

So `rastbuf` is not the only shared scratch and not even the big one.
`tl_cps`, `tl_levels`, `tl_order`, `tl_glyphs`, `tl_runs` and `tl_bidi` (32 KiB
by itself, ~45 KiB together) are the SHAPING scratch, and unlike the raster
buffer they are touched on EVERY text call, not only on a cache miss. The
"allocate it on a miss" answer above does not extend to them: 45 KiB per drawn
string is not a trade, it is a regression.

Three properties of the file make the right answer small:

- Every public entry -- `text_draw`, `text_draw_sz`, `text_draw_mono`,
  `text_draw_mono_sz`, `text_width`, `text_width_sz`, `text_measure`,
  `text_draw_run` -- funnels through **one** function. The file says so at
  line 81 and gives the reason: measuring and drawing must agree at the same
  px, so a separate measuring path would drift a few pixels per line in a way
  nobody could reproduce. There is exactly one place to lock.
- All six scratch buffers are bound into one struct at lines 125-128, in one
  statement. There is exactly one thing to protect.
- Text was already serialised -- by the BKL. A text lock is not new
  serialisation; it is the same serialisation, scoped to text.

**So the first piece of step 2 is a TEXT lock, not a glyph-cache lock**, and the
glyph cache falls inside it rather than needing one of its own. That also
answers hazard 3 for free: the caller of `glyph_get` is inside the same lock,
so a returned entry cannot be evicted under it.

Per-core scratch was the alternative and is rejected for now: 45 KiB x 8 cores
is 360 KiB of kernel `.bss` bought to remove a lock that is held for the
duration of one string. If text contention ever shows up in the holder profile,
that is the measurement that would justify it -- and the profile already exists
to say so.

Order of work: text lock → window-list lock + geometry snapshot →
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

### It is smaller than that paragraph, because there are only two device IRQs

The mechanism already exists: `interrupts.c:131` computes `bkl_free`, and
vectors 240 and 241 (TLB shootdown, parallel present) are already handled
without the lock. Declaring a vector BKL-free is a term in one expression.

And the list of things to declare is short. e1000 and virtio are **polled** —
there is no NIC interrupt on this machine. The BSP timer tick already runs
*before* the acquire, for a documented circular-wait reason. What is left is
IRQ 1 (PS/2 keyboard) and IRQ 12 (PS/2 mouse), and since the input-deferral fix
both handlers do almost nothing: read the port, track modifiers in their own
statics, and push one event.

**Except that they push to the same queue, and that queue says it is safe when
what makes it safe is the BKL:**

```c
static void inq_push(const struct inev *e)   /* IRQ-safe: no locks, no shared-state */
{
    int nt = (inq_tail + 1) % INQ_N;
    if (nt == inq_head) return;
    inq[inq_tail] = *e;
    inq_tail = nt;
}
```

One producer makes that correct. **There are two** — `wm_key` from IRQ 1 and
`wm_mouse_event` from IRQ 12, both a two-line wrapper around this function — and
today they cannot overlap only because every IRQ takes the global lock on the
way in. Drop the lock for those two vectors and two cores can be inside
`inq_push` at once: both read the same `inq_tail`, both write the same slot,
both store the same `nt`, and one keystroke is silently gone. Not corruption; a
dropped event, under exactly the flood the queue was sized for.

This is the M25 P3 audit's warning met in the concrete, and it is worth noticing
that the comment is not wrong so much as **unqualified**: "no locks" is a
statement about this function, and its correctness lives in a caller three files
away.

**And the obvious fix is wrong, which is worth writing down because it was
written down here first.** "CAS the tail" — claim a slot atomically, then fill
it — advances `inq_tail` *before* `inq[t]` is written, so the drain can observe
a tail past a slot that holds the previous tenant's event. Claim-then-write is
the standard MPSC hazard and a single CAS does not close it.

Three answers, and the conditions pick one:

- **A per-slot ready flag** the consumer spins on. Correct, lock-free, and it
  puts a spin in `wm_drain_input` to save six instructions in an IRQ. Wrong
  trade at this size.
- **Two queues, one per IRQ**, each genuinely single-producer, drained together.
  Fully lock-free and it fits the shape — exactly two producers, each its own
  vector. But it **loses the order between a keystroke and a click**, and there
  is no timestamp in `struct inev` to restore it. Ordering across the two
  devices is not obviously disposable (a modifier is sampled in the IRQ
  precisely so it reflects the instant of the press), so this needs an argument
  nobody has made rather than a preference.
- **A lock around the producers only.** The critical section is a bounds check,
  one struct copy and one store; the consumer touches `inq_head` alone and stays
  as it is. Exact ordering preserved, one atomic added to a path that already
  costs a port read.

The third, until something measures the second as necessary.

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
