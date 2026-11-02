# LogitFS

LogitFS provides the persistent root file system for LogitOS. It stores files and directories in fixed-size inodes, tracks block allocation with a bitmap, and commits metadata through a journal. Replacing a file writes fresh data blocks before publishing their metadata. The C implementations share the format definitions in `c/fs/logitfs/logitfs_fmt.h`; `tools/mkfs.py` mirrors that format in Python.

## Integration

Kernel initialisation discovers block devices, registers LogitFS with the VFS, and mounts the root file system in `c/kernel/init/kmain.c`. The root-device probe in `c/drivers/block/blkdev.c` recognises the LogitFS magic, supported version, and block size. The writable driver accesses the selected device through `c/fs/cache/bcache.c`.

For an AHCI root device, mounting uses polling. After SMP setup and device probing, `ahci_irq_attach()` attempts MSI attachment and checks interrupt delivery, retaining polling when needed. This ordering appears in `c/kernel/init/kmain.c` and `c/drivers/block/ahci.c`.

The writable driver keeps its superblock, bitmap, inode table, staging buffers, and transaction state in shared mount state. A filesystem lock spans each VFS operation, including metadata preparation and commit. This prevents concurrent callers from combining changes in the shared journal buffers. The locking rationale is documented in `c/fs/logitfs/logitfs.c`.

The registered `struct filesystem logitfs` supplies whole-file reads and replacement writes, positional reads, directory operations, attributes, and capacity reporting.

The root image also carries device firmware. `Makefile` packs `third_party/amd-firmware/amdgpu` at `/lib/firmware/amdgpu`, with its licence and manifest under `/licenses/amd-firmware`. Individual firmware binaries are image prerequisites. Resource discovery follows root mounting and GPU driver binding; `c/drivers/gpu/amd/polaris/resource/files.c` reads firmware through the VFS and checks its metadata, format, and pinned hashes.

## Versioned disk format

LogitFS uses 4,096-byte blocks, each containing eight 512-byte sectors. Block numbers are relative to the selected block device. The block layer applies partition offsets.

Block zero begins with `struct lfs_super`: thirteen little-endian 32-bit fields occupying 52 bytes. These describe the magic, version, block size, volume size, inode count, bitmap, inode table, data area, root inode, and journal. `fsck_super_valid()` in `c/fs/logitfs/fsck.c` checks region ordering and bounds, bitmap coverage, inode-table capacity, and journal capacity.

| Version | Definition | Meaning |
|---|---|---|
| 4 | `LFS_VERSION` | Format produced by `tools/mkfs.py`, including journal, timestamps, mode, and ownership fields |
| 5 | `LFS_ID_VERSION` | Adds persistent volume and object identities, revisions, and an object-allocation counter |

The geometry validator and root-device probe accept both versions. Version 5 retains the block geometry and 128-byte inode layout.

The defaults in `tools/mkfs.py` are 131,072 blocks, a logical size of 512 MiB, and 8,192 inode slots:

| Region | Default block range | Contents |
|---|---|---|
| Superblock | 0 | Format and geometry fields |
| Allocation bitmap | 1–4 | One allocation bit per block |
| Inode table | 5–260 | 128-byte inode records |
| Journal | 261–324 | One header block and 63 body slots |
| Data area | 325 onward | File data, directory records, and indirect blocks |

The mounted driver derives buffer sizes and offsets from the superblock. These ranges describe the image builder’s defaults.

## Inodes and directories

`struct lfs_dinode` in `c/fs/logitfs/logitfs_fmt.h` occupies 128 bytes:

| Byte offset | Contents |
|---|---|
| 0–3 | Type and padding |
| 4–7 | File or directory size, as a 32-bit byte count |
| 8–55 | Twelve direct block pointers |
| 56–59 | Single-indirect block pointer |
| 60–63 | Double-indirect block pointer |
| 64–87 | Access, modification, and inode-change timestamps |
| 88–99 | Mode, user ID, and group ID |
| 100–107 | Version 5 object ID |
| 108–115 | Version 5 revision |
| 116–123 | Version 5 allocation counter, used in the root inode |
| 124–127 | Reserved bytes |

Each indirect block holds 1,024 32-bit block numbers. Both `c/fs/logitfs/logitfs.c` and `c/fs/logitfs/lfsro.c` traverse direct pointers, then the single-indirect block, then the double-indirect tree.

Inode types identify free slots, regular files, and directories. A directory contains 64-byte `struct lfs_dirent` records: a 32-bit inode number and a 60-byte name field. The writable driver stores names with a zero terminator, allowing 59 bytes of name content. An entry whose first name byte is zero is available for reuse. The writable driver handles `.` and `..` during path resolution.

