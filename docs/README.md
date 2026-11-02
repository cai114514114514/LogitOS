# LogitOS documentation

118 pages about the kernel, drivers, file systems, networking, userland, the browser and the build. Every page was written against the source and checked name by name; the same index is the home page of the website documentation.

## Reading paths

**Build and run it:** [Building LogitOS](getting-started/building.md) → [Running LogitOS in QEMU](getting-started/running-in-qemu.md) → [Running on real hardware](getting-started/real-hardware.md) → [Repository map](getting-started/repository-map.md) → [Making your first change](getting-started/first-change.md)

**Understand the system:** [Architecture overview](architecture/overview.md) → [Boot sequence](architecture/boot-sequence.md) → [Kernel layout](architecture/kernel-layout.md) → [Memory model](architecture/memory-model.md) → [Processes and threads](architecture/processes-and-threads.md) → [System calls](architecture/system-calls.md)

**Write a driver:** [Device and driver model](architecture/device-model.md) → [PCI](kernel/pci.md) → [Driver overview](drivers/overview.md) → [USB](drivers/usb.md) → [Kernel diagnostics](kernel/diagnostics.md) → [Writing tests](contributing/writing-tests.md)

**Work on the browser:** [Browser architecture](browser/overview.md) → [Loading pages: fetch, URLs and the HTTP cache](browser/loading-and-fetch.md) → [HTML parsing](browser/html-parsing.md) → [CSS: parsing, cascade and computed style](browser/css.md) → [Layout: block, flex, grid and text](browser/layout.md) → [The JavaScript runtime](browser/javascript-runtime.md) → [Testing the browser](browser/testing.md)

**Contribute:** [Contribution workflow](contributing/workflow.md) → [Writing tests](contributing/writing-tests.md) → [Documentation style](contributing/documentation-style.md) → [Testing](architecture/testing.md) → [Make targets](reference/make-targets.md)

## Getting started

| Page | What it covers |
| --- | --- |
| [Building LogitOS](getting-started/building.md) | LogitOS builds an x86-64 kernel, boot loaders and userland programs with Clang, LLD, NASM and Rust. |
| [Running LogitOS in QEMU](getting-started/running-in-qemu.md) | LogitOS provides BIOS and UEFI launch targets that build a boot image and attach a writable LogitFS disk containing applications and user data. |
| [Running on real hardware](getting-started/real-hardware.md) | LogitOS boots through its BIOS optical loader or its UEFI loader on a FAT volume. |
| [Repository map](getting-started/repository-map.md) | LogitOS groups its operating system, applications, libraries, shared interfaces, host tools, tests, and guest filesystem inputs by responsibility. |
| [Making your first change](getting-started/first-change.md) | This walkthrough adds a regression case for cut, follows its host and guest builds, exercises the existing negative controls, and prepares a commit containing the two edited files. |

## Architecture

