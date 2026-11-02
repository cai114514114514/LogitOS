# Make targets

The root `Makefile` defines kernel builds, filesystem images, QEMU launches and core checks. Included fragments under `tools/build/` provide compiler caching, configuration tracking and scheduling; fragments under `tests/` add subsystem checks and supporting artifacts. This reference groups the main targets by purpose and identifies the files that define them.

## Output directory and settings

Run commands from the repository root. Plain `make` selects `all`, which builds the ISO. Applications and their filesystem image are built through the disk target, so request `make build/disk.img` after changing an application.

The default output directory is `build`, assigned by `BUILD := build` in `Makefile`. Choose another directory with a command-line assignment such as `make BUILD=build-reference all`. That assignment overrides the Makefile value; an environment assignment to `BUILD` is overridden by it.

| Setting | Effect |
|---|---|
| `BUILD` | Selects the output directory for rules expressed through this variable. |
| `CCACHE` | Selects the compiler cache command. The default searches for `ccache`; an empty assignment disables caching. |
| `CCACHE_DIR` | Selects the compiler cache directory. `tools/build/cache.mk` defaults to `.cache/ccache` under the repository root. |
| `JOBS` | Sets the automatic product build job count. The default is the smaller of 12 and the host CPU count. |
| `DISP` | Supplies QEMU’s display backend, such as `sdl` or `none`. |
| `HWHUD` | Set to `1` to enable the desktop hardware panel through `LOGIT_HWHUD`. The ESP loader build also receives this define and can import an optional developer SSH public key. |
| `TIMENOCH2` | Set to `1` to force PIT channel 2 calibration to fail through `TIME_TEST_NO_CH2`, allowing HPET calibration to be exercised when available. |

`tools/build/jobs.mk` enables automatic parallelism for its listed product goals when GNU Make exposes the `shell-export` feature. It respects a supplied job count or inherited jobserver.

Product rules use compiler dependency files to track headers. Configuration stamps maintained by `tools/build/config.mk` and `tools/build/config.py` track compiler commands and options. `tools/build/product.mk`, `tools/build/variants.mk` and `tools/build/extras.mk` attach those stamps to registered outputs.

The hardware settings are consumed by `c/kernel/diag/hwhud.c`, `c/boot/efi/loader.c` and `c/kernel/core/ktime.c`.

## Building artifacts

The ISO contains the kernel and BIOS loaders. The development disk contains applications, guest test programs and filesystem data. The release disk uses the shared package inventory with release exclusions and a generated wallpaper.

Expressions such as `$(DISK)` below are Makefile notation. Pass the expanded pathname when requesting a file target.

| Target | Default output | What it does |
|---|---|---|
| `all` | `build/logit.iso` | Builds `$(ISO)`. This is the default goal. |
| `$(KERNEL)` | `build/kernel.elf` | Links the kernel and writes `kernel.map` under `BUILD`. |
| `$(ISO)` | `build/logit.iso` | Packages the kernel, BIOS preload and BIOS loader through `tools/mkiso.py`. |
| `$(DISK)` | `build/disk.img` | Builds application dependencies and packages the development LogitFS image through `tools/mkfs.py`. |
| `release-disk` | `build/release.img` | Builds `$(RELEASE_DISK)` from the shared inventory, applies release exclusions and adds the wallpaper. |
| `esp` | `build/esp.img` | Packages the UEFI loader and kernel through `tools/mkesp.py`. Defined in `tests/uefi.mk`. |
| `iso-grub` | `build/logit-grub.iso` | Builds a GRUB ISO. The default input is the GRUB oracle kernel; `GRUB_INPUT_KERNEL` selects another input. |
| `sysroot` | `build/sysroot` | Stages C headers, startup objects and libraries. Defined in `tests/sysroot.mk`. |
| `$(POLARIS_FW_PIN_H)` | `build/gen/polaris_fw_pin.h` | Generates firmware digest pins from `third_party/amd-firmware/MANIFEST` through `tools/gpu/amd/fwpin.py`. |

The development and release disk recipes preserve selected user state from each image’s previous contents. `MKFS_PROFILE` in `Makefile` lists the preservation arguments; `tools/mkfs.py` and `tools/disk_profile.py` implement the merge and replacement process.

Both disk images install the firmware directory `third_party/amd-firmware/amdgpu` at `/lib/firmware/amdgpu`. They also package `third_party/amd-firmware/LICENSE.amdgpu` and `third_party/amd-firmware/MANIFEST` under `/licenses/amd-firmware`. `AMD_FIRMWARE_FILES` adds the individual binaries as development disk prerequisites, and the release disk depends on that disk.

