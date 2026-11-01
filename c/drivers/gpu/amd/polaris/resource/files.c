#include <string.h>
#include "amd/polaris/resource/files.h"
#include "bootmod.h"
#include "crypto.h"
#include "vfs.h"
#include "kheap.h"
#include "polaris_fw_pin.h"

#define FIRMWARE_DIRECTORY "/lib/firmware/amdgpu/"
#define GROUP_OTHER_WRITE 0022u

/* The pin table every file is checked against. Production reads the generated
 * table directly. A host gate that feeds synthetic blobs through this code (the
 * real firmware is not redistributable test data) defines
 * POLARIS_FW_PIN_HOST_OVERRIDE and points these at digests of its own blobs;
 * the kernel build never defines it, so there the table is read-only. */
#ifdef POLARIS_FW_PIN_HOST_OVERRIDE
const struct polaris_fw_pin *polaris_fw_pin_table = polaris_fw_pin_generated;
unsigned polaris_fw_pin_table_count = POLARIS_FW_PIN_GENERATED_COUNT;
#else
#define polaris_fw_pin_table polaris_fw_pin_generated
#define polaris_fw_pin_table_count POLARIS_FW_PIN_GENERATED_COUNT
#endif

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

/* The part of PATH after the firmware directory: the name the pin table and
 * the boot-module registry both use. */
static const char *firmware_name(const char *path)
{
    return path + sizeof(FIRMWARE_DIRECTORY) - 1;
}

/* True when DATA hashes to the pin recorded for NAME. A name with no pin fails
 * closed: a file nobody vouched for must not reach the GPU just because the
 * table forgot it. FWWIRE_NEGCTL_NO_PIN skips the comparison so the host gate
 * can watch its tamper checks go red without it. */
static int pin_matches(const char *name, const void *data, size_t bytes)
{
#ifdef FWWIRE_NEGCTL_NO_PIN
    (void)name;
    (void)data;
    (void)bytes;
    return 1;
#else
    for (unsigned i = 0; i < polaris_fw_pin_table_count; ++i) {
        if (strcmp(polaris_fw_pin_table[i].name, name)) {
            continue;
        }
        uint8_t digest[32];
        sha256(data, bytes, digest);
        return !memcmp(digest, polaris_fw_pin_table[i].sha256, sizeof digest);
    }
    return 0;
#endif
}

/* Format-check then pin-check a heap copy of a candidate file, and either hand
 * the buffer to OUT or free it. Shared by both sources so a boot module gets
 * exactly the scrutiny a rootfs file does. Format first: the parser is the
 * bounds-checked reader the kernel already trusts with arbitrary bytes, and a
 * structurally broken file reports FORMAT, which says more than a bare digest
 * mismatch would. */
static int accept_buffer(void *data, size_t bytes, const char *name,
                         enum polaris_fw_kind kind, struct polaris_fw_blob *out)
{
    struct polaris_fw_view view;
    if (polaris_fw_parse(kind, data, bytes, &view)) {
        kfree(data);
        return POLARIS_RESOURCE_FILE_FORMAT;
    }
    if (!pin_matches(name, data, bytes)) {
        kfree(data);
        return POLARIS_RESOURCE_FILE_PIN;
    }
    *out = (struct polaris_fw_blob){data, bytes};
    return POLARIS_RESOURCE_FILE_OK;
}

static int load_from_rootfs(const char *path, enum polaris_fw_kind kind,
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
    return accept_buffer(data, bytes, firmware_name(path), kind, out);
}

/* Copy the boot module into the heap rather than lending out its pages: the
 * release path frees every blob with kfree, the module is the loader's memory,
 * and a private copy is also what the digest is taken over, so nothing can
 * change between the check and the use. Returns MISSING when no module of this
 * name is registered, which lets the caller keep the rootfs result. */
static int load_from_bootmod(const char *name, enum polaris_fw_kind kind,
                             struct polaris_fw_blob *out)
{
    uint64_t module_bytes = 0;
    const void *module = bootmod_find(name, &module_bytes);
    if (!module) {
        return POLARIS_RESOURCE_FILE_MISSING;
    }
    if (!module_bytes || module_bytes > POLARIS_RESOURCE_FILE_LIMIT) {
        return POLARIS_RESOURCE_FILE_METADATA;
    }
    void *data = kmalloc((size_t)module_bytes);
    if (!data) {
        return POLARIS_RESOURCE_FILE_ALLOC;
    }
    memcpy(data, module, (size_t)module_bytes);
    return accept_buffer(data, (size_t)module_bytes, name, kind, out);
}

/* Root disk first, boot module second. The disk is what a normal install
 * ships; the module is what a USB stick carries when the disk has no firmware
 * or a damaged copy (a stale rootfs file must not strand a machine whose stick
 * holds the right one). If the module is absent too, the ROOTFS result stands:
 * it is the more specific diagnosis ("missing" vs "pin mismatch"). */
static int load_file(const char *path, enum polaris_fw_kind kind,
                     struct polaris_fw_blob *out,
                     enum polaris_resource_source *source)
{
    int result = load_from_rootfs(path, kind, out);
    if (!result) {
        *source = POLARIS_RESOURCE_SOURCE_ROOTFS;
        return result;
    }
    int module_result = load_from_bootmod(firmware_name(path), kind, out);
    if (module_result == POLARIS_RESOURCE_FILE_MISSING) {
        return result;
    }
    if (!module_result) {
        *source = POLARIS_RESOURCE_SOURCE_BOOTMOD;
    }
    return module_result;
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
            (enum polaris_fw_kind)kind, file, &set->source[kind]);
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
