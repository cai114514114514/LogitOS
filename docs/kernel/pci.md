# PCI

The PCI layer discovers hardware functions, accesses configuration space, sizes Base Address Registers (BARs), and connects device interrupts through MSI-X, MSI or legacy INTx. Enumeration publishes `struct device` records for driver matching. The bus implementation lives in `c/kernel/pci/`; registration, binding and resource helpers live in `c/drivers/core/`.

## Initialisation and enumeration

`pci_init()` selects configuration access through `pci_ecam_init()` and calls `pci_enumerate()`. Boot enumerates PCI before storage and network initialisation, then calls `dev_probe_all()` after `smp_init()` has established the interrupt infrastructure. See `c/kernel/pci/pci_init.c` and `c/kernel/init/kmain.c`.

AHCI illustrates this ordering. Early storage access uses polling. After driver binding, boot calls `ahci_irq_attach()`, which requests MSI and checks delivery with an IDENTIFY command. Its completion path uses polling when MSI setup or the delivery check fails. See `c/drivers/block/ahci.c`.

Enumeration starts at bus 0. `scan_bus()` examines function 0 in each of 32 slots, treating vendor ID `0xFFFF` as absent. The multifunction header bit determines whether it checks one function or all eight. For a type 1 bridge, `scan_func()` follows the secondary bus number. A visited-bus bitmap terminates cycles. These functions are in `c/kernel/pci/pci.c`.

Each discovered function produces a device record:

| Information | Recorded fields |
|---|---|
| Address | Segment, bus, slot, function and a name such as `0000:02:00.0` |
| Identity | Vendor and device IDs, revision, class, subclass and programming interface |
| Header | Header type and subsystem IDs for type 0 endpoints |
| Resources | BAR bases, sizes and flags |
| Capabilities | First MSI, MSI-X, PCI Express and vendor capability offsets |
| Interrupts | Firmware interrupt line and pin, with initial ownership state |

The scanner probes six BAR positions for type 0 endpoints and two for type 1 bridges. A 64-bit memory BAR consumes two positions. It publishes the record through `dev_add()`. The record and resource definitions are in `c/drivers/core/driver.h`.

`dev_find_class()` and `dev_find_id()` provide registry lookup. During binding, matching drivers are tried in registration order; the first probe returning zero owns the function. The fixed registry also gives device records stable addresses throughout their lifetime. See `c/drivers/core/device.c`.

The older comment beside `pci_find()` in `c/kernel/pci/pci.h` describes a lookup of function 0 on bus 0. The implementation first searches the registry, including functions found behind bridges, then falls back to checking function 0 of each slot on bus 0. Success requests I/O decoding, memory decoding and bus mastering. New drivers should use device records and `dev_enable_checked()`.

## Configuration space access

The public accessors take a bus, slot, function and byte offset:

| Width | Read | Write |
|---|---|---|
| 32 bits | `pci_cfg_read()` | `pci_cfg_write()` |
| 16 bits | `pci_cfg_read16()` | `pci_cfg_write16()` |
| 8 bits | `pci_cfg_read8()` | `pci_cfg_write8()` |

The 32-bit accessors align the offset down to a four-byte boundary; the 16-bit accessors align it down to a two-byte boundary. Declarations are in `c/kernel/pci/pci.h`, with implementations in `c/kernel/pci/pci.c`.

Configuration transactions use one of two mechanisms:

| Mechanism | Addressing | Register range per function |
|---|---|---|
| ECAM | Memory aperture described by ACPI MCFG | `0x000`–`0xFFF` |
| Legacy ports | Address at `0xCF8`, data at `0xCFC` and its byte lanes | `0x00`–`0xFF` |

`acpi_mcfg_entry()` exposes MCFG allocation entries. `pci_ecam_init()` selects the first entry for segment zero that `pci_ecam_set()` accepts. See `c/kernel/cpu/acpi/acpi.c` and `c/kernel/pci/pci_init.c`.

Within that aperture, configuration addresses use the MCFG base plus the bus shifted by 20 bits, slot shifted by 15 bits, function shifted by 12 bits and register offset. The base refers to bus zero even when the advertised range starts at a higher bus. Buses outside the selected range use the legacy ports.

ECAM access maps each bus on first use as a writable, uncached, identity-mapped 1 MiB window. This avoids allocating page tables for unused buses. First access can allocate memory and sleep, so drivers must prepare the bus in task or probe context before accessing configuration space from an interrupt handler.

A short configuration lock serialises transactions across CPUs and preserves local interrupt state. ECAM preparation happens before acquiring it. On the port path, the lock keeps the address selection and data access together so another CPU cannot redirect the transaction.

