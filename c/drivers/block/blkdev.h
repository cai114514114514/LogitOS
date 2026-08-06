#ifndef LOGIT_BLKDEV_H
#define LOGIT_BLKDEV_H

#include <stdint.h>

/* Read/write `count` 512-byte sectors at `lba` (virtio-blk if present, else ATA). */
int blk_read(uint32_t lba, uint8_t count, void *buf);
int blk_write(uint32_t lba, uint8_t count, const void *buf);
/* Return only once previously written data is on media, not merely accepted by
 * the device. A journal's ordering means nothing without this. */
int blk_flush(void);
unsigned long blk_flush_count(void);   /* barriers issued since boot (for tests) */

#endif /* LOGIT_BLKDEV_H */
