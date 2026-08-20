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

/* ==========================================================================
 * ASYNCHRONY: a request that outlives the call that made it
 *
 * Until 2026-08-20 `struct blk_ops` was read/write/flush, all three
 * synchronous, and c/kernel/mm/swap.c named the consequence as an open work
 * order in its own words: a page fault that needs the disk holds the big
 * kernel lock for the whole length of a device transfer, because there is no
 * way to give the CPU back in the middle of one. swap.c even wrote the
 * interface it wanted (see the comment above swap_io); this is that interface,
 * with the shape adjusted for two things swap.c could not see from where it
 * sat -- chunking, and the DMA bounce buffer.
 *
 * THE MODEL. A `struct blk_req` is owned by its submitter and lives until it
 * reports done. blk_submit() hands it to the driver; blk_poll() asks whether
 * it has finished. A driver may need SEVERAL device commands to satisfy one
 * request (AHCI's PRDT describes 32 MiB at a time, NVMe's MDTS less), so the
 * cursor fields below live in the request rather than in the driver: a chunked
 * transfer is now several commands with returns to the caller in between, and
 * driver-static state would be wrong the moment two media were in flight.
 *
 * WHO MAY DRIVE A REQUEST. Anyone holding the BKL, not only its submitter, and
 * that is what makes the design deadlock-free. The alternative -- "only the
 * submitter polls" -- means a second thread that wants the same medium waits
 * for the submitter to be scheduled, while the submitter is waiting for the
 * BKL the second thread holds. So blk_submit() on a medium that already has a
 * request in flight DRIVES THAT ONE TO COMPLETION FIRST and then proceeds.
 * Nobody ever waits for another thread to run in order for a device request to
 * finish.
 *
 * ONE IN FLIGHT PER MEDIUM. `inflight` is kept on the whole disk, never on the
 * partition, because a partition shares its parent's ops and ctx -- two
 * partitions of one disk are one queue. This interlock is what replaces what
 * non-preemptibility used to provide: ahci.c held g_ata_busy across its poll
 * because "a context switch here abandons a controller with CI still set and a
 * PRDT pointing at a buffer the next thread may reuse", and an explicit
 * in-flight record answers that without having to forbid the switch.
 * ======================================================================== */

#define BLK_OP_READ   0
#define BLK_OP_WRITE  1
#define BLK_OP_FLUSH  2

#define BLK_REQ_IDLE     0
#define BLK_REQ_INFLIGHT 1
#define BLK_REQ_DONE     2

/* blk_submit() refusals. Distinct values because a caller that gets -1 for
 * everything cannot tell "your arguments are wrong" (nothing to do about it)
 * from "this buffer cannot be DMA'd from an ASYNCHRONOUS request" (there is a
 * correct fallback, and c/kernel/mm/swap.c takes it and counts it). */
#define BLK_E_ARG    (-1)   /* no device, no ops, zero count, out of bounds */
#define BLK_E_NODMA  (-2)   /* async + a buffer the device cannot reach -- see blk_submit */

struct blk_req {
    /* --- the request: filled by blk_req_init(), read-only afterwards --- */
    struct blkdev *dev;
    uint64_t lba;            /* relative to `dev`; dev->start is added on submit */
    uint32_t count;          /* sectors (0 for BLK_OP_FLUSH) */
    void    *buf;
    uint8_t  op;             /* BLK_OP_* */
    uint8_t  async;          /* 1: the submitter will give the CPU up between polls */

    /* --- state the block layer owns --- */
    uint8_t  state;          /* BLK_REQ_* */
    int      status;         /* 0 ok, <0 error. Meaningful only in BLK_REQ_DONE */

    /* --- state the DRIVER owns, from submit until the poll that reports done.
     * In the request rather than in the driver for the reason above: the
     * request is what survives the gap between calls, so the cursor lives in
     * it. --- */
    uint64_t dev_lba;        /* absolute LBA of the chunk now in flight */
    uint32_t done;           /* sectors already transferred */
    uint32_t chunk;          /* sectors in the command now in flight */
    uint64_t deadline;       /* timer_ms() by which that command must complete */
    uint32_t tag;            /* driver handle for the in-flight command */
    uint8_t  attempt;        /* retries already spent on this chunk */
};

