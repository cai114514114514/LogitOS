# Block cache

The block cache sits between filesystem operations and the selected root block device, holding LogitFS data and metadata. It serves repeated reads from a fixed buffer pool and defers writes until eviction or synchronization. LogitFS uses synchronization barriers to order journal phases. File reads that populate the kernel page cache also pass through this layer, so a miss in the page cache can reuse resident blocks. The implementation and interface are in `c/fs/cache/bcache.c` and `c/fs/cache/bcache.h`.

## Design and ownership

Each buffer holds one 4 KiB block. `BC_BS` defines this size, matching `LFS_BS` in `c/fs/logitfs/logitfs_fmt.h`. A block spans eight 512-byte device sectors. The default pool contains `BC_NBUF`, or 256 buffers, providing 1 MiB of payload storage plus headers.

`bcache_init()` allocates the headers and payloads from the kernel heap. A hash table with 256 buckets indexes resident blocks by block number. An intrusive list tracks recency: recently accessed buffers move to the head, and allocation selects the tail. Free slots occupy the tail, which gives them priority over resident entries.

A single global pool serves the selected root device through the wrappers in `c/drivers/block/blkdev.c`. File data, directory blocks, indirect blocks, and other filesystem metadata share that pool. Caching directory and indirect blocks saves repeated reads during path resolution and file block mapping.

LogitFS holds `lfs_lock` across each filesystem operation, including its transaction, staging buffers, and cache accesses. This scope keeps reads consistent with metadata staged by the current operation. The correction at the top of `c/fs/cache/bcache.h` supersedes its older BKL ownership note; the current wrappers are in `c/fs/logitfs/logitfs.c`.

## Reads and eviction

`bcache_read()` copies a resident block into the caller’s buffer and updates its recency. On a miss, it claims the tail buffer, reads the requested block into it, and copies the result to the caller.

Claiming a dirty victim first writes its contents through `blk_write()`. On a write error, the victim stays resident and dirty, and the requested read goes directly into the caller’s buffer. A failed device read into a newly claimed entry releases that entry for reuse. These paths are implemented in `c/fs/cache/bcache.c`.

`bcache_read_run()` handles consecutive blocks. It copies resident blocks from the pool and groups adjacent misses into requests through `blk_read_n()`. A resident block ends a group of misses. This preserves coherence when a dirty buffer holds newer bytes than the device.

The run reader installs fetched blocks when the complete request contains at most one quarter of the pool. With 256 buffers, that threshold is 64 blocks, or 256 KiB. Larger requests deliver misses directly to the destination, protecting reusable blocks from displacement by a long stream. A victim write error stops installation of the current fetched group; its bytes are already in the caller’s buffer.

LogitFS’s `inode_pread()` groups consecutive physical block numbers and passes them through `bread_run()`. Its default ceiling, `READ_RUN`, is 128 blocks, or 512 KiB. Partial blocks pass through `bread()`. Whole file reads and offset reads share this walker in `c/fs/logitfs/logitfs.c`.

## Write-back policy

`bcache_write()` copies a complete block into a resident or newly claimed buffer, marks it dirty, and updates its recency. Allocation can first write an older dirty victim. On a victim write error, the requested write goes directly through `blk_write()`, while the victim remains cached.

The main write-back paths are:

| Path | Action |
|---|---|
| Eviction | Writes a dirty victim before reusing its buffer |
| `bcache_sync()` | Attempts every dirty buffer, then issues `blk_flush()` |
| `bcache_shutdown()` | Attempts writes for dirty buffers, then frees the pool |

`bcache_sync()` gathers dirty buffers into batches of up to sixteen `struct blk_span` entries. Each span describes one 4 KiB cache block. It submits these through `blk_write_batch()`, whose implementation in `c/drivers/block/blkdev.c` waits for every submitted request before returning. The batch bound limits stack use, and `lfs_lock` protects the cache buffers throughout synchronization.

Successful spans become clean and increment the write-back counter. Failed spans remain dirty for retry. Synchronization continues through the pool and issues `blk_flush()` after the writes, including when a write failed. It returns `0` when all writes and the flush succeed, or `-1` on failure. With the pool inactive, it forwards directly to `blk_flush()`.

