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
    POLARIS_RESOURCE_FILE_FORMAT = -5
};

struct polaris_resource_files {
    struct polaris_fw_sources firmware;
    struct polaris_fw_blob smc;
    unsigned valid_mask;
    int result[POLARIS_RESOURCE_FILE_SLOTS];
};

/* Names follow Linux v6.12 amdgpu_cgs.c and ASICID_IS_P20/P30 for 1002:67df.
 * key must be the actual SMU security selection (0 or 1), not PCI revision.
 * Revision alone selects only the appropriate key=1 candidate. */
const char *polaris_resource_file_path(enum polaris_fw_kind kind,
                                      uint8_t revision, unsigned key);

/* Read the installed kernel firmware directory through VFS. Files must be
 * root-owned regular files, not writable by group/other. These restrictions
 * retain the installed image's trust boundary; format checking is neither a
 * signature nor authentication. No file is downloaded or synthesized.
 *
 * Run during single-threaded boot after VFS mounts. The caller supplies a
 * zero-initialized, empty set. Partial successes remain owned by the set;
 * always release it, including failure. Bytes may feed runtime_start while
 * held, then be released after that synchronous call finishes. This function
 * neither selects a GPU arena nor invokes a hardware callback. */
int polaris_resource_files_load(uint8_t revision, unsigned key,
                                struct polaris_resource_files *);
void polaris_resource_files_release(struct polaris_resource_files *);

#endif
