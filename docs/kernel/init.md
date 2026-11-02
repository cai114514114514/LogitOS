# Kernel initialisation

Kernel startup converts the loader’s boot information into memory, interrupt, storage and desktop services, then brings the boot CPU into the scheduler. `kernel_main()` in `c/kernel/init/kmain.c` coordinates the main sequence. `c/boot/long.asm` prepares entry into C, `c/kernel/init/bootinfo.c` normalises boot data, and `wm_run()` continues startup after the first desktop render. Deferred power discovery, shutdown and reboot are coordinated by `c/kernel/init/power.c`.

## Entry into C

The native boot contract in `include/abi/logit_boot.h` specifies entry in x86-64 long mode with interrupts disabled. RAX carries `LOGIT_BOOT_MAGIC`, and RDI carries the physical address of a `struct logit_boot_header`. The loader identity maps the first 1 GiB, providing the address space used during early entry.

`logit_native_start` in `c/boot/long.asm` switches to the kernel’s 32 KiB stack before calling C. The common entry code configures x87 and SSE state and enables NX and SMEP when CPUID reports support. NX must be enabled before page tables begin setting the NX bit.

Entry calls `logit_boot_normalize()`, then `cpu_early_init()`, then `kernel_main()`. Early CPU setup discovers processor features, selects cryptographic backends and reports the platform through `c/crypto/cpu_report.c`.

The `mb_info` argument to `kernel_main()` points to the normalised block of boot information. Its name reflects the legacy layout retained by the kernel’s consumers of boot data.

## Boot information parsing

`logit_boot_normalize()` in `c/kernel/init/bootinfo.c` checks the entry magic, block magic, protocol version, header size, reserved field, block extent and declared identity mapping size. It also reads the final byte of the promised mapping range before walking the tags.

Each tag must fit inside the block. The walker advances by the tag size rounded up to eight bytes, and the end tag must finish the block. Extension tags retain their type numbers when they belong to the native namespace.

The adapter rewrites these tag types in place:

| Boot data | Internal type | Consumer |
|---|---:|---|
| Physical memory map | 6 | `pmm_init()` in `c/kernel/mm/phys/pmm.c` |
| Framebuffer description | 8 | `fb_init()` in `c/kernel/gui/fb/fb.c` |
| ACPI 1.0 RSDP | 14 | `c/kernel/cpu/acpi/acpi.c` |
| ACPI 2.0+ RSDP | 15 | `c/kernel/cpu/acpi/acpi.c` |
| End of tags | 0 | Consumer tag walkers |

It constructs an internal header of eight bytes at native offset 16 and returns that header’s address. The payloads stay at their original addresses. This lets BIOS and UEFI startup share the same consumer parsers.

Additional native tags carry firmware reports and loader files:

| Tag | Contents | Consumer |
|---|---|---|
| `LOGIT_BOOT_TAG_SMBIOS` | Copied SMBIOS entry point and structure table | `smbios_capture_boot_info()` in `c/drivers/board/smbios.c` |
| `LOGIT_BOOT_TAG_DISPLAY` | Firmware display modes and EDID report | `hwhud_capture_boot_info()` in `c/kernel/diag/hwhud.c` |
| `LOGIT_BOOT_TAG_MODULE` | Name and physical range of a loader-supplied file | `bootmod_init()` in `c/kernel/init/bootmod.c` |
| `LOGIT_BOOT_TAG_GPU_OPTOUT` | Instruction to treat the GPU self-test as disabled | `bootmod_init()` |

Consumers perform further payload checks. The PMM checks entry sizes in the memory map, reserves the internal range of boot information and builds the direct map before releasing high physical frames. ACPI validates the tagged RSDP and prefers it to the scan of the BIOS area. Framebuffer setup tries VirtIO GPU first, then reads the boot framebuffer description.

`bootmod_init()` checks module names and ranges, rejects duplicate names and overlapping byte ranges, and reserves accepted modules through `pmm_hold_range()`. This runs before `pmm_init()` because the UEFI loader’s module allocations appear as available RAM in the memory map. `bootmod_find()` later supplies a pointer after checking that the range is RAM and its direct mapping is ready. The contract is documented in `c/kernel/init/bootmod.h`.

