#ifndef LOGIT_LOGITFS_FMT_H
#define LOGIT_LOGITFS_FMT_H

#include <stdint.h>

/* The LogitFS on-disk format, in one place.
 *
 * These constants used to live only in c/fs/logitfs.c, with copies in
 * tools/mkfs.py and (for the magic) c/drivers/block/blkdev.c. fsck needs every
 * one of them and must read an image the driver has NOT mounted, so it cannot
 * borrow logitfs.c's in-RAM state -- which made a shared definition site the
 * only honest option. tools/mkfs.py still holds the Python mirror; the two are
 * checked against each other by tests/unit/fs_format_test.c, which reads a real
 * mkfs image and asserts every offset.
 *
 * --- version -----------------------------------------------------------------
 * The superblock version is still 4. Timestamps (atime/mtime/ctime) landed in
 * the inode's RESERVED area, which mkfs has always zero-filled, so:
 *   - a v4 image written before timestamps existed reads back with all three
 *     times = 0 (the epoch), which is exactly "unknown", not corruption;
 *   - an image written with timestamps is read correctly by code that predates
 *     them, because that code never looked at those bytes.
 * A format bump would therefore buy nothing and cost something real:
 * c/drivers/block/blkdev.c hard-codes LOGITFS_VERSION 4 in the root-selection
 * probe, and that file belongs to the storage line. An extension that is
 * compatible in both directions does not need a version number; one that is not
 * cannot be rescued by one. */

#define LFS_SECTOR      512
#define LFS_BS          4096                  /* block size */
#define LFS_SPB         (LFS_BS / LFS_SECTOR) /* 512B sectors per block (8) */
#define LFS_MAGIC       0x4C4F4749u           /* "LOGI" */
#define LFS_VERSION     4
#define LFS_INODE_SIZE  128
#define LFS_NDIRECT     12
#define LFS_PPB         (LFS_BS / 4)          /* u32 pointers per indirect block */
#define LFS_DIRENT_SZ   64
#define LFS_NAME_MAX    60
#define LFS_IPB         (LFS_BS / LFS_INODE_SIZE)   /* inodes per block (32) */

#define LFS_T_FREE      0
#define LFS_T_FILE      1
#define LFS_T_DIR       2

/* Largest byte count inode_write / imap can address: direct + single + double. */
#define LFS_MAX_FILE_SZ \
    ((uint64_t)(LFS_NDIRECT + LFS_PPB + (uint64_t)LFS_PPB * LFS_PPB) * LFS_BS)

/* --- the write-ahead log ---------------------------------------------------
 * Header block layout (block sb.log_start), all little-endian u32:
 *
 *   [0]                magic  LFS_LOG_MAGIC
 *   [1]                gen    monotonic transaction generation (informational)
 *   [2]                count  number of body blocks / targets, 1..log_max
 *   [3]                bcrc   CRC-32 of the `count` body blocks, in slot order
 *   [4 .. 4+count-1]   target block number for body slot i
 *   ...                zero
 *   [LFS_BS/4 - 1]     hcrc   CRC-32 of bytes [0, LFS_BS-4) of this block
 *
 * `hcrc` says the header itself is intact -- a torn header (the classic
 * "truncated final record") fails it and is discarded. `bcrc` says the bodies
 * belong to THIS header, which is the check that makes recovery safe against a
 * stale header sitting over a newer transaction's bodies; see the commit-record
 * argument in c/fs/logitfs.c. */
#define LFS_LOG_MAGIC   0x4C4F4735u           /* "LOG5" */
#define LFS_LOG_MAX     128                   /* cap on sb.log_blocks - 1 */
#define LFS_LOGH_MAGIC  0
#define LFS_LOGH_GEN    1
#define LFS_LOGH_COUNT  2
#define LFS_LOGH_BCRC   3
#define LFS_LOGH_TARGET 4                     /* first target word */
#define LFS_LOGH_HCRC   (LFS_BS / 4 - 1)

struct lfs_dinode {                           /* 128 bytes on disk */
    uint16_t type;
    uint16_t pad;
    uint32_t size;
    uint32_t direct[LFS_NDIRECT];
    uint32_t indirect;
    uint32_t double_indirect;
    /* Timestamps, whole seconds since the Unix epoch, from the RTC. Zero means
     * "never set" (a pre-timestamp image), which callers must not confuse with
     * 1970. Placed at the head of what used to be `reserved`, which mkfs has
     * always zeroed -- so this is a compatible extension, not a format bump. */
    int64_t  atime;                           /* last read */
    int64_t  mtime;                           /* last content change */
    int64_t  ctime;                           /* last inode change (incl. creation) */
    uint8_t  reserved[LFS_INODE_SIZE - 8 - LFS_NDIRECT * 4 - 8 - 24];
} __attribute__((packed));

struct lfs_dirent {                           /* 64 bytes on disk */
    uint32_t ino;
    char     name[LFS_NAME_MAX];
} __attribute__((packed));

/* The superblock, as 13 little-endian u32 at the start of block 0. */
struct lfs_super {
    uint32_t magic, version, block_size, total_blocks, inode_count;
    uint32_t bitmap_start, bitmap_blocks, inode_start, inode_blocks;
    uint32_t data_start, root_ino;
    uint32_t log_start, log_blocks;
};

#endif /* LOGIT_LOGITFS_FMT_H */
