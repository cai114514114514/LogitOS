# Memory model

LogitOS combines a low identity mapping for the kernel, a supervisor direct map of RAM and two virtual address windows for user processes. The physical memory manager owns frames; the virtual memory manager maintains translations; virtual memory areas describe reservations and backing objects. Page faults populate those reservations, while the page cache, reclaim and swap coordinate page ownership under memory pressure.

## Address-space layout

Both native loaders enter the kernel with four levels of x86-64 paging and an identity mapping of the first 1 GiB, built from 2 MiB leaves. The kernel extends this bootstrap mapping using the firmware memory map. The boot contract is defined in `include/abi/logit_boot.h` and implemented by `c/boot/bios/loader.asm` and `c/boot/efi/loader.c`.

UEFI copies its final handoff stub and GDT into an `EfiLoaderCode` page below 1 GiB. This keeps the handoff code reachable after switching to the bootstrap tables when firmware placed the loader image above that window (`c/boot/efi/loader.c`).

The principal virtual regions are:

| Region | Placement | Purpose |
|---|---|---|
| Low identity map | `0` to `0x40000000` | Kernel image, bootstrap data, page tables and allocations requiring low addresses |
| Kernel image | Begins at 32 MiB | Image bounded by `_kernel_start` and `_kernel_end` in `linker.ld` |
| Legacy user window | `0x40000000` to `0x80000000` | Process images, stacks and reservations |
| Preferred mapping window | `0x60000000` to `0x7F000000` | Reservation placement by first fit within the legacy window |
| Wide user window | `0x0000010000000000` to `0x0000800000000000` | Sparse user mappings from 1 TiB to 128 TiB |
| Supervisor direct map | `PHYSMAP_BASE`, `0xffff800000000000`, spanning `PHYSMAP_SIZE`, 64 TiB | RAM aliases at the base plus the physical address |

Range ends are exclusive. The preferred mapping window is part of the legacy user window. The user constants come from `c/kernel/mm/mm.h`; the physical map constants come from `c/kernel/mm/phys/physmap.h`.

The gap between the user windows belongs to kernel mappings. `mm_user_range()` validates that a nonempty range fits entirely within either user window, using subtraction to avoid overflow. Reservation validation and checks on user buffers apply this common layout rule.

## Physical memory and aliases

`pmm_init()` consumes the boot memory map and identifies whole pages marked available. Its bitmap, reference counts, pin counts and RAM provenance bitmap occupy available memory below 1 GiB. It reserves physical memory through the kernel end, its metadata extent and the boot information block. Firmware holes stay reserved (`c/kernel/mm/phys/pmm.c`).

Boot modules require additional ownership records because UEFI reports their buffers as available memory. Before PMM initialization, `bootmod_init()` validates their tags and calls `pmm_hold_range()`. These holds round outward to frames, exclude module storage from metadata placement and restore its reservation after the low and high RAM release passes. The module ABI uses 64-bit physical bounds, accommodating buffers above 4 GiB (`c/kernel/init/bootmod.c`, `include/abi/logit_boot.h`).

The direct map covers whole available RAM pages with supervisor, writable entries and NX when enabled. Aligned intervals use 2 MiB leaves; boundary fragments use 4 KiB leaves. PMM completes the low RAM aliases first, then constructs the high RAM aliases. High frames enter the free pool after the mappings have been installed and translations flushed.

Kernel code uses the conversions in `c/kernel/mm/mmhost.h`:

| Helper | Kernel behavior |
|---|---|
| `mm_p2v()` | Returns the identity alias below 1 GiB and the direct-map alias above it |
| `mm_physmap_ptr()` | Returns the direct-map alias for either low or high RAM |
| `mm_v2p()` | Converts a pointer in either kernel RAM alias to a physical address |

These helpers perform address arithmetic. Callers establish that the backing RAM is mapped and owned before dereferencing the result. Page table address fields and CR3 contain physical addresses.

## Kernel and process mappings

