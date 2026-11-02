# Device and driver model

LogitOS records PCI functions in a device registry and binds them to drivers through match tables and probe callbacks. Shared services provide configuration access, BAR resources, interrupt routing, and DMA allocation and ownership tracking. Drivers publish the interfaces consumed by storage, networking, USB, and other subsystems. The main contracts are in `c/drivers/core/driver.h` and `c/drivers/core/dma.h`.

## Devices and subsystem interfaces

The model represents hardware identity, driver ownership, and subsystem operations through different objects.

| Object | Responsibility |
|---|---|
| `struct device` | PCI address, identifiers, class codes, BAR resources, capability offsets, interrupt wiring, and binding state. |
| `struct dev_match` | Vendor, device, class, subclass, and programming interface filters, plus a private data tag. |
| `struct driver` | Name, bus type, match table, probe callback, optional removal callback, and optional display quiescence callback. |
| `struct dma_device` | DMA address constraints and lists of allocated buffers and mappings. |

The first three structures are defined in `c/drivers/core/driver.h`; the DMA context is defined in `c/drivers/core/dma.h`. The registry uses a fixed array, giving device entries stable addresses that drivers can retain in controller state (`c/drivers/core/device.c`).

Subsystem interfaces describe the operations available after initialization. For example, `struct netdev` provides Ethernet transmission, receive polling, and interrupt callbacks in `c/drivers/net/netdev.h`. AHCI calls `blk_register()` for discovered disks, so one PCI controller can supply several block devices (`c/drivers/block/ahci.c`, `c/drivers/block/blkdev.c`).

USB has a further binding layer. PCI drivers own host controllers, while `struct usb_driver` matches individual USB interfaces by class, subclass, and protocol. Each interface receives its own binding and private state, allowing several class drivers to serve a composite device. Probe failure affects the interface being tried (`c/drivers/usb/usb.h`, `c/drivers/usb/usb_bind.c`).

USB class drivers can supply a `service` callback for work that sleeps. The core runs these callbacks on a worker while holding the enumeration domain and a controller reference. Removal uses the same domain, preserving class state across control transfers (`c/drivers/usb/usb_core.c`).

## Driver registration and binding

A PCI driver supplies a match table terminated by `DEV_MATCH_END`. `DEV_ANY` and `DEV_ANYC` mark wildcard fields. The helpers in `c/drivers/core/driver.h` cover common combinations:

| Helper | Match |
|---|---|
| `DEV_MATCH_VD` | Vendor and device identifiers. |
| `DEV_MATCH_VD_DATA` | Vendor and device identifiers with a private data tag. |
| `DEV_MATCH_CLASS` | Class and subclass. |
| `DEV_MATCH_PROGIF` | Class, subclass, and programming interface. |
| `DEV_MATCH_VCLASS` | Vendor, class, and subclass. |

`DRIVER_DECLARE` emits a driver pointer into the kernel’s `logit_drivers` linker section. `linker.ld` retains this section between `__start_logit_drivers` and `__stop_logit_drivers`, inside the kernel image’s reserved extent. The source discovery rule in `Makefile` includes C files under `c/drivers`.

`dev_probe_all()` registers the built-in pointers and visits unbound devices. For each device, it tries matching drivers in registration order. A probe returning zero claims the device; a nonzero result advances to the next candidate. Registering a device-specific driver before a generic class driver gives it the first opportunity to bind (`c/drivers/core/device.c`).

Before calling the probe, the binder sets `dev->match`, exposing the selected entry’s `data` tag. The driver stores controller state through `dev_set_drvdata()`. A declining probe handles cleanup of its allocations and hardware setup; the binder then clears the match and private-data fields.

A short registry lock protects device publication and the driver list. Per-device callback ownership serializes probing and removal. Callbacks run outside the registry lock, allowing them to allocate or sleep. Later probe passes revisit devices that remain unbound (`c/drivers/core/device.c`).

