# Virtual file system

The virtual file system (VFS) resolves paths, selects mounts, checks permissions, retrieves metadata, and dispatches operations to filesystem backends. Callers address the global namespace; each backend receives paths beneath its mounted root. The public interface is in `c/fs/vfs/vfs.h`, with pathname walking, metadata storage, and credentials implemented in adjacent source files.

## Objects and file identity

The VFS uses paths and attributes to represent files.

| Structure | Role |
|---|---|
| `struct filesystem` | Backend name, callbacks, and optional instance state. |
| `struct vmountent` | Mount point, backend pointer, lifecycle state, active references, and operation ownership. |
| `struct vattr` | Type, permissions, ownership, link count, size, allocation, timestamps, and identity. |
| `struct file` | Open description containing a path, access mode, offset, reference count, and buffering state. |

`struct filesystem` is declared in `c/fs/vfs/vfs.h`, and `struct vattr` in `c/fs/vfs/vfs_meta.h`. Mount entries are private to `c/fs/vfs/vfs.c`. The descriptor layer defines `struct file` in `c/kernel/exec/fd/file.h`; `file_open_vfs()` checks access and constructs an open description in `c/kernel/exec/fd/file.c`.

`vfs_statx()` supplies a device value derived from the mount slot, numbered starting at one so that zero represents unknown identity. Together with a backend inode number, this identifies a file for the page cache. The adapter in `c/kernel/mm/cache/pcache_vfs.c` accepts regular files carrying `VA_INO`. Requiring that flag prevents unrelated files with an unknown inode number of zero from sharing a cache entry.

## Path resolution

The walker in `c/fs/vfs/vfs_path.c` maintains a resolved prefix and the input still to process.

| Function | Behaviour |
|---|---|
| `vfs_path_norm()` | Joins relative input to a working directory, collapses slashes, and processes `.` and `..`. |
| `vfs_path_resolve()` | Adds a component callback for symbolic link expansion and traversal checks. |
| `vfs_resolve()` | Supplies the VFS callback and the current credential. |

Successful results are absolute paths. The root is `/`; other results have no trailing slash. Processing `..` removes a component from the prefix already resolved. A symbolic link followed by `..` therefore ascends from the link target.

Symbolic link expansion places the target ahead of the remaining input. A relative target starts from the link’s containing directory; an absolute target restarts at the namespace root. The walker processes expansions iteratively and counts the links followed.

The `follow_final` argument controls whether the last component’s symbolic link is expanded. Reads, writes, and `vfs_stat()` follow it. `vfs_lstat()`, `vfs_readlink()`, and `vfs_delete()` address the link record itself. The VFS component callback obtains link targets from RAM metadata and checks search permission on recorded intermediate directories.

Empty input returns `VFS_ENOENT`. Path or component overflow returns `VFS_ENAMETOOLONG`, and exceeding the expansion bound returns `VFS_ELOOP`. These contracts are declared in `c/fs/vfs/vfs_path.h`; `tests/unit/vfs_path_test.c` contains normalization, expansion, permission-callback, and bounds cases.

## Mounts and operation ownership

A mount binds a namespace prefix to a `struct filesystem`. `vfs_register()` remembers the root backend, and `vfs_mount()` installs it at `/` through `vfs_mount_at()`. In `c/kernel/init/kmain.c`, initialization registers and mounts LogitFS, then creates `/proc` and attempts the procfs mount after root mounting succeeds.

`vfs_mount_at()` normalizes the mount point. For a point below `/`, it checks that the containing backend can enumerate that directory. It reserves a table slot, calls the backend mount hook, and publishes the entry after the hook succeeds.

Routing selects the longest matching prefix at a component boundary. The backend receives the suffix beneath that prefix, with `/` representing the mount point itself. `tests/unit/vfs_mount_test.c` checks this dispatch through a second RAMFS instance and tests paths beside a mount whose names share its prefix.

A backend operation acquires a mount reference and the mount’s operation owner. The table lock protects lookup and reference acquisition; waiting for ownership happens after releasing that lock. Each mount uses a recursive `struct io_domain`, allowing nested calls by the same task while other mounts can progress. Ownership follows the current thread, with a CPU identity used before a thread is available. See `c/fs/vfs/vfs.c` and `c/drivers/core/io_domain.h`.

`vfs_umount()` marks the entry as draining, waits for admitted references to finish, calls the backend unmount hook, removes RAM metadata beneath the mount point, and releases the slot.

