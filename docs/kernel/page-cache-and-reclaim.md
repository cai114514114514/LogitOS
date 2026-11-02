# Page cache, reclaim and swap

LogitOS recovers physical frames by dropping cached file pages, discarding anonymous pages filled with zeroes, and moving other eligible pages to swap. Reverse mappings identify the user mappings of each frame. Reference counts, pins and address-space guards govern eviction. When memory pressure persists, the out-of-memory (OOM) policy recovers pages from exited processes or selects a live process for termination.

## File pages and ownership

The page cache in `c/kernel/mm/cache/pcache.c` stores 4 KiB frames by file identity and page index. Its VFS adapter, `c/kernel/mm/cache/pcache_vfs.c`, obtains device number, inode number and size through `vfs_statx()`. Regular files carrying `VA_INO` qualify for caching. Paths reporting the same device and inode numbers share a file entry, including hard links.

Each file entry retains a path for backend reads through `vfs_pread()`. On a file fault, `do_file()` in `c/kernel/mm/virt/fault.c` calls `pcache_get_ref_counted()` and installs a mapping marked `VMM_PTE_FILE`. The cache acquires the caller’s frame reference while holding its lookup lock, protecting ownership through installation of the mapping.

The cache owns one frame reference per resident entry. Each user mapping owns another:

| State | Cache references | Mapping references |
|---|---:|---:|
| Cached, with no mappings | 1 | 0 |
| Cached and mapped once | 1 | 1 |
| Cached and mapped twice | 1 | 2 |

`pcache_file_put()` releases a file-handle reference. A file entry whose handle count reaches zero remains available for reuse with its cached pages. When the file table fills, opening another file can recycle an idle entry.

The constants in `c/kernel/mm/cache/pcache.h` provide 32 file slots. The page-entry pool is sized from half the physical frame count, with a minimum of 32 entries and an upper bound imposed by its signed entry indices. `pcache_init()` allocates page entries, hash buckets and the frame lookup table in one allocation. `c/kernel/init/kmain.c` initializes this metadata before device setup and installs the VFS adapter after filesystem mounting.

When the page-entry pool fills, the cache first seeks a frame whose sole reference belongs to the cache. Its second pass can remove an entry whose frame has other owners, releasing the cache’s reference and making the entry available for the requested page.

## Readahead and invalidation

`ra_advance()` in `c/kernel/mm/cache/pcache.c` tracks consecutive page requests within each file entry. Both hits and misses advance the sequence, so requests satisfied by an earlier batch continue the sequential-access history. A sequential miss starts a window of four additional pages; subsequent sequential misses double the window up to 32.

`ra_batch()` bounds each batch by the file’s end, the first cached page, available entries and free memory above the reclaim low watermark. It allocates the frames before reading and publishing them. This ordering matters because frame allocation can trigger reclaim: an installed page would immediately become a reclaim candidate while the batch was still allocating.

The batch sorts frames by physical address and groups adjacent frames into backend reads. Each run also stays within one physical-address alias region, allowing a backend read to use a continuous virtual buffer. Frames are zeroed before reading, including the unused tail of the file’s final page.

Mutation paths in `c/fs/vfs/vfs.c` invalidate cache entries after writes, deletion and rename operations. Invalidation advances the affected entry’s generation and releases its cached pages. A fill compares its saved generation before publication. When invalidation overlaps a read, the referenced lookup can return the demand page as a caller-owned snapshot while rejecting publication of the stale fill.

## Reverse mappings and eligibility

`c/kernel/mm/reclaim/rmap.c` maintains a chain for each physical frame. A node records an address-space root and a virtual page address. Mapping changes in `c/kernel/mm/virt/vmm.c`, together with fault and reclaim transitions, maintain these chains.

`rmap_init()` allocates `total_frames + total_frames / 2` nodes. Parallel arrays hold links, address-space root frame numbers and 64-bit virtual page numbers. Allocating this pool during initialization supplies the bookkeeping needed under memory pressure.

The historical contract in `c/kernel/mm/reclaim/rmap.h` states `rmap_count(f) == pmm_refcount(f)`. Cache ownership extends that condition. The current test in `candidate_extra()` in `c/kernel/mm/reclaim/reclaim.c` is:

```c
rmap_count(f) + pcache_holds(f) == pmm_refcount(f)
```

