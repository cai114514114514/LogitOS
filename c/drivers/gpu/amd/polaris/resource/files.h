#ifndef LOGIT_POLARIS_RESOURCE_FILES_H
#define LOGIT_POLARIS_RESOURCE_FILES_H

#include "amd/polaris/firmware/bundle.h"

#define POLARIS_RESOURCE_FILE_LIMIT (2u * 1024u * 1024u)
#define POLARIS_RESOURCE_FILE_SLOTS (POLARIS_FW_FILE_COUNT + 1u)
#define POLARIS_RESOURCE_ALL_FILES ((1u << POLARIS_RESOURCE_FILE_SLOTS) - 1u)

enum polaris_resource_file_result {
    POLARIS_RESOURCE_FILE_OK = 0,
    POLARIS_RESOURCE_FILE_MISSING = -1,
    POLARIS_RESOURCE_FILE_METADATA = -2,
    POLARIS_RESOURCE_FILE_ALLOC = -3,
    POLARIS_RESOURCE_FILE_READ = -4,
    POLARIS_RESOURCE_FILE_FORMAT = -5,
    POLARIS_RESOURCE_FILE_PIN = -6
};

/* Where an accepted file's bytes came from. NONE for a slot that failed. */
enum polaris_resource_source {
    POLARIS_RESOURCE_SOURCE_NONE = 0,
    POLARIS_RESOURCE_SOURCE_ROOTFS = 1,
    POLARIS_RESOURCE_SOURCE_BOOTMOD = 2
};

/* One pinned firmware file: the SHA-256 the vendored linux-firmware copy has.
 * The table is generated from third_party/amd-firmware/MANIFEST at build time
 * (tools/gpu/amd/fwpin.py), so the pin cannot drift from the recorded commit. */
struct polaris_fw_pin {
    const char *name;
    uint8_t sha256[32];
};

struct polaris_resource_files {
    struct polaris_fw_sources firmware;
    struct polaris_fw_blob smc;
    unsigned valid_mask;
    int result[POLARIS_RESOURCE_FILE_SLOTS];
    enum polaris_resource_source source[POLARIS_RESOURCE_FILE_SLOTS];
};

#ifdef POLARIS_FW_PIN_HOST_OVERRIDE
/* Host gates only (see files.c): the table the pin check reads. */
extern const struct polaris_fw_pin *polaris_fw_pin_table;
extern unsigned polaris_fw_pin_table_count;
#endif

/* Names follow Linux v6.12 amdgpu_cgs.c, which picks the SMU file from
 * ASICID_IS_P20/P30 (amdgpu.h): device ID and revision together, so 6fdf rev
 * ef gets polaris10_k_smc.bin like 67df rev ef. key must be the actual SMU
 * security selection (0 or 1), not PCI revision. */
const char *polaris_resource_file_path(enum polaris_fw_kind kind, uint16_t device,
                                      uint8_t revision, unsigned key);

/* Read the installed kernel firmware directory through VFS, and fall back to
 * the boot module of the same name (c/kernel/init/bootmod.c, delivered by the
 * USB ESP) for any file VFS cannot supply. Rootfs files must be root-owned
 * regular files, not writable by group/other. Whichever way a file arrives,
 * its SHA-256 must equal the pin generated from third_party/amd-firmware/
 * MANIFEST, else the slot fails with POLARIS_RESOURCE_FILE_PIN: format
 * checking alone is neither a signature nor authentication. The pin is an
 * integrity check against the vendored linux-firmware copy, not a vendor
 * signature. No file is downloaded or synthesized.
 *
 * Run during single-threaded boot after VFS mounts. The caller supplies a
 * zero-initialized, empty set. Partial successes remain owned by the set;
 * always release it, including failure. Bytes may feed runtime_start while
 * held, then be released after that synchronous call finishes. This function
 * neither selects a GPU arena nor invokes a hardware callback. */
int polaris_resource_files_load(uint16_t device, uint8_t revision, unsigned key,
                                struct polaris_resource_files *);
void polaris_resource_files_release(struct polaris_resource_files *);

#endif
