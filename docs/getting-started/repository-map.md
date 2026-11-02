# Repository map

LogitOS groups its operating system, applications, libraries, shared interfaces, host tools, tests, and guest filesystem inputs by responsibility. The main C and assembly implementations are under `c/`; Rust image and compression modules live under `rust/src/`. Source inventories in `Makefile` determine which implementations enter each binary, and `LICENSING.md` defines the licence boundaries for project code.

## Top-level directories

| Directory | Contents |
|---|---|
| `c/` | Boot code, kernel, drivers, filesystems, networking, cryptography, libraries, and applications. |
| `include/` | Shared ABI definitions under `include/abi/` and portability support for optional symbols in `include/weaksym.h`. |
| `rust/` | Freestanding Rust static library, with implementations under `rust/src/`. |
| `third_party/` | Imported code, source fonts, GPU firmware, and component licence records. |
| `fsroot/` | Configuration, documents, fonts, dictionaries, examples, and media used to construct guest filesystem images. |
| `tests/` | Test build fragments, host tests, guest programs, QEMU automation, fixtures, and recorded results. |
| `tools/` | Host generators, image builders, build support, audits, corpus utilities, and measurement tools. |
| `examples/` | Browser demonstrations and graphics SDK examples. |
| `docs/` | Developer guides, design references, implementation plans, audits, and engineering reports. |
| `LICENSES/` | Licence texts and notices for project and dependency material. |
| `build/` | Default build output directory. Configuration records under `build/.config/` are managed through `tools/build/config.mk`. |
| `.claude/` | Saved JavaScript task workflows under `.claude/workflows/`. |
| `.vscode/` | Editor configuration, including the compiler task in `.vscode/tasks.json`. |

Several root files serve as entry points:

| File | Role |
|---|---|
| `Makefile` | Source inventories, compilation, linkage, executable packaging, filesystem images, execution targets, and test inclusion. |
| `linker.ld` | Kernel entry point, section placement, and collection of driver registrations. |
| `AGENTS.md` | Workspace rules and evidence requirements. |
| `CLAUDE.md` | Subsystem map and dated engineering notes, including corrections to earlier statements. |
| `README.md` | Project introduction and development entry points. |
| `CONTRIBUTING.md` | Contribution, provenance, licensing, and authorship policy. |
| `TRANSPARENCY.md` | Rules for technical statements and for interpreting test evidence. |
| `SECURITY.md` | Security scope and vulnerability reporting guidance. |
| `LICENSE`, `LICENSING.md`, `THIRD_PARTY.md` | Overall licensing notice, project source licence mapping, and dependency inventory. |
| `RELEASING.md` | Source and binary release requirements. |

Use the documentation to locate an implementation, then follow its headers, callers, and build rules to establish its current behaviour.

## Operating system sources

| Directory | Responsibility |
|---|---|
| `c/boot/` | BIOS and UEFI loaders, kernel entry, interrupt assembly, context switching, entry into user mode, and application processor startup. |
| `c/kernel/` | Kernel initialization and operating system services, connected through `c/kernel/init/kmain.c`. |
| `c/drivers/` | Device registration, DMA and interrupt infrastructure, and hardware drivers. Start with `c/drivers/core/driver.h`. |
| `c/fs/` | VFS, filesystem implementations, block caching, filesystem checking, and control interfaces. Start with `c/fs/vfs/vfs.h`. |
| `c/net/` | Link and IP layers, transports, sockets, DNS, HTTP, TLS, and SSH. Common declarations are in `c/net/core/net.h`. |
| `c/crypto/` | Hashes, authenticated encryption, key derivation, public key and post quantum algorithms, trust anchors, and signature verification. Shared declarations include `c/crypto/crypto.h`. |
| `c/lib/` | Libraries selected for kernel and application consumers by `Makefile`. |
| `c/apps/` | Applications, services, application startup code, the C library, and GUI toolkit. |
| `c/include/` | Shared UI headers under `c/include/ui/`, including design tokens in `c/include/ui/logit_tokens.h`. |

The loaders under `c/boot/bios/` and `c/boot/efi/` reach the native kernel entry in `c/boot/long.asm`. Their shared boot information format is defined in `include/abi/logit_boot.h`.

