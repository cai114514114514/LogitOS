# Ethernet drivers

LogitOS connects Ethernet controllers to the network stack through `struct netdev`. Drivers configure the controller, manage DMA storage, submit Ethernet frames, and drain received packets. The common layer in `c/drivers/net/netdev.c` binds PCI devices, registers network interfaces, dispatches packet operations, and requests interrupt routes.

## Supported controllers

The tables in `c/drivers/net/net_ids.inc` identify PCI candidates. Probes then check controller resources, silicon identity, and initialization conditions.

| Driver | Controller family | PCI vendor:device matches |
|---|---|---|
| `virtio-net` | VirtIO networking using the modern PCI transport | `1af4:1000`, `1af4:1041` |
| `e1000` | Intel 8254x | `8086:100e`, `8086:1015`, `8086:1016`, `8086:1017`, `8086:101e`, `8086:1004`, `8086:100f`, `8086:1010`, `8086:1026`, `8086:1076`, `8086:1077` |
| `e1000e` | Intel 82574L/IT and 82574LA | `8086:10d3`, `8086:10f6` |
| `e1000-pch2` | Intel PCH2 with 82579LM/V | `8086:1502`, `8086:1503` |
| `rtl8139` | Realtek RTL8139 and listed variants or relabels | `10ec:8139`, `10ec:8138`, `1113:1211` |
| `rtl8169` | RTL8168G/GU and RTL8411B initialization paths, admitted through the MAC-version check below | `10ec:8169`, `10ec:8168`, `10ec:8161`, `10ec:8136`, `1186:4300` |
| `pcnet` | AMD PCnet-PCI II Am79C970A | `1022:2000` |

The earlier Ethernet table described the full RTL8169, RTL8168/8111, and RTL8101/8102 match family as supported. Current probe admission uses `rtl_identify()` in `c/drivers/net/rtl8168_id.c`: it decodes the transmit-configuration XID and accepts MAC versions 40, 42, and 44. These select the G, GU, and 8411B startup programs.

Intel initialization follows the controller generation. `c/drivers/net/e1000e.c` handles PCIe master shutdown, NVM autoload, MDIO ownership, and the 82574 PHY. `c/drivers/net/e1000_pch2.c` handles PCH2 firmware policy, software ownership, reset timing, and the 82579 PHY. Both use the legacy descriptor layouts in `c/drivers/net/e1000e_ring.h`.

PCnet also checks silicon identity after the PCI match. `pcnet_probe()` reads the chip version before configuring the Am79C970A path in `c/drivers/net/pcnet.c`.

## Binding and interface registration

`c/kernel/init/kmain.c` enumerates PCI, initializes networking, initializes multiprocessor support, and then calls `dev_probe_all()`. Networking needs packet exchange during address acquisition, so `netdev_init()` performs an early binding pass before APIC routing is ready.

That pass tries drivers in this order:

1. `virtio-net`
2. `e1000`
3. `e1000e`
4. `rtl8139`
5. `pcnet`
6. `rtl8169`
7. `e1000-pch2`

For each driver, the common layer examines matching unclaimed devices. A successful probe publishes its `struct netdev` through `dev_set_drvdata()` and returns zero. Failed probes release the attempted binding so scanning can continue.

Successful bindings receive names such as `eth0` and `eth1` in binding order. The first becomes the primary interface. Loopback registers before the cards as `lo`, with interface index 1 and address 127.0.0.1/8. The older “first card” description in `c/drivers/net/netdev.h` predates the current registration loop.

During `smp_init()`, `netdev_irq_route()` requests routes for registered PCI NICs. The later `dev_probe_all()` pass skips devices already bound. These paths are in `c/kernel/cpu/smp/smp.c` and `c/drivers/core/device.c`.

## The netdev interface

`c/drivers/net/netdev.h` assigns controller operations to the driver and interface configuration to the network layer.

| Type | Contents |
|---|---|
| `struct netdev` | Driver name, MAC address, interrupt line, MSI eligibility, and callbacks for transmission, receive polling, interrupt activation, interrupt handling, and link information |
| `struct netif` | Interface name, stable index, flags, copied MAC address, IPv4 address entries, and device pointer |

The `tx` callback accepts a complete Ethernet frame. Each backend copies the frame into DMA storage before submission. A zero return indicates successful submission; hardware completion follows the controller’s descriptor, ownership, or status protocol.

