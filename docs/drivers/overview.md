# Driver overview

LogitOS drivers connect PCI functions, legacy PC hardware and USB devices to the kernel’s storage, network, display, audio and input interfaces. Most implementations live under `c/drivers`; PCI discovery and routing also use `c/kernel/pci`. This page maps each driver family to its device matches, implemented behavior and repository test coverage. Test references describe the checks supplied by the tree.

## Discovery and binding

PCI enumeration follows bridges, records functions, sizes BAR resources and discovers capabilities. Configuration access uses an ACPI MCFG window when available, with legacy configuration ports as the fallback (`c/kernel/pci/pci.c`, `c/kernel/pci/pci_init.c`).

The device model defines three main objects in `c/drivers/core/driver.h`:

| Object | Role |
| --- | --- |
| `struct device` | An enumerated function, its resources, interrupt state and binding. |
| `struct dev_match` | Matching criteria for vendor, device, class, subclass and programming interface. |
| `struct driver` | A match table, probe and removal callbacks, and an optional display quiesce callback. |

`DRIVER_DECLARE` places a driver reference in the registry section retained by `linker.ld`. `dev_probe_all()` tries registered drivers against unbound devices; a successful probe establishes the binding. `Makefile` discovers C sources recursively under `c/drivers`.

Boot ordering follows subsystem dependencies. Framebuffer selection and root disk discovery run early. Ethernet initialization applies its own priority order. The general probe pass runs after SMP initialization, followed by late AHCI interrupt attachment (`c/kernel/init/kmain.c`).

USB binds class drivers to interfaces beneath the binding of the PCI host controller. Matching each interface’s class, subclass and protocol lets a composite device expose several functions (`c/drivers/usb/usb_bind.c`).

## Resources, interrupts and DMA

`dev_bar_map()` maps a memory BAR. `dev_bar_map_window()` maps a bounded interval within one. The second lets the Polaris self-test map its VRAM arena while the framebuffer keeps its write-combined mapping on other pages. Checked PCI enable and disable helpers verify Command register changes (`c/drivers/core/device.c`, `c/drivers/gpu/amd/bootfb.c`).

`dev_irq_request()` selects MSI-X, MSI or INTx according to device capabilities and the configured preference. PCI interrupt routing lives in `c/kernel/pci/pci_msi.c`; vector allocation and callback retirement live in `c/drivers/core/irq.c`. Teardown drains callbacks before releasing their state, retaining ownership when shutdown remains uncertain.

INTx resolution uses ACPI `_PRT`, supported Intel PCH routing registers and interrupt line information from firmware. The AML resolver follows bridges and handles interrupt link resources. After AML initialization, a rerouting pass revisits existing INTx bindings. I/O APIC support handles the controllers described by ACPI (`c/kernel/pci/pci_prt_aml.c`, `c/kernel/cpu/irq/ioapic.c`).

The DMA interface distinguishes CPU pointers from device addresses. It provides coherent buffers, streaming mappings, address masks, scatter/gather segments and completion tokens. Completion or acknowledged hardware shutdown permits reclamation; quarantine retains memory that a device may still access (`c/drivers/core/dma.h`).

Before reboot, `dev_quiesce_all()` invokes display quiescence and unbinds eligible devices. Storage remains bound. Unconfirmed DMA makes the reboot path require a platform reset before starting another kernel (`c/drivers/core/device.c`, `c/kernel/init/power.c`).

The QEMU educational PCI device, `1234:11e8`, supplies a worked driver example. Its self-test raises interrupts through MSI and forced INTx. `tests/boot/run-devmodel-test.sh` also checks configuration access and enumeration behind a PCIe bridge (`c/drivers/core/qemu_edu.c`).

## Storage drivers

The block layer registers disks and partitions behind a common request interface. It checks bounds, stages user buffers, and serializes admission and completion through each medium’s gate. Capable backends admit bounded queues. Flushes fence the queue, and overlapping operations involving a write wait for the conflicting request to finish. Partition parsing covers MBR, extended partitions and GPT (`c/drivers/block/blkdev.c`, `c/drivers/block/part.c`).

