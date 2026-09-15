#include <stdint.h>
#include "virtio.h"
/* Staging belongs to this device operation until completion, not just until
 * virtio_request publishes descriptors. Independent devices remain concurrent. */
static io_lock_t blk_gate = IO_LOCK_INIT;
#include "kprintf.h"
#include "panic.h"
#include "driver.h"
#include "blkdev.h"

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
static struct dma_buffer *blk_control;
/* Keep a handle reachable even if reset fails. In that case a direct caller
 * cannot regain its CPU buffer safely, so blk_rw fail-stops rather than return. */
static struct dma_mapping *blk_payload;
#define BLK_STATUS_OFFSET 64

static void blk_release(void)
{
    blk_ready = 0;
    if (virtio_stop(&blkdev)) return;
    if (blk_payload) { dma_unmap(blk_payload); blk_payload = NULL; }
    dma_free_coherent(blk_control); blk_control = NULL;
    virtio_queue_release(&blkdev, &blkvq);
}

int virtio_blk_init(void)
{
    if (virtio_init(VIRTIO_DEV_BLK, &blkdev, VIRTIO_BLK_F_FLUSH) != 0) return -1;
    if (virtio_queue_setup(&blkdev, 0, &blkvq) != 0) { blk_release(); return -1; }
    blk_control = dma_alloc_coherent(&blkdev.dma, 4096, 4096, 0);
    if (!blk_control) { blk_release(); return -1; }
    virtio_driver_ok(&blkdev);
    blk_ready = 1;
    dma_report("virtio-blk");
    /* Whether this device has a write cache decides whether the filesystem's
     * ordering means anything. Say it out loud at boot: "cache=writeback" is the
     * device telling us a completed write is NOT yet on media, so every ordering
     * the journal depends on needs a blk_flush() to hold. */
    kprintf("[virtio-blk] ready, cache=%s\n",
            (blkdev.features_lo & VIRTIO_BLK_F_FLUSH) ? "writeback (barriers REQUIRED)" : "none");
    return 0;
}

int virtio_blk_present(void) { return blk_ready && !blkdev.failed; }

/* virtio-blk device config, field 0: capacity in 512-byte sectors. Read as two
 * 32-bit MMIO loads because the config region is device memory and a 64-bit
 * load across it is not guaranteed to be a single transaction. */
uint64_t virtio_blk_capacity(void)
{
    if (!blk_ready || blkdev.failed || !blkdev.device) return 0;
    volatile uint32_t *cfg = (volatile uint32_t *)(void *)blkdev.device;
    return (uint64_t)cfg[0] | ((uint64_t)cfg[1] << 32);
}

static int blk_rw_one(int write, uint64_t lba, uint32_t count, void *buf)
{
    size_t bytes = (size_t)count * 512;
    /* Previously static identity-mapped hdr/status and a raw CPU-pointer DMA
     * descriptor. Control is now coherent; payload mapping handles high aliases
     * and bounces only when this one-descriptor request is not contiguous. */
    struct blk_req_hdr *hdr = blk_control->cpu;
    volatile uint8_t *status = (uint8_t *)blk_control->cpu + BLK_STATUS_OFFSET;
    hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0; hdr->sector = lba; *status = 0xFF;
    blk_payload = dma_map_kernel(&blkdev.dma, buf, bytes,
                                write ? DMA_TO_DEVICE : DMA_FROM_DEVICE,
                                DMA_MAP_CONTIGUOUS);
    if (!blk_payload) return -1;
    uint64_t token = dma_mapping_submit(blk_payload);
    if (!token) { dma_unmap(blk_payload); blk_payload = NULL; return -1; }
    struct virtio_buf b[3] = {
        { blk_control->dma, sizeof *hdr, 0, blk_control },
        { dma_mapping_addr(blk_payload, 0), (uint32_t)bytes, !write, NULL },
        { dma_addr_add(blk_control->dma, BLK_STATUS_OFFSET), 1, 1, NULL },
    };
    int rc = virtio_request(&blkdev, &blkvq, 0, b, 3);
    if (rc < 0 && !blkdev.quiesced) {
        /* Pins prevent freeing frames, not the caller reusing an existing stack
         * or cache buffer. Returning here would permit late DMA corruption. */
        if (!blkdev.failed) virtio_stop(&blkdev);
        if (!blkdev.quiesced) panic("virtio-blk: DMA reset failed; payload still device-owned");
    }
    int ok = rc >= 0 && *status == 0 && (write || (uint64_t)rc >= bytes + 1);
    if (rc >= 0) {
        /* TO_DEVICE never copies back; nevertheless its successfully transferred
         * bytes count toward direct/high DMA completion just like a read. */
        size_t valid = *status == 0 ? (write ? bytes : (rc > 0 ? (size_t)rc - 1 : 0)) : 0;
        if (valid > bytes) valid = bytes;
        if (dma_mapping_complete(blk_payload, token, valid)) ok = 0;
    }
    if (dma_unmap(blk_payload)) panic("virtio-blk: attempted to release owned DMA payload");
    blk_payload = NULL;
    return ok ? 0 : -1;
}

