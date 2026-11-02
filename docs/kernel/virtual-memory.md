# Virtual memory

LogitOS combines x86-64 page tables with virtual memory areas that describe reserved user ranges, permissions and backing objects. Page faults supply anonymous pages, resolve copy on write, map cached file pages and restore swapped contents. The main implementation is in `c/kernel/mm/virt/`; `c/kernel/mm/mmsys.c` handles memory system calls, and `c/kernel/cpu/tlb.c` coordinates translation invalidation across CPUs.

## Address spaces and page tables

The address translation walker uses four levels: PML4, PDPT, page directory and page table. Each level selects nine address bits. User mappings use 4 KiB leaf pages, and CR3 identifies the physical frame containing the PML4. In `c/kernel/mm/virt/vmm.c`, `vmm_map_page_in()` allocates intermediate tables as needed, while `vmm_pte()` locates an existing leaf entry.

The address layout reserves two user windows. The bounds below include the start and exclude the end.

| Virtual range | Purpose |
|---|---|
| `0x40000000` to `0x80000000` | Legacy user window under PML4 entry 0, PDPT entry 1 |
| `0x0000010000000000` to `0x0000800000000000` | Wide user window, from 1 TiB to 128 TiB, under PML4 entries 2–255 |
| `PHYSMAP_BASE`, `0xffff800000000000`, through the following 64 TiB | Supervisor window for direct RAM mappings |

`mm_user_range()` in `c/kernel/mm/mm.h` checks that a complete interval fits within one user window. This protects kernel mappings in the intervening gap and catches overflowing ranges through subtraction from the window’s end.

`vmm_new_space()` creates a PML4 and a private low PDPT. It copies the kernel roots, clears PDPT entry 1 in the private copy, and clears the wide user roots. The other roots share kernel page tables. Supervisor mapping operations edit the canonical kernel tree and publish newly created shared roots into registered address spaces under the kernel mapping guard. See `c/kernel/mm/virt/vmm.c`.

Entries in page tables contain physical addresses. `MM_PTE_ADDR` extracts bits 12–51, and `MM_PTE_FLAGS` preserves the complementary bits when carrying permissions and software state into another entry. Keeping these masks distinct preserves NX without incorporating it into a frame address.

Kernel code accesses RAM through `mm_p2v()` in `c/kernel/mm/mmhost.h`. Physical addresses below 1 GiB use the identity alias; higher addresses use the supervisor direct map. The builder in `c/kernel/mm/phys/pmm.c` maps whole pages from firmware regions marked available. It uses aligned 2 MiB leaves where an entire interval fits and 4 KiB leaves at boundaries.

Boot modules can occupy regions marked available. `c/kernel/init/kmain.c` calls `bootmod_init()` before `pmm_init()`. The registry in `c/kernel/init/bootmod.c` submits accepted module ranges to `pmm_hold_range()`. The allocator reserves the covered frames and places its metadata outside those ranges, preserving module contents while reclaiming other available RAM.

## Device mappings and cache attributes

`vmm_map_mmio_wc()` in `c/kernel/mm/virt/vmm.c` maps a device aperture at its supervisor identity address using writable 4 KiB entries with `VMM_PAT_WC`. The PAT encoding and initialization are defined in `c/kernel/cpu/power/pat.h` and `c/kernel/cpu/power/pat.c`.

The caller must initialize PAT on every online CPU, quiesce users of the aperture and retire incompatible mappings before invoking the helper. The mapping path publishes shared roots and completes translation invalidation after changes, including changes made before a later allocation failure.

## Virtual memory areas

A **virtual memory area**, or VMA, describes a reserved interval and the policy used to supply its pages. A private anonymous reservation can cover a large range while physical pages arrive as the process accesses it.

`struct vma` in `c/kernel/mm/virt/vma.h` records:

| Fields | Meaning |
|---|---|
| `start`, `end` | Interval aligned to a page |
| `prot` | Current read, write and execute permissions |
| `max_prot` | Permission ceiling retained across protection changes |
| `file`, `shm` | Page cache file handle or handle of a shared segment |
| `foff` | Byte offset in the backing object corresponding to `start` |
| `used` | Occupied table slot |

Anonymous areas have both backing handles set to `-1`. Each backed area owns a reference to its backing object. Cloning or splitting an area acquires another reference; removing an area releases its reference. Front trimming advances `foff` by the removed byte count, keeping virtual addresses aligned with the correct backing offsets. These transitions are implemented in `c/kernel/mm/virt/vma.c`.