The UEFI loader in `c/boot/efi/loader.c` also handles display mode selection, firmware module loading, SMBIOS copying, video BIOS ROM copying, and the GPU self-test opt-out. The display selection policy and firmware adapter live in `c/boot/efi/gop_pick.c` and `c/boot/efi/gop_pick_efi.c`.

Within the kernel, the main divisions are:

| Directory | Contents |
|---|---|
| `c/kernel/init/` | Boot information normalization, main initialization, shutdown, and reboot integration. |
| `c/kernel/core/` | Timekeeping, randomness, and settings. |
| `c/kernel/cpu/` | CPU features, protection setup, locks, TLB handling, ACPI, interrupt controllers, processor topology, power support, and SMP startup. |
| `c/kernel/mm/` | Physical allocation, kernel heap, virtual mappings, faults, shared memory, page cache, reclaim, and swap. |
| `c/kernel/sched/` | Scheduling, user thread support, and scheduler observation. |
| `c/kernel/sync/` | Wait primitives and deferred work. |
| `c/kernel/exec/` | Processes, syscall dispatch, program loading, descriptors, polling, PTYs, signals, tracing, and core dumps. |
| `c/kernel/gui/` | Window management, compositing, framebuffer operations, input queues, IME integration, clipboard, and notifications. |
| `c/kernel/audio/` | PCM services, mixing, and capture. |
| `c/kernel/pci/` | PCI enumeration, initialization, interrupt routing, MSI, and MSI-X support. |
| `c/kernel/module/` | Kernel module loading, ELF relocation handling, and exported symbols. |
| `c/kernel/diag/` | Logging, formatting, profiling, panic handling, and runtime diagnostics. |

Scheduler instrumentation lives under `c/kernel/sched/observe/`. Its flight recorder stores event rings for each CPU, and its input ledger records input arrival and outcomes. The hardware overlay is implemented in `c/kernel/diag/hwhud.c`; `make HWHUD=1` enables it.

Hardware work often spans several modules. Board support under `c/drivers/board/` covers SMBIOS, Intel PCH LPC and watchdog handling, Super I/O identification, and NCT6779 sensors with ACPI resource ownership checks. AMD Polaris code under `c/drivers/gpu/amd/polaris/` groups firmware and video BIOS resources, display inspection, memory management, and SDMA and SMU support. `c/drivers/gpu/amd/bootfb.c` handles its display survey and self-test in builds with the hardware overlay enabled.

Storage and audio controllers also have supporting modules. `c/drivers/block/ahci.c` contains polling, MSI completion, NCQ submission, and recovery. HDA routing and analog output planning live in `c/drivers/audio/hda_route.c` and `c/drivers/audio/hda_output_paths.c`, with controller integration in `c/drivers/audio/hda.c`.

Two caches serve different layers. `c/fs/cache/bcache.c` caches device blocks used by filesystems. `c/kernel/mm/cache/pcache.c` caches file pages whose physical frames can also be mapped into process address spaces.

## Applications

| Directory | Contents |
|---|---|
| `c/apps/coreutils/` | Shell, login, command-line utilities, and text tools under `c/apps/coreutils/text/`. |
| `c/apps/gui/` | Desktop applications, including the browser, and the AUI toolkit in `c/apps/gui/aui.c` and `c/apps/gui/aui.h`. |
| `c/apps/services/` | HTTP/HTTPS, SSH/SFTP, and syslog services in its protocol subdirectories. |
| `c/apps/libc/` | Userland C headers under `c/apps/libc/include/` and implementations under `c/apps/libc/src/`. |
| `c/apps/agent/` | Agent broker, manifests, catalog data, and filesystem integration. The broker is implemented in `c/apps/agent/agentd.c`. |
| `c/apps/lm/` | Model loading and text generation command in `c/apps/lm/lm.c`. |
| `c/apps/as/` | AetherScript implementation. |

Shared application support also lives directly in `c/apps/`. Application syscall wrappers are in `c/apps/logit.h`, and `c/apps/crt0_cli.asm` provides command-line startup.

The browser lives under `c/apps/gui/browser/`, with HTML, CSS, layout, painting, and JavaScript subsystems. Its inventory in `Makefile` discovers sources recursively, so new modules are picked up as the directory structure grows.

Frame loading and policy code lives under `c/apps/gui/browser/frame/`. The shared sandbox flag parser is in `c/apps/gui/browser/frame/sandbox.h`; JavaScript document runtime lifetime and sandbox policy integration are handled in `c/apps/gui/browser/js/runtime/page.c`.

