#include "amd/polaris/smu/toc.h"

/* Wire reference (fixed release so later Linux layout changes cannot silently
 * change this interface):
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/inc/smu_ucode_xfer_vi.h
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/smumgr/smu7_smumgr.c
 * The unusual HIGH address word before LOW is intentional. Serializing native
 * packed structs would hide that mistake and introduce host endian dependence. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static int cpu_range(const void *p, size_t n, uintptr_t *start, uintptr_t *end)
{
    uintptr_t a = (uintptr_t)p;
    if (!p || n > UINTPTR_MAX - a) return 0;
    *start = a;
    *end = a + n;
    return 1;
}

/* Both inputs are half-open ranges. Their checked end addresses come from
 * cpu_range() or the GPU-limit checks below; this helper does no addition. */
static int overlap(uint64_t first_start, uint64_t first_end,
                   uint64_t second_start, uint64_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int supported(uint32_t id)
{
    return (id >= POLARIS_SMU_IMAGE_SDMA0 &&
            id <= POLARIS_SMU_IMAGE_CP_MEC_JT2) ||
           id == POLARIS_SMU_IMAGE_RLC_G;
}

int polaris_smu_toc_encode(uint8_t *out, size_t capacity,
                            uint64_t toc_gpu_addr,
                            const struct polaris_smu_image *images,
                            size_t count, struct polaris_smu_toc_info *info)
{
    uintptr_t output_start, output_end;
    uintptr_t info_start, info_end;
    uintptr_t images_start = 0, images_end = 0;
    uint32_t present = 0;
    if (count > POLARIS_SMU_TOC_MAX_ENTRIES ||
        !cpu_range(out, POLARIS_SMU_TOC_BYTES, &output_start, &output_end) ||
        !cpu_range(info, sizeof(*info), &info_start, &info_end) ||
        (count && !cpu_range(images, count * sizeof(*images), &images_start, &images_end)) ||
        overlap(output_start, output_end, info_start, info_end) ||
        (count && (overlap(output_start, output_end, images_start, images_end) ||
                   overlap(info_start, info_end, images_start, images_end))))
        return POLARIS_SMU_TOC_INVALID;
    if (capacity < POLARIS_SMU_TOC_BYTES) return POLARIS_SMU_TOC_NO_SPACE;
    if ((toc_gpu_addr & 4095u) ||
        toc_gpu_addr > POLARIS_SMU_TOC_GPU_LIMIT - 4096u)
        return POLARIS_SMU_TOC_INVALID;

    /* Validate the complete inventory before the first output write: a bad
     * last image must not publish a half-updated directory from a prior boot. */
    for (size_t i = 0; i < count; ++i) {
        const struct polaris_smu_image *image = &images[i];
        uint32_t expected_flags = (image->id == POLARIS_SMU_IMAGE_RLC_G ||
                                  image->id == POLARIS_SMU_IMAGE_CP_MEC) ? 1u : 0u;
        if (!supported(image->id) || image->version > UINT16_MAX ||
            image->flags != expected_flags || !image->bytes ||
            ((image->gpu_addr | image->bytes) & 3u) ||
            image->gpu_addr >= POLARIS_SMU_TOC_GPU_LIMIT ||
            image->bytes > POLARIS_SMU_TOC_GPU_LIMIT - image->gpu_addr ||
            (present & (1u << image->id)) ||
            overlap(image->gpu_addr, image->gpu_addr + image->bytes,
                    toc_gpu_addr, toc_gpu_addr + 4096u))
            return POLARIS_SMU_TOC_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (overlap(image->gpu_addr, image->gpu_addr + image->bytes,
                        images[j].gpu_addr, images[j].gpu_addr + images[j].bytes))
                return POLARIS_SMU_TOC_INVALID;
        present |= 1u << image->id;
    }

    for (size_t i = 0; i < POLARIS_SMU_TOC_BYTES; ++i) out[i] = 0;
    put32(out, 1u);
    put32(out + 4, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        const struct polaris_smu_image *image = &images[i];
        uint8_t *entry = out + 8u + i * 28u;
        put16(entry, (uint16_t)image->id);
        put16(entry + 2, (uint16_t)image->version);
#ifdef POLARIS_SMU_TOC_NEGCTL_ADDRESS_ORDER
        put32(entry + 4, (uint32_t)image->gpu_addr);
        put32(entry + 8, (uint32_t)(image->gpu_addr >> 32));
#else
        put32(entry + 4, (uint32_t)(image->gpu_addr >> 32));
        put32(entry + 8, (uint32_t)image->gpu_addr);
#endif
        put32(entry + 20, image->bytes);
        put16(entry + 24, (uint16_t)image->flags);
    }
    info->present_mask = present;
    info->missing_mask = POLARIS_SMU_TOC_REQUIRED_MASK & ~present;
    info->entry_count = (uint32_t)count;
    return POLARIS_SMU_TOC_OK;
}