The VMA table uses static storage, allowing fault lookup to inspect metadata without allocating it. A spinlock protects the table. References needed by a clone or split are acquired while that lock protects the area; releases occur after unlocking because they can free backing resources. The guard for the address space coordinates operations spanning VMAs, page tables and backing references.

Placement uses first fit. Small requests with a zero hint begin between `MM_MMAP_BASE`, `0x60000000`, and `MM_MMAP_TOP`, `0x7F000000`. Requests exceeding that interval and requests with wide hints search the wide window. Exhausting legacy placement also leads to a wide search. A usable hint is rounded down to a page boundary and checked against placement windows and existing reservations. Kernel callers use fixed reservation helpers for addresses chosen by the program loader.

## Mapping system calls

`c/kernel/exec/syscall.c` forwards memory calls to `mm_syscall()` in `c/kernel/mm/mmsys.c`. RAX carries the syscall number; RDI, RSI and RDX carry its arguments. The public definitions are in `include/abi/logit_abi.h`.

| Call | Arguments | Operation |
|---|---|---|
| `SYS_MMAP` | Length, protection, hint | Reserve anonymous memory |
| `SYS_MMAP_FILE` | Pointer to `struct logit_mmap_file_req` | Reserve a file mapping |
| `SYS_MPROTECT` | Address, length, protection | Update reservation permissions and resident translations |
| `SYS_MUNMAP` | Address, length | Release reservations and mapped resources |

The C wrappers in `c/apps/libc/src/mman.c` translate protection flags into the kernel ABI and convert failures into `errno`.

A private anonymous `mmap()` reserves a VMA. Its first access enters the anonymous fault path. Anonymous `MAP_SHARED` mappings create an unnamed segment through `SYS_SHM_OPEN`, map it through `SYS_SHM_MAP`, then close the temporary handle through `SYS_SHM_CLOSE`. The VMA’s segment reference keeps the mapping alive. Segment creation allocates its backing frames in `c/kernel/mm/shm.c`.

The file request carries a hint, length, byte offset, descriptor and protection. The kernel acquires the descriptor, opens its stored path through the page cache, and reserves a VMA with its own backing reference. That reference allows the mapping to survive closing the descriptor. The syscall requires an offset aligned to 4 KiB.

The cache backend in `c/kernel/mm/cache/pcache_vfs.c` accepts regular files with a reported inode identity and reads their contents through the VFS interface for reads at an offset. This identity allows mappings of the same file to share cached frames.

## Changing protections

`SYS_MPROTECT` requires a start aligned to a page and rounds the length up to cover the final page. Under the guard for the address space, it performs two steps:

1. `vma_protect()` validates complete reservation coverage, permission ceilings and available slots for boundary splits, then updates the VMAs.
2. `vmm_protect_range_in()` rebuilds resident entries.

Validation precedes translation changes because VMA editing can fail when coverage or table capacity is insufficient. VMAs then supply permissions for future anonymous, file and segment faults, while resident entries enforce permissions on pages already supplied. See `c/kernel/mm/virt/vma.c` and `c/kernel/mm/virt/vmm.c`.

Anonymous areas receive read, write and execute ceilings. File areas receive read and execute ceilings. Segment areas retain the write right granted by the open handle, allowing an authorized mapping to remove and later restore write permission.

Protection zero creates a guard interval. For a resident page, the kernel clears the present bit and records `VMM_PTE_NOACCESS`, retaining the frame reference and contents. Restoring access rebuilds a present entry around that frame.

For private resident pages, granting write permission uses the frame reference count. A sole owner receives hardware write permission; multiple references produce `VMM_PTE_COW`. Removing write permission clears the COW marker because the fault classifier treats that marker as authority to resolve a write fault.

The software encodings are defined in `c/kernel/mm/virt/vmm.h`.

| Encoding | Meaning |
|---|---|
| `VMM_PTE_COW` | Write access resolved through copy on write |
| `VMM_PTE_ANON` | Anonymous page originating from zero filling |
| `VMM_PTE_FILE` | Page originating from file backing |
| `VMM_PTE_SHM` | Frame belonging to a shared segment |
| `VMM_PTE_NOACCESS` | Resident frame retained in a nonpresent entry |
| Swap entry | Nonpresent entry containing a swap-slot reference and saved state |

## Page faults

