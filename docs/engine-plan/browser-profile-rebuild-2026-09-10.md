# Browser profile loss during `make run` — 2026-09-10

The root disk recipe now preserves the entire `/browser` tree when an app
change requires repacking the disk. This includes Cookie snapshots,
localStorage snapshots, and future files or directories below that root. The
disk path stays the same, so the next ordinary rebuild migrates the profile
already present on that image. The packer and `make run` refuse a disk that is
already open, including a QEMU launched before the new lifecycle lock existed.

## Attribution

The previous Cookie acceptance proved two QEMU boots using the same disk. It
did **not** test the intervening `make run` dependency rebuild. Keep that older
result beside this correction: successful Cookie persistence cannot survive a
build tool that replaces the entire filesystem with a fresh one.

`$(DISK)` depends on packaged AEX programs, and `run` depends on `$(DISK)`.
Previously `tools/mkfs.py` created a fresh `Builder()` and opened the existing
disk using `wb`, without importing browser state. Editing an app then running
`make run` could therefore erase both Cookie and localStorage files. Repacking
while QEMU held the image also risked overwriting a mounted filesystem.

Read-only inode metadata from the reported default disk showed four unrelated
packaged files (`/browser.aex`, `/bin/login`, `/fonts/ui.ttf`, and
`/licenses/README.txt`) sharing the mkfs-generated timestamp
`2026-09-10T13:52:57Z` (21:52:57 local), later than the user's successful-login
capture at 21:39. This is evidence of system-image repacking, beyond the host
disk mtime, which guest writes also change. The captured process arguments and
open-file check tied the default QEMU to `build/disk.img`.

Evidence: `build-cookie-persistence-fix/rebuild-audit/live-metadata.json`.
Only inode/directory metadata was inspected; no Cookie, token, password,
storage value, phone number, or conversation body was inspected or logged.
This was a live metadata read, not an atomic filesystem snapshot. Guest inode
timestamps reflect the existing localtime RTC setup and are not used as UTC
evidence for the system pack time.

## Implementation

- `tools/disk_guard.py` holds an advisory lifecycle lock shared by the packer
  and `make run`. Host `lsof` catches already-open images from legacy/direct
  launchers. Missing ownership tooling, symlink destinations, non-regular
  destinations, and occupied disks fail closed without stopping any process.
- `tools/lfs_snapshot.c` opens the input read-only and uses a private mapping.
  It runs the existing kernel filesystem journal recovery and checker from
  `c/fs/fsck.c`, then writes a new private checked image. Corruption rejected by
  the checker aborts migration; the source is never repaired in place.
- `tools/disk_profile.py` copies the selected subtree into the new Builder,
  preserving exact opaque file bytes, empty files/directories, owner, mode and
  inode times. Direct, indirect and double-indirect files are supported. Cycles,
  invalid paths, and conflicts with packaged paths are refused.
- `tools/mkfs.py` now stages a mode-0600 complete file, flushes it, checks disk
  ownership again, atomically replaces the destination and flushes its parent
  directory. Errors before replacement retain the original image. A directory
  flush error after replacement reports failure with a complete new image
  already installed; it cannot promise that the old filename is still present.
- `Makefile` passes `--preserve /browser` and wraps ordinary `run` in the same
  guard. `tests/disk_profile.mk` builds the native helper and wires the positive
  host gate to its negative control and `ci-host`.

## Verification

`make BUILD=build-cookie-persistence-fix test-disk-profile test-mk-wired`
passed. The 15 host checks exercise real mkfs code and the native checker on
independent synthetic images. They cover opaque state including files crossing
single/double indirect boundaries, program replacement, private metadata,
empty nodes, open-file and shared-lock refusal, failed input/write preservation,
corruption refusal, committed journal replay, and invalid destinations.

The prerequisite negative control actually rebuilt without preservation and
failed on loss of the old browser state. Log:
`build-cookie-persistence-fix/rebuild-audit/final-host-gate.log`.

Real browser acceptance used a private copy of the frozen `snapshot-referrer`
image and only a local fixture server:

1. First QEMU boot set network HttpOnly/persistent and script persistent/session
   Cookies, and localStorage. All page-visible checks succeeded.
2. Only the test QEMU exited. The production packer genuinely rebuilt the same
   test disk from the frozen system files with an added package marker while
   retaining `/browser`. Native recovery replayed five committed journal
   blocks; seven profile inodes were retained. Before/after hashes differ.
3. Second QEMU boot fetched a read-only fixture which cannot seed the expected
   values. Persistent Cookies and localStorage were present; the old session
   Cookie was absent; HttpOnly remained hidden from script.

Results: `build-cookie-persistence-fix/disk-profile-guest/results.json`;
repack log: `build-cookie-persistence-fix/disk-profile-guest/rebuild.log`.
The reusable guest gate is `test-disk-profile-os`. Its driver refuses to
overwrite previous acceptance evidence.

## Boundaries

No default live disk was written, replaced, or restarted during this work.
This prevents future browser-profile loss on ordinary builds; it cannot
recover state already erased by an earlier mkfs run. It does not establish
which DS credential mechanism the service uses, change session-cookie
lifetime, or prove that a server will keep an account session valid.

This is a reserved browser-state migration, not a general operating-system
updater: new user-state roots outside `/browser` require an explicit preserve
rule. An explicit clean/removal of the image still removes its data. The
cooperative lock closes the normal make-build/make-run race; a direct external
launcher that bypasses it can still race after the final `lsof` check. Such
launchers should use `disk_guard.py` too. The guard is conservative and may
refuse read-only observers because it does not infer their intent.
