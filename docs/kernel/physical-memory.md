# Physical memory

LogitOS manages physical memory in 4 KiB frames. The allocator in `c/kernel/mm/phys/pmm.c` combines an allocation bitmap with ownership references, reclaim pins and diagnostic metadata. Firmware descriptors identify available RAM. Allocation policies preserve memory below 1 GiB for callers that use identity addresses while allowing user pages and heap arenas to use high RAM through the kernel’s physical map.

## Frames and metadata

`FRAME_SIZE` is 4096 bytes, defined in `c/kernel/mm/phys/pmm.h`. A physical address divided by this size gives the frame number used to index five flat metadata arrays.

| Metadata | Representation | Purpose |
|---|---|---|
| Allocation bitmap | One bit per frame | Marks allocated or reserved frames. |
| Reference counts | One `uint16_t` per frame | Records ownership references. |
| Reclaim pins | One byte per frame | Counts nested requests to protect a frame against eviction. |
| Poison bitmap | One bit per frame | Records free frames containing a valid diagnostic pattern. |
| RAM provenance bitmap | One bit per frame | Records whole pages covered by accepted available-memory descriptors. |

The allocation bitmap and reference table obey the same invariant: a frame’s allocation bit is set exactly when its reference count is nonzero. Ownership references allow multiple mappings to share a frame and return it to the free pool when the last reference is released.

All five arrays cover frame numbers from zero through the highest accepted, page-aligned endpoint of available RAM, including intervening holes. This layout gives reference and pin operations direct access to their metadata. Storage grows by three bitmap bits, a two-byte reference count and a one-byte pin count per tracked frame, plus alignment padding (`c/kernel/mm/phys/pmm.c`).

During initialisation, `metadata_place()` searches available RAM below 1 GiB, using the aligned kernel end as its lower bound. `metadata_available()` checks coverage across adjacent or overlapping available descriptors in any order. Candidate placement advances past overlapping boot information, then `skip_held()` advances it past held ranges. This protects loader data from the writes that initialise metadata. The allocator reserves the selected metadata extent while preserving available RAM between that extent and the kernel (`c/kernel/mm/phys/pmm.c`).

## Establishing the free pool

The native boot protocol defines memory descriptors in `include/abi/logit_boot.h`. `logit_boot_normalize()` in `c/kernel/init/bootinfo.c` validates the boot header and tag framing, then translates tag identifiers into the internal representation consumed by the kernel. Memory descriptor payloads retain their layout. This explains the Multiboot2 structure names in `c/kernel/mm/phys/pmm.c`.

An accepted available-memory descriptor has type 1, a nonzero length and an extent contained within `PHYSMAP_SIZE`. The allocator rounds its start up and its end down to frame boundaries, then accepts the resulting interval when it contains at least one whole frame. Reservations cover every frame touched by their byte range (`c/kernel/mm/phys/pmm.c`).

Before `pmm_init()`, kernel initialisation calls `bootmod_init()` to validate boot module tags and record their physical extents through `pmm_hold_range()`. The UEFI loader classifies module allocations as available RAM, so these holds preserve data such as AMD Polaris microcode. Module tags carry 64-bit physical endpoints, allowing modules above 4 GiB. The PMM hold table has room for 64 ranges, rounded outward to whole frames (`c/kernel/init/kmain.c`, `c/kernel/init/bootmod.c`, `c/kernel/init/bootmod.h`, `c/boot/efi/loader.c`, `include/abi/logit_boot.h`).

`pmm_init()` establishes the pool in this order:

1. Read the memory map, determine the tracked physical span and place metadata.
2. Initialise every tracked frame as reserved, with one reference.
3. Record RAM provenance and release available whole frames below 1 GiB.
4. Reserve memory from physical zero through the aligned kernel end, the metadata extent, boot information and held ranges.
5. Build physical-map aliases for low RAM, then high RAM.
6. After completing all high mappings, release available high frames and reapply held reservations.

The repeated reservation matters because releasing available descriptors clears the allocation bits of held module pages as well (`c/kernel/mm/phys/pmm.c`).

After its boot accounting report, `pmm_init()` calls `rmap_init()`. Kernel initialisation later calls `pcache_init(pmm_total_frames())`. These initialise the reverse-map and page-cache bookkeeping used by reclaim before user page faults can trigger it (`c/kernel/mm/phys/pmm.c`, `c/kernel/init/kmain.c`).

## Zones and allocation policies

Firmware regions populate one space of frame numbers. Allocation policy divides that space at `PMM_LOW_LIMIT`, defined as `0x40000000`, or 1 GiB, in `c/kernel/mm/phys/physmap.h`.