Exception vector 14 calls `mm_fault()` from `c/kernel/cpu/irq/interrupts.c`, passing the address from CR2, the processor’s error code and the instruction pointer. A successful resolution returns through the interrupt exit path to retry the instruction.

`mm_fault_in()` resolves an address in a specified space. Its internal guarded lookup reads the current PTE and VMA state. If another thread has already installed an entry permitting the access, it invalidates the local translation when that space is active and returns success. Otherwise, `mm_fault_classify()` selects the resolution. Both functions are in `c/kernel/mm/virt/fault.c`.

| Fault class | Resolution |
|---|---|
| Copy on write | Grant write access to a sole owner, or allocate and copy 4 KiB |
| Anonymous | Allocate and zero a frame, deriving write and NX bits from the VMA |
| File | Acquire a referenced frame through `pcache_get_ref_counted()` and install a file entry |
| Shared segment | Reference an existing segment frame and apply VMA permissions |
| Swap | Restore saved contents through `reclaim_swapin()` |

Swap classification precedes filling from a VMA. This preserves evicted contents: an anonymous reservation containing a swap entry must recover its saved bytes. The entry also carries enough backing information to restore directly mapped loader pages that have no VMA.

A COW copy carries flags through `MM_PTE_FLAGS`, updates the reverse map, flushes translations for the affected space, then releases the old frame reference. The source frame is pinned while allocation can invoke reclaim.

Anonymous and COW payload allocations use `pmm_alloc_any()`, allowing high RAM. Allocation failure triggers emergency reclaim and another allocation attempt. Reclaim can also report a stall after two complete clock revolutions free no frames while memory remains below the low watermark. In that case, `fault_frame()` returns an allocated payload frame to the allocator and reports `OOM_FAULT_STALLED`.

The stall accounting in `c/kernel/mm/reclaim/reclaim.c` resets after progress, recovery to the low watermark, disabling reclaim or an OOM decision. Atomic accounting marks an overlapping scan stale when its episode ends, so that scan’s result cannot reinstate the old stall.

Swapping in uses `pmm_alloc_reserve_any()`. If allocation still fails after emergency reclaim, it returns `OOM_SWAPIN_NOMEM` while retaining the swap entry and slot reference. `mm_fault_in()` then attempts `reclaim_frames(1)` after its internal guarded lookup returns and repeats the lookup if frames were freed.

Each invocation of `mm_fault_in()` makes at most one call to `oom_fault_decide()`. When recovery waits for memory, swapping in accepts any free frame; anonymous and COW recovery waits for frames above the allocator’s reserve. A permitted final retry walks the tables again and accepts an available payload frame despite a new stall report. The outcome where no victim could be marked permits that retry for `OOM_FAULT_STALLED`. See `c/kernel/mm/virt/fault.c`, `c/kernel/mm/reclaim/oom.c` and `c/kernel/mm/reclaim/oom.h`.

Unresolved user page faults are offered as `LOGIT_SIGSEGV`. An accepted signal proceeds through signal delivery; the fallback attempts a core dump and terminates the process. Unresolved kernel faults enter the panic path. This disposition is implemented in `c/kernel/cpu/irq/interrupts.c`.

## Fork and frame ownership

`vmm_clone_user_counted()` copies VMA metadata and walks allocated trees of user page tables. With COW enabled and a frame reference available, writable private pages become shared entries with hardware write permission cleared and `VMM_PTE_COW` set in both spaces. Each child mapping acquires a reference. The clone falls back to copying private pages when sharing is unavailable.

Entries carrying `VMM_PTE_SHM` retain their shared frame and permissions across cloning. Swap entries acquire another slot reference. Entries carrying `VMM_PTE_NOACCESS` preserve their contents through a frame reference, with a copy fallback when that reference cannot be acquired. See `c/kernel/mm/virt/vmm.c`.

Frame references account for mappings and ownership by backing objects such as the page cache and shared segments. `pmm_free()` decrements the count and returns a counted frame to the allocator when it reaches zero. This lets an unmap release one mapping’s ownership while other mappings continue using the frame.

## Unmapping and teardown

`SYS_MUNMAP` rounds the requested interval outward to page boundaries. It calls `vma_release()` before clearing entries in page tables, removing the reservation policy that would authorize a later fault to refill the range.

`vma_release()` removes overlapping areas, trims their ends or splits an area around a hole. It checks for a required split slot before mutation. A successful release requires overlap with at least one reservation and accepts gaps between reservations. See `c/kernel/mm/virt/vma.c`.

