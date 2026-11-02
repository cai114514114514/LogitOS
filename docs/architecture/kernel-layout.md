# Kernel layout

LogitOS links kernel services, drivers, file systems, networking, cryptography, and selected libraries into one x86-64 ELF image. The directories under `c/kernel/` group services by responsibility. `Makefile` selects the sources, `linker.ld` defines the image layout, and `c/boot/long.asm` connects loader entry to C initialization.

## Directory responsibilities

Headers generally live beside their implementations. This table covers every directory beneath `c/kernel/`, including nested directories.

| Directory | Responsibility |
|---|---|
| `c/kernel/init/` | Boot orchestration, boot information validation and conversion, boot module retention, shutdown, and reboot. Also contains developer SSH provisioning for hardware diagnostics builds. |
| `c/kernel/core/` | Clocks, software timers, CPU time accounting, random number generation, and persistent settings. |
| `c/kernel/cpu/` | CPU feature discovery, platform inventory, GDT and TSS setup, protection state queries, spinlocks, and TLB invalidation across CPUs. |
| `c/kernel/cpu/acpi/` | Firmware table discovery, bounded mappings, integrity checks, and extraction of interrupt controller and PCI configuration information. |
| `c/kernel/cpu/irq/` | IDT construction, exception and interrupt dispatch, local APIC and I/O APIC control, and APIC configuration policies. |
| `c/kernel/cpu/power/` | CPU idle and performance state policies, firmware power tables, temperature and frequency reporting, machine check handling, and PAT configuration. |
| `c/kernel/cpu/smp/` | Application processor startup, state for each CPU, topology bookkeeping, and coordination of work across CPUs. |
| `c/kernel/diag/` | Formatted output, retained logs, panic reports, backtraces, profiling, runtime diagnostics, and wait primitive self-tests. Also contains boot progress displays, hardware reports, and the desktop hardware panel. |
| `c/kernel/sched/` | Thread scheduling, context switching, blocking and waking, user thread lifecycle, futexes, and kernel benchmarks. |
| `c/kernel/sched/observe/` | Scheduler and interrupt event recording, input outcome tracking, records of coalesced inputs, and syscall control and export of observation runs. |
| `c/kernel/sync/` | Wait queues, mutexes, semaphores, condition variables, reader and writer locks, softirqs, tasklets, and worker jobs. |
| `c/kernel/exec/` | Process management, syscall dispatch, checked access to user memory, process tracing, and shared memory handles belonging to processes. |
| `c/kernel/exec/load/` | ELF and AEX loading, interpreter and script handling, process image creation and replacement, and file metadata syscalls. |
| `c/kernel/exec/fd/` | Open file descriptions and their backends, descriptor polling, event and timer descriptors, and pseudo terminals. |
| `c/kernel/exec/signal/` | Signal state and delivery, user signal frames, signal return, and ELF core dumps. |
| `c/kernel/mm/` | Memory management interfaces, memory syscalls, shared segments, and address translation adapters for kernel and host builds. |
| `c/kernel/mm/phys/` | Physical frame allocation, frame references and pins, kernel heap allocation, memory zones, and the direct map. |
| `c/kernel/mm/virt/` | Page tables, virtual area bookkeeping, page faults, and synchronization of address space operations. |
| `c/kernel/mm/cache/` | File page caching, readahead, and the VFS adapter for the page cache. |
| `c/kernel/mm/reclaim/` | Reverse mappings, page reclamation, swap transfers, and handling of memory exhaustion. |
| `c/kernel/gui/` | Window state and geometry, damage tracking, compositing, desktop surfaces, text rendering, GUI syscalls, clipboard, and notifications. |
| `c/kernel/gui/fb/` | Framebuffer setup, drawing targets, presentation, display scaling, cursor interfaces, and software cursor overlay composition. |
| `c/kernel/gui/ime/` | Pinyin composition, candidate UI, learned candidate weights, and persistence of those weights. |
| `c/kernel/gui/input/` | Input transport from interrupt handlers to the window manager, and application event queues associated with windows. |
| `c/kernel/pci/` | PCI configuration access, enumeration, resources and capabilities, ACPI integration, and MSI-X, MSI, and INTx routing. |
| `c/kernel/audio/` | Audio syscalls, PCM conversion and rings, playback mixing, and capture. Also contains the boot test tone for hardware diagnostics builds. |
| `c/kernel/module/` | Kernel module loading and lifecycle, relocatable ELF parsing and relocation, and the exported symbol table. |

