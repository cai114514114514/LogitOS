#ifndef LOGIT_POLARIS_MEMORY_GART_H
#define LOGIT_POLARIS_MEMORY_GART_H
#include <stddef.h>
#include <stdint.h>
/* GMC8 definitions: Linux v6.12 amdgpu_vm.h / gmc_v8_0.c.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c
 * Deliberately omit executable, PRT, ATC and fragment bits: these tables map
 * ordinary coherent system RAM for SDMA, not shader virtual memory.
 */
#define POLARIS_PTE_VALID UINT64_C(0x01)
#define POLARIS_PTE_SYSTEM UINT64_C(0x02)
#define POLARIS_PTE_SNOOPED UINT64_C(0x04)
#define POLARIS_PTE_READABLE UINT64_C(0x20)
#define POLARIS_PTE_WRITEABLE UINT64_C(0x40)
struct polaris_gart_descriptor {
    uint32_t start_page, end_page, table_base_page;
    size_t table_bytes;
};
int polaris_gart_encode(uint64_t system_physical, uint64_t flags, uint64_t *pte);
/* Build little-endian 64-bit PTEs into ordinary RAM, and produce GMC8 VM0
 * register payloads. table_gpu is the already-reserved VRAM address where the
 * caller will upload this table; gpu_base names a SEPARATE GPU virtual range.
 * This helper does not establish either mapping or enable VM_CONTEXT0.
 * Input pages must be pinned DMA-coherent system pages, owned until GPU idle.
 * Caller holds exclusive table ownership with hardware disabled; after upload
 * it must issue the architecture's write barrier and invalidate GPU TLBs before
 * submitting work. Failure leaves BOTH table/descriptor unchanged. Buffers and
 * descriptor must be disjoint from pages and one another. No concurrent edits.
 */
int polaris_gart_build(void *table, size_t capacity, uint64_t table_gpu,
                       uint64_t gpu_base, const uint64_t *system_pages,
                       size_t page_count, uint64_t flags,
                       struct polaris_gart_descriptor *);
#endif