| Page | What it covers |
| --- | --- |
| [Architecture overview](architecture/overview.md) | LogitOS is an experimental x86-64 operating system with native BIOS and UEFI loaders, a monolithic kernel, and applications running in ring 3. |
| [Boot sequence](architecture/boot-sequence.md) | LogitOS provides BIOS and UEFI loaders that enter the kernel in x86-64 long mode through the native protocol in include/abi/logitboot.h. |
| [Kernel layout](architecture/kernel-layout.md) | LogitOS links kernel services, drivers, file systems, networking, cryptography, and selected libraries into one x86-64 ELF image. |
| [System calls](architecture/system-calls.md) | LogitOS applications enter the kernel through int 0x80, passing a syscall number and up to three argument words in registers. |
| [Processes and threads](architecture/processes-and-threads.md) | LogitOS represents a process as shared resources and identity, with scheduler threads providing its execution contexts. |
| [Memory model](architecture/memory-model.md) | LogitOS combines a low identity mapping for the kernel, a supervisor direct map of RAM and two virtual address windows for user processes. |
| [Interrupts and time](architecture/interrupts-and-time.md) | LogitOS dispatches CPU exceptions, device interrupts and interprocessor requests through x86-64 interrupt vectors. |
| [Multiprocessor support](architecture/smp.md) | LogitOS starts additional logical CPUs through a shared boot trampoline and brings them into a scheduler with per-CPU execution state and a shared runnable ring. |
| [Device and driver model](architecture/device-model.md) | LogitOS records PCI functions in a device registry and binds them to drivers through match tables and probe callbacks. |
| [Storage stack](architecture/storage-stack.md) | LogitOS routes disk file access through the syscall layer, open file descriptions, the VFS, a filesystem backend and the block device registry. |
| [Network stack](architecture/network-stack.md) | LogitOS carries Ethernet frames from NIC drivers through IPv4 or IPv6 into transport buffers and socket interfaces. |
| [Graphics and windowing](architecture/graphics-and-windowing.md) | LogitOS runs its window manager and desktop compositor in the kernel. |
| [Text and input](architecture/text-and-input.md) | LogitOS routes keyboard and pointer input through the window manager, which handles desktop controls, selects recipients, and queues application events. |
| [Userland](architecture/userland.md) | LogitOS userland comprises desktop applications, command line tools, language runtimes, and system services running as x86-64 processes in ring 3. |
| [Applications and AEX packages](architecture/applications-aex.md) | AEX is LogitOS’s native executable container. |
| [AI-native design](architecture/ai-native-design.md) | LogitOS represents agent work as persistent tasks tied to application identities, source snapshots, and document revisions. |
| [Security model](architecture/security-model.md) | LogitOS implements CPU privilege separation, process address spaces, checked user-memory transfers, credentials, file permissions, and signature verification. |
| [Build system](architecture/build-system.md) | LogitOS uses GNU Make to coordinate compilation, generated interfaces, executable packaging, and image construction. |
| [Testing](architecture/testing.md) | LogitOS tests cover native unit tests, host integration tests, QEMU boots, guest programs, and desktop interactions through the QEMU Machine Protocol (QMP). |

## Kernel

