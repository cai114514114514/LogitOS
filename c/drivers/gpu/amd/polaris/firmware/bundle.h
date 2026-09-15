#ifndef LOGIT_POLARIS_FW_BUNDLE_H
#define LOGIT_POLARIS_FW_BUNDLE_H
#include <stddef.h>
#include <stdint.h>
#include "amd/polaris/smu/toc.h"

enum polaris_fw_kind {
    POLARIS_FW_SDMA0, POLARIS_FW_SDMA1, POLARIS_FW_CE, POLARIS_FW_PFP,
    POLARIS_FW_ME, POLARIS_FW_MEC, POLARIS_FW_RLC, POLARIS_FW_SMC
};
#define POLARIS_FW_FILE_COUNT 7u
#define POLARIS_FW_BUNDLE_IMAGES 9u
struct polaris_fw_blob { const void *data; size_t bytes; };
struct polaris_fw_sources { struct polaris_fw_blob file[POLARIS_FW_FILE_COUNT]; };
struct polaris_fw_view {
    const uint8_t *ucode;
    uint32_t bytes, version, feature_version;
    uint32_t jt_offset_dwords, jt_size_dwords;
    uint32_t ucode_start_addr; /* SMC only; SRAM address, never a CPU pointer. */
    const uint8_t *aux[4]; /* RLC v2.0 register format/list and separate variants. */
    uint32_t aux_bytes[4];
};
struct polaris_fw_bundle_info {
    uint64_t proposed_gpu_base;
    size_t bytes_used;
    size_t image_offset[POLARIS_FW_BUNDLE_IMAGES];
    struct polaris_smu_image image[POLARIS_FW_BUNDLE_IMAGES];
    struct polaris_smu_toc_info inventory;
};
/* Exact Polaris10 formats: SDMA 1.0/1.1 IP3.1; GFX1.0/RLC2.0 IP8.0;
 * SMC1.0 IP7.2. Structural validation is not authenticity. Caller must retain
 * provenance and supply each kind from the correct named file. Borrowed views
 * require the immutable inputs to remain alive. On failure output is intact. */
int polaris_fw_parse(enum polaris_fw_kind kind, const void *data, size_t bytes,
                     struct polaris_fw_view *out);
/* Normal PF inventory only. Explicit mc_base is proposed placement; no mapping,
 * GPU ownership, upload or load completion follows. SMC is parsed separately
 * and is deliberately absent from the TOC/0x47e load request.
 * MEC image_size follows its JT offset, not the generic payload length. Both
 * JT entries get independent page-aligned copies of MEC1's jump table (VI
 * uses MEC1 for JT2 too). All source and output objects must remain stable;
 * outputs must not overlap sources or each other. Failures change no output. */
int polaris_fw_bundle_plan(const struct polaris_fw_sources *sources,
                           uint64_t mc_base, struct polaris_fw_bundle_info *out);
int polaris_fw_bundle_stage(uint8_t *dst, size_t capacity, uint64_t mc_base,
                            const struct polaris_fw_sources *sources,
                            struct polaris_fw_bundle_info *out);
#endif
