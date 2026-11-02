# VirtIO drivers

LogitOS implements VirtIO block, network, GPU, entropy, balloon and SCSI drivers over a shared PCI transport. The transport discovers configuration regions, negotiates features and creates split virtqueues. Device drivers supply command formats and connect buffers to the block layer, network stack, framebuffer and physical memory allocator. The shared interfaces are defined in `c/drivers/virtio/virtio.h` and implemented in `c/drivers/virtio/virtio.c`.

## Transport and discovery

The transport reads vendor PCI capabilities to locate configuration regions:

| Capability type | Region | Purpose |
|---|---|---|
| 1 | Common configuration | Features, device status and queue configuration |
| 2 | Notification | Queue doorbells and notification address multiplier |
| 3 | Interrupt status | Reading the status byte acknowledges interrupts |
| 4 | Device configuration | Fields such as capacity, MAC address and balloon target |

`virtio_init()` selects the first device matching the vendor and device IDs in the registry. `virtio_init_device()` initializes the PCI function supplied by its caller. Using the caller's function matters when several devices share an ID: another lookup could select and reset a different function.

Initialization enables BAR decoding with bus mastering cleared and checks the PCI Command register through `dev_enable_checked()`. The transport maps memory BARs through `dev_bar_map()` and checks each capability’s offset and length against its BAR size. These resource helpers live in `c/drivers/core/device.c`.

The transport resets the device, waits for status zero and sets the acknowledgement and driver status bits. It requests feature bit 32 for the modern interface and intersects the caller’s requested low feature bits with those offered by the device. `struct virtio_dev` records the negotiated low bits for drivers to consult before using commands or fields that depend on a feature.

After the device accepts the feature selection, initialization enables bus mastering and checks the readback again. Drivers configure queues, then call `virtio_driver_ok()` to submit queue memory to the DMA ownership layer and publish readiness.

The six drivers described here use these PCI IDs and queues:

| Device | PCI device IDs under vendor `0x1af4` | Queues configured |
|---|---|---|
| Block | `0x1001` | 0: requests |
| Network | `0x1000`, `0x1041` | 0: receive; 1: transmit |
| GPU | `0x1050` | 0: control; 1: cursor, when available |
| RNG | `0x1005` | 0: entropy requests |
| Balloon | `0x1002` | 0: inflate; 1: deflate |
| SCSI | `0x1004`, `0x1048` | 0: control; 1: events; 2: requests |

The IDs appear in the device implementations, `c/drivers/virtio/virtio.h` and `c/drivers/net/net_ids.inc`. Drivers matching transitional IDs also use the modern PCI capability interface.

## Split virtqueues

`struct virtq` holds coherent DMA allocations for the descriptor table, available ring and used ring. `virtio_queue_setup()` allocates one 4 KiB page for each structure and caps the queue size at 256 entries, keeping all three structures within their allocations. It programs their DMA addresses into common configuration and calculates the doorbell address from the queue notification offset and transport multiplier.

Each descriptor contains a device address, length, access direction and optional link to another descriptor. The driver publishes a chain’s head in the available ring. The device completes it through an entry in the used ring containing the head ID and written byte count. Memory fences order descriptor writes, ring publication and notification in `c/drivers/virtio/virtio.c`.

`virtio_request()` holds the transport gate across chain construction, publication, notification and polling for the matching completion. It returns the written byte count on success and `-1` on failure. A completion of zero bytes is valid for commands such as GPU cursor updates. Polling enables interrupts, and the call restores the caller’s interrupt state on exit.

Block, GPU, RNG, balloon and SCSI also hold operation gates across command preparation and completion. Those gates protect shared staging buffers before and after the transport call. The network driver manages persistent rings through the network locking path. The operation gates are implemented in `c/drivers/core/io_lock.h`.

## DMA ownership and reset

Descriptors carry `dma_addr_t` values from DMA handles; CPU access uses the handles’ CPU pointers. Older comments in several VirtIO files describe staging that assumes an identity mapping. The current allocator in `c/drivers/core/dma.c` obtains CPU pointers through the physical memory map and records physical addresses for device access.

Queues and persistent staging use coherent allocations. Block and SCSI payloads use streaming mappings through `dma_map_kernel()` with `DMA_MAP_CONTIGUOUS`. Each payload occupies one descriptor, so the DMA layer supplies bounce storage when the original pages cannot form the required contiguous device range.