| Family | Devices and implemented path | Test coverage |
| --- | --- | --- |
| ATA PIO | Legacy primary master; IDENTIFY and LBA28 reads and writes through ATA ports. Writes issue a cache flush. | Legacy fallback implemented in `c/drivers/block/ata.c`. |
| AHCI | PCI `01:06:01`; multiple controllers and SATA disk ports, 512-byte logical sectors including 512e, LBA28/LBA48 DMA and NCQ for eligible disks. | Raw, MBR and GPT roots in `tests/boot/run-ahci-test.sh`; cases covering discovery and sector format in `tests/storage_hardware.mk`. |
| NVMe | PCI `01:08:02`; one selected NVM namespace with 512-byte or 4096-byte logical blocks. Native 4 KiB access is adapted to the block layer’s 512-byte units. | PCIe bridge, UEFI and native 4 KiB cases in `tests/storage_hardware.mk`. |
| VirtIO block | `1af4:1001`; modern PCI capabilities, reads, writes and negotiated flush operations. | QEMU root disk path in `tests/boot/run-test.sh`; DMA checks in `tests/dma.mk`. |
| VirtIO SCSI | `1af4:1004` and `1af4:1048`; one controller, targets 0–7, LUN 0 and direct-access disks with 512-byte logical blocks. | Both identities, filesystem changes, flushes and persistence across boots in `tests/virtio_scsi.mk`. |
| USB mass storage | Bulk-Only Transport interfaces carrying SCSI commands, `08:06:50`; direct-access media with 512-byte logical blocks and multiple LUNs. | xHCI, EHCI and combined cases, partition guards and persistence across cold boots in `tests/usb_storage.mk`. |

The backend implementations are `c/drivers/block/ahci.c`, `c/drivers/block/nvme.c`, `c/drivers/virtio/virtio_blk.c`, `c/drivers/virtio/virtio_scsi.c` and `c/drivers/usb/usb_storage.c`.

Early disk registration proceeds through NVMe, VirtIO block, AHCI, VirtIO SCSI and ATA. Root selection scans the resulting registry for a LogitFS superblock (`c/drivers/block/blkdev.c`).

The earlier overview listed NCQ as absent. AHCI now reserves an internal command slot and derives data-queue admission from controller slots and the disk’s reported depth. Recovery can attribute an NCQ failure through READ LOG EXT page `10h` and reset the SATA link through COMRESET. Root mounting polls; late attachment checks MSI delivery and compares queued reads with a serial baseline. Failed checks reduce queue admission or retain polling (`c/drivers/block/ahci.c`).

## Ethernet and Wi-Fi

Ethernet backends register through `c/drivers/net/netdev.c`. Its early probe order is VirtIO, Intel 8254x, Intel 82574, RTL8139, PCnet, RTL8169 and Intel PCH2. Receive polling visits registered backends.

PCI match entries establish eligibility for a probe. Controller checks then determine whether the backend can initialize that function. The shared inventory lives in `c/drivers/net/net_ids.inc`.

| Family | PCI match entries and probe scope | Test coverage |
| --- | --- | --- |
| VirtIO network | `1af4:1000`, `1af4:1041`; modern VirtIO PCI transport. | QEMU DHCP and HTTP payload checks in `tests/nic.mk`. |
| Intel 8254x | Vendor `8086`; devices `100e`, `1015`, `1016`, `1017`, `101e`, `1004`, `100f`, `1010`, `1026`, `1076`, `1077`. | Host match/ring checks and QEMU transfer/link cases in `tests/nic.mk`. |
| Intel 82574 | `8086:10d3`, `8086:10f6`; backend `c/drivers/net/e1000e.c`. | Host completion checks and QEMU transfers in `tests/e1000e.mk`. |
| Intel PCH2/82579 | `8086:1502`, `8086:1503`; backend `c/drivers/net/e1000_pch2.c`. | Production driver with a host register model in `tests/e1000_pch2.mk`. |
| Realtek RTL8139 | `10ec:8139`, `10ec:8138`, `1113:1211`. | Host checks and QEMU DHCP/HTTP cases in `tests/nic.mk`. |
| Realtek RTL8169 family | Inventory contains `10ec:8169`, `10ec:8168`, `10ec:8161`, `10ec:8136`, `1186:4300`; the current probe accepts MAC versions 40, 42 and 44. | Host descriptor, ring and match checks in `tests/nic.mk`. |
| AMD PCnet | `1022:2000`, followed by an Am79C970A silicon-version check. | Checks for descriptor errors and QEMU transfers across ring wrap in `tests/pcnet.mk`. |

