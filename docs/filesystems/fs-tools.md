# File-system control and tools

LogitOS exposes VFS controls and storage benchmarks through synthetic files under `/dev`. A shared LogitFS checker supports kernel checks, repairs, and host snapshots. Host tools build the root filesystem, preserve selected guest state during rebuilding, and construct BIOS and UEFI boot images.

## VFS control files

`c/fs/ctl/vfsctl.c` provides four synthetic files. Their reads and writes pass through permission checks in `c/fs/vfs/vfs.c`.

| Guest path | Interface |
|---|---|
| `/dev/vfsctl` | Write a command; read its latest response |
| `/dev/vfsmounts` | Read the mount table |
| `/dev/vfsmeta` | Read VFS metadata records and the credential table |
| `/dev/fsbench` | Write a benchmark command; read its report |

The file interface lets existing shell programs reach these controls through the VFS. `c/fs/ctl/vfsctl.h` records the motivation: adding operations through files reuses shell access and permission enforcement, keeping the syscall and utility surface small as operations grow.

In the guest shell:

```sh
echo id > /dev/vfsctl
cat /dev/vfsctl
cat /dev/vfsmounts
```

The response buffer is shared across processes, allowing the process running `cat` to retrieve the response produced by the process running `echo`. Command responses begin with `ok` or `err`.

After dispatch, `vfsctl_write()` returns the accepted byte count. Read the response to obtain the operation’s verdict.

## Control commands

The dispatcher in `c/fs/ctl/vfsctl.c` accepts these commands:

| Syntax | Operation |
|---|---|
| `stat <path>` | Report mode, owner, group, link count, type, and size |
| `access <path> <rwx>` | Check the requested read, write, and execute permissions |
| `chmod <octal> <path>` | Set permission bits |
| `chown <uid> <gid> <path>` | Set ownership |
| `ln <old> <new>` | Create a VFS hard-link record |
| `ln -s <target> <link>` | Create a VFS symbolic-link record |
| `readlink <path>` | Return a symbolic link’s target |
| `mount ramfs <label> <at>` | Create and mount a RAM filesystem |
| `mount lfsro <blkdev> <at>` | Mount a reader for LogitFS on the named block device |
| `umount <at>` | Unmount the filesystem at that path |
| `id` | Report process identity, credentials, login session, and supplementary groups |
| `setuid <uid> [gid]` | Change the calling process’s credentials |
| `su <uid> [gid]` | Change its parent process’s credentials |
| `fdtest <path>` | Exercise shared file offsets and descriptor lifetime |

The control node defaults to root ownership and mode `0644`. VFS checks write permission before dispatch. Ownership changes require root; mode changes accept the owner or root. Credential changes through `vfs_cred_set()` require root. These rules are implemented in `c/fs/vfs/vfs.c`, `c/fs/vfs/vfs_meta.c`, and `c/fs/vfs/vfs_cred.c`.

Both credential commands default the group ID to the supplied user ID. When submitted through `echo`, `setuid` targets that child process, while `su` targets its parent shell.

`fdtest` creates or truncates the supplied file and seeds it with `ABCDEF`. It attaches two descriptors to one open-file description, reads three bytes through each, closes one descriptor, and checks that the survivor reaches end of file. Successful output includes `OFFSET-SHARED` and `DESCRIPTION-SURVIVED-CLOSE`.

## Storage benchmarks

`c/fs/ctl/fsbench.c` measures durations inside the guest with `time_mono_ns()`. Timing reports include the median, minimum, maximum, and sample count. Each report line also reaches the serial log with a `[bench]` prefix.

In the guest shell, using the packaged browser file:

```sh
echo launch /browser.aex > /dev/fsbench
cat /dev/fsbench
```

