#ifndef LOGIT_VIRTIO_BLK_H
#define LOGIT_VIRTIO_BLK_H

#include <stdint.h>

int virtio_blk_init(void);                 /* 0 if a virtio-blk device is present + ready */
int virtio_blk_present(void);
int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);
int virtio_blk_flush(void);                /* commit the device write cache to media */

#endif /* LOGIT_VIRTIO_BLK_H */