RTL8168 identification reads the controller’s XID and selects the G, GU or 8411B startup path. The implementation programs MAC, ePHY and PHY state, publishes link speed and transitions, and masks device interrupts when its storm threshold is exceeded while polling continues (`c/drivers/net/rtl8168_id.c`, `c/drivers/net/rtl8168_hw.c`, `c/drivers/net/rtl8168_phy.c`, `c/drivers/net/rtl8169.c`).

Wi-Fi provides a station protocol implementation and a kernel network adapter. Transport callbacks supply channel selection, complete wireless frames, randomness and time. The station implements passive scanning, Open-System authentication, association, WPA2-PSK with CCMP-128, key exchange, rekeying and replay checks. The adapter publishes the authenticated link through the kernel interface table (`c/drivers/net/wifi/station.h`, `c/drivers/net/wifi/adapter/netdev.c`).

`tests/drivers/wifi/tests.mk` links production station and adapter code with fixtures for an access point generated by a Python cryptographic oracle. Its controls cover authenticated data, key reinstallation and link publication.

## VirtIO devices

The shared transport discovers modern PCI vendor capabilities, negotiates features and constructs split virtqueues. Drivers using transitional PCI identities also require these modern capabilities (`c/drivers/virtio/virtio.c`, `c/drivers/virtio/virtio.h`).

Alongside storage and network backends, the tree contains:

| Device | Implemented behavior | Test coverage |
| --- | --- | --- |
| GPU, `1af4:1050` | Framebuffer backing in RAM, transfer and flush of dirty regions, and a hardware cursor resource and queue. | QEMU boot configuration in `tests/boot/run-test.sh`; transport and DMA checks in `tests/dma.mk`. |
| Entropy, `1af4:1005` | Entropy reads, including short completions. | Nonzero, differing reads and device absence in `tests/virtio_rng.mk`. |
| Balloon, `1af4:1002` | Inflation, deflation, a startup round trip and application of a target available during probing. | Guest frame accounting compared with the host balloon report in `tests/virtio_balloon.mk`. |

These implementations live in `c/drivers/virtio/virtio_gpu.c`, `c/drivers/virtio/virtio_rng.c` and `c/drivers/virtio/virtio_balloon.c`.

## USB

The host-controller contract covers root ports, control transfers, bulk transfers and interrupt input. Backends own transfer mechanics and DMA lifetime; the core owns enumeration, topology and class binding (`c/drivers/usb/usb_hc.h`, `c/drivers/usb/usb_core.c`).

| Component | Implemented path | Test coverage |
| --- | --- | --- |
| xHCI | PCI `0c:03:30`; command, event and transfer rings for enumeration and class traffic. | Input reaching an application with PS/2 disabled in `tests/usb.mk`; transfer and lifecycle fixtures in `tests/usb_hub.mk`. |
| EHCI | PCI `0c:03:20`; high-speed root devices and low/full-speed devices through a high-speed hub’s transaction translator. Interrupt input uses bandwidth admission and a shared periodic schedule. | Fixtures for the production controller, HID traffic and cases with two controllers in `tests/ehci.mk`. |
| USB 2.0 hubs | Downstream port power/reset and topology propagation during enumeration. EHCI also monitors the hub status endpoint for advisory changes. | Host topology and QEMU cases in `tests/usb_hub.mk`. |
| HID | Decoding of report descriptors for keyboards, mice, tablets, gamepads and touchpads, with boot keyboard/mouse fallback. | Composite interfaces and tablet coordinates in `tests/usb_input_extensions.mk`; gamepad/touchpad consumers in `tests/drivers/usb/hid/generic.mk`. |
| Mass storage | SCSI commands over Bulk-Only Transport feeding the block registry. | Controller and media cases in `tests/usb_storage.mk`. |

Keyboard and pointer reports feed the window manager’s input interfaces. Gamepads map to keyboard input; touchpads produce pointer motion, buttons and scrolling (`c/drivers/usb/usb_hid.c`).

Connection changes on a root port queue work on the kernel worker so enumeration transfers can sleep. `tests/usb_hotplug.mk` defines connection and removal checks after boot for both controller backends.

## Graphics drivers