## Libraries and build membership

| Directory | Contents |
|---|---|
| `c/lib/agent/` | Agent protocol, tasks, model access, review, registry, and storage support. |
| `c/lib/audio/` | Audio decoders, bitstream handling, and signal processing. |
| `c/lib/video/` | Video decoders and codec support. |
| `c/lib/media/` | Container demultiplexing, playback clocks, and subtitle parsing. |
| `c/lib/image/` | Image dispatch, metadata, and C image format implementations. |
| `c/lib/text/` | Unicode handling, fonts, shaping, bidirectional text, glyph rasterization, and paragraph layout. |
| `c/lib/ime/` | Pinyin input handling, dictionary formats, generation support, and vocabulary inputs. |
| `c/lib/nn/` | Tensors, quantization, model loading, and inference operations. |
| `c/lib/wasm/` | WebAssembly parsing, validation, and execution. |
| `c/lib/logitparserutils/` | Parsing support: buffers, containers, encoding conversion, and streamed input. |
| `c/lib/logitwapcaplet/` | String interning used by LibCSS. |
| `c/lib/gfx/` | OpenLogit 2D graphics and animation support. |
| `c/lib/gfx3d/` | OpenLogit software 3D and shader support. |

Freestanding memory and string routines live directly in `c/lib/string.c`.

Directory placement indicates responsibility, while the source and link inventories in `Makefile` establish binary membership. Audio and video decoding, media containers, inference, agent support, CSS support libraries, and software 3D have userland build rules. The kernel also compiles image, text, IME, graphics, and WebAssembly code selected by its source inventory.

Allocation patterns help explain this arrangement. Continuous media decoding maintains stream state and uses the userland allocator. The kernel audio layer provides the PCM and mixing services needed to feed devices, with its interface documented in `c/kernel/audio/snd.h`.

Networking divides the same way: `c/net/http/` contains both kernel and userland implementations, while SSH sources are selected for userland consumers. Follow the source lists and link rules when changing shared code.

## Shared interfaces and Rust

`include/abi/` contains definitions shared across the kernel and applications: system calls, execution, boot information, agents, filesystem references, PTYs, desktop interfaces, and scheduler observation. For a syscall change, follow the interface through `include/abi/logit_abi.h`, application wrappers in `c/apps/logit.h`, and dispatch in `c/kernel/exec/syscall.c`.

Generated interfaces identify their inputs and generators. For example, `include/abi/logit_pack.h` is generated by `tools/gen_abi.py` from `include/abi/logit_calls.abi`. Change the authoritative input and regenerate its consumers together.

`include/weaksym.h` supplies linkage support for optional symbols in ELF and Mach-O builds. Focused host tests use it when compiling selected implementations with optional dependencies.

`rust/Cargo.toml` defines a freestanding static library. `rust/src/` contains image buffers, DEFLATE and zlib decompression, image decoders, PNG encoding, and VP8 modules. `rust/src/lib.rs` records the C interface constraints, and `Makefile` links the archive into the kernel and image-consuming applications.

## Tests and recorded evidence

Test build fragments live directly under `tests/` and in nested subsystem directories. `tools/mk_wired.py` checks their reachability through the root build; its regression cases live under `tests/tools/make/`.

| Directory | Contents |
|---|---|
| `tests/unit/` | Host unit, fuzz, and differential tests, stub headers, reference adapters, and fixture generators. |
| `tests/boot/` | QEMU boot and system test orchestration, supporting servers, image preparation, and result checks. |
| `tests/qmp/` | QEMU monitor automation, keyboard and mouse injection, screenshot drivers, and guest inspection helpers. |
| `tests/guest/` | Programs executed inside LogitOS to exercise kernel and library behaviour. |
| `tests/drivers/` | Driver harnesses, device models, controls, and evidence checks. |
| `tests/gpu/` | GPU tests, fixtures, and build fragments. |
| `tests/fixtures/` | Input corpora, sample files, web pages, and test programs. |
| `tests/golden/` | Stored rendering references, including the AUI images described in `tests/golden/aui/README.md`. |
| `tests/bench/` | Benchmark programs. |
| `tests/chrome/` | Chrome comparison tooling. |
| `tests/events/` | Event propagation and ordering fixtures. |
| `tests/jsfb/` | JavaScript framework benchmark baselines and implementation selection records. |
| `tests/jsmicro/` | JavaScript microtask ordering cases and comparison harnesses. |
| `tests/jssem/` | JavaScript semantic cases and host and guest comparison support. |
| `tests/textutils/` | Text tool cases, input trees, and a host comparison runner. |
| `tests/wpt-local/` | Project cases written in Web Platform Tests format. |
| `tests/tools/` | Development tool regression tests, including Makefile analysis and build configuration. |
| `tests/scoreboard/` | Recorded browser corpus results and supporting output. |

