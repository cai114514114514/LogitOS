#ifndef LOGIT_LOGITFS_H
#define LOGIT_LOGITFS_H

#include <stdint.h>
#include "vfs.h"

/* LogitFS: a hierarchical, inode-based on-disk filesystem with a free-block
 * bitmap, subdirectories, and a write-ahead log (4 KiB blocks).
 *   block 0          superblock
 *   block 1..        block bitmap (1 bit/block)
 *   block ..         inode table (128B inodes: type, size, direct[12],
 *                    indirect, double_indirect, atime/mtime/ctime)
 *   block log_start  write-ahead log (1 commit record + body slots)
 *   block data_start file/dir data; directories hold { u32 ino; char name[60] }
 *
 * Metadata mutations commit through the log, so a crash mid-op replays or
 * discards the whole transaction at mount. The invariant that holds after a
 * crash at ANY point, and the argument for it, are written out in full above
 * log_commit() in logitfs.c -- that argument is the design, not the code.
 *
 * On-disk format and the record layout: c/fs/logitfs_fmt.h (mirrored by
 * tools/mkfs.py). Superblock version is still 4; timestamps went into the
 * inode's always-zeroed reserved bytes, which is compatible in both directions.
 *
 * Read-write at runtime; images are built on the host by tools/mkfs.py. */
extern struct filesystem logitfs;

/* Flush the buffer cache and issue a device barrier -- `sync(2)`.
 *
 * There is deliberately no per-file fsync here: a mutating VFS op is one
 * journal transaction and commits (three barriers) before it returns, so a
 * vfs_write that returned has ALREADY reached media. The kernel's SYS_FSYNC
 * flushes an open fd's buffered contents through vfs_write, which is where the
 * durability actually comes from. Returns 0 on success. */
int logitfs_sync(void);

/* Drop the mount, writing back anything the buffer cache still holds. A caller
 * simulating power loss calls bcache_drop() first, so that nothing volatile
 * reaches the device. */
void logitfs_unmount(void);

/* Check the mounted filesystem; repair != 0 also writes the fixes back.
 * Returns 0 if clean or fully repaired. See c/fs/fsck.h for what is checked and
 * for the rule about what is safely repairable. Must not be called from inside
 * a VFS operation (it assumes no transaction is open). */
int logitfs_fsck(int repair);

/* Replace the timestamp clock (default: the RTC). For tests that need
 * reproducible mtimes; pass NULL to restore the RTC. */
void logitfs_set_clock(int64_t (*now_unix)(void));

/* How many consecutive blocks a whole-file read may ask the device for in ONE
 * command (default 128 = 512 KiB; clamped to that). Setting it to 1 restores the
 * block-at-a-time read path this filesystem had before, which is what makes the
 * before/after of that change measurable in a single boot rather than across two
 * of them on a host whose speed moves. Nothing in the kernel changes it; it
 * exists for /dev/fsbench and for the tests. */
void     logitfs_set_read_run(uint32_t n);
uint32_t logitfs_read_run(void);

#endif /* LOGIT_LOGITFS_H */
