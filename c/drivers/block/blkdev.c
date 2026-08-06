#include <stdint.h>
#include "blkdev.h"
#include "ata.h"
#include "virtio_blk.h"
#include "nvme.h"

/* Block layer: prefer NVMe (M24 bare-metal target), then virtio-blk (modern,
 * async, no PIO busy-poll), else the legacy ATA PIO driver. logitfs talks to this,
 * not the drivers directly. LBA + count are in 512-byte sectors. */

int blk_read(uint32_t lba, uint8_t count, void *buf)
{
    if (nvme_present())       return nvme_read(lba, count, buf);
    if (virtio_blk_present()) return virtio_blk_read(lba, count, buf);
    return ata_read(lba, count, buf);
}

int blk_write(uint32_t lba, uint8_t count, const void *buf)
{
    if (nvme_present())       return nvme_write(lba, count, buf);
    if (virtio_blk_present()) return virtio_blk_write(lba, count, buf);
    return ata_write(lba, count, buf);
}

/* Barrier: return only once everything already written is on media.
 *
 * A completed blk_write() means the DEVICE has the data, not that the platter
 * does -- both virtio-blk and NVMe may hold it in a write cache, and a disk is
 * free to reorder writes within that cache. So a journal that writes its blocks,
 * then its commit record, has ordered nothing unless a barrier separates them:
 * power loss can leave the commit record on media vouching for blocks that are
 * still in the cache and now gone. That is precisely the inconsistency the
 * journal exists to prevent, which is why an unbarriered journal can be worse
 * than none -- it asserts an order the hardware never promised.
 *
 * The counter is what makes that assertion testable from outside: a test can ask
 * whether the filesystem actually issued the barriers it claims to. */
static unsigned long flush_count;

int blk_flush(void)
{
    flush_count++;
    if (nvme_present())       return nvme_flush();
    if (virtio_blk_present()) return virtio_blk_flush();
    return ata_flush();
}

unsigned long blk_flush_count(void) { return flush_count; }
