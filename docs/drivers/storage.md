# Storage drivers

LogitOS exposes disks and partitions through the block device layer in `c/drivers/block/`. AHCI carries ATA commands over SATA using DMA, legacy ATA transfers sectors through CPU I/O ports, and NVMe uses submission and completion queues. The block layer handles capacity checks, partition offsets, request ownership and root selection, giving filesystems a common interface across these transports.

## Block devices and requests

`struct blkdev` in `c/drivers/block/blkdev.h` records a device’s name, capacity, operations and driver context. A partition also records its parent disk and starting sector. `BLK_SECTOR` defines the interface’s addressing unit as 512 bytes.

The public interfaces are implemented in `c/drivers/block/blkdev.c`:

| Interface | Purpose |
|---|---|
| `blk_register()` | Publish a disk with its operations and reported capacity. |
| `blk_dev_read()`, `blk_dev_write()`, `blk_dev_flush()` | Perform synchronous operations on a selected device. |
| `blk_req_init()`, `blk_submit()`, `blk_poll()` | Prepare, submit and advance a request across calls. |
| `blk_wait()` | Submit a request and poll it to completion, or return its completed status. |
| `blk_dev_batch()` | Submit read or write spans on a selected device and record each span’s status. |
| `blk_write_batch()` | Write spans through the selected root device. |
| `blk_read()`, `blk_read_n()`, `blk_write()`, `blk_flush()` | Access the selected root device. |

Reads and writes compare the requested count with the capacity remaining after the starting sector. This subtraction avoids overflow while containing access within the selected disk or partition. Submission adds the partition’s starting offset before invoking the driver.

`struct blk_ops` provides synchronous read, write and flush callbacks, a submit/poll pair, and an optional idle callback. AHCI and NVMe implement submit/poll. The ATA and VirtIO block adapters use synchronous callbacks, completing their requests during submission. `blk_wait()` calls the idle callback between polls when the driver provides one.

A `struct blk_req` carries the operation, buffer, sector range, progress and completion status. The submitter retains the request and buffer until completion. Drivers keep chunk progress in the request so that polling can complete one hardware command and issue the next.

## Ownership and DMA

Each whole disk owns an `io_lock_t` gate and a list of requests in submission order. Its partitions share that gate and request list. Registration sets admission depth to one; AHCI raises it when the controller and disk support Native Command Queuing, or NCQ. NVMe retains admission depth one despite having deeper hardware queues.

When admission reaches the depth limit, submission can drive an older request to completion. Overlapping ranges involving a write are serialised across the disk and its partitions. A flush drains earlier requests, and later requests wait for the flush to complete. These rules let another caller advance outstanding work while its original submitter is away from the CPU.

The gate serialises submission, polling and offlining across CPUs. Completion publishes the final request state with release ordering after driver cleanup; pollers observe it with acquire ordering. Submission rechecks the request’s state under the gate, preventing a completed request from being issued again. `blk_req_init()` prepares a request for reuse. These mechanisms are in `c/drivers/block/blkdev.c`; `c/drivers/core/io_lock.h` supplies the gate and interrupt-state handling.

A driver can return `BLK_E_AGAIN` for temporary backpressure. When an older request exists, the block layer drains it and retries admission. `blk_dev_batch()` manages up to sixteen requests at a time and waits for the entire group before reusing their storage. Each `struct blk_span` contains a device-relative LBA, sector count, buffer and completion status.

AHCI and NVMe allocate coherent metadata buffers and map payloads through `c/drivers/core/dma.c`. The DMA layer resolves kernel mappings to RAM pages, pins those pages and constructs scatter/gather segments. Device address masks and segment constraints can require a private bounce allocation. CPU aliases and device addresses have distinct representations.

For user buffers, the synchronous block wrapper allocates a 32 KiB staging buffer and copies chunks through the usercopy helpers. Direct request submission reports `BLK_E_NODMA` for a user pointer. Kernel buffers, including high aliases and mappings backed by scattered physical pages, reach the selected driver’s DMA mapper.

## Discovery and root selection

`c/kernel/init/kmain.c` enumerates PCI, calls `nvme_init()` and `virtio_blk_init()`, then calls `blk_init()` before mounting LogitFS.

Within `blk_init()`, disk registration follows this order:

| Driver group | Registration |
|---|---|
| NVMe | The selected namespace becomes `nvme0`. |
| VirtIO block | The detected device becomes `vblk0`. |
| AHCI | Discovered SATA disks receive names beginning with `ahci`. |
| VirtIO SCSI | The driver registers its discovered disks. |
| Legacy ATA | A successful primary-master IDENTIFY supplies `ata0`. |

