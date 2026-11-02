# procfs and ramfs

Procfs exposes kernel state through generated files under `/proc`. Ramfs provides writable, volatile storage from a static memory pool. Both use the VFS filesystem interface: procfs renders process records, memory counters and time information on demand, while each ramfs instance owns its directory entries and file contents. Their implementations are in `c/fs/procfs/procfs.c` and `c/fs/ramfs/ramfs.c`.

## Mounting and VFS integration

Both backends attach a `struct fs_iops` to a `struct filesystem`, defined in `c/fs/vfs/vfs.h`. The VFS resolves paths, checks permissions and passes paths relative to the selected mount point to the backend. A transaction owner for each mount serializes backend operations and protects shared rendering and directory enumeration buffers (`c/fs/vfs/vfs.c`).

After mounting the root filesystem successfully, the boot code attempts to create `/proc` and calls `procfs_mount()`. This function mounts the procfs singleton and records the prefix used by the file descriptor layer to recognise generated paths (`c/kernel/init/kmain.c`, `c/fs/procfs/procfs.c`).

`ramfs_create()` claims an instance with its own label, entries and byte arena. The filesystem control handler creates an instance and attaches it through `vfs_mount_at()`. A mount point below the root must already exist as a directory (`c/fs/ctl/vfsctl.c`, `c/fs/vfs/vfs.c`).

## What procfs exposes

The procfs root contains three system files, the caller’s `self` directory and numeric directories for processes whose table state is running or zombie. Process enumeration copies PIDs under the process table lock (`c/kernel/exec/proc.c`).

| Path | Contents |
|---|---|
| `/proc/meminfo` | Physical allocator counters, kernel heap statistics and address space count. |
| `/proc/uptime` | Monotonic elapsed seconds with two decimal places and a trailing newline. |
| `/proc/version` | The string returned by `procfs_src_version()`, followed by a newline. |
| `/proc/self` | A directory resolved to the calling process’s PID during each operation. |
| `/proc/<pid>/stat` | A compact process record on one line. |
| `/proc/<pid>/status` | Process information as labelled fields. |
| `/proc/<pid>/cmdline` | The stored process name followed by a NUL byte. |
| `/proc/<pid>/maps` | Virtual memory areas, permissions, backing handles and offsets. |

The namespace parser accepts positive decimal PIDs without leading zeroes. Resolving `self` inside the backend lets the same path follow each caller (`c/fs/procfs/procfs.c`).

The source interface in `c/fs/procfs/procfs.h` supplies copied process records, memory areas and counters. This keeps accounting with the subsystems that maintain it and lets the procfs unit test exercise the backend using controlled source data.

## Memory and time

`/proc/meminfo` publishes these fields (`c/fs/procfs/procfs.c`):

| Fields | Meaning |
|---|---|
| `MemTotal`, `MemFree`, `MemUsed` | Physical allocator frame counts multiplied by the frame size. |
| `FrameSize`, `Frames` | Frame size in bytes and total frame count. |
| `KHeapArena`, `KHeapLive`, `KHeapFree` | Heap arena bytes, live block accounting and bytes on heap free lists. |
| `KHeapAllocs`, `KHeapFrees`, `KHeapGrows` | Heap allocator allocation, free and arena growth counters. |
| `AddrSpaces` | Live address space count from the virtual memory area subsystem. |

Values labelled `kB` are byte counts divided by 1024. `FrameSize` is in bytes; `Frames`, the heap operation counters and `AddrSpaces` are counts.

The adapter in `c/fs/procfs/procfs_src.c` reads the physical allocator, heap and virtual memory area counters. `KHeapLive` includes blocks retained in per CPU magazines because the heap continues to account those blocks as allocated (`c/kernel/mm/phys/kheap.c`).

Uptime comes from `time_mono_ms()`, which measures elapsed milliseconds since time initialisation. The renderer converts that value into seconds and hundredths (`c/fs/procfs/procfs_src.c`, `c/kernel/core/ktime.h`).

## Process records

The `stat` record has this field order:

```text
pid (comm) state ppid tid fds gui
```

The fields report the PID, stored name, process state, parent PID, scheduler thread ID, open descriptor count and window ownership flag. State is `Z` for a zombie, `K` for a process marked by an accepted kill request, and `R` otherwise. Zombie state takes precedence over the kill mark (`c/fs/procfs/procfs.c`, `c/kernel/exec/proc.c`).

The `ps` parser finds the final closing parenthesis before reading the remaining fields. This accommodates stored names containing spaces or parentheses (`c/apps/coreutils/ps.c`).