The `rx_poll` callback drains receive work and invokes `net_rx_cb` with Ethernet bytes after removing device headers. The buffer remains valid for the duration of the callback. The driver can then return that storage to the controller.

`netdev_tx()` selects the primary interface. `netdev_tx_if()` selects an interface by index and checks for a transmit callback and both `NETIF_F_UP` and `NETIF_F_RUNNING`. `netdev_rx_poll()` visits every registered device with a receive callback.

Link state reaches the interface table through `netdev_set_link()`, which updates `NETIF_F_RUNNING` under the network lock. `netdev_link_up()` reads the primary interface’s flag. `netdev_link_info()` queries its optional callback while that interface is running. RTL8169 supplies speed and duplex from cached PHY status.

Address policy lives above the drivers. `c/net/core/netcfg.c` applies static settings or starts DHCP. In `c/net/core/dhcp.c`, the boot attempt shares a four-second deadline between waiting for the primary link and exchanging DHCP messages. Background acquisition continues through `dhcp_poll()` and follows link transitions.

## Rings and buffer ownership

These are the default queue and packet-storage sizes in the implementations. Several drivers allocate memory pages larger than the packet storage exposed to hardware.

| Driver | Receive storage | Transmit storage | Completion protocol |
|---|---|---|---|
| `e1000` | 64 descriptors with 2,048-byte buffers | 8 descriptors with 2,048-byte buffers | Descriptor done bits and head/tail registers |
| `e1000e` | 64 descriptors with 2,048-byte buffers | 32 descriptors, at most 31 pending, with 2,048-byte buffers | Descriptor done bits and head/tail registers |
| `e1000-pch2` | 64 descriptors with 2,048-byte buffers | 32 descriptors, at most 31 pending, with 2,048-byte buffers | Descriptor done bits and head/tail registers |
| `rtl8139` | One 8 KiB byte ring with wrap padding | Four hardware transmit slots | Receive headers and offsets; transmit status registers |
| `rtl8169` | 32 descriptors with 2,048-byte buffers | 16 descriptors with 2,048-byte buffers | Ownership bits and an end-of-ring marker |
| `pcnet` | 32 descriptors with 2,048-byte buffers | 32 descriptors with 2,048-byte buffers | Software Style 2 ownership and status fields |
| `virtio-net` | 32 posted packet buffers exposing 2,048 bytes each | 16 packet buffers exposing 2,048 bytes each | Available and used rings in split virtqueues |

### Intel descriptor rings

The 8254x receive path in `c/drivers/net/e1000.c` checks completed descriptors for length, errors, and end-of-packet status. It clears consumed descriptors and returns them through the receive tail register. The default refill batch is 16 descriptors, with any remainder returned when the drain ends. This reduces MMIO writes while replenishing the ring during a burst.

Transmission waits for the selected descriptor’s previous completion, copies the frame, requests FCS insertion and completion status, and advances the transmit tail.

The 82574 and PCH2 drivers use the 16-byte layouts in `c/drivers/net/e1000e_ring.h`. Both reclaim completed transmissions and track pending entries. They preserve one empty transmit slot because equal hardware head and tail positions denote an empty queue. Receive drains return consumed descriptors with a tail update after the batch.

### Realtek receive layouts

RTL8139 writes packets consecutively into a byte ring. Each packet begins with a four-byte status and size header. The receive loop removes the trailing four-byte FCS from the delivered length, advances to the next four-byte boundary, and writes the receive position minus 16 to the controller’s read pointer.

The subtraction belongs to the hardware protocol: an incorrect read pointer can make the controller treat the ring as full. The allocation includes 16 bytes plus 2,048 bytes of padding beyond the 8 KiB ring, accommodating a packet written across its end. `c/drivers/net/netring.h` contains the arithmetic; `c/drivers/net/rtl8139.c` contains the receive loop.

RTL8169 uses descriptors with ownership, first-segment, last-segment, error, and length fields. Its receive path accepts a complete frame in one descriptor, removes the FCS length, and restores ownership. The final descriptor retains the end-of-ring marker.

After observing released receive ownership, RTL8169 executes a DMA read barrier before accessing the payload. Startup writes descriptor addresses before enabling the engines, with each address’s high half written first. The source preserves the earlier claim that addresses followed engine enable beside this correction in `c/drivers/net/rtl8169.c`.