The module loader walks a loaded object’s `logit_drivers` section, calls `driver_register()`, and invokes `dev_probe_all()`. Module drivers enter the same registration list (`c/kernel/module/modload.c`).

`c/drivers/core/qemu_edu.c` is a compact example: it declares a match table, checks BAR liveness, stores private state, requests interrupts, and triggers delivery checks.

## Boot integration

`pci_init()` runs before framebuffer, storage, and network initialization. The general binding pass follows `smp_init()`, after local APIC and I/O APIC setup has been attempted (`c/kernel/init/kmain.c`).

Boot brings up several services before that pass:

| Service | Early initialization and later binding |
|---|---|
| NVMe and AHCI | Publish bindings during storage initialization so removal reaches their cleanup callbacks. AHCI initialization runs through the block layer. |
| VirtIO block and SCSI | Initialize before root mounting; later probes bind the initialized transports. |
| VirtIO GPU | Starts through framebuffer initialization; its later probe binds the existing transport. |
| Networking | Tries devices in NIC priority order, records bindings, and registers interfaces before SMP initialization. |

These paths are implemented in `c/drivers/block/nvme.c`, `c/drivers/block/ahci.c`, `c/drivers/block/blkdev.c`, `c/drivers/virtio/virtio_blk.c`, `c/drivers/virtio/virtio_scsi.c`, `c/drivers/virtio/virtio_gpu.c`, `c/kernel/gui/fb/fb.c`, and `c/drivers/net/netdev.c`.

During SMP initialization, `netdev_irq_route()` connects registered PCI NICs to the shared interrupt machinery. The previous description, “This pass requests INTx,” applies to NICs with `msi_ok` clear. NICs setting that field request MSI with INTx fallback; RTL8169 sets it. The general probe pass subsequently skips these existing bindings (`c/kernel/cpu/smp/smp.c`, `c/drivers/net/netdev.c`, `c/drivers/net/rtl8169.c`).

After the binding pass, `ahci_irq_attach()` attempts MSI attachment and checks delivery with an IDENTIFY command. AHCI uses polling during root mounting and retains polling paths for subsequent completion handling (`c/kernel/init/kmain.c`, `c/drivers/block/ahci.c`).

## PCI discovery and resources

`pci_init()` searches ACPI MCFG entries for a usable segment-zero ECAM window. Configuration accesses use ECAM for buses covered by that window and the legacy `0xCF8` and `0xCFC` ports elsewhere. ECAM mappings are created one bus at a time, reducing page-table allocation for unused portions of the aperture (`c/kernel/pci/pci_init.c`, `c/kernel/pci/pci.c`).

Prepare configuration access in task or probe context before using it from an interrupt handler: first access to an ECAM bus can allocate mappings. Configuration transactions use a short lock. Byte and word writes use their corresponding access widths, preserving adjacent PCI Status bits that clear when written with one (`c/kernel/pci/pci.h`, `c/kernel/pci/pci.c`).

Enumeration starts at bus zero. The multifunction header bit selects how many functions to inspect in a slot. PCI bridge secondary-bus registers lead the scan downstream, and a visited-bus bitmap terminates repeated paths.

Each discovered function contributes identity, class information, interrupt line and pin, BAR resources, and capability offsets to `dev_add()`. Capability walkers track visited offsets to terminate cyclic lists. `pci_cap_next()` supports repeated capability identifiers, including VirtIO’s vendor capabilities (`c/kernel/pci/pci.c`).

BAR sizing temporarily writes all ones to obtain the implemented address mask. `pci_bar_probe()` disables address decoding and verifies that transition before writing the BAR. It also checks restoration of the BAR and PCI Command register before publishing the resource. This protects other devices from accesses redirected by a temporary BAR value.

Drivers consume `dev->res`. `dev_bar_map()` maps a memory BAR at its physical address with writable, uncached attributes. I/O BAR entries supply port bases. `dev_bar_map_window()` maps a bounded byte range after checking bounds and arithmetic. Because mappings cover whole pages, callers sharing a BAR with scanout must keep every touched page outside the active framebuffer. This preserves its cache policy during diagnostic access (`c/drivers/core/driver.h`, `c/drivers/core/device.c`).