Some file boundaries make algorithms accessible to host tests. `c/kernel/pci/pci_init.c` connects ACPI discovery to enumeration in `c/kernel/pci/pci.c`. Likewise, `c/kernel/exec/fd/kpoll.c` implements readiness waiting through callbacks, while `c/kernel/exec/fd/kpollsys.c` translates user arguments and resolves descriptors. The host harness can supply controlled inputs to the underlying algorithm.

Observation has a similar division. `c/kernel/sched/observe/observe_control.c` manages runs and their syscall interface; `c/kernel/sched/observe/flight_recorder.c` records scheduler and interrupt events in rings for each participating CPU. Scheduler initialization prepares the facility, and a privileged caller starts a run.

## What joins the image

`Makefile` constructs `C_SRC` by recursively finding C files under `c/kernel/`, `c/drivers/`, `c/lib/`, `c/fs/`, `c/net/`, and `c/crypto/`. Its filters select which shared libraries and networking implementations enter the kernel. The build also adds uACPI interpreter sources from `third_party/uacpi/source/`.

| Tree | Contribution |
|---|---|
| `c/drivers/` | Device implementations and shared device infrastructure. |
| `c/fs/` | VFS operations, file system implementations, and block caching. |
| `c/net/` | Networking protocols and services selected by the source filter. |
| `c/crypto/` | Cryptographic primitives and trust handling. |
| `c/lib/` | Selected shared libraries, including graphics, image, and text support. |
| `c/boot/` | Kernel entry, interrupt entry, context switching, entry into user mode, and processor startup assembly. |

`ASM_SRC` matches assembly files directly under `c/boot/`. The BIOS and UEFI loaders use their own build rules. `OBJ` collects the selected C and assembly objects, and the final link combines them with the Rust static library named by `RUST_LIB`.

`Makefile` invokes `ld.lld` with `linker.ld` to produce the ELF output named by `KERNEL`, along with a linker map. The map resolves addresses printed in panic backtraces. Kernel C compilation retains frame pointers, and `c/kernel/diag/panic.c` checks alignment, readable memory, forward progress, and return addresses while walking the frame chain.

## Link layout

`linker.ld` selects `logit_native_start` as the ELF entry point and begins the image at 32 MiB, or `0x02000000`. The linked image uses equal virtual and physical addresses within the loader’s identity mapping.

The linker comment keeps the former 1 MiB placement beside its correction. That image span crossed firmware reservations on UEFI machines, including ACPI NVS under OVMF. The 32 MiB origin moves the image beyond that low firmware workspace while keeping it inside the initial mapping.

| Section or symbol | Placement | Purpose |
|---|---|---|
| `_kernel_start` | At the 32 MiB origin | Beginning of the kernel image. |
| `.boot` | Aligned to 8 bytes | Retains `.multiboot_header` input for the GRUB escape hatch and test oracle. |
| `.text` | Aligned to 4 KiB | Instruction sections. |
| `.rodata` | Aligned to 4 KiB | Constant data, followed by the driver registry aligned to 8 bytes. |
| `.data` | Aligned to 4 KiB | Initialized writable data. |
| `.bss` | Aligned to 4 KiB | Common symbols and storage initialized to zero. |
| `_kernel_end` | Rounded up to 4 KiB | End of the image span used by physical memory reservation. |

The script discards `.eh_frame`, `.eh_frame_hdr`, note sections, and compiler comments. Stack diagnostics use the frame pointer mechanism in `c/kernel/diag/panic.c`.

The native entry stack belongs to `.bss`. `c/boot/long.asm` reserves 32 KiB with 4 KiB alignment and switches to it before making calls. After configuring CPU state, entry calls `logit_boot_normalize()` in `c/kernel/init/bootinfo.c`, calls `cpu_early_init()`, and transfers control to `kernel_main()` in `c/kernel/init/kmain.c`.

