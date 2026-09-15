#include "amd/accel.h"
#include "driver.h"
#include "fb.h"
#include "pci.h"
#include "kprintf.h"
#include "ktime.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* Link the actual product wrapper and both actual cores. Only external PCI,
 * mapping, logging and compositor ownership are replaced. The wrapper's
 * volatile MMIO dereferences hit this array directly, so this gate also catches
 * DWORD-vs-byte offsets which a callback-only core test cannot see. */
static uint32_t registers[0x40000 / 4], before[0x40000 / 4];
static const uint32_t offsets[] = {
    0x5428, 0x2024, 0x0e4c, 0x1410, 0xd048, 0xd848, 0xd200, 0xda00,
};
static const uint32_t register_values[] = {
    8192, 0x41ff4000, 0x12345602, 0x12345603,
    0x12345604, 0x12345605, 0x12345606, 0x12345607,
};
static struct device dev;
static uint16_t command, power;
static uint8_t pm_cap;
static uint64_t map_result;
static unsigned maps, config_reads, pm_reads, cap_reads, bad_access;
static unsigned lock_depth, locks, unlocks, lock_faults, installs, nonnull_installs;
static fb_native_present_fn presenter;
static char messages[4096];
static size_t message_length;

void fb_graphics_lock(void) { ++lock_depth; ++locks; }
void fb_graphics_unlock(void)
{
    if (!lock_depth) ++lock_faults;
    else --lock_depth;
    ++unlocks;
}
void fb_set_native_present(fb_native_present_fn fn)
{
    if (lock_depth != 1) ++lock_faults;
    ++installs;
    if (fn) ++nonnull_installs;
    presenter = fn;
}

static void check_pci(uint8_t bus, uint8_t slot, uint8_t func)
{
    if (bus != dev.bus || slot != dev.slot || func != dev.func || lock_depth != 1)
        ++bad_access;
}
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    check_pci(bus, slot, func); ++config_reads;
    if (off == 4) return command;
    if (pm_cap && off == (uint16_t)(pm_cap + 4)) { ++pm_reads; return power; }
    ++bad_access;
    return UINT16_MAX;
}
uint8_t pci_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t id)
{
    check_pci(bus, slot, func); ++cap_reads;
    if (id != 1) ++bad_access;
    return pm_cap;
}
uint64_t dev_bar_map(struct device *card, int index)
{
    ++maps;
    if (card != &dev || index != 5 || lock_depth != 1) {
        ++bad_access;
        return 0;
    }
    return map_result;
}
uint64_t time_mono_ns(void) { return 0; }
void kprintf(const char *fmt, ...)
{
    if (lock_depth != 1) ++lock_faults;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(messages + message_length, sizeof messages - message_length,
                      fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t available = sizeof messages - message_length - 1;
        message_length += (size_t)n < available ? (size_t)n : available;
    }
}

static void init(void)
{
    amd_accel_reset();
    memset(&dev, 0, sizeof dev);
    dev.bus_type = DEV_BUS_PCI; dev.vendor = 0x1002; dev.device = 0x67df;
    dev.class_code = 3; dev.revision = 0xe7;
    dev.bus = 2; dev.slot = 3; dev.func = 1;
    dev.res[0] = (struct dev_resource){0xe0000000, 0x10000000,
        DEV_RES_MEM | DEV_RES_64 | DEV_RES_PREFETCH};
    dev.res[5] = (struct dev_resource){0xf1000000, 0x40000, DEV_RES_MEM};
    memset(registers, 0xff, sizeof registers);
    for (unsigned i = 0; i < 8; ++i) registers[offsets[i] / 4] = register_values[i];
    memcpy(before, registers, sizeof before);
    command = 2; power = 0; pm_cap = 0x40;
    map_result = (uint64_t)(uintptr_t)registers;
    maps = config_reads = pm_reads = cap_reads = bad_access = 0;
    locks = unlocks = lock_faults = installs = nonnull_installs = 0;
    message_length = 0; messages[0] = 0;
}

static struct amd_accel_info prepare(struct device *card)
{
    struct amd_accel_info info;
    memset(&info, 0xa5, sizeof info);
    CHECK(amd_accel_prepare(card, 0xe0100000, 1280u * 720u * 4u, 1280, 720) == -1);
    CHECK(amd_accel_query(&info) == 0);
    CHECK(info.stage != AMD_ACCEL_ACTIVE && info.commands == 0 &&
          info.presents == 0 && info.gpu_pixels == 0 &&
          info.mmio_writes == 0 && info.vram_writes == 0 &&
          info.canary_ready == 0 && info.software_fallback == 1);
    CHECK(presenter == NULL && nonnull_installs == 0); /* PRESENTER_REFUSAL_ORACLE */
    CHECK(installs >= 1 && locks == unlocks && !lock_depth && !lock_faults && !bad_access);
    CHECK(!memcmp(registers, before, sizeof registers));
    return info;
}

static void refused(enum polaris_probe_result result, unsigned expected_maps)
{
    struct amd_accel_info info = prepare(&dev);
    CHECK(info.stage == AMD_ACCEL_BLOCKED && info.polaris.result == result);
    CHECK(maps == expected_maps && info.polaris.vram_bytes == 0);
}

