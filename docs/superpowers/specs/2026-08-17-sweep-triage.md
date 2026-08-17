# The full sweep, triaged — what 522 targets actually found

Written while the device half of `make test-sweep` was still running (477/522).
Every entry below was diagnosed from the recorded logs, without a build, which
is why the split between "harness fault" and "product fault" is the useful axis:
the harness faults were fixable immediately and the product faults are queued
behind a boot.

**The headline is uncomfortable and it is the point of running this at all:
of the failures diagnosed so far, MOST WERE THE TEST, NOT THE SYSTEM** — and
several of them were confidently, specifically wrong in a way that would have
sent somebody after the wrong bug.

## The pattern that repeated five times

> **A control is a COPY of the thing it controls, differing deliberately in one
> place, and nothing in make or in C expresses "these two must stay identical
> except for that one difference".**

So they drift, and nothing notices, because a negative control is reached by no
aggregate suite — all of them sit in the 354 `UNWIRED` targets.

| where | drift | how it presented |
|---|---|---|
| `browser-noplat.elf` | `$(GFX_OBJ)` missing | 20 undefined `gfx_*` at link |
| `browser-noplat.aex` | `--stack-pages 2048` missing | **silent** — the control got a fraction of the real stack, and "comes up with none of the platform APIs" is satisfied by a control that died on a deep render stack |
| `test-wpt-negctl` | `$(GFX_SRC)` missing | `undefined reference to gfx_path_ellipse` |
| `test-wpt-fire-negctl` | the same | the same |
| `test-encoding-negctl` | the same | the same |

The sentence explaining the last three was **already a comment seven lines above
one of them**, written when the POSITIVE target was fixed.

`js_events.c` is the same disease in C rather than make: the positive install
path is explicitly loud, with a comment arguing that a prelude which fails to
install "leaves every page with the pre-existing event surface and nothing says
so". Twenty lines below, the negative control's install swallows its exception.

## Harness faults — the diagnosis was wrong, not just the verdict

### `test-shape-device` — eight failures about Arabic, none of them about Arabic

Reported "isolated forms are about half again as wide as joined ones" for four
Arabic strings. The screendump is the desktop with **Preview playing
sample.aac**; TextEdit never launched and `shaping.txt` was never displayed.

The tell was in the numbers: every row measured `1 ink groups, 444 px wide` — 7
code points, 13 code points, and a four-letter Hebrew word, all exactly 444. **A
measurement that returns the same answer for inputs of different lengths is not
measuring the input.** Converting the .ppm and looking at it settled it in a
minute.

Cause: the drive clicks a hardcoded (594,215), "the third icon of the first
row". That is a guess about disk contents; `fsroot/` has eight entries and the
grid renumbers when any of them changes.

Fixed by making the harness check WHAT IT OPENED before measuring — the kernel
already prints `[wm] launched <name>` and this harness was already reading that
serial log for its font report.

### `test-monitor` — the harness buried the bug it found

Eight checks pass. The ninth is real: **launching Clock kills the Activity
Monitor window.** Then the harness continues, `Frame().ox` is None because the
window is gone, and the run ends in a `TypeError` traceback pointing at the
harness. A reader sees a broken test; the finding is one line above it.

Fixed with a `fatal=True` on checks everything after them depends on.
Accumulate-and-continue is right for independent checks and stays.

### `test-glass` — 26 failures, and the oracle was wrong

All 26 are one boundary: `fresnel s=0`, `got 190, double says 255`.
`glass_schlick` clamps to `GLASS_FRES_MAX = 190`, argued over twenty lines: it
models the microfacet roughness of a frosted panel, because a polished lens
reaches R=1 at grazing incidence and an anodized one does not. The oracle
compares against **uncapped** Schlick.

Not fixed by teaching the oracle to clamp and leaving it there — that is a pure
weakening. The implementation's comment claims the cap "only ever clips the
single outermost ring", which is checkable, so it is now checked. **Verified
first: over every E in 1..24 the largest uncapped value at s >= 1 is 55.737
(E=24, s=1) against a cap of 190 — a margin of 3.4x.** That number is also the
abruptness the design is about: 255 to 55.7 across ONE pixel is a hairline, and
a gradient would not read as an edge.

### `test-kbench` — a gate on a syscall nobody calls, and a message naming nobody

Two failures.

`no SYS_GET_TIME line in the syscall histogram` is **already documented as
unfixed** at `kbench.c:416`: the harness reads SYS_GET_TIME's mean out of a
top-N list, "SYS 10 is absent because it is not called in the window at all,
which is a separate and older problem".

