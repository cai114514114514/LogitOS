# Power and platform drivers

LogitOS uses uACPI to evaluate firmware methods for batteries, external power, thermal zones, shutdown and reset. A native backend supplies kernel services, interrupt delivery and an embedded controller transport. Board support handles firmware inventory, watchdog control and hardware monitoring, while Intel PCH and integrated memory controller SMBus drivers read DDR3 Serial Presence Detect (SPD) data.

## ACPI initialization

`kernel_power_start()` creates the `acpi-init` thread. The window manager calls it after releasing its state lock, allowing firmware initialization to sleep while desktop processing continues. Status queries and power requests also initialize ACPI services on demand. These callers are in `c/kernel/gui/wm.c` and `c/kernel/init/power.c`.

Initialization proceeds through the following stages:

1. `power_acpi_native_start()` checks scheduler and clock readiness, initializes ACPI table access and copies the RSDP into stable kernel storage.
2. A startup job confirms that the shared `kworker` has executed before deferred firmware work becomes available.
3. `power_acpi_start()` initializes uACPI with fatal checksum and table signature checks, then loads the namespace.
4. The registered `pci_prt_aml_preinit()` callback requests the firmware interrupt model. It evaluates `_PIC(1)` when SMP initialization has committed to APIC interrupt routing.
5. EC discovery installs the handler for the embedded controller address space before namespace device initialization.
6. EC query delivery is enabled after namespace initialization completes.

This ordering makes EC fields available to firmware `_INI` methods and schedules asynchronous `_Qxx` execution after device initialization. The implementation spans `c/drivers/power/acpi/native.c`, `c/drivers/power/acpi/devices.c` and `c/kernel/pci/pci_prt_aml.c`.

The kernel consumer’s AML mutex serializes initialization, status queries and power transitions. Initialization failures remain recorded for the boot lifetime because retrying a partly initialized interpreter would require teardown of its namespace and resource ownership.

The boot thread calls `board_init()` before querying power devices. After successful AML initialization, it checks NCT6779 resource ownership and installs handlers for the power button while holding the AML mutex. It then releases the mutex and calls `pci_prt_aml_ready()`. When selection of the firmware interrupt model succeeded, that function installs the AML PCI interrupt resolver and attempts to reroute existing INTx users. Releasing the mutex first allows interrupt retirement to wait for handlers. See `c/kernel/init/power.c`.

## Power device snapshots

`kernel_power_status()` fills a `struct power_acpi_status`. Each query constructs a fresh snapshot and walks the namespace. The `ready` and `error` fields report interpreter readiness and initialization or traversal errors. Battery and thermal entries carry measurement status, while `ec_error` records EC binding and runtime errors. The interface is defined in `c/drivers/power/acpi/devices.h`.

| Device | Discovery | Firmware methods | Snapshot data |
|---|---|---|---|
| Battery | `PNP0C0A` | `_STA`, `_BST`, `_BIF` | Presence, state, rate, remaining capacity, voltage and design information |
| AC adapter | `ACPI0003` | `_PSR` | Combined external power state |
| Thermal zone | Namespace objects of thermal-zone type | `_TMP`, `_CRT` | Current temperature and critical threshold validity |

Battery sampling checks `_STA` bit 4 for an inserted battery before evaluating `_BST` and `_BIF`. The decoder requires four integer fields in `_BST` and thirteen elements in `_BIF`, with integers in its first nine elements. It validates integer widths, battery state flags and capacity units before publishing the corresponding values. See `c/drivers/power/acpi/devices.c`.

Use battery presence and `status_error` to determine whether status measurements are usable, and `info_error` before interpreting capacity units. Unit zero means mWh for capacity and mW for rate; unit one means mAh and mA. Voltage is in mV. `POWER_UNKNOWN` represents the firmware’s unknown sentinel.

The adapter result starts at `ac_online = -1`. A successful `_PSR` returning zero or one establishes an observation. An online adapter keeps the combined result online across subsequent adapter visits.

Thermal methods return tenths of kelvin. After checking the conversion range, the driver converts the value to signed millidegrees Celsius by multiplying by 100 and subtracting 273150. `status_error` governs the current temperature, and `critical_valid` governs the critical threshold.

## Native firmware services

`c/drivers/power/acpi/native.c` connects uACPI to kernel allocation, synchronization, scheduling, clocks, port I/O and PCI configuration access. Mutexes and events use kernel primitives. Sleeps yield through the scheduler, while short stalls use the monotonic clock. Port operations preserve the requested access width because register side effects can depend on it.