Byte and halfword writes use their hardware write widths. This preserves adjacent registers with bits cleared by writing one. Command at `0x04` shares a doubleword with Status at `0x06`; the former doubleword read/modify/write implementation replayed Status bits during Command updates and cleared error evidence. The correction and its reason are recorded in `c/kernel/pci/pci.c`.

## Capability discovery

`pci_cap_find()` returns the first matching standard capability offset. `pci_cap_next()` supports iteration: pass zero to start, then the previous result to find the next capability with the same ID. Both return zero when the search ends.

The standard walker checks the Status capability-list bit, aligns pointers, tracks visited offsets and bounds each traversal to 48 entries. Repeated offsets, pointers below `0x40` and an absent capability header terminate traversal. `pci_ext_cap_find()` starts at `0x100`, tracks visited offsets and uses a 64-entry bound. See `c/kernel/pci/pci.c`.

Modern VirtIO uses repeated vendor capabilities to describe its configuration structures. `c/drivers/virtio/virtio.c` iterates them, validates each BAR index and checks that the reported offset and length fit within the enumerated resource before constructing the structure address.

## BAR discovery and access

A `struct dev_resource` stores a BAR's `start`, `size` and `flags`. The flags are defined in `c/drivers/core/driver.h`:

| Flag | Meaning |
|---|---|
| `DEV_RES_IO` | I/O port range |
| `DEV_RES_MEM` | Memory aperture |
| `DEV_RES_64` | Memory BAR consuming two positions |
| `DEV_RES_PREFETCH` | Prefetchable attribute reported by the BAR |

`pci_bar_probe()` sizes a BAR through writes of all ones. Its contract requires an unbound function because sizing temporarily changes hardware configuration. The implementation in `c/kernel/pci/pci.c` performs these steps:

1. Save Command, clear I/O and memory decoding, and verify that both decoding bits are clear.
2. Save the low BAR value, write all ones, read the size mask, then restore and verify the saved value.
3. For a 64-bit BAR, probe and restore the upper half and combine the masks and addresses.
4. Restore and verify Command before publishing the resource.

Disabling decoding prevents the temporary probe address from overlapping another device's registers. A sleeping ownership lock serialises complete BAR probes; the configuration lock protects individual transactions.

The size mask identifies implemented BARs, including resources whose firmware address is zero. The return value gives the number of BAR positions consumed, allowing enumeration to advance over a 64-bit BAR's upper half.

Readback controls recovery. If decoding stays enabled, probing attempts to restore the entry Command value and returns an empty resource. Persistent BAR restoration failure returns an empty resource with decoding disabled. Command restoration failure requests the decoding-disabled state again, logs the result and withholds the resource.

`dev_bar_map()` maps a memory resource with writable, uncached identity mappings and returns its base address. Drivers use an I/O resource's base as a port address. `dev_enable_checked()` requests decoding for resource types with assigned addresses and sizes, then verifies Command readback. A nonzero bus-master argument sets bus mastering; zero clears it. See `c/drivers/core/device.c`.

`dev_bar_map_window()` maps a byte interval within a memory BAR and returns the interval's starting address. It checks resource bounds and address arithmetic, including the rounding needed for page mappings. Callers sharing a BAR with the framebuffer must keep every touched page clear of scanout: remapping the whole RX 580 aperture as uncached replaced the framebuffer mappings that had write combining enabled, as recorded in `c/drivers/core/device.c`.

The Polaris probe in `c/drivers/gpu/amd/polaris/device.c` maps BAR5 through its mapping callback and reads register observations. It records BAR0's CPU aperture size alongside the VRAM capacity read from the device. `c/drivers/gpu/amd/polaris/resource/device.c` rechecks BAR0 and BAR5 addresses against enumerated resources. The self-test arena in `c/drivers/gpu/amd/bootfb.c` uses `dev_bar_map_window()` for its planned BAR0 interval.

VirtIO shows the enable sequence: enable decoding with bus mastering cleared, map configuration structures, reset the transport, negotiate features, then enable bus mastering. See `c/drivers/virtio/virtio.c`.

## MSI and MSI-X

Drivers call `dev_irq_request()` with a device, handler, argument and diagnostic name. For a new request, `c/kernel/pci/pci_msi.c` first quiesces advertised MSI-X and MSI capabilities, then tries MSI-X, MSI and INTx in that order. `dev_irq_prefer()` caps the permitted modes for exercising fallback paths.