A request timeout marks the transport failed and resets the whole device. An acknowledged reset marks its DMA handles quiescent, allowing reclamation. A failed reset quarantines storage still owned by the device and blocks further submissions. All queues in a `struct virtio_dev` share this reset domain.

Block and SCSI panic when reset failure leaves a caller’s payload exposed to DMA. Returning would let the caller reuse that buffer while the device could still access it. Their removal paths take media offline and drain block requests before resetting the transport. GPU shutdown revokes framebuffer access through the framebuffer layer and drains existing CPU writers before resetting the device and releasing backing storage.

Reboot uses `dev_quiesce_all()` after filesystem draining and synchronization in `c/kernel/init/power.c`. For eligible devices outside the PCI storage and display classes, it attempts unbinding, then checks the binding and the state of bus mastering. After successful unbinding, remaining bus mastering triggers a checked disable of PCI decoding and bus mastering. Storage with bus mastering enabled contributes to the unconfirmed DMA count, which restricts reboot to platform reset paths. S5 poweroff proceeds through firmware with devices bound and restores filesystem admission if firmware returns.

## Block device

`c/drivers/virtio/virtio_blk.c` sends reads and writes as three descriptors: request header, payload and status byte. Read payloads permit device writes. Completion checks the status byte and requires enough returned bytes for the complete read payload and status.

Capacity comes from device configuration in units of sectors of 512 bytes. Transfers are split at `DMA_MAX_MAPPING`, defined as 1 MiB in `c/drivers/core/dma.h`, to bound mapping metadata and bounce storage.

The driver requests the flush feature. When negotiated, `virtio_blk_flush()` sends a flush command and checks its status. The block layer exposes it through the device’s flush callback.

`c/kernel/init/kmain.c` initializes VirtIO block before mounting the root filesystem. `blk_init()` registers it as `vblk0`, scans partitions and includes the resulting media in root selection. Later driver binding attaches the existing instance to its removal callback.

`blk_register()` initializes queue depth to one. VirtIO block supplies synchronous read, write and flush callbacks, so `c/drivers/block/blkdev.c` completes each request during submission.

## Network device

`c/drivers/virtio/virtio_net.c` manages receive and transmit rings directly. Persistent receive buffers provide space for arriving traffic across calls.

The driver allocates 32 receive buffers and 16 transmit buffers, each backed by a coherent page. Receive descriptors advertise 2048 bytes. Transmit descriptors advertise the packet’s length, including its header, with a maximum of 2048 bytes. Each packet occupies one descriptor containing a 12-byte modern VirtIO network header followed by the Ethernet frame. `c/drivers/net/netring.h` defines the header length and completion-counter arithmetic, including wraparound.

Receive buffers are posted before driver readiness. The receive path completes buffer ownership, checks the returned length, passes the Ethernet bytes to the network callback and reposts the buffer. Each drain processes at most 32 completions to bound work during sustained traffic. Later sends reclaim transmit completions; the transmit available ring requests interrupt suppression.

The driver requests MAC address and link status features and checks their acceptance individually. It reads an accepted MAC field from device configuration or chooses a locally administered address. Link checks read an accepted status field or assume the link is up, then log transitions.

The interrupt handler acknowledges the status byte and calls `net_rx_schedule()` when receive scheduling is enabled. Receive processing runs through the network softirq path in `c/net/core/net.c`, with an inline drain when an earlier drain remains pending. Polling also acknowledges the device, allowing INTx to deassert when polling consumes work before interrupt delivery.

The network timer raises a softirq every 10 ms, and both that handler and `net_poll()` drain receive queues. The `net_rx_poll_active()` diagnostic for the stack uses route availability, interrupt silence exceeding 100 ms and at least four unannounced completions counted during that silence. A call to `net_rx_schedule()` resets the unannounced count.

Registration and interrupt routing live in `c/drivers/net/netdev.c`. Older vector-65 comments in `c/drivers/virtio/virtio_net.c` describe the previous routing arrangement. The current route through the device model selects its preference through the NIC’s `msi_ok` field; VirtIO uses INTx, with that field at zero.

Address acquisition runs through `net_cfg_acquire()` in `c/net/core/netcfg.c`. User settings can supply static addresses. Automatic configuration shares an upper bound of four seconds across link waiting and the DHCP exchange, followed by background retries through `c/net/core/dhcp.c`. After acquisition fails at boot, the policy selects the static SLIRP configuration under a hypervisor; on hardware it continues acquisition with an unconfigured address.