`status` presents `Name`, `State`, `Pid`, `PPid`, `Tid`, `FDs`, `Gui`, `Cwd`, `Caps` and `FsPrefix`. `FDs` contains the descriptor count and limit, separated by `/`. `Caps` is a hexadecimal capability bitmap. Empty stored names render as `?`; empty working directories and filesystem prefixes render as `/` (`c/fs/procfs/procfs.c`).

Each `maps` line contains a hexadecimal start and end address, three permission characters, a backing description and a hexadecimal backing offset. The backing description is `anon`, `file:<handle>` or `shm:<handle>`. Permission characters represent read, write and execute access; zero permission bits render as `---`. The adapter obtains each area through `vma_nth()` and translates its permission bits into the procfs representation (`c/fs/procfs/procfs_src.c`, `c/kernel/mm/virt/vma.c`).

## Reads and process lifetime

The file descriptor layer recognises generated paths through `procfs_owns_path()`. Opening a generated file records its path and offset. Subsequent reads obtain bytes through `vfs_pread()` (`c/kernel/exec/fd/file.c`).

A read at offset zero renders the file into a cache entry. Later offsets consume that render while it remains cached. Reading again at offset zero refreshes the contents. Size queries render into a scratch buffer, allowing the first read to observe changes made after opening the file (`c/fs/procfs/procfs.c`).

`procfs_src_task()` copies process data under the process table lock and takes the descriptor lock while counting descriptors and copying associated fields. Procfs retains paths and rendered bytes between operations, so a fresh render resolves the PID again (`c/kernel/exec/proc.c`).

If the initial lookup for a fresh render finds that the process has disappeared, the backend returns `VFS_ENOENT`. The descriptor layer converts a negative VFS read result to `-1`. Zombies remain available while their records remain in the process table, and process creation assigns PIDs from the incrementing `next_pid` counter (`c/fs/procfs/procfs.c`, `c/kernel/exec/fd/file.c`, `c/kernel/exec/proc.c`).

Userland consumers demonstrate the interface: `ps` enumerates process directories and reads `stat`, `free` reads `meminfo`, and `uptime` reads the clock file (`c/apps/coreutils/ps.c`, `c/apps/coreutils/free.c`, `c/apps/coreutils/uptime.c`).

## Ramfs storage semantics

Ramfs represents each entry with its complete path relative to the filesystem root, a directory flag, and the offset and size of its data slice. Creation initializes an empty root directory. Static allocation supports both kernel use and host VFS tests (`c/fs/ramfs/ramfs.c`, `c/fs/ramfs/ramfs.h`).

A backend write creates a file or replaces its complete contents. File and directory creation require an existing directory parent. Each file’s bytes occupy a contiguous slice of the arena. A write reuses its slice when the new length is at most the current file size and the slice offset is nonzero. Otherwise, it reserves another slice at the allocation cursor (`c/fs/ramfs/ramfs.c`).

Whole-file reads copy up to the caller’s buffer size. Positional reads copy from the requested offset, return a short count near the end and return zero at or beyond the end. Directory enumeration reports immediate children. Deleting an entry makes its entry slot available for reuse (`c/fs/ramfs/ramfs.c`).

Writable file descriptions buffer edits in the descriptor layer. `file_fsync()` and the last `file_close()` submit dirty contents through `vfs_write()`. Both report a failed backend write as an error (`c/kernel/exec/fd/file.c`).

Rename requires an unused destination and an existing destination parent. For a directory, ramfs checks ancestry, destination collisions and every descendant’s resulting path length before updating the directory and its descendants. A rejected rename preserves the subtree. Deleting a nonempty directory returns `VFS_ENOTEMPTY` (`c/fs/ramfs/ramfs.c`).

During `vfs_umount()`, the VFS drains active operations and invokes the backend’s unmount hook. Ramfs clears its entries, resets its arena cursor and recreates the empty root. The instance owner then releases the pool slot with `ramfs_destroy()` after a successful unmount; the VFS test fixture follows this lifecycle (`c/fs/vfs/vfs.c`, `c/fs/ramfs/ramfs.c`, `tests/unit/vfs_mount_test.c`).

## Limitations

