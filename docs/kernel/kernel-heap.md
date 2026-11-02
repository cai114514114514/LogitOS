# Kernel heap

The kernel heap provides `kmalloc`, `kmalloc_quiet`, `kmalloc_low` and `kfree` for dynamic kernel storage. It divides arenas of contiguous physical frames into blocks, groups reusable blocks by size, splits excess space and merges adjacent free blocks. Caches on each CPU serve selected small sizes to reduce contention on the shared heap lock. The implementation is in `c/kernel/mm/phys/kheap.c`; its public interface and statistics are in `c/kernel/mm/phys/kheap.h`.

## Allocation domains

The allocator maintains two domains, each with nine shared free lists:

| Interface | Returned address | Use |
|---|---|---|
| `kmalloc(size)` | CPU pointer through the supervisor physmap, the kernel’s direct mapping of RAM | Kernel data |
| `kmalloc_quiet(size)` | Same address contract as `kmalloc` | Kernel data with allocation failure handled by the caller |
| `kmalloc_low(size)` | Identity address backed by physical RAM below 1 GiB | Executable kernel module storage |
| `kfree(ptr)` | Accepts an allocation base from either domain | Release storage |

`c/kernel/mm/phys/kheap.h` preserves the former identity mapping description beside a correction dated 2026-09-10. The current `kmalloc` contract uses a physmap pointer even when the backing RAM lies below 1 GiB. `mm_physmap_ptr` implements this conversion in `c/kernel/mm/mmhost.h`.

For arena growth, the physmap domain calls `pmm_alloc_contig_masked`. It searches RAM at or above 4 GiB first, then RAM between 1 and 4 GiB, then RAM below 1 GiB. This preserves low RAM for callers with address constraints. The low domain calls `pmm_alloc_contig`, whose search stays below 1 GiB. Both searches are implemented in `c/kernel/mm/phys/pmm.c`.

The module loader shows the distinction at work. In `c/kernel/module/modload.c`, the input file uses `kmalloc`, while the executable image uses `kmalloc_low`. The low identity alias supports execution and the module’s relocations to the low kernel image.

## Initialisation and arenas

Heap use follows `pmm_init`. During boot, `c/kernel/init/kmain.c` first calls `bootmod_init`, which records validated boot module ranges through `pmm_hold_range`. The PMM reserves those ranges and places its metadata around them. This protects firmware files supplied by the loader that occupy RAM the firmware map describes as available. The registration and reservation code is in `c/kernel/init/bootmod.c` and `c/kernel/mm/phys/pmm.c`.

Each arena acquisition starts at `ARENA_FRAMES`, or 1,024 frames. `FRAME_SIZE` is 4,096 bytes, giving a minimum arena size of 4 MiB. The heap doubles the frame count until the arena can contain the aligned request and its header, then inserts the arena as one free block. Arena sizing checks for overflow during doubling.

## Block layout

Each block begins with a `struct header` of 16 bytes, followed by a payload aligned to 16 bytes:

| Member or flag | Meaning |
|---|---|
| `size` | Atomic payload size combined with state flags |
| `prev_size` | Payload size of the preceding block; zero marks the arena’s first block |
| `F_FREE` | Block belongs to a shared free list |
| `F_LAST` | Block ends its arena |
| `F_LOW` | Block belongs to the low allocation domain |
| `F_PARK` | Block is claimed for freeing, held in a cache, or undergoing internal reuse |

Payload sizes are multiples of 16, leaving four low bits available for flags. A free block stores `struct fnode` in its payload, holding the next and previous pointers of the free list. The minimum payload is 16 bytes, enough for both pointers.

`prev_size` and `F_LAST` locate neighbouring blocks through header arithmetic and terminate traversal at arena boundaries. Doubly linked free lists let merging remove a neighbouring free block directly. These structures are defined in `c/kernel/mm/phys/kheap.c`.

## Size classes and reuse

`NUM_BINS` is nine per domain. `bin_index` assigns aligned payload sizes to these bins:

| Bin | Payload sizes |
|---|---|
| 0 | 16 bytes |
| 1 | 32 bytes |
| 2 | 48–64 bytes |
| 3 | 80–128 bytes |
| 4 | 144–256 bytes |
| 5 | 272–512 bytes |
| 6 | 528–1,024 bytes |
| 7 | 1,040–2,048 bytes |
| 8 | Above 2,048 bytes |

The ranges contain multiples of 16. For example, `kmalloc(33)` rounds its request to 48 bytes.

The shared allocation path searches the matching bin and then larger bins, taking the first block that fits. `split_block` divides the block when the remainder can hold another header of 16 bytes and at least `SPLIT_MIN`, or 64 bytes, of payload. A smaller tail accompanies the allocation.

When a block returns to the shared heap, `coalesce` merges it with free neighbours and updates the surrounding headers. Splitting makes excess space available to other requests; merging reconstructs larger blocks when their allocations end.

The historical explanation in `c/kernel/mm/phys/kheap.c` describes small buffers consuming whole freed window surfaces. `tests/unit/leak_kheap_test.c` exercises this reuse problem with overlapping large and medium allocations across repeated cycles.