Root mounting also connects the VFS to boot diagnostics. `c/kernel/init/kmain.c` records the result for `c/kernel/diag/hwreport.c` and enters the framebuffer log display in `c/kernel/diag/hwscreen.c` on failure. With a mounted root, `c/kernel/diag/hwreport_log.c` creates a numbered report beneath `/var/log` through `vfs_create_file()` and updates it through `vfs_write()`.

## Backend operations

`c/fs/vfs/vfs.h` defines two callback forms. Direct members of `struct filesystem` receive operation arguments. Members of `struct fs_iops` also receive the filesystem instance, giving them access to its `priv` state.

Setting `iops` selects the instance table for the operations it defines. The attribute hooks and `refpath` remain direct members of `struct filesystem`.

| Callback members | Responsibility |
|---|---|
| `mount`, `umount` | Establish and release backend state. |
| `list` | Produce a diagnostic listing. |
| `size`, `read` | Query file length and read starting at byte zero. |
| `pread` | Read a range starting at a byte offset. |
| `count`, `ent_name`, `ent_size`, `ent_is_dir` | Enumerate directories and describe entries. |
| `write` | Create or overwrite a file with supplied complete contents. |
| `del`, `mkdir`, `rename` | Change directory entries. |
| `getattr`, `setattr` | Retrieve and update attributes. |
| `statfs` | Report filesystem capacity and allocation counts. |
| `refpath` | Recover a backend path from persistent file identity. |

LogitFS supplies direct callbacks in `c/fs/logitfs/logitfs.c`. RAMFS and the reader for additional LogitFS devices supply instance tables in `c/fs/ramfs/ramfs.c` and `c/fs/logitfs/lfsro.c`. Those tables use positional initializers, so callback order matters when extending `struct fs_iops`.

The distinction between `read` and `pread` preserves existing callers. LogitFS’s `read` requires a buffer large enough for the complete file. Its `pread` accepts ranges, returns a short read at the end, and returns zero at or beyond the end. The rationale in `c/fs/vfs/vfs.h` explains that repeatedly emulating range reads by reading and discarding a growing prefix would make a sequential scan copy a quadratic amount of data.

`vfs_write()` submits complete file contents. The descriptor layer buffers writable descriptions and flushes through this operation. Eligible descriptions opened for reading stream through `vfs_pread()` after a probe establishes range-read support. See `c/kernel/exec/fd/file.c`.

For callers requiring an absent destination, `vfs_create_file()` holds mount ownership across the existence check and backend write. It repeats resolution after acquiring ownership and compares the result before proceeding.

## Credentials and permissions

`struct vcred` contains a user ID and primary group ID. `c/fs/vfs/vfs_cred.c` stores credentials and supplementary groups in a table keyed by PID. Lookup examines the process’s entry, then its parent chain, then the login session. A system service marker encountered during that walk supplies root credentials. Execution without a current process uses user and group ID zero.

The login session begins as root. `vfs_cred_session_set()` handles the session and caller identity updates under the credential lock after checking privilege. Combining the updates lets login establish the session and drop its own privilege in one call. Credential replacement and supplementary group changes require user ID zero.

`vmeta_permission()` in `c/fs/vfs/vfs_meta.c` selects owner, group, or other permission bits. Group selection includes the primary group and supplementary membership obtained through `vfs_cred_ingroup()`. `MAY_READ`, `MAY_WRITE`, and `MAY_EXEC` are 4, 2, and 1, matching the mode-bit numbering. User ID zero passes read and write checks; an execute request requires at least one execute bit.

The operation checks in `c/fs/vfs/vfs.c` include:

| Operation | Permission decision |
|---|---|
| Read a file | Read permission on the resolved name. |
| Overwrite a file | Write permission on its canonical data path. |
| Create a file or directory | Write and search permission on its containing directory. |
| Delete a name | Write and search permission on its containing directory. |
| Rename | Write and search permission on both containing directories. |
| Create a hard link | Read permission on the source and write and search permission on the destination directory. |
| Enumerate through `vfs_count()` or `vfs_getdents()` | Read permission on the directory. |
| Change mode | File ownership or user ID zero. |
| Change owner or group | User ID zero. |

`file_open_vfs()` checks access before returning an open description. Program loading checks execute permission in `c/kernel/exec/load/exec.c`.

File creation derives permissions from `0666`; directory creation starts from `0777`. Both apply the caller’s creation mask, whose default is `022`. `stamp_create()` sends ownership and mode to the backend attribute setter or RAM metadata store. When both already match the defaults, it saves a metadata record or backend transaction by skipping the stamp.

