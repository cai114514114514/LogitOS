# Glossary

This glossary defines names and abbreviations used in LogitOS source code, build rules and tests. Entries describe the current implementation and explain historical terms that remain in comments.

## ABI

ABI means application binary interface: the conventions compiled components use to exchange arguments, results and data. `include/abi/logit_abi.h` defines LogitOS system call numbers and shared layouts. Applications invoke system calls through `int 0x80`, placing the call number in `rax` and arguments in `rdi`, `rsi` and `rdx`; the result returns in `rax`. The wrappers in `c/apps/libc/src/io.c` implement this convention.

## AEX

AEX is the native executable container, combining application metadata with an ELF64 image. The metadata supports desktop discovery and launch configuration, while the embedded image uses the kernel’s ELF loader. Keeping ELF lets executable formats share the same segment validation and loading machinery. `c/kernel/exec/load/aex.h` defines the container; `c/kernel/exec/load/aex.c` parses and loads it. Version 3 requires an application ID and agent manifest, and computes a hash of the file for the launched application’s identity.

## AetherScript and A3

AetherScript is the project’s programming language. A3 names its current generation, which supports compilation on the host into native programs, including AEX artifacts for LogitOS. `c/apps/as/cli/main.c` provides the command entry point, and `c/apps/as/cli/commands.c` coordinates compilation and artifact publication. On LogitOS, the source shorthand launches an installed native artifact. Historical A2 references describe the earlier bytecode compiler and virtual machine.

## Agent runtime

The agent runtime coordinates application context, tasks and worker processes. Its broker accepts messages over local sockets, checks peer identity, launches workers, and manages checkpoints and results. `include/abi/aex_agent.h` defines application declarations and identity records; `c/lib/agent/sdk.h` defines messages; `c/apps/agent/agentd.c` implements the broker. Peer recognition compares the service user, application ID and image hash against registered applications, using identity obtained through the kernel.

## AUI

AUI is the application UI toolkit layered over LogitOS GUI system calls. It uses immediate mode: application code draws a widget and handles its interaction in the same call, then reconstructs the frame when an update is needed. `c/apps/gui/aui.h` describes this programming model. `c/apps/gui/aui.c` implements event feeding, frame construction and publication, including tracking drawing changes for damage reporting.

## BKL

BKL means big kernel lock, the global lock used by an earlier concurrency model to serialize kernel execution. Historical comments still describe that model. The correction in `c/kernel/cpu/irq/interrupts.c` records the current arrangement: objects have their own locks, and kernel entry tracks nesting depth on each CPU. `c/drivers/core/irq.c` uses the same nesting model for device interrupts and protects callback lifetime through its interrupt registry.

## Block cache

The block cache holds filesystem blocks between LogitFS and the block device layer. Cached writes mark buffers dirty; eviction and synchronization write those buffers to the device. Filesystem ordering comes from synchronization barriers. `c/fs/cache/bcache.h` describes this contract, and `c/fs/cache/bcache.c` implements lookup, eviction and writeback. Synchronization submits batches of up to 16 block spans, retains failed buffers as dirty for a later attempt, and finishes with a device flush.

## Boot module

A boot module is a named file that the UEFI loader places in RAM before entering the kernel. Modules supply inputs such as AMD Polaris firmware. `include/abi/logit_boot.h` describes each module with a name and a physical address range using 64-bit addresses. `c/kernel/init/bootmod.c` validates the ranges and reserves their frames before physical memory initialization, preserving the file contents when the allocator releases available RAM. `c/kernel/init/kmain.c` establishes this initialization order.

## Boot protocol

The native boot protocol is the handoff contract shared by the BIOS loader, UEFI loader and kernel. It specifies the entry CPU state, register arguments, temporary mappings, and a versioned header followed by information tags. `include/abi/logit_boot.h` defines the contract, including memory map, framebuffer and ACPI tags. UEFI extensions carry copied SMBIOS data, display modes and EDID, boot modules, and an indication that the GPU diagnostic self-test should be skipped.

## Capabilities

Capabilities are bits in a process’s authority bitmap. The system call dispatcher checks filesystem and network category grants before dispatching classified calls. Capability spawning checks that the requested child bitmap is a subset of the parent’s bitmap. `include/abi/logit_abi.h` defines the bits, `c/kernel/exec/proc.h` stores the grant, `c/kernel/exec/proc.c` implements the subset check, and `c/kernel/exec/syscall.c` applies the category gate.

