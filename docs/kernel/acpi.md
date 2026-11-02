# ACPI

LogitOS uses ACPI tables to discover processor topology, interrupt routes, PCI configuration windows, timers, and firmware images. The early table reader supports hardware initialization. Later, the power service starts the vendored uACPI interpreter to execute ACPI Machine Language (AML), query power devices, handle firmware events, and request shutdown or reset. The main implementations are `c/kernel/cpu/acpi/acpi.c` and `c/drivers/power/acpi/`.

## Table discovery

The BIOS loader searches the first KiB of the Extended BIOS Data Area, followed by `0xE0000` through `0xFFFFF`, at intervals of 16 bytes. It checks the Root System Description Pointer (RSDP) signature and checksums, then copies an accepted record into a native boot tag. This path is in `c/boot/bios/loader.asm`.

The UEFI loader searches the firmware configuration table and prefers the ACPI 2.0 entry. It checks the signature and copies the selected record into a boot tag. The kernel subsequently validates its checksums. The loader implementation is in `c/boot/efi/loader.c`.

`c/kernel/init/bootinfo.c` converts native ACPI tag types into the internal tag numbers 14 and 15 while retaining their payloads. This adapter explains the Multiboot2 terminology still present in the table reader’s interfaces.

After physical memory initialization, `c/kernel/init/kmain.c` calls `acpi_set_mb2_info()`. The first call to `acpi_tables_init()` searches the supplied tags for a valid RSDP, then falls back to scanning the BIOS region. The loader’s record takes priority because firmware with a compatibility support module can leave a stale or shadowed copy in that region.

`acpi_tables_init()` caches its result. It selects the XSDT when an RSDP of revision 2 or later supplies an XSDT address; otherwise it selects the RSDT. `acpi_find_table()` walks the selected root and returns the first validated child with the requested signature. Both functions are in `c/kernel/cpu/acpi/acpi.c`.

## Validation and mappings

The early reader checks firmware records through `c/kernel/cpu/acpi/acpi_integrity.c`.

| Record | Validation |
|---|---|
| RSDP | Signature and checksum over the first 20 bytes |
| RSDP revision 2 or later | Extended header availability, declared length, available bytes, and checksum over the complete record |
| System Description Table (SDT) | Header availability, declared length, available bytes, and checksum over the complete table |
| Root table | Expected `XSDT` or `RSDT` signature together with SDT integrity |

`c/kernel/cpu/acpi/acpi_integrity.h` defines the SDT header size as 36 bytes, the maximum SDT length as 1 MiB, and the maximum RSDP length as 4096 bytes. The mapper reads a table’s header first, bounds its length, then maps and validates the complete table. This ordering bounds the mapping request and checksum traversal before firmware lengths control either operation.

Firmware aliases begin at `PHYSMAP_BASE + PHYSMAP_SIZE`. New mappings created by the early reader have supervisor access, read access, and execution disabled when CPU protection supports it. After requesting a mapping, the reader checks that the page table entry is present, identifies the expected physical page, and has supervisor access.

The opening comment in `c/kernel/cpu/acpi/acpi.c` preserves the older assumption that tables reside in low memory. Its correction records an 8 GiB BIOS guest whose RSDT appeared at `0xbffe2430`, above the low identity window. The alias maps such firmware addresses through kernel mappings shared across process address spaces. The sharing machinery is in `c/kernel/mm/virt/vmm.c`.

`acpi_map_firmware()` exposes the same bounded mapping mechanism to other firmware readers. The board helpers in `c/drivers/board/board_io.h` use it for SMBIOS access in `c/drivers/board/smbios.c`.

The uACPI adapter in `c/drivers/power/acpi/native.c` uses the established direct map for RAM and the firmware alias for other physical ranges. It grants write access needed by firmware control structures and AML OperationRegions. Existing firmware aliases retain their cache attributes; newly created firmware or device mappings use uncached access. Aliases have kernel lifetime because several tables or OperationRegions can share their pages.

## Early table consumers

The table reader serves consumers before scheduler and interpreter startup.