Timestamps contain whole seconds since the Unix epoch; zero means unset. Reads update access time in the mounted inode table. A later transaction that stages the containing inode-table block persists that value. This avoids the three journal flushes for every read.

The mode field uses `LFS_MODE_SET` to distinguish stored permissions from an unset field, including a stored mode of zero. Attribute reads supply 0755 for directories and 0644 for files when the field is unset. Timestamps, mode, and ownership occupy formerly reserved bytes, preserving the layout of older version 4 images.

## Persistent identities

Version 5 places `struct lfs_identity_super` immediately after the 52-byte superblock prefix. The extension occupies 24 bytes and contains a magic value, a 128-bit volume identity, and a CRC32 covering its first twenty bytes.

Allocated inodes carry nonzero object IDs and revisions. The root inode stores the allocation counter, which inode allocation increments to assign new IDs. `flush_bitmap()` stages the root inode along with allocation metadata, placing the counter and newly published object in the same journal transaction. Content rewrites advance the affected inode’s revision; directory rewrites advance the directory’s revision.

`fsck_identity_valid()` in `c/fs/logitfs/fsck.c` checks the extension checksum, a nonzero volume identity, uniqueness of allocated object IDs, nonzero revisions, and the counter’s coverage of allocated IDs.

`tools/project_volume.py` converts a version 4 image into a new version 5 image. It obtains a recovered and checked input copy through `tools/lfs_snapshot.c`, assigns volume and inode identities, then validates the converted image through the same helper.

## Allocation and replacement writes

The allocation bitmap covers the entire volume, including metadata. `balloc()` in `c/fs/logitfs/logitfs.c` selects the lowest free block in the data area. An allocation hint skips a prefix known to be occupied. Clearing a lower allocation bit lowers the hint; loading or restoring the bitmap resets it. Inode allocation scans for an inode with the free type.

A replacement write builds a fresh block tree while the previous tree remains allocated. `bfree()` records pending releases in a bitmap of deferred frees. `flush_bitmap()` applies these releases when staging the allocation bitmap.

This ordering protects the previous file contents during preparation. Writes of file data precede the metadata commit. Reusing a block from the old file during that preparation could overwrite bytes still referenced by the previous inode. Deferred release keeps those blocks allocated until preparation is complete.

Create, overwrite, directory creation, deletion, and rename save the affected inodes and bitmap before preparing their changes. Their abort paths restore that state when preparation fails.

Deletion has a path for a full volume. When the directory’s block count stays constant, `inode_rewrite_logged()` stages its existing data blocks in the journal. Deletion can then release file blocks using the space the directory already owns. The reason for this path and its use by deletion are documented in `c/fs/logitfs/logitfs.c`.

## Journal and commit ordering

The journal contains complete metadata block images: allocation bitmaps, inode-table blocks, indirect pointer blocks, and directory contents. Regular file data goes to newly allocated destinations before the metadata referencing it commits.

For a transaction with staged metadata, `log_commit()` in `c/fs/logitfs/logitfs.c` performs three ordered stages:

1. Write journal bodies and flush. This also persists preceding file-data writes.
2. Write the journal header and flush. The durable header is the commit record.
3. Install the logged blocks at their target locations and flush.

The driver then writes a cleared header. A later synchronisation, including the next transaction’s first flush, persists that clear.

The header contains an informational transaction generation, a body count, target block numbers, a CRC32 over the bodies in slot order, and a CRC32 over the header. The header checksum detects torn records. The body checksum detects a stale header over body slots containing bytes from a later transaction.

The case analysis above `log_commit()` explains how recovery selects the state before the mutation or completes the committed mutation. Before a valid commit record reaches media, the prior metadata still describes the file system. After the record reaches media, replay installs the logged metadata. Flushing installation before clearing the record permits replay after another interruption.

`bcache_sync()` in `c/fs/cache/bcache.c` writes dirty buffers before issuing the device flush. It groups up to sixteen dirty blocks into arrays of `struct blk_span`, defined in `c/drivers/block/blkdev.h`, and calls `blk_write_batch()`. The batch waits for its submitted requests and resolves addresses through the selected root device, including its partition offset. Successful spans become clean; failed spans remain dirty. Writeback or device-flush failure produces an error.

In `c/drivers/block/blkdev.c`, a disk and its partitions share a request queue. A flush waits for outstanding requests and completes before later requests are admitted. Overlapping ranges involving a write are serialised. Drivers configure queue depth; the AHCI driver can use Native Command Queuing for eligible controllers and disks.

