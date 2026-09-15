#include "polaris_sdma.h"

/* Packet layout reference: AMD-authored Linux v6.12
 * drivers/gpu/drm/amd/amdgpu/{sdma_v3_0.c,tonga_sdma_pkt_open.h}:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h
 * emit_copy_buffer / emit_fill_buffer write a BYTE COUNT, without subtracting
 * one. SDMA v4's count-minus-one silently leaves a byte behind on Polaris.
 * Linux limits these transfers to 0x3fffe0 despite the 22-bit count field.
 * gmc_v8_0.c sets a 40-bit MC address mask; keeping range arithmetic below
 * that ceiling also prevents high bits being silently discarded by hardware.
 * No upstream initialization sequence is copied or run by this module. */

static int range_address(const struct polaris_gpu_range *range,
                         uint64_t offset, uint32_t bytes, uint64_t *address)
{
    if (!range || !range->bytes || !bytes ||
        ((range->gpu_base | range->bytes | offset | bytes) & 3u) ||
        range->gpu_base >= POLARIS_SDMA_GPU_LIMIT ||
        range->bytes > POLARIS_SDMA_GPU_LIMIT - range->gpu_base ||
        offset > range->bytes || bytes > range->bytes - offset)
        return POLARIS_SDMA_INVALID;
    *address = range->gpu_base + offset;
    return POLARIS_SDMA_OK;
}

static int append(struct polaris_sdma_stream *stream,
                  const uint32_t *packet, size_t count)
{
    if (!stream || !stream->words ||
        ((uintptr_t)stream->words & (sizeof(uint32_t) - 1u)) ||
        stream->used_dw > stream->capacity_dw ||
        stream->capacity_dw > SIZE_MAX / sizeof(uint32_t) ||
        stream->capacity_dw * sizeof(uint32_t) >
            UINTPTR_MAX - (uintptr_t)stream->words)
        return POLARIS_SDMA_INVALID;
    if (count > stream->capacity_dw - stream->used_dw)
        return POLARIS_SDMA_NO_SPACE;
    /* Build and validate the complete packet before touching the stream. A
     * partial command followed by an error is hazardous if a future caller
     * submits the already accumulated buffer. */
    for (size_t i = 0; i < count; ++i)
        stream->words[stream->used_dw + i] = packet[i];
    stream->used_dw += count;
    return POLARIS_SDMA_OK;
}

static int count_valid(uint32_t bytes)
{
    return bytes && bytes <= POLARIS_SDMA_MAX_BYTES && !(bytes & 3u);
}

int polaris_sdma_emit_copy(struct polaris_sdma_stream *stream,
                           const struct polaris_gpu_range *src,
                           uint64_t src_offset,
                           const struct polaris_gpu_range *dst,
                           uint64_t dst_offset, uint32_t bytes)
{
    uint64_t source, destination;
    if (!count_valid(bytes) ||
        range_address(src, src_offset, bytes, &source) ||
        range_address(dst, dst_offset, bytes, &destination))
        return POLARIS_SDMA_INVALID;
    if (source < destination + bytes && destination < source + bytes)
        return POLARIS_SDMA_INVALID;
    uint32_t packet[POLARIS_SDMA_COPY_DW] = {
        0x00000001u, bytes, 0u,
        (uint32_t)source, (uint32_t)(source >> 32),
        (uint32_t)destination, (uint32_t)(destination >> 32),
    };
#ifdef POLARIS_SDMA_NEGCTL_COUNT_MINUS_ONE
    packet[1]--; /* Gate must reject the tempting SDMA v4 encoding. */
#endif
    return append(stream, packet, POLARIS_SDMA_COPY_DW);
}

int polaris_sdma_emit_fill(struct polaris_sdma_stream *stream,
                           const struct polaris_gpu_range *dst,
                           uint64_t dst_offset, uint32_t value, uint32_t bytes)
{
    uint64_t destination;
    if (!count_valid(bytes) ||
        range_address(dst, dst_offset, bytes, &destination))
        return POLARIS_SDMA_INVALID;
    const uint32_t packet[POLARIS_SDMA_FILL_DW] = {
        0x0000000bu, (uint32_t)destination,
        (uint32_t)(destination >> 32), value, bytes,
    };
    return append(stream, packet, POLARIS_SDMA_FILL_DW);
}

int polaris_sdma_emit_fence(struct polaris_sdma_stream *stream,
                            const struct polaris_gpu_range *dst,
                            uint64_t dst_offset, uint32_t sequence)
{
    uint64_t destination;
    if (range_address(dst, dst_offset, sizeof(uint32_t), &destination))
        return POLARIS_SDMA_INVALID;
    const uint32_t packet[POLARIS_SDMA_FENCE_DW] = {
        0x00000005u, (uint32_t)destination,
        (uint32_t)(destination >> 32), sequence,
    };
    return append(stream, packet, POLARIS_SDMA_FENCE_DW);
}
