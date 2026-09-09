# AGENTS.md — the operating rules for any model working in this tree

`CLAUDE.md` is the map: what this machine is, subsystem by subsystem, with the
numbers and the dates they were measured. **Read it before you touch anything.**
This file does not repeat it. This file is the short list of things that will
cost you a day if you get them wrong, and it is written for a fresh agent with
no history here.

Several agents share this workspace at once. Everything below assumes that.

---

## 1. The comment is the deliverable, and here is the exact standard

This is the rule that most often gets skipped, and skipping it is how this
codebase stops being navigable. **A patch with correct code and no reason is
half a patch and will be sent back.**

Not every line needs a comment. Ordinary code that reads as what it does needs
none. What must be written down is **the thing the next reader cannot recover
from the code**:

- **why this and not the obvious alternative** — the alternative you rejected,
  and what it would have cost
- **the measurement** — the number, and how it was taken. `"64 MiB took 133 M
  iterations (0.068 s), 512 MiB would take 8.5 G (4.46 s)"` is a comment. `"this
  is faster"` is not.
- **the trap** — what a reasonable person would do here that is wrong, and how
  it fails. Prefer failure modes that are SILENT; those are the ones worth ink.
- **what you deliberately did NOT do**, and why. An absent feature with a stated
  reason is a decision. An absent feature with no reason is a bug nobody has
  found yet.

Three comments already in this tree, quoted so the standard is concrete rather
than described:

> `js_platform.c` — *"Two APIs are deliberately ABSENT rather than half-built and
> would be door eight if either is ever added … The rule if either lands: it
> must call through insert_run/insert_markup or be added to this file's wrap
> list in the SAME COMMIT, not after."*
> That comment was written before the API existed. Months later somebody added
> it and did the right thing **because the file said so**.

> `css_extra.c` — *"a property added to one of those producers is invisible to
> @supports until it is added here too."*
> Then exactly that happened, inside the file that documents the risk. The
> comment did not prevent it; it made the diagnosis take four minutes.

> `cpu_report.c` on why AVX is off — *"isr.asm wraps every C handler in
> FXSAVE/FXRSTOR, which saves x87 and XMM0-15 and nothing else — enable AVX
> without migrating to XSAVE and every interrupt silently truncates the top 128
> bits of every YMM register."*
> Without it, someone re-enables AVX and rediscovers this as data corruption.

**When you correct something this tree already believed, keep the old claim
beside the correction.** `CLAUDE.md` does this deliberately: *"where that
happened the correction is kept beside the old claim rather than quietly
overwritten, because somebody is going to arrive holding the old sentence."*

---

## 2. Five rules this tree paid for by losing a day each

**1. Suspect the apparatus first.** More failures here are the test than the
system. Cheap checks: does the number change when the input changes? Does the
control fire? Is the file it read the file you edited? **Is the harness looking
at the machine, or at itself?**
Real instances: a probe binary that did not link the file it was asked about
reported a whole live subsystem absent; a parser printed `0 samples over 0
sites` directly under a header saying 8,471 samples were taken; a driver looked
for the mouse cursor in a screendump on a machine that puts it on the hardware
cursor plane, so it is not in the picture at all.

**2. If you read the Makefile, join the continuations first.**
`re.sub(r"\\\r?\n[ \t]*", " ", text)`. Six tools here do it and each says why.

**3. One jar, two doors.** A constant spelled in two places agrees on the wrong
value about as often as the right one. `/dev/log`, `LOGIT_ARG_MAX` (one end
truncated at 32, the other at 48, each silently), the IME chord in eight
literals. **Derive it, or make one place authoritative.**

**4. A gate nobody runs is a gate that rots.** If you add a `tests/*.mk`
fragment, `make test-mk-wired` must stay green. If you add a negative control,
make it a **prerequisite** of its positive counterpart — naming it on a `ci-`
line satisfies the audit and runs it never, which is worse because it looks
fixed.

**5. A control that cannot be watched failing is worse than no control**,
because it reads like one. **Build the control, break the thing, watch it go
red, then fix it.** A green test that has never been shown to fail is not
evidence.

---

## 3. Absent beats present-and-wrong

If you cannot implement something correctly, **leave it absent and say so in a
comment**. Do not stub it to a plausible value.

The corpus proves this rather than arguing it — `tests/fixtures/jsperf/baidu-async-search.js`:

```js
var o = a.getContext === i ? !1 : a.getContext("2d");
if (o === !1) return !1;
```

With the method **absent** the page takes its fallback cleanly. With a
null-returning method the guard does not fire and the page walks on holding
null. Every feature test on the web has this shape.

Corollaries that are already house rules:
- `flock` returns `ENOSYS` because a caller that gets `0` believes it holds a lock.
- `statvfs` returns `ENOSYS` because a fabricated `f_bsize` is worse for a
  caller sizing a buffer than an error is.
- `canvas.toDataURL` threw for a year rather than fabricate bytes.

---

## 4. Building and testing, including the two that waste an afternoon

