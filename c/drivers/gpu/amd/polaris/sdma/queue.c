/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/sdma/queue.h"

/* Linux v6.12 sdma_v3_0.c: ring_set_wptr uses BYTE pointers, RB_BASE is
 * address >> 8, RB_SIZE is log2(DWORD count), and ring funcs require 16-DWORD
 * fetch padding. oss_3_0_d.h register indices below are converted to bytes.
 * This deliberately does not copy gfx_resume: boot firmware, VM mapping and
 * platform cache visibility must exist before attachment can be attempted. */
#define REG_F32          (0x3412u * 4u)
#define REG_RB_CNTL      (0x3480u * 4u)
#define REG_RB_BASE      (0x3481u * 4u)
#define REG_RB_BASE_HI   (0x3482u * 4u)
#define REG_RPTR         (0x3483u * 4u)
#define REG_WPTR         (0x3484u * 4u)
#define REG_POLL         (0x3485u * 4u)
#define REG_DOORBELL     (0x3492u * 4u)
#define REG_VIRTUAL      (0x34a7u * 4u)
#define REG_APE1         (0x34a8u * 4u)
#define WAIT_US          5000u
#define WAIT_POLLS       100000u

static int valid_map(const struct polaris_sdma_mapping *m)
{
    return m && m->cpu && m->range.bytes &&
        !(((uintptr_t)m->cpu | m->range.gpu_base | m->range.bytes) & 3u) &&
        m->range.gpu_base < POLARIS_SDMA_GPU_LIMIT &&
        m->range.bytes <= POLARIS_SDMA_GPU_LIMIT - m->range.gpu_base &&
        m->range.bytes <= UINTPTR_MAX - (uintptr_t)m->cpu;
}
static int overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}
static int maps_overlap(const struct polaris_sdma_mapping *a,
                         const struct polaris_sdma_mapping *b)
{
    return overlap(a->range.gpu_base, a->range.bytes,
                   b->range.gpu_base, b->range.bytes) ||
           overlap((uintptr_t)a->cpu, a->range.bytes,
                   (uintptr_t)b->cpu, b->range.bytes);
}
static int resolve(const struct polaris_sdma_queue_ops *ops,
                    const struct polaris_sdma_mapping *m)
{
    volatile uint32_t *cpu = 0;
    return ops->resolve_mapping(ops->opaque, m->range.gpu_base,
                                 m->range.bytes, &cpu) || cpu != m->cpu ?
           POLARIS_SDMA_QUEUE_UNCONFIGURED : 0;
}
static int reg(const struct polaris_sdma_queue_ops *ops, uint32_t address,
                uint32_t *value)
{
    if (ops->read_reg(ops->opaque, address, value) || *value == UINT32_MAX)
        return POLARIS_SDMA_QUEUE_IO;
    return 0;
}
static int configured(const struct polaris_sdma_queue_ops *ops,
                       const struct polaris_sdma_mapping *ring,
                       uint32_t *pointer)
{
    uint32_t id, f32, control, base, high, rptr, wptr, poll, doorbell, va, ape, select, cntl;
    unsigned order = 0;
    uint64_t words = ring->range.bytes / 4;
    if (ops->read_identity(ops->opaque, &id)) return POLARIS_SDMA_QUEUE_IO;
    if (id != 0x67df1002u) return POLARIS_SDMA_QUEUE_UNCONFIGURED;
    if (reg(ops, 0x0e44u, &select) || reg(ops, 0xd010u, &cntl) ||
        reg(ops, REG_F32, &f32) || reg(ops, REG_RB_CNTL, &control) ||
        reg(ops, REG_RB_BASE, &base) || reg(ops, REG_RB_BASE_HI, &high) ||
        reg(ops, REG_RPTR, &rptr) || reg(ops, REG_WPTR, &wptr) ||
        reg(ops, REG_POLL, &poll) || reg(ops, REG_DOORBELL, &doorbell) ||
        reg(ops, REG_VIRTUAL, &va) || reg(ops, REG_APE1, &ape))
        return POLARIS_SDMA_QUEUE_IO;
    while ((UINT64_C(1) << order) < words) order++;
    /* RPTR is read directly, so writeback must be disabled. Otherwise the
     * inherited queue could also DMA to an unowned old driver's WB address.
     * Reject swapped data, nonzero VMID, doorbell and shadow polling paths. */
    if (select || (cntl & 0x40018u) || (f32 & 1u) || !(control & 1u) ||
        ((control >> 1) & 31u) != order || (control & 0x0f003200u) ||
        base != (uint32_t)(ring->range.gpu_base >> 8) || high ||
        (poll & 1u) || (doorbell & 0x10000000u) || va || ape ||
        rptr != wptr || (rptr & 3u) || rptr >= ring->range.bytes)
        return POLARIS_SDMA_QUEUE_UNCONFIGURED;
    *pointer = wptr;
    return 0;
}