`247 poll passes through bkl_hlt_wait (limit 200) -- something is still polling
instead of blocking` is a real measurement over a real threshold, wrapped in a
message that names no culprit and therefore costs a debugging session every time
it fires. The tree already knows the answer to this shape of question:
`spinlock_t` records the acquiring caller's return address, and that is what
turned "the BKL is contended" into "`wm_run+0x2de` holds it 80% of the time". A
16-slot return-address histogram on `bkl_hlt_wait` is staged.

(Ruled out on the way: `proc.c:546` carries `bkl_hlt_wait(); /* the old poll;
run-kbench.sh must FAIL */` — but it is inside `#ifdef KBENCH_NEGCTL`. A proper
control, not a leftover.)

**And then the same instrument at three core counts bracketed the answer, which
changes what the finding probably IS:**

| target | cores | poll passes | verdict |
|---|---|---|---|
| `test-kbench-1core` | 1 | **52** | PASS |
| `test-kbench` | 4 | 247 | FAIL |
| `test-kbench-5` | 5 | 239 | FAIL |

About fifty per core, against a budget of 200 that does not mention cores. So
the likeliest reading is not "something is still polling" but **"the budget was
set on a smaller machine and never scaled"** — and the failure text asserts the
first reading in so many words. Which of the two it is, is exactly what the
staged return-address attribution answers: fifty passes from one site on every
core is a per-core idle path and the threshold is wrong; fifty from a scattering
of sites is a real poll. Nothing here gets changed until it says which.

Worth stating as a method rather than a result: **the negative control was not
the only control available.** Running the same gate at 1, 4 and 5 cores cost
nothing extra — all three were already in the sweep — and it separated a
threshold problem from a code problem before a line of kernel source was read.

### Not failures at all

`test-net-ab` wants `BEFORE=<other.iso>`; `test-perf-gate` wants `PERF_METRIC`.
Both printed their usage and exited nonzero, and both would have been FAIL in
every future sweep — two permanent entries in a list whose only value is that
everything in it is worth reading. Now `NEEDSARGS`, from a table that states the
argument each one wants.

## Product faults — real, and queued behind a boot

| target | finding |
|---|---|
| `test-monitor` | launching Clock kills the Activity Monitor window. **The obvious harness explanation was checked and ruled out** — after `test-shape-device` turned out to be a stale hardcoded click, the first suspicion was that the dock grew (gallery and settings are new, and they are what broke `test-fullsystem`) and the click had drifted. It has not: `qmp_monitor.py` DISCOVERS the dock's app count rather than assuming it, prints a note when it differs from `qmp_ui.NAPPS`, printed no such note, and `CLOCK_SLOT = 0` is `clock.aex`. It clicked Clock, and Monitor died |
| `test-live-page` | clicking a link does not run its handler |
| `test-browser-https` | the empty Browser viewport is not blank — 3902 ink px, and the check calling it is a control |
| `test-preview`, `test-preview-timing` | Preview lists `/media` as 0 entries, though the fixtures are on the disk (`FS_FILES` adds them, `tests/preview.mk` adds the prerequisite line correctly, and the names are present in `disk.img`). Two targets, one cause |
| `test-kbench` | 247 poll passes where the budget is 200 |
| `test-events` | **passes for the wrong reason** — see below |
| `test-h265-b` | `got 79 want 80`, one level, in B slices; declared incomplete |
| `test-desktop-os` | **a logged-in user cannot save a setting** — see the section below; the largest of these |
| `test-mse-os` | playback STALLS. `step` keeps climbing while `t_ms=4066 decoded=60 shown=59 segs=4+5` never move — and the signature is specific: one frame decoded that the presenter never showed, then nothing |
| `test-reftest` | 16 regressions against the ratchet, **and 52 newly passing** — the shape of a real feature landing. They cluster to about nine causes: four `gap-004-*` writing directions all 6140 px wrong, three `column-auto-repeat-auto-*`, three `mask-image/*`. `css-viewport/zoom/svg-stroke-width` points straight at the phase-2 stroke work |
| `test-wpt` | 20 new failures out of 246,542 subtests, over about five causes: eight in `css-anchor-position`, five subtests of one `contain-size-grid-003.html`, two popovers |
| `test-frameworks` | the corpus no longer matches its BASELINE — **because Vue and webpack now render**. An improvement reported as a failure; the baseline needs blessing, deliberately, with the diff read |

### `test-events` is green and its green is not evidence

`test-events-negctl` reports: *"the ordering suite PASSED with the propagation
walk stubbed out, so it is not measuring ordering."* The captured log says
**10/10 subtests passed** against the stubbed build, so the tests ran and all
passed anyway.

`tests/events/order.html` is a genuine ordering test — it asserts
`['aC:1','bC:1','c1:2','c2:2','bB:3','aB:3']` and six more orderings. It cannot
pass with the walk removed. So the walk was **not removed**, and the likely
mechanism is visible in the stub:

```js
var EP = null;
try { if (doc && doc.createElement) EP = Object.getPrototypeOf(doc.createElement('div')); } catch (e) {}
patch(EP); patch(doc); patch(G);
```

`patch(null)` returns immediately. If the Element prototype cannot be reached at
install time, only `document` and the global get patched — and every listener in
`order.html` is on an ELEMENT, whose `addEventListener` and `dispatchEvent` stay
NATIVE. Combined with the swallowed install exception, the control degrades
silently into a copy of the real build.

**So the control's conclusion is a correct observation with the wrong culprit
named**, which is the third instance of that exact shape in this sweep. Staged:
the stub throws if it cannot reach the Element prototype, and the install aborts
loudly rather than reporting a fiction.

### `test-fullsystem` — a list derived so it could not go stale, went stale

The guest listed eleven `.aex` at the disk root and the expectation held nine.
The guest was right. `ROOT_AEX` was built from `$(APPS)` under a comment saying
precisely why: "Derived from `$(APPS)` rather than restated, so adding an app
extends the test automatically instead of silently leaving the new one
untested." `gallery` and `settings` are packed from their own variables, so
**the assertion failed by naming the two apps it had itself forgotten to
expect** — which reads like a disk problem.

Now derived from `$(ROOT_AEX_PACK)`, the single list the disk rule itself packs
from. Verified behaviour-neutral: the `mkfs` command line is byte-identical
before and after.

### `test-forms-negctl` — a control built with flags the real build fixed a week ago

`browser.c:2314: expected identifier` — `it[i].hidden` expanding to
`__attribute__((visibility("hidden")))` out of musl's internal `features.h`.
Commit 339298854 (08-09) is titled *"browser: `hidden` is a macro, so struct
item lost a field and seven sites crashed"*, and fixed it with a target-specific
override on `$(BROWSER_JS_OBJ)`. `build/nofocus/browser.o` is not in that list.

Two near-misses worth recording. The error implicates `-DBROWSER_NO_FOCUS` by
position and has nothing to do with it — replaying the command without that flag
fails identically. And it briefly looked as though HEAD could not rebuild
`browser.c` at all, which would have been serious; the `.d` file settles it,
because the real object does not depend on `features.h`.

### `test-leak-os` — not runnable bare, and says so

`the app-churn driver did not run (only 1 launches). Build with CHURN=1.` The
Makefile documents it: *"REQUIRES A CHURN BUILD"*. Same category as `test-net-ab`
and `test-perf-gate` — a target that was never callable on its own — except that
what it needs is a whole build rather than an argument.

## The one that matters most: a logged-in user cannot save a setting

`test-desktop-os`:

```
FAIL: boot 2: A USER COULD NOT SAVE A SETTING. This is the original
      regression: settings_commit() refused because the file was root's
```

**The failure is real and the explanation in it is not.** The same log says

```
SETTINGS_USER uid=1000 store=/home/alice/.config/settings.conf defaults=/etc/settings.conf keys=1
STAT /home/alice/.config/settings.conf mode=600 type=reg uid=1000 gid=1000 size=117
```

— the file exists, alice owns it, it has content. Nothing was refused for
ownership. `SETCHECK-SET-FAIL ui.dark` is the actual event, and there is no
`[set] commit FAILED` line at all, so the refusal happens before the write.

It is `SYS_SETTING_SET`'s permission gate, and the gate is asking the wrong
question:

```c
if (settings_schema_find(key)) {          /* a schema key is machine state */
    ...
    if (me.uid != 0) return ID_E_PERM;
```

`ui.dark` is schema entry 75, group `SET_G_APPEARANCE` — an appearance
preference, exactly the kind a user is supposed to own.