| Table | Use |
|---|---|
| MADT, signature `APIC` | Processor APIC IDs, Local APIC address, I/O APIC controllers, and ISA interrupt overrides |
| MCFG | PCI Express ECAM allocation records |
| HPET | Timer register discovery |
| FADT, signature `FACP` | Accessors for legacy PM1 control fields and the reset register |
| VFCT | AMD Polaris VBIOS image discovery |

`c/kernel/cpu/smp/smp.c` calls `acpi_init()` to parse the MADT. CPU records pass through `c/kernel/cpu/irq/apic_model.c`, which handles Local APIC and Local x2APIC entries, checks enabled flags, and rejects conflicting processor identities.

The previous page’s description of the “first I/O APIC” applies to the compatibility address and GSI-base accessors. Routing consumes the controller list through `acpi_ioapic_count()` and `acpi_ioapic_get()`. `c/kernel/cpu/acpi/madt_irq.c` validates record lengths and MMIO bases and deduplicates controller addresses and GSI bases.

During initialization, `c/kernel/cpu/irq/ioapic.c` reads each controller’s redirection capacity. `c/kernel/cpu/irq/ioapic_map.c` validates its GSI interval before that controller’s redirection entries are programmed. Runtime routing selects the controller covering the requested GSI and applies the MADT’s recorded polarity and trigger overrides.

`c/kernel/pci/pci_init.c` reads MCFG entries through `acpi_mcfg_entry()` before PCI enumeration. PCI setup precedes SMP startup, which explains why root-table discovery has its own entry point.

`c/drivers/timer/hpet.c` obtains the HPET table through `acpi_find_table()`. After validating the register description and hardware capabilities, it disarms comparator interrupts and clears legacy replacement routing before enabling the main counter. This restores the PIT and RTC interrupt paths. `c/kernel/init/kmain.c` invokes platform clock initialization before its PIT tick probe.

The FADT accessors `acpi_pm1_cnt()` and `acpi_reset_reg()` remain available in `c/kernel/cpu/acpi/acpi.c`. Power transitions use uACPI.

`c/drivers/gpu/amd/polaris/resource/vbios.c` first tries a VBIOS boot module, then searches VFCT through the early table reader. It bounds image records, matches PCI location and vendor/device IDs, and checks the selected ROM’s length, signature, and ATOM header. This discovery runs before interpreter startup.

## Interpreter startup

The interpreter resides in `third_party/uacpi/`; its pinned revision and import record are in `third_party/uacpi/PROVENANCE.md`. The `Makefile` builds its event and sleep support and sets the default AML loop timeout to two seconds and maximum call stack depth to 64. The interpreter, power adapter, processor firmware reader, NCT ownership checker, and PCI routing evaluator receive the same configuration flags.

During desktop startup, `c/kernel/gui/wm.c` releases the window manager’s state lock and calls `kernel_power_start()`. That function creates the `acpi-init` thread. Thread context lets initialization sleep and wait for deferred work while the desktop handles input. Running initialization on its own thread also lets the shared worker execute the startup probe that initialization waits for.

`c/kernel/init/power.c` serializes the power consumer’s initialization, status queries, and transitions with an AML mutex. Status requests, shutdown, and reboot can initialize the interpreter on demand. Initialization errors are cached because retrying over a partially initialized namespace requires teardown and ownership handling.

The common initialization path registers `pci_prt_aml_preinit()` through `power_acpi_set_preinit()` before starting the interpreter. A status request arriving before the background thread follows this same path.

Startup proceeds as follows:

1. `power_acpi_native_start()` checks thread and clock availability, copies the validated RSDP, and waits for the shared worker to execute a queued probe.
2. `power_acpi_start()` calls `uacpi_initialize()` with `UACPI_FLAG_BAD_CSUM_FATAL` and `UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL`.
3. `uacpi_namespace_load()` loads the DSDT and supplementary AML tables and initializes firmware events.
4. The preinitialization hook calls `uacpi_set_interrupt_model()` when the kernel has committed to I/O APIC routing. This evaluates the optional `_PIC(1)` method before device initialization.
5. `power_ec_initialize()` binds supported embedded controllers.
6. `uacpi_namespace_initialize()` performs namespace device initialization, including `_STA` and `_INI` evaluation.
7. `power_ec_activate()` enables event delivery for successfully initialized embedded controllers.