`dev_enable_checked()` enables decoding for address spaces represented by usable BARs and controls bus mastering. Passing zero clears bus mastering, allowing register setup before DMA authorization; passing one enables it. The helper verifies PCI Command readback. Drivers establish controller readiness through their stop, reset, and queue protocols (`c/drivers/core/device.c`).

## Interrupt routing and dispatch

`dev_irq_request()` takes a device, callback, argument, and diagnostic name. Its default preference is MSI-X, MSI, then INTx. It records the selected mode and vector. Message interrupts target the requesting CPU’s local APIC (`c/kernel/pci/pci_msi.c`).

Setup quiesces inherited message sources and verifies address, data, and enable-state readbacks. An unconfirmed message-source shutdown enters a halt path during setup, protecting vector ownership during fallback.

The previous page described INTx routing “without `_PRT` evaluation or bridge interrupt swizzling in this path.” Current routing prefers evaluated ACPI `_PRT`, followed by supported Intel PCH RCBA routing, then the firmware Interrupt Line field with MADT overrides. Logs record disagreements between AML and RCBA (`c/kernel/pci/pci_msi.c`).

The AML resolver follows enumerated bridges and swizzles the interrupt pin while walking upstream. It accepts direct GSI entries and resolves interrupt links through `_CRS`. When allocation is needed, it selects a complete `_PRS` configuration, applies `_SRS`, and checks resource and device-status readback (`c/kernel/pci/pci_prt_aml.c`). The I/O APIC layer selects the controller covering the chosen GSI (`c/kernel/cpu/irq/ioapic.c`, `c/kernel/cpu/irq/ioapic_map.c`).

When SMP has selected I/O APIC routing, the ACPI startup hook selects that interrupt model before namespace initialization runs `_INI`. After successful initialization, `pci_prt_aml_ready()` installs the resolver and revisits existing INTx bindings. Rerouting takes the device’s binding ownership, retires the old route, installs the new route, and invokes the callback to collect pending controller work. A failed installation attempts to restore the previous route (`c/kernel/pci/pci_prt_aml.c`, `c/kernel/pci/pci_msi.c`, `c/kernel/init/power.c`).

The dynamic-vector layer installs gates in the shared IDT and supplies entry stubs that preserve general registers and x87/SSE state through `FXSAVE` and `FXRSTOR`. Dispatch records CPU-local kernel nesting and pins the callback and argument under a short lock. The callback runs outside that lock; its reference remains held through end-of-interrupt processing. Device handlers acknowledge controller causes, while the interrupt core sends the APIC or PIC acknowledgement (`c/drivers/core/irq.c`).

INTx functions sharing a GSI share one dispatcher and vector. The dispatcher calls each active member’s handler, which checks its own controller status (`c/kernel/pci/pci_msi.c`).

## Removal and reboot

`dev_unbind()` releases interrupts before invoking the driver’s removal callback. If source suppression fails, it preserves the binding for a retry. Successful release drains callback references, keeping callback arguments alive until dispatch finishes (`c/drivers/core/device.c`).

INTx removal suppresses the departing function’s source while other members continue using the shared route. Final removal masks the route and checks I/O APIC delivery state and Remote IRR. An uncertain route remains quarantined (`c/kernel/pci/pci_msi.c`, `c/kernel/cpu/irq/ioapic.c`).

After callback draining, vector reuse waits for every online CPU to check its local APIC for pending or active delivery. Cleanup IPIs and end-of-interrupt hooks advance these checks. Callback storage can be released while the vector number remains reserved (`c/drivers/core/irq.c`).

Reboot invokes `dev_quiesce_all()`, which visits devices in reverse registry order, unbinds eligible drivers, and checks PCI bus mastering. Storage remains bound and contributes to the unconfirmed-DMA count when its Bus Master bit is set. Display drivers can provide `quiesce` to confirm engine idleness while keeping the shutdown display available. The reboot path uses this result to choose permitted reset fallbacks (`c/drivers/core/device.c`, `c/kernel/init/power.c`).

