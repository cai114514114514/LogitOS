#ifndef AETHER_BLKDEV_H
#define AETHER_BLKDEV_H

#include <stdint.h>

/* Read/write `count` 512-byte sectors at `lba` (virtio-blk if present, else ATA). */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* AETHER_BLKDEV_H */