The framebuffer layer prefers VirtIO GPU and falls back to the firmware’s linear framebuffer. Intel, NVIDIA and AMD framebuffer probes associate that surface with a PCI function by checking that its complete physical range fits within one of the function’s memory BARs (`c/kernel/gui/fb/fb.c`).

| Family | Implemented path | Test coverage |
| --- | --- | --- |
| Intel display controllers | Vendor `8086`, display subclass `00` or `02`; ownership of the firmware framebuffer, memory decode and power state checks in `c/drivers/gpu/intel_bootfb.c`. | Controls for passive probing and ownership in `tests/intel_bootfb.mk`. |
| NVIDIA GTX 1050/1050 Ti | Device inventory and framebuffer ownership in `c/drivers/gpu/nvidia_pascal.c`; validation of the pinned firmware bundle and GP107 identity reads in `c/drivers/gpu/nvidia_pascal_accel.c`. | Host probe/firmware checks in `tests/nvidia_pascal.mk`; its QEMU binding fixture uses a synthetic display identity. |
| AMD/ATI display controllers | Vendor `1002`; firmware-framebuffer ownership in `c/drivers/gpu/amd/bootfb.c`. | Host ownership checks and QEMU binding cases in `tests/gpu/amd/bootfb.mk`. |
| AMD RV100 | `1002:5159`; startup fill/copy checks followed by presentation of dirty regions through the legacy 2D engine. | Host engine model, QEMU execution and presentation controls in `tests/gpu/amd/rv100/engine.mk` and `tests/gpu/amd/rv100/present.mk`. |
| AMD Polaris10 | Devices `67c0`, `67c1`, `67c2`, `67c4`, `67c7`, `67d0`, `67df`, `67c8`, `67c9`, `67ca`, `67cc`, `67cf`, `6fdf` under vendor `1002`. Resource inspection, firmware/SMU/SDMA runtime and a diagnostic boot self-test. | Host runtime/resource models in `tests/gpu/amd/polaris/runtime.mk` and `tests/gpu/amd/polaris/resource/gate.mk`; the fixture for boot binding in `tests/gpu/amd/bootfb.mk` stubs the self-test. |

The Polaris identity set comes from `c/drivers/gpu/amd/polaris/pci_id.h`. Its desktop entry validates caller-supplied VRAM resources and the current scanout mapping before starting the runtime and installing a presenter (`c/drivers/gpu/amd/polaris/desktop.c`).

The earlier overview listed VRAM ownership and selection of the security key as blockers for automatic startup of Polaris. In `HWHUD=1` builds, the boot path now surveys six CRTC display heads, inventories scanout and cursor surfaces, and plans an aligned 2 MiB arena. The self-test acquires an ownership lease, validates mappings, selects firmware using the SMU security key, and attempts SMU startup and SDMA0 fill/copy with readback. CPU presentation continues during this diagnostic path (`c/drivers/gpu/amd/bootfb.c`, `c/drivers/gpu/amd/polaris/selftest/selftest.c`).

Polaris VBIOS discovery uses the UEFI loader’s saved ROM image or a matching ACPI VFCT entry. Firmware loading tries the root filesystem and boot modules, checking structure and SHA-256 pins. The opt-out marker on the boot volume disables the self-test (`c/drivers/gpu/amd/polaris/resource/vbios.c`, `c/drivers/gpu/amd/polaris/resource/files.c`, `c/drivers/gpu/amd/polaris/selftest/opt_out.c`).

## Audio drivers

Audio drivers own codec programming, DMA rings and interrupt acknowledgement. The kernel audio layer supplies PCM conversion, resampling and mixing through the contract in `c/kernel/audio/snd.h`.

| Family | Devices and streams |
| --- | --- |
| HDA | PCI class `04:03`; controller selection favors onboard Intel identities `8086:1c20`, `8086:1d20` and `8086:1e20`. Codec discovery supplies analog playback and capture when a usable ADC path exists. Streams use 48 kHz stereo signed 16-bit PCM. |
| Intel AC’97 | `8086:2415`, `8086:2425`, `8086:2445`, `8086:2485`, covering ICH, ICH0, ICH2 and ICH3. Playback and capture use 48 kHz stereo signed 16-bit PCM. |
| Ensoniq ES1370 | `1274:5000`; DAC2 playback, ADC capture and AK4531 codec programming. Its divider produces the reported rate of 48662 Hz. |