The block layer applies the selected partition’s offset and manages admission on the underlying medium, whose queue is shared by its partitions. Devices begin with a queue depth of one; drivers can raise it through `blk_set_queue_depth()`. Admission waits for capacity and for overlapping requests involving a write. A flush drains earlier requests and completes before later requests enter the queue. These rules let eligible batch writes overlap while preserving flush ordering.

Under the cache contract, dirty buffers can reach the device at any time and in any order. LogitFS establishes ordering through completed synchronization barriers, as described in `c/fs/cache/bcache.h` and implemented in `c/fs/logitfs/logitfs.c`.

## Journal ordering

LogitFS stages transaction metadata in transaction buffers. Its `bread()` helper checks those buffers before consulting the block cache, giving the current operation access to its staged changes. Metadata enters the block cache during commit. If a run overlaps staged metadata, `bread_run()` serves the range through individual `bread()` calls.

For a transaction containing metadata, `log_commit()` uses three calls to `bcache_sync()`:

| Barrier | Writes covered | Purpose |
|---|---|---|
| B1 | File data and journal body blocks | Persists data and journal bodies before publishing the commit header |
| B2 | Journal commit header | Persists the commit record before installing metadata at its home blocks |
| B3 | Metadata at its home blocks | Completes installation before clearing the journal header |

Successful writes and barriers complete each phase before the next begins. Eviction can write buffers earlier within a phase; synchronization establishes the order between phases.

After B3, LogitFS clears the journal header through the cache. A later write-back persists that clear. Recovery can replay a surviving completed record. The journal’s body checksum detects a stale header paired with different body blocks. The ordering argument is beside `log_commit()` in `c/fs/logitfs/logitfs.c`; validation and replay are in `c/fs/logitfs/fsck.c`.

## Interaction with the page cache

The two caches address different objects:

| Property | Block cache | Page cache |
|---|---|---|
| Logical key | Block number on the selected root device | Device identity, inode identity, and file page index |
| Contents | Filesystem data and metadata | File contents |
| Payload storage | Kernel heap buffers | Physical page frames |
| Access | Copies blocks into caller buffers | Supplies frames for mappings and copies bytes for cached reads |
| Memory recovery | Reuses buffers within its allocated pool | Evicts entries and participates in kernel reclaim |

For a file page fault, `c/kernel/mm/virt/fault.c` obtains a frame through the page cache. On a miss, the backend in `c/kernel/mm/cache/pcache_vfs.c` calls `vfs_pread()`. VFS dispatches the offset read to the filesystem. For LogitFS, this reaches its block walker and then the block cache.

The page cache’s `pcache_pread()` helper copies bytes from cached frames. Its implementation is in `c/kernel/mm/cache/pcache.c`. The VFS backend resolves file identity from device and inode information before caching file pages.

After a successful backend write, VFS invalidates affected entries in the page cache. Delete paths also invalidate affected names, and a successful rename invalidates both source and destination. `pcache_invalidate_path()` matches the recorded path and the identity currently resolved through that path. Later lookups can fetch the changed contents. These hooks are in `c/fs/vfs/vfs.c`.

When reclaim drops a cached file frame, it calls `pcache_forget_frame()` to remove the entry and release the cache’s frame reference. That integration is in `c/kernel/mm/reclaim/reclaim.c`. Eviction from the block cache and cache drops reuse the existing pool; `bcache_shutdown()` frees its allocations.

## Lifecycle and inspection

During mount, LogitFS requests the default pool. On allocation failure, it tries 64, 32, 16, and eight buffers. Mount fails if every attempt fails. Pool capacity controls both residency and the run reader’s installation threshold. The allocation sequence is in `c/fs/logitfs/logitfs.c`.

`bcache_drop()` invalidates every entry and discards dirty contents. The crash simulation in `tests/unit/fs_crash_test.c` calls it before unmounting to model loss of volatile cache state.