| Command | Measurement or action |
|---|---|
| `blk <sectors> <reps>` | Time reads from the root block device at the requested transfer size |
| `file <path> [reps]` | Time size lookup and file reads with cold and warm caches |
| `launch <path> [reps]` | Time size lookup, allocation rounded to 512 bytes, reading, and their combined duration |
| `cache` | Report buffer-cache activity and device-read counters |
| `run <blocks>` | Set the maximum LogitFS read run, clamped to 1–128 blocks |
| `all [reps]` | Sweep transfer sizes and compare read runs of 1 and 128 blocks |
| `openfd <path>` | Measure descriptor heap usage and compare descriptor reads with a reference |
| `openmax` | Probe descending heap allocation sizes from 512 MiB to 1 MiB, stopping at the first success |

Timing commands accept 1–32 repetitions. An omitted count or a count outside that range selects nine samples for raw reads and five for file and launch measurements.

Cold preparation calls `logitfs_cache_cold()` in `c/fs/logitfs/logitfs.c`, which synchronizes dirty buffers and then drops the buffer cache upon successful synchronization. Warm measurements retain cache state between reads.

The `launch` report includes device-command and block counts from its final repetition. The `all` command compares read runs using the packaged browser and clock files, then restores the previous run setting.

`openfd` reads the file into a reference buffer, computes an FNV-1a checksum, and releases the buffer. It then measures heap usage around descriptor opening and reads through that descriptor in 4 KiB chunks. Its report compares checksum and byte count and shows device traffic for both paths.

From the repository root, the standard benchmark table is available through:

```sh
make bench-fs BENCH_BLK=virtio BENCH_REPS=5
```

The target builds the required ISO and root image, then invokes `tests/boot/run-fsbench.sh`. The harness accepts `virtio`, `ahci`, and `nvme`, boots QEMU with a copy of the root image, and prints the collected serial reports. It uses `BENCH-DONE` to determine completion.

## Checking and repairing LogitFS

The checker lives in `c/fs/logitfs/fsck.c`. Its interface in `c/fs/logitfs/fsck.h` uses `struct fsck_dev` to supply block read, write, and synchronization callbacks, a context pointer, and an optional device length. Kernel block I/O and host image access share the same checker.

`fsck_run()` accepts a repair flag and fills `struct fsck_report`.

| Area | Checks |
|---|---|
| Superblock | Format, region ordering, bounds, bitmap coverage, and inode capacity |
| Journal | Header and body checksums, target blocks, and pending records |
| Inodes | Types, block pointers, duplicate block claims, and sizes against mapped data |
| Directories | Entry alignment, dangling references, repeated inode references, and loops |
| Allocation | Bitmap entries against metadata and inode block claims |
| Version 5 identities | Volume identity, object IDs, revisions, and the allocation counter |

Repair actions include restoring bitmap bits, releasing leaked allocations, removing dangling or repeated directory references, freeing orphaned inodes, and truncating damaged block chains.

Repair begins with a preliminary inspection for duplicate block claims. A check returns zero when its problem count is zero; a repair returns zero when it reaches the end of the repair pass. `tests/unit/fs_fsck_test.c` follows repairs with another inspection and checks the contents of surviving files.

Journal recovery uses `fsck_log_recover()`. For a valid committed record, it installs the recorded blocks, invokes the synchronization callback when supplied, and clears the journal header. This ordering retains the recovery record until the installed blocks have crossed the callback’s synchronization boundary.

## Kernel checks and host snapshots

During mounting, `c/fs/logitfs/logitfs.c` validates geometry, processes journal recovery, loads filesystem metadata, and runs a consistency check. The final scan prints a `[fsck] mount check:` report.

`logitfs_fsck(int repair)` holds the filesystem lock and synchronizes cached writes before checking. After a repair request, it synchronizes and drops cached blocks, reloads the bitmap and inode table, and resets the allocation hint.

For host repacking, `tools/lfs_snapshot.c` opens the source image for reading and creates a private writable mapping. It recovers the journal in that mapping, checks the recovered filesystem, and creates a new snapshot file. Zero blocks become sparse holes, and the helper synchronizes the output before reporting success.

`tools/disk_profile.py` imports preserved state from this checked snapshot. Journal recovery affects the snapshot’s private view while the source image holds the original record.