The integration sequence is in `c/drivers/power/acpi/devices.c`. Interpreter initialization and table handling are in `third_party/uacpi/source/uacpi.c` and `third_party/uacpi/source/tables.c`.

The initialization thread calls `board_init()` before querying power devices. After interpreter startup succeeds, it runs `nct6779_acpi_gate()` and installs power button handlers while holding the AML mutex. The sensor gate checks the hardware monitor’s eight bytes of port space against SystemIO OperationRegions and authorizes access after a successful scan finds the range available. The check is in `c/drivers/board/nct_acpi.c` and `c/drivers/board/nct6779.c`.

The thread releases the AML mutex before calling `pci_prt_aml_ready()`. Moving PCI interrupt bindings can wait for interrupt handlers to retire, so that phase runs after releasing the power consumer’s mutex.

## Native services and firmware events

`c/drivers/power/acpi/native.c` implements the services declared by `third_party/uacpi/include/uacpi/kernel_api.h`: memory allocation and mapping, monotonic time, delays, sleeping, mutexes, semaphore events, spinlocks, interrupts, deferred work, I/O ports, and PCI configuration access.

The adapter obtains a stable RSDP copy through `acpi_copy_rsdp()`, translates the copy’s kernel address to a physical address, and publishes it through `uacpi_kernel_get_rsdp()`. This supplies the physical address required by uACPI’s interface.

The System Control Interrupt (SCI) connects firmware events to the kernel interrupt layer. The adapter checks the requested interrupt against the FADT’s SCI number, applies MADT overrides for ISA interrupt numbers, and routes the resulting Global System Interrupt (GSI) through an I/O APIC to CPU 0. Installation checks route availability. Retirement masks and drains the source before releasing its vector and callback context.

General Purpose Event (GPE) execution and notifications use a preallocated queue of 64 jobs. Interrupt handlers enqueue callbacks, and the shared `kworker` executes them in thread context. A spinlock protects queue operations; callbacks run after releasing it. Queue exhaustion returns an error. Completion barriers wait for interrupt producers and deferred callbacks to finish.

`c/drivers/power/acpi/button.c` installs a fixed power button event handler and notification handlers for `PNP0C0C` devices. It clears the latched fixed event before installing its handler. Control method buttons use notification value `0x80`. Installation also calls `uacpi_finalize_gpe_initialization()` to enable firmware GPE methods used by notifications.

Both button forms use a common path that coalesces presses while shutdown is in progress. A work item creates the `pwrbtn` thread, which calls `kernel_poweroff()`. This lets filesystem draining wait for I/O while the shared worker continues processing completions.

## PCI interrupt routing

`c/kernel/pci/pci_prt_aml.c` discovers PCI roots through `PNP0A08` and `PNP0A03`, evaluates `_SEG` and `_BBN`, and matches bridge namespace nodes to enumerated PCI topology through `_ADR`. It evaluates `_PRT` at the device’s bus and walks toward the root, applying interrupt-pin swizzling at each bridge.

A routing entry supplies a GSI or a link device and resource index. Link resolution checks `_STA`, reads `_CRS`, and uses `_PRS` and `_SRS` when allocation is needed. The resource index counts descriptors across the complete resource template. Allocation builds a complete selected configuration, checks resources used by cached links, and confirms the assigned interrupt and electrical attributes by reading `_CRS` and `_STA` again.

After interrupt-model selection and namespace initialization succeed, `pci_prt_aml_ready()` publishes the resolver and starts a pass over existing INTx bindings. `c/kernel/pci/pci_msi.c` gives evaluated AML routes priority over PCH routing and the firmware interrupt line. Migration retires the old binding, attempts the new binding, and attempts rollback after a binding failure. Diagnostics distinguish unchanged, moved, retained, failed, and lost routes.

## Power-device queries

`kernel_power_status()` returns a snapshot assembled by `power_acpi_query()`. `kernel_power_init()` also prints discovery and measurement diagnostics. The snapshot structures are in `c/drivers/power/acpi/devices.h`.

