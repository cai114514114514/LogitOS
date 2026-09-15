#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd/polaris/resource/device.h"
#include "driver.h"
#include "pci.h"
#include "vfs.h"

static unsigned checks;
static unsigned failures;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        printf("FAIL line %u: %s\n", (unsigned)__LINE__, #expression); \
    } \
} while (0)

/* Literal file/ROM/register fixtures are independent of production encoders.
 * Firmware payloads are synthetic test data, never hardware executable code. */
static const char *const file_paths[9] = {
    "/lib/firmware/amdgpu/polaris10_sdma.bin",
    "/lib/firmware/amdgpu/polaris10_sdma1.bin",
    "/lib/firmware/amdgpu/polaris10_ce.bin",
    "/lib/firmware/amdgpu/polaris10_pfp.bin",
    "/lib/firmware/amdgpu/polaris10_me.bin",
    "/lib/firmware/amdgpu/polaris10_mec.bin",
    "/lib/firmware/amdgpu/polaris10_rlc.bin",
    "/lib/firmware/amdgpu/polaris10_smc_sk.bin",
    "/lib/firmware/amdgpu/polaris10_k_smc.bin"
};
static uint8_t file_data[9][512];
static uint8_t vram_copy[256 * 1024];
static uint32_t registers[65536 / 4];
static uint32_t configuration[256 / 4];
static struct device gpu;
static unsigned missing_files;
static unsigned bad_metadata;
static unsigned bad_reads;
static unsigned allocations;
static unsigned frees;
static unsigned allocation_attempts;
static unsigned fail_allocation;
static unsigned map_count;
static unsigned file_stats;
static unsigned file_reads;
static int override_metadata;
static struct vattr metadata;
static int bad_read_delta;

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
}

static void make_vbios(void)
{
    memset(vram_copy, 0, sizeof vram_copy);
    vram_copy[0] = 0x55;
    vram_copy[1] = 0xaa;
    vram_copy[2] = 4;
    write_le16(vram_copy + 0x18, 0x50);
    write_le16(vram_copy + 0x48, 0x80);
    memcpy(vram_copy + 0x50, "PCIR", 4);
    write_le16(vram_copy + 0x54, 0x1002);
    write_le16(vram_copy + 0x56, 0x67df);
    write_le16(vram_copy + 0x5a, 24);
    write_le16(vram_copy + 0x80, 36);
    vram_copy[0x82] = 1;
    vram_copy[0x83] = 1;
    memcpy(vram_copy + 0x84, "ATOM", 4);
    write_le16(vram_copy + 0xa0, 0xc0);
    write_le16(vram_copy + 0xc0, 28);
    vram_copy[0xc2] = 1;
    vram_copy[0xc3] = 1;
    write_le16(vram_copy + 0xda, 0x100);
    write_le16(vram_copy + 0x100, 12);
    vram_copy[0x102] = 1;
    vram_copy[0x103] = 5;
    write_le32(vram_copy + 0x104, 4096);
    write_le16(vram_copy + 0x108, 64);
    write_le16(vram_copy + 0x10a, 8);
}

static void make_files(void)
{
    memset(file_data, 0, sizeof file_data);
    for (unsigned file = 0; file < 9; ++file) {
        uint8_t *bytes = file_data[file];
        write_le32(bytes, 512);
        write_le32(bytes + 4, file < 2 ? 52 : file == 6 ? 104 : file >= 7 ? 36 : 44);
        bytes[8] = file == 6 ? 2 : 1;
        bytes[10] = file < 2 ? 1 : 0;
        bytes[12] = file < 2 ? 3 : file >= 7 ? 7 : 8;
        bytes[14] = file < 2 ? 1 : file >= 7 ? 2 : 0;
        write_le32(bytes + 16, 100 + file);
        write_le32(bytes + 20, 64);
        write_le32(bytes + 24, 256);
        if (file == 5) {
            write_le32(bytes + 36, 8);
            write_le32(bytes + 40, 4);
        }
        if (file >= 7) write_le32(bytes + 32, 0x20000);
        for (unsigned offset = 256; offset < 512; ++offset)
            bytes[offset] = (uint8_t)(offset + file * 19);
    }
}