## Caches on each CPU

Each CPU has a magazine containing up to `MAG_DEPTH`, or 32 blocks, for each of six payload sizes: 16, 32, 64, 128, 256 and 512 bytes. Allocation lookup uses the request after alignment; admission on free uses the block’s payload size.

A matching physmap allocation first tries the current CPU’s magazine. Freeing a matching block parks it in the current CPU’s magazine when space is available. A full magazine sends the block through the shared free path. Blocks in the low domain use the shared lists.

Each magazine has its own lock. `kheap_cpu_index`, implemented in `c/kernel/cpu/smp/percpu.c`, supplies the CPU index. Magazine structures are aligned to 64 bytes, which keeps neighbouring magazines from sharing a writable cache line.

Cached blocks remain allocated in shared heap accounting. Their headers retain `F_PARK`, which protects their ownership while neighbouring blocks are freed and merged.

When the shared lists cannot satisfy a physmap request, the allocator drains every magazine, merges the returned blocks and searches again. It requests another arena if the search still fails. This makes cached storage available before arena growth. Both `kmalloc` and `kmalloc_quiet` follow this path in `c/kernel/mm/phys/kheap.c`.

## Synchronisation and failure handling

Shared lists, splitting, merging and arena growth use `kheap_lock`. Growth can acquire `pmm_lock` beneath it. Draining and statistics collection acquire magazine locks beneath `kheap_lock`; the cache allocation and free paths acquire their magazine lock alone.

Heap operations save and clear the local interrupt flag before acquiring locks, then restore its previous state on release. This prevents an interrupt handler from reacquiring a lock held by the code it interrupted. The lock interfaces and implementation are in `c/kernel/cpu/spinlock.h` and `c/kernel/cpu/spinlock.c`.

Before choosing a free path, `blk_claim_free` atomically sets `F_PARK`. Finding `F_FREE` or `F_PARK` produces a diagnostic for a double free and refuses the request. This claim covers both magazine insertion and return to a shared list, including the case where a magazine is full.

Requests of zero size, and requests that would overflow alignment or header arithmetic, return `NULL`. A failed PMM arena acquisition through `kmalloc` or `kmalloc_low` prints the allocation domain and frame count.

After an exhausted `kmalloc` request, the allocator releases `kheap_lock` and calls `oom_kheap_fail`. In `c/kernel/mm/reclaim/oom.c`, that hook counts the refusal, emits a diagnostic subject to a rate limit and invokes OOM victim selection. The allocation returns `NULL`. Exhaustion of the low domain returns `NULL` after the diagnostic for a failed growth.

`kmalloc_quiet` shares the physmap domain, bins and magazines with `kmalloc`. It suppresses the diagnostic for a failed growth and the OOM hook. The scheduler observer uses it in `c/kernel/sched/observe/observe_platform.c` after checking multiplication overflow and comparing the requested bytes with `pmm_free_bytes`. The observer then zeroes a successful allocation.

## Rules for callers

- Complete `pmm_init` before allocating heap storage, as required by `c/kernel/mm/phys/kheap.h`.
- Validate arithmetic used to calculate allocation sizes, check the returned pointer and provide a failure path.
- Use `kmalloc_quiet` when refusal should return through the caller’s failure policy without invoking OOM victim selection.
- Initialise payload bytes before reading them and keep accesses within the requested extent.
- Pass the allocation base to `kfree` once ownership ends. Synchronise with other users before releasing storage. `kfree(NULL)` returns immediately.
- Use `kmalloc_low` for executable module storage requiring the low identity address contract.
- Use `dma_alloc_coherent` or `dma_map_kernel` for device transfers. The interfaces in `c/drivers/core/dma.h` supply device addresses and transfer constraints.
- Preserve the lock ordering between heap and PMM, and between heap and magazine, when extending allocation paths. Account for the OOM hook when calling `kmalloc` while holding other subsystem locks.

The module loader in `c/kernel/module/modload.c` provides examples of checking allocation failures, initialising storage and releasing buffers on error paths.

## Debugging aids

`kheap_get_stats` fills `struct kheap_stats`. `kheap_report` prints domain totals, shared heap accounting and magazine activity.

| Fields | Interpretation |
|---|---|
| `arena_bytes`, `grows` | Arena bytes acquired and successful arena acquisitions |
| `live_bytes`, `live_blocks` | Allocated payload bytes and block counts, including cached blocks |
| `free_bytes` | Payload bytes accounted to shared free lists |
| `req_bytes`, `served_bytes` | Cumulative requested and supplied payload bytes for allocations served through shared bins |
| `allocs`, `frees` | Shared-bin allocations and returns to the shared heap |
| `splits`, `split_bytes`, `merges` | Split count, cumulative remainder payload from splitting and neighbour-merge count |
| `magazine_hits`, `magazine_puts`, `magazine_bytes` | Cache reuse count, insertion count and current cached payload |
| `magazine_drains`, `magazine_cpus` | Drain count and number of magazines with at least one recorded hit |
| `low_arena_bytes`, `far_arena_bytes` | Low-domain arena bytes and arena bytes acquired at or above 4 GiB |
| `max_phys` | Highest physical byte address covered by an acquired arena |