| Device | Discovery and evaluation |
|---|---|
| Battery | `PNP0C0A`; `_STA` checks battery presence, `_BST` supplies status, and `_BIF` supplies information |
| AC adapter | `ACPI0003`; `_PSR` reports external-power availability |
| Thermal zone | Namespace objects of thermal-zone type; `_TMP` supplies temperature and optional `_CRT` supplies a critical threshold |

Battery decoding validates package sizes, integer widths, state flags, and units before publishing values. Results retain firmware units and the unknown sentinel `POWER_UNKNOWN`. Each battery has `status_error` for presence/status evaluation and `info_error` for information evaluation. The information status governs use of the reported rate and capacity unit.

Thermal decoding bounds firmware values and converts tenths of kelvin to signed millidegrees Celsius. Each record has a temperature status and a `critical_valid` flag. AC adapter results are aggregated into `ac_online`: `-1` for unknown, `0` for offline, and `1` when an adapter reports online.

The snapshot stores eight batteries and sixteen thermal zones, reports truncation, and includes namespace and embedded controller errors. Its `ready` field reports interpreter initialization; battery and thermal status fields describe the corresponding measurements. The query implementation is in `c/drivers/power/acpi/devices.c`.

Processor firmware decoding lives in `c/kernel/cpu/power/firmware.c`. `cpu_power_load_acpi()` accepts a processor namespace path and decodes `_PSS`, `_PCT`, `_PPC`, and `_CST` into cached frequency and idle policies.

The older scope statement in `c/drivers/power/README.md` lists CPU state writes as future work. Current SMP startup calls `pstate_init()` and `cpuidle_init()`. On supported Intel CPUs, the frequency policy can derive its range from processor registers and program the permitted maximum or turbo ratio. These consumers are in `c/kernel/cpu/power/pstate.c` and `c/kernel/cpu/power/cpuidle.c`.

## Embedded controllers

The embedded controller driver discovers `PNP0C09` devices and evaluates their resources. A supported binding supplies two distinct SystemIO ports with fixed addresses, each one byte wide and using 16 bits of address decoding. Their order is data port followed by control port. `_GPE` supplies an integer global GPE. Resource validation and consistency checks against ECDT are in `c/drivers/power/acpi/ec/resources.c`.

Binding occurs between namespace loading and device initialization. Installing the OperationRegion handler connects controller fields and executes `_REG`, allowing `_INI` to access them. Event activation follows namespace initialization and checks for a query already latched by the controller. This ordering is implemented in `c/drivers/power/acpi/ec/driver.c`.

Transactions serialize through a mutex. After writing an input byte, the transport allows one microsecond for input-buffer status to become visible. Each handshake has a deadline and an iteration ceiling. Transaction failures quarantine the transport until reboot because an ambiguous write may already have affected the controller.

Deferred query handling releases the transaction mutex before evaluating the matching `_Qxx` method, allowing that method to access controller fields. The transport is in `c/drivers/power/acpi/ec/transport.c`; query dispatch is in `c/drivers/power/acpi/ec/driver.c`.

## Shutdown and reset

`c/kernel/init/power.c` owns filesystem ordering around power transitions. `kernel_poweroff()` drains filesystem admission, waits for admitted work, and calls `logitfs_sync()`. It then acquires the AML mutex, initializes the service if needed, prepares S5, and enters it.

The file preserves its historical description of guessed sleep types beside a correction. Current code calls `power_acpi_prepare_off()` and `power_acpi_enter_off()`. Through `third_party/uacpi/source/sleep.c`, these obtain the firmware’s `_S5` configuration, evaluate optional `_PTS`, and perform the sleep-register operations.

Before entering S5, the consumer flushes queued serial output so shutdown diagnostics reach the UART. If entry returns, it attempts to restore ACPI event handling for the working state, releases filesystem admission, and returns an error. Sync and preparation failures also release admission.

`kernel_reboot()` uses the same filesystem drain and sync path, then calls `dev_quiesce_all()` to quiesce devices and collect unconfirmed DMA stops. `c/drivers/core/device.c` checks driver removal and PCI Bus Master state, accounts for active storage DMA, and invokes display quiescence callbacks.

When DMA cessation is unconfirmed, reboot first attempts the board’s PCH reset through `board_reset()`. It then tries uACPI’s FADT reset mechanism. If execution continues, it attempts the PCH reset again.