The firmware reader object depends on the generated pin header before compilation. `c/drivers/gpu/amd/polaris/resource/files.c` checks loaded firmware against these manifest digests.

## Running and inspecting QEMU

`Makefile` defines the BIOS launch commands. `tests/uefi.mk` supplies the UEFI launch, while `tests/agent.mk` defines the session launcher used by `run` and `run-agent`.

| Target | What it does |
|---|---|
| `run` | Builds the ISO, session tools and selected `RUN_DISK`, then launches QEMU through `tools/agent_session.py`. The default disk is `$(RELEASE_DISK)`. A command-line `DISK` assignment selects that disk unless `RUN_DISK` is also supplied. |
| `run-dev` | Invokes `run` with `$(DISK)` selected as the run image. |
| `run-uefi` | Builds the ESP, development disk and a local firmware variables image, then starts QEMU with OVMF. The ESP uses AHCI; the LogitFS disk uses VirtIO block. |
| `run-agent` | Builds the ISO and session tools, then launches a session using the existing image selected by `AGENT_SESSION_DISK`. |
| `shot` | Builds its ISO and disk prerequisites, then uses `tools/shot.sh` to boot without a display window, capture the scanout through QMP and write a PNG. |
| `debug` | Boots the ISO and development disk with QEMU’s GDB server on port 1234, paused at startup. |

`AGENT_ENV` selects the configuration file passed to the session launcher. For `run`, the launcher uses optional configuration mode: absent model credentials produce a terminal diagnostic and an offline desktop. See `tools/agent_session.py`.

The interactive BIOS defaults are 1 GiB RAM, four CPUs using TCG, the `max` CPU model, a VirtIO GPU at 1920×1200, E1000 networking and HDA output through CoreAudio. Their settings are `QEMU_RAM`, `QEMU_SMP`, `QEMU_CPU`, `QEMU_GPU`, `QEMU_NET` and `QEMU_SND`.

For `run-uefi`, `OVMF_CODE` selects the firmware code image and `OVMF_VARS_SRC` selects the template copied into the build directory. The device arrangement and launch options are defined in `tests/uefi.mk`.

## Boot and subsystem checks

`test` requires host cryptography and networking checks, the ISO and the development disk before starting its boot recipe. `tests/boot/run-test.sh` checks `LOGIT_BOOT_OK` and the expected trust-store banner.

The environment column describes where checks execute. Host checks can also require compiled guest artifacts as fixtures.

| Target | Environment | What it checks |
|---|---|---|
| `test` | Host and QEMU | Runs `test-crypto` and `test-net`, builds both images and runs the boot smoke check. |
| `test-fullsystem` | QEMU | Runs the full-system harness with an application inventory derived from `ROOT_AEX_PACK`. Defined in `tests/fullsystem.mk`. |
| `test-mkiso` | Host and QEMU | Checks ISO structure and firmware execution of a marker fixture, with prerequisite controls. Defined in `tests/bootself.mk`. |
| `test-boot-contract` | Host | Checks the native boot header against BIOS, UEFI and kernel source consumers. Defined in `tests/bootself.mk`. |
| `test-bios-native` | QEMU | Runs native-entry controls, compares boot-information consumers against the GRUB oracle and boots the native kernel. Defined in `tests/bootself.mk`. |
| `test-uefi-native`, `test-uefi` | Host and QEMU | Check load policy, native-entry controls, boot-information comparisons and an OVMF boot. `test-uefi` depends on `test-uefi-native`. Defined in `tests/uefi.mk`. |
| `test-mm`, `test-mm-os` | Host; QEMU | Exercise memory management through host fixtures and the guest memory harness. Defined in `Makefile`. |
| `test-kheap` | Host | Checks heap allocation invariants and requires the cross-core free regression fixture. Defined in `Makefile`. |
| `test-exec`, `test-exec-os` | Host; QEMU | Exercise program loading through host fixtures and guest execution checks. Defined in `tests/exec.mk`. |
| `test-smp` | QEMU | Runs the guest SMP concurrency harness. Defined in `Makefile`. |
| `test-fb-clip` | Host | Compares clipped drawing with complete renders and checks presentation accounting. Defined in `Makefile`. |
| `test-fs-host` | Host | Groups block-cache, journal, simulated crash, filesystem-checker, format, bulk-read and metadata checks. Defined in `Makefile`. |
| `test-fs-boot` | QEMU | Groups mount, durability, crash, replay, large-file, barrier and metadata persistence checks. Defined in `Makefile`. |
| `test-fs` | Host and QEMU | Requires both filesystem groups. |
| `test-net`, `test-net-os` | Host; QEMU | Run protocol fixtures and a guest E1000, IPv4, TCP and HTTP transfer against a host server. Defined in `Makefile`. |
| `test-ip6`, `test-ip6-os` | Host; QEMU | Run IPv6, neighbour discovery, DNS and IPv4 fallback fixtures, and the guest IPv6 harness. Defined in `Makefile`. |
| `test-dns` | Host | Exercises DNS parsing and query progression, with a name-matching control. Defined in `tests/net.mk`. |
| `test-crypto` | Host | Runs cryptographic vectors, arithmetic, RNG, CPU-feature and AES checks, then checks generated trust roots. Defined in `Makefile`. |
| `test-crypto-diff` | Host | Generates reference vectors and compares implementation output against them. Defined in `Makefile`. |
| `test-tls-interop` | Host | Exercises TLS interoperability against an OpenSSL server. Defined in `Makefile`. |
| `test-nvme` | QEMU | Runs the boot smoke harness with the filesystem disk attached through NVMe. Defined in `Makefile`. |
| `test-ahci` | QEMU | Runs storage checks with raw, MBR, GPT and two-disk configurations. Defined in `Makefile`. |
| `test-usb` | Host and QEMU | Groups USB fixtures, input delivery, controller absence, coexistence and controls. Defined in `tests/usb.mk`. |
| `test-audio` | Host and QEMU | Groups PCM checks, captured playback, mixing, underrun recovery, device absence and capture-graph fixtures. Defined in `tests/audio.mk`. |
| `test-driver-host`, `test-driver-os` | Host; host and QEMU | Group driver fixtures and guest driver checks. Defined in `tests/driver_expansion.mk`. |
| `test-sysroot`, `test-sysroot-os` | Host; QEMU | Check sysroot layout, headers, linking and image contents, and guest execution of a linked program. Defined in `tests/sysroot.mk`. |
| `test-agent` | Host | Groups runtime, channel, storage, configuration, memory, session, review and Unix-socket polling checks. Defined across `tests/agent.mk` and `tests/net.mk`. |