## Building the root image

From the repository root:

```sh
make build/disk.img
```

`Makefile` supplies packaged inputs to `tools/mkfs.py`. Each specification identifies a host file or directory and its destination inside the guest filesystem. For example, `fsroot/etc` supplies the guest configuration directory, and `fsroot/docs/readme.txt` supplies the guest file `/docs/readme.txt`.

The builder emits LogitFS version 4 with this geometry:

| Property | Value |
|---|---|
| Block size | 4 KiB |
| Logical image size | 512 MiB |
| Inode count | 8,192 |
| Inode size | 128 bytes |
| Journal area | 64 blocks, including its header |
| File addressing | 12 direct pointers, single indirect, double indirect |

The C definitions are in `c/fs/logitfs/logitfs_fmt.h`; `tools/mkfs.py` contains their Python counterpart. `tests/unit/fs_format_test.c` reads a generated image through the C definitions and checks structure sizes, field offsets, image metadata, and consistency.

Directory imports sort file names and subdirectory names and retain empty directories. During directory imports, destination checks enforce a maximum path length of 255 bytes and component length of 59 bytes. Files ending in `.aex`, or installed beneath `/bin/`, `/sbin/`, `/usr/bin/`, or `/usr/sbin/`, receive stored mode `0755`.

New inode timestamps use the build time. `SOURCE_DATE_EPOCH` supplies a fixed timestamp when set. Preserved inodes receive their imported timestamps and ownership metadata.

The packer serializes the allocated prefix of the filesystem. `atomic_write()` in `tools/disk_profile.py` extends the output to the full logical size, and the free tail is sparse.

`Makefile` also imports `third_party/amd-firmware/amdgpu` into the guest directory `/lib/firmware/amdgpu`. It installs `third_party/amd-firmware/LICENSE.amdgpu` and `third_party/amd-firmware/MANIFEST` beneath the guest directory `/licenses/amd-firmware`. Individual firmware blobs are disk prerequisites, so changing a blob triggers repacking.

`make release-disk` builds the output selected by `RELEASE_DISK` in `Makefile`. It uses the shared package list with release exclusions and imports preserved state from the existing release image.

## Preserving guest state across rebuilds

The root and release image recipes use these preservation rules:

| Policy | Guest paths |
|---|---|
| Reserve existing subtrees as guest state | `/browser`, `/state`, `/home`, `/download` |
| Merge existing contents with packaged defaults | `/docs`, `/etc` |

For merged paths, existing file bytes and inode metadata take precedence, and packaged siblings are imported alongside them. `tools/disk_profile.py` imports timestamps, mode, owner, and group from the checked snapshot.

`tools/disk_guard.py` coordinates image use through a companion advisory lock and an open-file check using `lsof`. The packer holds the lock while building. It writes and synchronizes a temporary file, checks for open users again, renames the replacement over the destination, and synchronizes the containing directory.

## Boot image builders

Run these commands from the repository root:

| Command | Builder | Generated contents |
|---|---|---|
| `make build/logit.iso` | `tools/mkiso.py` | ISO9660 image with an El Torito BIOS entry, BIOS loader stages, and kernel payload |
| `make esp` | `tools/mkesp.py` | FAT image containing the UEFI loader and a kernel named `LOGIT.ELF` |

The ISO recipe is in `Makefile`. The EFI build rules and `esp` target are in `tests/uefi.mk`. These targets build their loader and kernel prerequisites.

`tools/mkiso.py` places the loader and kernel payloads and patches their locations into the BIOS stages. `tools/mkesp.py` uses mtools when available or its Python FAT16 writer. The default ESP layout places the FAT volume across the image.

The `--layout gpt` option wraps that volume in one EFI System Partition starting at LBA 2048, with a protective MBR and primary and backup GPT structures. Both FAT builders feed the same wrapper. `--size-mb` sets the FAT volume size; GPT adds 2,048 leading sectors and 33 trailing sectors.