RAM uses its established direct mapping. Other physical ranges use a supervisor alias shared with ACPI table access. The mapper checks the installed entries in the page table and retains shared mappings so concurrent table and OperationRegion users can continue accessing them.

The System Control Interrupt (SCI) adapter checks the interrupt identity against the FADT and applies interrupt overrides when resolving its route. Installation checks route availability. Removal confirms hardware masking before retiring the interrupt vector and callback context.

General purpose event (GPE) and notification work enters a preallocated queue of 64 entries. Interrupt submissions enqueue callbacks, and the shared `kworker` executes them in a context where they can sleep. Queue exhaustion returns an error to uACPI.

## Embedded controller support

An embedded controller (EC) exposes registers that firmware can use for battery, thermal and platform functions. LogitOS discovers `PNP0C09` devices and derives their ports and GPE from the namespace.

`c/drivers/power/acpi/ec/resources.c` requires a present, enabled and functioning controller. Its `_CRS` must describe two distinct I/O ports at fixed locations, each one byte wide, with the controller decoding 16 bits and the data port listed before the control port. `_GPE` supplies a global integer GPE. When an ECDT identifies the same namespace device, the driver checks its ports, GPE and available integer `_UID` against the namespace description.

The binding code checks port and GPE collisions between EC bindings and verifies GPE availability. Installing the handler for the embedded controller address space runs `_REG` before device initialization, making EC fields available to `_INI`. This ordering is implemented in `c/drivers/power/acpi/ec/driver.c` and supported by `third_party/uacpi/source/opregion.c`.

Read, write and query commands share a transaction mutex. Each input write waits for the input buffer to become empty, writes the byte, then delays one microsecond before checking completion. The delay gives the controller’s input buffer flag time to become visible before another byte is sent. Reads wait for the output buffer flag before consuming data.

Each handshake has a 250 ms deadline and a finite polling count. Transfers span one to eight bytes within the EC address space of 256 bytes. Reads publish their result after the complete transfer. Transaction failures record an error and quarantine the transport until reboot because the controller’s command state may be uncertain. See `c/drivers/power/acpi/ec/transport.c`.

The GPE callback samples status and queues work. The worker sends a query when the EC reports an SCI event and evaluates the matching `_Qxx` method for a nonzero response. It releases the transport mutex before AML evaluation because the method can access EC fields itself. Query failures and exhaustion of the budget of 32 queries leave delivery masked. Activation also checks for an event already pending after namespace initialization.

## Board services and power button

`board_early()` runs before storage probing, while boot still has one thread. It identifies a supported Intel LPC bridge at `00:1f.0`, records its resource configuration and attempts to halt a TCO watchdog inherited from firmware. The halt write preserves the other control bits and receives a readback check. Diagnostics distinguish an already halted timer, a successful halt and a refused halt request. Performing this step early protects later initialization from an inherited watchdog countdown when the halt succeeds. See `c/kernel/init/kmain.c` and `c/drivers/board/pch_lpc.c`.

`board_init()` reports SMBIOS board, system, BIOS and populated DIMM information, calls the iMC SPD scanner and identifies Super I/O chips on configuration ports forwarded by the LPC bridge. SMBIOS parsing validates checksums for entry points and bounds structure and string reads. It uses the loader’s copied SMBIOS data when available; physical table access is checked against captured firmware regions or the BIOS area. See `c/drivers/board/board.c` and `c/drivers/board/smbios.c`.

The NCT6779 driver discovers an enabled hardware monitor logical device and restores the previous selector for the Super I/O logical device. After AML initialization, `nct6779_acpi_gate()` checks the monitor’s I/O window of eight bytes against SystemIO OperationRegions. Successful admission permits serialized snapshots of temperature, fan speed and voltage that restore the original register bank. A firmware conflict or incomplete ownership scan records a permanent access error. See `c/drivers/board/nct6779.c` and `c/drivers/board/nct_acpi.c`.

`power_acpi_button_install()` attempts to arm both the fixed event for the ACPI power button and the notification handlers for `PNP0C0C` devices. It clears stale status of the fixed event before installing the handler and finalizes GPE initialization for buttons that use control methods. A fixed event or notification value `0x80` queues work that creates a `pwrbtn` thread to call `kernel_poweroff()`. Running shutdown in that thread lets filesystem draining wait for I/O completed by `kworker`. An atomic flag coalesces presses while shutdown is in progress. See `c/drivers/power/acpi/button.c`.