| Page | What it covers |
| --- | --- |
| [BIOS boot path](kernel/boot-bios.md) | LogitOS boots from a BIOS El Torito CD image through a preload at physical address 0x7c00 and a second stage loader at 0x10000. |
| [UEFI boot path](kernel/boot-uefi.md) | LogitOS boots through an x86-64 EFI application that reads the kernel from its boot volume, reserves the kernel’s physical memory, collects firmware information and boot modules, and… |
| [Kernel initialisation](kernel/init.md) | Kernel startup converts the loader’s boot information into memory, interrupt, storage and desktop services, then brings the boot CPU into the scheduler. |
| [CPU setup](kernel/cpu-setup.md) | LogitOS establishes floating point and protection state before entering the C kernel, then installs descriptor tables for interrupts and privilege transitions. |
| [ACPI](kernel/acpi.md) | LogitOS uses ACPI tables to discover processor topology, interrupt routes, PCI configuration windows, timers, and firmware images. |
| [Local APIC and I/O APIC](kernel/apic.md) | LogitOS uses local APICs for CPU timer interrupts and interprocessor interrupts (IPIs). |
| [SMP boot](kernel/smp-boot.md) | LogitOS starts application processors (APs) from the bootstrap processor (BSP), assigns each AP a stack and CPU slot, and admits it to the online CPU set after local initialization. |
| [Physical memory](kernel/physical-memory.md) | LogitOS manages physical memory in 4 KiB frames. |
| [Virtual memory](kernel/virtual-memory.md) | LogitOS combines x86-64 page tables with virtual memory areas that describe reserved user ranges, permissions and backing objects. |
| [Kernel heap](kernel/kernel-heap.md) | The kernel heap provides kmalloc, kmallocquiet, kmalloclow and kfree for dynamic kernel storage. |
| [Page cache, reclaim and swap](kernel/page-cache-and-reclaim.md) | LogitOS recovers physical frames by dropping cached file pages, discarding anonymous pages filled with zeroes, and moving other eligible pages to swap. |
| [Shared memory](kernel/shared-memory.md) | LogitOS shared memory lets processes communicate through mappings of the same physical pages. |
| [Scheduling](kernel/scheduling.md) | LogitOS schedules kernel and user threads from a shared circular run list. |
| [Synchronisation](kernel/synchronization.md) | LogitOS uses ticket spinlocks for short critical sections, wait queues to block threads, and deferred work to run callbacks in the context they need. |
| [Process management](kernel/processes.md) | LogitOS represents a process as an address space, shared resources and a set of scheduler threads. |
| [Program loading](kernel/program-loading.md) | LogitOS loads ELF programs into a private user address space, builds their startup stack, and enters user mode. |
| [Signals](kernel/signals.md) | LogitOS keeps signal dispositions, pending signals and blocked masks in the kernel. |
| [File descriptors, polling and PTYs](kernel/files-and-polling.md) | LogitOS uses reference counts to share open file descriptions between process descriptors. |
| [Debugging support: ptrace and core dumps](kernel/ptrace-and-coredumps.md) | LogitOS provides live process inspection through ptrace and records fatal user CPU exceptions as ELF64 core dumps. |
| [Kernel modules](kernel/modules.md) | LogitOS loads kernel modules from ELF relocatable objects, resolves references against a kernel export table, and registers their drivers with the device model. |
| [PCI](kernel/pci.md) | The PCI layer discovers hardware functions, accesses configuration space, sizes Base Address Registers (BARs), and connects device interrupts through MSI-X, MSI or legacy INTx. |
| [Kernel diagnostics](kernel/diagnostics.md) | LogitOS provides formatted console output, retained logs, diagnostic files, profiling, panic reports, and self-tests through c/kernel/diag. |
| [Time, randomness and settings](kernel/time-rng-settings.md) | LogitOS implements kernel clocks, random generation and persistent settings in c/kernel/core/. |
| [Kernel audio layer](kernel/audio.md) | The kernel audio layer connects application PCM streams to sound controller DMA rings. |
| [Window manager](kernel/window-manager.md) | The LogitOS window manager runs in the kernel and owns window geometry, stacking, focus, desktop controls and composition. |
| [Compositing and effects](kernel/compositing-and-effects.md) | LogitOS composes application canvases, window frames, desktop chrome and overlays into a retained screen back buffer. |
| [GUI events, clipboard and notifications](kernel/gui-events-and-services.md) | LogitOS delivers keyboard and pointer input through the window manager into application event queues. |

## Drivers

| Page | What it covers |
| --- | --- |
| [Driver overview](drivers/overview.md) | LogitOS drivers connect PCI functions, legacy PC hardware and USB devices to the kernel’s storage, network, display, audio and input interfaces. |
| [VirtIO drivers](drivers/virtio.md) | LogitOS implements VirtIO block, network, GPU, entropy, balloon and SCSI drivers over a shared PCI transport. |
| [Storage drivers](drivers/storage.md) | LogitOS exposes disks and partitions through the block device layer in c/drivers/block/. |
| [Ethernet drivers](drivers/network.md) | LogitOS connects Ethernet controllers to the network stack through struct netdev. |
| [Wi-Fi support](drivers/wifi.md) | The code in c/drivers/net/wifi/ implements an 802.11 infrastructure station with passive scanning, Open System authentication, association, WPA2-PSK key exchange and CCMP-128 data… |
| [USB](drivers/usb.md) | LogitOS provides a shared USB core, EHCI and xHCI host controllers, and class drivers for hubs, HID input and mass storage. |
| [GPU drivers](drivers/gpu.md) | LogitOS presents the desktop through the framebuffer layer in c/kernel/gui/fb/fb.c. |
| [Audio drivers](drivers/audio.md) | LogitOS implements HDA, Intel ICH AC97, and Ensoniq ES1370 controllers under c/drivers/audio/. |
| [Keyboard, mouse, serial and VGA](drivers/input-and-console.md) | LogitOS provides PS/2 keyboard and mouse input, COM1 serial communication and VGA text output through c/drivers/char. |
| [Timer hardware](drivers/timers.md) | LogitOS uses PIT interrupts to advance the shared tick counter, TSC or HPET counters to read elapsed time at finer resolution, and the CMOS RTC to establish calendar time. |
| [Power and platform drivers](drivers/power-and-platform.md) | LogitOS uses uACPI to evaluate firmware methods for batteries, external power, thermal zones, shutdown and reset. |