The `--file SRC:ESP_PATH` option copies an additional file into the ESP. `--firmware-dir` packages sorted `.bin` files under generated short names and creates a firmware index containing their lengths and original names. The `make esp` recipe uses the default layout; firmware inclusion is available through `tools/mkesp.py --firmware-dir`.

The UEFI loader in `c/boot/efi/loader.c` reads indexed firmware before leaving boot services and passes the contents as boot modules. `c/kernel/init/bootmod.c` validates module tags and retains their memory ranges.

The Polaris firmware reader in `c/drivers/gpu/amd/polaris/resource/files.c` tries the root filesystem first, then boot modules. Both sources pass format checks and SHA-256 pin checks. `tools/gpu/amd/fwpin.py` generates the pins from `third_party/amd-firmware/MANIFEST`, connecting accepted digests to the packaged firmware record.

The default `all` target builds the ISO. The root-image target rebuilds applications packaged on the LogitFS disk.

## Windows root installation

`tools/windows/install-root.ps1` installs a root image from Windows using PowerShell 7 with Administrator rights. Its `-Image` argument selects the source image.

`tools/windows/RootPlan.psm1` plans a partition in an aligned free extent immediately after C: on its GPT disk and supplies the LogitFS partition type. The installer requests confirmation before creating the partition and again before writing. It rechecks disk inventory, writes the image into the new partition, and compares the source SHA-256 digest with a readback of the written range. It records an installation transcript on the Desktop.

## Relevant verification targets

These targets define checks available in the tree:

| Target | Coverage |
|---|---|
| `test-vfs-os` | Guest control commands, a second mounted image, permissions, links, and shared descriptor offsets |
| `test-fsck` | Corruption detection, repairs, refusal cases, and surviving file contents |
| `test-fs-format` | C layout checks and inspection of the generated root image |
| `test-fdstream` | Guest descriptor memory measurements and content comparisons |
| `test-disk-profile` | State preservation and image lifecycle refusals during rebuilding |
| `test-disk-sparse` | Logical image bytes, free-tail allocation, and preservation-reader memory use |
| `test-snapshot-sparse` | Snapshot bytes, journal recovery, source preservation, and sparse allocation |

Definitions are in `Makefile`, `tests/fdstream.mk`, and `tests/disk_profile.mk`.

## Limitations

- `/dev/vfsctl` parses up to five whitespace-delimited tokens, truncating each to 127 bytes. Its response holds up to 1,023 bytes, and a later command can replace it before readback. `/dev/fsbench` retains up to 4,095 report bytes; serial output carries the full emitted report.
- VFS hard-link and symbolic-link records reside in memory and expire across reboot. Mount points below the root must already exist as directories. See `c/fs/vfs/vfs_meta.c` and `c/fs/vfs/vfs.c`.
- Benchmark cold preparation covers the LogitFS buffer cache, and measurements proceed after a preparation error. The `launch` timing ends after reading the file and excludes executable loading and painting. The host harness judges completion without a performance threshold.
- `openmax` retains the historical phrase “largest openable file” in its output, but probes heap allocations. Read descriptors can stream through `vfs_pread()` in `c/kernel/exec/fd/file.c`. The `openfd` comparison allocates a buffer for the complete reference file.
- Repair refuses duplicate block claims before applying writes because choosing a claimant would decide which file loses data. Invalid geometry, an invalid root, and invalid version 5 identities also cause refusal. With repair disabled, pending journal records are reported while current home blocks are inspected. The final mount-time scan reports its findings, and mounting proceeds.
- Directory imports reject symbolic links and entries other than regular files or directories. Full destination path-length checks apply to directory imports and preserved trees; direct file specifications use `add_file()` without that full-path check. Preservation through the packer accepts version 4 images. Reserved-path overlaps and file/directory type conflicts abort rebuilding.
- Replacing an existing LogitFS image requires a successful `lsof` ownership check. The lifecycle guard and atomic replacement belong to LogitFS packing; the ISO and ESP builders write their destination files directly.
- ESP extra-file components must fit 8.3 names. The firmware packer accepts up to 99 nonempty `.bin` files with names of at most 48 characters from its allowed ASCII set. The loader accepts an index of up to 4 KiB and shares 32 module slots with other boot modules.
- The Windows installer requires exactly one usable free extent across its inventory, immediately after C:, with enough space for the image. C: must be fully decrypted and unlocked, with BitLocker protection off. The policy requires 512-byte logical and physical sectors and refuses dynamic disks, existing LogitFS partitions, and ambiguous selection.
- Source comments in the GPT ESP path and the Windows installer flag X79-VG4 hardware as awaiting verification.

