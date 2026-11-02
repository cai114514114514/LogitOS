# Testing

LogitOS tests cover native unit tests, host integration tests, QEMU boots, guest programs, and desktop interactions through the QEMU Machine Protocol (QMP). Make targets connect harnesses to build inputs and negative controls. Static audits inspect fragment inclusion, harness reachability, and failure reporting. Choose tests based on the behavior that changed and the environment needed to observe it.

## Test layers

Run commands from the repository root. See [Building LogitOS](../getting-started/building.md) for build prerequisites.

| Kind | Command | What it exercises |
|---|---|---|
| Host unit test | `make test-part` | Partition parsing against synthetic MBR and GPT sectors. |
| Host integration test | `make test-poll` | Kernel polling and wait queues with a scheduler model using pthreads. |
| Host memory tests | `make test-mm` | Memory management over simulated physical memory, with sanitizers and controls. |
| Host crash injection | `make test-fs-crash` | LogitFS operations interrupted at simulated device writes, followed by recovery assertions. |
| Host drawing test | `make test-fb-clip` | Clipped framebuffer drawing and presentation pixel accounting. |
| Boot smoke test | `make test` | Host crypto and network prerequisites, then a headless boot with serial assertions. |
| Guest program test | `make test-poll-os` | Polling through libc, descriptor backends, guest threads, and timer interrupts. |
| Guest scheduling test | `make test-sched` | Weighted CPU shares, completed work, and priority interfaces. |
| Desktop interaction test | `make test-window` | Window operations through QMP input, guest geometry reports, and screenshots. |
| System integration test | `make test-fullsystem` | Filesystem, shell, applications, networking, browser rendering, and keyboard input. |

These commands are defined in `Makefile`, `tests/poll.mk`, `tests/sched.mk`, `tests/window.mk`, and `tests/fullsystem.mk`.

The categories overlap. Unit testing describes assertion scope, and host and guest describe execution location. A QMP driver runs on the host and controls and observes the guest.

## Host tests and their models

Host harnesses compile production sources into native executables and provide fixtures or replacement services around them. These models give tests control over malformed input, event timing, and persistence.

The partition parser reads sectors through a callback declared in `c/drivers/block/part.h`. `tests/unit/part_test.c` supplies synthetic sectors to the implementation in `c/drivers/block/part.c`. Its assertions cover valid tables, corrupt checksums, extended partition loops, invalid ranges, and read failures.

The polling gate links `c/kernel/exec/fd/kpoll.c` and `c/kernel/sync/wait.c` with `tests/unit/pollhost/hostsched.c`. The scheduler model parks and wakes threads through pthread condition variables. A model source in `tests/unit/poll_test.c` injects an event after calculating readiness, making the registration race reproducible.

Memory tests compile with `MM_HOSTTEST`. The address translation and processor operation adapters are in `c/kernel/mm/mmhost.h`. `tests/unit/mm_run.sh` enables AddressSanitizer and UndefinedBehaviorSanitizer and runs suites for frame allocation, address spaces, protection, reverse mappings, reclaim, the page cache, file mappings across fork, and shared memory.

The storage model in `tests/unit/fsstub/fs_sim.h` accepts writes to a volatile cache. Flushing persists pending data; a simulated power cut persists a subset and can tear a block. Batch writes pass each span through the same simulated write operation, preserving crash injection points. `tests/unit/fs_crash_test.c` varies the write budget, remounts the filesystem, checks consistency and contents, and allocates again to expose blocks assigned to multiple files.

`tests/unit/fb_clip_test.c` compares clipped drawing with the corresponding pixels from unclipped drawing and checks the surrounding pixels. It also checks presentation pixel counts for rectangles crossing screen edges. The recipe in `Makefile` links the framebuffer implementation and its overlay support from `c/kernel/gui/fb/fb.c` and `c/kernel/gui/fb/fb_overlay.c`.

Browser frame tests provide further host integration gates:

| Command | Coverage |
|---|---|
| `make test-frame-sandbox-parse` | Sandbox token parsing and Content Security Policy sandbox directive parsing. |
| `make test-frame-sandbox-host` | Browser frame behavior over mocked HTTP transport, including attribute and response policy restrictions. |
| `make test-frame-window` | Frame Window creation through the page runtime, navigation, and timer cleanup. |

The recipes and controls are in `tests/frame_sandbox.mk` and `tests/frame_window.mk`. Their fixtures are `tests/unit/frame_sandbox_test.c`, `tests/unit/passive_frame_test.c`, and `tests/unit/frame_window_test.c`.

## Boot and guest checks

In `Makefile`, `test` depends on `test-crypto`, `test-net`, the ISO, and the disk image. Its recipe invokes `tests/boot/run-test.sh`, which boots QEMU headlessly and requires these substrings in serial output:

