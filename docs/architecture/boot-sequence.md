# Boot sequence

LogitOS provides BIOS and UEFI loaders that enter the kernel in x86-64 long mode through the native protocol in `include/abi/logit_boot.h`. The bootstrap processor initialises kernel services and submits the first desktop frame before starting the other processors and completing driver binding. With the root filesystem mounted, the window manager adopts the boot context as a scheduled kernel thread, launches the initial GUI application and starts console login. The main control flow is in `c/boot/long.asm`, `c/kernel/init/kmain.c` and `c/kernel/gui/wm.c`.

## Firmware to desktop

The bootstrap processor, or BSP, runs the loader and kernel initialisation. Application processors, or APs, start during `smp_init()` and wait for scheduler readiness before joining scheduling.

```mermaid
sequenceDiagram
    participant F as Firmware
    participant L as Loader
    participant K as BSP kernel
    participant A as APs
    participant W as Window manager
    participant U as User processes

    alt BIOS
        F->>L: El Torito preload at 0x7c00
        L->>L: Read second stage through INT 13h
        L->>L: Verify A20; collect E820, ACPI and VBE data
        L->>L: Load ELF segments; enter long mode
    else UEFI
        F->>L: Start UEFI application
        L->>L: Load ELF; prepare boot modules and firmware tags
        L->>L: Select GOP mode
        L->>F: GetMemoryMap; ExitBootServices
        L->>L: Low handoff stub installs stack, GDT and page tables
    end

    L->>K: logit_native_start: boot magic and information block
    K->>K: Normalise boot data; initialise CPU features
    K->>K: Initialise interrupts, memory, storage and filesystems
    K->>K: Initialise settings, networking, fonts and image codecs
    K->>W: wm_init; wm_render
    Note over W: First desktop frame submitted

    K->>K: smp_init: configure APICs and calibrate local timer
    K->>A: INIT and startup IPIs
    A->>A: Trampoline and per-CPU initialisation
    A-->>K: Publish completed initialisation
    K->>K: Admit completed AP slots
    A->>A: Wait for scheduler readiness
    K->>K: Bind drivers; attach AHCI interrupts; report hardware

    alt Root filesystem mounted
        K->>K: LOGIT_BOOT_OK
        K->>W: wm_run
        W->>W: sched_init adopts boot context
        W->>A: smp_mark_sched_ready
        A->>A: Create idle context; join scheduling
        W->>W: Start compositor workers; enable interrupts
        W->>U: Request greeter or file manager launch
        W->>U: Spawn console login
        W->>K: Start acpi-init worker
        U->>U: Console login starts enabled services
        U->>U: Authenticate or execute shell
        W->>W: Handle input, session changes and damaged regions
    else Root mount failed
        K->>K: board_init; hwscreen_report
        Note over K: Continuously page through the boot log
    end
```

The ordering follows `c/kernel/init/kmain.c`, `c/kernel/cpu/smp/smp.c` and `c/kernel/gui/wm.c`. The diagnostic branch is implemented in `c/kernel/diag/hwscreen.c`.

## BIOS loader stages

`tools/mkiso.py` constructs an ISO9660 image with an El Torito boot catalog, preload, second stage loader and kernel payload. It patches the payload locations into the stages so they can read the required sectors directly.

Firmware loads the preload at physical address `0x7c00`. In `c/boot/bios/preload.asm`, it establishes segment registers and a stack, saves the firmware boot drive number, initialises serial output and checks for extended disk services. It reads the second stage into physical address `0x10000` through INT 13h and jumps there.

The preload length in the El Torito catalog uses 512-byte units. Extended CD reads use 2,048-byte blocks. `tools/mkiso.py` and `c/boot/bios/preload.asm` keep these units visible because confusing them selects the wrong payload bytes. Both BIOS stages configure serial output at 115,200 baud, matching `c/drivers/char/serial.c`.

The second stage in `c/boot/bios/loader.asm` performs these steps:

1. Tests A20 through an address alias check, trying BIOS, keyboard-controller and I/O-port enable methods as needed.
2. Collects the E820 memory map and searches for an ACPI RSDP with valid checksums.
3. Attempts to select a 1024 × 768, 32-bit VBE linear framebuffer.
4. Builds the native boot information block at physical address `0x8000`.
5. Reads the ELF64 header and program headers, loads the segments at their physical destinations and zeroes their memory tails.
6. Builds page tables and enters long mode before jumping to the ELF entry point.

