#ifndef LOGITOS_POLARIS_SDMA_H
#define LOGITOS_POLARIS_SDMA_H

#include <stddef.h>
#include <stdint.h>

/* Polaris uses SDMA 3.1 (the v3 packet format), not RV100's register blitter.
 * This module only builds CPU-memory packets. It does not map GPU memory,
 * initialize firmware/rings, submit work, or claim hardware acceleration. */
#define POLARIS_SDMA_MAX_BYTES 0x003fffe0u
#define POLARIS_SDMA_GPU_LIMIT UINT64_C(0x10000000000)
#define POLARIS_SDMA_COPY_DW 7u
#define POLARIS_SDMA_FILL_DW 5u
#define POLARIS_SDMA_FENCE_DW 4u

/* A GPU-visible allocation already mapped by the caller in one address space.
 * gpu_base is NOT a CPU pointer, CPU physical address, or PCI BAR address.
 * Supplying this descriptor does not establish a mapping. Its entire interval
 * must remain allocated/mapped until a future hardware consumer completes.
 * This initial API deliberately accepts only DWORD-aligned allocations and
 * transfers; tiled images and partial pixels require a separate contract. */
struct polaris_gpu_range {
    uint64_t gpu_base;
    uint64_t bytes;
};

/* Exclusive ownership of valid, aligned ordinary CPU memory is required.
 * This is a linear packet buffer, not a live circular hardware ring. Words are
 * native uint32_t; the LogitOS x86 target stores the required little endian.
 * Buffer/descriptors/stream metadata must be distinct CPU objects. */
struct polaris_sdma_stream {
    uint32_t *words;
    size_t capacity_dw;
    size_t used_dw;
};

enum polaris_sdma_result {
    POLARIS_SDMA_OK = 0,
    POLARIS_SDMA_INVALID = -1,
    POLARIS_SDMA_NO_SPACE = -2,
};

/* Every failure leaves both stream metadata and output words untouched.
 * Copy is deliberately not memmove: overlapping GPU intervals are refused,
 * including descriptors which alias the same allocation. */
int polaris_sdma_emit_copy(struct polaris_sdma_stream *stream,
                           const struct polaris_gpu_range *src,
                           uint64_t src_offset,
                           const struct polaris_gpu_range *dst,
                           uint64_t dst_offset, uint32_t bytes);
int polaris_sdma_emit_fill(struct polaris_sdma_stream *stream,
                           const struct polaris_gpu_range *dst,
                           uint64_t dst_offset, uint32_t value, uint32_t bytes);
/* Writes one 32-bit sequence value. No trap/interrupt packet is emitted;
 * submitting this packet alone does not prove completion or cache visibility. */
int polaris_sdma_emit_fence(struct polaris_sdma_stream *stream,
                            const struct polaris_gpu_range *dst,
                            uint64_t dst_offset, uint32_t sequence);

#endif