## CPU power support

The previous page listed CPU power state control as future work. SMP startup now calls `pstate_init()` and `cpuidle_init()` on the boot processor and during startup of application processors in `c/kernel/cpu/smp/smp.c`.

The performance ratio driver accepts Intel family 6 models `0x2d` and `0x3e` with MSR support and Enhanced Intel SpeedStep enabled by firmware. It requests the highest permitted ratio, including turbo when available, while preserving the other bits of the performance control register. The CPU checks and ratio policy are in `c/kernel/cpu/power/hardware.h` and `c/kernel/cpu/power/pstate.c`.

The scheduler calls `cpuidle_enter()`. Its policy selects HLT or an admitted MONITOR/MWAIT hint. MWAIT admission checks CPU capabilities, an APIC timer that always runs, supported substates and firmware exit latency of at most 200 microseconds. See `c/kernel/cpu/power/cpuidle.c` and `c/kernel/sched/sched.c`.

## Shutdown and reset

`kernel_poweroff()` begins a filesystem admission drain and calls `logitfs_sync()`. It then initializes ACPI as needed, prepares S5 through `power_acpi_prepare_off()` and requests entry through `power_acpi_enter_off()`. The drain remains held throughout this sequence. See `c/kernel/init/power.c`.

S5 preparation obtains firmware sleep types and evaluates `_PTS` through uACPI. If S5 entry returns, the kernel attempts to restore ACPI events for the working state, releases the filesystem drain and returns an error. Preparation and sync failures also release the drain. The firmware operations are implemented in `third_party/uacpi/source/sleep.c`.

`kernel_reboot()` performs the same drain and sync preparation, then calls `dev_quiesce_all()` before requesting reset. The device model retains storage and display bindings and attempts to unbind other drivers that provide removal hooks. After PCI device removal, it checks Bus Master state and attempts to clear any remaining enable.

Bound storage with Bus Master enabled counts as unconfirmed DMA because filesystem draining can coexist with controller receive engines or swap I/O. Display drivers with a `quiesce` hook report whether GPU activity has stopped. The AMD boot framebuffer driver forwards this check to its support for the Polaris self test in builds enabled by `HWHUD=1`. See `c/drivers/core/device.c` and `c/drivers/gpu/amd/bootfb.c`.

The reset sequence requests the FADT reset mechanism through uACPI, then tries the supported Intel PCH cold reset at port `0xCF9`. When the device checks report confirmed quiescence, the remaining fallbacks are the legacy 8042 reset pulse and a triple fault.

With unconfirmed DMA, the kernel also attempts the PCH reset before the FADT request. If those platform reset attempts return, it prints a refusal, flushes serial output and halts. This keeps a device that may still write RAM from continuing into a boot reached through a processor reset. See `c/kernel/init/power.c` and `c/drivers/board/pch_lpc.c`.

## Intel PCH SMBus access

The earlier page described the `8086:1d22` C600/X79 binding. The current allowlist also includes `8086:1c22` for 6 Series/C200 and `8086:1e22` for 7 Series. All require PCI class/subclass `0c:05`. The device allowlist establishes the supported register layout.

Registration uses `DRIVER_DECLARE`, and `dev_probe_all()` invokes the probe during boot. The binding path is in `c/drivers/core/device.c` and `c/kernel/init/kmain.c`.

Before controller access, `pch_smbus_probe()` checks clock readiness, controller mode and BAR4. The BAR must describe a nonzero I/O window aligned to 32 bytes, at least 32 bytes long and within the port address range. PCI I/O decode and the writes that enable the host both receive readback checks. See `c/drivers/platform/intel_pch_smbus.c`.

Each transaction claims the hardware semaphore, waits for an idle engine and starts an SMBus Byte Data read. Polling combines deadlines with an iteration ceiling.

| Stage | Time budget |
|---|---|
| Semaphore acquisition | 5 ms |
| Idle wait | 35 ms |
| Transfer completion | 35 ms |
| Stop confirmation after timeout | 5 ms |

A transfer timeout triggers a stop request for the transaction started by this driver. Recovery confirms that the engine stopped before releasing ownership. If confirmation fails, the driver retains the asserted stop request and semaphore, then quarantines the controller for the boot lifetime. Failed verification of PCI Command restoration also quarantines it. These rules preserve ownership when recovery leaves the hardware state uncertain.

