# Writing tests

LogitOS tests exercise production code on the host, run programs inside QEMU, and inspect desktop interaction through the QEMU Machine Protocol (QMP). Test sources and fixtures live under `tests/`; the root `Makefile` and included fragments provide their entry points. A regression test should connect an observable result to the changed code and use a negative control to demonstrate that its assertions detect the defect. `AGENTS.md` sets this evidence standard. Run the commands below from the repository root.

## Choose the observation

Choose the environment that exposes the behavior being changed. A host fixture supplies controlled inputs; a guest test exercises integration with the running system.

| Kind | Example | Observation |
|---|---|---|
| Host unit test | `tests/unit/dns_class_test.c`, `tests/dns_class.mk` | Parsed addresses, record acceptance, and cache behavior |
| Sanitizer and fuzz test | `tests/unit/x509_fuzz.c` | Memory errors during truncation and mutation |
| Known-answer test | `tests/unit/crypto_vec_test.c` | Output bytes compared with recorded vectors |
| Differential test | `tests/unit/crypto_diff_test.c` | Production results compared with generated reference results |
| Corpus test | `tests/wpt.mk`, `tests/reftest.mk` | Named subtest failures or rendered reference comparisons |
| Guest program | `tests/guest/exec/polltest.c`, `tests/poll.mk` | Readiness, wakeups, and libc interfaces |
| Desktop interaction | `tests/qmp/qmp_abort.py` | Application assertions, requested routes, and painted results |
| Storage failure injection | `tests/unit/fs_crash_test.c` | Contents and filesystem consistency after simulated power loss |
| Build graph check | `tests/tools/make/build_dir.py` | Artifact creation and the expected failure after removing a required rule |

Assert results that a caller observes. The DNS class suite checks returned addresses and subsequent cache use. The guest polling suite checks readiness and wakeups. The cancellation driver requires its result marker to reach the scanout.

## Add a host unit test

Place the test beside related suites in `tests/unit/` and add a target to the owning Make fragment. Link the production implementation or reuse a fixture that includes it.

For example, `tests/unit/dns_class_test.c` includes `tests/unit/dns_test.c`. That fixture includes `c/net/dns/dns.c` and supplies modeled transport, time, and randomness. The class suite constructs response records, checks A and AAAA parsing, and exercises asynchronous queries and caching.

Make failed assertions identify the input and expected result, and return a failing process status when assertions fail. The DNS class suite prints passed and failed counts and returns status 1 when failures occur.

Keep compilation inputs and rebuild dependencies together. `tests/abort.mk` derives its production source list from the existing Web API test group and shares prerequisites among the positive, control, and sanitizer executables. Those prerequisites include fixture headers, browser interface dependencies, and the fragment itself, so edits to those inputs invalidate each variant.

Use current production dependencies when extending a fixture. The `test-fb-clip` recipe in `Makefile` links both `c/kernel/gui/fb/fb.c` and the cursor overlay implementation in `c/kernel/gui/fb/fb_overlay.c`.

Header search order also affects the fixture. `tests/poll.mk` uses `-iquote` for kernel directories because kernel and host headers share names. This lets quoted kernel includes resolve through those directories while angle-bracket includes use the host toolchain. For optional symbols, follow the declaration, stub, and guard conventions in `include/weaksym.h`.

## Add sanitizer, fuzz, and differential coverage

Use sanitizer builds for code that handles lengths, buffers, ownership, or linked structures. In `tests/abort.mk`, `test-abort-san` builds with AddressSanitizer, UndefinedBehaviorSanitizer, debugging information, and frame pointers. Its recipe captures output, extracts diagnostics, and preserves the executable’s exit status.

For fuzz coverage, start with a valid fixture and make failures reproducible. `tests/unit/x509_fuzz.c` parses the certificate supplied on its command line, tries every truncation, then performs seeded mutations of full and truncated inputs. The `test-x509-fuzz` recipe in `Makefile` supplies `tests/unit/cert.der` and builds the parser with sanitizers. Preserve a small reproducer when a mutation exposes a defect, then add an assertion for the expected parser outcome.

For known-answer coverage, extend the vector generator and its consumer together. `tests/unit/crypto_vec_test.c` includes `tests/unit/crypto_vectors.h`. Set the generator’s output destination when regenerating that header, review the resulting changes, and run its consumer:

```sh
VECOUT=tests/unit/crypto_vectors.h python3 tests/unit/crypto_vec_gen.py
make test-crypto
```

For differential coverage, keep the reference calculation auditable. `tests/unit/crypto_diff_gen.py` checks its reference implementations against known answers before generating cases from a fixed seed. `tests/unit/crypto_diff_test.c` consumes those cases and compares output bytes. `make test-crypto-diff` runs the generator and consumer. Extend both when adding an operation, and add a control that demonstrates detection of a plausible error.

## Add a guest test

Use a guest test for behavior involving system calls, scheduling, interrupts, devices, or packaged applications.

For a native diagnostic program, follow `tests/poll.mk` and `tests/guest/exec/polltest.c`:

1. Compile the guest source with the application flags.
2. Link startup code, libc objects, and required linker scripts.
3. Package the executable through `tools/mkaex.py`.
4. Add its name to `CLI` and its package to the prerequisites of `$(DISK)`.
5. Add a harness under `tests/boot/` and a target with the required ISO and disk dependencies.

The polling example links `c/apps/libc/logit_tls.ld` to provide the TLS layout used by its threads. Its harness, `tests/boot/run-poll-test.sh`, launches the program through the serial shell and checks for `POLL_TEST_OK`. The program emits that marker after its assertions pass. The harness also checks for `POLL_TEST_FAILED`, bounds its wait, and fails if completion is missing.

Guest fixtures need both packing entries and rebuild dependencies. `tests/textutils.mk` appends mappings to `FS_FILES` and adds prerequisites to `$(DISK)`. List each fixture input in the image dependencies. Include the fragment itself when edits to its packing rules change image contents.

A kernel self-test needs an invocation and a harness assertion. `c/kernel/init/kmain.c` calls `cpu_simd_selftest()`, and `test-simd-os` in `tests/ci.mk` checks serial output for `CPU_SIMD_SELFTEST_OK`.

Boot acceptance should also check the root mount result. `c/kernel/init/kmain.c` emits `LOGIT_BOOT_OK` when the root mount succeeded. A failed mount produces a hardware report and later enters `hwscreen_report()` in `c/kernel/diag/hwscreen.c` to display the retained boot log. For a missing-root control, require the mount-failure diagnostic and reject any success marker throughout a bounded observation window.

Timer recovery tests can use `TIMENOCH2=1`, which `Makefile` maps to `TIME_TEST_NO_CH2`. In `c/kernel/core/ktime.c`, this makes PIT channel 2 calibration fail. `time_platform_init()` then attempts TSC calibration against HPET when the required counters are available. Extend the clock and timer assertions in `tests/boot/run-time-test.sh` with checks for the HPET calibration message and final clock source. `TIME_NEGCTL_NO_HPET_CALIB` suppresses that recovery attempt for a control.

The early PIT tick probe in `c/kernel/init/kmain.c` follows platform clock initialisation, where `c/drivers/timer/hpet.c` disables HPET legacy routing. The probe bounds its loop by both elapsed guest time and iteration count, so it can finish when the clock stalls.

The default `all` target builds the ISO. Rebuild application packages and the disk image with `make build/disk.img`, as described in [Building LogitOS](../getting-started/building.md).

## Add interaction, persistence, and build checks

For desktop behavior, place the driver under `tests/qmp/` and page fixtures under `tests/fixtures/`. `tests/qmp/qmp_abort.py` starts a local fixture server, waits for guest readiness, launches the browser, drives navigation, and checks application results.

Connect interaction to visible output. The cancellation driver checks assertions and requested routes, then captures successive scanouts until the result marker appears. It records structured results, serial output, requests, and a packet capture. Give each run its own output directory so sockets and evidence belong to that run.

Hardware runs can collect the milestones and device inventory emitted by `c/kernel/diag/hwreport.c`. `HWHUD=1` in `Makefile` enables the desktop diagnostic panel implemented in `c/kernel/diag/hwhud.c`. Pair these diagnostics with observations of the behavior under test, such as delivered input and captured frames.

For persistence, follow `tests/boot/run-durability-test.sh`. It copies the disk image and boots repeatedly against that copy, checking file contents through a guest verification program. Writes persist between those boots.