A negative control checks whether assertions detect a chosen defect. For example, `test-mm` requires `test-mm-negctl` before running its positive fixtures. Each target’s owning Makefile or fragment records its control dependencies.

## Browser checks and corpus tools

`Makefile` defines focused browser host checks. The frame fragments add integration checks, and `tests/wpt.mk` and `tests/reftest.mk` provide corpus runners, reports and baseline operations.

| Target | What it does |
|---|---|
| `test-browser` | Runs host checks for parsing, DOM, CSS, layout, painting and related browser behavior. |
| `test-frame-sandbox-parse`, `test-frame-sandbox-host` | Check sandbox token parsing and enforcement through iframe attributes and CSP sandbox directives, with prerequisite controls. Defined in `tests/frame_sandbox.mk`. |
| `test-frame-sandbox-host-san` | Runs the sandbox host modes with AddressSanitizer and UndefinedBehaviorSanitizer after `test-frame-sandbox-host`. |
| `test-frame-sandbox-guest` | Boots a page with frames from two origins in QEMU and checks serial output and HTTP request logs, after its browser control. |
| `test-frame-window` | Exercises same-origin frame Window integration and timer cleanup on the host, after wiring, liveness and cancellation controls. Defined in `tests/frame_window.mk`. |
| `wpt-fetch` | Fetches the selected Web Platform Tests corpus through `tools/wpt_fetch.sh`. |
| `test-wpt` | Runs the WPT runner with strict baseline checking and prerequisite controls. |
| `wpt` | Runs WPT in reporting mode. |
| `wpt-list`, `wpt-rank` | List selected files or produce a ranked failure report. |
| `wpt-baseline` | Writes failures from the run to the configured baseline. |
| `test-reftest` | Runs rendering comparisons against the baseline, with comparator and rendering controls. |
| `reftest` | Runs rendering comparisons in reporting mode. |
| `reftest-one`, `reftest-diff` | Run the case selected by `TEST`; the latter also writes test, reference and difference images. |
| `reftest-baseline` | Rewrites the rendering expected-failure baseline from the selected run. |
| `reftest-ahem-fetch` | Fetches the Ahem font used by rendering tests. |

WPT settings include `WPT_ROOT`, `ONLY`, `SUBSET`, `J` and `ORDER` for corpus location, filtering, worker count and shuffle seed. Rendering comparisons use `REFT_FILTER` and `TEST`.

Baseline-writing targets change expectations used by later checks. Use a full WPT run to regenerate the committed baseline. For a filtered run, select another output through `WPT_BASELINE`; `tests/unit/wpt_test.c` enforces this distinction.

