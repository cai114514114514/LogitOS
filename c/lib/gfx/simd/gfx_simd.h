/* S0's clock and glass counters only. Pixel kernels and ISA selection belong
 * to S2; having this header does not claim SIMD acceleration or hardware proof. */
#ifndef LOGIT_GFX_SIMD_H
#define LOGIT_GFX_SIMD_H

#include <stdint.h>

struct gfx_glass_prof {
    /* px counts visible panel bounding boxes, excluding the sampling halo;
     * rounded coverage and the write clip do not change that unit. */
    uint64_t calls, px, blur_ns, render_ns, memo_hit, memo_miss, memo_bypass;
};

/* Install the caller's monotonic ns clock; NULL disables timing, not drawing. */
void gfx_set_clock(uint64_t (*ns)(void));
const struct gfx_glass_prof *gfx_glass_prof(void);

/* Material instrumentation uses these without depending on a kernel timer.
 * Live profile readers must load each field atomically; it is not a snapshot. */
uint64_t gfx_now_ns(void);
void gfx_glass_record(uint64_t px, uint64_t blur_ns, uint64_t render_ns);

#endif