## Main initialisation order

The table groups the principal calls in execution order from `c/kernel/init/kmain.c`. Progress markers and diagnostic output occur between these calls.

| Stage | Principal calls | Purpose |
|---|---|---|
| Early reporting | `serial_init()`, `bootmark_init()`, `rust_inflate_selftest()` | Establish serial output and framebuffer progress reporting, then check DEFLATE. |
| Interrupt foundations | `idt_init()`, `gdt_init()`, `pic_remap()`, `pit_init(TIMER_HZ)`, `cpu_simd_selftest()` | Install descriptor tables, configure the PIT and check SIMD state across interrupts. |
| Boot modules and memory | `bootmark_early_end()`, `bootmod_init()`, `pmm_init()`, `lapic_virtual_wire()` | Retire the temporary display mapping, reserve loader files, initialise physical memory and configure PIC delivery through the local APIC. |
| Firmware and clocks | `acpi_set_mb2_info()`, `smbios_capture_boot_info()`, `hwhud_capture_boot_info()`, `board_early()`, `time_platform_init()` | Publish firmware information, handle the watchdog and discover platform clocks. |
| Cache and diagnostics | `pcache_init()`, `kdiag_init()` | Allocate page cache metadata and install diagnostic interrupt support. |
| PCI and display | `pci_init()`, `rust_png_selftest()`, `pat_init()`, `mce_init()`, `fb_init()` | Enumerate PCI, check PNG decoding, configure supported CPU facilities and initialise the framebuffer. |
| Process bookkeeping | `proc_init()`, `file_init()` | Initialise the process table and open-file-description pool. |
| Root storage | `nvme_init()`, `virtio_blk_init()`, `blk_init()`, `vfs_register()`, `vfs_mount()` | Discover storage, select the root device and attempt the LogitFS mount. |
| Filesystem consumers | `procfs_mount()`, `pcache_vfs_install()`, `devssh_provision_boot()`, `settings_init()` | Attach filesystem services and load configuration. Procfs, developer provisioning and settings depend on a successful root mount. |
| Network and media | `trust_banner()`, `net_init()`, `text_init()`, `img_init()` | Report compiled trust roots, configure networking, load fonts and register image codecs. |
| Initial desktop | `wm_init()`, `wm_render()`, `bootcon_stop()`, `fb_present_bench()`, `mouse_init()` | Construct and submit the first desktop frame, stop the boot console, measure framebuffer work and initialise the mouse. |
| Processors and drivers | `smp_init()`, `dev_probe_all()`, `ahci_irq_attach()`, conditional `amd_bootfb_probe_resources()` | Bring up processors and interrupt routing, bind drivers and perform later storage and display setup. |
| Runtime handoff | Device and hardware reports, `LOGIT_BOOT_OK`, `wm_run()` | Report the completed main sequence and enter scheduler startup when the root mount succeeded. |

`TIMER_HZ` is 100 in `c/drivers/timer/pit.h`. `pit_init()` calls `time_init()`, establishing the early clock before discovery of firmware tables. After memory setup, `time_platform_init()` discovers HPET. It can retry TSC calibration against HPET or promote a clock using PIT to HPET; see `c/kernel/core/ktime.c`.

Boot narration uses `kprintf()`, whose console sink writes to serial, VGA, the framebuffer boot console and the kernel log ring. `c/kernel/diag/klog.h` defines a ring of 2,048 records and a pinned copy of the first 2,048 records. These provide retained startup output after the serial messages have passed.

`c/kernel/diag/bootmark.c` draws progress squares through an early framebuffer mapping. `bootmark_early_end()` removes that temporary mapping before PMM setup. After framebuffer initialisation, `bootmark_resume()` can resume drawing through the established boot framebuffer mapping. `bootcon_stop()` ends console drawing after the first desktop render.

## Dependencies behind the order