With successful quiescence checks, execution can proceed to the 8042 reset pulse and a triple fault. With unconfirmed DMA activity, returning platform-reset attempts lead to a diagnostic halt because a CPU reset could let a device corrupt the next boot’s memory. PCH reset integration is in `c/drivers/board/board.c`.

## Limitations

- The early MADT reader stores up to 32 CPUs and eight I/O APICs and accepts Local APIC address overrides that fit in 32 bits. A rejected selected XSDT ends initialization without an RSDT retry. See `c/kernel/cpu/acpi/acpi.c`, `c/kernel/cpu/acpi/acpi.h`, and `c/kernel/cpu/acpi/madt_irq.h`.
- The native adapter supports one SCI route and PCI segment zero. Its shared worker can migrate, so GPE execution has no CPU 0 affinity guarantee. Mapping requests are bounded to 1 MiB. See `c/drivers/power/acpi/native.c`.
- AML PCI routing supports segment zero. Failed interrupt-model selection leaves AML routing inactive while power initialization continues. See `c/kernel/pci/pci_prt_aml.c` and `c/drivers/power/acpi/devices.c`.
- VFCT is subject to the early reader’s 1 MiB table bound; larger tables require the VBIOS boot module path. See `c/drivers/gpu/amd/polaris/resource/vbios.c`.
- Embedded controller support has four controller slots. Shared `_GLK` ownership, early ECDT bootstrap, GPIO interrupts, and resource forms outside the described SystemIO binding are unsupported. See `c/drivers/power/acpi/ec/resources.c`, `c/drivers/power/acpi/ec/driver.c`, and `c/drivers/power/acpi/ec/README.md`.
- Processor policy initialization precedes AML startup, and `cpu_power_load_acpi()` has no production caller. Firmware policy refresh, hotplug, and `_CST` change notifications are unsupported. See `c/kernel/cpu/smp/smp.c` and `c/kernel/cpu/power/firmware.h`.
- The NCT ownership scan covers OperationRegions available during its boot scan. SMM accesses and regions created by later method execution fall outside that check. See `c/drivers/board/nct_acpi.c`.
- The power service’s documented gaps include suspend/resume, `_BIX` queries, desktop power notifications, and validation on physical machines. See `c/drivers/power/README.md`.

## Validation coverage

The following targets encode checks for the described paths.

| Target | Coverage |
|---|---|
| `test-acpi-integrity-host` | RSDP and SDT integrity, with prerequisite checksum and signature controls |
| `test-power-aml-host` | Production power queries through uACPI, malformed firmware results, corrupted checksums, and a prerequisite temperature-conversion control |
| `test-power-ec-aml` | Modeled transactions and AML field access, with prerequisite transport-delay and initialization-order controls |
| `test-power` | QEMU shutdown, persistence on the following boot, background discovery, absence of journal replay, and reboot |
| `test-power-negctl` | Standalone abrupt-termination control that requires journal replay or a filesystem check finding |
| `test-hpet-host` | Table and capability parsing, comparator disarming, and legacy-route clearing; prerequisite controls cover address space, block ID, and revision checks |
| `test-hpet-guest` | HPET activation, clock cross-checking, restoration after PIT fallback, and monotonic reads across four QEMU CPUs; a prerequisite boot checks HPET absence |

The definitions are in `tests/acpi_integrity.mk`, `tests/drivers/power/acpi/test.mk`, `tests/drivers/power/acpi/ec/test.mk`, `tests/power.mk`, and `tests/hpet.mk`. `tests/boot/run-power-test.sh` and `tests/boot/run-hpet-test.py` implement the guest sequences. `test-power-negctl` has its own target in `tests/power.mk`; the positive power target lists the ISO and disk as prerequisites.

## Source map

