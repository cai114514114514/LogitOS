#ifndef LOGIT_POLARIS_RUNTIME_H
#define LOGIT_POLARIS_RUNTIME_H
#include "amd/polaris/platform.h"
#include "amd/polaris/memory/layout.h"
#include "amd/polaris/sdma/engine.h"
#include "amd/polaris/present.h"

enum polaris_runtime_stage {
    POLARIS_RUNTIME_OFF, POLARIS_RUNTIME_MEMORY, POLARIS_RUNTIME_FIRMWARE,
    POLARIS_RUNTIME_SMU, POLARIS_RUNTIME_RING, POLARIS_RUNTIME_ACTIVE,
    POLARIS_RUNTIME_FAILED
};
struct polaris_runtime_request {
    struct polaris_memory_request memory;
    struct polaris_fw_sources firmware;
    struct polaris_fw_blob smc;
    unsigned smc_security_key;
    uint32_t width, height, pitch;
    uint8_t *workspace; /* Ordinary RAM; >= firmware bundle_plan.bytes_used. */
    size_t workspace_bytes;
};
struct polaris_runtime {
    unsigned lock, attempted, quarantined;
    enum polaris_runtime_stage stage, failed_stage;
    int error;
    struct polaris_platform platform;
    struct polaris_memory_layout memory;
    struct polaris_fw_bundle_info firmware;
    struct polaris_smu_loader smu;
    struct polaris_sdma_engine engine;
    struct polaris_sdma_queue queue;
    struct polaris_present present;
};
/* The end-to-end driver entry for an exclusively owned physical Polaris10 PF.
 * Platform supplies an actually reserved CPU-visible arena and firmware files
 * with provenance, not a guessed free BAR tail. firmware_bytes is computed
 * from the real bundle; staging_bytes must be >=8192. Existing display timings
 * and MC/GART mappings are preserved: this is linear SDMA presentation, not
 * shader/3D, modesetting, DPM, or a recovery/reset driver.
 * Zero initialize once. Hold the same device/front-buffer lease throughout
 * start and presentation; callbacks and DMA allocations outlive this context.
 * No host substitute exists in production. ACTIVE requires real fresh SMU
 * load status, ring readbacks AND GPU fill/copy/readback via these callbacks.
 * A start failure after attempted is sticky; it does not revoke pending DMA.
 * Workspace and firmware input bytes may be released after start returns;
 * device allocations and the platform opaque context must remain allocated.
 */
int polaris_runtime_start(struct polaris_runtime *,
                           const struct polaris_platform *,
                           const struct polaris_runtime_request *);
int polaris_runtime_present(struct polaris_runtime *, const uint32_t *pixels,
                             uint64_t bytes, uint32_t stride,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