`vmm_new_space()` allocates a PML4 and a private low PDPT. It copies the kernel translations, clears PDPT entry 1 beneath PML4 entry 0 for the legacy user window, and clears PML4 entries 2 through 255 for the wide user window. The remaining roots share kernel translation structures. Later additions to shared kernel roots propagate through the registry of live address spaces (`c/kernel/mm/virt/vmm.c`).

Kernel mappings stay mapped across process switches, while each process owns its user translation trees. Threads within a process share its CR3, as shown by thread creation in `c/kernel/sched/uthread.c`.

User mappings receive translation paths with user access enabled. Leaf entries carry write and execute permissions. The ELF loader derives those permissions from segment flags; demand faults derive them from VMA protection (`c/kernel/exec/load/elf.c`, `c/kernel/mm/virt/fault.c`).

`MM_PTE_ADDR` extracts the physical address field, and `MM_PTE_FLAGS` extracts the other bits. These masks in `c/kernel/mm/mm.h` carry NX and software state through cloning, copy on write and swap transitions.

Framebuffer MMIO uses supervisor mappings at the aperture’s physical addresses. Kernel initialization programs PAT before framebuffer initialization, and application processors call PAT setup before joining scheduling. The framebuffer requests write combining when PAT setup succeeds and uncached mappings otherwise (`c/kernel/init/kmain.c`, `c/kernel/cpu/smp/smp.c`, `c/kernel/gui/fb/fb.c`).

The checked `vmm_map_mmio_wc()` interface accepts absent pages or existing identity mappings with matching cache attributes. It holds the shared kernel guard and flushes translations after publishing changes (`c/kernel/mm/virt/vmm.c`).

## Allocation and ownership

| Component | Responsibility |
|---|---|
| PMM | Allocates 4 KiB frames and contiguous extents; maintains references and pins |
| VMM | Maps, clones, protects and releases translations |
| VMA | Records virtual reservations, protection and backing-object references |
| Kernel heap | Suballocates physical arenas into kernel objects |
| Page cache | Owns cached file pages and supplies references to readers and mappings |
| Reverse map | Records user mappings pointing to each frame |
| Reclaim | Selects eligible frames and drops or swaps their contents |
| Swap | Allocates disk slots and transfers page contents |

A fresh PMM allocation has one reference. `pmm_ref()` acquires another, and `pmm_free()` releases one; reaching zero returns the frame to the free pool. Pins tell reclaim that a frame must stay in place while a kernel operation uses it (`c/kernel/mm/phys/pmm.c`).

The allocation interfaces express placement requirements:

- `pmm_alloc()` and `pmm_alloc_contig()` allocate below 1 GiB.
- `pmm_alloc_any()` prefers mapped high RAM and falls back to low RAM.
- `pmm_alloc_contig_masked()` constrains the entire extent by an address mask, alignment and boundary.
- `pmm_alloc_reserve_any()` allows swap-in to consume the fault reserve.

The default reserve is 128 frames. Regular allocation of a single frame holds back a global reserve and a reserve for the low zone, keeping space available for swap-in and for consumers that require low addresses.

`kmalloc()` obtains contiguous physical arenas and accesses them through the supervisor direct map, including arenas backed by low RAM. Size bins, splitting, coalescing and magazines per CPU reuse space within those arenas. `kmalloc_low()` supplies the low identity alias used for executable kernel modules, whose relative relocations must reach kernel exports. Both allocation domains use `kfree()` (`c/kernel/mm/phys/kheap.c`, `c/kernel/module/modload.c`).

`kmalloc_quiet()` uses the same allocation domain as `kmalloc()`, with allocation shortage reported through a null return. It suppresses the message about heap growth failure and the OOM policy hook. Scheduler observation pools use this interface for sizes derived from syscall arguments (`c/kernel/sched/observe/observe_platform.c`).

## Reservations and faults

A `struct vma` describes an interval aligned to pages, current protection, a protection ceiling, a backing offset and an optional handle to a file or shared memory. Each backed area owns a reference to its object. Cloning and splitting acquire references; releasing areas drops them. Fixed tables provide reservation metadata to the fault path without heap allocation (`c/kernel/mm/virt/vma.h`, `c/kernel/mm/virt/vma.c`).

