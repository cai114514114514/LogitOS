# Board, sensors and SMBIOS

LogitOS combines firmware inventory, Intel chipset discovery and Nuvoton hardware monitoring under `c/drivers/board/`. SMBIOS supplies board, system, BIOS and DIMM descriptions. The Intel LPC bridge provides configuration-port forwarding information, watchdog control and platform reset. The NCT6779 driver discovers the hardware monitor, checks its ACPI resource ownership and supplies temperature, fan and voltage snapshots to the optional desktop hardware panel.

## Initialisation

Board setup spans early boot and the power worker started by the window manager.

In `c/kernel/init/kmain.c`, `smbios_capture_boot_info()` saves the loader’s SMBIOS copy and firmware memory-region information. `board_early()` then discovers the LPC bridge and attempts to halt an inherited TCO watchdog before storage probing. The chipset snapshot is published while boot has one thread; subsequent board operations read that snapshot.

After scheduler startup, the window manager calls `kernel_power_start()`. Its worker in `c/kernel/init/power.c` performs this sequence:

1. `board_init()` reports SMBIOS identity, calls the memory-module SPD scanner and discovers Super I/O hardware.
2. `kernel_power_init()` requests ACPI interpreter initialisation and reports power-device status.
3. Following successful interpreter initialisation, `nct6779_acpi_gate()` checks the monitor’s I/O window while the worker holds the sleepable AML mutex.

Discovery establishes the monitor’s address before the ownership scan. The driver publishes the scan result under its spinlock, allowing later callers to obtain sensor snapshots.

The root-mount failure path also calls `board_init()` before entering the diagnostic screen, so board-report lines are available there (`c/kernel/init/kmain.c`).

## Board identification through SMBIOS

`c/drivers/board/smbios.c` recognises the `_SM3_` entry-point anchor and the `_SM_` anchor with its `_DMI_` tail. Entry parsing checks declared lengths and checksums before accepting the structure-table address.

The parser fills `struct smbios_info`, defined in `c/drivers/board/smbios.h`:

| SMBIOS type | Collected fields |
|---|---|
| 0 | BIOS vendor, version and release date. |
| 1 | System vendor and product. |
| 2 | Baseboard vendor, product and version. |
| 17 | DIMM locator, bank, decoded capacity, speed, vendor and part number. Entries with a decoded nonzero capacity enter the inventory. |

Formatted structure lengths bound field access. The walker searches for each string set’s terminating double NUL within the supplied table extent and stops when a record crosses that extent or has an unterminated string set. String copying adds a terminating NUL, trims trailing spaces and replaces bytes outside printable ASCII with `?`.

Under UEFI, `c/boot/efi/loader.c` validates and copies the entry point and structure table before boot services end. It passes them through `LOGIT_BOOT_TAG_SMBIOS`, defined in `include/abi/logit_boot.h`. This preserves tables placed in boot-services memory that the kernel can later reclaim.

`smbios_read()` uses that copy when available. Its physical-address path checks firmware extents and can scan the legacy BIOS segment, trying SMBIOS 3 entries before SMBIOS 2 entries. Outside the accepted BIOS area, extent checks reject known device apertures and require the table to fit within a captured firmware region.

`board_init()` prints the decoded identity as `[board]` log lines. Hardware selection follows chipset and Super I/O identifiers. SMBIOS strings provide descriptive inventory.

The board report also calls `imc_spd_scan()` in `c/drivers/platform/intel_imc_smbus.c`. For supported Sandy Bridge-EP and Ivy Bridge-EP memory controllers, this scans DDR3 SPD EEPROMs through the CPU’s integrated SMBus. The scanner caches its inventory after its first invocation.

## PCH LPC decoding and board controls

`pch_lpc_find()` in `c/drivers/board/pch_lpc.c` reads PCI configuration space at bus 0, device 31, function 0, written `00:1f.0`. It matches Intel vendor ID `0x8086` against these device ranges:

| Family | Device IDs |
|---|---|
| ICH9 | `0x2910` through `0x2919`. |
| 6 Series/C200 | `0x1c40` through `0x1c5f`. |
| C600/X79 | `0x1d40` through `0x1d41`. |
| 7 Series/C216 | `0x1e40` through `0x1e5f`. |

The resulting `struct pch_lpc` holds the device ID, family, ACPI I/O base and LPC interface enables.

| PCI configuration offset | Interpretation |
|---|---|
| `0x40` | ACPI base, masked with `0xff80`. |
| `0x44` | Bit `0x80` enables ACPI decoding. |
| `0x82` | Bit `0x1000` forwards configuration ports `0x2e` and `0x2f`; bit `0x2000` forwards ports `0x4e` and `0x4f`. |

`superio_probe()` follows these forwarding bits when choosing ports. Each probe enters configuration mode, reads identification registers and exits configuration mode. Its tables name selected Nuvoton/Winbond, ITE and Fintek chips.