`vmm_unmap_range_in()` releases resources according to the entry’s encoding:

| Entry | Resource released |
|---|---|
| Present user page | Reverse map record and frame reference |
| `VMM_PTE_NOACCESS` | Retained frame reference |
| Swap entry | Swap slot reference |

Frame references wait in a fixed batch controlled by `UNMAP_HOLD`, which defaults to 64. The function clears entries and invalidates local translations when the space is active. Before releasing each held batch, it waits for a shootdown when another CPU is using the space. Frames remain owned until those cached translations have been retired.

Range walks skip the coverage of absent intermediate tables, making sparse reservations practical to traverse. Range unmapping retains the frames that hold page tables for reuse. `vmm_free_user()` releases user mappings and their private tables; `vmm_free_space()` also retires the registration of the address space and releases its low PDPT and PML4. These paths are in `c/kernel/mm/virt/vmm.c`.

## TLB coordination

`vmm_switch()` publishes the previous and incoming address spaces around the CR3 write, then clears the record for the previous space. `vmm_space_busy_elsewhere()` checks both records on other CPUs. This accounts for translations belonging to either space during a switch.

`tlb_flush_all()` in `c/kernel/cpu/tlb.c` reloads the initiating CPU’s CR3 and serializes the broadcast transaction. It publishes a request flag for each peer CPU, sends interrupt vector 240 and waits for acknowledgements.

`tlb_service()` atomically claims the current CPU’s request, reloads CR3 and acknowledges completion. Both the IPI handler and the wait loop of the ticket lock call it. Polling from `c/kernel/cpu/spinlock.c` allows a CPU waiting with interrupts masked to service a pending request.

| Operation | Invalidation |
|---|---|
| New user page supplied by a fault | Local page invalidation when the target space is active |
| COW frame replacement | Local CR3 reload when active, plus a broadcast when the space is used elsewhere, before releasing the old reference |
| `munmap` | Local invalidations and a broadcast before releasing each held batch when needed |
| Resident `mprotect` changes | Local invalidations and one broadcast after the range when needed |
| Shared kernel mapping changes | Broadcast after publishing changes |
| Address space switch | CR3 reload |

Callers select when a broadcast is needed. Once invoked, `tlb_flush_all()` requests acknowledgement from every peer CPU.

## Synchronization and kernel copies

`MM_GUARD` acquires a recursive sleeping lock selected by a hash of CR3. Hash collisions serialize the affected spaces. Operations involving two spaces acquire their guards in hash order. Shared supervisor mappings use an additional kernel mapping guard, ordered after the guards for address spaces. Sleeping ownership allows transactions to span allocation and I/O. See `c/kernel/mm/virt/mmguard.h` and `c/kernel/mm/virt/mmguard.inc`.

`user_copy_to()` and `user_copy_from()` process memory one page at a time under the guard for the address space. `vmm_pin_user_page()` resolves access, takes a frame reference and pins the frame before the copy uses its kernel RAM alias. The reference protects frame lifetime across unmapping; the pin protects against reclaim. The copy path records accessed and dirty state because alias access bypasses the user translation. See `c/kernel/exec/usercopy.c` and `c/kernel/mm/virt/vmm.c`.

## Limitations

