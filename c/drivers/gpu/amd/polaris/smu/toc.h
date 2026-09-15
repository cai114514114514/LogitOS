#ifndef LOGITOS_POLARIS_SMU_TOC_H
#define LOGITOS_POLARIS_SMU_TOC_H

#include <stddef.h>
#include <stdint.h>

/* Linux v6.12 smu_ucode_xfer_vi.h and smu7_smumgr.c define this SMU7 PF
 * directory. In particular SDMA0 is ID 1, not ID 0 (which denotes SMU itself).
 * A directory is only RAM data: encoding never allocates GPU memory, loads
 * firmware, writes a mailbox or establishes readiness for LoadUcodes. */
#define POLARIS_SMU_TOC_BYTES 344u
#define POLARIS_SMU_TOC_MAX_ENTRIES 12u
#define POLARIS_SMU_TOC_REQUIRED_MASK 0x0000047eu
#define POLARIS_SMU_TOC_GPU_LIMIT UINT64_C(0x10000000000)
#define POLARIS_SMU_IMAGE_SDMA0 1u
#define POLARIS_SMU_IMAGE_SDMA1 2u
#define POLARIS_SMU_IMAGE_CP_CE 3u
#define POLARIS_SMU_IMAGE_CP_PFP 4u
#define POLARIS_SMU_IMAGE_CP_ME 5u
#define POLARIS_SMU_IMAGE_CP_MEC 6u
#define POLARIS_SMU_IMAGE_CP_MEC_JT1 7u
#define POLARIS_SMU_IMAGE_CP_MEC_JT2 8u
#define POLARIS_SMU_IMAGE_RLC_G 10u
#define POLARIS_SMU_IMAGE_UNHALT 1u

/* These are host descriptors, NOT wire structs. Versions wider than the wire
 * u16 are refused rather than silently truncated. gpu_addr denotes a proposed
 * GPU address, never a CPU pointer or a PCI BAR address. Only normal physical
 * function ucode entries are supported; VF storage, metadata and register
 * restore entries need different validation and are deliberately absent. */
struct polaris_smu_image {
    uint32_t id;
    uint32_t version;
    uint64_t gpu_addr;
    uint32_t bytes;
    uint32_t flags;
};

struct polaris_smu_toc_info {
    uint32_t present_mask;
    uint32_t missing_mask;
    uint32_t entry_count;
};

enum polaris_smu_toc_result {
    POLARIS_SMU_TOC_OK = 0,
    POLARIS_SMU_TOC_INVALID = -1,
    POLARIS_SMU_TOC_NO_SPACE = -2,
};

/* out is ordinary exclusively owned RAM and may be byte-aligned. The out,
 * images and info CPU objects must be disjoint. On failure out and info remain
 * untouched.
 * The TOC GPU address must be 4096 aligned (matching the SMU7 header BO).
 * Firmware ranges must be DWORD aligned (this API's minimum; whole-image
 * staging uses page alignment, while MEC jump tables can be subimages),
 * disjoint from one another and the
 * TOC's full 4096-byte allocation, and fit below the Polaris 40-bit limit.
 * Partial directories are allowed, including empty inventories; missing_mask
 * always reports missing members of the standard Polaris load mask 0x47e.
 * Zero missing_mask proves inventory only, NOT GPU mapping or load readiness. */
int polaris_smu_toc_encode(uint8_t *out, size_t capacity,
                            uint64_t toc_gpu_addr,
                            const struct polaris_smu_image *images,
                            size_t count, struct polaris_smu_toc_info *info);

#endif
