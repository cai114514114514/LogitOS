#ifndef LOGIT_LFSRO_TEST_BLKDEV_H
#define LOGIT_LFSRO_TEST_BLKDEV_H

#include <stdint.h>

struct blkdev { uint64_t nsectors; };
struct blkdev *blk_find(const char *name);
int blk_dev_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf);

#endif