## Compositor and damage

The compositor combines window surfaces and desktop elements into the displayed image. Damage describes regions that need recomposition after drawing, movement or another visible change. `c/kernel/gui/wm.c` tracks these regions, while `c/apps/gui/aui_damage.h` tracks toolkit damage rectangles and margins for effects extending beyond a primitive’s geometry. The window manager also supports a software cursor overlay applied during framebuffer presentation, allowing pointer movement to update the affected rectangles directly. `c/kernel/gui/fb/fb_overlay.c` implements overlay composition.

## Device model

The device model connects enumerated hardware with matching drivers. Device records describe hardware identity and resources; drivers supply match tables and probe callbacks. A successful probe binds the device to that driver. `c/drivers/core/driver.h` defines these contracts, and `c/drivers/core/device.c` implements matching, binding and teardown. Before power transitions, the model invokes eligible removal callbacks and checks bus mastering. Display drivers can supply a quiesce callback to report GPU activity while retaining their binding.

## Gate

A gate is a check whose verdict determines whether a test target succeeds. For example, `tests/aui_damage.mk` checks damage coverage and the extent of reported regions. The `test-mk-wired` target in `Makefile` invokes `tools/mk_wired.py` to check test fragment reachability through the include graph, accounting for declared exceptions. The `docs-stale` target invokes `tools/docs/stale.py` to flag pages whose named sources changed after their recorded verification commit.

## Guest and host

The guest is LogitOS executing inside the emulated machine; the host runs development tools and QEMU. Some tests compile kernel algorithms for execution on the host using adapters for machine operations. `c/kernel/mm/mmhost.h`, for example, supplies simulated physical memory addressing and page table operations. These terms identify where code executes and where a measurement was taken.

## HWHUD

HWHUD is the hardware diagnostic panel selected by the `HWHUD=1` build setting in `Makefile`. `c/kernel/diag/hwhud.c` draws display information, CPU and interrupt activity, compositor timing and device diagnostics over the desktop, making them available on machines without a serial connection. The panel hides while system UI occupies the screen, and clicking its title row collapses or expands it. `c/kernel/diag/hwhud.h` defines its window manager interface.

## IME

IME means input method editor. LogitOS integrates pinyin composition and a candidate interface into the window manager’s keyboard dispatch. `c/lib/ime/pinyin.c` implements segmentation and candidate lookup, and `c/kernel/gui/ime/ime_ui.h` describes the window integration. `c/kernel/gui/wm.c` delivers committed characters as `EV_KEY` events carrying Unicode codepoints, so applications receive committed text through their keyboard event path.

## INTx, MSI, MSI-X and GSI

INTx is the PCI interrupt mechanism that routes a device’s interrupt pin through a line that devices can share. MSI and MSI-X deliver interrupts as memory writes encoding a destination and vector. `c/kernel/pci/pci_msi.c` prefers MSI-X, then MSI, then INTx, subject to device support and platform policy. A GSI, or global system interrupt, identifies a platform interrupt input. `c/kernel/pci/pci_prt_aml.c` resolves ACPI PCI routing tables and link devices; `c/drivers/core/irq.c` coordinates vector retirement with callback completion and pending deliveries.

## Kernel diagnostics: kdiag, klog and kprof

These names identify complementary diagnostic facilities. Klog retains kernel log records; kprof collects execution samples and named timing spans; kdiag exposes reports and controls through synthetic files. Their interfaces appear in `c/kernel/diag/klog.h`, `c/kernel/diag/kprof.h` and `c/kernel/diag/kdiag.h`. Klog retains 2,048 records in its rolling ring and preserves the first 2,048 records in a pinned store, keeping early boot messages available after later logging overwrites the ring.

## LogitFS

LogitFS is the native persistent filesystem. It uses 4 KiB blocks, an inode table, a block allocation bitmap, and direct, single indirect and double indirect block pointers. Metadata changes are staged in a transaction log. The commit path flushes file data and log bodies before the commit record, then installs staged blocks at their filesystem locations. `c/fs/logitfs/logitfs_fmt.h` defines disk structures; `c/fs/logitfs/logitfs.c` implements operations, commit and recovery.

