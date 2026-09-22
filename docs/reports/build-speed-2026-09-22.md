# Build acceleration — 2026-09-22

The normal build now uses optional persistent compiler caching, bounded parallel
jobs on GNU Make 4.4+, compiler-generated header dependencies, and separate
compilation/link targets. Product flag and compiler changes are tracked by
configuration domain. The implementation lives in `tools/build/`; usage is in
[Building LogitOS](../BUILDING.md).

## Results

| Check | Observed result |
| --- | --- |
| Full product build | `gmake BUILD=build-fast all build-fast/disk.img` completed, including real application links and disk packing |
| Subsequent GNU Make 4.4.1 build | 0 C compiles, 0 disk packs |
| Subsequent Apple Make 3.81 build in the same directory | 0 C compiles, 0 disk packs |
| Browser `html/entities.inc` dependency change | Old rule: 75 C compiles; current rule: 1, `html/tokenizer.o` |
| Three real cached objects | 3/3 cache hits; object and dependency-file bytes match direct uncached compilation |
| Missing dependency file | Only the affected object rebuilt and restored its dependency file |
| Library-only update | Studio, Preview and Terminal relinked without recompiling their unchanged main sources |
| Automatic job scheduling | 12 concurrent fixture workers by default; explicit `-j1` retained 1; recursive `JOBS=3` shared 3 |
| QEMU smoke | `LOGIT_BOOT_OK`, 130 trust roots, named `isrg_x1` marker |
| Packed Browser identity | `/browser.aex` exactly matches the built 6,992,008-byte artifact |

The Browser comparison uses real compiler-produced `.d` files and `make -n -W`;
it counts scheduled compiler invocations, not elapsed time. The old rule is
restored in a temporary Makefile copy at its original position. Appending the
rule to the end instead gives **127**, an invalid baseline: `tests/mse.mk` later
adds 52 audio/video/media objects to `BROWSER_OBJ`. Make expands a rule's target
list when it reads that rule. No source timestamps were changed for this check.

Cache verification covered the Browser tokenizer, liquid-glass renderer and
kernel SHA-256 object. Each was rebuilt once with `CCACHE=` and once with the
cache enabled; all six `.o`/`.d` files matched their recorded hashes. A further
invocation compiled nothing.

Switching Make implementations initially invalidated 24 domains because Make
3.81 appended a space for an empty `UCFLAGS += $(UEXTRA)` while 4.4.1 did not.
Guarding that empty append fixes all 43 actual product configuration snapshots.
The configuration helper deliberately does not collapse whitespace: separate
options `(1, empty)` and `(empty, 1)` must retain different fingerprints.

## Regression checks

```sh
gmake test-build-incremental test-build-config test-build-jobs test-mk-wired
bash tests/boot/run-test.sh build-fast/logit.iso build-fast/disk.img
```

All passed. Each new positive gate requires its negative control and is wired
into `ci-host`. Observed control failures included:

- Restored broad dependencies: `compile set differs; extra=[...]`.
- Restored inline main compilation: unnecessary Studio/Preview/Terminal compiles.
- Removed configuration edges: `FAIL: kernel flag change kept stale object` on
  both Make implementations.
- Disabled automatic job selection: `expected peak 3, got {'active': 0, 'peak': 1}`.

The configuration checks also cover flag changes in both directions, compiler
replacement, quoting, interrupted and partial builds, failed compiles, same-tree
Make switching, and independent empty option slots. The job checks exercise 44
independent graphs. The wiring audit reports 390 fragments, 389 reachable and
one documented recursive-wrapper exception.

Small machine-readable results and logs are retained in
[the evidence directory](evidence/2026-09-22/build-speed/).

## Boundaries

These checks establish fewer unnecessary compiler invocations, working cache
reuse, correct worker limits, successful full builds and a kernel boot smoke
test. No wall-clock speed ratio or application-runtime performance improvement
was measured. The smoke test does not exercise every GUI feature.

Disk packing and sysroot staging retain their existing implementation. Disk
preservation, validation and active-VM guards were not weakened. The independent
verification disk did not replace the user's preserved original disk backup.