Memory setup precedes local APIC mapping. `lapic_virtual_wire()` configures LINT0 for PIC delivery through the local APIC. Platform clock discovery then disarms inherited HPET legacy routing, which can divert IRQ0 from the PIT. The PIT diagnostic follows these steps, enabling interrupts while sampling ticks over a nominal 50 ms interval with an additional iteration bound. The implementations are in `c/kernel/cpu/irq/lapic.c`, `c/drivers/timer/hpet.c` and `c/kernel/init/kmain.c`.

`board_early()` handles the TCO watchdog before storage probing and desktop construction. Firmware can leave that watchdog running, causing a reset during startup. Broader board inventory runs later through `board_init()` in `c/drivers/board/board.c`.

Page cache setup has two stages. `pcache_init()` allocates frame ownership and lookup tables soon after physical memory becomes available. Reclaim consults cache ownership, so the bookkeeping must exist before workloads create memory pressure. `pcache_vfs_install()` attaches the file backend after the root mount attempt. These stages are implemented in `c/kernel/mm/cache/pcache.c` and `c/kernel/mm/cache/pcache_vfs.c`.

After mounting the root, startup creates `/proc` and calls `procfs_mount()`. It loads settings before network and desktop setup because network configuration and appearance preferences use stored values. The relevant consumers are in `c/net/core/netcfg.c` and `c/kernel/gui/wm.c`.

For diagnostic builds, `HWHUD=1` enables `LOGIT_HWHUD` in `Makefile`. `devssh_provision_boot()` can consume public keys supplied by the UEFI loader from `\LOGIT\DEVSSH.PUB`. It provisions the `dev` account and authorised keys before creating the SSH enable flag. This precedes `/bin/login`, whose service startup reads the flag. The provisioning code and policy are in `c/kernel/init/devssh.c` and `c/kernel/init/devssh.h`.

PCI enumeration precedes storage, display and network initialisation so their discovery calls can use the device registry. Driver binding runs later through `dev_probe_all()`, after `smp_init()` has attempted APIC setup. AHCI discovery occurs inside `blk_init()`; `ahci_irq_attach()` subsequently attempts MSI delivery and checks queued operation. See `c/kernel/pci/pci_init.c`, `c/drivers/core/device.c`, `c/drivers/block/blkdev.c` and `c/drivers/block/ahci.c`.

`pat_init()` prepares write combining before the boot framebuffer receives its kernel mapping. Resource discovery for the AMD boot display follows the root mount and driver binding because it loads filesystem resources for the bound display device. These dependencies are implemented in `c/kernel/cpu/power/pat.c`, `c/kernel/gui/fb/fb.c` and `c/drivers/gpu/amd/bootfb.c`.

## Entering the scheduler

`wm_run()` in `c/kernel/gui/wm.c` calls `sched_init()` on the boot CPU’s existing execution context. The scheduler represents that context as the `wm` thread, creates an idle thread for the boot CPU, and starts deferred work, the wait/wake self test, benchmarking support and scheduler observation setup. These calls are in `c/kernel/sched/sched.c`.

Application processors brought online by `smp_init()` wait for scheduler readiness. After `sched_init()`, the window manager calls `smp_mark_sched_ready()` and starts parallel rendering workers. Released processors register their current stacks as idle threads and enter scheduling through `c/kernel/cpu/smp/smp.c`.

The window manager enables interrupts and chooses its initial login state. The default build checks whether `/etc/passwd` has content: accounts select the greeter, while an empty store selects desktop startup. In diagnostic builds, a store containing just the provisioned `dev` row also permits desktop startup.

It then launches `/bin/login` on the serial console, releases the window manager state lock and calls `kernel_power_start()` before entering its runtime loop.

`LOGIT_BOOT_OK` is printed immediately before `wm_run()` on the successful root mount path. Scheduler initialisation and deferred AML discovery occur after this marker.

## Deferred power discovery

`kernel_power_start()` creates the `acpi-init` thread in `c/kernel/init/power.c`. AML evaluation can sleep and dispatch deferred work. Starting discovery after scheduler setup supplies those facilities, and placing it outside the window manager lock allows input processing to continue during firmware evaluation.

The thread first calls `board_init()`, then `kernel_power_init()`. Power initialisation begins with `power_acpi_native_start()`, which checks scheduler and clock availability, obtains an RSDP copy and queues a startup probe on the shared worker. It waits for that probe within a timeout before reporting native service readiness. This handshake is implemented in `c/drivers/power/acpi/native.c`.