int main(void)
{
    init();
    struct amd_accel_info info = prepare(&dev);
    CHECK(info.stage == AMD_ACCEL_MMIO_READ && info.family == AMD_FAMILY_POLARIS &&
          info.pci_device == 0x67df && info.blocker == AMD_BLOCK_POLARIS_INIT);
    CHECK(maps == 1 && config_reads == 2 && pm_reads == 1 && cap_reads == 1);
    CHECK(info.mmio_reads == 8 && info.polaris.mmio_reads == 8 &&
          info.polaris.result == POLARIS_PROBE_OBSERVED);
    CHECK(info.polaris.vram_bytes == (8ull << 30) &&
          info.polaris.cpu_aperture_bytes == (256ull << 20));
    CHECK(info.polaris.mc_location == 0x41ff4000 &&
          info.polaris.mc_base == (256ull << 30) &&
          info.polaris.mc_end == (264ull << 30) && info.polaris.layout_covers_vram);
    CHECK(info.polaris.srbm_status2 == 0x12345602 && info.polaris.vm_context0 == 0x12345603 &&
          info.polaris.sdma_f32[0] == 0x12345604 && info.polaris.sdma_f32[1] == 0x12345605 &&
          info.polaris.sdma_ring[0] == 0x12345606 && info.polaris.sdma_ring[1] == 0x12345607);
    CHECK(strstr(messages, "vram_mib=8192 aperture_mib=256") &&
          strstr(messages, "acceleration=unavailable") && !strstr(messages, "stage=active"));

    amd_accel_reset();
    CHECK(amd_accel_query(&info) == 0 && info.stage == AMD_ACCEL_OFF &&
          info.family == AMD_FAMILY_UNKNOWN && info.pci_device == 0 &&
          info.polaris.result == POLARIS_PROBE_NONE && !info.polaris.vram_bytes &&
          !info.mmio_reads && info.software_fallback == 1 && presenter == NULL);
    unsigned old_locks = locks;
    CHECK(amd_accel_query(NULL) == -1 && locks == old_locks);

    /* Each literal register is made unreadable in turn. Reaching a failed read
     * after exactly i+1 accesses proves that the real wrapper did not satisfy
     * its report with constants or fetch all eight from one location. */
    for (unsigned i = 0; i < 8; ++i) {
        init(); registers[offsets[i] / 4] = UINT32_MAX;
        memcpy(before, registers, sizeof before);
        info = prepare(&dev);
        CHECK(info.stage == AMD_ACCEL_BLOCKED && info.polaris.result == POLARIS_PROBE_READ &&
              info.mmio_reads == i + 1 && info.polaris.mmio_reads == i + 1 && maps == 1);
    }

    init(); pm_cap = 0;
    info = prepare(&dev);
    CHECK(info.stage == AMD_ACCEL_MMIO_READ && pm_reads == 0 && config_reads == 1);
    init(); pm_cap = 0xf8;
    info = prepare(&dev);
    CHECK(info.stage == AMD_ACCEL_MMIO_READ && pm_reads == 1 && config_reads == 2);
    init(); pm_cap = 0xfc;
    refused(POLARIS_PROBE_POWER, 0);
    CHECK(pm_reads == 0 && config_reads == 1);
    init(); pm_cap = 0xff;
    refused(POLARIS_PROBE_POWER, 0);
    CHECK(pm_reads == 0 && config_reads == 1);
    for (unsigned p = 1; p <= 3; ++p) {
        init(); power = (uint16_t)p; refused(POLARIS_PROBE_POWER, 0);
    }
    init(); command = 0; refused(POLARIS_PROBE_DECODE, 0);
    init(); command = UINT16_MAX; refused(POLARIS_PROBE_DECODE, 0);
    init(); dev.res[5].flags = DEV_RES_IO; refused(POLARIS_PROBE_BAR, 0);
    init(); dev.res[5].size = 0xd000; refused(POLARIS_PROBE_BAR, 0);
    init(); dev.res[0].start = 0xd0000000; refused(POLARIS_PROBE_SURFACE, 0);
    init(); map_result = 0; refused(POLARIS_PROBE_MAP, 1);

    const uint16_t others[] = { 0x67ef, 0x6987, 0x6863, 0x731f, 0x744c, 0xffff };
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; ++i) {
        init(); dev.device = others[i]; info = prepare(&dev);
        CHECK(maps == 0 && config_reads == 0 && cap_reads == 0 && info.mmio_reads == 0 &&
              info.polaris.result == POLARIS_PROBE_NONE && info.stage == AMD_ACCEL_BLOCKED);
    }
    for (unsigned i = 0; i < 5; ++i) {
        init();
        if (i == 0) dev.vendor = 0x10de;
        if (i == 1) dev.bus_type = DEV_BUS_PLATFORM;
        if (i == 2) dev.class_code = 1;
        if (i == 3) dev.subclass = 1;
        if (i == 4) dev.header_type = 1;
        info = prepare(&dev);
        CHECK(maps == 0 && config_reads == 0 && info.polaris.result == POLARIS_PROBE_NONE);
    }
    init(); info = prepare(NULL);
    CHECK(maps == 0 && config_reads == 0 && info.family == AMD_FAMILY_UNKNOWN);

    printf("POLARIS_INTEGRATION: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
