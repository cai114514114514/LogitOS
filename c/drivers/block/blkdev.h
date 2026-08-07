#ifndef LOGIT_BLKDEV_H
#define LOGIT_BLKDEV_H

#include <stdint.h>

/* The block layer: a registry of block devices, and the one device the
 * filesystem is mounted on.
 *
 * This used to be three functions that asked "is NVMe here? is virtio here?"
 * and called the winner, which was enough while there was exactly one disk and
 * it was always the whole raw device. It is not enough on hardware: a machine
 * has several controllers, a controller has several ports, a disk has a
 * partition table, and the root filesystem lives in ONE partition of ONE of
 * them. So a device now has a name, a capacity, a set of ops, and -- if it is a
 * partition -- a parent and an offset. logitfs is unchanged and still calls
 * blk_read/blk_write/blk_flush; those now go to whichever device was selected
 * as root, and a partition's offset is added underneath, which is precisely why
 * the filesystem does not have to know that partitions exist. */

#define BLK_SECTOR    512
#define BLK_MAX_DEV   24
#define BLK_NAME_MAX  16
#define BLK_LABEL_MAX 40

struct blkdev;

struct blk_ops {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *ctx);
};

struct blkdev {
    char     name[BLK_NAME_MAX];   /* "nvme0", "ahci0", "ahci0p1" */
    const struct blk_ops *ops;
    void    *ctx;                  /* driver cookie handed back to ops */
    uint64_t start;                /* first LBA of this device within the medium */
    uint64_t nsectors;             /* length in 512-byte sectors */
    struct blkdev *parent;         /* whole disk this partition sits in, else NULL */
    int      part_index;           /* 1-based partition number; 0 for a whole disk */
    int      scheme;               /* partition scheme found ON this device (PART_*) */
    uint8_t  type_mbr;             /* MBR partition type byte */
    char     label[BLK_LABEL_MAX]; /* GPT name / MBR type description, for the log */
};

/* Register a whole disk. `nsectors` is the device's own reported capacity: it
 * bounds every request, so it must come from the hardware, never from a
 * partition table. Returns the device, or NULL if the table is full. */
struct blkdev *blk_register(const char *name, const struct blk_ops *ops,
                            void *ctx, uint64_t nsectors);

int             blk_count(void);
struct blkdev  *blk_at(int i);
struct blkdev  *blk_find(const char *name);

/* Read/write/flush a SPECIFIC device. Bounds-checked against that device's
 * length, so a partition cannot be walked off the end of into its neighbour --
 * which is the containment a raw offset alone does not give you. */
int blk_dev_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
int blk_dev_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
int blk_dev_flush(struct blkdev *d);

/* The device the filesystem is mounted on. */
struct blkdev *blk_root(void);
void           blk_set_root(struct blkdev *d);
const char    *blk_root_name(void);

/* Bring up the storage stack: register whatever the individual drivers found,
 * probe AHCI, read every disk's partition table, publish the partitions as
 * devices, pick the root, and print all of it.
 *
 * Idempotent, and called automatically by the first blk_read/blk_write/
 * blk_flush if nobody called it first. That is not laziness for its own sake:
 * the kernel has no driver-init ordering mechanism, so the alternative is a
 * line in kmain.c -- and a block layer that only works when a file outside it
 * remembers to call it is a block layer that silently has no disk the day
 * somebody reorders that file. The first thing the filesystem does at mount is
 * read sector 0, so the lazy path fires at exactly the right moment. Calling it
 * explicitly from kmain is still worth doing: it puts the enumeration log where
 * a reader expects it, next to the other driver bring-up. */
void blk_init(void);

/* --- what logitfs uses; unchanged signatures, now routed through the root --- */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);
/* Return only once previously written data is on media, not merely accepted by
 * the device. A journal's ordering means nothing without this. */
int blk_flush(void);
unsigned long blk_flush_count(void);   /* barriers issued since boot (for tests) */

#endif /* LOGIT_BLKDEV_H */