Implementations live in `c/drivers/audio/hda.c`, `c/drivers/audio/ac97/pci.c` and `c/drivers/audio/es1370/driver.c`. AC’97 matching derives from `c/drivers/audio/ac97/ac97_models.h`.

HDA confirms D0 transitions before configuring output paths. Its planner routes analog line-out, speaker and headphone pins to the stereo stream. Jack polling applies automatic muting: headphones mute line-outs and speakers, while a connected line-out mutes speakers (`c/drivers/audio/hda_route.c`, `c/drivers/audio/hda_output_paths.c`).

GPU HDA functions receive a codec and sink survey followed by controller teardown. HDA diagnostics report controller identity, routed pins, jack state, readback of the output enable, DMA progress and interrupt mode. `HWHUD=1` builds also provide a one-second 440 Hz boot tone through the mixer and playback path (`c/drivers/audio/hda_status.h`, `c/kernel/audio/boot_test_tone.c`).

`tests/drivers/audio/tests.mk` groups controller/framework checks and QEMU playback capture. `tests/drivers/audio/capture/tests.mk` injects PCM through an external QEMU audio backend and compares guest recordings with supplied samples. Cases with multiple cards exercise HDA playback alongside AC’97 or ES1370 capture. `tests/hda_x79.mk` checks controller selection and ownership after a failed probe.

## Legacy input, console and timers

Legacy PC devices use direct initialization and interrupt entry points.

| Family | Implemented behavior | Source |
| --- | --- | --- |
| PS/2 keyboard | Scancode set 1, US QWERTY translation, extended keys, modifiers and key release. | `c/drivers/char/keyboard.c` |
| PS/2 mouse | Packet decoding and negotiated IntelliMouse wheel packets. | `c/drivers/char/mouse.c` |
| Serial | COM1 at `0x3f8`, 115200 baud, eight data bits, no parity and one stop bit; buffered transmission and polled reception. | `c/drivers/char/serial.c` |
| VGA text | An 80 × 25 console at `0xb8000`, scrolling and tracking of the cursor position. | `c/drivers/char/vga.c` |
| PIC | Interrupt remapping, masks and acknowledgement. | `c/drivers/timer/pic.c` |
| PIT | Periodic interrupts on channel zero at `TIMER_HZ`, defined as 100 Hz. | `c/drivers/timer/pit.c`, `c/drivers/timer/pit.h` |
| RTC | CMOS calendar reads and clock cross-check input. | `c/drivers/timer/rtc.c` |
| HPET | A counter discovered through ACPI, used as a clock source. | `c/drivers/timer/hpet.c` |

The earlier overview gave COM1’s rate as 38400 baud. The current divisor is 1, selecting 115200 baud. Serial output uses a 64 KiB ring and drains ready FIFO space during writes and timer ticks. Overflow produces loss accounting and a notice; panic output and shutdown flushing use bounded synchronous drains (`c/drivers/char/serial.c`).

Clock selection and calibration live in `c/kernel/core/ktime.c`. HPET can supply TSC calibration when PIT calibration fails. Local APIC and I/O APIC implementations live in `c/kernel/cpu/irq/lapic.c` and `c/kernel/cpu/irq/ioapic.c`.

`tests/usb.mk` includes PS/2 coexistence and absent-USB cases. `tests/hpet.mk` combines host parsing/capability checks with QEMU cases that enable or remove HPET.

## Power and platform devices

ACPI power services evaluate firmware methods through uACPI. They discover batteries, external-power devices and thermal zones, and provide shutdown and reset operations. The native backend supplies memory mapping, port I/O, PCI access, interrupts and deferred work (`c/drivers/power/acpi/devices.c`, `c/drivers/power/acpi/native.c`).

Support for embedded controllers connects SystemIO resources described by firmware to EC OperationRegions and deferred GPE queries. Installing the handler for the address space before namespace device initialization lets initialization methods access EC fields (`c/drivers/power/acpi/ec/driver.c`).

The power button driver handles fixed events and notifications from `PNP0C0C` devices. A press queues work that starts a shutdown thread, allowing filesystem draining to run in a context that can sleep (`c/drivers/power/acpi/button.c`).