`logitfs_sync()` flushes the filesystem cache and device. Buffering for open files sits above the transaction boundary: `file_fsync()` in `c/kernel/exec/fd/file.c` sends a dirty file description’s complete buffered contents through `vfs_write()`, reaching the LogitFS replacement operation.

## Mount and recovery

The writable mount validates the superblock geometry and, for version 5, the volume extension. It allocates its buffers and invokes journal recovery before loading the bitmap and inode table. It then validates the loaded version 5 inode identities.

`fsck_log_recover()` in `c/fs/logitfs/fsck.c` checks the header, body count, target bounds, and body checksum. Valid transactions are installed in slot order. Recovery synchronises those installations before clearing the header.

Headers carrying the journal magic but failing checksum or count validation are cleared when a write callback is available. A record with a forbidden target returns an error. Forbidden targets include block zero, the journal itself, and blocks beyond the volume.

After loading the tables, the writable mount invokes a diagnostic consistency scan and prints its result with the `[fsck] mount check:` prefix. Repair is available through the checker interface.

## Read-only mounts

`c/fs/logitfs/lfsro.c` supplies filesystem instances associated with named block devices. `lfsro_create()` assigns each instance its own superblock, staging buffers, and operation lock. Reads go directly through the selected device.

The reader supplies file sizes, reads, positional reads, and directory enumeration. Mounting validates geometry through the shared superblock validator.

The control request `mount lfsro <blkdev> <at>` is dispatched by `c/fs/ctl/vfsctl.c` through `/dev/vfsctl`. It attaches the reader at an existing directory. The syntax is documented in `c/fs/ctl/vfsctl.h`; mount-point validation lives in `c/fs/vfs/vfs.c`.

## Consistency checking and repair

`fsck_run()` operates through `struct fsck_dev` in `c/fs/logitfs/fsck.h`. Block-read, block-write, and synchronisation callbacks let the same checker serve kernel callers and host tools.

The scan examines geometry, journal state, the root inode, version 5 identities, inode block chains, directory reachability, and bitmap agreement. Directory traversal uses a queue allocated on the heap, keeping directory depth off the kernel call stack.

After initial validation, repair paths in `c/fs/logitfs/fsck.c` handle these findings:

| Finding | Repair behaviour |
|---|---|
| Referenced block marked free | Set its allocation bit |
| Allocated block without a reference | Clear its allocation bit |
| Size exceeds the mapped prefix | Truncate the size and trim affected pointers |
| Directory size contains a partial record | Round down to complete directory records |
| Entry names an invalid or free inode | Remove the entry |
| Entry revisits a directory or names an already reached inode | Remove the additional entry |
| Allocated inode unreachable from the root | Clear the inode and reclaim its blocks |
| Unknown inode type | Clear the inode |
| Duplicate block reference detected by repair preflight | Refuse the repair pass before writing |
| Invalid geometry, root type, or version 5 identity state | Report a fatal result |

Repairs can remove names and shorten files. Refusal to repair duplicate blocks preserves the competing references because the stored structure provides no basis for choosing an owner.

`logitfs_fsck(0)` checks the mounted writable filesystem; a nonzero argument requests repair. The wrapper holds the filesystem lock, flushes cached writes before checking, and reloads the bitmap and inode table after a repair attempt.

For host image processing, `tools/lfs_snapshot.c` opens the source read-only and replays its journal into a private memory mapping. It checks the recovered view and writes a new snapshot after validation succeeds. The preservation workflow used by `tools/mkfs.py` obtains its input through this helper.

## Test coverage

The repository defines the following checks. These descriptions identify assertions in their sources.

| Target | Source and scope |
|---|---|
| `test-fs-cache` | `tests/unit/fs_cache_test.c`: cached reads, deferred writes, eviction, flush ordering, and persistence across simulated power cuts |
| `test-fs-format` | `tests/unit/fs_format_test.c`: structure sizes, field offsets, image fields, and consistency |
| `test-fs-journal` | `tests/unit/fs_journal_test.c`: replay, torn headers, stale bodies, count bounds, and rejected targets |
| `test-fs-crash` | `tests/unit/fs_crash_test.c`: interrupted creation, overwrite, directory creation, deletion, and rename |
| `test-fsck` | `tests/unit/fs_fsck_test.c`: damaged images, repairs, refusal, and surviving file contents |
| `test-fs-abort` | `tests/unit/fs_abort_test.c`: continued use of a mounted volume after staging failure |
| `test-fs-full-delete` | `tests/unit/fs_full_delete_test.c`: deletion after exhausting free blocks |