- VMA storage has slots for 64 address spaces and 32 areas per space. Splits consume additional area slots. Cloning of user huge pages is rejected. The 64 TiB window of the direct map describes address layout; allocator capacity also depends on fitting its metadata in low RAM. See `c/kernel/mm/virt/vma.h`, `c/kernel/mm/virt/vmm.c` and `c/kernel/mm/phys/physmap.h`.
- `mmap(PROT_NONE)` produces readable protection, and libc rejects `MAP_FIXED`. Guard intervals use a subsequent `mprotect(PROT_NONE)`. Writable file mappings are rejected because the cache lacks writeback; `msync()` returns `ENOSYS`. See `c/apps/libc/src/mman.c` and `c/kernel/mm/mmsys.c`.
- Faults for absent anonymous, file and segment pages require `VMA_READ`, including reservations carrying write or execute permission alone. Resident `PROT_NONE` frames retain references outside the reverse map and are unavailable to reclaim. See `c/kernel/mm/virt/fault.c` and `c/kernel/mm/virt/vmm.h`.
- The comment above `vmm_protect_range_in()` says swap-in obtains protection from the VMA. The implementation skips swap entries, and `reclaim_swapin()` restores saved entry permissions. A protection change while a page is swapped can be lost when it returns. See `c/kernel/mm/virt/vmm.c` and `c/kernel/mm/reclaim/reclaim.c`.
- Resident protection changes discard `VMM_PTE_SHM`. Granting write permission can mark a segment frame COW, allowing a later write to create a private copy. See `c/kernel/mm/virt/vmm.c` and `c/kernel/mm/virt/fault.c`.
- Cloning invalidates changed source permissions locally and reloads the active source CR3. The clone function lacks a broadcast shootdown for sibling CPUs using the source space. See `c/kernel/mm/virt/vmm.c`.
- The “subset of reserved space” description of `vma_release()` in `c/kernel/mm/virt/vma.h` is stricter than the implementation’s overlap requirement.
- Older TLB comments describe a timeout that returns. The current implementation attempts a raw UART diagnostic and halts the initiating CPU while retaining the transaction lock if acknowledgements are missing or excessive. `TLB_WAIT_SPINS` defaults to 50,000,000 iterations. See `c/kernel/cpu/tlb.c`.
- `vmm_map_mmio_wc()` rejects RAM, user addresses and present leaf mappings with another physical address or cache type. It supports neither splitting of huge pages nor transitions of cache type while a mapping is live. Its source records no X79-VG4 hardware verification. See `c/kernel/mm/virt/vmm.c`.

## See also

- [Memory model](../architecture/memory-model.md)
- [Physical memory](physical-memory.md)
- [Page cache, reclaim and swap](page-cache-and-reclaim.md)
- [Shared memory](shared-memory.md)
- [Program loading](program-loading.md)
- [Synchronisation](synchronization.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/mm/mm.h` | User windows, PTE masks and fault interfaces |
| `c/kernel/mm/mmhost.h` | RAM aliases and architecture helpers |
| `c/kernel/mm/virt/vmm.c` | Page tables, mapping, cloning, protection, unmapping, teardown and CPU tracking |
| `c/kernel/mm/virt/vmm.h` | Mapping interfaces and software PTE encodings |
| `c/kernel/mm/virt/vma.c` | Reservation placement, backing references, splitting and protection validation |
| `c/kernel/mm/virt/vma.h` | VMA structure, permissions and table capacities |
| `c/kernel/mm/virt/fault.c` | Fault classification, resolution and recovery retries |
| `c/kernel/mm/virt/mmguard.h` | Address-space and kernel mapping guards |
| `c/kernel/mm/virt/mmguard.inc` | Guard ownership and address space registry |
| `c/kernel/mm/mmsys.c` | Memory syscall validation and dispatch |
| `c/kernel/mm/phys/physmap.h` | Direct-map layout and readiness interfaces |
| `c/kernel/mm/phys/pmm.c` | Frame allocation, references, boot reservations and direct-map construction |
| `c/kernel/init/kmain.c` | Initialization order |
| `c/kernel/init/bootmod.c` | Module validation and registration of held ranges |
| `c/kernel/mm/cache/pcache.c` | Referenced acquisition of cached file pages |
| `c/kernel/mm/cache/pcache_vfs.c` | File identity validation and VFS reads |
| `c/kernel/mm/shm.c` | Shared segment storage and frame lookup |
| `c/kernel/mm/reclaim/reclaim.c` | Reclaim, stall accounting and swap-in |
| `c/kernel/mm/reclaim/oom.c` | OOM decisions and recovery waits |
| `c/kernel/mm/reclaim/oom.h` | Recovery outcomes and stall interfaces |
| `c/kernel/cpu/power/pat.c` | PAT initialization |
| `c/kernel/cpu/power/pat.h` | Write combining encoding and MMIO mapping declaration |
| `c/kernel/cpu/tlb.c` | Shootdown transactions, acknowledgements and fatal failure handling |
| `c/kernel/cpu/spinlock.c` | Shootdown servicing during ticket lock waits |
| `c/kernel/cpu/irq/interrupts.c` | Page fault entry, TLB IPI dispatch and fault disposition |
| `c/kernel/exec/syscall.c` | Forwarding of memory syscall arguments |
| `c/kernel/exec/usercopy.c` | Copies through referenced and pinned RAM aliases |
| `include/abi/logit_abi.h` | Public syscall definitions and file mapping request |
| `c/apps/libc/src/mman.c` | C mapping wrappers and error translation |
