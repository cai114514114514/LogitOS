#include <stdint.h>
#include "virtio.h"
#include "kprintf.h"

/* virtio-blk: block read/write over the virtio transport. Synchronous (one
 * request at a time), which matches how logitfs uses the block layer. */

#define VIRTIO_BLK_T_IN   0   /* read  (device writes our buffer) */
#define VIRTIO_BLK_T_OUT  1   /* write (device reads our buffer)  */

struct blk_req_hdr { uint32_t type; uint32_t reserved; uint64_t sector; } __attribute__((packed));

static struct virtio_dev blkdev;
static struct virtq       blkvq;
static int                blk_ready;

int virtio_blk_init(void)
{
    if (virtio_init(VIRTIO_DEV_BLK, &blkdev, 0) != 0) return -1;
    if (virtio_queue_setup(&blkdev, 0, &blkvq) != 0) return -1;
    virtio_driver_ok(&blkdev);
    blk_ready = 1;
    return 0;
}

int virtio_blk_present(void) { return blk_ready; }

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

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf)  { return blk_rw(0, lba, count, buf); }
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) { return blk_rw(1, lba, count, (void *)buf); }