- `LOGIT_BOOT_OK`;
- text from the trust store containing the expected counts for roots and skipped roots, defaulting to 130 and zero;
- a substring of the root name, defaulting to `isrg_x1`.

The trust assertions make changes to the compiled root set visible during a boot check. The script accepts overrides through `EXPECT_ROOTS`, `EXPECT_SKIPPED`, and `EXPECT_ROOT_NAME`.

Loader changes have additional entry points:

| Command | Coverage |
|---|---|
| `make test-bios-native` | Native entry controls, diagnostic comparison with a GRUB test oracle, and a kernel boot check. |
| `make test-uefi-native` | Host checks of loader policy, native entry controls, firmware comparisons, and a kernel boot under OVMF. |

These targets live in `tests/bootself.mk` and `tests/uefi.mk`. The UEFI fragment discovers firmware through `UEFI_NATIVE_OVMF_CODE` and `UEFI_NATIVE_OVMF_VARS`; callers can override both variables.

Guest programs extend testing through libc, system calls, and device backends. `tests/boot/run-poll-test.sh` sends the polling program command to the serial shell and waits for `POLL_TEST_OK` or `POLL_TEST_FAILED`. The program in `tests/guest/exec/polltest.c` checks pipes, descriptor closure, console readiness, selection over descriptors, event descriptors, and timer descriptors.

Run both polling layers when changing this interface:

```sh
make test-poll
make test-poll-os
```

The host fixture controls event placement around registration. The guest program exercises the integrated descriptor backends and timer behavior.

`make test-sched` boots with two CPUs and runs `tests/guest/sched/schedtest.c`. The program compares CPU shares and completed work for equal and unequal weights. `tests/boot/run-sched-test.sh` also checks priority settings across program replacement using the command-line consumers.

Build dependencies determine which changes reach the guest. The default `all` target builds the ISO. Guest programs are packaged into the disk image, and targets such as `test-poll-os` and `test-fullsystem` depend on both artifacts. The default disk build command is:

```sh
make build/disk.img
```

## QMP and desktop assertions

QMP tests combine input injection, serial diagnostics, and screenshots. Each observation captures a different part of an interaction.

`tests/qmp/qmp_window.py` checks geometry against guest window reports. Its resize test captures a window, grows it, shrinks it back to the starting frame, and compares pixels below the menu bar. The geometry check establishes the final frame; the pixel comparison detects stale content around the resized window.

Shared helpers in `tests/qmp/qmp_ui.py` provide keyboard and pointer input, screenshots, display scale conversion, and parsers for guest geometry and pointer reports.

`make test-fullsystem` invokes `tests/boot/run-fullsystem-test.sh`, which delegates assertions to `tests/qmp/qmp_fullsystem.py`. In `tests/fullsystem.mk`, expected application inputs come from `ROOT_AEX_PACK`, the list used by the disk recipe. The driver reads package display names, compares the guest’s listing of root applications, and checks application launches.

The same driver serves an HTTP fixture on the host. It compares the guest’s reported response length and FNV-1a checksum with the served bytes. Browser checks inspect rendered colors, text pixels, a PNG resource, and link navigation.

`make test-frame-sandbox-guest` serves pages from two host origins and examines guest serial output and the fixture server’s request log. Its control uses a browser build that ignores sandbox flags. `tests/qmp/frame_sandbox_guest.py` requires restriction assertions to fail and positive assertions to keep passing.

The gate for the state lock in the window manager uses guest counters to test lock placement:

```sh
make build/disk.img
make test-wm-state-lock
```

`tests/wm_state_lock.mk` first runs a control kernel that holds the state lock during compositor pixel work. It then runs the production kernel. `tests/qmp/qmp_wm_state_lock.py` requires composites to occur, checks that every measured control composite held the lock, and requires zero such composites in the production run.

Telemetry in `c/kernel/gui/wm.c` captures performance and lock data under the state lock and prints the captured data after releasing it. Lock attribution rows are cumulative and emitted when a tag’s call count changes. The driver retains the latest complete row for each tag.

## Negative controls

A negative control introduces a known defect and checks that the harness detects its effect. The control wrapper succeeds when the expected failure condition occurs.

| Control | Injected defect | Required observation |
|---|---|---|
| Host polling | `POLL_NO_PREREGISTER` moves readiness checking before wait queue registration. | The executable exits unsuccessfully; its output identifies the lost wakeup assertions. |
| Guest polling | `POLLTEST_NEGCTL_NOPOLL` substitutes polling that reports zero readiness. | The program prints a report, emits no success marker, and records at least five failed checks and two passing checks. |
| Memory management | Builds remove individual protection, reclaim, cache, or shared memory checks. | Each control compiles, exits unsuccessfully, and emits a `FAIL:` assertion. |
| Window resize | `WM_RESIZE_DAMAGE_LIE` suppresses damage reporting for the previous frame. | At least one resize pixel check fails. |
| Weighted scheduling | `SCHED_IGNORE_WEIGHT` removes weight dependence from the scheduling charge. | Unequal-weight cases fail with CPU-share ratios near one, the equal-weight case passes, and four checks pass out of six. |

