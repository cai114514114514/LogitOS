#ifndef LOGIT_LOGITFS_H
#define LOGIT_LOGITFS_H

#include "vfs.h"

/* LogitFS v4: a hierarchical, inode-based on-disk filesystem with a free-block
 * bitmap, subdirectories, and a write-ahead log (4 KiB blocks).
 *   block 0          superblock
 *   block 1..        block bitmap (1 bit/block)
 *   block ..         inode table (128B inodes: type, size, direct[12], indirect, double_indirect)
 *   block log_start  write-ahead log (1 header + data slots)
 *   block data_start file/dir data; directories hold { u32 ino; char name[60] }
 * Metadata mutations commit through the log, so a crash mid-op replays or
 * discards the whole transaction at mount. Read-write at runtime; built on the
 * host by tools/mkfs.py. */
extern struct filesystem logitfs;

#endif /* LOGIT_LOGITFS_H */