static void reset(void)
{
    CHECK(allocations == frees);
    allocations = 0;
    frees = 0;
    allocation_attempts = 0;
    fail_allocation = 0;
    missing_files = 0;
    bad_metadata = 0;
    bad_reads = 0;
    map_count = 0;
    file_stats = 0;
    file_reads = 0;
    override_metadata = 0;
    bad_read_delta = -1;
    make_vbios();
    make_files();
    memset(configuration, 0, sizeof configuration);
    memset(registers, 0, sizeof registers);
    configuration[0] = 0x67df1002;
    configuration[1] = 6;
    configuration[2] = 0x030000e7;
    configuration[4] = 0x80000008;
    configuration[9] = 0xa0000000;
    registers[0x5428 / 4] = 8192;
    registers[0x2024 / 4] = 0x13ff1200;
    gpu = (struct device){.bus_type = DEV_BUS_PCI, .vendor = 0x1002,
        .device = 0x67df, .class_code = 3};
    gpu.res[0] = (struct dev_resource){.start = 0x80000000,
        .size = 0x10000000, .flags = DEV_RES_MEM};
    gpu.res[5] = (struct dev_resource){.start = 0xa0000000,
        .size = 65536, .flags = DEV_RES_MEM};
}

uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset)
{
    CHECK(bus == 0 && slot == 0 && function == 0 && !(offset & 3) && offset < 256);
    return configuration[offset / 4];
}

uint64_t dev_bar_map(struct device *device, int bar)
{
    ++map_count;
    CHECK(device == &gpu && (bar == 0 || bar == 5));
    return (uintptr_t)(bar == 0 ? (void *)vram_copy : (void *)registers);
}

void *kmalloc(size_t bytes)
{
    ++allocation_attempts;
    if (fail_allocation == allocation_attempts) return NULL;
    void *result = malloc(bytes);
    if (result) ++allocations;
    return result;
}

void kfree(void *memory)
{
    if (memory) ++frees;
    free(memory);
}

static int file_index(const char *path)
{
    for (unsigned file = 0; file < 9; ++file)
        if (!strcmp(path, file_paths[file])) return (int)file;
    return -1;
}

int vfs_stat(const char *path, struct vattr *attributes)
{
    ++file_stats;
    int file = file_index(path);
    if (file < 0 || (missing_files & (1u << file))) return -1;
    *attributes = (struct vattr){.type = VT_REG, .mode = 0644, .size = 512};
    if (override_metadata) {
        *attributes = metadata;
    }
    if (bad_metadata & (1u << file)) attributes->mode = 0666;
    return 0;
}

int vfs_read(const char *path, void *buffer, int capacity)
{
    ++file_reads;
    int file = file_index(path);
    if (file < 0 || (missing_files & (1u << file))) return -1;
    CHECK(capacity == 513);
    memcpy(buffer, file_data[file], 512);
    return (bad_reads & (1u << file)) ? 512 + bad_read_delta : 512;
}

static int parse_atom(struct polaris_atom_reservations *out)
{
    return polaris_atom_reservations_parse(vram_copy, sizeof vram_copy,
        (struct polaris_memory_range){UINT64_C(0x1200000000), UINT64_C(0x200000000)},
        0x10000000, out);
}