`power_acpi_start()` then initialises uACPI, loads the namespace, sets up support for the embedded controller and initialises namespace devices. Queries gather battery status, AC adapter status and thermal zone status, which `kernel_power_init()` logs. These operations are in `c/drivers/power/acpi/devices.c`.

Before namespace startup, the power path registers `pci_prt_aml_preinit()`. When SMP setup has established I/O APIC routing, this callback selects the firmware’s I/O APIC interrupt model after namespace loading and before device initialisation. See `c/kernel/pci/pci_prt_aml.c`.

After successful AML startup, the thread checks ACPI ownership of the NCT6779 hardware monitor ports and installs power button handling. It releases the AML mutex before calling `pci_prt_aml_ready()`, which installs the AML PCI routing resolver and refreshes existing INTx routes. Route changes can wait for active interrupt handlers to retire, so this ordering releases the AML owner before that wait.

## Shutdown and reboot

User processes request shutdown through `SYS_POWEROFF` and `SYS_REBOOT`. The dispatcher in `c/kernel/exec/syscall.c` checks for UID zero before calling `kernel_poweroff()` or `kernel_reboot()`. Desktop confirmation actions call the same functions from `c/kernel/gui/wm_overlay.c`.

ACPI power button events also reach `kernel_poweroff()`. Event handling queues work, and that work creates a shutdown thread. This gives filesystem draining a context that can sleep while the shared worker continues completing deferred I/O. The path is implemented in `c/drivers/power/acpi/button.c`.

Both operations begin with shared preparation in `c/kernel/init/power.c`:

1. Acquire the mutex for power requests.
2. Call `vfs_drain_begin()` to close filesystem admission and wait for admitted activity to finish.
3. Print `[power] syncing...` and call `logitfs_sync()`.
4. Continue to the hardware operation after synchronisation succeeds.

The drain uses a `struct vfs_drain` token. Returning error paths release that token through `vfs_drain_end()`, restoring filesystem admission. Already admitted work can finish while the drain waits; see `c/fs/vfs/vfs.c` and `c/fs/vfs/vfs.h`.

`logitfs_sync()` calls `bcache_sync()`. The cache submits dirty buffers in batches of up to 16 spans, marks successful writes clean and calls `blk_flush()` for the final storage barrier. Errors propagate to the power preparation path. The implementation is in `c/fs/logitfs/logitfs.c` and `c/fs/cache/bcache.c`.

Poweroff initialises AML services as needed, prepares ACPI S5 and requests entry into that state. It flushes queued serial output before entry. If execution returns, it attempts to restore handling of the ACPI working state, restores filesystem admission and returns an error. Devices retain their bindings during this path so a returning S5 attempt preserves the services needed to continue running.

Reboot calls `dev_quiesce_all()` after filesystem synchronisation. The device model retains storage bindings and counts enabled bus mastering on those controllers as unconfirmed DMA. Display drivers can supply a quiesce callback while retaining their bindings. Other eligible devices pass through removal and checks of bus mastering. These rules are implemented in `c/drivers/core/device.c`.

The reset sequence depends on the quiescence result:

| Quiescence result | Reset attempts, in order |
|---|---|
| No unconfirmed DMA reported | ACPI reset, `board_reset()` for PCH CF9 reset, 8042 reset pulse, then triple fault after a 50 ms wait |
| Unconfirmed DMA reported | `board_reset()`, ACPI reset, then another `board_reset()` attempt |

The triple fault path saves the previous IDT and loads an IDT with a zero limit before issuing `int3`. Its recovery code restores the previous IDT, calls `dev_probe_all()` and reopens filesystem admission if control returns. Reset sequencing is in `c/kernel/init/power.c`; PCH reset handling is in `c/drivers/board/board.c`.

Older comments in `c/kernel/init/power.c` and `c/kernel/init/power.h` describe guessed S5 values and permanent halts on failure. Their adjacent corrections and current functions use firmware AML for S5 and restore filesystem admission on returning failures.