One boot caller demonstrates these interfaces together. In builds selected by `HWHUD=1` in `Makefile`, `devssh_provision_boot()` runs after root mounting and before login starts. The provisioner in `c/kernel/init/devssh.c` validates the supplied key module, prepares the `dev` account and its SSH files through VFS operations, and creates `/etc/sshd.enabled` after provisioning succeeds. `c/kernel/init/devssh.h` defines the account paths and build guard.

## Metadata and provenance

`attrs_of()` in `c/fs/vfs/vfs.c` uses a backend attribute lookup when both `getattr` and `setattr` are present. A failed lookup falls back to RAM metadata, as does a backend without the callback pair. Requiring both hooks gives attribute lookup and modification the same provider.

The RAM store in `c/fs/vfs/vfs_meta.c` holds permissions, ownership, symbolic link targets, and hard link groups. A path without a RAM record receives root ownership and a default mode of `0755` for a directory or `0644` for a regular file.

An older comment in `c/fs/vfs/vfs_meta.h` describes every backend as using RAM metadata and durable attributes as future work. Current LogitFS supplies both attribute callbacks in `c/fs/logitfs/logitfs.c`; its setter commits mode and ownership through the filesystem transaction machinery.

`struct vattr` carries flags describing the origin of its fields:

| Flag | Meaning |
|---|---|
| `VA_STORED` | Metadata comes from a record; `vfs_statx()` also sets it for a hard link group. |
| `VA_DURABLE` | Recorded mode and ownership reside on the medium. |
| `VA_TIMES` | Backend timestamps are available. |
| `VA_INO` | The backend supplied an inode number. |
| `VA_ID` | Persistent volume identity, object identity, and revision are available. |

These flags qualify particular fields. A result can combine durable ownership and mode with a link count obtained from a RAM group.

`vfs_stat()` retrieves attributes after following symbolic links. `vfs_lstat()` can return the link record itself. `vfs_statx()` adds mount identity and fills missing size information. Directory size represents an entry count; the fallback for a regular file estimates its block count by rounding byte length to 512-byte units. For a symbolic link inspected without following it, size is the target text length.

`c/kernel/exec/load/meta.c` translates attributes and provenance into the metadata ABI in `include/abi/logit_abi.h`. `vfs_statfs()` reports capacity for the filesystem containing an existing path. LogitFS supplies total and free block and inode counts.

## Links, enumeration, and cache invalidation

Symbolic links are RAM records containing target text. Hard links form RAM groups with one canonical member naming the backend data. Data access maps aliases to that canonical path. Deleting the canonical member while another survives promotes a survivor and requests a backend rename. See `c/fs/vfs/vfs_meta.c` and `c/fs/vfs/vfs.c`.

Directory rename preflights metadata paths with `vmeta_rename_check()`. After backend success, `vmeta_renamed()` moves records for the directory and its descendants. Symbolic link targets retain their literal text. `tests/unit/vfs_rename_meta_test.c` contains cases for descendant permissions, link groups, target text, capacity, and refused mutations.

Enumeration combines synthetic provider entries with backend entries. `vfs_getdents()` returns batches of `struct vdirent` records and advances the caller’s cursor. `vfs_ent_name_copy()` copies backend names while holding mount ownership because a backend can return a pointer into scratch storage.

`c/fs/ctl/vfsctl.c` supplies synthetic control and inspection files including `/dev/vfsctl`, `/dev/vfsmounts`, and `/dev/vfsmeta`. Synthetic command writes pass their permission check before dispatch.

Successful writes invalidate the canonical data path in the page cache. Successful renames invalidate both paths, and deletion invalidates the removed name. The calls follow the mutation so invalidation also covers cache contents filled from the earlier data during the operation. The ordering and its rationale are documented beside the calls in `c/fs/vfs/vfs.c`.

## Limitations