static void atom_cases(void)
{
    reset();
    struct polaris_atom_reservations result;
    CHECK(parse_atom(&result) == 0);
    CHECK(result.firmware.base == UINT64_C(0x1200400000));
    CHECK(result.firmware.bytes == 65536);
    CHECK(result.driver_scratch.base == UINT64_C(0x12003fe000));
    CHECK(result.driver_scratch.bytes == 8192);
    CHECK(result.table_revision == 5 && !result.driver_allocation_required);

    /* Failed parsing preserves caller state for every truncated input size. */
    for (size_t bytes = 0; bytes < 2048; ++bytes) {
        struct polaris_atom_reservations saved = result;
        CHECK(polaris_atom_reservations_parse(vram_copy, bytes,
            (struct polaris_memory_range){0, UINT64_C(0x200000000)},
            0x10000000, &result) < 0);
        CHECK(!memcmp(&result, &saved, sizeof result));
    }
    write_le16(vram_copy + 0x56, 0x67ef);
    CHECK(parse_atom(&result) == POLARIS_ATOM_WRONG_DEVICE);
    make_vbios();
    vram_copy[0x82] = 2;
    CHECK(parse_atom(&result) == POLARIS_ATOM_BAD_IMAGE);
    write_le16(vram_copy + 0x80, 40);
    CHECK(parse_atom(&result) == 0);
    make_vbios();
    write_le16(vram_copy + 0xda, 0);
    CHECK(parse_atom(&result) == POLARIS_ATOM_NO_USAGE_TABLE);
    make_vbios();
    vram_copy[0x103] = 3;
    CHECK(parse_atom(&result) == POLARIS_ATOM_UNSUPPORTED_TABLE);
    vram_copy[0x103] = 4;
    CHECK(parse_atom(&result) == POLARIS_ATOM_UNSUPPORTED_TABLE);
    write_le16(vram_copy + 0x10a, 0);
    CHECK(parse_atom(&result) == 0 && result.driver_scratch.bytes == 0);
    write_le32(vram_copy + 0x104, 0x80001000);
    CHECK(parse_atom(&result) == POLARIS_ATOM_UNSUPPORTED_TABLE);
    write_le32(vram_copy + 0x104, 0x40001000);
    CHECK(parse_atom(&result) == 0 && result.firmware.bytes == 0);
    make_vbios();
    write_le32(vram_copy + 0x104, 0x00800000);
    CHECK(parse_atom(&result) == POLARIS_ATOM_BAD_RESERVATION);
    write_le32(vram_copy + 0x104, 4);
    CHECK(parse_atom(&result) == POLARIS_ATOM_BAD_RESERVATION);
    write_le32(vram_copy + 0x104, 0);
    CHECK(parse_atom(&result) == 0 && result.driver_allocation_required == 1);
    CHECK(result.driver_scratch.base == UINT64_C(0x120fffe000));
    make_vbios();
    CHECK(polaris_atom_reservations_parse(vram_copy, sizeof vram_copy,
        (struct polaris_memory_range){0, 0x10000000}, 0x20000000, &result) < 0);
    CHECK(polaris_atom_reservations_parse(vram_copy, sizeof vram_copy,
        (struct polaris_memory_range){0, 0x10000000}, 0x10000000,
        (void *)(vram_copy + 0x100)) < 0);
}

static void firmware_cases(void)
{
    reset();
    CHECK(!strcmp(polaris_resource_file_path(POLARIS_FW_SMC, 0xe7, 1), file_paths[8]));
    CHECK(!strcmp(polaris_resource_file_path(POLARIS_FW_SMC, 0xe1, 1),
        "/lib/firmware/amdgpu/polaris10_k2_smc.bin"));
    CHECK(!strcmp(polaris_resource_file_path(POLARIS_FW_SMC, 0xc7, 1),
        "/lib/firmware/amdgpu/polaris10_smc.bin"));
    CHECK(!strcmp(polaris_resource_file_path(POLARIS_FW_SMC, 0xe7, 0), file_paths[7]));
    CHECK(polaris_resource_file_path(POLARIS_FW_SMC, 0xe7, 2) == NULL);
    struct polaris_resource_files files = {0};
    CHECK(polaris_resource_files_load(0xe7, 1, &files) == 0);
    CHECK(files.valid_mask == 0xff && allocations == 8);
    CHECK(!memcmp(files.smc.data, file_data[8], 512));
    struct polaris_fw_bundle_info bundle;
    CHECK(polaris_fw_bundle_plan(&files.firmware, 0, &bundle) == 0);
    CHECK(bundle.inventory.present_mask == 0x5fe);
    CHECK(polaris_resource_files_load(0xe7, 1, &files) < 0);
    polaris_resource_files_release(&files);
    CHECK(allocations == frees && !files.valid_mask);
    missing_files = 1u << 0;
    bad_metadata = 1u << 1;
    bad_reads = 1u << 2;
    file_data[3][12] = 9;
    CHECK(polaris_resource_files_load(0xe7, 1, &files) < 0);
    CHECK(files.valid_mask == 0xf0);
    CHECK(files.result[0] == POLARIS_RESOURCE_FILE_MISSING);
    CHECK(files.result[1] == POLARIS_RESOURCE_FILE_METADATA);
    CHECK(files.result[2] == POLARIS_RESOURCE_FILE_READ);
    CHECK(files.result[3] == POLARIS_RESOURCE_FILE_FORMAT);
    polaris_resource_files_release(&files);
    CHECK(allocations == frees);
    reset();
    override_metadata = 1;
    const struct vattr rejected_metadata[] = {
        {.type = VT_REG, .mode = 0644, .size = 0},
        {.type = VT_REG, .mode = 0644, .size = UINT64_MAX},
        {.type = VT_REG, .mode = 0644, .size = 2097153},
        {.type = VT_REG, .mode = 0644, .size = 512, .uid = 1000},
        {.type = VT_DIR, .mode = 0755, .size = 512}
    };
    for (unsigned i = 0; i < sizeof rejected_metadata / sizeof rejected_metadata[0]; ++i) {
        metadata = rejected_metadata[i];
        CHECK(polaris_resource_files_load(0xe7, 1, &files) < 0);
        CHECK(!files.valid_mask && allocation_attempts == 0 && file_reads == 0);
        polaris_resource_files_release(&files);
    }
    reset();
    bad_reads = 0x1ff;
    bad_read_delta = 1;
    CHECK(polaris_resource_files_load(0xe7, 1, &files) < 0);
    CHECK(!files.valid_mask && files.result[0] == POLARIS_RESOURCE_FILE_READ);
    polaris_resource_files_release(&files);
    CHECK(allocations == frees);
    reset();
    fail_allocation = 1;
    CHECK(polaris_resource_files_load(0xe7, 0, &files) < 0);
    CHECK(files.result[0] == POLARIS_RESOURCE_FILE_ALLOC && files.valid_mask == 0xfe);
    polaris_resource_files_release(&files);
    CHECK(allocations == frees);
}