The polling controls and prerequisite relationships are in `tests/poll.mk`. Guest verdict checks are in `tests/boot/run-poll-negctl.sh`; memory verdict checks are in `tests/unit/mm_run.sh`.

The window control rebuilds a temporary copy of the tree and changes the switch in `c/kernel/gui/wm.c`. Its verdict is implemented in `tests/qmp/qmp_window.py`, and `tests/window.mk` makes it a prerequisite of `test-window`.

The scheduler control build is defined in `tests/schedneg.mk`, with verdict checks in `tests/boot/run-sched-negctl.sh`. `tests/sched.mk` makes it a prerequisite of `test-sched`.

Browser controls also check failure patterns. The parser control in `tests/frame_sandbox.mk` requires exactly three assertions about ASCII case to fail. Each frame Window control in `tests/frame_window.mk` requires failures carrying its assigned tag, zero passing assertions with that tag, and zero failures outside it.

When adding a gate, make its control a prerequisite of the positive target. Observe the intended assertion failing and record the failure output. Document the reason for the injected defect beside its recipe or implementation.

## Gate organisation and audits

Subsystems keep related rules in Make fragments under `tests/`. The root `Makefile` includes fragments directly and through other fragments. For example, `tests/exec.mk` includes `tests/poll.mk`, and `tests/thread.mk` includes `tests/sched.mk`.

Four commands inspect this structure:

| Command | Check |
|---|---|
| `make test-mk-wired` | `tools/mk_wired.py` follows literal fragment includes and checks transitive reachability from `Makefile`. |
| `make test-audit` | `tools/audit_tests.py` checks harness reachability, failure propagation, aggregate membership, stranded controls, and duplicate includes. |
| `make check-test-liveness` | `tools/check-test-liveness.py` searches harnesses for failure paths and suspicious patterns. |
| `make test-negctl-drift` | `tools/negctl_drift.py` compares selected build and packaging tokens in positive and control recipes. |

The test audit records existing aggregate membership debt and stranded controls by target name, then reports additions outside those sets. `make test-audit-list` prints its inventory and returns success.

The liveness checker requires a recognized failure path or a reason in the allowlist. Unreferenced scripts, literal pointer coordinates, and failure paths associated solely with emulator death produce warnings.

The Make parsers in `tools/mk_wired.py`, `tools/audit_tests.py`, and `tools/negctl_drift.py` join backslash continuations before interpreting rules. Preserve that step when extending their analysis.

## Suites, sweeps, and CI

Filesystem aggregates offer several scopes:

| Command | Selection |
|---|---|
| `make test-fs-host` | Native cache, journal, crash, consistency, format, and related checks. |
| `make test-fs-boot` | Guest checks for mount, durability, crash recovery, replay, large files, barriers, and metadata. |
| `make test-fs` | Both aggregates. |

`Makefile` defines these aggregates, and included fragments add further prerequisites.

For broader testing, `make test-sweep-host` runs the host selection and `make test-sweep` also runs the device selection. `tests/sweep.mk` and `tests/boot/sweep-targets.sh` arrange host targets in parallel, device targets in sequence, and confirmation runs of failures one at a time. `tests/boot/sweep-confirm.sh` replaces initial failure results with the confirmation verdicts.

Sweep results distinguish executed tests, aggregates, targets requiring arguments, and missing targets. The Make wrapper accepts `PASS`, `AGGREGATE`, `NEEDSARGS`, and `NOTARGET` rows when deciding its final exit status.

CI entry points in `tests/ci.mk` invoke `tools/ci.sh`:

| Command | Requested workflow |
|---|---|
| `make ci` | Clone the current commit, build, then run the host selection. |
| `make ci-boot` | Clone, build, then run the boot selection. |
| `make ci-all` | Clone, build, then run both selections. |
| `make ci-here` | Build and run the host selection in the working tree. |

The clone uses committed inputs. The driver writes step logs and records `PASS`, `FAIL`, or `TIMEOUT`.

CI obtains target lists through `tools/audit_tests.py --suites=host` and `--suites=boot`. Classification uses recipe text and image prerequisites. Aggregate reachability uses another calculation: an `UNWIRED` finding describes aggregate membership, and CI selection follows the classifier.

CI excludes names ending in `-negctl` or `-control` from direct selection. Prerequisites on positive targets bring their controls into execution.

## Limitations