Segment data passes through a 32 KiB bounce buffer at physical address `0x70000`. Firmware reads run in real mode; a protected-mode helper copies each chunk to its destination and returns for the next read. This keeps copying dependent on a controlled segment setup, avoiding reliance on BIOS preservation of the hidden segment state used by unreal mode.

For the final transition, `protected_native_entry` installs CR3, enables PAE, sets the long mode enable bit and turns on paging. A far jump reaches `native_long_entry`, which supplies the native boot magic and information pointer before jumping to the kernel.

## UEFI loader stages

`tools/mkesp.py` builds a FAT boot volume containing the loader at `\EFI\BOOT\BOOTX64.EFI` and the kernel at `\LOGIT.ELF`. Its default layout places the FAT volume across the image; `--layout gpt` wraps it in an EFI System Partition beginning at LBA 2048, with primary and backup GPT structures. The `esp` target in `tests/uefi.mk` builds the default image.

The application enters through `efi_main()` in `c/boot/efi/loader.c`. It disables the firmware watchdog, obtains the filesystem attached to its loaded-image device handle and opens the kernel on that volume. Using this handle associates the kernel with the medium that supplied the loader.

After checking the ELF headers, the loader reserves the physical span covered by the loadable segments and verifies that firmware reports the allocation as loader data. It clears the span, including BSS and alignment gaps, then reads the segments into place. `linker.ld` places the kernel at 32 MiB. Its comment retains the earlier 1 MiB placement and explains the move: that span crossed memory owned by firmware.

The loader reserves four pages immediately below the kernel for a temporary stack and page tables. The kernel’s later reservation of low memory through the kernel image covers these active tables. A further executable page below 1 GiB holds a copy of the handoff code and its GDT.

Before creating the framebuffer tag, the loader selects a GOP mode. `c/boot/efi/gop_pick_efi.c` enumerates usable linear 32-bit modes and obtains EDID from the handle associated with the chosen GOP interface. The policy in `c/boot/efi/gop_pick.c` prefers the geometry from a valid preferred progressive detailed timing. Its fallbacks are the largest offered mode within 2% of that aspect ratio, then the largest usable mode. After a failed mode change, it requests restoration of the original mode and builds the framebuffer tag from the resulting live descriptor.

The boot block also carries a copied RSDP, validated SMBIOS data with SMBIOS 3 preferred, and a display report containing firmware geometry, offered modes, EDID bytes and the selection result. Copying SMBIOS data preserves it across reclamation of firmware boot-services memory. These layouts are defined in `include/abi/logit_boot.h`.

The loader reads files listed by `\LOGIT\FW\INDEX.TXT` into boot modules. Each module tag records a name and physical range with 64-bit endpoints. It also collects available display-controller option ROM images through the EFI PCI I/O protocol; `c/boot/efi/vbios_rom.c` names each copy using its PCI location.

The GPU self-test opt-out follows the same handoff. `c/boot/efi/gpu_optout_marker.c` checks for `\LOGIT\NOGPU` and `\LOGIT\NOGPU.TXT` before firmware files consume module slots. Presence produces a `nogpu` module. An uncertain lookup or failure to carry the module produces `LOGIT_BOOT_TAG_GPU_OPTOUT`. The consumer in `c/drivers/gpu/amd/polaris/selftest/opt_out.c` treats either form as an opt-out.

The final memory map and firmware exit form one coordinated operation. The loader finishes allocations and switches diagnostics to raw serial output before obtaining the final map, because firmware console output can allocate memory and invalidate its key. On each attempt, it checks the kernel span, auxiliary pages, boot information buffer and module allocations against that map. The ownership policy is in `c/boot/efi/load_policy.h`.

If `ExitBootServices()` fails, the loader obtains a fresh map, rebuilds its memory-map tag and retries once. After success, it calls the low copy of `logit_handoff_stub`. The stub installs the temporary stack, GDT and CR3, reloads segment selectors and jumps to the kernel while retaining long mode. Keeping this code and its GDT below 1 GiB supports firmware placement of the UEFI application above the initial kernel mapping.

## Shared kernel entry

Both loaders satisfy the contract in `include/abi/logit_boot.h`.

| Item | Entry state |
|---|---|
| Execution mode | x86-64 long mode with four-level paging |
| Interrupt flag | Clear |
| `RAX` | `LOGIT_BOOT_MAGIC` |
| `RDI` | Physical address of `struct logit_boot_header` |
| Initial mapping | First 1 GiB identity-mapped through writable, executable 2 MiB pages |
| Stack | Aligned temporary stack in low memory |