```sh
make                 # the ISO — the KERNEL ONLY
make build/disk.img  # what actually rebuilds a ring-3 program
make run             # QEMU, 1 GiB, virtio-gpu
make test-mk-wired   # every tests/*.mk is reachable
```

- **`make` alone does NOT rebuild an app.** Editing `c/apps/**` and running
  `make` prints "Nothing to be done" and leaves the old binary on the disk
  image. `make build/disk.img` is the check.
- **`BUILD=` is overridable and reaches the recipes.** Use `BUILD=build-<yours>`
  so several agents can build at once without manufacturing each other's
  failures. `make clean-scratch` removes those trees; it does not touch `build/`.
  **Correction (2026-08-30, kept beside the original): as a COMMAND-LINE
  assignment (`make BUILD=build-<yours> target`) the sentence is exactly true;
  as an ENVIRONMENT prefix (`BUILD=build-<yours> make target`) it is not.**
  The Makefile says `BUILD := build`, and a plain `:=` silently beats an
  inherited environment variable while a command-line assignment beats the
  `:=` — the env-prefix form builds straight into `build/` and every artifact
  "vanishes" from the tree you thought you were using (found by an SSH
  attack-testing session chasing a phantom artifact-sweeper for half an hour;
  there was no sweeper, there was one `:=`).
- **A link line is only proven by a link that runs.** Timestamps cannot answer
  this: `rm` the binary and ask for it back. Two `.elf` files here were
  unlinkable for weeks while `make` correctly said "up to date".

**Host reality (macOS / Apple Silicon).** These take down gates for reasons that
have nothing to do with the code under test:
- `<string.h>` makes `memset`/`memcpy` **macros** at every `-O`. Declaring them
  as bare externs after a host `<string.h>` fails to parse. Guard with `#ifndef`.
- **`__attribute__((weak))` on a declaration is an ELF idiom.** On Mach-O an
  undefined weak symbol is a **hard link error**. Use `include/weaksym.h`, and
  register every new `*_install` with `LOGIT_WEAK_STUB(name)` **in the same
  commit as its call** — one missing line reddened fourteen host gates at once.
- **Hand-copied source lists** (`PROBE_SRC`, `CANVAS_SRC`, …) drift when a file
  grows a dependency. Prefer a subtraction from an existing variable.
- **BSD vs GNU**: `stat -f %z` not `-c %s`; stock `bash` is 3.2.57 where
  `"${arr[@]}"` on an empty array is fatal under `set -u`.
- **A gate that cannot run on this host must SKIP LOUDLY** — one line naming the
  missing capability and the command that would settle it — and never pass
  silently. A gate that fails for an unrelated reason trains people to ignore
  red.

---

## 5. Measuring: what counts as evidence

- **Measure in the guest** for anything about what the browser or the OS can do.
  Host probes here do not link every translation unit the real binary links, and
  that has produced published wrong answers more than once.
- **Never measure wall clock on the host.** Other agents run QEMU concurrently.
  `tools/perf/` times everything inside the guest and says why.
- **"The API exists" is not a result.** The scar: `make test-wpt
  ONLY=css/css-grid` read `531/11152` **with and without** the grid
  implementation, because the runner linked the translation unit and never
  called it. Show a real page behaving differently.
- **Count the right thing.** Painted text runs, frames *shown*, captured PCM —
  not changed pixels, not a clean log. A stall and a black frame are the same
  screenshot; a silent sound card and a working one have identical registers.
- **A number that falls can mean things improved.** A file that never ran
  contributes no subtests; revive it and it contributes its failures too. Say so
  rather than hiding it.

---

## 6. Do not

- **Fit a site.** Nothing may branch on a hostname, URL, framework, library or
  bundle name. Sites are specimens; the deliverable is the general defect they
  expose. This is the owner's standing instruction and it is not negotiable.
- **Mimic another browser or spoof any signal** to please a checker. An honest
  browser that identifies itself and is refused is a correct result.
- **Delete a gate to make a build green.** Re-point it or delete it *with its
  reason*, in the same commit. A target that resolves to nothing is worse than
  one that fails.
- **Edit a file another agent is working in.** Check `git status` and mtimes
  first. Ten minutes old is somebody else's. If your fix belongs in a contested
  file, write the patch into your report instead — **but check the mtime rather
  than assuming**: deferring on stale ownership has already cost this tree an
  un-applied fix.
- **Commit with `git add -A`.** Path-limited commits only; the workspace is
  shared and the scoreboard writes megabytes of run output.

---

## 7. What "done" looks like

A change is finished when all of these are true:

1. It builds — and if it touches a ring-3 program, `make build/disk.img` builds.
2. Its gate is wired (`make test-mk-wired` green) and its negative control is a
   prerequisite, not a suggestion.
3. **The control was watched going red**, and the report says what it printed.
4. The measurement is from the guest where that matters, with before and after.
5. The comment says why, what was measured, and what you deliberately did not do.
6. Nothing outside your assigned files changed.

If any of these cannot be met, **say which and why**. An honest partial result
with the gap named is worth more than a complete-looking one, and this tree
would rather have "I could not make the boundary real, so I kept the refusal"
than a feature that looks finished.