static int probe(struct polaris_resource_report *report)
{
    return polaris_resources_probe_device(&gpu, 0x82000000, 640 * 480 * 4,
                                           640, 480, report);
}

static void device_cases(void)
{
    reset();
    uint32_t saved_registers[65536 / 4];
    memcpy(saved_registers, registers, sizeof registers);
    struct polaris_resource_report report;
    CHECK(probe(&report) == 0);
    CHECK(report.vbios_bytes_read == 262144 && report.atom_result == 0);
    CHECK(report.reservations.firmware.base == UINT64_C(0x1200400000));
    CHECK(report.reservations.firmware.bytes == 65536);
    CHECK(report.firmware_mask[0] == 0xff && report.firmware_mask[1] == 0xff);
    CHECK(report.ownership_missing == 1 && report.security_key_unknown == 1);
    CHECK(report.device.vram_bytes == UINT64_C(0x200000000));
    CHECK(file_stats == 16 && file_reads == 16 && map_count == 2);
    CHECK(allocations == frees);
    CHECK(!memcmp(saved_registers, registers, sizeof registers));

    reset();
    vram_copy[0] = 0;
    missing_files = (1u << 4) | (1u << 8);
    CHECK(probe(&report) == 0);
    CHECK(report.atom_result == POLARIS_ATOM_BAD_IMAGE);
    CHECK(report.firmware_mask[0] == 0xef && report.firmware_mask[1] == 0x6f);
    CHECK(report.ownership_missing == 1 && allocations == frees);

    /* Every failed preflight below must stop before mappings, allocations,
     * file reads or an indirect register selection can be issued. */
    static const unsigned bad_offsets[] = {0, 4, 8, 12, 16, 32, 36};
    static const uint32_t bad_values[] = {
        0x67ef1002, 0, 0x020000e7, 0x00010000, 0x90000008, UINT32_MAX, 0xb0000000
    };
    for (unsigned i = 0; i < sizeof bad_offsets / sizeof bad_offsets[0]; ++i) {
        reset();
        configuration[bad_offsets[i] / 4] = bad_values[i];
        CHECK(probe(&report) < 0);
        CHECK(map_count == 0 && allocation_attempts == 0 && file_stats == 0);
    }
    reset();
    configuration[1] |= 1u << 20;
    configuration[0x34 / 4] = 0x40;
    configuration[0x40 / 4] = 0x4001; /* PM capability loops to itself. */
    CHECK(probe(&report) < 0 && map_count == 0);
    configuration[0x40 / 4] = 1;
    configuration[0x44 / 4] = 3;
    CHECK(probe(&report) < 0 && map_count == 0);
    reset();
    gpu.seg = 1;
    CHECK(probe(&report) < 0 && map_count == 0);
    reset();
    CHECK(polaris_resources_probe_device(&gpu, 0x8ffff000, 640 * 480 * 4,
        640, 480, &report) < 0 && map_count == 0);
    reset();
    fail_allocation = 1; /* No VBIOS scratch: firmware inventory still runs. */
    CHECK(probe(&report) == 0);
    CHECK(report.vbios_bytes_read == 0 && report.atom_result < 0);
    CHECK(report.firmware_mask[0] == 0xff && report.firmware_mask[1] == 0xff);
    CHECK(allocations == frees);
}

int main(void)
{
    atom_cases();
    firmware_cases();
    device_cases();
    CHECK(allocations == frees);
    printf("POLARIS_RESOURCES: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