## DMA addresses and ownership

A driver initializes `struct dma_device` with an address mask and configures alignment, boundary, maximum segment size, and segment-count constraints. CPU pointers address kernel mappings; the wrapped `dma_addr_t` type carries addresses written into hardware descriptors (`c/drivers/core/dma.h`).

| Operation | Result |
|---|---|
| `dma_alloc_coherent()` | Physically contiguous, zeroed storage with a CPU mapping and DMA address. |
| `dma_map_kernel()` | A pinned mapping of existing kernel RAM, represented by scatter/gather segments. A bounce allocation is attempted when the direct layout fails device constraints. |

Submission marks a resource as owned by the device and returns a token. After observing hardware completion, the driver supplies that token to the completion operation. The API checks the token and ownership state. Transfer direction controls bounce-buffer copying; receive completion copies the reported valid bytes back to the caller. `dma_wmb()` and `dma_rmb()` provide publication and completion ordering (`c/drivers/core/dma.c`).

Free and unmap operations check resource state. After hardware acknowledges a stop or reset, the driver calls `dma_device_quiesced()` to invalidate outstanding tokens and permit resource release. `dma_device_quarantine()` blocks new submissions and retains resources recorded as owned by the device when shutdown is uncertain.

The driver supplies the hardware acknowledgement behind these transitions. Allocate and map in task or probe context, and retain caller buffers throughout device ownership (`c/drivers/core/dma.h`).

## Driver inventory

The principal implementations and discovery paths are:

| Area | Implementations |
|---|---|
| Storage | ATA PIO in `c/drivers/block/ata.c`; NVMe and AHCI in `c/drivers/block/nvme.c` and `c/drivers/block/ahci.c`; VirtIO block and SCSI in `c/drivers/virtio/virtio_blk.c` and `c/drivers/virtio/virtio_scsi.c`. |
| Ethernet | VirtIO net, e1000, e1000e, e1000-pch2, RTL8139, RTL8169, and PCnet. Driver declarations and PCI matches are in `c/drivers/net/netdev.c` and `c/drivers/net/net_ids.inc`. |
| Wi-Fi | Station protocol code in `c/drivers/net/wifi/station.c` and the network adapter in `c/drivers/net/wifi/adapter/netdev.c`. |
| USB | xHCI integration in `c/drivers/usb/usb_core.c` and EHCI in `c/drivers/usb/ehci.c`; hub, HID, and mass-storage classes in `c/drivers/usb/usb_hub.c`, `c/drivers/usb/usb_hid.c`, and `c/drivers/usb/usb_storage.c`. |
| Display | VirtIO GPU in `c/drivers/virtio/virtio_gpu.c`; firmware framebuffer bindings in `c/drivers/gpu/intel_bootfb.c`, `c/drivers/gpu/amd/bootfb.c`, and `c/drivers/gpu/nvidia_pascal.c`. |
| Audio | HDA in `c/drivers/audio/hda.c`, Intel AC97 in `c/drivers/audio/ac97/pci.c`, and ES1370 in `c/drivers/audio/es1370/driver.c`. |
| Other PCI devices | VirtIO RNG and balloon in `c/drivers/virtio/virtio_rng.c` and `c/drivers/virtio/virtio_balloon.c`; Intel PCH DDR3 SPD inventory in `c/drivers/platform/intel_pch_smbus.c`; EDU in `c/drivers/core/qemu_edu.c`. |
| Board | SMBIOS, PCH LPC, TCO watchdog, and Super I/O integration through `c/drivers/board/board.c`; memory-controller SPD in `c/drivers/platform/intel_imc_smbus.c`; hardware monitoring in `c/drivers/board/nct6779.c` and `c/drivers/board/nct_acpi.c`. |
| ACPI devices | Battery and thermal queries in `c/drivers/power/acpi/devices.c`, power-button handling in `c/drivers/power/acpi/button.c`, and embedded-controller discovery in `c/drivers/power/acpi/ec/driver.c`. |
| Input and console | PS/2 keyboard and mouse, serial, and VGA text output in `c/drivers/char/keyboard.c`, `c/drivers/char/mouse.c`, `c/drivers/char/serial.c`, and `c/drivers/char/vga.c`. |
| Timers and legacy interrupts | PIT, HPET clocksource, RTC, and PIC implementations in `c/drivers/timer/pit.c`, `c/drivers/timer/hpet.c`, `c/drivers/timer/rtc.c`, and `c/drivers/timer/pic.c`. |