| Zone | Physical range | Use |
|---|---|---|
| Low | Below 1 GiB | Page tables and kernel or driver objects accessed through the identity mapping. |
| High | From 1 GiB through the tracked span | RAM accessed through the physical map. |

Both zones share the allocation and ownership tables. Each zone has its own cursor for allocations of a single frame. Reserved frames and gaps between available regions remain occupied in the bitmap (`c/kernel/mm/phys/pmm.c`).

| Interface | Allocation policy |
|---|---|
| `pmm_alloc()` | One low frame, subject to reserve floors. |
| `pmm_alloc_any()` | One high frame when available, then low RAM, subject to reserve floors. |
| `pmm_alloc_any_nowait()` | The same zone preference and reserve floors, using a direct scan under the allocator lock. |
| `pmm_alloc_reserve()` | One low frame with access to reserve capacity. |
| `pmm_alloc_reserve_any()` | High RAM followed by low RAM, with access to reserve capacity. |
| `pmm_alloc_contig()` | A contiguous run below 1 GiB. |
| `pmm_alloc_contig_masked()` | A contiguous run satisfying an address mask, alignment and boundary constraints. |

These interfaces return physical addresses, with zero indicating failure. Every claimed frame starts with one ownership reference (`c/kernel/mm/phys/pmm.h`, `c/kernel/mm/phys/pmm.c`).

Scans for a single frame examine bitmap words of 64 bits, skip occupied words and wrap once through the selected zone. When the tracked span extends beyond 4 GiB, the high cursor starts at 4 GiB. It can wrap into the range between 1 and 4 GiB after exhausting the higher range.

The masked contiguous allocator searches at or above 4 GiB first, then between 1 and 4 GiB, then below 1 GiB. Each candidate run fits within one search range. The allocator checks the complete run before claiming any of its frames.

The mask is an inclusive address limit formed from consecutive low one bits, including the all-ones mask. It constrains the allocation’s final byte. Alignment and boundary arguments are byte counts expressed as powers of two, or zero. Zero alignment selects page alignment; smaller valid alignments are raised to page alignment. A zero boundary permits an unrestricted extent, while a nonzero boundary requires the complete allocation to fit within one boundary window (`c/kernel/mm/phys/pmm.c`).

Heap growth follows these policies in `grow()`. General heap arenas use masked contiguous allocation and `mm_physmap_ptr()`, including when their backing frames are low. The heap’s low-memory domain uses `pmm_alloc_contig()` and the identity alias (`c/kernel/mm/phys/kheap.c`).

## The physical map

`c/kernel/mm/phys/physmap.h` defines a 64 TiB virtual window beginning at `PHYSMAP_BASE`, `0xffff800000000000`. Addresses within that window follow this relation:

```text
virtual address = PHYSMAP_BASE + physical address
```

The builder maps whole pages from accepted available-memory descriptors in both zones. Fully covered, aligned intervals can use 2 MiB leaves; edges use 4 KiB leaves. This keeps mappings within the accepted RAM intervals.

Physical-map entries permit supervisor reads and writes. Leaves carry NX when EFER.NXE is enabled. Page tables come from low RAM through `pmm_alloc_contig(1)`, making them accessible through the initial identity mapping while the map is being constructed (`c/kernel/mm/phys/pmm.c`).

Address conversion helpers live in `c/kernel/mm/mmhost.h`.

| Helper | Kernel behaviour |
|---|---|
| `mm_p2v()` | Uses the identity alias below 1 GiB and the physical-map alias above it. |
| `mm_physmap_ptr()` | Uses the physical-map alias for both low and high addresses. |
| `mm_v2p()` | Converts an address in the low identity range or physical-map window back to a physical address. |

The builder reloads CR3 after installing mappings, then publishes readiness. `pmm_physmap_low_ready()` reports completion of low aliases; `pmm_physmap_ready()` reports completion of the full map. `vmm_new_space()` retains the shared physical-map entries when constructing a process address space (`c/kernel/mm/phys/pmm.c`, `c/kernel/mm/virt/vmm.c`).

`pmm_is_ram()` checks every frame touched by a byte range against the RAM provenance bitmap. Provenance includes available RAM occupied by the kernel, metadata or boot modules. `bootmod_find()` combines this check with the readiness flag appropriate to the module’s extent before returning its bytes through `mm_p2v()` (`c/kernel/mm/phys/pmm.c`, `c/kernel/init/bootmod.c`).

## Ownership and reclaim pins

`pmm_ref()` adds an ownership reference to an allocated frame. `pmm_free()` drops one reference; the transition to zero returns the frame to the free pool. Releasing a contiguous allocation requires a reference drop for every constituent frame.