Both native loaders use ELF loadable program headers to place the image:

- `c/boot/bios/loader.asm` copies segment contents to their physical destinations and clears each segment’s zero-fill tail.
- `c/boot/efi/loader.c` reserves the complete image span through firmware, clears it, and reads segment contents into the allocation.

The UEFI loader also copies its handoff code and GDT into a page below 1 GiB before changing page tables. Execution can continue through that page when firmware has placed the loader’s own image above the initial identity mapping.

## Image reservation and runtime memory

The linker boundary feeds the physical allocator. `mm_kernel_end_phys()` in `c/kernel/mm/mmhost.h` exposes `_kernel_end` to `c/kernel/mm/phys/pmm.c`. During initialization, the allocator reserves physical memory from zero through the rounded image end, its metadata, and the boot information block.

Allocator metadata occupies available low RAM at or above the image end. The placement search checks coverage by available firmware descriptors and avoids boot information and retained boot modules. It can span adjacent available descriptors and search beyond holes following the image.

Boot modules require their own reservations because the UEFI memory map reports their loader allocations as available RAM. `kernel_main()` calls `bootmod_init()` in `c/kernel/init/bootmod.c` before `pmm_init()`. Module registration records the physical ranges to retain; the allocator excludes those ranges from metadata placement and reserves their frames when releasing available memory.

Runtime heap allocation uses the supervisor direct map. `c/kernel/mm/phys/physmap.h` defines `PHYSMAP_BASE` as `0xffff800000000000`. `c/kernel/mm/phys/pmm.c` constructs mappings there for whole pages reported as available RAM. `c/kernel/mm/phys/kheap.c` uses this alias for heap arenas, including arenas backed by physical pages below 1 GiB.

Executable module storage uses `kmalloc_low()`, which returns memory through the low identity alias. The reason appears in `c/kernel/mm/phys/kheap.c`: module code needs an executable alias and addresses within reach of relative calls into the linked kernel. `c/kernel/module/modload.c` uses this allocator for the block containing loaded module sections.

These allocation domains explain why a kernel pointer may refer either to the fixed low image or to a high virtual address backed by the heap.

## Driver registration

`DRIVER_DECLARE()` in `c/drivers/core/driver.h` emits a pointer to a `struct driver` into the `logit_drivers` input section. `linker.ld` retains these pointers within `.rodata` and brackets them with `__start_logit_drivers` and `__stop_logit_drivers`.

`dev_probe_all()` in `c/drivers/core/device.c` registers the collected drivers and tries their match tables and probe callbacks against discovered devices. `kernel_main()` invokes this pass after PCI enumeration and SMP initialization, when APIC setup is available for device interrupts.

The registry’s placement connects registration to memory ownership. Keeping it before `_kernel_end` includes it in the allocator’s image reservation. The linker comment records the failure this prevents: an orphan section beyond that boundary could occupy frames subsequently handed out as free memory.

## Adding a kernel source file

1. Choose the directory that owns the component’s state and lifecycle. Keep its interface header beside the implementation and split supporting mechanisms into focused files. Follow `AGENTS.md` when explaining design choices, invariants, measurements, and failure modes.

2. Use the existing source discovery rules. A C file under `c/kernel/` enters `C_SRC`; `OBJ` derives its object path, and the C compilation recipe creates the destination directories. Assembly discovery through `ASM_SRC` covers files directly under `c/boot/`.

3. Check header names against the include search. `INCDIRS` gathers a sorted list of directories under `c/` and `include/`, subject to namespace exclusions. Qualified includes help resolve collisions; `c/kernel/exec/proc.c` and `c/kernel/exec/syscall.c` contain examples.

4. Apply the required compiler configuration to firmware integration code. `UACPI_CFLAGS` supplies uACPI headers and interpreter limits. `Makefile` applies it to the interpreter, power drivers, `c/kernel/cpu/power/firmware.c`, `c/drivers/board/nct_acpi.c`, and `c/kernel/pci/pci_prt_aml.c`. Add a corresponding rule when a new source needs that configuration.

