/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_WIDE_DMA_SNAPSHOT_WAIT_H
#define LOGIT_WIDE_DMA_SNAPSHOT_WAIT_H
#include "dma.h"
uint64_t timer_ms(void);
void sched_poll_wait(void);

/* The original test compared instantaneous global counts around one disk
 * request. A concurrent 4 KiB filesystem read produced mappings=0/1 and
 * pins=0/2 after a correct 512 KiB transfer. Wait for all transient mappings
 * to drain, with a deadline: a retained mapping/pin/bounce still fails. This
 * does not stop devices or release any resource on the test's behalf. */
static int wide_dma_idle_snapshot(struct dma_stats *snapshot, uint64_t deadline)
{
    for (;;) {
        dma_get_stats(snapshot);
        if (!snapshot->active_mappings && !snapshot->pinned_pages &&
            !snapshot->direct_bytes && !snapshot->bounce_bytes)
            return 1;
        if (timer_ms() >= deadline) return 0;
        sched_poll_wait();
    }
}
#endif
