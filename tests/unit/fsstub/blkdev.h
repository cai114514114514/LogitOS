#ifndef LOGIT_BLKDEV_H
#define LOGIT_BLKDEV_H

/* Host stub for the block layer: only the four entry points LogitFS and the
 * buffer cache use. The implementation is the simulated disk in fs_sim.h --
 * a device with a VOLATILE WRITE CACHE, which is the only device model on which
 * a durability test proves anything.
 *
 * A stub that wrote straight to an array would make every barrier a no-op and
 * every ordering bug invisible, which is exactly the shape of the test that
 * lets an unbarriered journal pass. */

#include <stdint.h>

#define BLK_SECTOR 512

int  blk_read(uint32_t lba, uint8_t count, void *buf);
/* The wide read the buffer cache's run path uses. Present here so the host
 * tests drive the SAME coalescing code the machine does -- a run reader that
 * only ever ran on device is a run reader whose off-by-one nobody checked. */
int  blk_read_n(uint64_t lba, uint32_t count, void *buf);
int  blk_write(uint32_t lba, uint8_t count, const void *buf);
int  blk_flush(void);
unsigned long blk_flush_count(void);

#endif