- The VFS has no vnode cache, and open descriptions retain paths without holding inode or mount references for their lifetime. Mount references protect active VFS calls. See `c/fs/vfs/vfs.c` and `c/kernel/exec/fd/file.h`.
- Intermediate traversal checks consult RAM metadata through `vmeta_lookup()`, bypassing backend attributes. Directory modes stored by LogitFS are consequently incompletely enforced during traversal. Creation stamping also discards setter and RAM-store errors, so creation can succeed after an ownership or mode stamp fails. See `c/fs/vfs/vfs.c`.
- Symbolic links and hard link groups disappear on reboot. Enumeration omits names held solely in RAM metadata. Canonical hard link deletion changes the RAM group before requesting the backend rename, with no rollback if that rename fails. See `c/fs/vfs/vfs_meta.c` and `c/fs/vfs/vfs.c`.
- The mount table holds eight entries with 64-byte mount point buffers. RAM metadata holds 256 records with 112-byte path and target buffers. The walker uses a 256-byte resolved-path buffer, defaults to eight symbolic link expansions, and limits components to 59 bytes through `LFS_NAME_MAX - 1`. String buffers include their terminating zero; callers and backends can impose smaller limits. See `c/fs/vfs/vfs.c`, `c/fs/vfs/vfs_meta.h`, `c/fs/vfs/vfs_path.h`, and `c/fs/logitfs/logitfs_fmt.h`.
- Repeated mount points or filesystem pointers are rejected. Child mounts prevent unmounting. Rename and hard link creation across mounts return `VFS_EXDEV`; mount points and paths containing mounts prevent rename with `VFS_EBUSY`. See `c/fs/vfs/vfs.c`.
- The operations table has no callback for writing from an offset. Missing range-read or capacity callbacks return `VFS_ENOSYS`; synthetic files handled directly by the VFS reject nonzero range-read offsets. See `c/fs/vfs/vfs.h` and `c/fs/vfs/vfs.c`.
- Directory changes between enumeration calls can cause skipped or repeated entries, as documented in `include/abi/logit_abi.h`. LogitFS and RAMFS rename callbacks reject occupied destinations. See `c/fs/logitfs/logitfs.c` and `c/fs/ramfs/ramfs.c`.

## Source map

| File | What it contains |
|---|---|
| `c/fs/vfs/vfs.h` | Public API, backend operations, capacity information, and directory records. |
| `c/fs/vfs/vfs.c` | Mount lifecycle, dispatch, permissions, metadata routing, enumeration, and invalidation. |
| `c/fs/vfs/vfs_path.h` | Path contracts, errors, and limits. |
| `c/fs/vfs/vfs_path.c` | Normalization and symbolic link expansion. |
| `c/fs/vfs/vfs_meta.h` | Attributes, credentials, permission masks, provenance flags, and store limits. |
| `c/fs/vfs/vfs_meta.c` | RAM metadata, links, creation masks, and permission decisions. |
| `c/fs/vfs/vfs_cred.c` | Process credentials, supplementary groups, and session identity. |
| `c/drivers/core/io_domain.h` | Recursive operation ownership. |
| `c/fs/logitfs/logitfs.c` | LogitFS callbacks, durable attributes, and capacity reporting. |
| `c/fs/logitfs/logitfs_fmt.h` | Format constants, including name capacity. |
| `c/fs/ramfs/ramfs.c` | RAMFS instance operations. |
| `c/fs/logitfs/lfsro.c` | Instance operations for additional LogitFS devices. |
| `c/fs/ctl/vfsctl.c` | Synthetic control and inspection files. |
| `c/kernel/init/kmain.c` | Root mounting, procfs initialization, and boot callers. |
| `c/kernel/init/devssh.h` | Developer account paths and build guard. |
| `c/kernel/init/devssh.c` | Key validation and account provisioning through VFS operations. |
| `c/kernel/diag/hwreport.c` | Root mount status and hardware reports. |
| `c/kernel/diag/hwreport_log.c` | Numbered reports created and updated through the VFS. |
| `c/kernel/diag/hwscreen.c` | Framebuffer log display after root mount failure. |
| `Makefile` | Build configuration, including `HWHUD=1`. |
| `c/kernel/exec/fd/file.h` | Open description state. |
| `c/kernel/exec/fd/file.c` | Access checks, streaming reads, buffering, and flushing. |
| `c/kernel/exec/load/exec.c` | Execute permission checks during loading. |
| `c/kernel/exec/load/meta.c` | Metadata and directory syscall translation. |
| `c/kernel/mm/cache/pcache_vfs.c` | Page cache identity and range-read adapter. |
| `include/abi/logit_abi.h` | Metadata ABI and directory cursor contract. |
| `tests/unit/vfs_path_test.c` | Path normalization, expansion, and bounds cases. |
| `tests/unit/vfs_mount_test.c` | Mount routing, permissions, and link cases. |
| `tests/unit/vfs_rename_meta_test.c` | Metadata subtree rename regression cases. |

## See also

- [Storage stack](../architecture/storage-stack.md)
- [LogitFS](logitfs.md)
- [procfs and ramfs](procfs-and-ramfs.md)
- [File descriptors, polling and PTYs](../kernel/files-and-polling.md)
- [Page cache, reclaim and swap](../kernel/page-cache-and-reclaim.md)
- [File-system control and tools](fs-tools.md)