## LOGITLM

LOGITLM is the model file format used by the local inference library. It stores model configuration and tensors in one file, with tensor locations computed from the layout. Contiguous tensor storage lets the loader use pointers into mapped file contents, reducing the need to copy model weights. `c/lib/nn/model.h` defines the format, `c/lib/nn/model.c` implements the loader, and `c/apps/lm/lm.c` uses it in the inference application.

## LPK

LPK is the signed payload container associated with the `.lpk` extension. Its header carries the payload length, intended filename, SHA-256 digest, signer public key and Ed25519 signature. Verification checks the digest and signature and compares the signer with compiled trust roots. A caller option supports verification under an arbitrary signer for tools and tests. `c/crypto/trust/pkgsig.h` defines the format and interface, and `c/crypto/trust/pkgsig.c` implements signing and verification.

## LSL and OLS-IR

LSL is OpenLogit’s typed shader source language, with vertex and fragment stages. Its compiler produces OLS-IR, the versioned shader instruction representation used by the software 3D extension. `c/lib/gfx3d/include/openlogit_lsl.h` defines the source language interface, and `c/lib/gfx3d/include/openlogit_3d.h` defines the instruction representation. `c/lib/gfx3d/shader/lsl/compiler.c` validates the generated representation before publishing a compiled shader.

## Mini-libc

Mini-libc is the project’s C library for applications. Its headers and implementations live under `c/apps/libc/`, providing C facilities and wrappers for LogitOS services. For example, `c/apps/libc/src/io.c` implements application I/O through the native system call ABI and translates kernel results into C return values and error reporting. These wrappers provide the operating system interface used by ported C programs.

## Negctl

Negctl abbreviates negative control. It commonly names a build variant with a targeted defect or disabled mechanism, used to demonstrate that a test detects the property being measured. In `tests/aui_damage.mk`, one control removes effect margins and requires damage coverage checks to fail. `tools/negctl_drift.py` compares selected recipe inputs and packaging options between controls and their positive counterparts to expose accumulated differences.

## OOM

OOM means out of memory. LogitOS uses an OOM policy to recover memory from exited processes or select an eligible process for termination when allocation and reclaim need help. `c/kernel/mm/reclaim/oom.c` implements victim selection and a bounded wait for recovery. `c/kernel/mm/virt/fault.c` routes memory allocation failures through this policy and retries recoverable faults with a fresh page table walk, because mappings can change while the caller waits.

## OpenLogit

OpenLogit is the native graphics runtime and its rendering extensions. Its core interface covers devices, surfaces, command lists, submission and resource lifetimes. `c/lib/gfx/include/openlogit.h` defines that interface, and `c/lib/gfx3d/include/openlogit_3d.h` defines the software 3D extension. The toolkit and desktop graphics code use OpenLogit rendering facilities.

## Page cache, or pcache

The page cache holds file pages in physical frames that can be mapped into process address spaces. Its key identifies a file and page index, allowing mappings of the same file page to share a resident frame. Reclaim can recover clean cached pages because their contents can be read from the file again. `c/kernel/mm/cache/pcache.h` describes the ownership model, and `c/kernel/mm/virt/fault.c` obtains and maps cached frames during file faults.

## Physmap

The physmap is the kernel’s supervisor mapping of physical RAM into a reserved virtual address region. It gives kernel code access to RAM beyond the low identity mapping. `c/kernel/mm/phys/physmap.h` defines the layout and mapping readiness queries, and `c/kernel/mm/mmhost.h` supplies address conversion helpers. Page tables and device interfaces retain physical addresses where their contracts require them.

## PMM, VMM and VMA

PMM means physical memory manager: it allocates physical frames and tracks references and pins. VMM means virtual memory manager: it manages page tables and mappings. A VMA, or virtual memory area, records the backing and permissions of an address range, including ranges populated on a later page fault. Their interfaces are defined in `c/kernel/mm/phys/pmm.h`, `c/kernel/mm/virt/vmm.h` and `c/kernel/mm/virt/vma.h`.

## Polaris and GPU lease

