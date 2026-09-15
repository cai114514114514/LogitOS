#include "amd/polaris/present.h"

static int valid(const struct polaris_sdma_mapping *m)
{
    return m && m->cpu && m->range.bytes &&
        !(((uintptr_t)m->cpu | m->range.gpu_base | m->range.bytes) & 3u) &&
        m->range.gpu_base < POLARIS_SDMA_GPU_LIMIT &&
        m->range.bytes <= POLARIS_SDMA_GPU_LIMIT - m->range.gpu_base &&
        m->range.bytes <= UINTPTR_MAX - (uintptr_t)m->cpu;
}
static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{ return a < b + m && b < a + n; }
static int alias(const struct polaris_sdma_mapping *a,
                  const struct polaris_sdma_mapping *b)
{
    return overlap(a->range.gpu_base, a->range.bytes,
                   b->range.gpu_base, b->range.bytes) ||
           overlap((uintptr_t)a->cpu, a->range.bytes,
                   (uintptr_t)b->cpu, b->range.bytes);
}
static int failure(struct polaris_present *p, int result)
{
    if (p->queue->quarantined || result == POLARIS_SDMA_QUEUE_BUSY) {
        p->quarantined = 1;
        p->active = 0;
        return -2;
    }
    return -1;
}
int polaris_present_init(struct polaris_present *p, struct polaris_sdma_queue *q,
                          const struct polaris_sdma_mapping *staging,
                          const struct polaris_sdma_mapping *scanout,
                          uint32_t width, uint32_t height, uint32_t pitch)
{
    int rc = -1;
    volatile uint32_t *mapped = 0;
    if (!p) return -1;
    if (__atomic_exchange_n(&p->lock, 1u, __ATOMIC_ACQUIRE)) return -1;
    if (p->quarantined) { rc = -2; goto done; }
    if (p->active || !q || !q->attached || q->quarantined ||
        !valid(staging) || !valid(scanout) || staging->range.bytes < 8192 ||
        alias(staging, scanout) || alias(staging, &q->ring) ||
        alias(staging, &q->fence) || alias(scanout, &q->ring) ||
        alias(scanout, &q->fence) || !width || !height || width > UINT32_MAX / 4 ||
        (pitch & 3u) || pitch < width * 4 ||
        (uint64_t)pitch * height > scanout->range.bytes) goto done;
    if (q->ops.resolve_mapping(q->ops.opaque, scanout->range.gpu_base,
                              scanout->range.bytes, &mapped) ||
        mapped != scanout->cpu) goto done;
    p->queue = q;
    p->staging = *staging;
    p->scanout = *scanout;
    p->width = width; p->height = height; p->pitch = pitch;
    /* Separate allocations within staging keep the queue's non-overlap
     * contract. Never scratch the visible surface merely to prove a canary. */
    struct polaris_sdma_mapping a = {{staging->range.gpu_base, 4096}, staging->cpu};
    struct polaris_sdma_mapping b = {{staging->range.gpu_base + 4096, 4096},
                                     staging->cpu + 1024};
    int submitted = polaris_sdma_queue_fill(q, &b, 0, 0xa55ac33cu, 4096);
    if (submitted) {
        rc = failure(p, submitted); goto done;
    }
    for (unsigned i = 0; i < 1024; i++) a.cpu[i] = 0x7f123456u ^ (i * 0x10201u);
#ifndef POLARIS_PRESENT_NEGCTL_SKIP_COPY
    submitted = polaris_sdma_queue_copy(q, &a, 0, &b, 0, 4096);
    if (submitted) {
        rc = failure(p, submitted); goto done;
    }
#endif
    p->active = 1;
    rc = 0;
done:
    __atomic_store_n(&p->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
int polaris_present_rect(struct polaris_present *p, const uint32_t *pixels,
                          uint64_t bytes, uint32_t stride,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    int rc = -1;
    if (!p) return -1;
    if (__atomic_exchange_n(&p->lock, 1u, __ATOMIC_ACQUIRE)) return -2;
    if (p->quarantined || (p->queue && p->queue->quarantined)) {
        p->quarantined = 1; p->active = 0; rc = -2; goto done;
    }
    if (!p->active || !pixels || ((uintptr_t)pixels & 3u) || (stride & 3u) ||
        !w || !h || x >= p->width || y >= p->height ||
        w > p->width - x || h > p->height - y || stride < p->width * 4 ||
        bytes > UINTPTR_MAX - (uintptr_t)pixels ||
        (uint64_t)(y + h - 1) * stride + (uint64_t)(x + w) * 4 > bytes ||
        overlap((uintptr_t)pixels, bytes, (uintptr_t)p->staging.cpu, p->staging.range.bytes) ||
        overlap((uintptr_t)pixels, bytes, (uintptr_t)p->scanout.cpu, p->scanout.range.bytes) ||
        overlap((uintptr_t)pixels, bytes, (uintptr_t)p->queue->ring.cpu, p->queue->ring.range.bytes) ||
        overlap((uintptr_t)pixels, bytes, (uintptr_t)p->queue->fence.cpu, p->queue->fence.range.bytes))
        goto done;
    for (uint32_t row = 0; row < h; ) {
        uint64_t capacity = p->staging.range.bytes;
        if (capacity > POLARIS_SDMA_MAX_BYTES) capacity = POLARIS_SDMA_MAX_BYTES;
        /* Coalesce complete scanlines when both layouts are packed. Partial
         * rectangles preserve destination pitch/gutters; no tiled assumption. */
        uint32_t rows = 1;
        if (!x && w == p->width && p->pitch == w * 4 && capacity >= (uint64_t)w * 4) {
            rows = (uint32_t)(capacity / ((uint64_t)w * 4));
            if (rows > h - row) rows = h - row;
        }
        uint64_t total = (uint64_t)w * rows;
        for (uint64_t off = 0; off < total; ) {
            uint32_t count = (uint32_t)(total - off > capacity / 4 ? capacity / 4 : total - off);
            for (uint32_t i = 0; i < count; i++) {
                uint64_t index = off + i;
                p->staging.cpu[i] = pixels[(uint64_t)(y + row + index / w) * (stride / 4) + x + index % w];
            }
            uint64_t dst = (uint64_t)(y + row) * p->pitch + (uint64_t)x * 4 + off * 4;
            int submitted = polaris_sdma_queue_copy(p->queue, &p->staging, 0,
                                                      &p->scanout, dst, count * 4);
            if (submitted) {
                rc = failure(p, submitted); goto done;
            }
            p->uploaded_bytes += (uint64_t)count * 4;
            off += count;
        }
        row += rows;
    }
    p->frames++;
    p->pixels += (uint64_t)w * h;
    rc = 0;
done:
    __atomic_store_n(&p->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
