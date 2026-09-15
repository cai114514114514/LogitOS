#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "nvidia_pascal_accel.h"

static int checks, failures;
static int measure_calls, map_calls, mmio_reads, mmio_writes;
static int fail_index = -1;
static enum nv1050_accel_blocker fail_reason;
static uint32_t fake_boot0 = (0x137u << 20) | 0xa1u;
static uint32_t fake_boot1;
static uint32_t fake_enable = 0x00000001u;
static uint64_t fake_map = 0xf6000000ull;

static void check(int yes, const char *what)
{
    checks++;
    if (!yes) { failures++; printf("FAIL: %s\n", what); }
}

static uint8_t nybble(char c)
{
    return c <= '9' ? (uint8_t)(c - '0') : (uint8_t)(c - 'a' + 10);
}

static int measure(void *ctx, const struct nv1050_fw_manifest_entry *entry,
                   uint32_t *size, uint8_t digest[32])
{
    (void)ctx;
    int index = measure_calls++;
    if (index == fail_index && fail_reason == NV1050_BLOCK_FW_MISSING)
        return NV1050_BLOCK_FW_MISSING;
    *size = entry->size;
    if (index == fail_index && fail_reason == NV1050_BLOCK_FW_SIZE)
        (*size)++;
    for (unsigned i = 0; i < 32; i++)
        digest[i] = (uint8_t)((nybble(entry->sha256_hex[i * 2]) << 4) |
                              nybble(entry->sha256_hex[i * 2 + 1]));
    if (index == fail_index && fail_reason == NV1050_BLOCK_FW_HASH)
        digest[0] ^= 0x80;
    return 0;
}

static uint64_t map_bar0(void *ctx, struct device *dev)
{
    (void)ctx; (void)dev;
    map_calls++;
    return fake_map;
}

static uint32_t read32(void *ctx, uint64_t base, uint32_t off)
{
    (void)ctx; (void)base;
    mmio_reads++;
    switch (off) {
    case 0x000000: return fake_boot0;
    case 0x000004: return fake_boot1;
    case 0x000200: return fake_enable;
    default: return 0;
    }
}