Polaris names the AMD GPU family targeted by the RX 580 bring-up code. `c/drivers/gpu/amd/polaris/device.c` inspects device registers, and `c/drivers/gpu/amd/polaris/resource/device.c` surveys firmware and VRAM reservations. A GPU lease records ownership through free, held and retired states, defined in `c/drivers/gpu/amd/gpu_lease.h`. In HWHUD builds, the stage B3 diagnostic self-test acquires the lease before hardware writes and exercises firmware startup and SDMA memory operations within a reserved VRAM arena.

## Processes and threads

A process owns application state, including an address space, file descriptor table, working directory and capability grant. A thread holds execution state that the scheduler can run, block and resume. Kernel work also executes in scheduler threads. `c/kernel/exec/proc.h` defines process state, `c/kernel/sched/sched.h` declares scheduling operations, and `c/kernel/sched/sched.c` defines the thread structure.

## Reclaim, reverse maps and swap

Reclaim recovers physical frames by dropping contents that can be reconstructed or saving contents to swap. Reverse maps identify user mappings that reference a frame, allowing reclaim to locate their page table entries during eviction. Swap provides numbered slots on a block device for saved page contents. `c/kernel/mm/reclaim/reclaim.h`, `c/kernel/mm/reclaim/rmap.h` and `c/kernel/mm/reclaim/swap.h` describe these responsibilities.

## Scheduler observation

Scheduler observation is the measurement facility controlled through `SYS_SCHED_OBSERVE`. Its flight recorder stores events in rings for each CPU, covering wakes, context switches, interrupts and intervals with interrupts disabled. Its input ledger records deadlines and outcomes, and coalesced groups describe inputs merged into a delivered event. `include/abi/logit_sched_observe.h` defines the records and commands; `c/kernel/sched/observe/observe_control.c` manages runs and exports records in pages. Scheduler hooks record when a thread becomes runnable and when it starts executing.

## Shared memory, or SHM

SHM maps physical frames owned by a segment into multiple processes so writes through their mappings are shared. The segment holds a reference to each frame, and mappings acquire additional references. Names and references govern the object’s lifetime, allowing existing mappings to survive removal of its name. `c/kernel/mm/shm.h` defines these ownership rules; `c/kernel/exec/proc.h` defines process-local segment handles and write rights.

## SMP, BSP and AP

SMP means symmetric multiprocessing, the machinery supporting execution across multiple processors. The BSP is the bootstrap processor that begins initialization; APs are application processors started during SMP initialization. `c/kernel/cpu/smp/smp.c` coordinates startup, and `c/kernel/cpu/smp/percpu.h` defines each CPU’s current thread, descriptor tables and kernel entry depth. The SMP code also distributes framebuffer presentation across CPUs and disables that path when repeated timeouts meet the policy in `c/kernel/cpu/smp/present_policy.h`.

## Softirq, tasklet and workqueue

These are mechanisms for deferred work. A softirq is a numbered handler dispatched during kernel exit processing. A tasklet queues a callback on that tier and serializes each item’s execution against itself. A workqueue executes callbacks in a kernel worker thread, allowing handlers to wait for I/O or acquire sleeping locks. `c/kernel/sync/work.h` defines the interfaces, and `c/kernel/sync/work.c` implements dispatch and queue processing.

## Sysroot

The sysroot is the target header and library layout supplied to a compiler running in LogitOS. It contains application headers, the C library archive, startup objects and compiler support files. `tools/mksysroot.py` assembles these inputs in the locations expected by the TinyCC port, allowing application builds to use the compiler’s configured search paths.

## TLB shootdown

A TLB shootdown coordinates invalidation of cached address translations across CPUs. LogitOS publishes a request, sends interprocessor interrupts, flushes translations locally and waits for acknowledgements before returning to the caller. This ordering protects frames from reuse while another CPU retains an older translation. `c/kernel/cpu/tlb.h` describes the protocol, and `c/kernel/cpu/tlb.c` implements request publication, acknowledgement and a fatal stop when the transaction fails.

## VFS

VFS means virtual filesystem, the layer that resolves paths and dispatches operations to mounted filesystems. It manages mount selection, path traversal, links and permission checks, and routes access to synthetic kernel files. `c/fs/vfs/vfs.h` defines filesystem operation interfaces; `c/fs/vfs/vfs.c` implements dispatch and common checks. Applications use these operations to access the filesystem backends.

## Wait queue