Message interrupts require a ready local APIC. The destination address is `0xFEE00000` combined with the requesting CPU's APIC ID shifted by 12 bits. Message data contains the allocated vector, using fixed delivery and edge triggering.

`irq_alloc_vector()` installs the callback and IDT gate from the pool spanning `0x60` through `0x7F`. Dispatch borrows the callback under a short lock, invokes it outside that lock and retains its active reference through end-of-interrupt acknowledgement. See `c/drivers/core/irq.c`.

MSI-X setup reads the table's BAR indicator and offset, maps that BAR and checks that the first 16-byte table entry fits within the resource. It masks the entry, writes and verifies the destination address and vector, then unmasks the entry. Finally, it enables MSI-X, clears the function mask and verifies those control bits.

MSI setup clears and verifies the enable bit before programming the destination. It handles 32-bit and 64-bit address layouts, writes and verifies the message fields, and clears per-vector mask bits when supported. Clearing the multiple-message enable field selects one message. Setup then enables MSI and verifies the control bits.

Both message paths suppress legacy INTx through `PCI_CMD_INTX_DIS` and check readback. If setup fails to establish a quiescent message source, the production failure path logs the problem, disables local interrupts and halts the calling CPU. This keeps a source that may still target the vector from reaching callback storage released by probe failure. See `c/kernel/pci/pci_msi.c`.

## INTx routing

INTx route selection uses this priority in `c/kernel/pci/pci_msi.c`:

| Source | Route selection |
|---|---|
| ACPI `_PRT` | Evaluated routing entry, including link-device resources |
| PCH RCBA | Recognised Intel chipset registers select PIRQA–H at GSI 16–23 for supported bus-0 slots |
| Firmware interrupt line | Values 1–15 pass through ACPI ISA source overrides; values 16–254 supply a GSI directly |

The previous page described firmware-line routing and listed `_PRT` evaluation and bridge swizzling as absent. `c/kernel/pci/pci_prt_aml.c` now implements both. When `_PRT` and RCBA disagree, routing logs both GSIs and uses `_PRT`.

When SMP has selected I/O APIC routing, the AML integration requests `_PIC(1)` after namespace loading and before namespace initialisation runs `_INI`. After successful AML initialisation, the worker in `c/kernel/init/power.c` calls `pci_prt_aml_ready()` outside the AML lock. This publishes the resolver and starts migration of existing INTx bindings. The namespace ordering is implemented in `c/drivers/power/acpi/devices.c`.

The resolver discovers PCI roots through their ACPI identities and uses `_SEG`, `_BBN` and child `_ADR` values to connect namespace devices to the enumerated topology. It searches the current bus's routing table, then walks upstream bridges. Each bridge crossing swizzles the zero-based pin as `(pin + child_slot) % 4`.

A direct `_PRT` entry supplies a GSI with level triggering and active-low polarity. A link entry supplies a descriptor index in the complete resource template. The resolver checks link presence and enable status, then reads the selected current interrupt resource. IRQ descriptors pass through ISA source overrides; Extended IRQ descriptors supply GSIs directly.

For an unassigned link, the resolver selects a usable interrupt from `_PRS`, builds a complete configuration from the chosen alternative and submits it through `_SRS`. It protects cached sibling interrupt assignments before that write, then rereads the selected interrupt and checks its number, polarity, trigger mode and link enable status. See `c/kernel/pci/pci_prt_aml.c`.

Functions sharing a GSI share one vector. Dispatch calls each registered member, allowing each driver to inspect and acknowledge its own device. The shared wire retains the electrical description from the highest-priority accepted source. Joining requests are checked against conflicting requests from peers at the same priority.

`c/kernel/pci/pci_platform.h` selects edge triggering for the TCG hypervisor identified through CPUID. Its comment records the interrupt storm that motivated this workaround. Other routes use the selected source's trigger mode and polarity.

ACPI discovery records multiple I/O APICs. Routing selects the controller covering the requested GSI through `c/kernel/cpu/irq/ioapic_map.c`; register programming and readback checks live in `c/kernel/cpu/irq/ioapic.c`.

## Rerouting and retirement

Live rerouting acquires each device's `bind_busy` ownership and checks the INTx binding generation captured for the pass. A resolver miss retains the existing route. A move suppresses the source, retires its old membership and binds the new route using the destination CPU captured by the first INTx binding.

Successful moves call the driver once to service accumulated status. Failed moves attempt to restore the old route. Quarantine of the old route or failure to restore leaves the device with interrupts disabled and polling required. Lifecycle contention is retried with at most 100 requested sleeps of 5 ms; scheduling and timer granularity determine the elapsed time. See `c/kernel/pci/pci_msi.c` and `c/kernel/pci/pci_prt_aml.c`.