## File systems

| Page | What it covers |
| --- | --- |
| [Virtual file system](filesystems/vfs.md) | The virtual file system (VFS) resolves paths, selects mounts, checks permissions, retrieves metadata, and dispatches operations to filesystem backends. |
| [LogitFS](filesystems/logitfs.md) | LogitFS provides the persistent root file system for LogitOS. |
| [Block cache](filesystems/block-cache.md) | The block cache sits between filesystem operations and the selected root block device, holding LogitFS data and metadata. |
| [procfs and ramfs](filesystems/procfs-and-ramfs.md) | Procfs exposes kernel state through generated files under /proc. |
| [File-system control and tools](filesystems/fs-tools.md) | LogitOS exposes VFS controls and storage benchmarks through synthetic files under /dev. |

## Networking

| Page | What it covers |
| --- | --- |
| [Sockets](networking/sockets.md) | LogitOS provides an asynchronous client broker for DNS, TCP and optional TLS, alongside descriptor sockets for TCP, UDP, UNIX communication and raw ICMP. |
| [Link layer, IPv4 and IPv6](networking/link-and-ip.md) | LogitOS carries IPv4 and IPv6 over Ethernet. |
| [TCP and UDP](networking/tcp-udp.md) | LogitOS implements TCP streams and IPv4 UDP datagrams in c/net/transport/. |
| [DNS](networking/dns.md) | LogitOS resolves host names through the kernel stub resolver in c/net/dns/dns.c. |
| [TLS and certificates](networking/tls.md) | LogitOS provides TLS 1.2 and TLS 1.3 clients, a TLS 1.3 server, X.509 certificate path verification, OCSP stapling checks, and a compiled certificate authority store. |
| [HTTP](networking/http.md) | LogitOS provides HTTP/1.1 and HTTP/2 client engines, HPACK header compression, connection pooling, cookies, URL handling and WebSockets. |
| [SSH](networking/ssh.md) | LogitOS implements an SSH-2 server for remote shells, command execution, SFTP and TCP forwarding. |

## Cryptography

| Page | What it covers |
| --- | --- |
| [Cryptography overview](crypto/overview.md) | LogitOS implements cryptographic primitives in c/crypto and uses them in TLS, SSH, package verification, account authentication and applications. |
| [Hashes, AEADs and KDFs](crypto/symmetric-and-hashing.md) | LogitOS implements hashes, message authentication, symmetric encryption and key derivation in c/crypto/hash, c/crypto/aead and c/crypto/kdf. |
| [Public-key and post-quantum cryptography](crypto/public-key-and-post-quantum.md) | LogitOS implements classical signatures and key agreement in c/crypto/pubkey, with ML-KEM and ML-DSA in c/crypto/pq. |
| [Trust roots and package signing](crypto/trust-and-signing.md) | LogitOS uses Ed25519 signatures for package payloads and application images. |

## Userland