| Family | Devices and implemented behavior |
| --- | --- |
| Intel PCH SMBus | `8086:1c22`, `8086:1d22`, `8086:1e22` with the SMBus class; DDR3 SPD byte reads at addresses `0x50`–`0x57`. |
| Intel CPU iMC SMBus | `8086:3ca8` and `8086:0ea8`, Sandy Bridge-EP and Ivy Bridge-EP; two controllers accessed through extended PCI configuration space. |
| SMBIOS | Firmware, system, baseboard and DIMM inventory. |
| Intel LPC helpers | ICH9, 6 Series/C200, C600/X79 and 7 Series/C216; early TCO watchdog handling, Super I/O identification and CF9 reset. |
| NCT6779 | Temperatures, fan speeds and voltages from an enabled logical device for the hardware monitor, after ACPI ownership checks. |

SPD decoding checks CRC and reports module geometry and identity. Its capacity figures describe the discovered modules. The CPU iMC reader restores the control register after each byte operation (`c/drivers/platform/intel_pch_smbus.c`, `c/drivers/platform/intel_imc_smbus.c`, `c/drivers/platform/ddr3_spd.c`).

Board integration lives in `c/drivers/board/board.c`, `c/drivers/board/smbios.c` and `c/drivers/board/pch_lpc.c`. NCT6779 transactions preserve selector and bank state; an ACPI namespace scan checks the monitor’s I/O window before access (`c/drivers/board/nct6779.c`, `c/drivers/board/nct_acpi.c`).

CPU power support reads firmware idle and performance tables. Idle selection uses supported MWAIT hints when the CPU provides an APIC timer that always runs, with HLT fallback. EIST ratio control and thermal sampling apply model guards for Intel family 6 models `2d` and `3e`; initialization requests the highest permitted ratio (`c/kernel/cpu/power/cpuidle.c`, `c/kernel/cpu/power/pstate.c`, `c/kernel/cpu/power/thermal.c`, `c/kernel/cpu/power/hardware.h`).

ACPI method and EC interpreter fixtures live in `tests/drivers/power/acpi/test.mk` and `tests/drivers/power/acpi/ec/test.mk`. `tests/power.mk` supplies QEMU shutdown and reboot checks. `tests/x79_chipset.mk` exercises PCH SMBus/SPD behavior through a host controller model.

## Inspecting support

`dev_dump()` prints PCI identities, classes, resources and bindings. Backend messages add storage, network, framebuffer and USB state (`c/drivers/core/device.c`).

`tools/driver_inventory.py` summarizes a saved serial log and retains PCI records, selected backend messages and USB topology. Controller identity accompanies USB addresses because each controller has its own address space.

`make HWHUD=1` enables a hardware panel over the desktop. It displays observations including network link state, AHCI diagnostics, HDA routes and stream progress, board sensors and Polaris survey/self-test status (`Makefile`, `c/kernel/diag/hwhud.c`).

`tests/driver_expansion.mk` groups checks under `test-driver-host` and `test-driver-os`. Family fragments define their assertions, controls and environment requirements.

## Limitations