## Broad runs and revision verification

`tests/sweep.mk` provides working-tree sweeps. `tests/ci.mk` invokes the CI driver, while `tests/fullsystem.mk` exposes revision verification.

| Target | What it does |
|---|---|
| `test-sweep` | Builds the ISO and disk, runs discovered host targets in parallel and guest targets serially, confirms failures individually, then applies the result-file verdict. |
| `test-sweep-host` | Runs the host portion, confirms failures and applies the result-file verdict. |
| `test-sweep-resume` | Runs targets missing from an existing sweep’s records and invokes the confirmation script. |
| `test-sweep-confirm` | Rechecks failures from an existing result file and applies the result-file verdict. |
| `verify-commit` | By default, clones the selected revision, builds both ISO and disk, then runs `test-fullsystem` through `tools/verify-commit.sh`. `REV` selects the revision; `VERIFY_ARGS` supplies script options. |
| `ci` | Invokes `tools/ci.sh` for audits, a clone of HEAD, a build and discovered host suites. |
| `ci-here` | Invokes the CI driver against the working tree. |
| `ci-host` | Runs prerequisites attached to this aggregate by included fragments. |
| `ci-boot` | Runs attached prerequisites, then invokes the CI driver with its boot-suite selection. |
| `ci-all` | Invokes the CI driver with host and boot suites selected. |

The sweep settings `SWEEP_JOBS`, `SWEEP_TMO` and `SWEEP_OUT` control parallelism, per-target timeout and result-file location. After resuming a sweep, run `test-sweep-confirm` to apply the Makefile’s verdict.

The CI driver derives its suite selection through `tools/audit_tests.py`. Settings include `CI_DIR`, `CI_JOBS` and `CI_LOG`. The driver reports test-audit findings as advisory and treats negative-control drift as a failure. See `tools/ci.sh`.

## Build and repository audits

| Target | What it checks |
|---|---|
| `test-mk-wired` | Checks transitive reachability of test fragments, including nested directories and declared exceptions. Requires its regression fixture through `tests/tools/make/wired.mk`. |
| `test-audit`, `test-audit-list` | Run the test audit or print its inventory. Defined in `tests/ci.mk`. |
| `test-negctl-drift` | Checks source, flag and recipe drift between controls and positive counterparts. Defined in `tests/ci.mk`. |
| `check-test-liveness` | Checks test scripts for failing paths and reports other harness concerns through `tools/check-test-liveness.py`. |
| `test-license-audit` | Runs provenance and licensing checks after its controls. Defined in `tests/license.mk`. |
| `check-abi` | Checks generated ABI files through `tools/gen_abi.py`. |
| `check-roots` | Regenerates a temporary trust bundle and compares it with the checked-in bundle. |
| `docs-stale` | Lists pages whose cited sources changed between their recorded verification commit and HEAD. Also reports pages without verification records. Defined in `Makefile` through `tools/docs/stale.py`. |
| `test-build-incremental` | Checks incremental compilation and link decisions. |
| `test-build-sysroot-idle` | Checks idle sysroot and disk dependency decisions. |
| `test-build-config` | Checks rebuilding after compiler or option changes. |
| `test-build-jobs` | Checks scheduling policy with small Make fixtures. |
| `test-build-dir` | Checks creation of a fresh output directory for host checks. |
| `test-build-relocation` | Checks dependency behavior after source relocation. |

The build regression targets are defined in `tests/build_incremental.mk`, `tests/build_config.mk`, `tests/build_jobs.mk`, `tests/build_dir.mk` and `tests/tools/make/relocation.mk`. Each requires its negative control.

## Fonts and cleanup

These targets are defined in `Makefile`.

| Target | What it does |
|---|---|
| `verify-font-sources` | Checks vendored source-font hashes. |
| `verify-fonts` | Checks source-font and packaged runtime-font hashes. |
| `regen-fonts` | Regenerates runtime font subsets through `tools/mkfont.py` and verifies their hashes. |
| `clean` | Recursively removes the selected `BUILD` directory. |
| `clean-scratch` | Recursively removes repository-root entries matching `build-*`, `refute-*` and `wbuild`, including matching directories used by other workflows. |

## Limitations