A wait queue connects a condition with threads waiting for it to become true. Checking the condition and coordinating the transition to a blocked state under the queue’s locking protocol prevents a wakeup from passing between those steps. Mutexes, semaphores and condition variables build on this machinery. `c/kernel/sync/wait.h` defines the queue and locking rules, and `c/kernel/sched/sched.h` explains how parking and waking coordinate through the scheduler.

## WPT and ratchet

WPT means Web Platform Tests, the external corpus exercised by the browser’s host test harness. A ratchet compares current failure identities with a recorded baseline and rejects new failures. `tests/wpt.mk` enables strict comparison for `test-wpt`. `tests/unit/wpt_test.c` performs the comparison and reports new failures and newly passing entries, making the baseline the gate’s reference.

## Limitations

- The dispatcher’s capability category gate enforces `CAP_FS` and `CAP_NET`; it carries `CAP_RAW`, `CAP_PROC` and `CAP_GUI` grants without checking those categories. See `include/abi/logit_abi.h` and `c/kernel/exec/syscall.c`.
- AEX signature verification records a verdict while loading continues for unsigned images and invalid or untrusted signatures. See `c/kernel/exec/load/aex.c`.
- Writable file mappings are refused in `c/kernel/mm/mmsys.c`.
- Polaris stage B3 tests firmware startup and SDMA within its arena. Native desktop presentation is outside that self-test, and its header marks X79-VG4 hardware validation as pending. See `c/drivers/gpu/amd/polaris/selftest/selftest.h`.
- Scheduler observation’s GUI input receipt call remains unwired. The input ledger accepts the allocation state indicating that no delivery token was minted. See `c/kernel/exec/syscall.c` and `c/kernel/sched/observe/input_ledger.h`.

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_abi.h` | System call convention, numbers, shared layouts and capability bits. |
| `include/abi/logit_boot.h` | Native loader handoff, CPU entry state and information tags. |
| `c/boot/bios/loader.asm`, `c/boot/efi/loader.c` | Loader implementations producing the native boot information. |
| `c/kernel/init/bootmod.h`, `c/kernel/init/bootmod.c`, `c/kernel/init/kmain.c` | Boot module registry, page reservations and initialization order. |
| `include/abi/aex_agent.h` | Agent declarations, activation modes and identity records. |
| `c/kernel/exec/load/aex.h`, `c/kernel/exec/load/aex.c` | AEX format, validation, signature verdicts and loading. |
| `c/apps/as/cli/main.c`, `c/apps/as/cli/commands.c` | AetherScript command entry, native build coordination and installed artifact launch. |
| `c/lib/agent/sdk.h`, `c/apps/agent/agentd.c` | Agent messages, peer recognition and worker coordination. |
| `c/apps/gui/aui.h`, `c/apps/gui/aui.c`, `c/apps/gui/aui_damage.h` | Toolkit programming model, frame publication and damage tracking. |
| `c/kernel/gui/wm.c`, `c/kernel/gui/fb/fb_overlay.c` | Composition, input dispatch and software cursor presentation. |
| `c/kernel/gui/ime/ime_ui.h`, `c/lib/ime/pinyin.c` | Input method integration and pinyin lookup. |
| `c/kernel/cpu/irq/interrupts.c`, `c/drivers/core/irq.c` | Kernel entry nesting and device interrupt lifetime. |
| `c/drivers/core/driver.h`, `c/drivers/core/device.c` | Device resources, driver contracts, matching, binding and quiescence. |
| `c/kernel/pci/pci_msi.c`, `c/kernel/pci/pci_prt_aml.c` | PCI interrupt selection and ACPI routing resolution. |
| `c/drivers/gpu/amd/polaris/device.c`, `c/drivers/gpu/amd/polaris/resource/device.c` | Polaris register inspection and resource survey. |
| `c/drivers/gpu/amd/gpu_lease.h` | GPU ownership states and lease contract. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.h`, `c/drivers/gpu/amd/polaris/selftest/selftest.c` | Stage B3 firmware and SDMA diagnostic self-test. |
| `c/fs/cache/bcache.h`, `c/fs/cache/bcache.c` | Block caching, eviction, batched writeback and barriers. |
| `c/fs/logitfs/logitfs_fmt.h`, `c/fs/logitfs/logitfs.c` | Disk structures, filesystem operations and transactions. |
| `c/fs/vfs/vfs.h`, `c/fs/vfs/vfs.c` | Filesystem interfaces, path dispatch and common checks. |
| `c/kernel/mm/cache/pcache.h`, `c/kernel/mm/virt/fault.c` | File page caching, fault handling and recovery retries. |
| `c/kernel/mm/phys/physmap.h`, `c/kernel/mm/mmhost.h` | RAM mapping layout, address conversion and host adapters. |
| `c/kernel/mm/phys/pmm.h` | Physical frames, reservations, references and pins. |
| `c/kernel/mm/virt/vmm.h`, `c/kernel/mm/virt/vma.h` | Page mappings and virtual area records. |
| `c/kernel/mm/reclaim/reclaim.h`, `c/kernel/mm/reclaim/rmap.h`, `c/kernel/mm/reclaim/swap.h` | Frame recovery, reverse mappings and swap storage. |
| `c/kernel/mm/reclaim/oom.c` | OOM victim selection and recovery waits. |
| `c/kernel/mm/shm.h`, `c/kernel/mm/mmsys.c` | Shared segment ownership and memory system calls. |
| `c/kernel/exec/proc.h`, `c/kernel/exec/proc.c` | Process state, shared memory handles and capability subset checks. |
| `c/kernel/exec/syscall.c` | System call dispatch and capability category checks. |
| `c/kernel/sched/sched.h`, `c/kernel/sched/sched.c` | Thread state, scheduling, parking and observation hooks. |
| `include/abi/logit_sched_observe.h`, `c/kernel/sched/observe/observe_control.c` | Scheduler observation records, run control and export. |
| `c/kernel/sched/observe/flight_recorder.h`, `c/kernel/sched/observe/input_ledger.h` | Event recording and input accounting interfaces. |
| `c/kernel/cpu/smp/smp.c`, `c/kernel/cpu/smp/percpu.h`, `c/kernel/cpu/smp/present_policy.h` | CPU startup, CPU state and parallel presentation policy. |
| `c/kernel/cpu/tlb.h`, `c/kernel/cpu/tlb.c` | Translation invalidation across CPUs. |
| `c/kernel/sync/wait.h`, `c/kernel/sync/work.h`, `c/kernel/sync/work.c` | Wait queues and deferred work. |
| `c/kernel/diag/kdiag.h`, `c/kernel/diag/kdiag.c`, `c/kernel/diag/kprof.h` | Diagnostic files, controls and profiling interfaces. |
| `c/kernel/diag/klog.h`, `c/kernel/diag/klog.c` | Log ring and pinned boot record storage. |
| `c/kernel/diag/hwhud.h`, `c/kernel/diag/hwhud.c` | Hardware panel and window manager integration. |
| `c/crypto/trust/pkgsig.h`, `c/crypto/trust/pkgsig.c` | LPK format, signing and verification. |
| `c/lib/nn/model.h`, `c/lib/nn/model.c`, `c/apps/lm/lm.c` | LOGITLM format, loader and inference application. |
| `c/lib/gfx/include/openlogit.h` | Core graphics runtime interface. |
| `c/lib/gfx3d/include/openlogit_3d.h`, `c/lib/gfx3d/include/openlogit_lsl.h`, `c/lib/gfx3d/shader/lsl/compiler.c` | Software 3D, shader instructions and LSL compilation. |
| `c/apps/libc/src/io.c`, `tools/mksysroot.py` | Application I/O wrappers and compiler filesystem layout. |
| `tests/aui_damage.mk`, `tools/negctl_drift.py` | Damage negative controls and recipe comparison. |
| `Makefile`, `tools/mk_wired.py`, `tools/docs/stale.py` | Build settings, test fragment reachability and documentation freshness checks. |
| `tests/wpt.mk`, `tests/unit/wpt_test.c` | Browser host test configuration and failure baseline comparison. |

## See also

- [Architecture overview](../architecture/overview.md)
- [System calls](../architecture/system-calls.md)
- [Memory model](../architecture/memory-model.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [Agent runtime](../userland/agent-runtime.md)
- [Testing](../architecture/testing.md)
- [Kernel diagnostics](../kernel/diagnostics.md)
- [GPU drivers](../drivers/gpu.md)