The startup helpers in `c/drivers/net/rtl8168_hw.c` wait for FIFO state, leave firmware ownership, and apply the selected MAC and ePHY program. `c/drivers/net/rtl8168_phy.c` waits for an inherited PHY reset, configures the PHY, and starts autonegotiation. Completion waits in the ERI, ePHY, CSI, and PHY access paths are bounded, and failures propagate back to the probe.

### PCnet descriptors

PCnet selects Software Style 2 before publishing its initialization block. `c/drivers/net/pcnet_ring.h` defines the 16-byte descriptors, negative buffer-length encoding, and receive validation.

The driver prepares descriptor fields, executes a DMA write barrier, and publishes ownership last. Receive validation checks status, complete-frame markers, and length. Transmit reclamation counts successful and failed completions after the device returns ownership.

### VirtIO queues

`c/drivers/virtio/virtio_net.c` uses queue 0 for receive and queue 1 for transmit. Each packet buffer contains a 12-byte VirtIO header followed by Ethernet bytes; the 2,048-byte capacity includes that header. Descriptor IDs identify the corresponding packet buffers.

The transport in `c/drivers/virtio/virtio.c` configures split virtqueues. The network probe requires queue capacity for at least 32 receive buffers and 16 transmit buffers.

Receive buffers are posted before the device becomes ready. The drain consumes used-ring entries, delivers frame bytes, reposts buffers, and notifies the receive queue after the batch. Transmit buffers are reclaimed during subsequent sends, and the transmit available ring requests suppression of completion interrupts.

The helpers in `c/drivers/net/netring.h` calculate pending completions using unsigned 16-bit subtraction, preserving progress when the counters wrap.

## Interrupts and receive scheduling

`netdev_irq_enable()` saves the receive callback. After routing succeeds, the common layer calls the routed device’s `irq_enable` hook. Register-based drivers enable their interrupt sources there; VirtIO records the callback.

The previous page described routing as INTx for every NIC. Current `netdev_irq_route()` uses `msi_ok` to select MSI with INTx fallback for RTL8169. The other Ethernet backends select INTx. `c/kernel/pci/pci_msi.c` owns route allocation, shared INTx dispatch, and callback retirement.

INTx resolution prefers evaluated ACPI `_PRT` information, then supported PCH routing registers, then the firmware interrupt line. `c/kernel/pci/pci_prt_aml.c` installs the AML resolver and initiates rerouting when the namespace becomes available.

NIC handlers acknowledge their controller and call `net_rx_schedule()`. In `c/net/core/net.c`, this raises `SOFTIRQ_NET`, whose handler drains devices through `netdev_rx_poll(eth_input)`. Notifications can coalesce. If another notification arrives while a previous drain remains owed, the scheduling path drains inline.

A recurring 10 ms timer also raises `SOFTIRQ_NET`. Its handler drains receive work before processing TCP timers. `net_poll()` supplies another receive path. Each backend bounds its receive drain so sustained traffic permits control to return.

RTL8169 checks link state during receive polling and link-change interrupts. Its interrupt handler masks controller interrupts after more than 50,000 invocations within its one-second counting window, leaving periodic polling to service receive work.

Acknowledgement placement follows each controller’s implementation. Intel, PCnet, and VirtIO polling paths also acknowledge causes. RTL8139 acknowledges them in its interrupt handler. Its source records nine successful 900 KiB transfers with the existing drain and zero successful transfers after adding acknowledgement to that drain, explaining the placement in `c/drivers/net/rtl8139.c`.

## Locking, DMA, and removal

`net_lock()` in `c/net/core/netlock.c` disables local interrupts and acquires a recursive lock owned by the current CPU. Recursion supports receive processing that generates a reply and enters transmission. Driver receive drains hold the lock across delivery to the stack. `NET_GUARD`, defined in `c/net/core/net.h`, releases the lock when its scope exits.

The Ethernet transmit wrapper in `c/net/link/eth.c` holds this lock while constructing and submitting a frame. It zero-pads short frames to 60 bytes before the NIC appends the FCS. Central padding supplies the minimum frame length and prevents stack contents from reaching the wire as padding.

