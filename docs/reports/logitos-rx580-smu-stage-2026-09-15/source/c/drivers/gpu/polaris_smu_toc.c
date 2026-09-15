#include "polaris_smu_toc.h"

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

static int overlap(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return a < d && c < b;
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
    uintptr_t out_a, out_b, info_a, info_b, in_a = 0, in_b = 0;
    uint32_t present = 0;
    if (count > POLARIS_SMU_TOC_MAX_ENTRIES ||
        !cpu_range(out, POLARIS_SMU_TOC_BYTES, &out_a, &out_b) ||
        !cpu_range(info, sizeof(*info), &info_a, &info_b) ||
        (count && !cpu_range(images, count * sizeof(*images), &in_a, &in_b)) ||
        overlap(out_a, out_b, info_a, info_b) ||
        (count && (overlap(out_a, out_b, in_a, in_b) ||
                   overlap(info_a, info_b, in_a, in_b))))
        return POLARIS_SMU_TOC_INVALID;
    if (capacity < POLARIS_SMU_TOC_BYTES) return POLARIS_SMU_TOC_NO_SPACE;
    if ((toc_gpu_addr & 4095u) ||
        toc_gpu_addr > POLARIS_SMU_TOC_GPU_LIMIT - 4096u)
        return POLARIS_SMU_TOC_INVALID;

    /* Validate the complete inventory before the first output write: a bad
     * last image must not publish a half-updated directory from a prior boot. */
    for (size_t i = 0; i < count; ++i) {
        const struct polaris_smu_image *im = &images[i];
        uint32_t expected_flags = (im->id == POLARIS_SMU_IMAGE_RLC_G ||
                                  im->id == POLARIS_SMU_IMAGE_CP_MEC) ? 1u : 0u;
        if (!supported(im->id) || im->version > UINT16_MAX ||
            im->flags != expected_flags || !im->bytes ||
            ((im->gpu_addr | im->bytes) & 3u) ||
            im->gpu_addr >= POLARIS_SMU_TOC_GPU_LIMIT ||
            im->bytes > POLARIS_SMU_TOC_GPU_LIMIT - im->gpu_addr ||
            (present & (1u << im->id)) ||
            overlap(im->gpu_addr, im->gpu_addr + im->bytes,
                    toc_gpu_addr, toc_gpu_addr + 4096u))
            return POLARIS_SMU_TOC_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (overlap(im->gpu_addr, im->gpu_addr + im->bytes,
                        images[j].gpu_addr, images[j].gpu_addr + images[j].bytes))
                return POLARIS_SMU_TOC_INVALID;
        present |= 1u << im->id;
    }

    for (size_t i = 0; i < POLARIS_SMU_TOC_BYTES; ++i) out[i] = 0;
    put32(out, 1u);
    put32(out + 4, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        const struct polaris_smu_image *im = &images[i];
        uint8_t *entry = out + 8u + i * 28u;
        put16(entry, (uint16_t)im->id);
        put16(entry + 2, (uint16_t)im->version);
#ifdef POLARIS_SMU_TOC_NEGCTL_ADDRESS_ORDER
        put32(entry + 4, (uint32_t)im->gpu_addr);
        put32(entry + 8, (uint32_t)(im->gpu_addr >> 32));
#else
        put32(entry + 4, (uint32_t)(im->gpu_addr >> 32));
        put32(entry + 8, (uint32_t)im->gpu_addr);
#endif
        put32(entry + 20, im->bytes);
        put16(entry + 24, (uint16_t)im->flags);
    }
    info->present_mask = present;
    info->missing_mask = POLARIS_SMU_TOC_REQUIRED_MASK & ~present;
    info->entry_count = (uint32_t)count;
    return POLARIS_SMU_TOC_OK;
}