`linker.ld` selects `logit_native_start` as the ELF entry. In `c/boot/long.asm`, this entry switches to a 32 KiB kernel stack, configures x87 and SSE state, clears OSXSAVE, and enables NX and SMEP when supported by the CPU.

Assembly calls `logit_boot_normalize()` in `c/kernel/init/bootinfo.c`. It checks the entry magic, header magic, version, block extent, declared mapping extent and tag framing. It also reads the final byte of the promised identity mapping.

The normaliser rewrites the memory-map, framebuffer, ACPI and end-tag type numbers in place, returning an internal view used by existing consumers. The payload layouts are shared on both sides, which explains retained names such as `mb_info`. Extension tags for SMBIOS, display information, modules and GPU opt-out keep their native type numbers.

Next, `cpu_early_init()` in `c/crypto/cpu_report.c` initialises CPU feature information, selects cryptographic backends and prints CPU reports. Assembly then calls `kernel_main()`.

## Early kernel initialisation

`kernel_main()` in `c/kernel/init/kmain.c` establishes the BSP services in the following order.

| Phase | Operations |
|---|---|
| Serial and early checks | Initialises serial output and boot marks, prints the C-entry message and runs the inflate self-check. |
| Interrupt foundations | Calls `idt_init()`, `gdt_init()`, `pic_remap()` and `pit_init(TIMER_HZ)`, then runs the SIMD interrupt self-check. |
| Memory and platform data | Calls `bootmod_init()` before `pmm_init()`, establishes LAPIC virtual-wire delivery, supplies boot information to ACPI and captures SMBIOS and display reports. |
| Clocks and cache metadata | Calls `board_early()`, initialises platform clocks, probes PIT delivery and allocates page-cache frame metadata. |
| Diagnostics and discovery | Installs diagnostic support, enumerates PCI functions and runs the PNG self-check. |
| Display | Calls `pat_init()` and `mce_init()`, then establishes the framebuffer through `fb_init()`. |
| Processes and storage | Initialises process and open-file tables, starts NVMe and VirtIO block support, then calls `blk_init()` to discover media and select the root device. |
| Filesystems and settings | Registers and mounts LogitFS, mounts `/proc` after root-mount success, installs the VFS page-cache backend, attempts developer SSH provisioning in hardware builds and loads settings. |
| Network and content | Prints the trust-store banner, initialises networking, loads fonts and registers image codecs. |
| First desktop frame | Calls `wm_init()` and `wm_render()`, stops the boot console, runs framebuffer diagnostics and initialises the mouse. |
| Multiprocessor and drivers | Calls `smp_init()`, probes registered drivers, attaches AHCI interrupts, probes AMD boot-display resources after root-mount success, and prints hardware reports. |
| Scheduler handoff | With the root mounted, prints `LOGIT_BOOT_OK` and calls `wm_run()`. |

The memory ordering protects data supplied by the loader. `bootmod_init()` validates module tags and calls `pmm_hold_range()` before allocator initialisation. UEFI module allocations appear as available RAM in the translated firmware map, so these holds protect their contents during metadata placement and memory release. The implementation is in `c/kernel/init/bootmod.c` and `c/kernel/mm/phys/pmm.c`.

`pmm_init()` uses the firmware map to build supervisor mappings and releases higher RAM after those mappings succeed. The loader’s first 1 GiB mapping supplies access for this initial work.

Clock setup spans two phases. `pit_init()` calls `time_init()` before heap or filesystem services exist. After physical memory setup, `time_platform_init()` discovers HPET and can retry TSC calibration against it. HPET setup disarms firmware comparator interrupts and legacy replacement routing before enabling the counter. The PIT delivery probe then runs with both a time bound and an iteration bound. See `c/drivers/timer/pit.c`, `c/kernel/core/ktime.c` and `c/drivers/timer/hpet.c`.

Other dependencies shape the sequence. `board_early()` attempts to quiesce the platform watchdog before storage probing. Settings precede networking and window-manager setup because both consume configuration. PCI enumeration supplies the device registry used by early drivers; the later binding pass follows APIC setup.