| File | What it contains |
|---|---|
| `c/boot/bios/loader.asm` | BIOS RSDP discovery, validation, and copying |
| `c/boot/efi/loader.c` | UEFI configuration-table discovery and copying |
| `c/kernel/init/bootinfo.c` | Native boot-tag adaptation |
| `c/kernel/init/kmain.c` | ACPI handoff and hardware initialization order |
| `c/kernel/cpu/acpi/acpi.c`, `c/kernel/cpu/acpi/acpi.h` | Early table discovery, mappings, lookup, and accessors |
| `c/kernel/cpu/acpi/acpi_integrity.c`, `c/kernel/cpu/acpi/acpi_integrity.h` | Record validation and size bounds |
| `c/kernel/cpu/acpi/madt_irq.c`, `c/kernel/cpu/acpi/madt_irq.h` | I/O APIC record validation and storage |
| `c/kernel/mm/virt/vmm.c` | Shared kernel mappings |
| `c/kernel/cpu/irq/apic_model.c` | MADT processor validation |
| `c/kernel/cpu/irq/ioapic.c`, `c/kernel/cpu/irq/ioapic_map.c` | Controller discovery, GSI intervals, and routing |
| `c/kernel/cpu/smp/smp.c` | MADT consumption and processor policy startup |
| `c/kernel/pci/pci_init.c` | MCFG consumption |
| `c/drivers/timer/hpet.c` | HPET validation and firmware handoff |
| `c/drivers/gpu/amd/polaris/resource/vbios.c` | VBIOS module and VFCT selection |
| `c/kernel/gui/wm.c`, `c/kernel/init/power.c` | Power-service startup, serialization, and transitions |
| `c/drivers/core/device.c`, `c/drivers/board/board.c` | Device quiescence and PCH reset integration |
| `c/drivers/board/board_io.h`, `c/drivers/board/smbios.c` | Firmware mapping access and SMBIOS reading |
| `c/drivers/board/nct_acpi.c`, `c/drivers/board/nct6779.c` | Sensor-port ownership checks |
| `c/drivers/power/acpi/devices.c`, `c/drivers/power/acpi/devices.h` | Interpreter integration and power snapshots |
| `c/drivers/power/acpi/native.c` | Kernel services, SCI, mappings, and deferred work |
| `c/drivers/power/acpi/button.c` | Fixed and control method power buttons |
| `c/kernel/pci/pci_prt_aml.c` | Interrupt-model selection and AML routing |
| `c/kernel/pci/pci_msi.c`, `c/kernel/pci/pci_route.h` | Route priority and INTx migration |
| `c/kernel/cpu/power/firmware.c`, `c/kernel/cpu/power/firmware.h` | Processor AML decoding and caller requirements |
| `c/kernel/cpu/power/pstate.c`, `c/kernel/cpu/power/cpuidle.c` | Frequency and idle policies |
| `c/drivers/power/acpi/ec/resources.c` | Embedded controller resource validation |
| `c/drivers/power/acpi/ec/driver.c`, `c/drivers/power/acpi/ec/transport.c` | Controller binding, events, and transactions |
| `c/drivers/power/README.md`, `c/drivers/power/acpi/ec/README.md` | Integration notes and recorded limitations |
| `third_party/uacpi/PROVENANCE.md` | Interpreter provenance and pinned revision |
| `third_party/uacpi/include/uacpi/kernel_api.h` | Required kernel services |
| `third_party/uacpi/source/uacpi.c`, `third_party/uacpi/source/tables.c` | Interpreter startup and table handling |
| `third_party/uacpi/source/utilities.c` | Interrupt-model and PCI routing APIs |
| `third_party/uacpi/source/opregion.c`, `third_party/uacpi/source/sleep.c` | OperationRegion registration, sleep, and reset |
| `Makefile` | Interpreter sources and configuration |
| `tests/acpi_integrity.mk`, `tests/drivers/power/acpi/test.mk`, `tests/drivers/power/acpi/ec/test.mk` | Integrity, power-query, and controller gates |
| `tests/power.mk`, `tests/boot/run-power-test.sh` | Power-transition gates |
| `tests/hpet.mk`, `tests/unit/hpet_test.c`, `tests/boot/run-hpet-test.py` | HPET host and guest checks |

## See also

- [BIOS boot path](boot-bios.md)
- [UEFI boot path](boot-uefi.md)
- [Local APIC and I/O APIC](apic.md)
- [SMP boot](smp-boot.md)
- [PCI](pci.md)
- [Power and platform drivers](../drivers/power-and-platform.md)