`dev_irq_release()` suppresses the device source before retiring its callback. Shared INTx removal drains that member's active callbacks. The last member also masks the I/O APIC route and checks that delivery has drained. Failed masking or draining quarantines the route and vector against reuse.

I/O APIC draining checks Delivery Status and, for level interrupts, Remote IRR. It first allows delivery to complete, then attempts controller acknowledgement while retaining the old vector. See `c/kernel/cpu/irq/ioapic.c`.

Earlier vector retirement relied on callback draining. `irq_free_vector()` now clears callback ownership after draining active dispatches and keeps the vector quarantined until every CPU in the online snapshot has checked its local APIC IRR and ISR. Cleanup runs through remote kick IPIs, the LAPIC acknowledgement hook and checks after dispatch releases its active reference. See `c/drivers/core/irq.c` and `c/kernel/cpu/irq/lapic.c`.

For MSI and MSI-X, failed disable readback returns an error and retains interrupt ownership. `dev_unbind()` then preserves the driver binding and callback storage so teardown can be retried. See `c/drivers/core/device.c`.

Before reboot, `dev_quiesce_all()` walks the registry in reverse order, removes eligible drivers and checks Bus Master readback. Storage remains bound, with enabled bus mastering counted as unconfirmed DMA. Display drivers remain bound and can report idleness through their `quiesce` callback. The reset policy in `c/kernel/init/power.c` requires a platform reset when DMA remains unconfirmed.

## Diagnostics and fixtures

Boot logs the selected configuration mechanism and registry count. `dev_dump()` prints addresses, IDs, classes, bindings, capability presence, interrupt modes and BAR resources. `dev_irq_count()` reports deliveries to the device's vector; members sharing an INTx vector observe the same count.

`pci_route_diag()` reports the selected route, I/O APIC delivery and mask state, and PCI assertion status. `pci_intx_delivery_snapshot()` copies the INTx binding generation, vector and count of dispatcher admissions under the INTx lock. That count records handler admission on the shared wire and excludes the rerouting catchup callback. See `c/kernel/pci/pci_route.h` and `c/kernel/pci/pci_msi.c`.

`make HWHUD=1` enables the hardware panel. Its INTx display calls `pci_intx_diag()` to show I/O APIC entries and member PCI Command and Status registers. See `Makefile` and `c/kernel/diag/hwhud.c`.

The fixtures encode checks at several layers:

| Fixture | Checks |
|---|---|
| `tests/unit/pci_test.c` | BAR sizing and recovery, capability traversal, ECAM addressing and bridge enumeration |
| `tests/unit/devmodel_test.c` | Driver matching, binding, lookup and checked Command updates |
| `tests/unit/pci_hardware_test.c` | Command writes preserving Status, MCFG ranges starting above bus zero and trigger selection |
| `tests/unit/pci_msi_test.c` | MSI/MSI-X programming, fallback and failures during configuration writes |
| `tests/unit/pci_intx_test.c` | Shared dispatch, concurrent removal, acknowledgement draining and route quarantine |
| `tests/unit/bkl_irq_test.c` | Callback lifetime, CPU vector quarantine and cleanup progress with PIC fallback |
| `tests/boot/run-devmodel-test.sh` | Legacy-port and ECAM guest configurations, bridge discovery and MSI/INTx delivery markers |
| `tests/boot/run-pci-intx-test.py` | Guest checks for shared delivery and stale routes around vector reuse |

`make test-pci` runs the fixtures for configuration, device model and message programming. `make test-devmodel` runs both enumeration guest configurations. The targets for shared interrupts are `make test-pci-intx-host` and `make test-pci-intx-guest`; `make test-bkl-irq-host` covers dynamic dispatch and CPU quarantine. These last three targets depend on their negative controls in `tests/pci_intx.mk`, `tests/pci_intx_guest.mk` and `tests/dma.mk`.

## Limitations