## Limitations

- Rejected native boot information produces a serial diagnostic, flushes queued output and halts entry.
- Framebuffer initialisation failure prints `LOGIT_FB_FAIL` and halts startup. The fallback to the boot framebuffer requires a 32-bit RGB description.
- Root mount failure continues through later diagnostics, then calls `board_init()` and enters the paging loop for the boot log in `hwscreen_report()`. This path stops before scheduler startup and deferred AML discovery.
- AML initialisation caches its first result, including failure; subsequent queries and requests reuse that result.
- Shutdown drains filesystem activity and synchronises LogitFS without a process notification or application teardown pass.
- If DMA remains unconfirmed and the platform reset attempts return, reboot prints a refusal and halts. It skips the 8042 and triple fault fallbacks because those could leave devices writing into memory reused by the next boot.
- The desktop milestone records frame submission. Scanout and input interaction require observation on the target machine.

## See also

- [Boot sequence](../architecture/boot-sequence.md)
- [ACPI](acpi.md)
- [Physical memory](physical-memory.md)
- [Scheduling](scheduling.md)
- [SMP boot](smp-boot.md)
- [Power and platform drivers](../drivers/power-and-platform.md)
- [Kernel diagnostics](diagnostics.md)

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_boot.h` | Native entry contract, header and tag layouts. |
| `c/boot/long.asm`, `c/crypto/cpu_report.c` | Entry stack, CPU setup, feature discovery and SIMD checks. |
| `c/kernel/init/bootinfo.c` | Boot block validation and conversion to the internal view. |
| `c/kernel/init/bootmod.c`, `c/kernel/init/bootmod.h` | Module validation, physical reservations and lookup. |
| `c/kernel/init/kmain.c` | Main startup sequence, branches that depend on the mount, and boot markers. |
| `c/kernel/mm/phys/pmm.c` | Consumption of the memory map, reservations and construction of the direct map. |
| `c/kernel/cpu/acpi/acpi.c` | Boot RSDP validation and firmware table discovery. |
| `c/drivers/board/smbios.c`, `c/kernel/diag/hwhud.c` | Firmware inventory and capture of display reports. |
| `c/drivers/board/board.c`, `c/drivers/board/nct6779.c` | Watchdog handling, board inventory, reset and monitor access policy. |
| `c/drivers/timer/pit.h`, `c/drivers/timer/pit.c`, `c/kernel/core/ktime.c` | PIT rate, early clock setup and platform clock selection. |
| `c/drivers/timer/hpet.c`, `c/kernel/cpu/irq/lapic.c` | HPET routing cleanup and local APIC setup. |
| `c/kernel/mm/cache/pcache.c`, `c/kernel/mm/cache/pcache_vfs.c` | Page cache metadata and VFS backend installation. |
| `c/kernel/diag/kprintf.c`, `c/kernel/diag/kdiag.c` | Console output and diagnostic setup. |
| `c/kernel/diag/klog.c`, `c/kernel/diag/klog.h` | Log ring and pinned boot records. |
| `c/kernel/diag/bootmark.c`, `c/kernel/diag/hwscreen.c` | Framebuffer progress reporting, boot console and paged failure report. |
| `c/kernel/cpu/power/pat.c`, `c/kernel/cpu/power/mce.c`, `c/kernel/gui/fb/fb.c` | Cache policy, machine check setup and framebuffer initialisation. |
| `c/drivers/block/blkdev.c`, `c/drivers/block/ahci.c` | Storage discovery and later AHCI interrupt setup. |
| `c/kernel/init/devssh.c`, `c/kernel/init/devssh.h`, `Makefile` | Diagnostic build selection and developer SSH provisioning. |
| `c/apps/coreutils/login.c`, `c/apps/coreutils/boot_services.h` | Login startup and service enable flags. |
| `c/kernel/core/settings.c`, `c/net/core/netcfg.c` | Stored settings and network configuration policy. |
| `c/kernel/pci/pci_init.c`, `c/drivers/core/device.c` | PCI enumeration, driver binding and device quiescence. |
| `c/drivers/gpu/amd/bootfb.c` | Resource discovery for the
