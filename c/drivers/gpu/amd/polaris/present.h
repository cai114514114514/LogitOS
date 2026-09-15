#ifndef LOGIT_POLARIS_PRESENT_H
#define LOGIT_POLARIS_PRESENT_H
#include "amd/polaris/sdma/queue.h"

struct polaris_present {
    unsigned lock, active, quarantined;
    struct polaris_sdma_queue *queue;
    struct polaris_sdma_mapping staging, scanout;
    uint32_t width, height, pitch;
    uint64_t frames, pixels, uploaded_bytes;
};
/* The platform owns the scanout lease: no CPU cursor/AP/other GPU may write
 * the front buffer until each call completes. Init runs fill AND patterned
 * copy/readback in staging only. A configured queue alone is never active.
 * All descriptors and source buffers are stable/disjoint ordinary CPU objects;
 * their pointed-to device mappings remain owned for the context's lifetime. */
int polaris_present_init(struct polaris_present *, struct polaris_sdma_queue *,
                          const struct polaris_sdma_mapping *staging,
                          const struct polaris_sdma_mapping *scanout,
                          uint32_t width, uint32_t height, uint32_t pitch);
/* XRGB8888 linear surface, pitch and stride in bytes. Pixels starts at the
 * complete back buffer, not the dirty rectangle. bytes bounds all reads.
 * 0 completed; -1 invalid/pre-submit failure permits CPU fallback; -2 unknown
 * GPU completion forbids ALL front-buffer CPU writes and memory reuse. */
int polaris_present_rect(struct polaris_present *, const uint32_t *pixels,
                          uint64_t bytes, uint32_t stride,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