Small unhinted reservations search the preferred legacy mapping window first. Requests larger than that window, and requests with wide hints, search the wide window. Exhausting legacy placement also leads to the wide window. Sparse VMM range walks skip absent translation subtrees.

The exception handler calls `mm_fault()` for page faults before continuing exception handling. The resolver handles these states (`c/kernel/cpu/irq/interrupts.c`, `c/kernel/mm/virt/fault.c`):

| State | Resolution |
|---|---|
| Untouched anonymous reservation | Allocate and zero a frame; install VMA permissions |
| Untouched file reservation | Acquire a page-cache frame reference and map it |
| Untouched reservation for shared memory | Acquire a reference to an existing segment frame |
| Write fault on a COW page | Restore write access for a sole owner, or copy the frame for the writer |
| Swap entry | Read the slot into a frame and restore saved permissions |

Swap classification precedes demand filling because the slot holds the page’s preserved contents.

Fork clones VMAs and populated user translation trees. With copy on write enabled, writable private pages turn read only and are marked COW in both processes. Entries marked `VMM_PTE_SHM` point both processes at the same frames with the same permissions (`c/kernel/mm/virt/vmm.c`).

Protection changes update VMA metadata before rebuilding resident permissions. Making a resident page `PROT_NONE` moves its frame reference into a nonpresent entry marked `VMM_PTE_NOACCESS`. Restoring protection or unmapping then recovers that recorded ownership (`c/kernel/mm/mmsys.c`, `c/kernel/mm/virt/vmm.c`).

## Page cache and shared backing

The page cache matches files by device and inode, then indexes pages by cache file handle and page index. Each cached frame has a reference owned by the cache; mappings acquire additional references. File faults obtain their reference through `pcache_get_ref_counted()` (`c/kernel/mm/cache/pcache.c`).

The VFS adapter obtains identity from file metadata and contents through offset reads. Sequential accesses trigger bounded readahead, with hits advancing the access history too. VFS mutation hooks invalidate cache entries; existing mappings hold their own references to the frames they received (`c/kernel/mm/cache/pcache_vfs.c`, `c/fs/vfs/vfs.c`).

Creation of a shared memory segment allocates and zeroes its frames. The segment owns a reference to each frame, and faults acquire mapping references. The reference held by the segment also keeps those frames outside reclaim’s ownership equality (`c/kernel/mm/shm.c`).

Initialization follows these dependencies: `pmm_init()` initializes the reverse map; kernel initialization allocates page cache metadata and later installs its VFS backend. The first page fault originating from user mode initializes reclaim and swap (`c/kernel/init/kmain.c`, `c/kernel/mm/virt/fault.c`).

## Reclaim and swap

Reclaim checks ownership using three structures:

`rmap_count(frame) + pcache_holds(frame) == pmm_refcount(frame)`

The reverse map accounts for user mappings, the cache term accounts for its reference, and PMM supplies the total. This admits both mapped file pages and pages held by the cache alone. Heap arenas, page tables and backing for shared memory hold references outside that relation (`c/kernel/mm/reclaim/reclaim.c`).

A persistent clock hand scans physical frames. Accessed mappings receive a second chance. For a candidate, reclaim tries the guards of its owning address spaces, revalidates mappings and references, then revokes writable translations and completes invalidation before examining contents.

The eviction choices are:

1. Drop a cached file page whose contents can be read again.
2. Drop an anonymous page whose contents are all zero.
3. Preserve the remaining contents in swap.

Scanning triggered by allocation starts below `pmm_total_frames() / 32` free frames and aims toward `pmm_total_frames() / 16`, subject to a scan budget. An episode without progress is bounded by two complete clock revolutions. Below the low watermark, exhausting that allowance makes `reclaim_stalled()` report a stall. A productive scan, recovery to the low watermark, disabling reclaim or an OOM decision resets the episode. Atomic state prevents a scan from restoring counts belonging to an episode that ended while it ran.

Swap uses 4 KiB slots with reference counts. Device selection excludes the root device and its related parent or partitions, rejects a LogitFS signature, and accepts swap magic or a first sector of all zeros. Initialization clears the slot reference table and rewrites the header for the current boot (`c/kernel/mm/reclaim/swap.c`).