For storage failure injection, extend `tests/unit/fs_crash_test.c` and its device fixture in `tests/unit/fsstub/fs_sim.h`. The fixture models volatile writes, flushes, partial survival of pending writes, and torn blocks. The test sweeps write budgets around filesystem operations, remounts, checks contents, allocates again, and runs filesystem consistency checks. Preserve these device semantics when extending the cases: they make write ordering observable.

For build graph checks, follow `tests/build_dir.mk` and `tests/tools/make/build_dir.py`. The positive case asks Make to create a fresh build directory and checks the resulting artifact. The control removes the directory rule from a temporary Makefile and requires the corresponding missing-target diagnostic. Extend such checks with assertions about both the requested product and the expected failure.

Use guest time for guest duration assertions. `tests/guest/exec/polltest.c` measures intervals through `SYS_MONOTONIC_MS`. `AGENTS.md` requires guest measurements because concurrent host workloads affect elapsed host time.

## Prove the negative control

A negative control introduces a plausible defect and runs the assertion that should detect it. Share production source lists, fixtures, packaging settings, and comparison rules with the positive case.

`tests/abort.mk` builds a control with `ABORT_NOTIFY_EARLY`, a switch in `c/apps/gui/browser/js/net/abort.c`. It moves source notification before dependent states are updated. The control recipe requires status 1, exactly one failed assertion, and this diagnostic:

```text
FAIL: all dependent states settle before source listeners run
```

The control target succeeds when it observes that expected failure. Compilation errors, crashes, and unrelated assertion failures fail this control.

Other defects call for different mutations:

| Mechanism | Example |
|---|---|
| Restore the earlier DNS class acceptance rule | `DNS_NO_CLASS_MATCH` in `tests/dns_class.mk` and `c/net/dns/dns.c` |
| Mutate a source copy in the build directory | Bounds controls in `tests/h264.mk` |
| Remove a QEMU device | `tests/virtio_rng.mk` |
| Shift rendered rectangles by one pixel | `tests/aui-host-harness.mk` |
| Remove a rule from a temporary Makefile | `tests/tools/make/build_dir.py` |
| Suppress TSC calibration against HPET | `TIME_NEGCTL_NO_HPET_CALIB` in `c/kernel/core/ktime.c` |

Watch the defective case fail before accepting the positive result. Record the assertion, exit status, and environment. Comments should explain the defect represented by the mutation and why the observation detects it.

## Wire the test into Make

Keep related recipes in a fragment under `tests/` and include it from the root `Makefile` or an included owner fragment. Include paths resolve from the repository root. For example, `tests/exec.mk` includes `tests/poll.mk`.

Declare test targets phony, make controls prerequisites of their positive targets, and attach positive targets to the appropriate suite. This excerpt from `tests/abort.mk` shows the arrangement:

```make
.PHONY: test-abort
test-abort: test-abort-negctl $(ABORT_DIR)/current
ci-host: test-abort
```

Use `ci-host` for host suite membership and `ci-boot` for guest suite membership. `tools/audit_tests.py` uses these declarations when checking reachability.

Automatic execution uses recipe classification. `tools/ci.sh` asks `tools/audit_tests.py` for host and boot targets; the classifier examines each target’s recipe and direct prerequisites. Review that classification when adding a wrapper target, particularly when another target owns its guest dependencies.

The discovery filter excludes test names ending in `-negctl` or `-control`. A prerequisite connects the control to execution of the positive test. Listing a control beside the positive target on a suite declaration can satisfy reachability accounting while leaving the control excluded from the automatic execution list.

Check the wiring with:

```sh
make test-mk-wired
make test-audit
make test-negctl-drift
```

| Target | Check |
|---|---|
| `test-mk-wired` | Existing Make fragments, including nested fragments, have an include path from the root, apart from declared exceptions |
| `test-audit` | Harness references, failure signaling, suite reachability, stranded controls, and duplicate includes |
| `test-negctl-drift` | Recognized control pairs retain relevant source, object, library, and packaging tokens; controls also have distinguishing recipe tokens |

`tools/mk_wired.py` follows nested literal includes. Its declared exception covers `tests/schedneg.mk`, a wrapper that includes the root `Makefile`.

