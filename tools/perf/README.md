# tools/perf — measuring LogitOS instead of arguing about it

The owner's verdict after a day of parallel work was **"感觉变卡了"** — it got
laggier. That is a report, not a number, and this tree has been wrong every
single time it settled a performance question by reasoning:

- mini-libc's allocator was O(N²) and nobody knew until it was measured;
- "emulation is why JavaScript is slow" was wrong by two orders of magnitude,
  and wrong about *which thing* was slow — TCG costs 8.5× on integer work while
  compile cost 609× and allocation 3092×, both allocator-bound.

So this directory contains an instrument, not an opinion.

```
perfbench.py         one boot, every metric, median + spread
sweep.py             the same harness across a list of commits
bisect.sh            the `git bisect run` form
plant-regression.sh  proves the harness can detect anything at all
fixture/bench.html   the page the page-load metric loads
```

```sh
make test-perf                              # report
PERF_METRIC=read_net_ms PERF_THRESHOLD=1400 make test-perf-gate
python3 tools/perf/sweep.py --commits shas.txt --rounds 3 --reference <sha>
bash tools/perf/plant-regression.sh         # the self-test
```

## What is measured

| metric | what it is |
| --- | --- |
| `boot_ok_ms` | reset → `LOGIT_BOOT_OK`, host clock (includes GRUB, constant) |
| `desktop_ms` | reset → `[wm] desktop live` |
| `kboot_ok_ms` | the same, from klog's own timestamps — only where `/dev/kmsg` exists |
| `shell_net_ms` | 24 fork+execve round trips of `/bin/true` |
| `read_net_ms` | 6 reads of a 2.2 MB font off logitfs (`wc`) |
| `launch_net_ms` | 8 loads of `/bin/as`, a 270 KB mini-libc-linked image, running a trivial script |
| `page_net_ms` | 8 fetches of a fixed host-served page: DNS + TCP + HTTP + body |
| `mouse_tax_ms` | what a moving pointer costs concurrent work — see below |
| `floor_ms` | the harness's own cost, measured twice per run |

`*_net_ms` is the raw phase minus `floor_ms`. The raw forms are kept too,
because the subtraction is an assumption and assumptions should be visible.

**Not the browser.** A GUI `.aex` execve'd from a tty has no window and never
returns — probed, it hangs at the prompt — so "browser launch" cannot be a
phase. Its two honest components are measured separately instead: the loader
path (`launch_net_ms`) and reading megabytes off logitfs (`read_net_ms`).

## Why the numbers are trustworthy

The host runs other agents' QEMU instances. Measured here, `read_ms` moved
**2.3×** — 765 ms to 1750 ms — between two runs of the *same ISO*, purely
because a build was running alongside. Four defences, in order of importance:

1. **Nothing is paced from the host inside a measured window.** A phase's whole
   command block is written to the serial socket in *one* write before the
   guest starts executing it, so the guest never sits at the prompt waiting for
   the harness to type. QEMU's 16550 applies flow control, so a large blob is
   throttled rather than dropped.

2. **Every interval is bracketed by two markers the guest emitted**, dated by
   when their bytes arrived. Where `/dev/kstat` exists the guest's own
   `uptime_ms` is recorded alongside as a cross-check — but only the last few
   hours of 2026-08-07 have that device, so the host-clock marker is the
   primary and the only one that works across the whole day.

3. **Repetition and a median**, with MAD rather than stdev: on a contended box
   with 3–5 samples, one starved run must not be allowed to define the spread.
   Each phase also repeats its unit of work K times so that the guest clock's
   10 ms granularity is under 1% of the window instead of 25% of it.

4. **`sweep.py` benchmarks a pinned reference build in every round** and reports
   ratios to it. A ratio cancels whatever the host was doing in that minute. A
   sweep that visits commit A at 10:00 and commit Z at 11:20 and compares their
   absolute numbers is measuring 10:00 against 11:20.

## Two confounds that produced confidently wrong answers

Both were found by the harness disagreeing with itself, and both are worth
knowing about beyond this directory:

- **The first phase after the prompt was not measuring itself.** The waitq
  self-test, the time cross-check and the IPv6 DAD timers all still fire for
  several seconds after `/bin/sh` appears. The floor read 190 ms; after a
  discarded warm-up it reads 20–30 ms. Everything measured in that window was
  inflated by an order of magnitude on the floor.

- **The pointer metric reported the mouse making the machine 30–40% *faster*,
  every time.** Not noise — a steady sign error. The cause is not compositing:
  `tty_read` blocks in `hlt` and is woken by the next interrupt, so with the
  pointer still it waits up to a full 10 ms timer tick per character, while a
  moving pointer delivers PS/2 IRQ12s that wake it immediately. **The shell
  round trip is tick-latency-bound**, so any interrupt source at all speeds it
  up, and the compositor's cost was invisible underneath it. The pointer phase
  now runs a file read instead, and is bracketed by two identical phases either
  side of it so it is compared against its neighbours rather than against a
  phase measured six phases earlier (later phases are systematically faster —
  warm cache, self-tests finished).

## "Slower" and "did not build" are different findings

A commit that does not compile is recorded `BUILD-FAIL` with the stage that
failed and the last error line, and is never given a number. `bisect.sh` exits
**125** (git bisect's *skip*) for it. A harness that scored a build break as
"bad" would name the wrong commit with total confidence.

This is not hypothetical. **23 of 2026-08-07's commits — 22:39 to 23:45 — build
an ISO and fail to build the disk image**, because the Makefile referenced
`c/apps/audio/audiocheck.c` 66 minutes before that file was committed. Since
every ring-3 program lives on the disk image, a harness that stopped at `make`
would have benchmarked one commit's kernel against a stale userland and never
noticed.

## The harness must be able to fail

`tests/qmp/qmp_freeze.py` in this repo passes on broken and working builds
alike, because its coordinates rotted and nobody ever checked that it could
still fail. So `plant-regression.sh` builds a synthetic history in a throwaway
clone — several commits, exactly one of which inserts a busy wait on the VFS
read path — measures both ends to *derive* the threshold rather than hardcode
one, and runs `git bisect run bisect.sh` over it. It exits 0 only if the bisect
names the planted commit. If the plant does not move the metric it says so, and
that is a finding about the metric, not a flaky test.

The harness lives outside the tree being bisected (`PERF_TOOLS`), so a bisect
can never end up measuring changes to its own instrument.