## SPD inventory and decoding

The PCH scan visits addresses `0x50` through `0x57`. It first reads byte 2 to identify DDR3, then reads bytes 0 through 145 for decoding. `struct pch_spd_inventory` records accepted modules, rejected contents, read errors and summed module capacity. A completed probe attaches the inventory to its device even when the scan records errors; `pch_spd_inventory()` exposes the same inventory.

Sandy Bridge-EP and Ivy Bridge-EP have an additional SPD path through the CPU’s integrated memory controller. `imc_spd_scan()` searches PCI buses from 255 downward for Intel device `3ca8` or `0ea8` at device 15, function 0. This direct search reaches the uncore function outside bridge enumeration. ECAM supplies access to the two controllers’ register groups at `0x180` and `0x190`. See `c/drivers/platform/intel_imc_smbus.c`.

Both `board_init()` and the boot hardware reporter call the iMC scanner; its guard admits one scan attempt per boot. The reporter’s call ensures that inventory is available before the later board report. These callers are in `c/drivers/board/board.c` and `c/kernel/diag/hwreport.c`.

Each iMC byte read saves the control register, pauses polling of the DIMM thermal sensor, selects the SPD address and issues a command to read a byte. It writes the saved control value back after the attempt. The idle wait has a 40 ms deadline and completion has a 35 ms deadline, each bounded by one million polls. A NACK advances the scan; a controller that is busy or times out ends that controller’s scan.

`struct imc_spd_inventory` records the discovered PCI device and bus, controller and address for each module, rejection and error counts, and summed module capacity. `imc_spd_inventory()` exposes the result.

Both SMBus paths use `ddr3_spd_decode()`. It validates the DDR3 type, CRC and geometry before filling `struct ddr3_spd_info`. Byte 0 selects CRC coverage of 117 or 126 bytes, with the stored CRC in bytes 126 and 127. Capacity derives from chip density, primary bus width, device width and rank count. ECC extension width has its own field.

The decoder also extracts the SPD revision, module type, raw manufacturer identifier, four serial bytes and a part number of eighteen characters. It replaces nonprintable bytes in the part number with question marks and trims trailing spaces. See `c/drivers/platform/ddr3_spd.c`.

## Verification paths

The repository defines the following checks.

| Target | Coverage |
|---|---|
| `make test-power-aml-host` | AML execution, battery decoding, changing thermal measurements and malformed firmware results |
| `make test-power-ec-aml` | EC transport, OperationRegions, initialization ordering and deferred queries |
| `make test-x79-chipset-host` | PCH SMBus transactions, SPD decoding, ownership checks and recovery |
| `make test-amd-bootfb-host` | Framebuffer binding and forwarding of the Polaris quiesce result, using a self-test stub |
| `make test-power` | QEMU shutdown, persisted contents, journal replay checks, background ACPI discovery and reboot |
| `make test-power-negctl` | Interrupted-write control requiring journal replay or a filesystem check finding |

The AML, EC, chipset and framebuffer host targets have prerequisite negative controls. The controls require named assertion failures after mutations to production behavior. The QEMU durability control, `test-power-negctl`, is invoked from `tests/power.mk` on its own, outside `test-power`.

## Limitations

- Power snapshots retain eight batteries and sixteen thermal zones, with a truncation flag for additional entries. `_BIX`, desktop power notifications and suspend/resume remain outside the power service described in `c/drivers/power/README.md`.
- Native ACPI PCI access supports segment zero. Deferred firmware work uses a shared worker that can migrate between CPUs.
- EC support has four slots for the boot lifetime. Shared `_GLK` ownership, aliased I/O resources, GPIO interrupts, `_GPE` methods that return a package and early ECDT bootstrap access are unsupported. Port collision checks cover this driver’s bindings. A failed EC write may have completed a prefix before quarantine.
- ACPI SMBus OperationRegions have no installed handler. The platform SMBus interfaces provide DDR3 SPD reads. The PCH inventory holds eight modules; the iMC inventory holds sixteen across two controllers. The iMC scan requires ECAM and selects the first supported function found while searching buses downward.
- `module_capacity_mib` sums accepted module capacities. Firmware reservations and failed memory training can reduce usable RAM.
- `cpu_power_load_acpi()` and `pstate_tick()` have no callers in the current tree. The load governor and firmware table loading remain unwired; the idle state cache is empty and selects HLT.
- NCT6779 ownership checking covers SystemIO OperationRegions present during the boot scan. Undocumented SMM users and regions created by later method execution are outside that check.
- Host fixtures and QEMU checks establish behavior in their named environments. EC, chipset, iMC and board sources record pending physical-device validation; their presence in the tree establishes implementation scope, while operation on every matching board awaits validation.

