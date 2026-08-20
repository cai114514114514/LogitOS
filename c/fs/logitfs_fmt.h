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

/* --- the geometry tools/mkfs.py builds, and NOTHING ELSE MAY READ IT --------
 *
 * These are not format. The driver must never use them: total_blocks and
 * inode_count are superblock FIELDS, and logitfs_mount() sizes every allocation
 * it makes from sb.* precisely so that an image built by a different mkfs -- an
 * older one, a bigger one, tests/unit/fsstub/fs_sim.h's -- mounts and is read
 * correctly. A compile-time total_blocks in the driver would silently misread
 * every image that disagreed with it, which is the worst outcome available
 * here, so the invariant is worth stating as a rule rather than a habit:
 *
 *     grep -r LFS_MKFS_ c/  must return this file and nothing else.
 *
 * What they are FOR is the two-definition-site problem this header opens with.
 * The offsets below have a Python mirror and are checked against a real image;
 * the image's SIZE had no mirror at all, so tools/mkfs.py could raise
 * INODE_COUNT past what fsck_super_valid() accepts and produce an image the
 * kernel refuses to mount, with nothing between the edit and a machine that
 * does not boot. tests/unit/fs_format_test.c now reads the real image and
 * asserts its geometry against these, so the drift is caught on the host.
 *
 * On 2026-08-20 they went 16384/256 -> 131072/8192 (64 MiB -> 512 MiB, 256 ->
 * 8192 inodes) because ONE FILE of a C toolchain -- cc1plus, 39,797,952 B
 * measured on the build host -- did not fit in the whole filesystem. The
 * arithmetic, the measurement it came from, and the two things it cost are in
 * tools/mkfs.py above these same constants.
 *
 * LFS_VERSION IS STILL 4, and this is the claim to be sure of before believing
 * anything else here: every changed quantity is already a superblock field, so
 * a pre-2026-08-20 image (total_blocks 16384, inode_count 256, bitmap_blocks 1,
 * inode_blocks 8, data_start 74) mounts on the new kernel unchanged and is read
 * with its own geometry. A bump would have been the wrong tool anyway -- it
 * cannot rescue an incompatible change and it costs a real thing, because
 * c/drivers/block/blkdev.c hard-codes LOGITFS_VERSION 4 in the root-selection
 * probe. Compatibility here is a property of where the numbers LIVE, not of a
 * version word, and it is proved by mounting an old image, not by asserting it.
 *
 * SO IT WAS MOUNTED. A copy of the last pre-growth build/disk.img was kept and
 * tests/boot/run-fsmount-test.sh was pointed at it with the new kernel: two real
 * boots, NO -snapshot, "[fsck] mount check: clean" on both, and the files the
 * kernel WROTE during boot 1 read back in boot 2. Reading an old image is the
 * weaker half of that and would have been the easy thing to check; writing to
 * one is where a wrong bitmap_blocks or inode_blocks would actually corrupt. */
#define LFS_MKFS_TOTAL_BLOCKS  131072u        /* 512 MiB at LFS_BS */
#define LFS_MKFS_INODE_COUNT   8192u
#define LFS_MKFS_LOG_BLOCKS    64u

/* Derived exactly as tools/mkfs.py's serialize() derives them, so the test can
 * compare a computation against the image instead of a hand-copied number: a
 * constant typed twice is the drift this file exists to prevent. */
#define LFS_MKFS_BITMAP_BLOCKS \
    ((LFS_MKFS_TOTAL_BLOCKS + 8u * LFS_BS - 1u) / (8u * LFS_BS))
#define LFS_MKFS_INODE_BLOCKS \
    ((LFS_MKFS_INODE_COUNT * LFS_INODE_SIZE + LFS_BS - 1u) / LFS_BS)
#define LFS_MKFS_DATA_START \
    (1u + LFS_MKFS_BITMAP_BLOCKS + LFS_MKFS_INODE_BLOCKS + LFS_MKFS_LOG_BLOCKS)

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
    /* Owner and mode, in the next slice of the same always-zeroed reserved area
     * and compatible in exactly the same way. `xmode` is ZERO on every image
     * mkfs has ever written and on every inode this filesystem allocated before
     * these fields existed, which is why it needs a presence bit of its own:
     * mode 0 is a legal mode (a file nobody may touch), so "0 means unset" would
     * make it unrepresentable. LFS_MODE_SET is that bit.
     *
     * A caller reading xmode == 0 has learned something real -- nobody has ever
     * set a mode on this file -- and must apply the default rather than report
     * 0000. That distinction is carried all the way out to ring 3 as
     * LSTA_MODE_STORED (include/abi/logit_abi.h): a stat that cannot tell a
     * chosen 0644 from a defaulted one is a stat that lies quietly. */
    uint32_t xmode;                           /* 0 = never set; else LFS_MODE_SET | 07777 */
    uint32_t uid;
    uint32_t gid;
    uint8_t  reserved[LFS_INODE_SIZE - 8 - LFS_NDIRECT * 4 - 8 - 24 - 12];
} __attribute__((packed));

#define LFS_MODE_SET   0x8000u                /* xmode presence bit */
#define LFS_MODE_BITS  07777u

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