`fb_init()` prefers VirtIO GPU and falls back to a supplied 32-bit RGB framebuffer. PAT setup precedes framebuffer mapping so the fallback can select write combining. In `c/kernel/gui/wm.c`, `wm_init()` allocates desktop buffers, applies settings, prepares input-method state and cursors, scans applications and draws the background. `wm_render()` submits the initial frame before AP startup.

Storage also has an early and a late phase. Root mounting uses the early storage path; after SMP and driver binding, `ahci_irq_attach()` attempts MSI completion delivery for the controllers discovered earlier. This ordering is implemented in `c/drivers/block/ahci.c`.

## Application processor startup

`smp_init()` in `c/kernel/cpu/smp/smp.c` discovers CPUs through ACPI and initialises the BSP’s local APIC. It calibrates the local timer, initialises BSP power and idle support, and records topology. It then attempts to route legacy device interrupts through the I/O APIC; successful routing masks the legacy PIC.

For each eligible AP, the BSP allocates a 64 KiB stack and fills a mailbox with the kernel CR3, stack top, C entry address and claimed CPU slot. It copies the trampoline to physical address `0x8000`. `lapic_start_ap()` in `c/kernel/cpu/irq/lapic.c` sends INIT followed by two startup IPIs using vector 8.

The trampoline in `c/boot/ap_trampoline.asm` takes the AP through real mode, protected mode and long mode. It loads the shared kernel page-table root, configures floating-point and protection state, switches to the assigned stack and calls `ap_entry()`.

The C entry initialises the local APIC, loads the shared IDT, builds the AP’s GDT and TSS, configures PAT and machine-check handling, initialises power and idle support, records topology and arms a periodic timer. It publishes completion through its claimed slot. The BSP admits that slot to the online CPU count after observing the publication. `c/kernel/cpu/smp/smp_boot_model.h` defines this handshake.

Earlier documentation described AP preemption as using an uncalibrated timer count. The current BSP calibration measures the divided LAPIC timer against a calibrated TSC, rejects disturbed register-access endpoints and selects a median of accepted samples. AP startup requests a period of 1.6 ms plus 0.2 ms per CPU slot index. The stagger reduces simultaneous contention for the scheduler lock.

Each admitted AP waits for `smp_mark_sched_ready()`. After readiness is published, it creates an idle context and joins scheduling. This barrier permits CPU setup before the global run queue exists.

A startup timeout is resolved against a possible late AP through the slot state machine. When cancellation wins, the BSP stops mailbox reuse and keeps the potentially live stack and its slot assignment available to the late CPU that may still consume them.

## Scheduler and first user processes

`wm_run()` calls `sched_init()` in `c/kernel/sched/sched.c`. The scheduler creates a thread record named `wm` for the existing boot context, establishes the run queue, creates the BSP idle thread and starts deferred-work and diagnostic threads.

The window manager publishes scheduler readiness, starts compositor workers and enables interrupts. Its first user-process creation request is a GUI launch selected from the account-store state.

| Account-store state | Initial GUI launch |
|---|---|
| `/etc/passwd` contains data | `/sbin/greeter.aex` |
| `/etc/passwd` is absent or empty | `files.aex`, the file manager |

With `HWHUD=1`, `devssh_desktop_locked()` supplies the decision. A store consisting of locked `dev` account rows permits the file manager to start; other account rows select the greeter. The policy is in `c/kernel/init/devssh.c`.

That build can also receive `\LOGIT\DEVSSH.PUB` from the boot volume as the `devssh-authorized-keys` module. `tests/uefi.mk` passes the loader’s build setting. After root mounting, `devssh_provision_boot()` validates the supplied public keys and provisions the account, authorised keys and SSH enable file before console login starts.

The GUI launch request precedes `proc_spawn("/bin/login", ...)` in `c/kernel/gui/wm.c`. A successful GUI launch creates the first user process. Console login is the next creation request.

GUI launching creates an address space, loads an AEX image, builds its stack, creates a process record and publishes a user thread. Console spawning uses the corresponding path in `c/kernel/exec/load/exec.c`. Both paths attach file descriptors 0, 1 and 2 to the serial TTY. `Makefile` packages the programs into the filesystem image.

At dispatch, the scheduler installs the incoming thread’s address space and kernel entry stack. A fresh user thread first reaches `ring3_bootstrap` in `c/boot/enter_user.asm`, which releases the scheduler lock and calls `enter_user`. That routine constructs an interrupt-return frame and executes `iretq` to enter ring 3 with interrupts enabled.