Reference counts saturate at `PMM_REF_MAX`, 65535. The increment that reaches this value succeeds and retains the frame for the rest of the boot. Later reference requests fail, and releases preserve the saturated count. This trades a retained frame for protection against premature reuse after reference-count overflow (`c/kernel/mm/phys/pmm.c`).

Reclaim pins protect access to a frame’s current location. `pmm_pin()` and `pmm_unpin()` maintain a nesting count, and reclaim consults `pmm_pincount()` when selecting candidates. Pin users retain ownership references throughout their access. The pin count saturates at 255, making its reclaim protection permanent (`c/kernel/mm/phys/pmm.h`, `c/kernel/mm/reclaim/reclaim.c`).

Runtime allocation, reference and pin operations take `pmm_lock` while saving and disabling local interrupts. Heap growth takes the heap lock before the PMM lock. `pmm_alloc()` and `pmm_alloc_any()` call `reclaim_on_alloc()` before acquiring the PMM lock, allowing reclaim to release frames through allocator operations (`c/kernel/mm/phys/pmm.c`).

## Reserves and allocation pressure

The default reserve is 128 frames, or 512 KiB. The older comment in `c/kernel/mm/phys/pmm.h` describes 32 frames; the initialiser in `c/kernel/mm/phys/pmm.c` sets 128 and records that bursts of faults reading pages back from swap exhausted the earlier cushion. `pmm_set_reserve()` adjusts the threshold.

Allocations of a single frame that obey the reserve require the global free count to exceed the threshold. Taking a low frame also requires the low-zone free count to exceed it. These checks preserve a cushion for low-memory callers even when high RAM is plentiful. Reserve allocation and both contiguous allocation interfaces use direct scans and can consume this capacity (`c/kernel/mm/phys/pmm.c`).

`reclaim_swapin()` obtains its destination through `pmm_alloc_reserve_any()`. If allocation still fails after emergency reclaim, it returns `OOM_SWAPIN_NOMEM`, preserving the swap entry and slot. `mm_fault_in()` can then request a larger bounded reclaim pass after releasing its address-space guard and retry through a fresh page-table walk (`c/kernel/mm/reclaim/reclaim.c`, `c/kernel/mm/reclaim/oom.h`, `c/kernel/mm/virt/fault.c`).

Reclaim bounds an episode without progress to two revolutions of the tracked frame span. One revolution can clear accessed bits; the next can evict pages that remain unused. Progress, recovery to the low watermark, disabling reclaim or an OOM decision resets the episode. On an initial anonymous or copy-on-write allocation attempt, `fault_frame()` gives an obtained frame back if `reclaim_stalled()` reports exhaustion. The resulting memory decline can reach one OOM decision outside the address-space guard. A retry permitted by that decision accepts an available frame and walks the page tables again (`c/kernel/mm/reclaim/reclaim.c`, `c/kernel/mm/virt/fault.c`).

## Accounting and diagnostics

The accounting interfaces distinguish firmware RAM capacity, the tracked address span and current ownership.

| Interface | Meaning |
|---|---|
| `pmm_total_bytes()` | Sum of accepted available intervals after inward page alignment. |
| `pmm_total_frames()` | Highest accepted, aligned endpoint divided by `FRAME_SIZE`, including intervening holes. |
| `pmm_used_frames()` | Frames with nonzero references, including reserved frames. |
| `pmm_free_frames()` | Tracked frames minus used frames. |
| `pmm_free_bytes()` | Free frames multiplied by `FRAME_SIZE`. |
| `pmm_shared_frames()` | Frames with at least two references. |
| `pmm_refs_total()` | Sum of stored frame reference counts. |
| `pmm_pinned_frames()` | Frames whose ownership reference count saturated. |
| `pmm_pins_live()` | Frames with a nonzero reclaim pin count. |

These definitions follow `c/kernel/mm/phys/pmm.c`. Multiplying `pmm_total_frames()` by the frame size gives the tracked physical span. The comment for `frames_total` in `struct logit_meminfo` calls it usable RAM; its assignment in `c/kernel/mm/mmsys.c` uses the span count.

Available RAM occupied by boot modules contributes to `pmm_total_bytes()`. Holding those pages also contributes reservation references to `pmm_used_frames()` and `pmm_refs_total()` (`c/kernel/mm/phys/pmm.c`).

Zone and mapping counters provide further detail:

| Interface | Meaning |
|---|---|
| `pmm_low_zone_free_frames()` | Current free frames below 1 GiB. |
| `pmm_high_free_frames()` | Current free frames at or above 1 GiB. |
| `pmm_high_live_frames()` | Frames consumed from the high free pool established at boot. |
| `pmm_high_allocations()` | Cumulative frame allocations in high RAM. |
| `pmm_high_max_phys()` | Highest allocated high frame base. |
| `pmm_physmap_pages()` | Mapped 4 KiB page equivalents across both zones. |
| `pmm_physmap_table_pages()` | Page-table frames allocated by the physical-map builder. |

