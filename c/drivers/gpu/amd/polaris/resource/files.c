#include "amd/polaris/resource/files.h"
#include "vfs.h"
#include "kheap.h"

#define FIRMWARE_DIRECTORY "/lib/firmware/amdgpu/"
#define GROUP_OTHER_WRITE 0022u

static int is_polaris20(uint8_t revision)
{
    return revision == 0xe3 || revision == 0xe4 || revision == 0xe5 ||
           revision == 0xe7 || revision == 0xef;
}

const char *polaris_resource_file_path(enum polaris_fw_kind kind,
                                      uint8_t revision, unsigned key)
{
    static const char *const paths[POLARIS_FW_FILE_COUNT] = {
        FIRMWARE_DIRECTORY "polaris10_sdma.bin",
        FIRMWARE_DIRECTORY "polaris10_sdma1.bin",
        FIRMWARE_DIRECTORY "polaris10_ce.bin",
        FIRMWARE_DIRECTORY "polaris10_pfp.bin",
        FIRMWARE_DIRECTORY "polaris10_me.bin",
        FIRMWARE_DIRECTORY "polaris10_mec.bin",
        FIRMWARE_DIRECTORY "polaris10_rlc.bin"
    };
    if (kind < POLARIS_FW_SDMA0 || kind > POLARIS_FW_SMC || key > 1) {
        return NULL;
    }
    if (kind != POLARIS_FW_SMC) {
        return paths[kind];
    }
    if (!key) {
        return FIRMWARE_DIRECTORY "polaris10_smc_sk.bin";
    }
    if (is_polaris20(revision)) {
        return FIRMWARE_DIRECTORY "polaris10_k_smc.bin";
    }
    if (revision == 0xe1 || revision == 0xf7) {
        return FIRMWARE_DIRECTORY "polaris10_k2_smc.bin";
    }
    return FIRMWARE_DIRECTORY "polaris10_smc.bin";
}

static int load_file(const char *path, enum polaris_fw_kind kind,
                     struct polaris_fw_blob *out)
{
    struct vattr attributes = {0};
    if (vfs_stat(path, &attributes)) {
        return POLARIS_RESOURCE_FILE_MISSING;
    }
    if (attributes.type != VT_REG || attributes.uid != 0 ||
        (attributes.mode & GROUP_OTHER_WRITE) || !attributes.size ||
        attributes.size > POLARIS_RESOURCE_FILE_LIMIT) {
        return POLARIS_RESOURCE_FILE_METADATA;
    }

    /* One extra byte detects a file that grew since stat. With the bounded
     * read, neither a changing length nor a forged header grows this buffer. */
    size_t bytes = (size_t)attributes.size;
    void *data = kmalloc(bytes + 1);
    if (!data) {
        return POLARIS_RESOURCE_FILE_ALLOC;
    }
    int read_bytes = vfs_read(path, data, (int)(bytes + 1));
    if (read_bytes < 0 || (size_t)read_bytes != bytes) {
        kfree(data);
        return POLARIS_RESOURCE_FILE_READ;
    }

    struct polaris_fw_view view;
    if (polaris_fw_parse(kind, data, bytes, &view)) {
        kfree(data);
        return POLARIS_RESOURCE_FILE_FORMAT;
    }
    *out = (struct polaris_fw_blob){data, bytes};
    return POLARIS_RESOURCE_FILE_OK;
}

int polaris_resource_files_load(uint8_t revision, unsigned key,
                                struct polaris_resource_files *set)
{
    if (!set || key > 1 || set->valid_mask || set->smc.data || set->smc.bytes) {
        return -1;
    }
    for (unsigned kind = 0; kind < POLARIS_FW_FILE_COUNT; ++kind) {
        if (set->firmware.file[kind].data || set->firmware.file[kind].bytes) {
            return -1;
        }
    }
    for (unsigned kind = 0; kind < POLARIS_RESOURCE_FILE_SLOTS; ++kind) {
        struct polaris_fw_blob *file = kind == POLARIS_FW_SMC ? &set->smc :
                                       &set->firmware.file[kind];
        set->result[kind] = load_file(polaris_resource_file_path(
            (enum polaris_fw_kind)kind, revision, key),
            (enum polaris_fw_kind)kind, file);
        if (!set->result[kind]) {
            set->valid_mask |= 1u << kind;
        }
    }
    return set->valid_mask == POLARIS_RESOURCE_ALL_FILES ? 0 : -1;
}

void polaris_resource_files_release(struct polaris_resource_files *set)
{
    if (!set) {
        return;
    }
    for (unsigned kind = 0; kind < POLARIS_FW_FILE_COUNT; ++kind) {
        kfree((void *)set->firmware.file[kind].data);
    }
    kfree((void *)set->smc.data);
    *set = (struct polaris_resource_files){0};
}