Browser frame tests are defined in `tests/frame_sandbox.mk` and `tests/frame_window.mk`. The parser, host, guest, and frame window targets attach negative controls as prerequisites.

Execution domain matters when reading results. `tests/jssem.mk`, for example, describes a host QuickJS comparison and a guest run using browser engine objects. Interpret recorded evidence using its revision, environment, and workload, following `TRANSPARENCY.md`.

## Host tools

Files directly under `tools/` include image and executable builders, generators, CI drivers, audits, corpus fetchers, and model utilities. Useful entry points include `tools/mkiso.py` for ISO construction, `tools/mkfs.py` for filesystem images, `tools/mkaex.py` for executable packaging, and `tools/mksysroot.py` for the guest C development environment.

| Directory | Contents |
|---|---|
| `tools/build/` | Make support for job scheduling, compiler caching, configuration tracking, product inventories, and build variants. |
| `tools/coldcode/` | Probe builds with coverage instrumentation and reports showing which functions a workload exercises. |
| `tools/gpu/` | Offline AMD firmware inspection, packaging, staging, and digest generation utilities. |
| `tools/mkicons/` | Host icon generator, vector path inputs, and rendering support. |
| `tools/mmtrace/` | QEMU memory reference tracing, replacement policy simulation, controls, and reports. |
| `tools/perf/` | Guest performance measurement, comparison, and regression detection. |
| `tools/pkgroots/` | Package trust key input and a published development signing seed, documented in `tools/pkgroots/DEV-SIGNING-KEY.txt`. |
| `tools/psl/` | Public Suffix List snapshot, provenance, generator, and verification data. |
| `tools/roots/` | Root CA certificates, provenance records, and verification support for the compiled trust store. |
| `tools/windows/` | Root image installer in `tools/windows/install-root.ps1` and partition planning policy in `tools/windows/RootPlan.psm1`. |

Each measurement tool answers a different question. `tools/mmtrace/` records guest page reference sequences for offline policy comparisons; `tools/perf/` collects performance measurements using guest clocks and counters.

`tools/gpu/amd/fwpin.py` generates the Polaris firmware digest table from `third_party/amd-firmware/MANIFEST`, keeping the accepted hashes tied to the vendored inventory.

## Guest filesystem inputs

The image recipe in `Makefile` combines selected `fsroot/` inputs with built executables, generated assets, notices, and a staged sysroot. `tools/mkfs.py` applies destination mappings and recursively packs selected directories. A new file beneath a selected directory can enter the guest image through that recursive packing.

| Directory | Contents |
|---|---|
| `fsroot/as/` | AetherScript examples and library sources. |
| `fsroot/docs/` | Guest documents and text rendering samples. |
| `fsroot/etc/` | Configuration inputs, including `fsroot/etc/agent.conf`. |
| `fsroot/fonts/` | Runtime font subsets, hashes, and source descriptions. |
| `fsroot/icons/` | Icon artwork records and asset handling notes. |
| `fsroot/ime/` | Pinyin dictionaries. |
| `fsroot/licenses/` | Guest data notices, including the vocabulary supplement notice. |
| `fsroot/media/` | Sample audio files. |
| `fsroot/www/` | Demonstration web content and SSH file transfer instructions. |

Inspect the image mappings when tracing a guest pathname back to its source. Executables, fonts, firmware, and development files can originate elsewhere in the repository.

For example, `Makefile` maps `third_party/amd-firmware/amdgpu/` to the guest’s `/lib/firmware/amdgpu/` and installs its licence and manifest alongside the other notices. `tools/mkesp.py` accepts `--firmware-dir` to stage firmware on an EFI System Partition; the UEFI loader passes those files as boot modules. The consumer in `c/drivers/gpu/amd/polaris/resource/files.c` checks their SHA-256 digests against the generated pins.

## Documentation, examples, and workflows