The same source implements two board controls:

- `tco_quiesce()` examines the watchdog at ACPI base plus `0x60`. For a running timer, it sets the halt bit while preserving the other control bits, then reads the control register back. Its report distinguishes an already halted watchdog, a successful halt and a refused halt.
- `pch_reset()` preserves unrelated bits in port `0xcf9`, writes the reset-arm state and then requests a cold platform reset. `board_reset()` flushes queued serial output before invoking it.

## Discovering the NCT6779

`board_init()` selects the monitor driver when the Super I/O ID satisfies `(id & 0xfff8) == 0xc560`, allowing the revision bits covered by that mask.

In `c/drivers/board/nct6779.c`, `nct6779_find()` holds the monitor spinlock while it:

1. Enters configuration mode with two `0x87` writes.
2. Saves the logical-device selector from register `0x07`.
3. Reads the chip ID and selects hardware-monitor logical device `0x0b`.
4. Reads the base from registers `0x60` and `0x61`, then aligns it to eight bytes.
5. Accepts a nonzero base when bit 0 of enable register `0x30` is set.
6. Restores the saved logical-device selector and exits configuration mode with `0xaa`.

The restoration path also covers rejected discoveries, so the caller’s logical-device selection survives the probe.

The accepted base identifies a banked register interface:

| Port or register | Role |
|---|---|
| Base plus 5 | Register index port. |
| Base plus 6 | Register data port. |
| Register `0x4e` | Bank selector. |

A register number encodes the bank in its upper byte and the index in its lower byte.

## ACPI ownership

The monitor begins with access pending. `nct6779_acpi_gate()` asks `nct_acpi_check()` to examine the complete aligned I/O window of eight bytes.

`c/drivers/board/nct_acpi.c` walks ACPI OperationRegion objects and compares SystemIO ranges with that window. It checks each region as the walk encounters it, so the scan’s coverage follows the namespace walk without a fixed collection limit. Range calculations widen endpoints and clip firmware ranges to the x86 port space.

An overlap produces a diagnostic containing the claimant’s namespace path. Object acquisition, object release and namespace-walk failures receive diagnostics identifying the failed step. After scanning, the monitor driver checks that its base still matches the examined address and publishes the result under its spinlock.

`nct6779_read()` reports these states:

| Return value | Meaning |
|---|---|
| `0` | Snapshot completed. |
| `-1` | No usable monitor was discovered. |
| `-11` | ACPI admission is pending. |
| `-16` | An ACPI SystemIO region claims part of the window. |
| `-5` | The ownership scan failed, or the base changed during the scan. |

A firmware claim or scan failure persists for the boot session. Repeating discovery after successful admission returns access to the pending state, requiring admission for the newly selected base. Interpreter initialisation failure keeps access pending.

## Temperature, fan and voltage snapshots

`struct nct_sensors` in `c/drivers/board/nct6779.h` carries the monitor base and arrays for six temperatures, six fan speeds and fifteen voltages.

| Reading | Registers | Representation |
|---|---|---|
| Temperature | `0x490` through `0x495`. | Signed integer degrees Celsius, labelled `SYSTIN`, `CPUTIN` and `AUXTIN0` through `AUXTIN3`. The interface treats `-128` as disconnected. |
| Fan speed | Byte pairs starting at `0x4c0`, `0x4c2`, `0x4c4`, `0x4c6`, `0x4c8` and `0x4ca`. | Two bytes combined directly into an RPM value. |
| Voltage | `0x480` through `0x48e`. | Millivolts after internal-divider scaling. |

Voltage conversion uses an 8 mV step for most channels and a 16 mV step for `AVSB`, `3VCC`, `3VSB` and `VBAT`. Labels, register addresses and scaling factors are defined together in `c/drivers/board/nct6779.c`.

A snapshot takes the spinlock with local interrupts saved and disabled. After checking admission, it saves the current bank, reads the channels and restores the bank before releasing the lock. Keeping the saved bank within the critical section prevents concurrent OS readers from saving and restoring one another’s temporary bank selection. Callers consume the arrays after a successful return.

## Reporting the values

The board report logs SMBIOS identity, LPC decoding, watchdog state and Super I/O identification through `c/drivers/board/board.c`.

Its first sensor-read attempt follows discovery and precedes ACPI admission. For a discovered monitor still awaiting admission, the shared error branch prints `[hwmon] NCT6779 monitor disabled or absent; not enabled`. That message also covers the pending state; the access return value and hardware-panel status distinguish the causes.

With `HWHUD=1`, `Makefile` defines `LOGIT_HWHUD`, enabling hardware-panel integration in `c/kernel/gui/wm.c`. The panel uses a one-second sampling interval and yields to system overlays through the decisions in `c/kernel/diag/hwhud_panel.h`.