Here, `f` denotes a physical frame address. During an eviction attempt, the scanner also includes its temporary frame reference in the accounting.

This equality identifies the owners reclaim can release: recorded user mappings and the cache entry. Eligibility also requires a complete reverse map and a zero pin count. Kernel allocations retain references outside this accounting. Shared-memory segments likewise retain a reference per frame in `c/kernel/mm/shm.c`, preserving the storage shared by their mappings.

`pmm_pin()` and `pmm_unpin()` in `c/kernel/mm/phys/pmm.c` let temporary users protect a frame during an operation. References express ownership; pins express a current requirement for residency.

## Reclaim policy

The scanner in `c/kernel/mm/reclaim/reclaim.c` advances a persistent clock hand over physical frames. For an eligible mapped frame, it checks the accessed bits in all recorded mappings. An access through any mapping earns a second chance: the scanner clears those bits and advances. A frame owned solely by the cache proceeds directly to eviction consideration.

For a cold candidate, the scanner follows this order:

| Candidate | Action | Recovery on later access |
|---|---|---|
| Cached frame with `VMM_PTE_FILE` in every mapping | Clear mappings and release mapping and cache references | Fetch the file page again |
| Uncached frame with `VMM_PTE_ANON` in every mapping and zeroes throughout | Clear mappings and release mapping references | Supply anonymous memory filled with zeroes |
| Other eligible mapped frame without cache ownership | Write a swap slot and replace mappings with swap entries | Read the saved bytes from swap |

This order applies to each frame encountered by the hand. Cache ownership supplies a file reconstruction path. Dropping an anonymous page requires a content check because its reconstruction path supplies zeroes.

`reclaim_on_alloc()` runs before the physical allocator takes its lock. Initialization sets the low watermark to `total_frames / 32` and the high watermark to `total_frames / 16`. Below the low watermark, allocation requests enough reclaim to reach the high watermark.

An allocation pass examines at most 16,384 frames, also bounded by twice the physical frame count and the current episode’s remaining allowance. A pass that frees nothing starts a backoff of 512 allocation calls. The backoff applies while free memory exceeds half the low watermark. Below that cushion, allocation resumes reclaim attempts while scan allowance remains.

## Bounded pressure episodes

An episode accumulates the frames examined by scans that free nothing. Its allowance is two complete clock rotations, allowing one visit to clear accessed bits and another to consider eviction. `reclaim_stalled()` reports exhaustion while free memory remains below the low watermark.

Recovering a frame or observing free memory at or above the low watermark ends the episode. Disabling reclaim and completing an OOM decision also end it. `reclaim_frames()` permits up to two rotations for a requested number of frames; `reclaim_emergency()` uses the allocation scan budget. Both share the episode allowance.

The episode count and scanner ownership occupy one atomic word. Ending an episode marks a running scan stale, causing its later accounting to be discarded. A caller finding another scanner active returns immediately: that scanner may need an address-space or filesystem lock held by the caller.

Initialization occurs through `reclaim_late_init()` on the first page fault from ring 3, as wired in `c/kernel/mm/virt/fault.c`. By this stage the block devices needed for swap selection have been enumerated.

## Coordinating eviction

Reclaim takes a snapshot of reverse-map values and tries to acquire an address-space guard for every mapping. It then revalidates address-space lifetime, page-table entries and frame reference accounting. Failed guard acquisition unwinds the acquired guards and advances the scanner. The locking contract is in `c/kernel/mm/virt/mmguard.h`.

Before checking page contents or transferring bytes, `reclaim_freeze()` revokes writable permission from every recorded mapping and calls `vmm_flush_space()`. Address-space guards serialize kernel changes; translation invalidation makes the permission change effective for user accesses.

The scanner retains an additional frame reference throughout the attempt. Successful eviction clears translations before releasing mapping references. Cleanup releases the scanner reference and address-space guards. After an unsuccessful attempt, cleanup restores saved writable permissions on entries that still map the frame.

## Swap storage and transitions

`c/kernel/mm/reclaim/swap.c` selects a registered block device. Selection checks the device’s relationship to the root device, capacity and first sector. Eligible devices carry `SWAP_MAGIC` or an all-zero first sector. The root device, its parent and child relationships, sibling partitions sharing its parent, and devices carrying a LogitFS signature are excluded.