NVMe, VirtIO block and ATA disks are scanned for partitions immediately after registration. AHCI and VirtIO SCSI register their disk groups first; the block layer then scans each group. This affects registry order when a driver discovers several disks.

Root selection walks the registry and reads sector zero of each candidate. The first candidate with the LogitFS magic, a recognised version and the expected block size becomes root. Mounting performs further validation in `c/fs/logitfs/logitfs.c` and `c/fs/logitfs/fsck.c`. If the search exhausts its candidates, the first whole disk becomes the mount candidate, giving mount diagnostics a device to report.

`blk_probe_partitions()` also publishes partitions for devices discovered later during boot. USB storage calls it from `c/drivers/usb/usb_storage.c`. The scanner records a completed scan on the disk and preserves the selected root.

## AHCI

`c/drivers/block/ahci.c` matches PCI class/subclass/programming-interface `01:06:01` and maps BAR5. Class matching provides the discovery path across controller vendors.

### Controller and port setup

Controller preparation follows this ownership sequence:

1. Enable MMIO decoding while preserving the firmware’s initial bus-master state.
2. Request firmware ownership release when the BIOS handoff capability is present.
3. Clear PCI bus mastering and confirm the result.
4. Stop the firmware command and FIS engines on implemented ports.
5. Enable bus mastering and prepare attached SATA disks.

PCI command readbacks and engine stop acknowledgements establish ownership before the driver installs its DMA addresses.

A port reporting a device with an incomplete link, or an active link with signature `0xffffffff`, receives COMRESET after installation of the command list and FIS receive area. The driver enables FIS reception, holds reset, waits for the link and signature, then starts the command engine. This path handles ports firmware left uninitialised.

Each accepted port receives a coherent metadata allocation sized for the controller’s command slots and rounded to whole pages:

| Offset | Contents |
|---|---|
| `0x000` | Command list |
| `0x400` | Received FIS area |
| `0x800 + slot * 0x100` | Command table and physical region descriptors for each slot |

The allocation ranges from 4 KiB for one slot to 12 KiB for 32 slots. The offsets provide the required alignments. CPU accesses use the allocation’s CPU alias; registers and descriptors receive its DMA address. Controller capability selects a 32-bit or 64-bit DMA mask.

### Commands and queueing

IDENTIFY supplies the model, capacity, logical sector size and LBA48 capability. NCQ requires LBA48, controller support and valid IDENTIFY queue information. Its depth is the smaller of the drive depth and the controller’s slot count minus one, capped at 31. A depth of at least two enables FPDMA QUEUED reads and writes. One slot per port is reserved for internal commands.

The earlier description of AHCI as a single-slot polling driver is retained as historical commentary in `c/drivers/block/ahci.c`; the current implementation combines NCQ data slots with late MSI attachment.

Submission assigns each block request a data slot and retains it across chunks. Commands sent without NCQ use slot zero and select DMA EXT for LBA48 devices or DMA for LBA28 devices. Each chunk is bounded by `DMA_MAX_MAPPING`, defined as 1 MiB in `c/drivers/core/dma.h`, and the command’s addressing and count limits. The eight-entry PRDT can represent 32 MiB, while the DMA mapping bound determines the active transfer limit.

For commands sent without NCQ, polling checks the command-issue bit, task-file errors and the transferred byte count for reads. A short read triggers recovery. NCQ completion follows the SATA active bitmap. Each slot retains its command specification, request and DMA mapping.

### Recovery

Block commands receive an eight-second deadline per attempt. Recovery snapshots active commands before stopping the engines, then consumes successful completions before resetting DMA ownership for the port. This preserves completed read data before quiescence invalidates its DMA token.

For an NCQ task-file error that permits recovery without a link reset, READ LOG EXT page `0x10` supplies the failed tag, status, error and LBA. A valid tag charges the retry to that command; otherwise recovery charges every unfinished command. A retry counter reaching eight fails the port.

COMRESET handles a busy device, fatal transport error, the first NCQ timeout, repeated timeouts without NCQ, or an unavailable NCQ error log. After three recoveries involving NCQ, admission depth drops to one. Existing NCQ requests retain their protocol through their remaining chunks, allowing the outstanding queue to drain.

The IDENTIFY bring-up path uses the same command and DMA machinery with a bounded spin budget.

### Interrupt attachment and diagnostics

Root mounting runs with AHCI interrupts masked. After `smp_init()` and `dev_probe_all()`, `ahci_irq_attach()` attempts to obtain one MSI vector per controller and issues IDENTIFY to test delivery. A 50 ms check selects MSI assistance or polling. If IDENTIFY remains outstanding at that point, the driver drains it under the command recovery policy.

