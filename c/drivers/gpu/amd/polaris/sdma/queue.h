/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGITOS_POLARIS_SDMA_QUEUE_H
#define LOGITOS_POLARIS_SDMA_QUEUE_H
#include "amd/polaris/sdma/packet.h"

struct polaris_sdma_mapping {
    struct polaris_gpu_range range;
    volatile uint32_t *cpu;
};
enum polaris_sdma_sync {
    POLARIS_SDMA_TO_DEVICE, POLARIS_SDMA_TO_CPU
};
/* SDMA0 only, VMID 0, register WPTR submission. resolve_mapping must inspect
 * the platform's actual current GPU mapping, covering the entire interval,
 * and return its CPU mapping; echoing the supplied address is not evidence.
 * sync implements platform CPU cache/device HDP visibility and ordering, NOT
 * just a compiler barrier. TO_DEVICE also prevents dirty destination cache
 * lines from overwriting subsequent GPU writes. Callbacks must be bounded.
 * Exclusive device/VM ownership and allocation lifetimes span attach through
 * all operations; another queue/firmware owner must not reconfigure it. */
struct polaris_sdma_queue_ops {
    void *opaque;
    int (*read_identity)(void *, uint32_t *vendor_device);
    int (*read_reg)(void *, uint32_t byte_offset, uint32_t *);
    int (*write_wptr)(void *, uint32_t byte_pointer);
    int (*resolve_mapping)(void *, uint64_t gpu_address, uint64_t bytes,
                           volatile uint32_t **cpu);
    int (*sync)(void *, enum polaris_sdma_sync,
                const struct polaris_sdma_mapping *);
    uint64_t (*now_us)(void *);
};
enum polaris_sdma_queue_result {
    POLARIS_SDMA_QUEUE_OK = 0,
    POLARIS_SDMA_QUEUE_INVALID = -1,
    POLARIS_SDMA_QUEUE_BUSY = -2,
    POLARIS_SDMA_QUEUE_UNCONFIGURED = -3,
    POLARIS_SDMA_QUEUE_IO = -4,
    POLARIS_SDMA_QUEUE_TIMEOUT = -5,
    POLARIS_SDMA_QUEUE_QUARANTINED = -6,
    POLARIS_SDMA_QUEUE_READBACK = -7,
    POLARIS_SDMA_QUEUE_SEQUENCE = -8
};
struct polaris_sdma_queue {
    unsigned lock, attached, quarantined;
    struct polaris_sdma_queue_ops ops;
    struct polaris_sdma_mapping ring, fence;
    uint32_t wptr_bytes, sequence;
    uint64_t submitted, completed, copied_bytes, filled_bytes;
};

/* Zero-initialize once, do not reset/re-attach a quarantined context. Attach
 * performs readbacks only; it cannot configure or cold-start an SDMA engine.
 * Register RPTR mode requires RPTR writeback disabled, avoiding writes to an
 * inherited owner's writeback address; SRBM_GFX_CNTL must select VMID 0.
 * Ring: 256..65536 bytes, power of two, GPU base 256-byte aligned. Fence:
 * exactly one DWORD, separate from ring and every work buffer. CPU descriptor
 * objects must be mutually disjoint and separate from all mapped memory.
 * Re-attachment of an already attached context is refused, preserving its
 * unique fence sequence. Success proves matching configuration, not execution. */
int polaris_sdma_queue_attach(struct polaris_sdma_queue *,
                              const struct polaris_sdma_queue_ops *,
                              const struct polaris_sdma_mapping *ring,
                              const struct polaris_sdma_mapping *fence);

/* Synchronous copy/fill with fixed 5 ms / 100000-poll maximum completion wait.
 * Each transaction appends a fence and pads to the 16-DWORD fetch boundary.
 * It requires both that fence and RPTR == WPTR, then compares EVERY destination
 * DWORD to source/value. Buffers remain exclusively owned, allocated and mapped
 * on ALL returns: timeout does not cancel DMA. A quarantined context performs
 * no further callbacks; only an independently verified hardware reset could
 * establish when pending DMA and its allocations are safe to release.
 * Source and destination must not overlap each other or queue control memory
 * in either GPU or CPU address space. The first operation also provides the
 * actual execution test; there is no caller-supplied ready Boolean. */
int polaris_sdma_queue_copy(struct polaris_sdma_queue *,
                            const struct polaris_sdma_mapping *src,
                            uint64_t src_offset,
                            const struct polaris_sdma_mapping *dst,
                            uint64_t dst_offset, uint32_t bytes);
int polaris_sdma_queue_fill(struct polaris_sdma_queue *,
                            const struct polaris_sdma_mapping *dst,
                            uint64_t dst_offset, uint32_t value, uint32_t bytes);
#endif