- `tools/ci.sh` requires Linux or WSL. It treats test audit findings as advisory and control drift as a failing gate.
- CI invokes default `make`, then requires both ISO and disk artifacts. Since `all` builds the ISO, a fresh clone can stop at the missing disk check before running suites.
- `tests/qmp/qmp_window.py` uses `build/disk.img` under its repository root even when passed an ISO from another build directory.
- The host polling wrapper accepts any unsuccessful executable exit. The guest polling wrapper checks aggregate failure and pass counts. Inspect assertion messages to establish the cause.
- Static audits use syntax patterns and recorded exceptions. The fragment audit inventories existing files, so missing optional includes can escape its reachability check.
- Some firmware checks return success after printing `SKIP` when QEMU or OVMF is unavailable. Read skip messages alongside exit status.
- As stated in `TRANSPARENCY.md`, a passing test demonstrates the tested behavior in the named environment. Evidence for broader hardware support, standards compliance, and security must cover those scopes.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Image dependencies, host gates, filesystem aggregates, and fragment includes. |
| `c/drivers/block/part.h`, `c/drivers/block/part.c`, `tests/unit/part_test.c` | Partition parser interface, implementation, and synthetic fixtures. |
| `tests/poll.mk`, `tests/exec.mk` | Polling builds, controls, guest packaging, and fragment wiring. |
| `c/kernel/exec/fd/kpoll.c`, `c/kernel/sync/wait.c` | Polling and wait queue implementations. |
| `tests/unit/pollhost/hostsched.c`, `tests/unit/poll_test.c` | Host scheduler model and controlled wakeup tests. |
| `c/kernel/mm/mmhost.h`, `tests/unit/mm_run.sh` | Memory test adapters, sanitizers, suites, and control verdicts. |
| `tests/unit/fsstub/fs_sim.h`, `tests/unit/fs_crash_test.c` | Storage persistence model and crash recovery assertions. |
| `tests/unit/fb_clip_test.c`, `c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.c` | Clipping assertions, framebuffer implementation, and overlay support. |
| `tests/frame_sandbox.mk`, `tests/unit/frame_sandbox_test.c`, `tests/unit/passive_frame_test.c` | Sandbox gates, parser assertions, and browser fixtures. |
| `tests/frame_window.mk`, `tests/unit/frame_window_test.c` | Frame Window integration assertions and controls. |
| `tests/boot/run-test.sh` | Boot marker and assertions for the trust store. |
| `tests/bootself.mk`, `tests/uefi.mk` | BIOS and UEFI boot gates. |
| `tests/guest/exec/polltest.c` | Guest polling tests and completion markers. |
| `tests/boot/run-poll-test.sh`, `tests/boot/run-poll-negctl.sh` | Guest polling execution and control verdicts. |
| `tests/thread.mk`, `tests/sched.mk`, `tests/schedneg.mk` | Scheduler gate wiring, guest packaging, and control build. |
| `tests/guest/sched/schedtest.c`, `c/kernel/sched/sched.c` | Scheduler workload and weight control implementation. |
| `tests/boot/run-sched-test.sh`, `tests/boot/run-sched-negctl.sh` | Scheduler execution, consumer checks, and control verdicts. |
| `tests/window.mk`, `tests/qmp/qmp_window.py`, `c/kernel/gui/wm.c` | Window gate, resize control, pixel assertions, and WM telemetry. |
| `tests/wm_state_lock.mk`, `tests/qmp/qmp_wm_state_lock.py` | Builds for the state lock, counter assertions, and attribution parsing. |
| `tests/qmp/qmp_ui.py` | Shared QMP interaction and geometry helpers. |
| `tests/qmp/frame_sandbox_guest.py` | Sandbox assertions using guest output and HTTP request logs. |
| `tests/fullsystem.mk`, `tests/boot/run-fullsystem-test.sh`, `tests/qmp/qmp_fullsystem.py` | Integration dependencies, launcher, and system assertions. |
| `tools/mk_wired.py`, `tools/audit_tests.py` | Fragment reachability, test inventory, classification, and structural audits. |
| `tools/check-test-liveness.py`, `tools/negctl_drift.py` | Failure path inspection and control recipe comparisons. |
| `tests/sweep.mk`, `tests/boot/sweep-targets.sh`, `tests/boot/sweep-confirm.sh` | Broad execution, result handling, and sequential confirmation. |
| `tests/ci.mk`, `tools/ci.sh` | CI entry points, cloning, builds, suite selection, and verdicts. |
| `TRANSPARENCY.md` | Evidence requirements and scope of claims. |

## See also

- [Build system](build-system.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
- [Make targets](../reference/make-targets.md)
- [Writing tests](../contributing/writing-tests.md)
- [Contribution workflow](../contributing/workflow.md)
