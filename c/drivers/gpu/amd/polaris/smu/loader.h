#ifndef LOGIT_POLARIS_SMU_LOADER_H
#define LOGIT_POLARIS_SMU_LOADER_H
#include "amd/polaris/firmware/bundle.h"

/* Which start-up sequence the loader picked after reading the SMC state, in the
 * words of polaris10_start_smu() (Linux v6.12 polaris10_smumgr.c): the SMC RAM
 * already running (smu7_is_smc_ram_running), or a cold start through the
 * protection-mode or the non-protection-mode sequence, chosen by
 * SMU_FIRMWARE.SMU_MODE. REFUSED means the registers did not describe a state
 * the loader is willing to touch. */
enum polaris_smu_start_branch {
    POLARIS_SMU_BRANCH_NONE = 0,
    POLARIS_SMU_BRANCH_REFUSED,
    POLARIS_SMU_BRANCH_WARM,
    POLARIS_SMU_BRANCH_COLD_PROTECTED,
    POLARIS_SMU_BRANCH_COLD_UNPROTECTED
};

/* The five SMC registers the loader reads before it decides, exactly as read,
 * and the decision. Linux v6.12 smu_7_1_3_d.h names: ixSMU_FIRMWARE
 * (0xe00030a4), ixSMC_SYSCON_CLOCK_CNTL_0 (0x80000004, bit 0 ck_disable),
 * ixSMC_PC_C (0x80000370), ixSMC_SYSCON_RESET_CNTL (0x80000000, bit 0 rst_reg),
 * ixFIRMWARE_FLAGS (0x3f000, bit 0 INTERRUPTS_ENABLED). */
struct polaris_smu_preflight {
    uint32_t smu_firmware;
    uint32_t clock_control;
    uint32_t program_counter;
    uint32_t reset_control;
    uint32_t firmware_flags;
    enum polaris_smu_start_branch branch;
};

struct polaris_smu_loader_ops {
    void *opaque;
    int (*read_identity)(void *opaque, uint32_t *vendor_device);
    int (*read_reg)(void *opaque, uint32_t byte_offset, uint32_t *value);
    int (*write_reg)(void *opaque, uint32_t byte_offset, uint32_t value);
    int (*resolve_mapping)(void *opaque, uint64_t gpu_address, uint64_t bytes,
                           volatile uint8_t **cpu);
    int (*sync_to_device)(void *opaque, uint64_t gpu_address, uint64_t bytes);
    uint64_t (*now_us)(void *opaque);
    /* Optional (may be NULL). Called exactly once per load, after the SMC
     * registers were read and the branch was chosen and BEFORE the loader
     * changes any SMC or mailbox state, so a caller that logs it leaves
     * evidence even if the sequence then hangs. */
    void (*report_preflight)(void *opaque, const struct polaris_smu_preflight *preflight);
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
    /* What was seen and how long the slow part took; see polaris_smu_load. */
    struct polaris_smu_preflight preflight;
    unsigned preflight_valid;
    uint64_t upload_microseconds;
    /* Messages whose previous mailbox response was not 1, and the first such
     * response (0 when the wait for it timed out). Linux logs and goes on. */
    uint32_t previous_response_anomalies, previous_response_first;
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
 * Every wait is capped at 1 s (Linux hwmgr->usec_timeout) and 1000000 polls;
 * the whole operation at 10 seconds. The 10 s limit does not run while the SMC
 * is held in reset (from asserting reset to releasing it, which includes the
 * firmware upload): stopping there would leave a half-written image behind.
 * The time spent in that window is not charged to the limit and is reported in
 * upload_microseconds.
 * A mailbox message first waits for the previous response and logs, but does
 * not fail on, a response other than 1 or a timeout of that wait (Linux v6.12
 * smu7_send_msg_to_smc); only the response to the message itself decides.
 * Success requires fresh ACKs and the complete SRAM load mask, not just ACK.
 * The directory must include MEC JT1/JT2 even though 0x47e omits their bits.
 */
int polaris_smu_load(struct polaris_smu_loader *loader,
                     const struct polaris_smu_loader_ops *ops,
                     const struct polaris_smu_load_request *request);
#endif