**Two features, each correct alone, contradicting where they meet.**
`settings_prepare_user()` points the store at `$HOME/.config/settings.conf` so a
user's schema values override root's defaults *under a file they own* — that is
the entire per-user settings feature. The M28 gate closes a real hole ("any
process could rewrite machine-wide persistent configuration"), and its premise —
"a schema key is machine state" — is true of the system store and stops being
true the moment a session is active.

So the test should be **where the write lands**, not what the key is called, and
the discriminator already exists and is already used by
`settings_store_path()` and `settings_commit()`: `user_path[0]`.

`SETCTL_RESET` inherits the same correction, and its own code already argues the
point: `settings_reset()` deletes `settings_store_path()` — the user's file when
one is active — "so that a user resetting their settings must not be able to
delete root's defaults". That function was written user-aware; only the gate in
front of it was not, and between them a logged-in user could not reset settings
that were hers to begin with.

Staged, not applied — it is kernel source and the sweep's device half is
running. The refusal must survive for the system store, which is what the
negative half of the gate is for.

## `test-frameworks` is red because two frameworks started working

The gate's own header says what it is:

> It is a CHANGE DETECTOR, not a wish list... the target is green today and goes
> red the moment a Web API lands — which is exactly the acceptance check
> `webapi_probe.c`'s header asks for: *"when a name from channel 1 is
> implemented, the channel-2 error for that page must change, and if it does
> not, the implementation did not matter."* When it fires, update BASELINE in
> the same commit and say which cause moved.

It fired. Which cause moved:

| | baseline | 2026-08-17 |
|---|---|---|
| **vue** | 3 causes, `SVGElement (global constructor)`, `#app=0`, no button | **0 causes**, `#app=106`, button reads `"count is 0"` |
| **webpack** | 1 cause, `document.currentScript`, `#app=0` | **0 causes**, `#app=148`, button reads `"count is 0"` |
| svelte | 5 causes, `HTMLTemplateElement.content`, `#app=0` | unchanged — `template.content` is still undefined |

`SVGElement` now reports `ctor` rather than `undefined` in the missing-feature
list, and `document.currentScript` has left it entirely. **Two frameworks went
from a blank page to a rendered, interactive one**, and the acceptance check the
programme set for itself — that implementing a name must change that page's
error — is met rather than refuted.

Blessing the baseline needs a run, so it is queued behind the sweep. The
important half is not the bless: it is that a red target here means the
opposite of what red usually means, and the harness says so in its own header
for anyone who reads that far.

## What the sweep CONFIRMED — the half that produces no findings

A sweep's other output is that standing claims are still true, and nobody writes
that down because there is nothing to fix. Checked against today's logs:

| claim in CLAUDE.md | measured 2026-08-17 |
|---|---|
| Storage: "12 targets, all green" (dated 2026-08-08) | **all 12 green again**, incidentally, nine days and much unrelated work later |
| `test-crypto-diff`: "140,214 differential cases" | `total pass 140214 fail 0` — exact |
| `test-webp-vp8`: "31 cases, every one byte-exact" | `31 lossy-WebP cases, 0 failed` |
| the trust store: "a 130-root bundle" | `roots: 130 compiled in` |
| `test-jpeg`: "maxd=0 on all 13 decode cases" | 13 `maxd=0` lines of 14 cases — exact |

**The JPEG row is here because I nearly broke it.** The log says "all 14 JPEG
cases passed" and the document says 13, which looks like drift. It is not: 13 of
the 14 report `maxd=0` and the fourteenth is not a pixel comparison. Counting
before editing is the difference between correcting a document and damaging one.

The contrast with the test-suite section is the finding. That section's numbers
had drifted from 522/352 to 594/359 while these are exact to the digit — and the
difference is what they measure. A count of *the tree's own targets* changes
every time anybody adds a test; a count of *cases inside one gate* changes only
when that gate is edited, and its author edits the number in the same commit.

## Instrument faults found in the sweep itself

- **A failing target was deleting its siblings' results.** `grep -v -f
  recheck.txt` reads target NAMES as unanchored regexes. Replayed on this run's
  real data, one failing `test-fs` deletes all six recorded `test-fs*` rows,
  including two other targets' passes, and nothing re-runs them. The duplicate
  confirmation pass was deleted in favour of the one that filters positively on
  the status field.
- **`make test-sweep` exited 0 no matter what.** `sweep-confirm.sh` ends with
  `grep -v ... | sort | sed`, and a pipeline's status is its last command's. A
  sweep of 525 targets that could not fail. Found by `tools/audit_tests.py`
  under MUTE — the instrument built to find that shape elsewhere, finding it
  here. The gate now lives in `tests/sweep.mk` and judges the result file.
- **One phantom row**, `st-h264-pts`, a target nobody wrote, with the real
  `test-h264-pts` missing its result. Mechanism never identified. `run_one` now
  refuses a name that is not in the enumerated list, because a phantom row lies
  twice — a failure that is not one, and a target that never ran.

## The audit, 388 findings -> 14

`DEAD` now computes the reason it already demanded ("another harness drives
it"), transitively from Makefile roots outward — 33 -> 14. `UNWIRED`'s 354 are
recorded by name as debt with a growth gate, because "ci runs 22 of 588 targets"
is true and should keep being said, while 354 findings made the other two
categories unreadable.

**And every reachability gain disclosed muteness.** Dead files are excluded from
the MUTE check, so deadness HIDES muteness: fixing the walk surfaced three
verdicts, wiring the lock instruments surfaced two more. It happened three times
in one pass and it is not a coincidence.