- Procfs provides read and directory operations; mutation callbacks are absent. Its formats are project specific: `stat` has seven fields, `cmdline` contains the stored name, and `uptime` has one column (`c/fs/procfs/procfs.c`).
- Procfs has four render cache entries with 4096-byte buffers, keyed by reader PID and path. Descriptors in one process reading the same path share a render. Eviction can cause a continuation to render again. Output exceeding the emitter’s capacity receives `[procfs: render truncated]` (`c/fs/procfs/procfs.c`).
- The lifetime comment in `c/fs/procfs/procfs.h` says reads of a vanished PID return an error. The implementation checks existence during rendering, so a continuation at a nonzero offset can consume cached bytes after the process disappears (`c/fs/procfs/procfs.c`).
- Collection spans several accessor calls. Memory counters and successive memory areas can change during a render, producing bytes that combine observations from different instants (`c/fs/procfs/procfs_src.c`, `c/kernel/mm/virt/vma.c`).
- Ramfs has four instance slots. Each instance holds 48 entries including the root, a 16 KiB byte arena and 96 bytes per stored path including its terminator (`c/fs/ramfs/ramfs.c`).
- Ramfs deletion and shrinking retain consumed arena space until reset. Writes requiring another slice consume additional space, including rewrites of a slice at offset zero. A failed first write can leave an empty file entry because entry creation precedes the arena capacity check (`c/fs/ramfs/ramfs.c`).
- The filesystem control unmount handler calls `vfs_umount()` and retains the ramfs pool slot. It calls `ramfs_destroy()` when a mount attempt fails, so repeated successful mount and unmount cycles can exhaust the instance pool (`c/fs/ctl/vfsctl.c`).
- Both backends omit filesystem capacity reporting. A VFS capacity request for an existing path returns `VFS_ENOSYS` (`c/fs/procfs/procfs.c`, `c/fs/ramfs/ramfs.c`, `c/fs/vfs/vfs.c`).

## Tests

The repository defines these checks:

| Command | Scope |
|---|---|
| `make test-procfs` | Namespace, formats, caller resolution, refresh behaviour, process disappearance and partial reads using controlled source data. |
| `make test-procfs-os` | QEMU integration through userland programs, including a held uptime descriptor compared with a fresh open. |
| `make test-vfs-mount` | Public VFS operations using ramfs, including mount routing, permissions, links and directory rename. |
| `make test-vfs-mount-asan` | The VFS mount suite with address and undefined behaviour sanitizers. |

`tests/procfs.mk` makes its negative control a prerequisite of the positive procfs test. The control caches renders during size queries; its recipe requires exactly five failed assertions, all labelled as liveness checks.

The QEMU harness invokes `uptime -d`, which opens a descriptor, waits three seconds and compares its first read with a fresh open (`tests/boot/run-procfs-test.sh`, `c/apps/coreutils/uptime.c`).

`tests/ramfs_rename.mk` makes the directory rename control a prerequisite of both VFS mount targets. It requires failures involving descendant contents, moving a directory into a descendant and an overlong descendant path.

## Source map

| File | What it contains |
|---|---|
| `c/fs/procfs/procfs.h` | Source interfaces, copied records and lifetime rationale. |
| `c/fs/procfs/procfs.c` | Namespace, formats, render cache and mount integration. |
| `c/fs/procfs/procfs_src.c` | Memory, time, version and memory area adapters. |
| `c/kernel/exec/proc.c` | Process enumeration, copied records and caller PID lookup. |
| `c/kernel/mm/virt/vma.c` | Memory area enumeration and address space count. |
| `c/kernel/mm/phys/pmm.h` | Physical allocator counters and frame size. |
| `c/kernel/mm/phys/kheap.h` | Heap statistics interface. |
| `c/kernel/mm/phys/kheap.c` | Heap accounting and magazine treatment. |
| `c/kernel/core/ktime.h` | Monotonic clock contract. |
| `c/kernel/exec/fd/file.c` | Generated file reads, buffered writes and flush errors. |
| `c/kernel/init/kmain.c` | Boot setup for `/proc`. |
| `c/fs/ramfs/ramfs.h` | Instance creation and destruction interface. |
| `c/fs/ramfs/ramfs.c` | Static storage, file operations, rename and reset behaviour. |
| `c/fs/vfs/vfs.h` | Filesystem operation interfaces. |
| `c/fs/vfs/vfs.c` | Mount routing, permissions, dispatch and unmount draining. |
| `c/fs/ctl/vfsctl.c` | Ramfs creation and unmount control handling. |
| `c/apps/coreutils/ps.c` | Process enumeration and `stat` parsing. |
| `c/apps/coreutils/free.c` | Memory reporting through procfs. |
| `c/apps/coreutils/uptime.c` | Uptime display and held descriptor diagnostic. |
| `tests/unit/procfs_test.c` | Procfs tests with controlled source data. |
| `tests/unit/vfs_mount_test.c` | VFS and ramfs behaviour tests. |
| `tests/procfs.mk` | Procfs test targets and liveness control. |
| `tests/ramfs_rename.mk` | Ramfs directory rename control. |
| `tests/boot/run-procfs-test.sh` | QEMU integration harness. |
| `Makefile` | VFS mount test recipes. |

## See also

- [Virtual file system](vfs.md)
- [File-system control and tools](fs-tools.md)
- [Process management](../kernel/processes.md)
- [Command-line tools](../userland/coreutils.md)