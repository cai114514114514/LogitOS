#include <stdint.h>
#include "virtio.h"
#include "kprintf.h"

/* virtio-blk: block read/write over the virtio transport. Synchronous (one
 * request at a time), which matches how logitfs uses the block layer. */

#define VIRTIO_BLK_T_IN    0  /* read  (device writes our buffer) */
#define VIRTIO_BLK_T_OUT   1  /* write (device reads our buffer)  */
#define VIRTIO_BLK_T_FLUSH 4  /* commit the device write cache to media */

/* Feature bit 9. A virtio-blk device without it has no writeback cache at all,
 * so a completed write is already on media and a flush would be meaningless --
 * which is why failing to negotiate it makes virtio_blk_flush() a correct no-op
 * rather than a silent failure. */
#define VIRTIO_BLK_F_FLUSH (1u << 9)

struct blk_req_hdr { uint32_t type; uint32_t reserved; uint64_t sector; } __attribute__((packed));

static struct virtio_dev blkdev;
static struct virtq       blkvq;
static int                blk_ready;

int virtio_blk_init(void)
{
    if (virtio_init(VIRTIO_DEV_BLK, &blkdev, VIRTIO_BLK_F_FLUSH) != 0) return -1;
    if (virtio_queue_setup(&blkdev, 0, &blkvq) != 0) return -1;
    virtio_driver_ok(&blkdev);
    blk_ready = 1;
    /* Whether this device has a write cache decides whether the filesystem's
     * ordering means anything. Say it out loud at boot: "cache=writeback" is the
     * device telling us a completed write is NOT yet on media, so every ordering
     * the journal depends on needs a blk_flush() to hold. */
    kprintf("[virtio-blk] ready, cache=%s\n",
            (blkdev.features_lo & VIRTIO_BLK_F_FLUSH) ? "writeback (barriers REQUIRED)" : "none");
    return 0;
}

int virtio_blk_present(void) { return blk_ready; }

/* virtio-blk device config, field 0: capacity in 512-byte sectors. Read as two
 * 32-bit MMIO loads because the config region is device memory and a 64-bit
 * load across it is not guaranteed to be a single transaction. */
uint64_t virtio_blk_capacity(void)
{
    if (!blk_ready || !blkdev.device) return 0;
    volatile uint32_t *cfg = (volatile uint32_t *)(void *)blkdev.device;
    return (uint64_t)cfg[0] | ((uint64_t)cfg[1] << 32);
}

static int blk_rw(int write, uint64_t lba, uint32_t count, void *buf)
{
    if (!blk_ready) return -1;
    static struct blk_req_hdr hdr;     /* static: identity-mapped, single outstanding */
    static volatile uint8_t status;
    hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr.reserved = 0;
    hdr.sector = lba;
    status = 0xFF;
    struct virtio_buf b[3] = {
        { (uint64_t)(uintptr_t)&hdr,    sizeof hdr,   0 },           /* device reads the header */
        { (uint64_t)(uintptr_t)buf,     count * 512u, !write },      /* read: device writes data */
        { (uint64_t)(uintptr_t)&status, 1,            1 },           /* device writes status */
    };
    int rc = virtio_request(&blkdev, &blkvq, 0, b, 3);
    if (rc < 0) return -1;
    /* Cross-check the DMA length: a read must return count*512 data bytes plus
     * the 1 status byte, so a short (or stale) completion can't pass as success. */
    if (!write && (uint64_t)rc < (uint64_t)count * 512 + 1) return -1;
    return status == 0 ? 0 : -1;
}

/* Tell the device to put its write cache on media, and wait for it to say it
 * has. This is what makes the filesystem's write ORDERING mean anything: a disk
 * may reorder writes inside its own cache, so a journal that commits its header
 * without a barrier can land the header on media while the blocks that header
 * vouches for are still in the cache. Power loss there leaves exactly the
 * inconsistency the journal exists to prevent -- an unbarriered journal can be
 * worse than none, because it asserts an order the hardware never promised. */
int virtio_blk_flush(void)
{
    if (!blk_ready) return -1;
    if (!(blkdev.features_lo & VIRTIO_BLK_F_FLUSH)) return 0;   /* no cache to flush */
    static struct blk_req_hdr hdr;     /* static: identity-mapped, single outstanding */
    static volatile uint8_t status;
    hdr.type = VIRTIO_BLK_T_FLUSH;
    hdr.reserved = 0;
    hdr.sector = 0;                    /* the spec requires 0 for FLUSH */
    status = 0xFF;
    struct virtio_buf b[2] = {         /* no data buffer: header in, status out */
        { (uint64_t)(uintptr_t)&hdr,    sizeof hdr, 0 },
        { (uint64_t)(uintptr_t)&status, 1,          1 },
    };
    if (virtio_request(&blkdev, &blkvq, 0, b, 2) < 0) return -1;
    return status == 0 ? 0 : -1;
}

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf)  { return blk_rw(0, lba, count, buf); }
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) { return blk_rw(1, lba, count, (void *)buf); }
