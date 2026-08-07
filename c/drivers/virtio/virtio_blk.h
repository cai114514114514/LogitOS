#ifndef LOGIT_VIRTIO_BLK_H
#define LOGIT_VIRTIO_BLK_H

#include <stdint.h>

int virtio_blk_init(void);                 /* 0 if a virtio-blk device is present + ready */
int virtio_blk_present(void);
int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);
int virtio_blk_flush(void);                /* commit the device write cache to media */
/* Device capacity in 512-byte sectors (0 if absent). Read from the virtio-blk
 * device config, so the block layer can bound requests and the partition
 * scanner can reject a table that claims sectors the device does not have. */
uint64_t virtio_blk_capacity(void);

#endif /* LOGIT_VIRTIO_BLK_H */
