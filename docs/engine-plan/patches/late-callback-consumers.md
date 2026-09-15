# Late callback consumers — review-only patch

**Correction, later on 2026-09-09:** the statement below records the earlier
handoff. After checking the other group's latest source and stale modification
time, the patch was reconciled into production `browser.c`, preserving their
changes. `tests/late_callbacks.mk` now runs the actual event loop and is wired
into `test-browser-expansion`. Its early-consumer control fails modes 0–2 at
the first indefinite park; all four positive modes pass. The linked disk was
also exercised in QEMU: native Shift-wheel, then no further input, runs direct
navigation, inserted script and inserted-script navigation. Serial and driver
records are in `build/site-general/guest-consumers/`; the subsequent independent
display:contents geometry case failed, so that complete sequence is not called
green. This correction supersedes the old “not applied/no guest” status while
retaining the evidence boundary of the original handoff below.

**Not applied. Before applying, the other group currently editing `browser.c`
must review/reconcile this patch against its latest event-loop changes.** This
handoff contains no production edit or Makefile change.

`late-callback-consumers.patch` moves the single outer navigation consumer after
scroll/load/pageshow callbacks, drains inserted scripts and settles their effects
first, then consumes navigation. It removes the relocated `!navigated` guard:
a newly loaded page can itself request navigation from a late callback. `load()`
retains its existing bounded redirect loop. Script/console work clears narrow
paint classifications and publishes new JS output to the status line.

The old path consumed navigation before these callbacks. Inserted scripts were
only drained during initial loading or a timer pass that actually ran. Neither
queued scripts nor queued navigation appeared in the idle-wake predicate.

## Exact reproduction

Serve this directory over HTTP, open `late-callback.html?mode=0`, then Shift-wheel
once over the page and stop all input. Repeat with modes 1 and 2:

- `mode=0`: the scroll listener directly navigates to `late-callback-target.html`.
- `mode=1`: it inserts an inline script; the page must show `SCRIPT-RAN`.
- `mode=2`: it inserts an inline script which navigates to the target.

There are no timers or network requests besides navigation. Original production
source reached its first indefinite park with the scroll listener executed but
with these consumers unfinished. Mode 3 is an extra initial-script-scroll
control; it passes in the original too and is **not** negative evidence for the
removed `!navigated` guard.

## Host evidence and commands

Artifacts are confined to `build/wiring-next/late-callback/`. `browser.original.c`
is the production snapshot; `browser.patched.c` applies only the proposed patch.
`source.sha256` records the snapshot hash. Both `.probe.c` files have identical
apparatus changes: repair the copied file's relative include and replace the
existing **host-only** `wait_idle` no-op with an observer. That observer stops at
the first `wait_idle(0)` and checks state **before another loop iteration**.
Thus a host spin cannot falsely make blocked guest navigation look successful.

The source list derives from the existing real `app_main` runtime-scroll harness.
DOM, QuickJS, page queue, layout, painting and script/navigation consumers are
real; host window/event/clock recording and the existing in-memory HTTP fixture
replace OS services. No extra JS timers or wake events are injected after the
single wheel action.

```sh
make -f Makefile -f build/wiring-next/late-callback/probe.mk BUILD=build late-callback-negative
make -f Makefile -f build/wiring-next/late-callback/probe.mk BUILD=build late-callback-positive
```

The negative target is a prerequisite of the positive target. The original
returns 1 for each of modes 0–2 with these precise failures:

```text
FAIL: late callback navigation consumed before indefinite park
FAIL: late inserted script executed before indefinite park
FAIL: late callback navigation consumed before indefinite park
```

Each original failure occurs at `wait_ms=0`, 4 polls, after the listener ran.
Final `late-callback-positive` exited 0: patched modes 0–3 all passed.
Navigation modes 0/2 issued exactly one destination request before park (5 polls);
script mode 1 completed before park (4 polls). `git apply --check` also passed
against the tree as read at handoff; that does not replace the other group's review.

Logs are `original-0.log` through `original-2.log`; success logs use the `patched-`
prefix. The final run also checks initial-scroll mode 3 as a positive control.

This is an isolated host control-flow result, not guest installation or visual
acceptance. No disk image, QEMU run, permanent CI gate or integration with the
other group's latest changes is claimed. The removed `!navigated` guard and
console/status repaint classification were reviewed statically; this fixture
does not independently isolate those two branches.