Drivers allocate coherent memory through `c/drivers/core/dma.h`. Each allocation supplies a CPU pointer and a DMA address; descriptors receive the DMA address. RTL8139, PCnet, and PCH2 use a 32-bit DMA mask. RTL8169 selects a 64-bit mask for recognized Realtek PCIe devices with ID `8168` or `8161`, and a 32-bit mask for the remaining cases. Its capability decision uses the same `rtl_identify()` function as probe admission.

`dev_unbind()` retires interrupt callbacks before invoking the removal hook. Drivers confirm controller reset or DMA shutdown before reclaiming buffers. An unconfirmed stop quarantines DMA allocations, retaining memory that hardware may still access.

RTL8169 requires both reset acknowledgement and successful PCI bus-master disable before releasing its buffers. A later probe uses `dma_device_resume()` to reopen a quiesced DMA context; live or quarantined allocations cause refusal.

Reboot reaches NIC removal through `dev_quiesce_all()`, called from `c/kernel/init/power.c`. The device model checks the PCI bus-master bit after removal and attempts to clear it when needed. If execution returns past the final reset attempt, `power_abort()` calls `dev_probe_all()` to attempt rebinding.

## Diagnostics and checks

`netif_dump()` reports interface names, flags, drivers, MAC addresses, IPv4 addresses, and the primary interface. Driver reports add link and traffic information.

The 8254x statistics code in `c/drivers/net/e1000_stats.h` accumulates hardware counters, adapting its 32-bit accumulation to clearing and persistent register values. PCnet, 82574, and PCH2 inspect transmit completion status when reclaiming descriptors.

`net_rx_irq_count()` counts receive scheduling calls. `net_rx_poll_active()` reports polling when no NIC route has been recorded, or when at least four unannounced receive entries have accumulated during interrupt silence exceeding 100 ms. Each scheduling interrupt clears the accumulated evidence.

The repository defines these checks:

| Definition | Coverage |
|---|---|
| `tests/nic.mk` | Shared ring helpers, PCI matches, 8254x statistics and link checks, and QEMU transfers for 8254x, VirtIO, and RTL8139 |
| `tests/pcnet.mk` | PCnet descriptor validation and QEMU transfers across ring wrap |
| `tests/e1000e.mk` | Intel descriptor and MDIO result validation, plus QEMU transfers |
| `tests/e1000_pch2.mk` | Host register-model checks of the PCH2 implementation |
| `tests/unit/netif_test.c` | Interface registration, binding priority, transmit selection, receive polling, and interrupt routing |
| `tests/dma_drivers.mk` | Driver DMA address and lifetime checks, with PCI Command checks and mutation controls |

`tests/boot/run-nic-test.sh` checks binding, DHCP, and a deterministic 32 KiB HTTP response. `tests/boot/nic_http_result.py` validates its length, FNV value, and SHA-256 digest. The PCnet and 82574 guest definitions use `tests/boot/run-pcnet-test.py`, which checks two 128 KiB responses and observed server requests.

The DMA target depends on its mutation controls and the PCI Command contract target. `tests/unit/pci_command_run.py` checks RTL8169 preparation, reset acknowledgement, startup, bus-master enable, and DMA publication order by inspecting source.

## Limitations

- Physical validation remains pending for the RTL startup paths, PCH2 operation, and the 82574LA match, as recorded in `c/drivers/net/rtl8169.c`, `c/drivers/net/e1000_pch2.c`, and `c/drivers/net/net_ids.inc`.
- `NETIF_MAX` provides four interface slots, including loopback. Each Ethernet backend stores one controller instance in static state.
- The Ethernet transmit path selects the primary NIC through `netdev_tx()`. Receive callbacks carry bytes and length without an ingress interface index.
- Boot registration initially sets the running flag. RTL8169 publishes subsequent link observations through `netdev_set_link()`; the other Ethernet backends retain that initial flag.
- Receive return counts vary by backend: 82574, PCH2, and PCnet count delivered frames; the other backends can count consumed entries that were rejected. RTL8139 and VirtIO transmit success counters count submissions.
- PCH2 requires its supported firmware ownership and hardware NVM autoload conditions. Managed firmware coexistence, software NVM configuration, LANPHYPC recovery, and suspend remain outside that path.
- RTL startup omits external PHY firmware patches and PAUSE advertisement, and keeps chip ASPM controls disabled. The RTL8169 DMA fixture in `tests/unit/dma_driver_test.c` stubs MAC/PHY startup.
- The match tables exclude Intel I217/I218/I219, I225/I226, and Realtek RTL8125. `c/drivers/net/net_ids.inc` records their differing initialization and register contracts.