5. Connect initialization through the owning subsystem. `c/kernel/init/kmain.c` establishes dependency ordering, including boot module registration before physical allocator initialization and PCI enumeration before device probing. Drivers using the device model provide their match table and probe callback through `DRIVER_DECLARE()`.

6. Review affected host harnesses. Their source lists and include paths define their dependency sets. A new directory may require updating `KCORE_INC`, `KCPU_INC`, `KEXEC_INC`, `KMM_INC`, or `KGUI_INC` in `Makefile`. The corresponding groups using `-iquote` derive from those definitions.

Compiler options `-MMD -MP` generate header dependencies. `tools/build/product.mk` tracks kernel compiler, assembler, and linker configuration changes and forces C objects with missing dependency files through their compilation recipe.

For a new make fragment under `tests/`, connect it to the root build and check reachability with `make test-mk-wired`. `AGENTS.md` requires a negative control to be a prerequisite of its positive test and requires observing the control fail.

## Limitations

- Native entry requires the kernel image to fit within the first 1 GiB. The UEFI loader also requires firmware to grant the image’s fixed physical allocation and stops boot when that reservation fails (`include/abi/logit_boot.h`, `c/boot/efi/loader.c`).
- Initial image mappings use writable, executable 2 MiB pages. The 4 KiB section alignments in `linker.ld` describe placement; per-section protection is absent from the loader entry contract (`include/abi/logit_boot.h`).

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Source discovery, include groups, compilation rules, kernel linking, and test wiring. |
| `tools/build/product.mk` | Product configuration tracking and recovery of missing dependency files. |
| `linker.ld` | Entry symbol, image origin, section placement, driver registry bounds, and image boundaries. |
| `include/abi/logit_boot.h` | Native loader entry contract, initial mapping constants, and boot module format. |
| `c/boot/long.asm` | Native entry, initial stack, CPU setup, and transfer to C. |
| `c/boot/bios/loader.asm` | ELF segment loading and zero fill for BIOS boot. |
| `c/boot/efi/loader.c` | Firmware reservations, ELF segment loading, boot modules, and placement of the handoff code. |
| `c/kernel/init/bootinfo.c` | Validation and conversion of native boot information. |
| `c/kernel/init/bootmod.c` | Boot module validation, registration, and physical range retention. |
| `c/kernel/init/kmain.c` | Subsystem initialization and boot ordering. |
| `c/kernel/cpu/power/firmware.c` | Reading CPU power state tables through uACPI. |
| `c/kernel/sched/observe/observe_control.c` | Observation run lifecycle, syscall handling, and export control. |
| `c/kernel/sched/observe/flight_recorder.c` | Event rings for each CPU and scheduler and interrupt recording. |
| `c/kernel/pci/pci_init.c` | ACPI integration for PCI enumeration. |
| `c/kernel/exec/fd/kpoll.c` | Callback-based readiness waiting. |
| `c/kernel/exec/fd/kpollsys.c` | Descriptor and user argument handling for polling syscalls. |
| `c/kernel/diag/panic.c` | Panic reporting and validation of frame pointer backtraces. |
| `c/kernel/mm/mmhost.h` | Address translation, access to the linker end, and host adapters. |
| `c/kernel/mm/phys/pmm.c` | Image and boot data reservation, metadata placement, and direct map construction. |
| `c/kernel/mm/phys/physmap.h` | Low memory boundary and direct map definitions. |
| `c/kernel/mm/phys/kheap.c` | Heap arenas and the low allocation domain for executable storage. |
| `c/kernel/module/modload.c` | Module allocation, loading, and lifecycle. |
| `c/drivers/core/driver.h` | Driver interface and declaration macro. |
| `c/drivers/core/device.c` | Driver registration, matching, and binding. |
| `AGENTS.md` | Source comment and verification conventions. |

## See also

- [Architecture overview](overview.md)
- [Boot sequence](boot-sequence.md)
- [Memory model](memory-model.md)
- [Device and driver model](device-model.md)
- [Build system](build-system.md)
- [Contribution workflow](../contributing/workflow.md)