`lines_thermal()` in `c/kernel/diag/hwhud.c` combines CPU package temperature from supported model-specific registers with NCT temperatures and nonzero fan readings, subject to row space. Following a successful NCT snapshot, a voltage row formats `CPUVC`, `3VCC`, `3VSB`, `VBAT` and `VTT` in volts with three decimal places.

Admission status appears as `nct pending`, `nct firmware-owned` or `nct ACPI scan failed`.

## Safety rules for board registers

Register selection is shared state. Reading a sensor requires writes to index and bank selectors, making transaction scope and restoration part of correct acquisition.

- Follow the LPC forwarding bits when choosing Super I/O configuration ports.
- Require the matched chip ID, enabled hardware-monitor logical device and nonzero aligned base.
- Complete ACPI admission before accessing hardware-monitor registers.
- Keep namespace inspection under the sleepable AML mutex. Use the monitor spinlock for port transactions and saved selector state.
- Restore the logical-device selection on every discovery exit and the bank after each snapshot.
- Preserve unrelated bits in board-control writes, as the watchdog halt and reset paths do.

`c/drivers/board/board_io.h` centralises the board drivers’ port, PCI configuration and firmware-memory access. The SPD reader in `c/drivers/platform/intel_imc_smbus.c` also restores the memory controller’s original control register after every byte transaction, returning firmware polling configuration to its previous state.

## Limitations

- Procfs exposes memory, uptime, version and process information; SMBIOS and board-sensor data reach consumers through the board drivers and the hardware panel (`c/fs/procfs/procfs.c`).
- The ACPI scan covers SystemIO OperationRegions present when it runs. Later dynamic regions and undocumented System Management Mode accesses fall outside that check. The spinlock coordinates OS readers (`c/drivers/board/nct_acpi.c`, `c/drivers/board/nct6779.c`).
- Sensor acquisition implements the NCT6779 layout, and other chips in the Super I/O tables receive identification reports. The monitor interface covers sensor acquisition only (`c/drivers/board/board.c`, `c/drivers/board/nct6779.h`).
- SMBIOS parsing caps the table extent at 64 KiB and stores up to eight DIMMs with decoded nonzero capacity (`c/drivers/board/smbios.c`, `c/drivers/board/smbios.h`).
- Board and ownership source comments mark X79-VG4 hardware verification as pending (`c/drivers/board/board.h`, `c/drivers/board/nct_acpi.c`).

## Source map

| File | What it contains |
|---|---|
| `c/drivers/board/board.c` | Board reporting, early watchdog handling, monitor discovery and reset integration. |
| `c/drivers/board/board.h` | Lifecycle interface and recorded verification scope. |
| `c/drivers/board/board_io.h` | Port, PCI configuration and firmware-memory access adapters. |
| `c/drivers/board/smbios.c` | Entry validation, bounded structure parsing, boot-data capture and firmware-address checks. |
| `c/drivers/board/smbios.h` | SMBIOS inventory structures and parser interfaces. |
| `c/drivers/board/pch_lpc.c` | Chipset matching, LPC decoding, Super I/O identification, watchdog halt and reset. |
| `c/drivers/board/pch_lpc.h` | Chipset, watchdog and Super I/O report structures. |
| `c/drivers/board/nct6779.c` | Monitor discovery, access state, register tables and snapshot transactions. |
| `c/drivers/board/nct6779.h` | Sensor arrays, labels and acquisition interface. |
| `c/drivers/board/nct_acpi.c` | SystemIO ownership scan and conflict diagnostics. |
| `c/drivers/board/nct_acpi.h` | I/O range and ownership-check interfaces. |
| `c/drivers/platform/intel_imc_smbus.c` | Supported memory-controller SPD scans and control restoration. |
| `c/boot/efi/loader.c` | Validation and copying of firmware SMBIOS data. |
| `include/abi/logit_boot.h` | SMBIOS boot-tag format. |
| `c/kernel/init/kmain.c` | Early SMBIOS capture, board setup and root-failure reporting path. |
| `c/kernel/init/power.c` | Board discovery, AML initialisation and monitor admission ordering. |
| `c/kernel/gui/wm.c` | Power-worker startup and optional hardware-panel integration. |
| `c/kernel/diag/hwhud.c` | Temperature, fan, voltage and admission-status presentation. |
| `c/kernel/diag/hwhud_panel.h` | Panel sampling cadence and visibility decisions. |
| `c/fs/procfs/procfs.c` | Procfs namespace and generated content. |
| `Makefile` | Hardware-panel build selection and ACPI include configuration. |

## See also

- [Driver overview](overview.md)
- [Power and platform drivers](power-and-platform.md)
- [ACPI](../kernel/acpi.md)
- [Kernel diagnostics](../kernel/diagnostics.md)
- [procfs and ramfs](../filesystems/procfs-and-ramfs.md)
- [Running on real hardware](../getting-started/real-hardware.md)