Magazine hits bypass the shared allocation counters. Parking a block increments cache counters while its payload remains in `live_bytes`; draining transfers it back to shared free accounting. Headers occupy arena space outside the payload totals.

For a repeated workload, compare arena growth once the caches have warmed against live and cached payload. At a quiescent point, `live_bytes - magazine_bytes` gives the payload held outside magazines. The difference between `served_bytes` and `req_bytes` records overhead from alignment and unused tails for allocations served through shared bins. Selected totals reach procfs through `c/fs/procfs/procfs_src.c`.

Successful arena growth emits diagnostics through every allocation entry point, including `kmalloc_quiet`.

During scheduler observation, the lock wrappers call `flight_irq_disabled_begin` and `flight_irq_disabled_end` in `c/kernel/sched/observe/flight_recorder.c`. They time the outermost interval bracketed by these hooks, including lock waiting and work under the lock, and subtract the recorder’s accounted cost.

The main search of the free lists has a `FREELIST_WALK_MAX` bound of 1,000,000 visits per bin. Exceeding it prints a corruption diagnostic and clears that bin’s list root.

For failure injection, `make GROWFI=1` adds `KHEAP_GROW_FAULT_INJECT`. The injection counter forces every other arena acquisition attempt to fail, beginning at attempt 17. The setting is in `Makefile`; the hook is in `c/kernel/mm/phys/kheap.c`.

The host harnesses define the following checks:

| Entry point | Checks encoded by the harness |
|---|---|
| `make test-kheap` | Allocation overlap, payload preservation and duplicate frees under injected growth failures; depends on `test-kheap-crosscore` |
| `make test-kheap-crosscore` | Duplicate frees across CPUs, concurrent frees and a full magazine; requires a `KHEAP_NO_CROSSCORE_CLAIM` control to expose duplicate ownership |
| `make test-leak` | Arena growth and accounting under repeated allocations of mixed sizes; requires builds with `KHEAP_NO_SPLIT` and `KHEAP_NO_COALESCE` to fail |

## Limitations

- Arena frames remain owned by the heap for the boot’s lifetime. Growth requires contiguous physical frames, so fragmentation can cause failure while other memory remains free.
- Payload bytes are uninitialised.
- Free validation assumes a valid allocation base and readable header. State flags carry no allocation generation, so a stale pointer can pass validation after its block has been allocated again.
- The rescan of the free lists immediately after draining has no visit bound. Clearing a corrupt bin’s root strands its blocks while their bytes remain in shared free accounting.
- Scheduler observation covers intervals bracketed by the lock wrappers. Bare interrupt-masking instructions and assembly paths fall outside that coverage.

## See also

- [Physical memory](physical-memory.md)
- [Virtual memory](virtual-memory.md)
- [Synchronisation](synchronization.md)
- [Kernel modules](modules.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/mm/phys/kheap.c` | Arenas, bins, splitting, merging, magazines, ownership checks and diagnostics |
| `c/kernel/mm/phys/kheap.h` | Public interface, address contract and statistics structure |
| `c/kernel/mm/phys/pmm.c` | Contiguous frame allocation, zone preference and boot module reservations |
| `c/kernel/mm/phys/pmm.h` | Frame size and PMM interfaces |
| `c/kernel/mm/mmhost.h` | Conversion between physical addresses and kernel RAM pointers |
| `c/kernel/cpu/spinlock.h` | Lock interfaces and interrupt-state contract |
| `c/kernel/cpu/spinlock.c` | Ticket locks, interrupt-state handling and observation hooks |
| `c/kernel/cpu/smp/percpu.c` | CPU index hook for magazines |
| `c/kernel/init/kmain.c` | Boot module registration and PMM initialisation ordering |
| `c/kernel/init/bootmod.c` | Boot module validation, registration and physical range holds |
| `c/kernel/module/modload.c` | Data and executable allocations in module loading |
| `c/kernel/mm/reclaim/oom.c` | Heap refusal accounting and OOM victim selection |
| `c/kernel/sched/observe/observe_platform.c` | Checked, zeroed observer allocations through `kmalloc_quiet` |
| `c/kernel/sched/observe/flight_recorder.c` | Timing of intervals bracketed by lock observation hooks |
| `c/drivers/core/dma.h` | Device buffer and mapping interfaces |
| `c/fs/procfs/procfs_src.c` | Heap statistics supplied to procfs |
| `tests/unit/kheap_test.c` | Overlap, payload and duplicate-free checks |
| `tests/unit/kheap_crosscore_test.c` | Duplicate-free scenarios across CPUs |
| `tests/unit/kheap_crosscore_run.py` | Cross-CPU test and control runner |
| `tests/unit/leak_kheap_test.c` | Repeated allocation workload and accounting assertions |
| `tests/unit/leak_run.sh` | Workload runner and split/coalesce controls |
| `Makefile` | Heap test targets and growth failure injection setting |
