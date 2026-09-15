#ifndef LOGIT_POLARIS_RESOURCE_ATOM_H
#define LOGIT_POLARIS_RESOURCE_ATOM_H

#include "amd/polaris/memory/layout.h"

#define POLARIS_VBIOS_COPY_BYTES (256u * 1024u)

enum polaris_atom_result {
    POLARIS_ATOM_OK = 0,
    POLARIS_ATOM_BAD_IMAGE = -1,
    POLARIS_ATOM_WRONG_DEVICE = -2,
    POLARIS_ATOM_NO_USAGE_TABLE = -3,
    POLARIS_ATOM_UNSUPPORTED_TABLE = -4,
    POLARIS_ATOM_BAD_RESERVATION = -5
};

struct polaris_atom_reservations {
    struct polaris_memory_range firmware;
    struct polaris_memory_range driver_scratch;
    unsigned table_revision;
    unsigned block_policy;
    unsigned driver_allocation_required;
};

/* Parse a copied, little-endian ATOM image for exactly 1002:67df. Both ranges
 * are GPU MC addresses. Zero-sized ranges are absent. Supports the legacy
 * VRAM_UsageByFirmware 1.4/1.5 tables used after RV770; older tables use a
 * different address unit and are deliberately rejected.
 *
 * The table is filled at VBIOS runtime. A valid ROM file, VFCT image or VRAM
 * copy alone cannot prove that it contains every current allocation. This
 * parser reports declared reservations; it NEVER grants ownership of the
 * remaining VRAM. Input and output must be stable and disjoint. Failure
 * preserves output; the image need not be aligned. */
int polaris_atom_reservations_parse(const void *image, size_t bytes,
                                    struct polaris_memory_range vram,
                                    uint64_t aperture_bytes,
                                    struct polaris_atom_reservations *out);
const char *polaris_atom_result_name(int result);

#endif