- Host models and QEMU checks establish behavior in their named environments. Physical compatibility requires evidence for the controller, board and configuration in use (`TRANSPARENCY.md`).
- DMA uses RAM physical addresses without an IOMMU. The driver contract has no suspend/resume callbacks (`c/drivers/core/dma.h`, `c/drivers/core/driver.h`).
- AHCI omits ATAPI and port multipliers. AHCI, VirtIO SCSI and USB storage require 512-byte logical blocks. USB discovery follows root mounting, so the current startup path cannot select a USB root disk (`c/drivers/block/ahci.c`, `c/drivers/virtio/virtio_scsi.c`, `c/drivers/usb/usb_storage.c`, `c/kernel/init/kmain.c`).
- RTL8168 startup omits external PHY firmware patches. The 82579 backend rejects configurations with managed firmware. Later Intel LAN families and RTL8125 require additional backends (`c/drivers/net/rtl8168_phy.c`, `c/drivers/net/e1000_pch2.c`, `c/drivers/net/net_ids.inc`).
- Wi-Fi has no physical radio transport or firmware startup. WPA3, protected management frames and roaming remain unsupported (`c/drivers/net/wifi/README.md`).
- USB lacks UHCI/OHCI backends, downstream hub hotplug and isochronous transfers. EHCI also lacks companion routing and interrupt endpoints with high bandwidth. Mass storage supports Bulk-Only Transport, excluding UAS (`c/drivers/usb/usb_core.c`, `c/drivers/usb/usb_hub.c`, `c/drivers/usb/ehci.c`, `c/drivers/usb/xhci.c`, `c/drivers/usb/usb_storage.c`).
- Intel and GTX 1050 display paths retain the firmware framebuffer. NVIDIA command submission remains unavailable. Automatic Polaris execution serves the diagnostic self-test; automatic desktop acceleration and modesetting remain unavailable (`c/drivers/gpu/intel_bootfb.c`, `c/drivers/gpu/nvidia_pascal_accel.c`, `c/drivers/gpu/amd/bootfb.c`).
- The kernel random generator does not consume VirtIO entropy. Balloon target changes after probing have no periodic caller (`c/drivers/virtio/virtio_rng.c`, `c/drivers/virtio/virtio_balloon.h`).
- Audio selects the first successfully registered playback and capture devices. Runtime selection UI, USB Audio, VirtIO Sound and HDMI/DP playback remain absent (`c/drivers/audio/README.md`, `c/drivers/audio/hda.c`).
- ACPI power services omit suspend/resume. CPU ratio control has no scheduler load governor. EC support requires the restricted SystemIO resource contract documented in `c/drivers/power/acpi/ec/README.md` (`c/drivers/power/README.md`, `c/kernel/cpu/power/pstate.c`).
- NCT6779 ownership checking covers SystemIO OperationRegions present during the namespace scan. Later dynamic regions and undocumented SMM access remain outside that check (`c/drivers/board/nct_acpi.c`).

## Source map