## GPU device

`c/drivers/virtio/virtio_gpu.c` creates a 2D resource backed by coherent RAM and binds it to scanout 0. `c/kernel/gui/fb/fb.c` adopts its CPU pointer as the framebuffer.

Initialization queries display information and examines entry 0. It takes that entry’s dimensions when enabled with a nonzero width, starting from a 1920 × 1200 default. A width below 1280 or height below 800 selects the default because the desktop uses a 1280 × 800 design canvas.

Presentation clips a dirty rectangle to the framebuffer, transfers its pixels to the host resource and flushes the rectangle.

The cursor uses a 64 × 64 resource with alpha and queue 1. Defining an image transfers its pixels through the control queue before submitting the cursor update. Movement sends a 56-byte cursor command. For a VirtIO framebuffer, `c/kernel/gui/wm.c` selects the cursor plane when image definition succeeds and otherwise composites the pointer into the frame.

The device owns the framebuffer and cursor backing after attachment completes. Shutdown drains CPU framebuffer writers, then resets both queues before releasing the backing allocations.

## RNG device

`c/drivers/virtio/virtio_rng.c` submits writable buffers on queue 0. `virtio_rng_get()` copies data through a 64-byte coherent staging buffer, repeating requests after short completions. It returns accumulated progress if a later request fails and stops when a completion supplies zero bytes.

Probe performs two 16-byte reads. The serial diagnostic reports the length of a short read on failure, or the lengths, whether all bytes are zero and whether the reads match once both reads complete.

## Balloon device

`c/drivers/virtio/virtio_balloon.c` donates memory by holding physical frames outside the allocator’s free pool. Inflate requests contain guest physical frame numbers for 4 KiB pages. The array has a coherent DMA allocation; its entries encode physical addresses shifted right by 12.

Inflation allocates free frames and records their addresses. Deflation submits previously donated frames and frees them after acknowledgement. Each batch contains up to 1024 frame numbers. The table of held frames accommodates 65,536 pages, or 256 MiB.

Probe runs an inflate and deflate diagnostic over four pages, then applies a nonzero target already present in device configuration. `virtio_balloon_poll()` provides a callable adjustment path: it reads the target, changes the held count and writes the resulting actual count after an adjustment.

Failure recovery accounts for earlier donations and any submitted inflate batch whose completion is uncertain. Confirmed reset permits those frames to return to the allocator.

## SCSI device

`c/drivers/virtio/virtio_scsi.c` transports SCSI commands on request queue 2. `blk_init()` calls `virtio_scsi_init()` before root selection. Discovery issues inquiry and capacity commands, then registers accepted disks through the block layer. Target numbers determine names such as `vscsi0` and `vscsi3`.

Reads and writes use 16-byte SCSI commands with 64-bit block addresses. Transfer chunks respect the controller’s sector limit and `DMA_MAX_MAPPING`. Flushes use the 10-byte synchronize-cache command and require a successful SCSI response.

Descriptor ordering follows device access direction. Writes place outgoing data before the writable response; reads place the writable response before incoming data. Completion checks response length, sense length, residual count, transport response and SCSI status. Discovery makes up to three attempts when a command returns unit attention.

The controller operation gate covers commands across all discovered targets because they share command staging and a reset domain.

## Limitations

- The transport supports modern PCI configuration and split queues, with storage for four queues per transport. Legacy register transport, VirtIO MMIO transport and packed queues are absent.
- Each of these six drivers uses one static device instance. Synchronous commands serialize per device. VirtIO network supports one bound NIC and omits multiqueue, checksum offload and segmentation offload.
- VirtIO network link checks update its log. The interface flag read by `netdev_link_up()` reports the initial running state because this driver has no `netdev_set_link()` call.
- VirtIO RNG’s production consumer is its probe diagnostic. The kernel generator in `c/kernel/core/rng.c` has no connection to this driver.
- Balloon probe applies the initial target. `virtio_balloon_poll()` has no production call site, so later target changes require additional integration. Inflation draws from free frames without invoking reclaim.
- SCSI supports one controller, targets 0 through 7, LUN 0 and direct-access disks with 512-byte logical blocks. Its control and event queues receive no commands or buffers; hotplug, task management and additional request queues are absent.
- GPU support covers 2D resources and the mode selected during initialization. Runtime mode changes and 3D command submission are absent. The cursor plane is outside the scanout captured by a screendump, as documented in `c/drivers/virtio/virtio_gpu.h`.

