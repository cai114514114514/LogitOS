#include "amd/polaris/smu/stage.h"
#include "amd/polaris/firmware.h"

/* Build the complete image before any future GPU ownership handoff. Linux
 * v6.12 amdgpu_ucode_init_bo() places whole firmware payloads on page
 * boundaries; smu7_smumgr.c puts the TOC in a separate page-aligned buffer.
 * This RAM layout reserves equivalent disjoint pages in one proposed span.
 * It does not establish that span in VRAM/GTT. Normal PF retains the entire
 * payload: upstream's 20-byte digest removal is restricted to VF handling. */
static int span_ok(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn &&
           (uintptr_t)b < (uintptr_t)a + an;
}
static uint64_t page_up(uint64_t n)
{ return (n + 4095u) & ~UINT64_C(4095); }

int polaris_smu_stage_sdma(uint8_t *dst, size_t capacity, uint64_t gpu_base,
                           const void *fw0, size_t bytes0,
                           const void *fw1, size_t bytes1,
                           struct polaris_smu_stage_info *out)
{
    if (!span_ok(dst, capacity) || !span_ok(out, sizeof *out) ||
        !span_ok(fw0, bytes0) || !span_ok(fw1, bytes1) ||
        overlap(dst, capacity, out, sizeof *out) ||
        overlap(dst, capacity, fw0, bytes0) || overlap(dst, capacity, fw1, bytes1) ||
        overlap(out, sizeof *out, fw0, bytes0) || overlap(out, sizeof *out, fw1, bytes1))
        return -1;
    if ((gpu_base & 4095u) || gpu_base >= POLARIS_SMU_TOC_GPU_LIMIT ||
        capacity > POLARIS_SMU_TOC_GPU_LIMIT - gpu_base)
        return -1;
    struct polaris_sdma_firmware fw[2];
    if (polaris_sdma_firmware_parse(fw0, bytes0, &fw[0]) ||
        polaris_sdma_firmware_parse(fw1, bytes1, &fw[1])) return -1;
    for (unsigned i = 0; i < 2; i++)
        if (fw[i].ip_major != 3 || fw[i].ip_minor != 1) return -1;
    uint64_t offsets[2] = {4096u, 4096u + page_up(fw[0].ucode_bytes)};
    uint64_t total = offsets[1] + page_up(fw[1].ucode_bytes);
    if (total > capacity || total > SIZE_MAX) return -1;
    struct polaris_smu_image images[2];
    for (unsigned i = 0; i < 2; i++) images[i] = (struct polaris_smu_image){
        .id = i ? POLARIS_SMU_IMAGE_SDMA1 : POLARIS_SMU_IMAGE_SDMA0,
        .version = fw[i].ucode_version,
        .gpu_addr = gpu_base + offsets[i], .bytes = fw[i].ucode_bytes, .flags = 0,
    };
    uint8_t toc[POLARIS_SMU_TOC_BYTES];
    struct polaris_smu_toc_info inventory;
    if (polaris_smu_toc_encode(toc, sizeof toc, gpu_base, images, 2, &inventory))
        return -1;
    struct polaris_smu_stage_info result = {
        .proposed_gpu_base = gpu_base, .bytes_used = (size_t)total,
        .image_offset = {(size_t)offsets[0], (size_t)offsets[1]},
        .image_bytes = {fw[0].ucode_bytes, fw[1].ucode_bytes}, .inventory = inventory,
    };
    /* All possible failures are above this point. Zero unused pages/padding
     * so later DMA cannot expose leftover RAM or consume accidental entries. */
    for (size_t n = 0; n < result.bytes_used; n++) dst[n] = 0;
    for (size_t n = 0; n < sizeof toc; n++) dst[n] = toc[n];
    for (unsigned i = 0; i < 2; i++)
        for (uint32_t n = 0; n < fw[i].ucode_bytes; n++)
            dst[result.image_offset[i] + n] = fw[i].ucode[n];
#ifdef POLARIS_SMU_STAGE_NEGCTL_CORRUPT
    dst[result.image_offset[1]] ^= 1u; /* Watched wrong-instance payload copy. */
#endif
    *out = result;
    return 0;
}