- Configuration access supports segment zero and one ECAM aperture fixed at boot. Extended configuration reads through the legacy path return all ones; writes are ignored.
- Enumeration runs once from bus 0 and follows firmware secondary bus assignments. Bus renumbering, BAR address allocation and hotplug discovery are absent.
- BAR probing can describe an implemented resource at address zero. Mapping helpers reject that base, and checked enabling skips such resources when deriving decoding requirements.
- The device registry holds 64 functions. Dynamic device interrupts share 32 vectors.
- Message setup programs one MSI message or MSI-X table entry zero per function. Destination encoding accepts physical APIC unicast IDs through `0xFE`; interrupt remapping is absent.
- AML routing supports segment zero, stores up to 16 root bridges and 64 assignments of link resources, and bounds topology walks to 32 hops. Link allocation chooses IRQs below 16 and accepts configurations whose other resources require no further allocation. Disabled links retain fallback routing.
- Routing and retirement source comments record synthetic AML, model and QEMU evidence. `_PRT` and RCBA wiring remain unverified on X79-VG4 hardware.

## See also

- [Device and driver model](../architecture/device-model.md)
- [Kernel initialisation](init.md)
- [ACPI](acpi.md)
- [Local APIC and I/O APIC](apic.md)
- [VirtIO drivers](../drivers/virtio.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/pci/pci.h` | Configuration API, register constants, capability helpers and BAR-probe contract |
| `c/kernel/pci/pci.c` | Configuration access, capability traversal, BAR sizing, enumeration and legacy lookup |
| `c/kernel/pci/pci_init.c` | MCFG selection and PCI initialisation |
| `c/kernel/pci/pci_msi.c` | Message interrupts, INTx routing, migration, diagnostics and retirement |
| `c/kernel/pci/pci_prt_aml.c` | ACPI routing tables, link resources, bridge swizzling and resolver publication |
| `c/kernel/pci/pci_route.h` | Route descriptions, migration statistics and delivery snapshots |
| `c/kernel/pci/pci_platform.h` | INTx trigger selection and TCG workaround |
| `c/kernel/cpu/acpi/acpi.c` | MCFG entries, I/O APIC discovery and ISA interrupt overrides |
| `c/kernel/cpu/irq/ioapic_map.c` | Controller registry and GSI range lookup |
| `c/kernel/cpu/irq/ioapic.c` | Route programming, readback, masking and controller acknowledgement |
| `c/kernel/cpu/irq/lapic.c` | CPU checks for pending interrupts and the hook for vector cleanup |
| `c/kernel/init/kmain.c` | Enumeration, binding and late AHCI interrupt attachment |
| `c/kernel/init/power.c` | AML worker, routing publication and reboot policy |
| `c/drivers/power/acpi/devices.c` | Namespace loading, PCI preinitialisation hook and namespace initialisation |
| `c/drivers/core/driver.h` | Device records, resources and driver interfaces |
| `c/drivers/core/device.c` | Registry, binding, BAR mapping, checked Command updates and quiescing |
| `c/drivers/core/io_lock.h` | Short device locks preserving interrupt state |
| `c/drivers/core/io_domain.h` | Sleeping ownership locks used by mapping and probing |
| `c/drivers/core/irq.h` | Vector pool and interrupt lifecycle interface |
| `c/drivers/core/irq.c` | Dynamic vectors, dispatch, counters and CPU quarantine |
| `c/drivers/block/ahci.c` | Late MSI attachment, delivery checks and polling completion |
| `c/drivers/virtio/virtio.c` | Capability iteration and staged enabling |
| `c/drivers/gpu/amd/polaris/device.c` | BAR validation, register reads and aperture observations |
| `c/drivers/gpu/amd/polaris/resource/device.c` | Configuration-space BAR checks during resource discovery |
| `c/drivers/gpu/amd/bootfb.c` | Bounded mapping of the Polaris self-test arena |
| `c/kernel/diag/hwhud.c` | INTx display for the hardware panel |
| `tests/unit/pci_test.c` | Synthetic configuration and BAR fixtures |
| `tests/unit/devmodel_test.c` | Matching, binding and Command fixtures |
| `tests/unit/pci_hardware_test.c` | Status preservation, ECAM addressing and trigger fixtures |
| `tests/unit/pci_msi_test.c` | Message programming and failure fixtures |
| `tests/unit/pci_intx_test.c` | Shared interrupt and retirement fixtures |
| `tests/unit/bkl_irq_test.c` | CPU quarantine and callback lifetime fixtures |
| `tests/boot/run-devmodel-test.sh` | Guest assertions about enumeration and interrupt delivery |
| `tests/boot/run-pci-intx-test.py` | Guest assertions about shared interrupts and stale routes |
| `Makefile` | PCI fixture targets, guest configurations and the hardware panel option |
| `tests/pci_intx.mk` | Shared INTx host target and the prerequisite for its negative control |
| `tests/pci_intx_guest.mk` | Shared INTx guest builds and negative controls |
| `tests/dma.mk` | IRQ lifecycle host target and the prerequisite for its negative control |
