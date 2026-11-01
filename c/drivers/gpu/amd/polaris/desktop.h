#ifndef LOGIT_POLARIS_DESKTOP_H
#define LOGIT_POLARIS_DESKTOP_H
#include "amd/polaris/native.h"
#include "amd/polaris/runtime.h"

struct polaris_desktop_info {
    enum polaris_runtime_stage stage, failed_stage;
    int error;
    unsigned quarantined;
    uint64_t submitted, completed, frames, pixels, uploaded_bytes;
    uint32_t smu_load_status; /* SRAM UcodeLoadStatus read back by the loader */
    /* How long the SMC firmware upload took (0 when the SMC was already
     * running), and how many mailbox messages found a previous response other
     * than 1 (smu/loader.h), the first such response being
     * smu_previous_response_first. */
    uint64_t smu_upload_microseconds;
    uint32_t smu_previous_response_anomalies;
    uint32_t smu_previous_response_first;
};

/* What a successful start leaves behind. PRESENT is the product behaviour: the
 * GPU takes over presenting the desktop and fb's native-present hook is
 * installed after both canaries pass. SELF_TEST runs exactly the same canaries
 * and then stops: the hook is never installed and the CPU keeps drawing the
 * screen, whatever the outcome. The first run on real hardware (stage B3 of
 * docs/superpowers/plans/2026-11-01-rx580-stage-b.md) must not be able to
 * change what the user sees. */
enum polaris_desktop_mode {
    POLARIS_DESKTOP_MODE_PRESENT = 0,
    POLARIS_DESKTOP_MODE_SELF_TEST
};

/* Return values of bind and run. Both carry polaris_runtime_start()'s own
 * contract: REFUSED means nothing was started and the CPU fallback is intact,
 * QUARANTINED means the GPU may be in an unknown state and must not be used
 * again. */
enum polaris_desktop_result {
    POLARIS_DESKTOP_OK = 0,
    POLARIS_DESKTOP_REFUSED = -1,
    POLARIS_DESKTOP_QUARANTINED = -2
};

/* Product entry once the platform owns the PCI PF, knows ALL firmware VRAM
 * reservations and has mapped BARs UC. This function takes fb's graphics lock,
 * verifies the existing GOP/Multiboot surface against those native resources,
 * runs the complete driver and installs the fb hook ONLY after both canaries.
 * It does not invent the missing ownership record from the size of a BAR.
 * Resource mappings/opaque storage remain live permanently, including failure.
 * No reset/retry API is exposed. Boot discovery without such a resource record
 * deliberately continues through device.c's read-only path. This is
 * polaris_desktop_bind() followed by polaris_desktop_run() in PRESENT mode. */
int polaris_desktop_start(const struct polaris_native_resources *,
                           const struct polaris_runtime_request *);

/* The two halves of polaris_desktop_start, for a caller that must do work with
 * the bound platform before the runtime starts (stage B3 checks the mapping
 * callback and reads the SMU security key, both of which need the live
 * platform, and the key decides which firmware file the request carries).
 *
 * bind: validates the resources against the boot surface and binds the native
 * platform (read-only: PCI config and BAR5 reads). REQUEST supplies only the
 * geometry and memory description here; its firmware fields are not looked at.
 * *platform is written on success and stays valid for the life of the system.
 * One bind per boot; a second call is refused.
 *
 * run: starts the runtime from a bound platform and, in PRESENT mode only,
 * installs the native-present hook after the runtime reports ACTIVE. PLATFORM
 * may be the platform bind returned or a wrapper around it; the runtime keeps
 * a copy of the struct, so any opaque pointer inside must outlive the system. */
int polaris_desktop_bind(const struct polaris_native_resources *,
                         const struct polaris_runtime_request *,
                         struct polaris_platform *platform);
int polaris_desktop_run(const struct polaris_platform *platform,
                        const struct polaris_runtime_request *,
                        enum polaris_desktop_mode);
int polaris_desktop_query(struct polaris_desktop_info *);
#endif