Match tables establish probe eligibility. Ethernet tables name device identifiers because controller register protocols vary. xHCI matches the controller class and programming interface. Firmware framebuffer probes also check that the complete active framebuffer lies within a memory BAR of the candidate display function.

The AMD framebuffer path includes Polaris resource discovery and, with `HWHUD=1`, a display survey and a GPU self-test using an ownership lease (`c/drivers/gpu/amd/bootfb.c`). HDA’s `hda-display` binding surveys GPU audio codecs and pins, then releases controller resources (`c/drivers/audio/hda.c`).

## Inventory and validation

After binding, boot calls `dev_dump()`, `dma_report("boot")`, and `hwreport_dump()`. The device dump includes PCI addresses, identifiers, class triples, bound drivers, capabilities, interrupt modes, vectors, and BAR resources (`c/kernel/init/kmain.c`, `c/drivers/core/device.c`).

`tools/driver_inventory.py` summarizes an existing serial log. Its positional argument selects the log and `--json` selects structured output. It retains the latest parsed device record for each PCI address, selected backend messages, the presence of `LOGIT_BOOT_OK`, and topology fields from `USB_DEV` records. Binding state comes from the driver field; categories come from class codes. `--profile x79` attaches a requested profile with `identified_by_log` set to false.

The tree defines these validation entry points:

| Target | Scope |
|---|---|
| `make test-pci` | Synthetic configuration space, BAR sizing, capabilities, enumeration, binding, and interrupt setup logic (`Makefile`). |
| `make test-devmodel` | Two QEMU configurations covering EDU MSI and INTx delivery, legacy configuration access, ECAM, and USB-controller discovery behind a bridge (`tests/boot/run-devmodel-test.sh`). |
| `make test-pci-intx-host` | Shared dispatch, callback lifetime, vector retirement, and I/O APIC transactions, with prerequisite negative controls (`tests/pci_intx.mk`). |
| `make test-pci-intx-guest` | Shared INTx delivery and stale-route protection, with prerequisite negative controls (`tests/pci_intx_guest.mk`). |
| `make test-pci-bar-safety-host` | BAR sizing and PCI Command restoration checks, including negative variants (`tests/pci_bar_safety.mk`). |
| `make test-dma` | DMA ownership, mapping, block, and driver checks with negative controls (`tests/dma.mk`). |
| `make BUILD=build-device-model WIDEVERIFY=1 DMA_CAPTURE_VERIFY=1 test-dma-os` | DMA guest acceptance across firmware and memory configurations, driver exercises, and shutdown checks (`tests/dma.mk`). |

## Limitations