## Source map

| File | What it contains |
|---|---|
| `c/fs/ctl/vfsctl.c` | Synthetic nodes, command parsing, dispatch, and responses |
| `c/fs/ctl/vfsctl.h` | Control protocol and interface rationale |
| `c/fs/ctl/fsbench.c` | Guest storage measurements and reports |
| `c/fs/vfs/vfs.c` | Control routing, permissions, mounts, and link operations |
| `c/fs/vfs/vfs_meta.c` | Metadata defaults and VFS link records |
| `c/fs/vfs/vfs_cred.c` | Credential changes and reporting |
| `c/fs/logitfs/fsck.c` | Geometry validation, journal recovery, checking, and repair |
| `c/fs/logitfs/fsck.h` | Checker callbacks, reports, and public interfaces |
| `c/fs/logitfs/logitfs.c` | Mount checks, mounted repair, and cache controls |
| `c/fs/logitfs/logitfs_fmt.h` | Disk structures, versions, and format constants |
| `c/kernel/exec/fd/file.c` | Descriptor opening and streaming reads |
| `c/boot/efi/loader.c` | ESP firmware reads and boot-module handoff |
| `c/kernel/init/bootmod.c` | Boot-module validation, memory retention, and lookup |
| `c/drivers/gpu/amd/polaris/resource/files.c` | Firmware loading, format checks, and digest checks |
| `include/abi/logit_boot.h` | Boot-module tag definitions and limits |
| `tools/mkfs.py` | LogitFS image construction and import rules |
| `tools/disk_profile.py` | State preservation and atomic image replacement |
| `tools/disk_guard.py` | Image locking and open-file checks |
| `tools/lfs_snapshot.c` | Journal recovery and checking in a private image mapping |
| `tools/mkiso.py` | BIOS ISO construction |
| `tools/mkesp.py` | FAT construction, GPT wrapping, extra files, and firmware indexes |
| `tools/gpu/amd/fwpin.py` | Firmware digest header generation |
| `third_party/amd-firmware/MANIFEST` | Firmware provenance and SHA-256 digests |
| `third_party/amd-firmware/LICENSE.amdgpu` | Packaged firmware licence |
| `tools/windows/install-root.ps1` | Windows partition creation, image writing, and readback verification |
| `tools/windows/RootPlan.psm1` | Disk inventory policy and partition planning |
| `Makefile` | Root and release packaging, BIOS image rules, and filesystem targets |
| `tests/uefi.mk` | UEFI loader and ESP image build rules |
| `tests/disk_profile.mk` | Preservation and sparse-image verification targets |
| `tests/fdstream.mk` | Descriptor measurement targets and controls |
| `tests/boot/run-fsbench.sh` | QEMU benchmark driver and serial collection |
| `tests/boot/run-vfs-test.sh` | Guest control-interface assertions |
| `tests/unit/fs_fsck_test.c` | Corruption, repair, refusal, and content checks |
| `tests/unit/fs_format_test.c` | Generated-image and C-format checks |

## See also

- [Virtual file system](vfs.md)
- [LogitFS](logitfs.md)
- [Block cache](block-cache.md)
- [Building LogitOS](../getting-started/building.md)
- [Running on real hardware](../getting-started/real-hardware.md)
- [Testing](../architecture/testing.md)
- [Make targets](../reference/make-targets.md)