The layout reserves eight 512-byte sectors for the header area and eight sectors per slot. Each slot holds one 4 KiB page. Slot numbers begin at one. Capacity is capped at 262,144 slots, providing up to 1 GiB of page payload. `swap_init()` allocates the slot reference table before transfers begin.

When a shared frame is evicted, every mapping receives an entry for the same slot. Each entry owns one slot reference. Fork increments these references; unmap, address-space teardown and successful swap faults release them through `c/kernel/mm/virt/vmm.c` and `c/kernel/mm/reclaim/reclaim.c`.

`try_swap()` pins the source frame, installs swap entries and flushes translations before issuing the write. Address-space guards remain held across the transfer. A failed write restores the saved entries and releases the slot references. A successful write removes reverse mappings and releases the resident mapping references.

The entry format in `c/kernel/mm/virt/vmm.h` stores the slot in bits 12–51. Other markers retain writable, copy-on-write and anonymous state, while NX retains execute permission. Slot decoding masks the slot field to exclude NX.

## Swap faults and I/O waits

On a swap fault, `reclaim_swapin()` allocates through `pmm_alloc_reserve_any()`. The physical allocator’s reserve defaults to 128 frames. If allocation fails, swap-in requests emergency reclaim and tries again. A successful read installs the resident mapping, records its reverse mapping and releases the slot reference.

The historical boolean description in `c/kernel/mm/reclaim/reclaim.h` has three outcomes in the implementation: success, an entry or device failure, and `OOM_SWAPIN_NOMEM`, defined in `c/kernel/mm/reclaim/oom.h`. Frame exhaustion returns `OOM_SWAPIN_NOMEM` while preserving the swap entry and slot reference. This lets the fault handler seek memory recovery after releasing its address-space guard.

A busy flag serializes swap transfers. The device path submits a block request and polls in intervals with a default budget of two million cycles. On expiry, callers in kernel context wait through `sched_poll_wait()`. A submission returning `BLK_E_NODMA` uses the synchronous block path.

Historical comments in `c/kernel/mm/reclaim/swap.c` describe these waits as handing off the big kernel lock. The current `sched_poll_wait()` in `c/kernel/sched/sched.c` saves kernel-entry depth, clears it while calling `cpuidle_enter()`, and restores it on wake. The idle implementation in `c/kernel/cpu/power/cpuidle.c` chooses MWAIT when its CPU and firmware checks permit it, with HLT as the fallback.

## OOM victim selection

The policy in `c/kernel/mm/reclaim/oom.c` first asks to recover user pages from exited processes. `oom_task_reap_dead()` in `c/kernel/exec/proc.c` tries the address-space guard and revalidates the process slot, PID and address-space root before releasing user mappings. The policy measures the change in free frames; recovering frames permits an immediate fault retry.

For live processes, an outstanding kill mark takes precedence over choosing another victim. A new selection follows these rules:

1. Gather running processes and tally their resident mappings through the reverse map.
2. Protect processes with no parent and no window, using the same predicate as process termination.
3. Consider candidates with at least 64 resident mappings.
4. Choose the largest eligible candidate, preferring the largest candidate without a window when its tally reaches `reclaim_high()`.

The tally counts recorded resident mappings. Shared mappings contribute to each owning process’s count, and reservations contribute as their pages become resident.

Termination sets a process kill mark. `proc_kill_check()` in `c/kernel/exec/proc.c` consumes the mark and calls process exit. The syscall entry path in `c/kernel/exec/syscall.c` invokes this check.

## OOM fault recovery

Anonymous faults and copy-on-write faults that need a new frame allocate through `fault_frame()` in `c/kernel/mm/virt/fault.c`. Allocation failure first requests emergency reclaim. The function also checks `reclaim_stalled()` after allocation. If the episode has exhausted its scan allowance, it returns an acquired frame to the allocator and records `OOM_FAULT_STALLED`. This brings sustained pressure to the OOM policy while frames above the reserve may still be available.

`mm_fault_in()` handles the recorded shortage after the guarded fault operation returns. It calls `oom_fault_decide()` and, when recovery permits, retries from the start with a fresh page-table walk.