Pass a task-specific, relative `BUILD` value as a command-line Make assignment when sharing the workspace. The root `Makefile` sets `BUILD := build`, and a command-line assignment takes precedence over that definition.

`make ci` uses a clone of committed `HEAD`; `make ci-here` uses the working tree. `.gitignore` ignores `tests/`, so force-add new test files using path-limited additions. `AGENTS.md` requires commits to contain the assigned files.

## Maintain golden files

A golden file records expected output. Review its changes alongside the behavior that produced those bytes.

For rendered images, follow `tests/aui-host-harness.mk`. It compares rendered scenes with compressed PPM images under `tests/golden/aui/`. The fixture draws deterministic box glyphs, and each rendering case runs in a child process to contain cached state.

The comparison uses `tools/ppm_diff.py` with zero channel tolerance and zero permitted bad pixels. The tool reports maximum channel delta, bad-pixel count, and a bounding box. Status 1 identifies a pixel mismatch; status 2 identifies an input, size, or format error. The shift control requires every comparison to produce status 1 and the pixel-mismatch diagnostic.

For an intended visual change, inspect the failing comparison’s renders and difference images against the previous goldens. Then regenerate expectations and run the comparison again:

```sh
make test-aui-host GOLDEN_UPDATE=1
make test-aui-host
```

`tests/golden/aui/README.md` asks for maximum delta and changed-pixel counts per mode in the commit message, and for regeneration after rebasing. Include source and golden changes in the same review.

The gzip helper in `tools/ppm_diff.py` fixes the timestamp and stored filename, allowing unchanged rendered output to reproduce the same compressed bytes.

Object output has another pattern. `tests/aui-split.mk` compares stripped object hashes with `tests/golden/aui_split.sha256`. It also provides `aui-split-symdiff` for comparing symbols across revisions when function movement changes object layout. Choose the comparison that matches the property being preserved.

## Maintain corpus baselines

Corpus baselines record accepted failures. `tests/wpt.mk` uses `tests/unit/wpt_expected_fail.txt`; `test-wpt` runs with strict checking and fails on newly observed failures. `tests/reftest.mk` uses `tests/unit/reftest_expected_fail.txt` for rendering regressions.

When adding coverage, identify the corpus file or subset and preserve stable result identities. WPT baseline entries identify the test path and subtest name. Reftest baseline entries identify the test path.

Check the harness too. WPT provides `test-wpt-harness`, and the reftest gate includes comparator and rendering controls. Reftests render pages and their linked references; the AUI golden harness compares renders with committed images.

Refresh expectations after reviewing the changed results:

```sh
make wpt-baseline
make reftest-baseline
```

The WPT runner rejects writes to its committed baseline path from runs restricted through `--subset` or `--only`. Use another destination through `-b` for a subset baseline. Record the corpus location, selected coverage, executed counts, and reviewed failures when reporting a baseline change.

## Record evidence and documentation changes

Report the exact commands, environment, positive result, and observed control failure. Preserve the logs and artifacts needed to interpret the result. `CONTRIBUTING.md` requires skipped, failing, and unrun checks to be identified in the contribution description.

For documentation review after committed source changes, run:

```sh
python3 tools/docs/stale.py
```

The script uses `tools/docs/docs_index.py` to compare cited sources between each page’s recorded verification commit and `HEAD`. Read the affected pages against the source and update them. After verifying this page against the committed revision, record that revision with:

```sh
python3 tools/docs/stale.py --mark docs/contributing/writing-tests.md
```

## Limitations

- Host fixtures establish behavior for the code and modeled dependencies they execute. Guest integration and physical hardware require evidence from those environments. Input and desktop milestones in `c/kernel/init/kmain.c` describe initialisation progress; interaction and scanout require further observations.
- The X.509 mutation loop relies on sanitizer diagnostics and completion; it lacks semantic assertions for individual mutated certificates.
- The WPT runner reports an absent corpus and returns success. Record corpus availability and executed counts with results.
- AUI golden update mode skips the shift control and rewrites expectations. Acceptance requires a comparison run with update mode disabled.
- `tools/ci.sh` requires Linux and treats the general test audit as advisory. Direct `make test-audit` fails on actionable findings; existing unwired targets and stranded controls have recorded baseline debt.
- Wiring, CI classification, and drift checks use static parsing. Computed includes, generated rules, naming conventions, and recognized recipe tokens constrain their coverage.
- Documentation staleness reporting compares committed revisions. Uncommitted source edits require direct review.