## See also

- [Driver overview](overview.md)
- [ACPI](../kernel/acpi.md)
- [PCI](../kernel/pci.md)
- [Physical memory](../kernel/physical-memory.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/power/acpi/devices.h` | Power snapshots, units, validity fields and service interfaces |
| `c/drivers/power/acpi/devices.c` | Namespace initialization, device queries and power wrappers |
| `c/drivers/power/acpi/native.c` | Kernel services, mappings, SCI handling and deferred work |
| `c/drivers/power/acpi/button.c` | Power-button handlers, deferred shutdown and press coalescing |
| `c/drivers/power/acpi/ec/resources.c` | EC resource validation and ECDT consistency checks |
| `c/drivers/power/acpi/ec/driver.c` | EC binding, OperationRegions and deferred queries |
| `c/drivers/power/acpi/ec/transport.c` | EC handshakes, transfers and quarantine |
| `c/drivers/power/acpi/ec/README.md` | EC scope and evidence limitations |
| `c/drivers/power/README.md` | Power-service integration notes and remaining scope |
| `c/drivers/board/board.h` | Board initialization contract and evidence scope |
| `c/drivers/board/board.c` | Board reporting, iMC scan caller and reset integration |
| `c/drivers/board/pch_lpc.c` | LPC matching, TCO watchdog control, Super I/O identification and CF9 reset |
| `c/drivers/board/smbios.c` | SMBIOS handoff capture, validation and inventory decoding |
| `c/drivers/board/nct6779.c` | Monitor discovery and serialized sensor snapshots |
| `c/drivers/board/nct_acpi.c` | Firmware ownership checks for the monitor’s I/O window |
| `c/kernel/init/power.c` | Query serialization, discovery thread, shutdown and reset |
| `c/kernel/init/kmain.c` | Early board initialization and driver probing |
| `c/kernel/gui/wm.c` | Desktop startup of ACPI discovery |
| `c/kernel/diag/hwreport.c` | Boot diagnostics and iMC scan caller |
| `c/kernel/pci/pci_prt_aml.c` | Firmware interrupt-model selection and PCI routing activation |
| `c/kernel/cpu/smp/smp.c` | CPU power initialization during processor startup |
| `c/kernel/cpu/power/hardware.h` | CPU eligibility checks and hardware interfaces |
| `c/kernel/cpu/power/pstate.c` | Intel performance-ratio policy |
| `c/kernel/cpu/power/cpuidle.c` | HLT and MONITOR/MWAIT policy |
| `c/kernel/cpu/power/firmware.c` | Firmware performance and idle-state parser and caches |
| `c/kernel/sched/sched.c` | Scheduler entry into CPU idle policy |
| `c/drivers/core/device.c` | Driver registration, binding and reset quiescence checks |
| `c/drivers/gpu/amd/bootfb.c` | Display-driver quiesce hook for the Polaris self-test |
| `c/drivers/platform/intel_pch_smbus.h` | PCH SPD inventory interface |
| `c/drivers/platform/intel_pch_smbus.c` | PCH matching, transactions, recovery and SPD scanning |
| `c/drivers/platform/intel_imc_smbus.h` | iMC SPD inventory interface |
| `c/drivers/platform/intel_imc_smbus.c` | Uncore discovery, controller transactions and SPD scanning |
| `c/drivers/platform/ddr3_spd.h` | Decoded module structure and decoder interface |
| `c/drivers/platform/ddr3_spd.c` | DDR3 CRC, geometry and identification decoding |
| `third_party/uacpi/source/opregion.c` | Address-space handler installation and `_REG` execution |
| `third_party/uacpi/source/sleep.c` | Firmware sleep preparation, entry, restoration and reset |
| `tests/drivers/power/acpi/test.mk` | AML query checks and conversion control |
| `tests/drivers/power/acpi/ec/test.mk` | EC transport and interpreter checks with controls |
| `tests/x79_chipset.mk` | PCH SMBus and SPD host-test targets |
| `tests/gpu/amd/bootfb.mk` | Framebuffer host checks and prerequisite controls |
| `tests/power.mk` | QEMU shutdown, reboot and durability-control targets |