The earlier description that swap faults bypass OOM recovery is superseded by the `OOM_SWAPIN_NOMEM` path. For this outcome, `mm_fault_in()` first calls `reclaim_frames(1)` outside the guard. If reclaim recovers a frame, it retries the fault. A further memory shortage proceeds to one OOM decision and a final retry, including when another thread has changed the mapping’s fault class during recovery.

The decision distinguishes three outcomes:

| Outcome | Fault-handler action |
|---|---|
| `OOM_FAULT_RETRY` | Retry after memory recovery |
| `OOM_FAULT_DECLINE` | Return the unresolved fault |
| `OOM_FAULT_NONE` | Retry a fault caused by stalled reclaim; otherwise return the unresolved fault |

`OOM_FAULT_NONE` covers both a lack of eligible victims and another CPU owning the chooser. After every decision, `reclaim_stall_reset()` ends the episode. The final fault walk accepts an available payload frame even if reclaim stalls again during that retry.

When another process is selected, the default recovery wait allows 128 waits and tries to recover exited processes after each one. Anonymous and copy-on-write faults wait for free memory above the allocator’s reserve. Swap-in waits for any free frame because its allocation can use that reserve.

The allocation hooks serve different purposes. `oom_alloc_fail()` records physical allocator refusals. `oom_kheap_fail()` requests victim selection after a kernel-heap failure, with the heap lock released. `kmalloc_quiet()` in `c/kernel/mm/phys/kheap.c` suppresses the heap growth failure message and OOM hook, returning NULL on refusal. Scheduler observation allocations use this path through `c/kernel/sched/observe/observe_platform.c` for pools sized by caller requests.

## Diagnostics and tests

| Interface | Information |
|---|---|
| `pcache_report()` and `pcache_audit()` | Residency, sharing, readahead, backend cost and cache-entry consistency |
| `rmap_audit()` | Mapping counts, frame ownership and node-pool accounting |
| `reclaim_report()` | Scans, second chances, drops, swaps and skipped candidates |
| `swap_report()` | Slot use, transfer failures, polling cycles, elapsed transfer cycles and waits |
| `oom_stats()` | Victims, recovered frames, retry decisions and successful fault retries |

The OOM statistics distinguish permission to retry from a retried fault that succeeds. Another allocation can consume recovered memory between those events.

`tests/unit/mm_reclaim_test.c` checks swap round trips, pins, second chances, sharing, failed writes and exhaustion. `tests/unit/mm_pcache_test.c` checks identity, invalidation, capacity and readahead. `tests/unit/bkl_mm_test.c` checks caller references across invalidation and reads overlapping invalidation. `tests/unit/mm_oom_test.c` checks victim policy and recovery of exited processes.

The entry points in `Makefile` and `tests/mem.mk` describe the available checks:

| Command | Coverage |
|---|---|
| `make test-mm` | Host memory suites, with mutation controls as a prerequisite |
| `make test-oom` | Host OOM policy suite and its victim-selection control |
| `make test-swap` | Guest swap pressure, data verification, audits and subsequent liveness |
| `make test-swap-negctl` | Guest pressure with the swap device omitted |
| `make test-oom-os` | Guest victim selection and fault recovery |

`tests/unit/mm_run.sh` includes controls for pin checks, zero-content checks, cache identity and readahead policy. `tests/unit/oom_run.sh` substitutes selection of the newest process.

The guest swap harness, `tests/boot/run-swap-test.sh`, defaults to NVMe and accepts `SWAPBUS=ahci`. It waits for the pressure workload’s terminal marker before sending follow-up commands. Its control requires the workload’s start marker and rejects successful completion without swap. `make test-swap-negctl` is a distinct target; `make test-swap` has image prerequisites.

The OOM harness is `tests/boot/run-oom-test.sh`. Its defaults in `tests/mem.mk` are 320 MiB of RAM, a 160 MiB hog and a 96 MiB requesting process.

## Limitations

