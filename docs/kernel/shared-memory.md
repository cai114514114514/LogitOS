# Shared memory

LogitOS shared memory lets processes communicate through mappings of the same physical pages. A segment owns the backing frames, process handles grant access, and virtual memory areas describe each view. Named segments connect unrelated processes; anonymous shared mappings pass to children through fork. The implementation spans `c/kernel/mm/shm.c`, `c/kernel/exec/shmfd.c`, and the virtual memory code.

## Objects and process handles

The segment registry in `c/kernel/mm/shm.c` stores each segment’s frames, page count, name, creator UID, mode, and reference count. Creation allocates and zeroes every backing page, giving callers an allocation result before they start using the segment. If allocation fails partway through, it releases the frames already obtained.

`seg_fill()` uses `pmm_alloc_any_nowait()` while holding the registry lock. This allocator respects the normal reserve floor and keeps reclaim, device waits, and OOM policy outside that critical section (`c/kernel/mm/phys/pmm.c`).

Each `struct proc` contains an `fd` table for file descriptions and a `shm` table for shared memory handles. `NSHM` provides 16 shared memory handle slots per process. Each `struct proc_shm_handle` records a segment index and a write access flag (`c/kernel/exec/proc.h`).

These tables assign integers within their own namespaces, so the same number can identify both a file descriptor and a segment handle. Applications use `lshm_map()` and `lshm_close()` for segment handles, and `munmap()` for mapped views (`c/apps/libc/include/logit_shm.h`).

`proc_shm_install()` transfers an acquired segment reference into a process handle. `proc_shm_acquire()` resolves a handle under `fd_lock`, checks any requested write access, and takes a temporary segment reference. This reference keeps the segment alive while a mapping is created, including when another thread closes the handle concurrently (`c/kernel/exec/shmfd.c`).

## User interface

The shared memory ABI defines four calls in `include/abi/logit_abi.h`. Their handlers live in `c/kernel/mm/mmsys.c`; the named libc interface lives in `c/apps/libc/src/shm.c`.

| System call | Number | Libc interface | Operation |
|---|---:|---|---|
| `SYS_SHM_OPEN` | 176 | `lshm_open()` | Open or create a named segment; a null name at the syscall boundary creates an anonymous segment |
| `SYS_SHM_MAP` | 177 | `lshm_map()` | Map a byte length at an offset measured in pages |
| `SYS_SHM_UNLINK` | 178 | `lshm_unlink()` | Remove a segment’s name |
| `SYS_SHM_CLOSE` | 179 | `lshm_close()` | Release a process handle |

The declarations and flags are in `c/apps/libc/include/logit_shm.h`. `LSHM_CREAT` requests creation, `LSHM_EXCL` rejects an existing name, and `LSHM_WRITE` requests write access. Names contain 1–31 bytes and exclude `/`. They serve as lookup keys in the segment registry.

Creation takes a size measured in pages of 4096 bytes and records the creator’s UID and mode. Opening an existing segment uses its established size and checks read permission, plus write permission when requested. The permission check selects owner or other mode bits from the caller’s UID; UID zero bypasses it. Unlinking requires write permission (`c/kernel/mm/shm.c`).

For anonymous sharing, libc’s `mmap()` handles `MAP_SHARED | MAP_ANONYMOUS` by creating an unnamed segment, mapping it, and closing the temporary handle. This path requires `fd == -1` and a zero offset. The mapping holds its own segment reference after the handle is closed (`c/apps/libc/src/mman.c`).

At the syscall boundary, mapping returns a base address or zero on failure. After local argument validation, `lshm_map()` translates zero to `MAP_FAILED` with `ENOMEM`. `lshm_close()` reports an invalid handle through `EBADF` (`c/apps/libc/src/shm.c`).

## Mapping semantics

`lshm_map()` accepts a length in bytes, an offset in pages, and protection flags. The syscall encoding places the page offset in the low 32 bits of the third argument and protection flags in the upper 32 bits. The handler converts the offset to bytes and calls `vma_reserve_shm()` (`c/kernel/mm/mmsys.c`).

`vma_reserve_shm()` rounds the length up to pages, checks the rounded range against the segment size, and reserves virtual address space. Each resulting `struct vma` records:

| Field | Meaning for a shared mapping |
|---|---|
| `shm` | Backing segment index |
| `foff` | Byte offset in the segment corresponding to the area’s start |
| `prot` | Current access permissions |
| `max_prot` | Read and execute permissions, plus write permission when granted by the handle |

The VMA takes a segment reference. Its backing information and permission ceiling are attached while the VMA table lock is held, so concurrent readers see the completed mapping metadata (`c/kernel/mm/virt/vma.c`).

One segment can have several views at different virtual addresses and offsets. `vma_shm_at()` computes the backing page index from `foff` and the address’s displacement within the area.

Backing frames exist from creation; page table entries are installed on access. `do_shm()` obtains the segment frame, takes a physical memory reference, and maps it with `VMM_PTE_SHM`. Write and execute permissions come from the VMA (`c/kernel/mm/virt/fault.c`).

The VMA carries the handle’s write permission ceiling through handle closure, fork, and area splits. `vma_protect()` checks requested permissions against that ceiling before changing the areas (`c/kernel/mm/virt/vma.c`).

## Lifetime and fork

Segment lifetime follows references held by handles, VMAs, and operations in progress. A named segment remains available for later opens when its reference count reaches zero. Unlinking clears its name; an unnamed or unlinked segment is freed when its final reference is released (`c/kernel/mm/shm.c`).

A named exchange can follow this sequence:

1. Participants open the segment and create their mappings.
2. A participant with write permission unlinks the name after the intended participants have opened it.
3. Participants close their handles while their mappings retain segment references.
4. Participants unmap their views, releasing the remaining references.

Segment references and physical frame references account for different ownership. Each segment owns one reference per backing frame, and each installed shared page table entry owns another. During teardown, VMA cleanup can release the segment before the page table walk releases its entries. `pmm_free()` decrements the frame’s reference count and returns the frame to the allocator when it reaches zero (`c/kernel/mm/shm.c`, `c/kernel/mm/phys/pmm.c`).

Fork preserves sharing through three mechanisms:

- `proc_shm_clone()` copies handles and their write rights, taking a segment reference for each inherited handle.
- `vma_space_clone()` copies areas and takes a segment reference for each inherited shared area.
- The `VMM_PTE_SHM` branch in `vmm_clone_user_counted()` takes a frame reference and installs the child’s page table entry with the parent’s permissions.

The page table marker lets fork preserve shared writes. Applying copy on write here would give a writer a private frame and break communication (`c/kernel/mm/virt/vmm.c`, `c/kernel/mm/virt/vmm.h`).

Splitting an area takes another segment reference. Trimming its front advances `foff`, preserving the association between the remaining addresses and their segment pages (`c/kernel/mm/virt/vma.c`). Process exit closes handles through `proc_shm_close_all()`; address space teardown releases VMA and frame references (`c/kernel/exec/proc.c`, `c/kernel/mm/virt/vmm.c`).

## Interaction with reclaim

A live segment holds one physical memory reference per backing page. Each installed mapping adds another reference.

Reclaim’s initial candidate check requires the reverse mapping count plus any page cache reference to equal the physical reference count. A segment frame with N mapped entries has N reverse mappings and N+1 references, so the check excludes it. Before its first mapping, the frame has a segment reference and no reverse mappings, which also excludes it (`c/kernel/mm/reclaim/reclaim.c`).

This ownership rule preserves frame identity under memory pressure. Reconstructing shared bytes into different frames would break communication between participants. The segment’s reference keeps its backing frames resident throughout the segment’s lifetime (`c/kernel/mm/shm.h`). Shared page faults obtain those existing frames through `do_shm()` (`c/kernel/mm/virt/fault.c`).

## Users and inspection

Libc provides anonymous sharing through `mmap()` and named sharing through the `lshm_*` interface. File mappings use the `SYS_MMAP_FILE` path in `c/apps/libc/src/mman.c`.

Process inspection copies VMA metadata through `c/fs/procfs/procfs_src.c`. The maps output in `c/fs/procfs/procfs.c` labels segment mappings with `shm:` followed by the segment index and prints the backing byte offset. `shm_report()` prints the live segment count, held pages, memory usage, and reference error count (`c/kernel/mm/shm.c`).