- PCI enumeration runs once from bus zero, uses segment zero, and sizes firmware-assigned BARs. PCI hotplug rescanning and driver suspend/resume hooks are absent; `DEV_BUS_PLATFORM` is reserved (`c/kernel/pci/pci.c`, `c/drivers/core/driver.h`).
- The registry holds 64 devices. The dynamic interrupt pool contains 32 vectors, and MSI/MSI-X setup uses one vector per function. Message destinations require APIC IDs below `0xff` (`c/drivers/core/device.c`, `c/drivers/core/irq.h`, `c/kernel/pci/pci_msi.c`).
- DMA addresses are physical RAM addresses without IOMMU translation. Streaming mappings accept kernel RAM and have a 1 MiB size limit (`c/drivers/core/dma.h`, `c/drivers/core/dma.c`).
- Module unloading is unavailable because registered driver pointers remain in the registry (`c/kernel/module/modload.c`).
- USB hub change bitmaps provide diagnostics; downstream hub hotplug handling remains absent (`c/drivers/usb/usb_hub.c`).
- The Wi-Fi station adapter requires a radio transport; the physical Intel transport remains unimplemented (`c/drivers/net/wifi/README.md`).
- GPU HDA bindings survey codecs and pins; HDMI/DP playback awaits GPU display support. Polaris self-test and memory-controller setup carry pending X79-VG4 hardware verification notes (`c/drivers/audio/hda.c`, `c/drivers/gpu/amd/polaris/memory/gmc_aperture.h`).
- Registry bindings and boot logs alone do not establish physical operation. Legacy paths can operate alongside unbound PCI entries (`tools/driver_inventory.py`, `TRANSPARENCY.md`).

## Source map

| File | What it contains |
|---|---|
| `c/drivers/core/driver.h` | Device, resource, match, driver, and interrupt contracts. |
| `c/drivers/core/device.c` | Registration, binding, removal, BAR mapping, dumps, and reboot quiescence. |
| `c/drivers/core/irq.h`, `c/drivers/core/irq.c` | Dynamic vectors, entry stubs, dispatch references, and vector quarantine. |
| `c/drivers/core/dma.h`, `c/drivers/core/dma.c` | DMA constraints, allocations, mappings, tokens, and resource lifetime. |
| `c/kernel/pci/pci.h`, `c/kernel/pci/pci.c`, `c/kernel/pci/pci_init.c` | Configuration access, MCFG selection, enumeration, capabilities, and BAR sizing. |
| `c/kernel/pci/pci_msi.c` | MSI-X, MSI, shared INTx, rerouting, and teardown. |
| `c/kernel/pci/pci_prt_aml.c` | ACPI interrupt-model selection, routing tables, bridge swizzling, and interrupt links. |
| `c/kernel/cpu/irq/ioapic.c`, `c/kernel/cpu/irq/ioapic_map.c` | GSI ownership, routing, and controller delivery draining. |
| `c/kernel/init/kmain.c`, `c/kernel/cpu/smp/smp.c`, `c/kernel/init/power.c` | Boot ordering, NIC routing, AML startup, and reset policy. |
| `c/drivers/net/netdev.h`, `c/drivers/net/netdev.c`, `c/drivers/net/net_ids.inc` | Network interfaces, early binding, interrupt policy, and PCI matches. |
| `c/drivers/block/blkdev.c`, `c/drivers/block/ahci.c`, `c/drivers/block/nvme.c` | Block publication and early storage bindings. |
| `c/drivers/usb/usb.h`, `c/drivers/usb/usb_bind.c`, `c/drivers/usb/usb_core.c` | Interface binding, class callbacks, workers, and removal ownership. |
| `c/kernel/module/modload.c` | Module driver registration and unload refusal. |
| `c/drivers/core/qemu_edu.c` | Example PCI driver and interrupt-delivery checks. |
| `linker.ld`, `Makefile` | Driver-section retention, source discovery, and core validation targets. |
| `tools/driver_inventory.py` | Serial-log inventory parser. |
| `tests/boot/run-devmodel-test.sh`, `tests/pci_intx.mk`, `tests/pci_intx_guest.mk` | Enumeration and interrupt validation entry points. |
| `tests/pci_bar_safety.mk`, `tests/dma.mk` | BAR safety and DMA validation entry points. |
| `TRANSPARENCY.md` | Project rules for interpreting implementation and test evidence. |

## See also

- [Driver overview](../drivers/overview.md)
- [PCI](../kernel/pci.md)
- [Local APIC and I/O APIC](../kernel/apic.md)
- [Memory model](memory-model.md)
- [Kernel modules](../kernel/modules.md)
- [Running on real hardware](../getting-started/real-hardware.md)
- [Testing](testing.md)