The simulator in `tests/unit/fsstub/fs_sim.h` models a volatile device cache. A simulated power cut persists a selected subset of pending writes and can tear one block at 512-byte sector boundaries. The crash sweep checks file bytes, directory state, allocation consistency, and subsequent allocation. Its batch adapter sends each span through the simulated write function, retaining the device-write cut points.

Recipes live in `Makefile` and `tests/fs_rename_space.mk`. The latter supplies control prerequisites for the crash, abort, and full-volume deletion checks.

## Limitations

- The writable driver supports one mounted volume. The read-only instance pool has two slots; those mounts skip journal replay, a full consistency scan, and version 5 identity validation.
- Replacement writes accept a signed `int` length and require fresh blocks plus journal capacity. The default journal has 63 body slots. Each staging call consumes a slot, including repeated targets.
- In check mode, `fsck_run()` reports pending committed journal blocks while scanning their installed destinations. The snapshot helper recovers its private view before checking.
- Checker repairs write outside the filesystem transaction protocol. Directory-repair write errors and the mounted wrapper’s synchronisation result after repair go unchecked.
- `log_commit()` records I/O errors but continues its remaining stages. The mount recovery wrapper treats a negative recovery result as an ignored journal and continues; findings from the diagnostic mount scan also permit mounting to continue.
- The batched writeback path is marked as unverified on X79-VG4 hardware in `c/fs/cache/bcache.c`.
- `TRANSPARENCY.md` retains an older statement describing journaling and crash consistency as undocumented. The current implementation documents a metadata journal and an argument about power loss above `log_commit()` in `c/fs/logitfs/logitfs.c`; that argument assumes successful I/O and device flushes.

## Source map

| File | What it contains |
|---|---|
| `c/fs/logitfs/logitfs_fmt.h` | Format versions, superblock, inode, directory, and journal layouts |
| `c/fs/logitfs/logitfs.c` | Writable driver, allocation, transactions, mounting, attributes, and checker wrapper |
| `c/fs/logitfs/fsck.h` | Checker callbacks, reports, and interfaces |
| `c/fs/logitfs/fsck.c` | Geometry and identity validation, journal recovery, scans, and repairs |
| `c/fs/logitfs/lfsro.c` | Read-only instances and block traversal |
| `c/fs/cache/bcache.c` | Cached block I/O, batched writeback, and device flushes |
| `c/drivers/block/blkdev.c` | Root-device probing, partition addressing, queues, and batch I/O |
| `c/drivers/block/blkdev.h` | Block requests, queue depth, and batch span interface |
| `c/drivers/block/ahci.c` | AHCI polling, MSI attachment, and command queueing |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware reads and validation |
| `c/kernel/init/kmain.c` | Root mounting and later driver attachment |
| `c/kernel/exec/fd/file.c` | Buffered file-description flushes |
| `c/fs/vfs/vfs.c` | Root mounting and mount-point validation |
| `c/fs/ctl/vfsctl.c` | Read-only mount control dispatch |
| `c/fs/ctl/vfsctl.h` | Filesystem control command syntax |
| `tools/mkfs.py` | Version 4 image construction and default geometry |
| `tools/project_volume.py` | Version 5 conversion and identity handling |
| `tools/lfs_snapshot.c` | Recovery and validation of private image copies |
| `tools/disk_profile.py` | Image preservation through the snapshot helper |
| `tests/unit/fs_cache_test.c` | Cache, writeback, and flush assertions |
| `tests/unit/fs_format_test.c` | Format and image-layout assertions |
| `tests/unit/fs_journal_test.c` | Journal validation and replay cases |
| `tests/unit/fs_crash_test.c` | Power-cut sweeps and content assertions |
| `tests/unit/fs_fsck_test.c` | Corruption and repair cases |
| `tests/unit/fs_abort_test.c` | State restoration after staging failure |
| `tests/unit/fs_full_delete_test.c` | Deletion with exhausted block space |
| `tests/unit/fsstub/fs_sim.h` | Simulated media, volatile writes, and power cuts |
| `Makefile` | Image packaging and core filesystem test recipes |
| `tests/fs_rename_space.mk` | Mutation-failure tests and control prerequisites |
| `TRANSPARENCY.md` | Project assertion rules and the older consistency statement |

## See also

- [Virtual file system](vfs.md)
- [Block cache](block-cache.md)
- [File-system control and tools](fs-tools.md)
- [Storage stack](../architecture/storage-stack.md)
