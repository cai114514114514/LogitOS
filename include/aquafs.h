#ifndef AQUA_AQUAFS_H
#define AQUA_AQUAFS_H

#include "vfs.h"

/* AquaFS: a tiny read-only on-disk filesystem.
 *   sector 0      superblock  { u32 magic="AQUA", u32 version, u32 num_files }
 *   sectors 1..2  directory   16 x { char name[48]; u32 start_lba; u32 size }
 *   sectors 3..   file data, laid out contiguously
 * Built on the host by tools/mkfs.py. */
extern struct filesystem aquafs;

#endif /* AQUA_AQUAFS_H */