The MSI destination CPU can halt between polls; other CPUs and polling mode use a pause loop. The ISR records status and completion observations. Polling under the medium gate advances requests and releases their DMA mappings. A short interrupt gate coordinates status collection with recovery epochs.

Late attachment also compares up to sixteen disjoint 4 KiB NCQ reads with serial DMA EXT reads. A failed comparison lowers queue depth to one. Failure of the serial fallback offlines the port.

`ahci_diag()`, declared in `c/drivers/block/ahci.h`, reports queue depth, interrupt mode, interrupt counts, completions found by polling, maximum unreaped commands, errors, recoveries and link resets.

## Legacy ATA

`c/drivers/block/ata.c` implements PIO on the primary ATA channel’s master device. IDENTIFY supplies the model and LBA28 capacity used to register the disk.

Each sector moves through 256 reads or writes of the 16-bit data port. Status polling waits for the busy bit to clear and the data-request bit to become ready. Transfers enable interrupts during their polling window and restore the caller’s interrupt state afterward. Each read or write command receives up to eight attempts.

The adapter in `c/drivers/block/blkdev.c` splits requests into pieces of at most 255 sectors to fit the ATA interface’s count argument. The ATA driver checks the complete LBA28 range before programming the task-file registers, preventing address truncation.

After transferring write data, the driver issues FLUSH CACHE and waits for the busy bit to clear.

## NVMe

`c/drivers/block/nvme.c` selects an enumerated PCI function with class/subclass/programming-interface `01:08:02` and maps its sized BAR0 resource. PCI enumeration follows bridges in `c/kernel/pci/pci.c`, allowing discovery on downstream buses.

Initialisation confirms controller stop, allocates coherent admin queues, programs their DMA addresses, enables PCI bus mastering and starts the controller. Identify commands supply the model, transfer limit and namespace format. Namespace selection uses the first active namespace reported by the controller, with namespace 1 as the fallback.

Before creating I/O queues, the driver negotiates their number and reads the granted submission and completion queue counts. It also checks that queue doorbells fit within the mapped BAR.

Admin and I/O queues use depth 64. Submission chooses an I/O queue from the submitting CPU’s index, falling back to queue zero when the index exceeds the created queue set. The request tag retains the queue index and command identifier so another CPU can poll it.

Payloads use Physical Region Page pointers, or PRPs. The first pointer addresses the initial data page. Transfers extending beyond that page use a second page pointer or a coherent PRP list owned by the queue. Chunk selection applies the controller transfer limit and the driver’s 1 MiB PRP bound.

### Native 4 KiB blocks

NVMe accepts namespaces with 512-byte or 4096-byte logical blocks. It converts namespace capacity into the block layer’s 512-byte sectors and converts addresses and counts back to native blocks when constructing commands.

A partial native block uses a coherent 4 KiB sector buffer. Reads fetch the complete native block and copy the requested bytes to the caller. Writes read the native block, merge the caller’s bytes and submit the complete block for writing. Request progress advances after that write completes.

The medium’s admission depth remains one across both commands. Requests through different partitions share that ownership, serialising partial writes even when their 512-byte ranges occupy different parts of the same native block.

### Completion and health

Polling consumes completion entries using their phase bit and command identifier. Each block command receives a ten-second deadline. On timeout, the driver disables the controller and waits for its ready bit to clear before releasing the request’s DMA resources.

During initialisation, NVMe attempts to read and print SMART/health log page `0x02`. `nvme_health()` refreshes temperature, available spare capacity, endurance consumed, power counters and media errors. Failed health reads produce a diagnostic.

## Partitions

`c/drivers/block/part.c` reads partition tables through a sector-read callback. Registered devices and synthetic disk images use the same parser.

The MBR parser examines four primary entries and follows extended partition chains. Logical partition offsets are relative to their EBR; links to subsequent EBRs are relative to the extended container. The walk requires forward progress, checks EBR locations against the container and device, and stops after at most 128 iterations. Accepted partition data ranges are checked against device capacity.

GPT parsing checks the header CRC and streams the entry array through a CRC calculation. A 512-byte assembly buffer reconstructs entries that cross sector boundaries, avoiding allocation of the complete array. The caller receives accepted entries after the array checksum succeeds. Failure of the primary GPT leads to a probe of the backup header at the final device sector.

An MBR signature permits GPT probing, including hybrid layouts. If GPT probing fails, a protective MBR produces an empty result; other signed layouts proceed to MBR parsing.