Eviction installs swap entries and pins the source frame before writing. Swap transfers are serialized. A failed write restores the saved mappings; after a page is swapped back in, the slot reference is released.

## Exhaustion recovery

Anonymous and COW payload allocation can reach OOM policy after reclaim fails. Stalled reclaim also takes this path: `fault_frame()` returns an allocated frame to PMM and records the reason for declining the fault. `mm_fault_in()` releases the guard on the address space before calling `oom_fault_decide()`, then re-walks translations for any retry (`c/kernel/mm/virt/fault.c`).

The earlier “retry once” description covers the retry following an OOM decision. A shortage during swap-in also has a preceding reclaim opportunity. After reserve allocation and emergency reclaim fail, `reclaim_swapin()` returns `OOM_SWAPIN_NOMEM` and keeps the swap entry and slot. Outside the guard, the fault path requests a bounded scan for one frame and retries if it frees memory. A further allocation shortage reaches one OOM decision and a final retry.

OOM policy first reaps exited processes, then uses resident tallies from the reverse map to choose an eligible live victim. It prefers the largest process without a window when its tally reaches `reclaim_high()`; otherwise it chooses the largest eligible process. Waiting for another victim accepts any free frame for swap-in, while anonymous and COW allocation wait for free frames above the reserve (`c/kernel/mm/reclaim/oom.c`).

The decision distinguishes recovery, selection of the caller or an expired wait, and failure to select a victim. A frame withheld because reclaim stalled may be accepted on the final retry when the policy finds no victim, including when another CPU owns the chooser.

## Mapping lifetime and concurrency

Sleeping guards hashed by CR3 serialize address space operations that may allocate or wait for I/O. Shared kernel mappings have a dedicated guard. Reclaim tries owner guards and backs off on contention (`c/kernel/mm/virt/mmguard.h`, `c/kernel/mm/virt/mmguard.inc`).

Unmapping clears entries and holds the frame references until the required TLB invalidation completes. Resident protection changes also request invalidation across cores when another core runs the address space. This orders translation retirement before frame reuse (`c/kernel/mm/virt/vmm.c`, `c/kernel/cpu/tlb.c`).

## Limitations

- The 64 TiB window of the direct map is an address reservation. Flat PMM metadata must fit in available low RAM; reverse map and page cache metadata also require contiguous allocations in low memory.
- VMA storage provides 64 records per address space with 32 areas each. Splitting consumes additional areas (`c/kernel/mm/virt/vma.h`).
- Heap growth requires contiguous frames, and freed blocks stay inside arenas owned by the heap (`c/kernel/mm/phys/kheap.c`).
- File mappings are read only; the page cache has no writeback path for dirty pages (`c/kernel/mm/cache/pcache.h`).
- Reclaim skips pinned frames, incomplete reverse maps and frames with more than 16 user mappings. Frames backing shared memory and frames held by a `PROT_NONE` entry also fail its ownership test (`c/kernel/mm/reclaim/reclaim.c`).
- Swap requires an eligible device and caps storage at 262,144 slots. Swapped COW sharers return in private frames (`c/kernel/mm/reclaim/swap.c`, `c/kernel/mm/reclaim/reclaim.c`).
- Protection updates skip swapped entries; swap-in restores saved permissions without consulting current VMA protection. Rebuilding resident permissions also drops `VMM_PTE_SHM` and can assign COW behavior to shared memory (`c/kernel/mm/virt/vmm.c`).
- A TLB shootdown that misses its acknowledgement bound halts the initiating CPU, which keeps ownership of its pages (`c/kernel/cpu/tlb.c`).
- `vmm_map_mmio_wc()` rejects user addresses, pages identified as available RAM and paths that require splitting a large page. Callers must establish PAT setup and quiesce previous users. PAT and write combining behavior for MMIO are marked unverified on X79-VG4 hardware (`c/kernel/mm/virt/vmm.c`, `c/kernel/cpu/power/pat.c`).

## See also