static int blk_rw(int write, uint64_t lba, uint32_t count, void *buf)
{
    IO_GUARD(&blk_gate);
    if (!blk_ready || blkdev.failed || !buf || !count || lba > UINT64_MAX - count) return -1;
    /* Bound per-request DMA metadata/bounce memory without growing kernel heap.
     * The block layer must not pre-bounce this driver: it owns this mapping. */
    while (count) {
        uint32_t n = count;
        if (n > DMA_MAX_MAPPING / 512) n = DMA_MAX_MAPPING / 512;
        if (blk_rw_one(write, lba, n, buf)) return -1;
        buf = (uint8_t *)buf + (size_t)n * 512;
        lba += n; count -= n;
    }
    return 0;
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
    IO_GUARD(&blk_gate);
    if (!blk_ready || blkdev.failed) return -1;
    if (!(blkdev.features_lo & VIRTIO_BLK_F_FLUSH)) return 0;
    struct blk_req_hdr *hdr = blk_control->cpu;
    volatile uint8_t *status = (uint8_t *)blk_control->cpu + BLK_STATUS_OFFSET;
    hdr->type = VIRTIO_BLK_T_FLUSH; hdr->reserved = 0; hdr->sector = 0;
    *status = 0xFF;
    struct virtio_buf b[2] = {
        { blk_control->dma, sizeof *hdr, 0, blk_control },
        { dma_addr_add(blk_control->dma, BLK_STATUS_OFFSET), 1, 1, NULL },
    };
    if (virtio_request(&blkdev, &blkvq, 0, b, 2) < 0) return -1;
    return *status == 0 ? 0 : -1;
}

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf)  { return blk_rw(0, lba, count, buf); }
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf) { return blk_rw(1, lba, count, (void *)buf); }

/* Early root-disk initialization precedes generic binding. Bind the existing
 * transport so dev_unbind reaches the block-layer admission fence first. */
static int blk_probe_existing(struct device *dev)
{
    return dev == blkdev.dev && blk_ready && !blkdev.failed ? 0 : -1;
}
static void blk_remove(struct device *dev)
{
    if (dev != blkdev.dev) return;
    blk_dev_offline(blk_find("vblk0"));  /* reject/drain callers before reset */
    blk_release();
}
static const struct dev_match blk_ids[] = {
    DEV_MATCH_VD(VIRTIO_VENDOR, VIRTIO_DEV_BLK), DEV_MATCH_END
};
static struct driver blk_driver = {
    .name = "virtio-blk", .bus_type = DEV_BUS_PCI, .match = blk_ids,
    .probe = blk_probe_existing, .remove = blk_remove,
};
DRIVER_DECLARE(blk_driver);