/* Link seams used only by nvidia_pascal_accel.c's production wrapper. */
int vfs_size(const char *path) { (void)path; return -1; }
int vfs_read(const char *path, void *buf, int max)
{ (void)path; (void)buf; (void)max; return -1; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
uint64_t dev_bar_map(struct device *dev, int idx)
{ return map_bar0(NULL, dev) && idx == 0 ? fake_map : 0; }
void kprintf(const char *fmt, ...) { (void)fmt; }
const char *nvidia_pascal_model_name(uint16_t id)
{ return id == 0x1c81 ? "GeForce GTX 1050" : NULL; }

static struct device gp107(void)
{
    struct device dev;
    memset(&dev, 0, sizeof dev);
    dev.bus_type = DEV_BUS_PCI;
    dev.vendor = 0x10de;
    dev.device = 0x1c81;
    dev.class_code = 0x03;
    dev.subclass = 0x00;
    dev.header_type = 0;
    dev.res[0].start = fake_map;
    dev.res[0].size = 16u << 20;
    dev.res[0].flags = DEV_RES_MEM;
    dev.res[1].start = 0xe0000000ull;
    dev.res[1].size = 256u << 20;
    dev.res[1].flags = DEV_RES_MEM | DEV_RES_PREFETCH;
    return dev;
}

static void reset_fixture(void)
{
    measure_calls = map_calls = mmio_reads = mmio_writes = 0;
    fail_index = -1;
    fail_reason = NV1050_BLOCK_NONE;
    fake_boot0 = (0x137u << 20) | 0xa1u;
    fake_boot1 = 0;
    fake_enable = 1;
    fake_map = 0xf6000000ull;
}

static int run(struct device *dev, struct nv1050_accel_info *info)
{
    struct nv1050_fw_reader fw = { NULL, measure };
    struct nv1050_hw_reader hw = { NULL, map_bar0, read32 };
    return nv1050_accel_prepare_with_ops(dev, &fw, &hw, info);
}

int main(void)
{
    check(nv1050_fw_manifest_count() == NV1050_FW_MANIFEST_COUNT,
          "manifest has the 22 Nouveau GP107 prerequisite files");
    check(strcmp(NV1050_LINUX_FIRMWARE_COMMIT,
                 "1522c78ab870b3c051d8a3a1d24ecbbc12b23be5") == 0,
          "manifest pins the audited linux-firmware commit");
    check(strcmp(NV1050_LINUX_NOUVEAU_COMMIT,
                 "587858367581b9c55c3690f4e63382ad622719d4") == 0,
          "stage model pins the audited Nouveau source commit");

    unsigned acr = 0, sec2 = 0, gr = 0;
    for (size_t i = 0; i < nv1050_fw_manifest_count(); i++) {
        const struct nv1050_fw_manifest_entry *e = nv1050_fw_manifest_at(i);
        check(e && e->path && e->size && strlen(e->sha256_hex) == 64,
              "every firmware entry has path, exact size and SHA-256");
        if (strstr(e->path, "/acr/")) acr++;
        else if (strstr(e->path, "/sec2/")) sec2++;
        else if (strstr(e->path, "/gr/")) gr++;
    }
    check(acr == 4 && sec2 == 6 && gr == 12,
          "manifest is ACR4 + SEC2 ABI0/1 six + GR12");
    check(nv1050_fw_manifest_at(NV1050_FW_MANIFEST_COUNT) == NULL,
          "manifest lookup refuses an out-of-range index");

    struct nv1050_accel_info info;
    reset_fixture();
    struct device dev = gp107(); dev.vendor = 0x1234;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_WRONG_DEVICE &&
          measure_calls == 0 && map_calls == 0 && mmio_reads == 0 && mmio_writes == 0,
          "wrong PCI identity reaches neither firmware nor MMIO");

    reset_fixture();
    dev = gp107(); dev.res[0].size = 0x80000;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_BAD_BAR0 &&
          measure_calls == 0 && map_calls == 0 && mmio_reads == 0 && mmio_writes == 0,
          "short PRI BAR is refused before firmware or MMIO");

    reset_fixture();
    dev = gp107(); dev.res[0].start = UINT64_MAX - 7; dev.res[0].size = 0x100000;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_BAD_BAR0 &&
          measure_calls == 0 && map_calls == 0 && mmio_reads == 0 && mmio_writes == 0,
          "wrapped PRI BAR is refused before firmware or MMIO");

    reset_fixture();
    dev = gp107(); fail_index = 0; fail_reason = NV1050_BLOCK_FW_MISSING;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_FW_MISSING &&
          info.firmware_verified == 0 && map_calls == 0 && mmio_reads == 0 &&
          mmio_writes == 0 && info.software_fallback,
          "missing firmware preserves software fallback with zero MMIO");

    reset_fixture();
    dev = gp107(); fail_index = 4; fail_reason = NV1050_BLOCK_FW_SIZE;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_FW_SIZE &&
          info.firmware_verified == 4 && map_calls == 0 && mmio_reads == 0 &&
          mmio_writes == 0 && info.software_fallback,
          "wrong firmware size stops at the exact entry before MMIO");

    reset_fixture();
    dev = gp107(); fail_index = 10; fail_reason = NV1050_BLOCK_FW_HASH;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_FW_HASH &&
          info.firmware_verified == 10 && map_calls == 0 && mmio_reads == 0 &&
          mmio_writes == 0 && info.software_fallback,
          "corrupt firmware SHA-256 cannot reach MMIO");

    reset_fixture();
    dev = gp107(); fake_map = 0;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_MAP_FAILED &&
          info.firmware_verified == 22 && map_calls == 1 && mmio_reads == 0 &&
          mmio_writes == 0 && info.software_fallback,
          "BAR mapping failure happens after all hashes and before reads");

    reset_fixture();
    dev = gp107(); fake_boot0 = (0x134u << 20) | 0xa1u;
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_WRONG_CHIPSET &&
          info.chipset == 0x134 && map_calls == 1 && mmio_reads == 3 &&
          info.mmio_reads == 3 && info.mmio_writes == 0 && info.software_fallback,
          "PCI-matched non-GP107 silicon gets three reads, zero writes and fallback");

    reset_fixture();
    dev = gp107();
    check(run(&dev, &info) < 0 && info.blocker == NV1050_BLOCK_CHANNEL_STACK &&
          info.firmware_verified == 22 && info.chipset == 0x137 &&
          map_calls == 1 && mmio_reads == 3 && info.mmio_writes == 0 &&
          info.software_fallback,
          "verified GP107 stops before MMU PFIFO channel CE writes");
    check(nvidia_pascal_accel_fill(0, 0, 1, 1, 0) < 0 &&
          nvidia_pascal_accel_copy(0, 0, 0, 0, 1, 1) < 0,
          "fill and copy refuse until an off-screen CE canary exists");
    check(strcmp(nvidia_pascal_accel_blocker_name(NV1050_BLOCK_CHANNEL_STACK),
                 "mmu-fifo-channel-ce") == 0,
          "channel blocker is machine-readable");

    printf("NV_ACCEL_BRINGUP: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