After requesting console login and releasing the window-manager lock, `wm_run()` calls `kernel_power_start()`. Its `acpi-init` thread collects board information, initialises ACPI platform services and, after successful AML setup, completes firmware PCI interrupt routing. `c/kernel/init/power.c` places this work in a thread because AML initialisation can sleep and dispatch deferred work.

## Login and the continuing desktop

The boot invocation of console login calls `boot_services()` before authentication. In `c/apps/coreutils/boot_services.h`, regular enable files owned by root and protected against group or other writes control startup of the SSH, HTTP and HTTPS daemons. Each child receives a log file and an input stream that returns EOF, preserving console input for login.

When the account file is absent, `c/apps/coreutils/login.c` executes `/bin/sh` as root. With a readable account file, it prompts for credentials. Successful authentication establishes the session, changes to the account’s home directory and executes its configured shell.

The graphical greeter establishes the same session through `c/apps/gui/greeter/greeter.c`. The window manager observes session credentials. After seeing a nonzero session UID and waiting for the unlock delay, it applies session settings, redraws the background and starts the file manager once.

The continuing `wm_run()` loop handles input, observes session changes, reaps processes, supervises the agent service, polls networking and renders damaged regions. The window manager and compositor workers execute as kernel threads; applications execute as user processes.

## Retained GRUB path

Earlier documentation described GRUB and Multiboot2 as the product boot path. The current default ISO recipe in `Makefile` packages the native BIOS stages through `tools/mkiso.py`.

The retained `iso-grub` target links a comparison kernel with the entry code and Multiboot2 header from `tests/fixtures/bootoracle/`. `grub.cfg` loads that kernel through `multiboot2`. `tests/bootself.mk` uses this route to compare boot information supplied by GRUB with the native loader output.

## Boot progress markers

| Serial output | Point reached |
|---|---|
| `LOGIT_BIOS_PRELOAD_OK` | BIOS preload established serial output. |
| `[efi] ebs ok` | UEFI firmware exit succeeded. |
| `LOGIT_BOOT_NATIVE_OK version=0001` | Native boot-information validation and normalisation completed. |
| `[logitos] long mode, C kernel running` | Execution entered `kernel_main()`. |
| `[logitos] desktop up; mouse + keyboard armed` | The initial rendering call and mouse initialisation returned. |
| `LOGIT_BOOT_OK` | Late BSP initialisation reached the marker with the root filesystem mounted. |

These messages originate in `c/boot/bios/preload.asm`, `c/boot/efi/loader.c`, `c/kernel/init/bootinfo.c` and `c/kernel/init/kmain.c`.

The UEFI loader and `c/kernel/diag/bootmark.c` also draw boot-stage squares on the framebuffer. The kernel removes the temporary early mapping before physical memory initialisation and resumes marks after framebuffer setup.

On root-mount failure, `kernel_main()` collects the board report and enters `hwscreen_report()`. The report continuously cycles through summary and full-log pages using the framebuffer and monotonic clock. It runs on the boot context, before `wm_run()` and scheduler setup.

## Limitations