int polaris_sdma_queue_attach(struct polaris_sdma_queue *q,
                              const struct polaris_sdma_queue_ops *ops,
                              const struct polaris_sdma_mapping *ring,
                              const struct polaris_sdma_mapping *fence)
{
    int rc;
    uint32_t pointer;
    if (!q) return POLARIS_SDMA_QUEUE_INVALID;
    if (__atomic_exchange_n(&q->lock, 1u, __ATOMIC_ACQUIRE))
        return POLARIS_SDMA_QUEUE_BUSY;
    if (q->quarantined) { rc = POLARIS_SDMA_QUEUE_QUARANTINED; goto done; }
    if (q->attached) { rc = POLARIS_SDMA_QUEUE_BUSY; goto done; }
    if (!ops || !ops->read_identity || !ops->read_reg || !ops->write_wptr ||
        !ops->resolve_mapping || !ops->sync || !ops->now_us ||
        !valid_map(ring) || !valid_map(fence) ||
        ring->range.bytes < 256 || ring->range.bytes > 65536 ||
        (ring->range.bytes & (ring->range.bytes - 1)) ||
        (ring->range.gpu_base & 255u) || fence->range.bytes != 4 ||
        maps_overlap(ring, fence)) { rc = POLARIS_SDMA_QUEUE_INVALID; goto done; }
    rc = configured(ops, ring, &pointer);
    if (rc) goto done;
    if (resolve(ops, ring) || resolve(ops, fence)) {
        rc = POLARIS_SDMA_QUEUE_UNCONFIGURED; goto done;
    }
    q->ops = *ops;
    q->ring = *ring;
    q->fence = *fence;
    q->wptr_bytes = pointer;
    q->attached = 1;
done:
    __atomic_store_n(&q->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}

static int sync_map(struct polaris_sdma_queue *q, enum polaris_sdma_sync dir,
                     const struct polaris_sdma_mapping *m)
{
    return q->ops.sync(q->ops.opaque, dir, m) ? POLARIS_SDMA_QUEUE_IO : 0;
}
static int work(struct polaris_sdma_queue *q,
                 const struct polaris_sdma_mapping *src, uint64_t src_offset,
                 const struct polaris_sdma_mapping *dst, uint64_t dst_offset,
                 uint32_t value, uint32_t bytes)
{
    uint32_t words[32] = {0}, pointer, sequence, count, target, rptr, wptr;
    struct polaris_sdma_stream stream = {words, 32, 0};
    int rc;
    uint64_t start, last;
    if (!q) return POLARIS_SDMA_QUEUE_INVALID;
    if (__atomic_exchange_n(&q->lock, 1u, __ATOMIC_ACQUIRE))
        return POLARIS_SDMA_QUEUE_BUSY;
    if (q->quarantined) {
#ifdef POLARIS_QUEUE_NEGCTL_LATE_COMPLETION
        if (q->fence.cpu && q->fence.cpu[0] == q->sequence) {
            rc = POLARIS_SDMA_QUEUE_OK; goto done;
        }
#endif
        rc = POLARIS_SDMA_QUEUE_QUARANTINED; goto done;
    }
    if (!q->attached) { rc = POLARIS_SDMA_QUEUE_UNCONFIGURED; goto done; }
    if (!valid_map(dst) || (src && !valid_map(src)) ||
        maps_overlap(dst, &q->ring) || maps_overlap(dst, &q->fence) ||
        (src && (maps_overlap(src, &q->ring) || maps_overlap(src, &q->fence) ||
                 maps_overlap(src, dst)))) {
        rc = POLARIS_SDMA_QUEUE_INVALID; goto done;
    }
    if (src) rc = polaris_sdma_emit_copy(&stream, &src->range, src_offset,
                                        &dst->range, dst_offset, bytes);
    else rc = polaris_sdma_emit_fill(&stream, &dst->range, dst_offset, value, bytes);
    if (rc) { rc = POLARIS_SDMA_QUEUE_INVALID; goto done; }
    if (q->sequence == UINT32_MAX) {
        rc = POLARIS_SDMA_QUEUE_SEQUENCE; goto quarantine;
    }
    sequence = q->sequence + 1;
    if (polaris_sdma_emit_fence(&stream, &q->fence.range, 0, sequence)) {
        rc = POLARIS_SDMA_QUEUE_INVALID; goto done;
    }
    rc = configured(&q->ops, &q->ring, &pointer);
    /* Once attached, changed pointers/configuration or lost mapping is loss
     * of exclusive engine ownership, potentially with foreign DMA in flight.
     * A pre-submit failure here must not authorize CPU destination fallback. */
    if (rc) goto quarantine;
    if (pointer != q->wptr_bytes || resolve(&q->ops, &q->ring) ||
        resolve(&q->ops, &q->fence) || resolve(&q->ops, dst) ||
        (src && resolve(&q->ops, src))) {
        rc = POLARIS_SDMA_QUEUE_UNCONFIGURED; goto quarantine;
    }
    if (sync_map(q, POLARIS_SDMA_TO_DEVICE, dst) ||
        (src && sync_map(q, POLARIS_SDMA_TO_DEVICE, src))) {
        rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
    }
    /* Zero plus a non-reused sequence prevents a previous completion from
     * satisfying this wait. The flush/readback happens before publishing WPTR. */
    q->fence.cpu[0] = 0;
    if (sync_map(q, POLARIS_SDMA_TO_DEVICE, &q->fence) ||
        sync_map(q, POLARIS_SDMA_TO_CPU, &q->fence)) {
        rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
    }
    if (q->fence.cpu[0]) { rc = POLARIS_SDMA_QUEUE_READBACK; goto quarantine; }
    count = (uint32_t)stream.used_dw;
    /* Zero DWORD is an SDMA NOP. Pad the final absolute pointer, not merely
     * packet length: attachment can start near the circular wrap boundary. */
    while (((pointer / 4) + count) & 15u) words[count++] = 0;
    for (uint32_t i = 0; i < count; i++)
        q->ring.cpu[((pointer / 4) + i) & (q->ring.range.bytes / 4 - 1)] = words[i];
    if (sync_map(q, POLARIS_SDMA_TO_DEVICE, &q->ring)) {
        rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
    }
    target = (pointer + count * 4) & (uint32_t)(q->ring.range.bytes - 1);
    start = last = q->ops.now_us(q->ops.opaque);
    q->sequence = sequence;
    q->submitted++;
    /* Even a failing callback may already have posted WPTR. From this point
     * no error permits retry, memory reuse, or CPU fallback onto destination. */
    if (q->ops.write_wptr(q->ops.opaque, target)) {
        rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
    }
    q->wptr_bytes = target;
    for (unsigned poll = 0; poll < WAIT_POLLS; poll++) {
        uint64_t now = q->ops.now_us(q->ops.opaque);
        if (now < last || now - start >= WAIT_US) {
            rc = POLARIS_SDMA_QUEUE_TIMEOUT; goto quarantine;
        }
        last = now;
        if (sync_map(q, POLARIS_SDMA_TO_CPU, &q->fence) ||
            reg(&q->ops, REG_RPTR, &rptr) || reg(&q->ops, REG_WPTR, &wptr)) {
            rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
        }
        if (wptr != target || (rptr & 3u) || rptr >= q->ring.range.bytes) {
            rc = POLARIS_SDMA_QUEUE_UNCONFIGURED; goto quarantine;
        }
        if (q->fence.cpu[0] == sequence && rptr == target) {
            now = q->ops.now_us(q->ops.opaque);
            if (now < last || now - start >= WAIT_US) {
                rc = POLARIS_SDMA_QUEUE_TIMEOUT; goto quarantine;
            }
            goto verify;
        }
    }
    rc = POLARIS_SDMA_QUEUE_TIMEOUT;
    goto quarantine;
verify:
    if (sync_map(q, POLARIS_SDMA_TO_CPU, dst) ||
        (src && sync_map(q, POLARIS_SDMA_TO_CPU, src))) {
        rc = POLARIS_SDMA_QUEUE_IO; goto quarantine;
    }
    for (uint32_t i = 0; i < bytes / 4; i++) {
        uint32_t expected = src ? src->cpu[src_offset / 4 + i] : value;
        if (dst->cpu[dst_offset / 4 + i] != expected) {
            rc = POLARIS_SDMA_QUEUE_READBACK; goto quarantine;
        }
    }
    q->completed++;
    if (src) q->copied_bytes += bytes;
    else q->filled_bytes += bytes;
    rc = POLARIS_SDMA_QUEUE_OK;
    goto done;
quarantine:
    q->quarantined = 1;
done:
    __atomic_store_n(&q->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
int polaris_sdma_queue_copy(struct polaris_sdma_queue *q,
                            const struct polaris_sdma_mapping *src,
                            uint64_t src_offset,
                            const struct polaris_sdma_mapping *dst,
                            uint64_t dst_offset, uint32_t bytes)
{
    if (!src) return POLARIS_SDMA_QUEUE_INVALID;
    return work(q, src, src_offset, dst, dst_offset, 0, bytes);
}
int polaris_sdma_queue_fill(struct polaris_sdma_queue *q,
                            const struct polaris_sdma_mapping *dst,
                            uint64_t dst_offset, uint32_t value, uint32_t bytes)
{
    return work(q, 0, 0, dst, dst_offset, value, bytes);
}