`tests/unit/mm_shm_test.c` contains host assertions for frame identity across address spaces and fork, mapping offsets, reclaim exclusion, unlink lifetime, and VMA protection metadata. `tests/unit/shmfd_test.c` uses modelled segment references to check process handle ownership, inherited rights, and references retained during mapping.

## Limitations

- `SHM_SEGMAX` permits eight segments globally, and `SHM_PAGEMAX` permits 512 pages per segment: 2 MiB each and 16 MiB total. Segment size is fixed at creation, and contents disappear on reboot (`c/kernel/mm/shm.h`).
- Protection changes can break sharing for resident pages. `vmm_protect_range_in()` drops `VMM_PTE_SHM` when rebuilding an entry and marks writable pages for copy on write when their reference count exceeds one. A segment frame already has references from its segment and mapping, so a subsequent write can produce a private copy. The protection case in `tests/unit/mm_shm_test.c` checks VMA metadata without calling this page table update path (`c/kernel/mm/virt/vmm.c`, `c/kernel/mm/virt/fault.c`).
- Initial `PROT_NONE` requests receive read permission. `mprotect()` accepts zero access permissions, subject to the resident page defect above. The fault classifier requires `VMA_READ` for an untouched shared page, so views requesting write or execute permission without read permission fail on first access (`c/kernel/mm/mmsys.c`, `c/kernel/mm/virt/fault.c`).
- Access checks implement owner and other classes; group mode bits have no matching permission class (`c/kernel/mm/shm.c`).
- Futex wait and wake keys derive from the virtual address and address space, so shared mappings across processes lack a supported futex synchronization mechanism. `sem_init()` rejects a nonzero process sharing argument with `ENOSYS` (`c/kernel/sched/uthread.c`, `c/apps/libc/src/pthread.c`).
- Within `c/apps/`, named `lshm_*` call sites appear only in libc.

## See also

- [Virtual memory](virtual-memory.md)
- [Page cache, reclaim and swap](page-cache-and-reclaim.md)
- [Process management](processes.md)
- [File descriptors, polling and PTYs](files-and-polling.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/mm/shm.c` | Segment registry, allocation, permissions, references, and diagnostics |
| `c/kernel/mm/shm.h` | Segment API, resource limits, and ownership rationale |
| `c/kernel/exec/shmfd.c` | Process handle installation, acquisition, cloning, and closure |
| `c/kernel/exec/proc.h` | Process tables and shared memory handle representation |
| `c/kernel/exec/proc.c` | Handle inheritance and process cleanup |
| `c/kernel/mm/mmsys.c` | Shared memory, unmapping, and protection syscall handlers |
| `include/abi/logit_abi.h` | Syscall numbers, arguments, and return conventions |
| `c/kernel/mm/virt/vma.h` | Shared mapping metadata |
| `c/kernel/mm/virt/vma.c` | Reservation, offsets, references, and permission ceilings |
| `c/kernel/mm/virt/fault.c` | Shared frame installation and copy on write faults |
| `c/kernel/mm/virt/vmm.h` | Shared page table marker |
| `c/kernel/mm/virt/vmm.c` | Fork, page protection, and address space teardown |
| `c/kernel/mm/phys/pmm.c` | Frame allocation and reference counting |
| `c/kernel/mm/reclaim/reclaim.c` | Reclaim ownership checks |
| `c/apps/libc/include/logit_shm.h` | Named shared memory interface and handle contract |
| `c/apps/libc/src/shm.c` | Named API validation and syscall translation |
| `c/apps/libc/src/mman.c` | Anonymous shared mapping and memory protection wrappers |
| `c/kernel/sched/uthread.c` | Futex wait and wake keys |
| `c/apps/libc/src/pthread.c` | Semaphore implementation |
| `c/fs/procfs/procfs_src.c` | Transfer of VMA metadata to procfs |
| `c/fs/procfs/procfs.c` | Shared mapping labels in process maps |
| `tests/unit/mm_shm_test.c` | Mapping, fork, reclaim, lifetime, and VMA metadata assertions |
| `tests/unit/shmfd_test.c` | Process handle ownership and reference assertions |