| Page | What it covers |
| --- | --- |
| [The C library](userland/libc.md) | LogitOS provides an application C library in c/apps/libc, called mini-libc in source comments and build rules. |
| [Command-line tools](userland/coreutils.md) | LogitOS keeps 57 command-line programs in c/apps/coreutils/ and its text/ subdirectory. |
| [System services](userland/services.md) | LogitOS provides HTTP and HTTPS serving, SSH access, SFTP file transfer and local logging through user processes in c/apps/services/. |
| [Desktop applications](userland/desktop-apps.md) | LogitOS provides desktop applications for file management, editing, browsing, media playback, settings and agent tasks. |
| [Agent runtime](userland/agent-runtime.md) | LogitOS runs application agents through agentd, a userland broker that manages application context, source grants, model requests, document revisions, and persistent tasks. |

## Browser

| Page | What it covers |
| --- | --- |
| [Browser architecture](browser/overview.md) | The LogitOS browser is a native ring 3 application that combines the browser interface, document engine and JavaScript bindings in one process. |
| [The browser application: windows, tabs and navigation](browser/app-and-navigation.md) | The browser application owns the window, tabs, address field, navigation history and library panels. |
| [Loading pages: fetch, URLs and the HTTP cache](browser/loading-and-fetch.md) | The browser loads documents, subresources and JavaScript Fetch responses in ring 3. |
| [HTML parsing](browser/html-parsing.md) | The browser parses HTML in two stages: a tokenizer turns source bytes into tokens, and a tree builder creates DOM nodes using insertion modes and recovery rules. |
| [The document tree](browser/dom.md) | The browser represents parsed HTML and content created by scripts as a native C tree shared by parsing, CSS, layout, editing, and JavaScript bindings. |
| [CSS: parsing, cascade and computed style](browser/css.md) | The browser combines NetSurf LibCSS with native code for stylesheet assembly, custom properties, nesting, additional properties, animation and JavaScript interfaces. |
| [Layout: block, flex, grid and text](browser/layout.md) | The browser’s layout engine turns a styled document and a canvas width into element geometry, scrollable extents and an ordered display list. |
| [Painting and hit testing](browser/paint.md) | The browser paints a flat display list and searches that list in reverse to resolve pointer targets. |
| [The JavaScript runtime](browser/javascript-runtime.md) | The browser embeds QuickJS and connects it to the document, browser APIs and application event loop. |
| [DOM and event bindings](browser/dom-bindings.md) | The browser exposes its native document tree through QuickJS objects, C accessors and JavaScript preludes. |
| [Frames, documents and isolation](browser/frames-and-isolation.md) | The browser embeds network documents through native document owners and creates local frame documents through auxiliary JavaScript contexts. |
| [Workers, streams and WebAssembly](browser/workers-streams-wasm.md) | The browser implements dedicated workers with QuickJS runtimes, streams with JavaScript state machines, and WebAssembly with the interpreter under c/lib/wasm/. |
| [Storage, crypto, text and media APIs](browser/storage-crypto-media.md) | The browser combines native C services with JavaScript installed into QuickJS contexts. |
| [Browser security model](browser/security-model.md) | The browser carries document identity into requests, filters responses exposed to scripts, applies embedding and sandbox policies to frames, and bounds selected sources of script work… |
| [Testing the browser](browser/testing.md) | Browser tests exercise production components on the host, load packaged pages in QEMU, and check the connection between application sources, linked code, and test runners. |

## AetherScript

| Page | What it covers |
| --- | --- |
| [AetherScript: language and toolchain](aetherscript/overview.md) | AetherScript is LogitOS’s language for scripts and applications. |
| [AetherScript language guide](aetherscript/language-guide.md) | AetherScript 3 uses indentation to structure statically typed programs and compiles them to native code. |
| [AetherScript type system and semantic analysis](aetherscript/type-system.md) | AetherScript’s typed compiler resolves names, infers expression types, checks control flow, and records decisions for execution and LLVM lowering. |
| [AetherScript lexer and parser](aetherscript/frontend.md) | The AetherScript frontend turns source text into a syntax tree used by semantic checking, editor queries, and native code generation. |
| [AetherScript IR and the LLVM backend](aetherscript/ir-and-backend.md) | AetherScript’s native compiler checks a project into a typed tree, then lowers that tree to textual LLVM intermediate representation (IR). |
| [AetherScript runtime and system bindings](aetherscript/runtime-and-system-bindings.md) | Compiled AetherScript programs combine native machine code with the C runtime in c/apps/as/runtime/. |
| [The AetherScript command line and editor support](aetherscript/cli-and-editor.md) | AetherScript’s CLI checks projects, generates LLVM IR, builds native programs, runs tests, and answers semantic completion queries. |
| [Testing AetherScript](aetherscript/testing.md) | AetherScript tests cover compiler diagnostics, generated native programs, the evaluator used by Studio, and compiled applications running inside LogitOS. |