| Directory | Contents |
|---|---|
| `docs/getting-started/` | Building, QEMU, hardware, repository navigation, and first-change guides. |
| `docs/architecture/` | System architecture and subsystem overviews. |
| `docs/kernel/` | Kernel implementation guides. |
| `docs/drivers/` | Driver and hardware interface guides. |
| `docs/filesystems/` | VFS, LogitFS, block cache, and filesystem tool guides. |
| `docs/networking/` | Socket and network protocol guides. |
| `docs/crypto/` | Cryptographic primitives, trust roots, and signing guides. |
| `docs/userland/` | C library, command-line tools, services, desktop applications, and agent runtime guides. |
| `docs/libraries/` | Shared library overview. |
| `docs/reference/` | Make target, syscall, and terminology references. |
| `docs/contributing/` | Contribution workflow, test writing, and documentation style. |
| `docs/as/` | AetherScript planning material. |
| `docs/assets/` | Documentation images and screenshots. |
| `docs/bugs/` | Dated subsystem audit findings. |
| `docs/design/` | Visual design references and generation prompts. |
| `docs/engine-plan/` | Browser implementation plans, inventories, and status records. |
| `docs/hw/` | Hardware bring-up records, including `docs/hw/X79-VG4.md`. |
| `docs/reports/` | Engineering reports, measurements, and captured evidence. |
| `docs/superpowers/` | Design specifications and implementation plans. |
| `examples/browser/` | Browser demonstrations, including `examples/browser/signed-report.html`. |
| `examples/openlogit/` | Graphics SDK example sources and assets. |
| `.claude/workflows/` | Saved implementation and review workflows. |

Documents carry different kinds of evidence. `docs/design/settings/prompt.md` identifies its image as a design reference; `docs/as/A4-FEATURE-PLAN.md` identifies its contents as planned work. Use those labels when interpreting their descriptions.

## Vendored code and licences

`LICENSING.md` assigns GPL-3.0-or-later to project code in the operating system core, including most of `c/lib/`. Project applications, public headers, Rust sources, tools, tests, documentation, and shared image sources under `c/lib/image/` use MIT. Imported material retains its upstream terms, and combined binaries follow the distribution rules in `LICENSING.md`.

| Directory | Imported material | Licence records |
|---|---|---|
| `third_party/quickjs/` | QuickJS JavaScript engine and support code. | MIT; `third_party/quickjs/LICENSE`. |
| `third_party/libm/` | Selected musl mathematical functions. | MIT and other permissive terms in individual files; `third_party/libm/COPYRIGHT` and source notices. |
| `third_party/css/` | Locally modified NetSurf LibCSS under `third_party/css/libcss/`. | MIT; `third_party/css/libcss/COPYING`. Local changes are described in `third_party/css/PROVENANCE.md`. |
| `third_party/uacpi/` | uACPI interpreter headers and sources. | MIT; `third_party/uacpi/LICENSE`, with import details in `third_party/uacpi/PROVENANCE.md`. |
| `third_party/tcc/` | TinyCC compiler and runtime support. | LGPL compiler terms in `third_party/tcc/COPYING` and source headers. `third_party/tcc/lib/libtcc1.c` carries GPL-2.0-or-later with an exception for linking its compiled runtime. |
| `third_party/ffmpeg-aacsbr/` | FFmpeg spectral band replication sources and adaptation headers. | Imported sources use LGPL-2.1-or-later; see `third_party/ffmpeg-aacsbr/COPYING.LGPLv2.1` and `third_party/ffmpeg-aacsbr/README.logitos.md`. |
| `third_party/fonts/` | Noto source fonts and DejaVu Sans. | Noto uses OFL-1.1; DejaVu uses the Bitstream Vera terms and Arev copyright notice. Individual records are identified in `third_party/fonts/README.md`. |
| `third_party/amd-firmware/` | AMD Polaris binary microcode from linux-firmware. | Binary redistribution terms in `third_party/amd-firmware/LICENSE.amdgpu`; upstream commit and file hashes in `third_party/amd-firmware/MANIFEST`. |

Older CSS inventories list imported support libraries alongside LibCSS. The current build uses the project implementations in `c/lib/logitparserutils/` and `c/lib/logitwapcaplet/`, whose project code follows the GPL-3.0-or-later core library policy.

External code and data also appear outside `third_party/`:

