/* SPDX-License-Identifier: MIT
 * Actual guest helper with deterministic stats/clock, not DMA emulation. */
#include <assert.h>
#include <stdio.h>
#include "wide_dma_snapshot_wait.h"
static struct dma_stats samples[2];
static unsigned sample, count;
static uint64_t now;
void dma_get_stats(struct dma_stats *out) { *out=samples[sample]; }
uint64_t timer_ms(void) { return now; }
void sched_poll_wait(void) { now++;if(sample+1<count)sample++; }
static void reset(void) {
    samples[0]=(struct dma_stats){0};samples[1]=(struct dma_stats){0};
    now=sample=0;count=1;
}
int main(void) {
    struct dma_stats out;
    /* Controls come first: a deadline cannot turn retained resources into a
     * passing snapshot, even if only one accounting dimension remains. */
    for(unsigned field=0;field<4;field++) {
        reset();
        if(field==0)samples[0].active_mappings=1;
        if(field==1)samples[0].pinned_pages=2;
        if(field==2)samples[0].direct_bytes=4096;
        if(field==3)samples[0].bounce_bytes=4096;
        assert(!wide_dma_idle_snapshot(&out,3));assert(now==3);
    }
    puts("WIDE_DMA_SNAPSHOT_CONTROLS_PASS retained mapping/pin/direct/bounce rejected");
    reset();assert(wide_dma_idle_snapshot(&out,3));assert(now==0);
    reset();count=2;samples[0].active_mappings=1;
    samples[0].pinned_pages=2;samples[0].direct_bytes=4096;
    assert(wide_dma_idle_snapshot(&out,3));assert(now==1);
    assert(!out.active_mappings&&!out.pinned_pages&&!out.direct_bytes);
    puts("WIDE_DMA_SNAPSHOT_PASS checks=13");
    return 0;
}
