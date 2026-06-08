#include <stdint.h>
#include "blkdev.h"
#include "ata.h"
#include "virtio_blk.h"
#include "nvme.h"

/* Block layer: prefer NVMe (M24 bare-metal target), then virtio-blk (modern,
 * async, no PIO busy-poll), else the legacy ATA PIO driver. aquafs talks to this,
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