| Location | Provenance and terms |
|---|---|
| `tools/mmtrace/qemu-plugin.h` | QEMU plugin API header with a GPL-2.0-or-later notice. |
| `tools/roots/` and `c/crypto/trust/roots_bundle.inc` | Mozilla/NSS-derived trust data, classified as MPL-2.0 in `THIRD_PARTY.md`; provenance records are in `tools/roots/README.md`. |
| `tools/psl/` and `c/net/http/cookie_psl.inc` | Public Suffix List source and generated table under MPL-2.0. `tools/psl/README.md` also records the test fixture’s CC0/public domain notice. |
| `c/lib/logitparserutils/registry.c` | Generated IANA character set registry data under CC0-1.0. |
| `fsroot/fonts/` | Modified Noto subsets under OFL-1.1, documented in `fsroot/fonts/README.md`. |
| `c/kernel/diag/hwscreen_font.h` | Generated bitmap font derived from the Noto subset, with its OFL notice embedded in the header. |
| `c/lib/ime/qwen35-zh.tsv.gz` | Derived vocabulary input. Apache-2.0 licensing and supporting source records are in `fsroot/licenses/ime/Qwen3.5-NOTICE.md`. |
| `c/apps/as/` | AetherScript’s recorded clox lineage and retained MIT notice are documented in `THIRD_PARTY.md`. |

Read component notices together with the integration policy when updating dependencies. `LICENSES/` supplies licence texts; component records identify which terms apply.

## Limitations

- Exact upstream import revisions for musl libm and LibCSS remain unrecorded in `THIRD_PARTY.md` and `third_party/css/PROVENANCE.md`.
- TinyCC runtime sources `third_party/tcc/lib/va_list.c` and `third_party/tcc/lib/alloca86_64.S` carry no individual licence notices. `THIRD_PARTY.md` records the unresolved attribution gap.

## See also

- [Building LogitOS](building.md)
- [Making your first change](first-change.md)
- [Architecture overview](../architecture/overview.md)
- [Build system](../architecture/build-system.md)
- [Testing](../architecture/testing.md)
- [Libraries](../libraries/overview.md)
- [Contribution workflow](../contributing/workflow.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Source inventories, binary linkage, image mappings, and test inclusion. |
| `linker.ld` | Kernel entry, section layout, and driver registration collection. |
| `c/kernel/init/kmain.c` | Kernel initialization and subsystem integration. |
| `c/boot/efi/loader.c` | UEFI loading, boot information construction, firmware modules, and kernel handoff. |
| `c/drivers/core/driver.h` | Device enumeration, driver matching, and interrupt contracts. |
| `c/drivers/board/board.c`, `c/drivers/board/nct_acpi.c` | Board initialization and ACPI resource ownership checks. |
| `c/drivers/gpu/amd/bootfb.c` | Boot framebuffer ownership and Polaris survey and self-test integration. |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading and digest verification. |
| `c/fs/vfs/vfs.h` | Filesystem interface and VFS responsibilities. |
| `c/fs/cache/bcache.h`, `c/kernel/mm/cache/pcache.h` | Device block and file page cache designs. |
| `c/kernel/audio/snd.h` | Kernel PCM and mixing interface. |
| `c/apps/gui/browser/frame/sandbox.h`, `c/apps/gui/browser/js/runtime/page.c` | Frame sandbox flags and document runtime integration. |
| `include/abi/logit_abi.h`, `c/apps/logit.h`, `c/kernel/exec/syscall.c` | Shared syscall interface, application wrappers, and kernel dispatch. |
| `rust/Cargo.toml`, `rust/src/lib.rs` | Rust crate configuration, modules, and C integration constraints. |
| `tools/build/config.mk` | Build configuration records and dependencies. |
| `tools/mkfs.py` | Filesystem image construction and recursive directory packing. |
| `tools/mkesp.py` | EFI System Partition construction and firmware staging. |
| `tools/gpu/amd/fwpin.py` | Firmware digest table generation from the vendored manifest. |
| `tools/mk_wired.py` | Test fragment reachability audit. |
| `tests/frame_sandbox.mk`, `tests/frame_window.mk` | Browser frame tests and negative controls. |
| `LICENSING.md` | Project licence boundaries and combined binary rules. |
| `THIRD_PARTY.md` | Dependency provenance, upstream terms, and recorded gaps. |
| `TRANSPARENCY.md` | Evidence requirements and rules for technical statements. |
