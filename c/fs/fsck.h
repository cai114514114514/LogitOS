#ifndef LOGIT_FSCK_H
#define LOGIT_FSCK_H

#include <stdint.h>
#include "logitfs_fmt.h"

/* fsck: an offline consistency checker and repairer for LogitFS.
 *
 * A journal and a checker answer different questions, which is why having one
 * does not remove the need for the other. The journal guarantees that a crash
 * leaves the filesystem at a transaction boundary -- but only against the
 * failure it models, which is "the machine stopped". It says nothing about a
 * block the device returned wrong, a bug in the filesystem itself, an image
 * built by a broken mkfs, or a disk somebody edited. Those produce damage that
 * is perfectly transaction-atomic and still nonsense: a bitmap that disagrees
 * with the inodes, a dirent pointing at a free inode, a directory that is its
 * own ancestor. Only a full scan finds those.
 *
 * fsck talks to a device through a callback pair rather than blkdev, because
 * the point of an OFFLINE checker is that it runs on an image nobody has
 * mounted -- including on the build host, over a file, which is where the tests
 * drive it (tests/unit/fs_fsck_test.c).
 *
 * The repair rule is: fix only what has ONE correct answer, and report
 * everything else rather than guessing. A checker that guesses turns recoverable
 * damage into confident nonsense, which is worse than refusing. */

struct fsck_dev {
    int (*read)(void *ctx, uint32_t blk, void *buf);          /* LFS_BS bytes */
    int (*write)(void *ctx, uint32_t blk, const void *buf);   /* NULL => read-only */
    /* Barrier: return once everything written so far is on media. Recovery
     * needs exactly one, between installing the logged blocks and clearing the
     * header -- without it the clear can land first and a second crash loses
     * the transaction recovery had just restored, with the log no longer saying
     * how to restore it again. May be NULL only for a device that cannot
     * reorder (a host file the checker fsyncs itself). */
    int (*sync)(void *ctx);
    void *ctx;
    uint32_t nblocks;         /* device length in LFS_BS blocks, 0 = trust the superblock */
};

/* What was found, by class. `fixed_*` counts the subset that was repaired
 * (always 0 when repair == 0). */
struct fsck_report {
    int fatal;                 /* image not usable: superblock/geometry/root */
    int problems;              /* total problems found */
    int fixed;                 /* total problems repaired */

    int journal_replayed;      /* committed transactions installed */
    int journal_discarded;     /* torn/stale headers cleared */
    int bad_inode_type;        /* inode type not FREE/FILE/DIR */
    int bad_size;              /* size beyond what the block chain can hold */
    int bad_blockptr;          /* pointer outside the data area */
    int dup_block;             /* one block claimed by two inodes (NOT repairable) */
    int bitmap_missing;        /* referenced block marked free */
    int bitmap_leaked;         /* unreferenced block marked used */
    int dirent_bad_ino;        /* dirent -> out of range / free inode */
    int dirent_bad_size;       /* directory size not a whole number of dirents */
    int dir_loop;              /* directory reachable from itself */
    int multi_linked;          /* inode named by more than one dirent */
    int orphan_inode;          /* allocated inode no dirent names */
};

/* Report progress/problems; called once per finding. May be NULL.
 *
 * `msg` is a printf-style format containing at most two `%u`, filled from `a`
 * then `b` in order -- so a renderer is `kprintf(msg, a, b)` and a message with
 * fewer conversions than arguments is still well-defined. Keeping the arguments
 * numeric (rather than letting callers format their own strings) is what allows
 * this to run in the kernel, where there is no scratch buffer to format into. */
typedef void (*fsck_say)(void *ctx, const char *msg, uint32_t a, uint32_t b);

/* Run the checker. repair != 0 writes fixes back (requires dev->write).
 * Returns 0 if the image is clean or was fully repaired, -1 otherwise. */
int fsck_run(struct fsck_dev *dev, int repair, struct fsck_report *rep,
             fsck_say say, void *saycx);

/* Validate a superblock's geometry: every region inside the image, in mkfs
 * order, non-overlapping, and within the static caps the driver assumes.
 * Shared with logitfs_mount so the mounted and the checked view of "is this a
 * usable image" cannot drift apart. Returns 0 if usable. */
int fsck_super_valid(const struct lfs_super *sb);

/* Recover the write-ahead log in place: install a committed transaction, or
 * clear a torn/stale one. Returns the number of blocks installed, or -1.
 * Used by fsck and by logitfs_mount (through the same code, so a replay is a
 * replay wherever it happens). */
int fsck_log_recover(struct fsck_dev *dev, const struct lfs_super *sb, int *discarded);

/* CRC helpers over the log header, exported so mkfs-side tooling and the unit
 * tests seal a record exactly the way the filesystem does. */
uint32_t lfs_log_hdr_crc(const void *hdr_block);

#endif /* LOGIT_FSCK_H */
