#ifndef LOGIT_POLARIS_DESKTOP_H
#define LOGIT_POLARIS_DESKTOP_H
#include "amd/polaris/native.h"
#include "amd/polaris/runtime.h"

struct polaris_desktop_info {
    enum polaris_runtime_stage stage, failed_stage;
    int error;
    unsigned quarantined;
    uint64_t submitted, completed, frames, pixels, uploaded_bytes;
};
/* Product entry once the platform owns the PCI PF, knows ALL firmware VRAM
 * reservations and has mapped BARs UC. This function takes fb's graphics lock,
 * verifies the existing GOP/Multiboot surface against those native resources,
 * runs the complete driver and installs the fb hook ONLY after both canaries.
 * It does not invent the missing ownership record from the size of a BAR.
 * Resource mappings/opaque storage remain live permanently, including failure.
 * No reset/retry API is exposed. Boot discovery without such a resource record
 * deliberately continues through device.c's read-only path. */
int polaris_desktop_start(const struct polaris_native_resources *,
                           const struct polaris_runtime_request *);
int polaris_desktop_query(struct polaris_desktop_info *);
#endif
