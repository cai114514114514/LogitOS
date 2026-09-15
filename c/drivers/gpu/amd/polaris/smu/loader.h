#ifndef LOGIT_POLARIS_SMU_LOADER_H
#define LOGIT_POLARIS_SMU_LOADER_H
#include "amd/polaris/firmware/bundle.h"

struct polaris_smu_loader_ops {
    void *opaque;
    int (*read_identity)(void *opaque, uint32_t *vendor_device);
    int (*read_reg)(void *opaque, uint32_t byte_offset, uint32_t *value);
    int (*write_reg)(void *opaque, uint32_t byte_offset, uint32_t value);
    int (*resolve_mapping)(void *opaque, uint64_t gpu_address, uint64_t bytes,
                           volatile uint8_t **cpu);
    int (*sync_to_device)(void *opaque, uint64_t gpu_address, uint64_t bytes);
    uint64_t (*now_us)(void *opaque);
};
struct polaris_smu_load_request {
    const struct polaris_fw_view *smc;
    unsigned smc_security_key; /* 1: matching normal/_k/_k2 SMC; 0: _sk SMC. */
    uint64_t toc_gpu, scratch_gpu;
    const struct polaris_smu_image *images;
    size_t image_count;
};
struct polaris_smu_loader {
    unsigned lock, quarantined, loaded;
    uint32_t soft_regs, load_status, messages, uploaded_dwords, polls;
    uint8_t protected_mode, security_key, started;
};
enum polaris_smu_load_result {
    POLARIS_SMU_LOAD_OK = 0, POLARIS_SMU_LOAD_INVALID = -1,
    POLARIS_SMU_LOAD_BUSY = -2, POLARIS_SMU_LOAD_IO = -3,
    POLARIS_SMU_LOAD_TIMEOUT = -4, POLARIS_SMU_LOAD_UNSUPPORTED = -5,
    POLARIS_SMU_LOAD_REJECTED = -6, POLARIS_SMU_LOAD_QUARANTINED = -7
};
/* One zero-initialized context owns a physical PF for its lifetime. Caller
 * holds exclusive GPU ownership (including indirect port 11) and keeps all
 * allocations alive even on failure. No software reset clears quarantine.
 * Context, ops, request, image descriptors and borrowed firmware must be
 * mutually disjoint stable CPU objects, also disjoint from mapped outputs.
 * The parsed SMC view must originate from a provenance-checked correct chip/
 * revision/key file; a header cannot authenticate firmware. TOC/image/scratch
 * mappings must be established and CPU visible before calling. resolve_mapping
 * must inspect actual mappings; sync flushes cache/HDP and orders GPU reads.
 * All callbacks are bounded. This executes the register protocol: it is not a
 * simulator or a readiness override. Cold boot follows both protected and
 * non-protected Polaris10 branches, without changing power/voltage policies.
 * Every wait is capped at 100000 polls/100 ms; whole operation at 2 seconds.
 * Success requires fresh ACKs and the complete SRAM load mask, not just ACK.
 * The directory must include MEC JT1/JT2 even though 0x47e omits their bits.
 */
int polaris_smu_load(struct polaris_smu_loader *loader,
                     const struct polaris_smu_loader_ops *ops,
                     const struct polaris_smu_load_request *request);
#endif