- Writable file mappings, dirty-page writeback and synchronization of mapped writes are absent. `c/kernel/mm/mmsys.c` rejects writable file-mapping requests.
- Invalidation removes cache ownership while existing mappings retain their earlier bytes. Pool pressure can also remove cache ownership from mapped frames. Such frames require swap for later eviction.
- Reclaim skips pinned frames, incomplete reverse maps and frames with more than 16 mappings. Reverse-map pool exhaustion marks the affected physical frame incomplete for the rest of the boot.
- The clock has one sampling hand and no policy for fairness between processes. Swap faults restore a private frame for each mapping; subsequent eviction writes the page again.
- Swap ownership lasts for one boot. Initialization rewrites the header and starts an empty slot table.
- OOM waits can expire. File-page allocation failures bypass the OOM fault-retry trigger. Kernel-heap victim selection leaves allocation recovery to the caller.

## Source map

| File | What it contains |
|---|---|
| `c/kernel/mm/cache/pcache.h` | Cache interfaces, capacity constants and ownership contract |
| `c/kernel/mm/cache/pcache.c` | Identity, frame ownership, readahead, invalidation and diagnostics |
| `c/kernel/mm/cache/pcache_vfs.c` | VFS identity checks and offset-read adapter |
| `c/kernel/mm/reclaim/rmap.h` | Reverse-map contract and historical accounting |
| `c/kernel/mm/reclaim/rmap.c` | Mapping chains, node pool, snapshots and audits |
| `c/kernel/mm/reclaim/reclaim.h` | Reclaim interfaces and sharing bound |
| `c/kernel/mm/reclaim/reclaim.c` | Clock policy, bounded episodes, eviction and swap faults |
| `c/kernel/mm/reclaim/swap.h` | Swap layout constants and slot interfaces |
| `c/kernel/mm/reclaim/swap.c` | Device selection, slot references, transfers and diagnostics |
| `c/kernel/mm/reclaim/oom.h` | OOM outcomes and reclaim-episode interfaces |
| `c/kernel/mm/reclaim/oom.c` | Resident tallies, victim policy, recovery waits and statistics |
| `c/kernel/mm/phys/pmm.h` | Frame-size constant and allocator interfaces |
| `c/kernel/mm/phys/pmm.c` | Frame references, pins, allocation hooks and reserve |
| `c/kernel/mm/phys/kheap.c` | Heap failure hook and quiet allocation path |
| `c/kernel/mm/virt/vmm.h` | Page-origin markers and swap-entry encoding |
| `c/kernel/mm/virt/vmm.c` | Mapping bookkeeping, slot lifetime and translation flushing |
| `c/kernel/mm/virt/mmguard.h` | Address-space guard contract |
| `c/kernel/mm/virt/fault.c` | File, anonymous, copy-on-write and swap faults; OOM retries |
| `c/kernel/mm/mmsys.c` | File-mapping requests and memory controls |
| `c/kernel/mm/shm.c` | Shared-segment frame ownership |
| `c/kernel/exec/proc.c` | Process adapter, kill marks and exited-process recovery |
| `c/kernel/exec/syscall.c` | Syscall checks for pending termination |
| `c/kernel/sched/sched.c` | Scheduler polling wait |
| `c/kernel/cpu/power/cpuidle.c` | Idle-state selection |
| `c/kernel/sched/observe/observe_platform.c` | Quiet allocation for observation requests |
| `c/kernel/init/kmain.c` | Cache initialization and VFS adapter installation |
| `c/fs/vfs/vfs.c` | Mutation paths that invalidate cached pages |
| `tests/unit/mm_reclaim_test.c` | Reclaim and swap checks |
| `tests/unit/mm_pcache_test.c` | Cache identity, capacity and readahead checks |
| `tests/unit/bkl_mm_test.c` | Concurrent cache lifetime and stale-fill checks |
| `tests/unit/mm_oom_test.c` | OOM policy and exited-process recovery checks |
| `tests/unit/mm_run.sh` | Memory suites and mutation controls |
| `tests/unit/oom_run.sh` | OOM suite and victim-policy control |
| `tests/boot/run-swap-test.sh` | Guest pressure, integrity and liveness checks |
| `tests/boot/run-oom-test.sh` | Guest victim selection and recovery checks |
| `Makefile` | Memory and swap test targets |
| `tests/mem.mk` | OOM targets and pressure defaults |

## See also

- [Physical memory](physical-memory.md)
- [Virtual memory](virtual-memory.md)
- [Shared memory](shared-memory.md)
- [Process management](processes.md)
- [Memory model](../architecture/memory-model.md)
- [Block cache](../filesystems/block-cache.md)