## Libraries

| Page | What it covers |
| --- | --- |
| [Libraries](libraries/overview.md) | LogitOS shares image decoding, media handling, rendering, text processing, inference, and agent support through c/lib/ and the Rust crate in rust/. |
| [OpenLogit 2D: geometry, rasterization and canvas](libraries/openlogit-2d.md) | OpenLogit supplies the software 2D rendering core used by LogitOS applications and the kernel compositor. |
| [OpenLogit effects, animation and SIMD kernels](libraries/openlogit-effects-and-animation.md) | OpenLogit provides software blur and glass effects, animation sampling, retained surface composition, UI interaction helpers and pixel kernels. |
| [OpenLogit 3D: the software renderer](libraries/openlogit-3d.md) | OpenLogit 3D renders indexed triangles on the CPU through programmable vertex and fragment stages, clipping, depth testing, texture sampling and blending. |
| [Using OpenLogit from applications](libraries/openlogit-sdk.md) | OpenLogit provides drawing commands, surfaces, animation, and software 3D rendering for LogitOS applications. |
| [Text rendering: fonts, shaping and line breaking](libraries/text-rendering.md) | LogitOS shares font parsing, bidirectional text processing, shaping, and glyph rasterization through c/lib/text/. |

## Reference

| Page | What it covers |
| --- | --- |
| [Make targets](reference/make-targets.md) | The root Makefile defines kernel builds, filesystem images, QEMU launches and core checks. |
| [System call table](reference/syscall-table.md) | The LogitOS ABI declares 197 syscall numbers, including the historical spawn entry and a reserved GUI receipt entry. |
| [Glossary](reference/glossary.md) | This glossary defines names and abbreviations used in LogitOS source code, build rules and tests. |

## Contributing

| Page | What it covers |
| --- | --- |
| [Contribution workflow](contributing/workflow.md) | A LogitOS contribution brings implementation, design rationale, provenance, and verification evidence to review. |
| [Writing tests](contributing/writing-tests.md) | LogitOS tests exercise production code on the host, run programs inside QEMU, and inspect desktop interaction through the QEMU Machine Protocol (QMP). |
| [Documentation style](contributing/documentation-style.md) | LogitOS documentation helps a systems programmer find the implementation, understand its design, and assess the evidence for its behaviour. |

## For agents

- **In this checkout:** the `logitos-docs` MCP server (`.mcp.json`, `tools/docs/mcp_server.py`) searches these pages, reads a page or one section, and maps a source file to the pages that describe it (`docs_for_source`). Task recipes are skills in `.agents/skills/`.
- **After changing code:** `make docs-stale` lists pages that name a changed file since the commit recorded in `docs/VERIFIED.json`; fix them, then record the check with `python3 tools/docs/stale.py --mark <page>`.
- **On the website:** every page as raw Markdown next to its HTML (also served for `Accept: text/markdown`), `/llms.txt` and `/llms-full.txt`, the same pair per section under `/docs/<section>/`, `/docs-index.json` (summary, headings, sources, make targets, verified commit, possibly stale sources, and `by_source`), and `/.well-known/skills/index.json`.

Older dated design notes and reports are kept in this directory and under `docs/superpowers/`; they record history and may describe code that has since changed.