`pmm_report()` prints ownership, zone, metadata and reserve statistics. Allocation also records the lowest global free-frame count it observes and announces decreases in steps of 256 frames by default. `pmm_low_free_frames()` returns this historical watermark (`c/kernel/mm/phys/pmm.c`).

`pmm_audit()` walks the metadata, checks bitmap/reference agreement, recomputes ownership totals and low-zone availability, and compares them with the running counters.

Poisoning detects writes through stale pointers by filling frames on their final release and checking the pattern when they are allocated again. Level 1, the default, covers the first `PMM_POISON_HEAD` bytes, or 64 bytes. Level 2 covers the whole frame. `pmm_set_poison()` clears poison validity when changing levels so that later checks use matching extents. Detected violations are reported and counted through `pmm_bugs()` (`c/kernel/mm/phys/pmm.c`, `c/kernel/mm/phys/pmm.h`).

Host fixtures contain checks for reference transitions, saturation and poisoning in `tests/unit/mm_pmm_test.c`; mapping construction and allocation constraints in `tests/unit/physmap_test.c`; and metadata placement across descriptors, firmware holes and boot information in `tests/unit/pmm_metadata_test.c`.

## Limitations

- The 64 TiB window describes virtual layout capacity. Flat metadata must fit in available low RAM. Metadata-placement or low-alias failure disables the allocator; high-map failure retains the low pool and leaves high frames reserved (`c/kernel/mm/phys/pmm.c`).
- Contiguous allocation can fail through fragmentation despite substantial aggregate free capacity. Masked runs must also fit within one of its three search ranges (`c/kernel/mm/phys/pmm.c`).
- Address conversion uses arithmetic and range checks. Callers must establish ownership and mapping readiness; RAM provenance alone proves neither (`c/kernel/mm/mmhost.h`, `c/kernel/mm/phys/pmm.h`).
- Allocated frames retain existing contents. Callers requiring zero-filled memory initialise it. Default poisoning overwrites the first 64 bytes on final release (`c/kernel/mm/phys/pmm.c`).
- Overlapping available descriptors contribute repeatedly to `pmm_total_bytes()`. Bitmap release counts each available frame once (`c/kernel/mm/phys/pmm.c`).

## See also

- [Memory model](../architecture/memory-model.md)
- [Virtual memory](virtual-memory.md)
- [Kernel heap](kernel-heap.md)
- [Page cache, reclaim and swap](page-cache-and-reclaim.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/mm/phys/pmm.c` | Frame allocation, metadata placement, physical-map construction, held ranges and accounting. |
| `c/kernel/mm/phys/pmm.h` | Allocator interfaces, ownership and pin contracts, and frame constants. |
| `c/kernel/mm/phys/physmap.h` | Zone boundary, physical-map window and readiness interfaces. |
| `c/kernel/mm/mmhost.h` | Physical-address conversion and host-test adaptation. |
| `include/abi/logit_boot.h` | Boot memory descriptors, module ranges and initial identity-map contract. |
| `c/boot/efi/loader.c` | UEFI memory classification and boot module allocation. |
| `c/kernel/init/bootinfo.c` | Boot information validation and normalisation. |
| `c/kernel/init/bootmod.c` | Module validation, physical holds and lookup readiness checks. |
| `c/kernel/init/bootmod.h` | Boot module interfaces and lifetime contract. |
| `c/kernel/init/kmain.c` | Module, allocator and page-cache initialisation order. |
| `c/kernel/mm/virt/vmm.c` | Process address spaces and shared kernel mappings. |
| `c/kernel/mm/virt/fault.c` | Payload allocation and fault retries after reclaim or OOM decisions. |
| `c/kernel/mm/phys/kheap.c` | Heap arena allocation and alias selection. |
| `c/kernel/mm/reclaim/reclaim.c` | Reclaim pins, pressure handling and swap-in allocation. |
| `c/kernel/mm/reclaim/oom.h` | Memory-shortage results and OOM decision interfaces. |
| `c/kernel/mm/mmsys.c` | Allocator statistics exposed to userland. |
| `include/abi/logit_abi.h` | Definition of `struct logit_meminfo`. |
| `tests/unit/mm_pmm_test.c` | Reference, saturation, poisoning and accounting checks. |
| `tests/unit/physmap_test.c` | Physical-map and allocation-policy checks. |
| `tests/unit/pmm_metadata_test.c` | Metadata-placement checks. |