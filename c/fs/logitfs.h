#ifndef LOGIT_LOGITFS_H
#define LOGIT_LOGITFS_H

#include "vfs.h"

/* LogitFS v3: a hierarchical, inode-based on-disk filesystem with a free-block
 * bitmap and subdirectories (4 KiB blocks).
 *   block 0          superblock
 *   block 1..        block bitmap (1 bit/block)
 *   block ..         inode table (128B inodes: type, size, direct[12], indirect)
 *   block data_start file/dir data; directories hold { u32 ino; char name[60] }
 * Read-write at runtime; built on the host by tools/mkfs.py. */
extern struct filesystem logitfs;

#endif /* LOGIT_LOGITFS_H */