## See also

- [Driver overview](overview.md)
- [VirtIO drivers](virtio.md)
- [Device and driver model](../architecture/device-model.md)
- [Network stack](../architecture/network-stack.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/net/netdev.h` | Device callbacks, receive buffer lifetime, interface structures, and capacity constants |
| `c/drivers/net/netdev.c` | Binding priority, registration, dispatch, link queries, and interrupt routing |
| `c/drivers/net/net_ids.inc` | PCI matches and controller eligibility notes |
| `c/drivers/net/netring.h` | Ring arithmetic, VirtIO header size, and Realtek field helpers |
| `c/drivers/net/e1000.c` | Intel 8254x initialization, rings, interrupts, and diagnostics |
| `c/drivers/net/e1000_stats.h` | Intel statistics accumulation and link decoding |
| `c/drivers/net/e1000e.c` | Intel 82574 initialization, PHY access, queues, and removal |
| `c/drivers/net/e1000e_ring.h` | Shared Intel legacy descriptors and validation helpers |
| `c/drivers/net/e1000_pch2.c` | PCH2 ownership, reset, PHY configuration, queues, and removal |
| `c/drivers/net/rtl8139.c` | Byte receive ring, transmit slots, interrupts, and diagnostics |
| `c/drivers/net/rtl8169.c` | Realtek descriptor rings, link reporting, DMA selection, and removal |
| `c/drivers/net/rtl8168_id.c` | XID decoding and accepted MAC versions |
| `c/drivers/net/rtl8168_hw.c` | Firmware ownership exit and MAC/ePHY startup |
| `c/drivers/net/rtl8168_phy.c` | PHY access, reset completion, tuning, and autonegotiation |
| `c/drivers/net/pcnet.c` | PCnet initialization, queues, interrupts, and shutdown |
| `c/drivers/net/pcnet_ring.h` | Software Style 2 layouts and validation |
| `c/drivers/virtio/virtio_net.c` | VirtIO network queues and packet buffers |
| `c/drivers/virtio/virtio.c` | Modern VirtIO PCI transport, queue setup, and reset |
| `c/drivers/core/dma.h` | DMA addresses, allocation handles, and ownership API |
| `c/drivers/core/dma.c` | DMA allocation, quarantine, quiescence, and resume |
| `c/drivers/core/device.c` | Device probing, unbinding, and reboot quiescence |
| `c/kernel/pci/pci_msi.c` | Interrupt allocation, INTx resolution, dispatch, and retirement |
| `c/kernel/pci/pci_prt_aml.c` | ACPI PCI routing and link-device resolution |
| `c/net/core/net.c` | Receive scheduling, timer service, polling, and diagnostics |
| `c/net/core/net.h` | Network entry points and scoped locking |
| `c/net/core/netcfg.c` | Boot address policy |
| `c/net/core/dhcp.c` | Boot deadline and background DHCP acquisition |
| `c/net/core/netlock.c` | Recursive network lock |
| `c/net/link/eth.c` | Ethernet construction, padding, transmission, and input |
| `c/kernel/init/kmain.c` | PCI and network initialization order |
| `c/kernel/cpu/smp/smp.c` | NIC interrupt routing during processor setup |
| `c/kernel/init/power.c` | Reboot quiescence and final abort path |
| `tests/nic.mk` | Common NIC, statistics, link, and guest check definitions |
| `tests/pcnet.mk` | PCnet check definitions |
| `tests/e1000e.mk` | Intel 82574 check definitions |
| `tests/e1000_pch2.mk` | PCH2 register-model check definitions |
| `tests/unit/netif_test.c` | Interface and dispatch checks |
| `tests/dma_drivers.mk` | DMA and PCI Command check dependencies |
| `tests/unit/dma_driver_test.c` | DMA adapters, startup stubs, and lifetime checks |
| `tests/unit/pci_command_run.py` | PCI Command guards and source ordering controls |
| `tests/boot/run-nic-test.sh` | Guest binding, DHCP, and HTTP checks |
| `tests/boot/nic_http_result.py` | Deterministic 32 KiB response validation |
| `tests/boot/run-pcnet-test.py` | PCnet and 82574 guest transfer checks |