| File | What it contains |
| --- | --- |
| `c/drivers/core/driver.h`, `c/drivers/core/device.c` | Device contracts, matching, binding, BAR mapping, quiescence and diagnostics. |
| `c/drivers/core/dma.h`, `c/drivers/core/irq.c` | DMA ownership contract and interrupt callback lifetime. |
| `c/kernel/pci/pci.c`, `c/kernel/pci/pci_init.c` | PCI enumeration and configuration access. |
| `c/kernel/pci/pci_msi.c`, `c/kernel/pci/pci_prt_aml.c` | PCI interrupts, AML routing and rerouting. |
| `c/drivers/core/qemu_edu.c` | Example PCI driver and a self-test for interrupt delivery. |
| `c/kernel/init/kmain.c`, `c/kernel/init/power.c` | Initialization order and reboot policy. |
| `c/drivers/block/blkdev.c`, `c/drivers/block/part.c` | Block requests, disk registry, partitions and root selection. |
| `c/drivers/block/ata.c`, `c/drivers/block/ahci.c`, `c/drivers/block/nvme.c` | ATA, SATA and NVMe backends. |
| `c/drivers/net/netdev.c`, `c/drivers/net/net_ids.inc` | Network interfaces, probe priority and Ethernet match inventory. |
| `c/drivers/net/rtl8169.c`, `c/drivers/net/rtl8168_id.c`, `c/drivers/net/rtl8168_hw.c`, `c/drivers/net/rtl8168_phy.c` | Realtek revision checks and controller startup. |
| `c/drivers/net/wifi/station.h`, `c/drivers/net/wifi/adapter/netdev.c` | Wi-Fi station contract and kernel adapter. |
| `c/drivers/virtio/virtio.c`, `c/drivers/virtio/virtio.h` | Shared PCI transport and split virtqueues. |
| `c/drivers/virtio/virtio_blk.c`, `c/drivers/virtio/virtio_scsi.c`, `c/drivers/virtio/virtio_net.c` | VirtIO storage and Ethernet backends. |
| `c/drivers/virtio/virtio_gpu.c`, `c/drivers/virtio/virtio_rng.c`, `c/drivers/virtio/virtio_balloon.c` | Display, entropy and balloon devices. |
| `c/drivers/usb/usb_hc.h`, `c/drivers/usb/usb_core.c`, `c/drivers/usb/usb_bind.c` | USB controller contract, enumeration and interface binding. |
| `c/drivers/usb/xhci.c`, `c/drivers/usb/ehci.c` | USB host-controller backends. |
| `c/drivers/usb/usb_hid.c`, `c/drivers/usb/usb_hub.c`, `c/drivers/usb/usb_storage.c` | Input, hub and storage classes. |
| `c/kernel/gui/fb/fb.c` | Framebuffer selection, presentation and cursor integration. |
| `c/drivers/gpu/intel_bootfb.c`, `c/drivers/gpu/nvidia_pascal.c`, `c/drivers/gpu/amd/bootfb.c` | Ownership of the firmware framebuffer and GPU probing. |
| `c/drivers/gpu/nvidia_pascal_accel.c`, `c/drivers/gpu/amd/accel.c` | NVIDIA prerequisite checks and RV100 presentation integration. |
| `c/drivers/gpu/amd/polaris/pci_id.h`, `c/drivers/gpu/amd/polaris/desktop.c` | Polaris identities and runtime entry. |
| `c/drivers/gpu/amd/polaris/resource/vbios.c`, `c/drivers/gpu/amd/polaris/resource/files.c` | VBIOS discovery and pinned firmware loading. |
| `c/drivers/gpu/amd/polaris/selftest/selftest.c`, `c/drivers/gpu/amd/polaris/selftest/opt_out.c` | Staged diagnostic execution and handling of the opt-out. |
| `c/kernel/audio/snd.h`, `c/drivers/audio/hda.c`, `c/drivers/audio/ac97/pci.c`, `c/drivers/audio/es1370/driver.c` | Audio contract and controller integration. |
| `c/drivers/audio/hda_route.c`, `c/drivers/audio/hda_output_paths.c`, `c/drivers/audio/hda_status.h` | HDA routing, automatic muting and diagnostics. |
| `c/kernel/audio/boot_test_tone.c` | Generation and playback of the diagnostic boot tone. |
| `c/drivers/char/keyboard.c`, `c/drivers/char/mouse.c`, `c/drivers/char/serial.c`, `c/drivers/char/vga.c` | Legacy input and console devices. |
| `c/drivers/timer/pic.c`, `c/drivers/timer/pit.c`, `c/drivers/timer/rtc.c`, `c/drivers/timer/hpet.c` | Legacy interrupts and timer hardware. |
| `c/kernel/core/ktime.c`, `c/kernel/cpu/irq/lapic.c`, `c/kernel/cpu/irq/ioapic.c` | Clock calibration and APIC support. |
| `c/drivers/power/acpi/devices.c`, `c/drivers/power/acpi/native.c`, `c/drivers/power/acpi/ec/driver.c`, `c/drivers/power/acpi/button.c` | ACPI services, embedded controllers and power buttons. |
| `c/drivers/platform/intel_pch_smbus.c`, `c/drivers/platform/intel_imc_smbus.c`, `c/drivers/platform/ddr3_spd.c` | SMBus access and DDR3 SPD inventory. |
| `c/drivers/board/board.c`, `c/drivers/board/smbios.c`, `c/drivers/board/pch_lpc.c`, `c/drivers/board/nct6779.c`, `c/drivers/board/nct_acpi.c` | Board identity, watchdog/reset helpers and sensor ownership. |
| `c/kernel/cpu/power/cpuidle.c`, `c/kernel/cpu/power/pstate.c`, `c/kernel/cpu/power/thermal.c`, `c/kernel/cpu/power/hardware.h` | CPU idle policy, ratio control, thermal sampling and model guards. |
| `Makefile`, `linker.ld`, `c/kernel/diag/hwhud.c` | Driver compilation, registration and integration of the hardware panel. |
| `tools/driver_inventory.py`, `tests/driver_expansion.mk` | Inventory from a serial log and driver test groups. |
| `TRANSPARENCY.md` | Interpretation of implementation and validation claims. |

## See also

- [Device and driver model](../architecture/device-model.md)
- [VirtIO drivers](virtio.md)
- [Storage drivers](storage.md)
- [Ethernet drivers](network.md)
- [Wi-Fi support](wifi.md)
- [USB](usb.md)
- [GPU drivers](gpu.md)
- [Audio drivers](audio.md)
- [Keyboard, mouse, serial and VGA](input-and-console.md)
- [Timer hardware](timers.md)
- [Power and platform drivers](power-and-platform.md)
- [Running on real hardware](../getting-started/real-hardware.md)
- [Testing](../architecture/testing.md)