For a cold cache that preserves writes, `logitfs_cache_cold()` synchronizes, checks the result, and drops entries on success. It holds `lfs_lock` throughout this sequence, keeping concurrent writers outside the interval between synchronization and discard.

`bcache_getstats()` reports hits, misses, evictions, dirty evictions, successful buffer write-backs, synchronization calls, resident buffers, and dirty buffers. It also records read requests issued to the block layer, blocks requested, and blocks bypassed by large run reads. These counters measure activity at the cache boundary; lower layers may split requests into additional device commands. `logitfs_cache_stats()` reads the counters under the filesystem lock, and `c/fs/ctl/fsbench.c` formats them.

The test sources cover these behaviors:

| File | Coverage |
|---|---|
| `tests/unit/fs_cache_test.c` | Resident reads, deferred writes, synchronization, LRU eviction, bounds, and discard |
| `tests/unit/fs_bulkread_test.c` | Coalescing, stream bypass, small file caching, and dirty blocks within runs |
| `tests/unit/fs_crash_test.c` | Recovery after simulated power loss, including volatile cache discard |

`tests/unit/fsstub/fs_sim.h` implements batch writes by issuing simulated writes in sequence and recording each status, preserving the simulator’s crash cut points. The corresponding targets in `Makefile` are `make test-fs-cache`, `make test-bulkread`, and `make test-fs-crash`.

## Limitations

- The cache runs no periodic worker that writes back data. Write progress comes from eviction, synchronization, direct fallback, and teardown.
- `bcache_shutdown()` ignores errors from writes to buffers and frees the pool without a device flush. Durability requires successful synchronization before teardown.
- `log_commit()` records I/O failures and continues through later phases before returning failure. Its journal ordering argument depends on successful writes and barriers.
- File mapping requests reject write access in `c/kernel/mm/mmsys.c`. The page cache has no path that writes back dirty file pages.
- Invalidation of the page cache removes entries without updating existing mappings. Processes with pages already mapped can retain older bytes across filesystem mutations, as documented in `c/kernel/mm/cache/pcache.c`.

## See also

- [LogitFS](logitfs.md)
- [Virtual file system](vfs.md)
- [Storage stack](../architecture/storage-stack.md)
- [Page cache, reclaim and swap](../kernel/page-cache-and-reclaim.md)

## Source map

| File | What it contains |
|---|---|
| `c/fs/cache/bcache.h` | Cache contract, constants, interface, statistics, and concurrency correction |
| `c/fs/cache/bcache.c` | Allocation, lookup, recency, reads, write-back batches, synchronization, and teardown |
| `c/fs/logitfs/logitfs_fmt.h` | Filesystem block and sector sizes |
| `c/fs/logitfs/logitfs.c` | Filesystem locking, transaction staging, journal barriers, block runs, and cache lifecycle |
| `c/fs/logitfs/fsck.c` | Journal validation and recovery |
| `c/drivers/block/blkdev.h` | Block interfaces and batch span structure |
| `c/drivers/block/blkdev.c` | Root device access, partition offsets, queue admission, batching, and flush ordering |
| `c/kernel/mm/cache/pcache.c` | File-page identity, cached reads, invalidation, and frame ownership |
| `c/kernel/mm/cache/pcache_vfs.c` | Page-cache backend using VFS identity and offset reads |
| `c/kernel/mm/virt/fault.c` | File faults using page-cache frames |
| `c/kernel/mm/reclaim/reclaim.c` | Reclaim of cached file frames |
| `c/kernel/mm/mmsys.c` | File mapping requests and access restrictions |
| `c/fs/vfs/vfs.c` | Read dispatch and page-cache invalidation after mutations |
| `c/fs/ctl/fsbench.c` | Cache counter reporting |
| `tests/unit/fs_cache_test.c` | Buffer-cache behavior assertions |
| `tests/unit/fs_bulkread_test.c` | Coalescing, bypass, and dirty-block coherence assertions |
| `tests/unit/fs_crash_test.c` | Recovery assertions with simulated volatile-state loss |
| `tests/unit/fsstub/fs_sim.h` | Simulated media, batch writes, barriers, and crash cut points |
| `Makefile` | Cache and filesystem test targets |