struct blk_ops {
    /* THE SYNCHRONOUS FALLBACK. A transport that genuinely cannot be split
     * implements these: ATA PIO moves the data through the CPU with no way to
     * leave and come back, and virtio-blk's request is one descriptor chain
     * whose completion this tree reads inside the same call. For those the
     * block layer synthesises submit/poll -- the request IS complete when
     * submit returns, which is the truth about that transport and not a stub:
     * nothing is reported finished that has not finished. */
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *ctx);

    /* THE INTERFACE. A driver that can hand back an in-flight token implements
     * these two and leaves read/write/flush NULL -- one implementation per
     * driver, never two, because two is how a synchronous path and an
     * asynchronous path come to disagree about ordering. submit() returns 0
     * when the request was accepted and <0 when it was refused before anything
     * was issued; poll() returns 1 when the request has finished (status is
     * then valid) and 0 while it is still running. Neither may block for the
     * length of a transfer. */
    int (*submit)(void *ctx, struct blk_req *r);
    int (*poll)(void *ctx, struct blk_req *r);
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
    struct blk_req *inflight;      /* whole disks only: the request this medium is
                                    * running, NULL when idle. See above. */
};

/* Fill a request. Bounds are NOT checked here -- blk_submit does that once, at
 * the point where a refusal can be reported. `count` is ignored for
 * BLK_OP_FLUSH. */
void blk_req_init(struct blk_req *r, struct blkdev *d, int op,
                  uint64_t lba, uint32_t count, void *buf);

/* Hand the request to the device. 0 = accepted (it may already be complete --
 * ask blk_poll, never assume), BLK_E_* = refused. On a refusal the request is
 * left in BLK_REQ_DONE with `status` set, so a caller that only looks at
 * blk_poll()/status still sees the failure rather than waiting forever.
 *
 * ASYNC AND THE BOUNCE, stated where a caller will read it: an async request
 * whose buffer is outside the identity-mapped region is REFUSED with
 * BLK_E_NODMA rather than bounced. blkdev.c has ONE static bounce buffer and
 * it is safe only while there is never a second transfer in flight -- the
 * exact assumption this file has just removed. Refusing is not a limitation
 * dressed up as a rule: the caller's correct response is to issue the same
 * request synchronously, which bounces exactly as it always did. */
int  blk_submit(struct blk_req *r);

/* 1 = complete (status valid), 0 = still running. Safe on a request that is
 * already complete, and safe from any thread holding the BKL. */
int  blk_poll(struct blk_req *r);

/* The synchronous convenience, and the whole of what blk_dev_read/write/flush
 * are: submit, then poll to completion. It runs with interrupts on (QEMU's IO
 * thread produces the completion and only runs when this vCPU yields) and with
 * the block layer's no-preemption flag raised -- exactly the discipline each
 * driver used to implement for itself inside its own poll loop. Returns the
 * request's status. */
int  blk_wait(struct blk_req *r);

/* Async submissions that had to be refused for BLK_E_NODMA. Zero on this
 * machine, and printed rather than assumed, because "it is always zero" is the
 * kind of claim that stops being true silently. */
unsigned long blk_async_refusals(void);

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
 * which is the containment a raw offset alone does not give you.
 *
 * These are blk_req_init + blk_submit + blk_wait and nothing else, which is why
 * logitfs, bcache, lfsro, fsbench and the partition scanner are untouched by
 * the split above: their path IS the async path, driven to completion. */
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

/* The same read, without the 255-sector ceiling `uint8_t count` imposes.
 *
 * That ceiling was not a device limit, it was a type. Measured on this machine
 * (make bench-fs): a virtio-blk read costs ~81 us for 8 sectors and ~120 us for
 * 255 -- so the cost is a FIXED per-command charge of roughly 80 us plus a
 * marginal rate under 0.3 us per sector. Reading a 3 MB app as 741 separate
 * 8-sector commands therefore pays that fixed charge 741 times and spends 51 ms
 * doing it, which is the whole of what "launching the Browser feels slow"
 * consisted of. One command per 512 KiB pays it 6 times.
 *
 * `count` is bounded only by the device and by what the caller's buffer can
 * hold; adapters for drivers whose own interface is narrower (NVMe, legacy ATA
 * PIO) chunk underneath, so a caller never has to know which driver it got. */
int blk_read_n(uint64_t lba, uint32_t count, void *buf);
/* Return only once previously written data is on media, not merely accepted by
 * the device. A journal's ordering means nothing without this. */
int blk_flush(void);
unsigned long blk_flush_count(void);   /* barriers issued since boot (for tests) */

#endif /* LOGIT_BLKDEV_H */