`blk_probe_partitions()` publishes accepted entries as devices named by appending `p` and a number to the disk name, such as `ahci0p1`. Numbers follow accepted parser entries in order. Each partition inherits its disk’s operations and driver context, with its own offset and length. GPT names become diagnostic labels, retaining printable ASCII characters and replacing other characters with `?`.

## Flush ordering

`blk_flush()` routes a flush through the root device and counts calls. Backend operations are:

| Backend | Flush path |
|---|---|
| AHCI | FLUSH CACHE EXT or FLUSH CACHE, selected by LBA48 capability. |
| NVMe | NVM Flush for the selected namespace. |
| ATA PIO | FLUSH CACHE follows each write command’s data transfer. |
| VirtIO block | A flush request when the feature was negotiated. |
| VirtIO SCSI | SYNCHRONIZE CACHE(10), with completion status checked. |
| USB storage | SYNCHRONIZE CACHE(10), with completion required before success. |

The implementations reside in `c/drivers/block/ahci.c`, `c/drivers/block/nvme.c`, `c/drivers/block/ata.c`, `c/drivers/virtio/virtio_blk.c`, `c/drivers/virtio/virtio_scsi.c` and `c/drivers/usb/usb_storage.c`.

In `c/fs/cache/bcache.c`, `bcache_sync()` submits dirty cache blocks through `blk_write_batch()` in groups of up to sixteen. It clears successfully written blocks’ dirty flags and retains failed blocks for another sync. The final flush follows completion of every batch. The function reports errors from writeback or flushing.

## Removal and reboot

AHCI removal offlines and drains the controller’s block devices, stops their ports, then releases DMA resources. NVMe follows the same sequence at controller scope. Offlining retains registry objects so existing callers can receive errors through their stored device handles.

Both drivers require hardware stop acknowledgement before releasing outstanding DMA resources. An unconfirmed stop quarantines DMA state and panics, preserving caller memory while hardware may still access it.

During reboot, `dev_quiesce_all()` in `c/drivers/core/device.c` keeps bound PCI storage controllers attached and reads back PCI bus mastering. An enabled bit counts as unconfirmed DMA because filesystem draining can leave a FIS receiver or raw swap I/O active. With DMA unconfirmed, `c/kernel/init/power.c` attempts platform reset paths and halts if they return.

## Test coverage

The repository defines these checks:

| Source | Behaviour exercised |
|---|---|
| `tests/unit/blkreq_test.c` | Request completion, partition offsets, medium ownership, user staging and offlining with fixture drivers. |
| `tests/unit/part_test.c` | Synthetic MBR and GPT images, malformed ranges, EBR cycles, CRC failures, entries crossing sectors and backup recovery. |
| `tests/unit/storage_hardware_test.py` and `tests/unit/storage_hardware_test.c` | Selected production discovery, namespace-format and AHCI ownership functions against fixtures. |
| `tests/unit/block_dma_test.py` and `tests/unit/block_dma_test.c` | Production command and stop functions against DMA/MMIO fixtures, including partial native-block transfers. |
| `tests/boot/run-storage-hardware-test.py` | QEMU topology and sector-format cases, with filesystem byte checks across two boots through shared harness code. |
| `tests/boot/run-ahci-test.sh` | AHCI root selection and file contents for raw disks, MBR, GPT and two-controller configurations. |
| `c/drivers/block/ahci.c` | Boot self-tests for MSI delivery and queued-read comparison against serial DMA EXT reads. |

`make test-blk-async`, `make test-storage-hardware-host` and `make test-dma-block` select the host fixtures through `tests/mem.mk`, `tests/storage_hardware.mk` and `tests/dma.mk`. Each target has a negative-control prerequisite. `make test-ahci` runs four AHCI configurations through `Makefile`; `make test-ahci-sector` selects the sector-format harness through `tests/storage_hardware.mk`.

## Limitations

- The block registry holds 24 devices, including partitions. The partition parser retains 16 partitions per disk and accepts GPT arrays containing at most 1024 entries.
- AHCI examines up to four controllers and retains up to eight disks. It accepts 512-byte logical sectors, including 512e media. ATAPI and port multipliers are unsupported. Interrupt attachment supports MSI with polling fallback.
- Device unbinding releases the interrupt vector before `ahci_shutdown()` masks controller interrupt sources. `c/drivers/block/ahci.c` records this interval as a removal lifecycle risk.
- Legacy ATA addresses the primary master through LBA28, giving a 128 GiB address range, and assumes 512-byte sectors. Its post-write flush wait checks the busy bit; `ata_flush()` returns success without issuing another command or checking the flush error status.
- VirtIO block returns success from its flush path when the flush feature was not negotiated.
- NVMe maintains one controller and one selected namespace, with up to four I/O queue pairs. Namespace metadata and protection information are unsupported. Health counters retain the low 64 bits of the controller’s 128-bit fields.
- Partition parsing uses 512-byte table sectors. NVMe’s native 4 KiB data translation does not translate partition-table LBAs expressed in native 4 KiB units. Overlapping accepted partitions remain published with a diagnostic warning.
- Registration and partition probing serve serialised boot discovery. Runtime hotplug and rescanning mounted partition tables are outside that interface.
- The AHCI source records MSI, NCQ and recovery work as unverified on X79-VG4 hardware. Physical-device coverage must be stated per device under `TRANSPARENCY.md`. The QEMU storage harness records a skip when its IDE model rejects native 4 KiB logical sectors.