## Test coverage

These targets define checks for the drivers. Their descriptions summarize harness assertions.

| Target | Coverage |
|---|---|
| `test-dma-virtio` | Transport, block, network, GPU, RNG and balloon code against a host device model with distinct CPU aliases and device addresses above 4 GiB. Controls exercise PCI selection, address handling, reset ownership and framebuffer writer draining. |
| `test-virtio-rng` | Boot reads, diagnostic verdicts and serial format, with controls for device absence and entropy disclosure. |
| `test-virtio-balloon` | Inflate and deflate diagnostics, guest free-frame accounting and host observation of a target applied during probe. |
| `test-virtio-scsi` | Modern controller discovery, targets 0 and 3, filesystem byte checks and persistence across two boots. Controls cover an absent controller and unsupported sector size. |
| `test-virtio-scsi-transitional` | The SCSI checks through transitional PCI ID `0x1004`. |

The targets and their control prerequisites are defined in `tests/dma.mk`, `tests/virtio_rng.mk`, `tests/virtio_balloon.mk` and `tests/virtio_scsi.mk`.

## See also

- [Device and driver model](../architecture/device-model.md)
- [Storage drivers](storage.md)
- [Ethernet drivers](network.md)
- [GPU drivers](gpu.md)
- [Physical memory](../kernel/physical-memory.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/virtio/virtio.h` | Transport state, split-ring structures and request interfaces |
| `c/drivers/virtio/virtio.c` | PCI capabilities, feature negotiation, queues, polling and reset |
| `c/drivers/core/device.c` | Device lookup, BAR mapping, checked PCI enablement and reboot quiescence |
| `c/drivers/core/driver.h` | Device and driver registration interfaces |
| `c/drivers/core/dma.h`, `c/drivers/core/dma.c` | DMA addresses, allocations, mappings and ownership |
| `c/drivers/core/io_lock.h` | Device operation gates |
| `c/drivers/virtio/virtio_blk.c` | Block requests, capacity, flush and removal |
| `c/drivers/block/blkdev.c` | Block registration, synchronous submission, partition scanning and root selection |
| `c/drivers/virtio/virtio_net.c` | Packet rings, interrupts, link logging and removal |
| `c/drivers/net/netdev.c`, `c/drivers/net/net_ids.inc` | NIC binding, PCI matching and interrupt routing |
| `c/drivers/net/netring.h` | Ring arithmetic and network header size |
| `c/net/core/net.c` | Receive scheduling, softirq processing and polling diagnostics |
| `c/net/core/netcfg.c`, `c/net/core/dhcp.c` | Address configuration, boot deadline and background DHCP |
| `c/drivers/virtio/virtio_gpu.c`, `c/drivers/virtio/virtio_gpu.h` | Scanout resources, presentation, cursor commands and shutdown |
| `c/kernel/gui/fb/fb.c` | Framebuffer adoption, presentation and CPU writer draining |
| `c/kernel/gui/wm.c` | Cursor selection and compositor integration |
| `c/drivers/virtio/virtio_rng.c` | Entropy requests and probe diagnostic |
| `c/kernel/core/rng.c` | Kernel random generator |
| `c/drivers/virtio/virtio_balloon.c` | Page donation, target handling and ownership recovery |
| `c/drivers/virtio/virtio_scsi.c` | SCSI discovery, command transport and block callbacks |
| `c/kernel/init/kmain.c` | Early device initialization and later driver binding |
| `c/kernel/init/power.c` | Filesystem synchronization and power transitions |
| `tests/dma.mk`, `tests/unit/dma_virtio_test.c`, `tests/unit/dma_virtio_run.py` | DMA targets, host device model and fault controls |
| `tests/virtio_rng.mk`, `tests/boot/run-virtio-rng-test.sh`, `tests/boot/check-virtio-rng-line.py` | RNG boot checks and diagnostic controls |
| `tests/virtio_balloon.mk`, `tests/boot/run-virtio-balloon-test.py` | Balloon diagnostics and guest/host accounting |
| `tests/virtio_scsi.mk`, `tests/boot/run-virtio-scsi-test.py` | SCSI discovery, persistence checks and controls |