## See also

- [Testing](../architecture/testing.md)
- [Make targets](../reference/make-targets.md)
- [Contribution workflow](workflow.md)
- [Making your first change](../getting-started/first-change.md)

## Source map

| File | What it contains |
|---|---|
| `AGENTS.md`, `CONTRIBUTING.md`, `TRANSPARENCY.md` | Evidence requirements, contribution reporting, and claim rules |
| `Makefile`, `.gitignore` | Build variables, image dependencies, test entry points, and ignored files |
| `tests/ci.mk`, `tools/ci.sh` | Audit entry points, guest SIMD assertion, and CI execution |
| `tools/mk_wired.py`, `tools/audit_tests.py`, `tools/negctl_drift.py` | Fragment reachability, harness auditing, CI discovery, and control drift checks |
| `tests/dns_class.mk`, `tests/unit/dns_class_test.c`, `tests/unit/dns_test.c`, `c/net/dns/dns.c` | Resolver fixture, class acceptance tests, and production implementation |
| `tests/abort.mk`, `tests/unit/abort_test.c`, `c/apps/gui/browser/js/net/abort.c` | Shared build inputs, sanitizer execution, and notification ordering control |
| `include/weaksym.h` | Portable optional symbol declarations, stubs, and guards |
| `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.c` | Framebuffer implementation and cursor overlay dependency |
| `tests/unit/x509_fuzz.c`, `tests/unit/cert.der` | Certificate truncation and mutation fixture |
| `tests/unit/crypto_vec_gen.py`, `tests/unit/crypto_vectors.h`, `tests/unit/crypto_vec_test.c` | Generated known-answer data and its consumer |
| `tests/unit/crypto_diff_gen.py`, `tests/unit/crypto_diff_test.c` | Reference checks, seeded case generation, and byte comparisons |
| `tests/poll.mk`, `tests/guest/exec/polltest.c`, `tests/boot/run-poll-test.sh` | Host and guest polling tests, packaging, and completion checks |
| `tools/mkaex.py`, `c/apps/libc/logit_tls.ld`, `tests/textutils.mk` | Executable packaging, TLS layout, and guest fixture packing |
| `c/kernel/init/kmain.c`, `c/kernel/diag/hwscreen.c` | Kernel self-test invocation, mount acceptance, and boot failure display |
| `c/kernel/core/ktime.c`, `c/drivers/timer/hpet.c`, `tests/boot/run-time-test.sh` | Calibration fault hooks, HPET recovery, routing setup, and guest time assertions |
| `c/kernel/diag/hwreport.c`, `c/kernel/diag/hwhud.c` | Hardware milestones, inventory, and desktop diagnostics |
| `tests/qmp/qmp_abort.py` | Guest interaction, request checks, scanout assertions, and evidence capture |
| `tests/unit/fs_crash_test.c`, `tests/unit/fsstub/fs_sim.h`, `tests/boot/run-durability-test.sh` | Storage failure simulation and persistence across boots |
| `tests/h264.mk`, `tests/virtio_rng.mk` | Source mutation and device removal controls |
| `tests/build_dir.mk`, `tests/tools/make/build_dir.py` | Build graph assertions and a temporary Makefile control |
| `tests/exec.mk`, `tests/schedneg.mk` | Nested fragment inclusion and the declared wrapper exception |
| `tests/aui-host-harness.mk`, `tests/unit/aui_host/aui_host.c`, `tests/unit/aui_host/aui_host_test.c` | Golden comparison recipes, rendering fixture, and case isolation |
| `tests/golden/aui/README.md`, `tools/ppm_diff.py` | Golden review policy, pixel comparison, and deterministic compression |
| `tests/aui-split.mk`, `tests/golden/aui_split.sha256` | Object hash expectations and symbol comparison |
| `tests/wpt.mk`, `tests/unit/wpt_test.c`, `tests/reftest.mk`, `tests/unit/reftest.c` | Corpus execution, expected failures, baseline updates, and harness controls |
| `tools/docs/stale.py`, `tools/docs/docs_index.py` | Documentation staleness reporting and verification records |