## See also

- [Storage stack](../architecture/storage-stack.md)
- [Device and driver model](../architecture/device-model.md)
- [PCI](../kernel/pci.md)
- [VirtIO drivers](virtio.md)
- [USB](usb.md)
- [Block cache](../filesystems/block-cache.md)
- [Running on real hardware](../getting-started/real-hardware.md)

## Source map

| File | What it contains |
|---|---|
| `c/drivers/block/blkdev.h` | Block device, operation, request and span structures; public interfaces and limits. |
| `c/drivers/block/blkdev.c` | Registry, request admission, ownership, bounds checks, user staging, partitions and root selection. |
| `c/drivers/block/ahci.c` | Controller handoff, port setup, NCQ, DMA commands, recovery, MSI attachment and removal. |
| `c/drivers/block/ahci.h` | AHCI initialisation, interrupt attachment, diagnostics and removal interfaces. |
| `c/drivers/block/ata.c` | Primary-master IDENTIFY, PIO transfers, retries and write flushing. |
| `c/drivers/block/nvme.c` | Controller setup, queue negotiation, PRPs, native-block translation, completion and health reporting. |
| `c/drivers/block/part.h` | Partition records, parser interface and retained-entry limit. |
| `c/drivers/block/part.c` | MBR, extended-chain and GPT parsing. |
| `c/drivers/core/io_lock.h` | Atomic device gates and interrupt-state handling. |
| `c/drivers/core/dma.h` | DMA addresses, constraints, mapping bounds and ownership interfaces. |
| `c/drivers/core/dma.c` | RAM resolution, pinning, scatter/gather mappings and bounce allocations. |
| `c/drivers/core/device.c` | Device lookup, BAR mapping, unbinding and storage DMA checks before reboot. |
| `c/drivers/virtio/virtio_blk.c` | VirtIO block transfers and flush handling. |
| `c/drivers/virtio/virtio_scsi.c` | SCSI disk discovery, block operations and cache synchronisation. |
| `c/drivers/usb/usb_storage.c` | USB storage operations, registration and partition probing. |
| `c/kernel/pci/pci.c` | PCI enumeration and bridge traversal. |
| `c/kernel/init/kmain.c` | Storage initialisation, root mounting and late AHCI interrupt attachment. |
| `c/kernel/init/power.c` | Filesystem draining and reboot handling with unconfirmed DMA. |
| `c/fs/logitfs/logitfs.c` | Root filesystem mounting. |
| `c/fs/logitfs/fsck.c` | Filesystem geometry validation. |
| `c/fs/cache/bcache.c` | Dirty-block batching, writeback status and final flushing. |
| `tests/unit/blkreq_test.c` | Block request engine fixtures. |
| `tests/unit/part_test.c` | Partition parser fixtures. |
| `tests/unit/storage_hardware_test.py` | Production-function extraction and discovery controls. |
| `tests/unit/storage_hardware_test.c` | Discovery, namespace-format and ownership fixtures. |
| `tests/unit/block_dma_test.py` | Production command-function extraction and DMA controls. |
| `tests/unit/block_dma_test.c` | DMA/MMIO and native-block transfer fixtures. |
| `tests/boot/run-storage-hardware-test.py` | QEMU storage topology and sector-format cases. |
| `tests/boot/run-virtio-scsi-test.py` | Shared QEMU setup, filesystem byte checks and reboot verification. |
| `tests/boot/run-ahci-test.sh` | AHCI raw-disk, partition and multiple-controller boot checks. |
| `Makefile` | AHCI boot-test targets. |
| `tests/mem.mk` | Block request fixture targets and interlock control. |
| `tests/storage_hardware.mk` | Storage discovery, topology and sector-format test targets. |
| `tests/dma.mk` | Block DMA fixture targets and controls. |
| `TRANSPARENCY.md` | Rules for implementation and validation claims. |