- UEFI loading requires firmware to grant the fixed kernel span and auxiliary allocations. Refusal stops the loader. See `c/boot/efi/loader.c` and `linker.ld`.
- Failure of `fb_init()` prints `LOGIT_FB_FAIL` and halts. Desktop-buffer allocation failure in `wm_init()` can produce a blank screen while execution continues. See `c/kernel/init/kmain.c` and `c/kernel/gui/wm.c`.
- AVX stays disabled because interrupt entry uses FXSAVE/FXRSTOR, which preserves x87 and XMM state and omits upper YMM state. See `c/boot/long.asm` and `c/boot/isr.asm`.
- AP preemption falls back to fixed timer counts when LAPIC calibration is unavailable or fails. See `c/kernel/cpu/irq/lapic.c` and `c/kernel/cpu/smp/smp.c`.
- An empty account file selects the file manager under the default GUI policy, while console login attempts authentication against that file. See `c/kernel/gui/wm.c` and `c/apps/coreutils/login.c`.
- `LOGIT_BOOT_OK` precedes scheduler setup and application creation. Desktop scanout, input interaction and login require observation of the later stages.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Product BIOS image recipes, GRUB comparison image and application packaging. |
| `tools/mkiso.py` | ISO layout, El Torito catalog and loader payload patches. |
| `c/boot/bios/preload.asm` | Firmware entry, preserving the boot drive and loading the second stage. |
| `c/boot/bios/loader.asm` | BIOS discovery, ELF loading, boot-information construction and long-mode transition. |
| `c/drivers/char/serial.c` | Kernel serial configuration and output. |
| `tools/mkesp.py` | FAT boot-volume construction and optional GPT layout. |
| `tests/uefi.mk` | ESP targets and UEFI loader build settings. |
| `c/boot/efi/loader.c` | UEFI loading, firmware-data collection, firmware exit and handoff stub. |
| `c/boot/efi/load_policy.h` | Allocation ownership and memory-map coverage checks. |
| `c/boot/efi/gop_pick.c` | GOP geometry selection policy. |
| `c/boot/efi/gop_pick_efi.c` | GOP enumeration, associated EDID lookup and mode changes. |
| `c/boot/efi/vbios_rom.c` | Collection of display-controller ROMs into boot modules. |
| `c/boot/efi/gpu_optout_marker.c` | GPU self-test marker lookup and module construction. |
| `c/drivers/gpu/amd/polaris/selftest/opt_out.c` | Consumption of the GPU opt-out module and tag. |
| `include/abi/logit_boot.h` | Native entry contract and boot-information layouts. |
| `linker.ld` | Kernel entry, placement and section layout. |
| `c/boot/long.asm` | Kernel stack, CPU execution state and entry into C. |
| `c/kernel/init/bootinfo.c` | Native block validation and tag normalisation. |
| `c/kernel/init/bootmod.c` | Module validation, registration and memory reservations. |
| `c/crypto/cpu_report.c` | Early CPU feature setup, cryptographic backend selection and reporting. |
| `c/kernel/init/kmain.c` | BSP initialisation order and boot outcome selection. |
| `c/kernel/mm/phys/pmm.c` | Firmware-map consumption, reservations and physical memory mappings. |
| `c/drivers/timer/pit.c` | PIT programming and initial clock setup. |
| `c/kernel/core/ktime.c` | Platform clock discovery and calibration. |
| `c/drivers/timer/hpet.c` | HPET setup and firmware interrupt-route cleanup. |
| `c/drivers/board/board.c` | Early watchdog handling and board reports. |
| `c/kernel/gui/fb/fb.c` | Display selection, framebuffer mapping and presentation diagnostics. |
| `c/drivers/block/ahci.c` | Early storage operation and late MSI attachment. |
| `c/kernel/cpu/smp/smp.c` | AP startup, online admission and the barrier on scheduler readiness. |
| `c/kernel/cpu/smp/smp_boot_model.h` | AP publication, admission and cancellation interfaces. |
| `c/kernel/cpu/irq/lapic.c` | Local APIC setup, startup IPIs and timer calibration. |
| `c/boot/ap_trampoline.asm` | AP transition into long mode and C. |
| `c/kernel/sched/sched.c` | Adoption of the boot context, thread creation and dispatch. |
| `c/kernel/gui/wm.c` | First frame, initial applications, session unlock and desktop loop. |
| `c/kernel/init/devssh.c` | Developer SSH provisioning and hardware-build desktop policy. |
| `c/kernel/init/power.c` | Deferred board and ACPI initialisation. |
| `c/kernel/exec/load/exec.c` | Console-process loading, stack creation and TTY attachment. |
| `c/boot/enter_user.asm` | First user-thread entry and transition into ring 3. |
| `c/boot/isr.asm` | Interrupt entry and floating-point state preservation. |
| `c/apps/coreutils/login.c` | Console authentication and shell execution. |
| `c/apps/coreutils/boot_services.h` | Startup of enabled services during boot login. |
| `c/apps/gui/greeter/greeter.c` | Graphical authentication and session establishment. |
| `c/kernel/diag/bootmark.c` | Framebuffer progress marks and boot console. |
| `c/kernel/diag/hwscreen.c` | Continuing boot-log display after root-mount failure. |
| `grub.cfg` | GRUB menu entry for the comparison kernel. |
| `tests/bootself.mk` | Native-loader controls and GRUB comparison recipes. |

## See also

- [BIOS boot path](../kernel/boot-bios.md)
- [UEFI boot path](../kernel/boot-uefi.md)
- [Kernel initialisation](../kernel/init.md)
- [SMP boot](../kernel/smp-boot.md)
- [Processes and threads](processes-and-threads.md)
- [Graphics and windowing](graphics-and-windowing.md)
- [Running LogitOS in QEMU](../getting-started/running-in-qemu.md)