- `shot` reads fixed inputs at `build/logit.iso` and `build/disk.img`, defaults to `build/desktop.png`, and requests a 1280×800 GPU mode. With another `BUILD`, its prerequisites and capture inputs can refer to different builds. See `tools/shot.sh`.
- Build isolation is partial: archive paths selected by `RUST_LIB` and `RUST_LIB_HOST` are shared across `BUILD` choices, and the interactive launch recipes use a fixed QMP socket path.
- The EFI loader rule tracks source changes but does not track changes to `HWHUD`. Use a fresh `BUILD` directory when switching that setting. See `tests/uefi.mk`.
- The `esp` recipe supplies loader and kernel inputs. To add the firmware set to an ESP, invoke `tools/mkesp.py` with `--firmware-dir`; see [Running on real hardware](../getting-started/real-hardware.md).
- Commands using `tools/ci.sh` require Linux. The driver invokes the default Make goal, then requires both ISO and disk outputs. A fresh clone can stop at the missing-disk check because `all` requests the ISO.
- `docs-stale` compares committed revisions. It excludes uncommitted source changes, and missing verification records alone leave its exit status successful. See `tools/docs/stale.py` and `tools/docs/docs_index.py`.
- Some checks report skips when tools, firmware or host capabilities are unavailable. Interpret results in their named execution environment, following `TRANSPARENCY.md`.

## See also

- [Building LogitOS](../getting-started/building.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
- [Build system](../architecture/build-system.md)
- [Testing](../architecture/testing.md)
- [Writing tests](../contributing/writing-tests.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Default goal, artifact rules, packaging, QEMU commands, core checks, fonts and cleanup. |
| `tools/build/jobs.mk`, `tools/build/cache.mk` | Product scheduling and compiler caching. |
| `tools/build/config.mk`, `tools/build/config.py` | Compiler and option tracking. |
| `tools/build/product.mk`, `tools/build/variants.mk`, `tools/build/extras.mk` | Product dependency and configuration registration. |
| `tools/mkiso.py`, `tools/mkfs.py`, `tools/disk_profile.py` | ISO construction, filesystem packaging and state preservation. |
| `tools/gpu/amd/fwpin.py`, `third_party/amd-firmware/MANIFEST` | Firmware pin generation and recorded digests. |
| `third_party/amd-firmware/LICENSE.amdgpu` | Firmware redistribution terms packaged with both disks. |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading and validation against generated pins. |
| `c/kernel/diag/hwhud.c`, `c/boot/efi/loader.c`, `c/kernel/core/ktime.c` | Hardware panel, developer-key loading and calibration hooks. |
| `tools/mkesp.py`, `tests/uefi.mk`, `tests/bootself.mk` | ESP construction, firmware launches and boot checks. |
| `tests/sysroot.mk`, `tools/mksysroot.py` | Sysroot construction and checks. |
| `tests/agent.mk`, `tools/agent_session.py` | Agent checks and session launches. |
| `tools/shot.sh`, `tests/boot/run-test.sh` | Screenshot capture and boot smoke assertions. |
| `tests/exec.mk`, `tests/net.mk` | Program loading, DNS, socket and network checks. |
| `tests/unit/fb_clip_test.c` | Framebuffer clipping and presentation accounting assertions. |
| `tests/usb.mk`, `tests/audio.mk`, `tests/driver_expansion.mk` | USB, audio and driver test groups. |
| `tests/frame_sandbox.mk`, `tests/frame_window.mk` | Frame sandbox and Window integration checks and controls. |
| `tests/wpt.mk`, `tests/reftest.mk`, `tests/unit/wpt_test.c` | Browser corpus targets, settings and WPT baseline handling. |
| `tests/fullsystem.mk`, `tools/verify-commit.sh` | Full-system checks and revision verification. |
| `tests/sweep.mk`, `tests/boot/sweep-targets.sh`, `tests/boot/sweep-resume.sh`, `tests/boot/sweep-confirm.sh` | Sweep execution, resumption, confirmation and verdicts. |
| `tests/ci.mk`, `tools/ci.sh`, `tools/audit_tests.py` | Audit targets, CI orchestration and suite discovery. |
| `tools/negctl_drift.py`, `tools/check-test-liveness.py` | Control drift and harness audits. |
| `tools/mk_wired.py`, `tests/tools/make/wired.mk` | Fragment reachability and its regression prerequisite. |
| `tools/docs/stale.py`, `tools/docs/docs_index.py` | Documentation freshness reporting. |
| `tests/license.mk`, `tools/license_audit.py` | Licensing and provenance checks. |
| `tests/build_incremental.mk`, `tests/build_config.mk` | Incremental and configuration regression targets. |
| `tests/build_jobs.mk`, `tests/build_dir.mk`, `tests/tools/make/relocation.mk` | Scheduling, output-directory and relocation checks. |
| `tools/gen_abi.py`, `tools/genroots.py`, `tools/mkfont.py` | ABI, trust-root and font generation. |
| `TRANSPARENCY.md` | Project claim and evidence rules. |