- [Architecture overview](overview.md)
- [Physical memory](../kernel/physical-memory.md)
- [Virtual memory](../kernel/virtual-memory.md)
- [Kernel heap](../kernel/kernel-heap.md)
- [Page cache, reclaim and swap](../kernel/page-cache-and-reclaim.md)
- [Shared memory](../kernel/shared-memory.md)

## Source map

| File | What it contains |
|---|---|
| `include/abi/logit_boot.h` | Bootstrap mapping contract and module bounds |
| `c/boot/bios/loader.asm` | BIOS bootstrap page tables |
| `c/boot/efi/loader.c` | UEFI bootstrap tables and low handoff page |
| `linker.ld` | Kernel placement and image boundaries |
| `c/kernel/init/bootmod.c` | Module validation and physical range holds |
| `c/kernel/init/kmain.c` | Memory, cache and framebuffer initialization order |
| `c/kernel/mm/mm.h` | User windows, PTE masks and fault interfaces |
| `c/kernel/mm/mmhost.h` | Conversions between physical addresses and kernel aliases |
| `c/kernel/mm/phys/physmap.h` | Boundary of low memory and layout of the direct map |
| `c/kernel/mm/phys/pmm.c` | Metadata, reservations, frame ownership, allocation and construction of the direct map |
| `c/kernel/mm/phys/pmm.h` | Frame allocation, reference, pin and reserve interfaces |
| `c/kernel/mm/phys/kheap.c` | Heap arenas, bins, coalescing, magazines and shortage handling |
| `c/kernel/module/modload.c` | Low allocation for executable kernel modules |
| `c/kernel/sched/observe/observe_platform.c` | Quiet allocation for observation pools |
| `c/kernel/mm/virt/vmm.c` | Mapping, cloning, protection, MMIO and teardown |
| `c/kernel/mm/virt/vmm.h` | Resident, COW, shared, no access and swap encodings |
| `c/kernel/mm/virt/vma.h` | Area records, protection fields and table limits |
| `c/kernel/mm/virt/vma.c` | Reservation placement, splitting and backing references |
| `c/kernel/mm/virt/fault.c` | Fault classification, page population and recovery retries |
| `c/kernel/mm/virt/mmguard.h` | Contracts for address space and shared kernel guards |
| `c/kernel/mm/virt/mmguard.inc` | Sleeping guards and registry of live address spaces |
| `c/kernel/mm/cache/pcache.c` | Cache identity, frame references, readahead and invalidation |
| `c/kernel/mm/cache/pcache.h` | Cache interfaces and scope of read-only mappings |
| `c/kernel/mm/cache/pcache_vfs.c` | VFS metadata and adapter for offset reads |
| `c/fs/vfs/vfs.c` | Cache invalidation after filesystem mutations |
| `c/kernel/mm/reclaim/rmap.c` | Reverse-map allocation and mapping records |
| `c/kernel/mm/reclaim/reclaim.c` | Clock scanning, ownership checks, eviction and swap-in |
| `c/kernel/mm/reclaim/swap.c` | Device selection, slot references and serialized transfers |
| `c/kernel/mm/reclaim/oom.c` | Resident tallies, victim selection and bounded waits |
| `c/kernel/mm/reclaim/oom.h` | Fault recovery outcomes and stall interfaces |
| `c/kernel/mm/shm.c` | Frame allocation and ownership for shared segments |
| `c/kernel/mm/mmsys.c` | Memory syscall coordination |
| `c/kernel/exec/load/elf.c` | Segment permissions and file-backed load ranges |
| `c/kernel/cpu/irq/interrupts.c` | Exception hook for page faults |
| `c/kernel/cpu/tlb.c` | Invalidation across cores and acknowledgement handling |
| `c/kernel/cpu/power/pat.c` | PAT programming on each CPU |
| `c/kernel/cpu/power/pat.h` | Write-combining flag and mapping interface |
| `c/kernel/cpu/smp/smp.c` | PAT setup during application-processor startup |
| `c/kernel/gui/fb/fb.c` | Framebuffer mappings and cache attribute selection |
| `c/kernel/sched/uthread.c` | Threads sharing their process address space |
