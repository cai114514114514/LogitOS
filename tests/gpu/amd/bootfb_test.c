#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver.h"
#include "pci.h"
#include "amd/bootfb.h"
#include "amd/polaris/resource/device.h"
#include "amd/polaris/selftest/selftest.h"

static int checks, failures;
static uint16_t cfg_command = PCI_CMD_MEM;
static uint16_t cfg_pmcsr;
static uint8_t cfg_pm_cap = 0x40;
static uint32_t screen_w = 1280, screen_h = 800;
static uint64_t lfb_addr = 0xe0100000ull;
static uint64_t lfb_bytes = 1280ull * 800 * 4;
static int have_lfb = 1;
static unsigned cfg_writes, enables, maps, irqs;
/* Calls of the monotonic clock: the display survey is the only caller, so zero
 * proves the frame-counter measurement (the boot stall) never started. */
static unsigned clock_reads;
/* Set to make dev_bar_map fail for BAR5, to reach the "BAR5 not mapped" skip. */
static int bar5_unmappable;
static unsigned accel_prepares, accel_resets;
static int accel_result = -1;
static uint64_t accel_lfb, accel_bytes;
static uint32_t accel_width, accel_height;
static unsigned resource_probes;
/* The display-survey glue gets a report that proves the MC window, plus a fake
 * BAR5 whose frame counter the stub clock advances (see time_mono_ns). */
static int survey_fixture;
static struct polaris_resource_report survey_report;

/* The display survey (stage B2) exists only in HWHUD builds. The test is built
 * both ways; SURVEY_EXPECTED says which behaviour the production object under
 * test must show. The negative control builds the HWHUD object while expecting
 * the plain behaviour, so the plain-build assertions are proven able to fail. */
#ifdef LOGIT_HWHUD
#define SURVEY_COMPILED_IN 1
#else
#define SURVEY_COMPILED_IN 0
#endif
#ifdef AMD_BOOTFB_NEGCTL_EXPECT_NO_SURVEY
#define SURVEY_EXPECTED 0
#else
#define SURVEY_EXPECTED SURVEY_COMPILED_IN
#endif
/* Stage B3: the kernel glue of the GPU self-test, driven through the very ops
 * table bootfb.c hands to polaris_selftest_run. The run itself is a stub that
 * records what it was given and, when exercise_ops is set, calls every
 * callback once, so the adapters are tested against the stub kernel services
 * below instead of being trusted. */
#ifdef AMD_BOOTFB_NEGCTL_SELFTEST_IN_ANY_BUILD
#define SELFTEST_EXPECTED 0
#else
#define SELFTEST_EXPECTED SURVEY_EXPECTED
#endif
static unsigned selftest_runs;
static struct polaris_selftest_inputs selftest_inputs;
static int exercise_ops;
static int leave_lease_held;
static int selftest_quiesce_result;
static unsigned selftest_quiesces;
static unsigned bar0_whole_maps;
static unsigned window_maps;
static uint64_t window_offset, window_bytes;
static unsigned firmware_loads, firmware_releases, workspace_allocs, workspace_frees;
static uint16_t firmware_device;
static uint8_t firmware_revision;
static unsigned firmware_key;
static struct {
    int acquire_result;
    int held_after_acquire;
    int held_after_retire;
    int registers_mapped;
    int window_mapped;
    uint32_t config_value;
    int diagnostic_ok;
    int diagnostic_unaligned_refused;
    int diagnostic_out_of_range_refused;
    uint32_t diagnostic_value;
    uint64_t clock_us;
    int firmware_result;
    int workspace_ok;
} glue;
static uint32_t fake_bar5[0x4600];
static uint64_t resource_lfb, resource_bytes;
static uint32_t resource_width, resource_height;

enum polaris_selftest_result
polaris_selftest_run(const struct polaris_selftest_inputs *inputs,
                     const struct polaris_selftest_ops *ops,
                     struct polaris_selftest_report *report)
{
    ++selftest_runs;
    selftest_inputs = *inputs;
    *report = (struct polaris_selftest_report){
        .stage = POLARIS_SELFTEST_STAGE_SDMA, .result = POLARIS_SELFTEST_OK
    };
    if (!exercise_ops) {
        return POLARIS_SELFTEST_OK;
    }
    glue.acquire_result = ops->acquire_lease(ops->context);
    glue.held_after_acquire = ops->lease_held(ops->context);
    if (glue.acquire_result) {
        return POLARIS_SELFTEST_FAILED_LEASE;
    }
    volatile uint8_t *registers = 0;
    volatile uint8_t *window = 0;
    glue.registers_mapped = !ops->map_registers(ops->context, &registers) &&
                            registers == (volatile uint8_t *)(uintptr_t)fake_bar5;
    const struct polaris_arena_plan *plan = inputs->arena_plan;
    glue.window_mapped = plan &&
                         !ops->map_arena_window(ops->context,
                                                plan->arena.base - plan->aperture.base,
                                                plan->arena.bytes, &window) &&
                         window != 0;
    ops->read_pci_config(ops->context, 0x04, &glue.config_value);
    fake_bar5[0x2024 / 4] = 0x12345678u;
    glue.diagnostic_ok = !ops->read_diagnostic_register(ops->context, 0x2024, &glue.diagnostic_value);
    glue.diagnostic_unaligned_refused = ops->read_diagnostic_register(ops->context, 0x2026, &glue.diagnostic_value) != 0;
    glue.diagnostic_out_of_range_refused = ops->read_diagnostic_register(ops->context, 0x40000, &glue.diagnostic_value) != 0;
    glue.clock_us = ops->now_us(ops->context);
    struct polaris_resource_files files = {0};
    glue.firmware_result = ops->load_firmware(ops->context, 1, &files);
    ops->release_firmware(ops->context, &files);
    void *workspace = ops->allocate_workspace(ops->context, 64);
    glue.workspace_ok = workspace != 0;
    ops->free_workspace(ops->context, workspace);
    if (!leave_lease_held) {
        ops->retire_lease(ops->context);
        glue.held_after_retire = ops->lease_held(ops->context);
    }
    return POLARIS_SELFTEST_OK;
}

void polaris_selftest_format_hud(char *out, int capacity,
                                 const struct polaris_selftest_report *report)
{
    (void)report;
    snprintf(out, (size_t)capacity, "stub-hud");
}

int polaris_selftest_quiesce(const struct polaris_selftest_ops *ops,
                             struct polaris_selftest_report *report)
{
    (void)ops;
    (void)report;
    ++selftest_quiesces;
    return selftest_quiesce_result;
}

int polaris_selftest_opted_out(void)
{
    return 0;
}

int polaris_resource_files_load(uint16_t device, uint8_t revision, unsigned key,
                                struct polaris_resource_files *files)
{
    (void)files;
    ++firmware_loads;
    firmware_device = device;
    firmware_revision = revision;
    firmware_key = key;
    return 0;
}

void polaris_resource_files_release(struct polaris_resource_files *files)
{
    (void)files;
    ++firmware_releases;
}

void *kmalloc(size_t bytes)
{
    ++workspace_allocs;
    return malloc(bytes);
}

void kfree(void *memory)
{
    ++workspace_frees;
    free(memory);
}

/* The resource implementation has its own real-parser gate. This fixture
 * verifies the boot wrapper passes the actual framebuffer and calls once. */
int polaris_resources_probe_device(struct device *device, uint64_t lfb,
                                   uint64_t bytes, uint32_t width, uint32_t height,
                                   struct polaris_resource_report *report)
{
    (void)device;
    resource_probes++;
    resource_lfb = lfb;
    resource_bytes = bytes;
    resource_width = width;
    resource_height = height;
    *report = survey_fixture ? survey_report :
              (struct polaris_resource_report){.ownership_missing = 1};
    return 0;
}

static void check(int yes, const char *what)
{
    checks++;
    if (!yes) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    (void)bus; (void)slot; (void)func;
    if (off == PCI_CFG_COMMAND) return cfg_command;
    if (cfg_pm_cap && off == (uint16_t)(cfg_pm_cap + 4)) return cfg_pmcsr;
    return 0;
}

uint8_t pci_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap)
{
    (void)bus; (void)slot; (void)func;
    return cap == 1 ? cfg_pm_cap : 0;
}

void pci_cfg_write16(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint16_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; cfg_writes++; }
void dev_enable(struct device *dev, int master)
{ (void)dev; (void)master; enables++; }
uint64_t dev_bar_map(struct device *dev, int bar)
{
    (void)dev;
    maps++;
    if (bar == 0) bar0_whole_maps++;
    if (bar5_unmappable) return 0;
    return survey_fixture && bar == 5 ? (uint64_t)(uintptr_t)fake_bar5 : 0;
}
/* Mapping part of BAR0 is allowed, and recorded: the self-test must ask for the
 * arena window and nothing more. */
uint64_t dev_bar_map_window(struct device *dev, int bar, uint64_t offset, uint64_t bytes)
{
    if (bar != 0) return 0;
    window_maps++;
    window_offset = offset;
    window_bytes = bytes;
    return dev->res[0].start + offset;
}
uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    (void)bus; (void)slot; (void)func;
    return 0x00100000u | off;
}
int dev_irq_request(struct device *dev, irq_handler_t fn, void *arg, const char *name)
{ (void)dev; (void)fn; (void)arg; (void)name; irqs++; return -1; }
uint32_t fb_width(void) { return screen_w; }
uint32_t fb_height(void) { return screen_h; }
int fb_boot_lfb_range(uint64_t *addr, uint64_t *bytes)
{
    if (!have_lfb) return 0;
    *addr = lfb_addr;
    *bytes = lfb_bytes;
    return 1;
}
void kprintf(const char *fmt, ...) { (void)fmt; }
int ksnprintf(char *buf, int max, const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    int written = vsnprintf(buf, (size_t)max, fmt, arguments);
    va_end(arguments);
    return written;
}
/* 1 us per call; the CRTC0 frame counter (dword 0x1ba6) ticks every 16.66 ms. */
uint64_t time_mono_ns(void)
{
    static uint64_t now_ns = 1000000;
    clock_reads++;
    now_ns += 1000;
    fake_bar5[0x1ba6] = (uint32_t)(now_ns / 16661185u);
    return now_ns;
}
int time_get_source(void) { return 0; }
const char *time_source_name(int source) { (void)source; return "stub"; }
int amd_accel_prepare(struct device *dev, uint64_t lfb, uint64_t bytes,
                      uint32_t width, uint32_t height)
{
    (void)dev;
    accel_prepares++;
    accel_lfb = lfb;
    accel_bytes = bytes;
    accel_width = width;
    accel_height = height;
    return accel_result;
}
void amd_accel_reset(void) { accel_resets++; }

static struct device amd_gpu(uint16_t device)
{
    struct device d;
    memset(&d, 0, sizeof d);
    memcpy(d.name, "0000:01:00.0", 13);
    d.bus_type = DEV_BUS_PCI;
    d.bus = 1; d.slot = 0; d.func = 0;
    d.vendor = 0x1002; d.device = device;
    d.class_code = PCI_CLASS_DISPLAY;
    d.subclass = 0;
    d.header_type = 0;
    d.res[0].start = 0xe0000000ull;
    d.res[0].size = 256ull << 20;
    d.res[0].flags = DEV_RES_MEM | DEV_RES_PREFETCH;
    d.res[2].start = 0xf6000000ull;
    d.res[2].size = 64ull << 10;
    d.res[2].flags = DEV_RES_MEM;
    return d;
}

static void reset_bus(void)
{
    cfg_command = PCI_CMD_MEM;
    cfg_pmcsr = 0;
    cfg_pm_cap = 0x40;
    screen_w = 1280;
    screen_h = 800;
    lfb_addr = 0xe0100000ull;
    lfb_bytes = 1280ull * 800 * 4;
    have_lfb = 1;
    cfg_writes = enables = maps = irqs = 0;
    accel_prepares = accel_resets = 0;
    accel_result = -1;
    accel_lfb = accel_bytes = 0;
    accel_width = accel_height = 0;
}

static int no_takeover(void)
{
    return cfg_writes == 0 && enables == 0 && maps == 0 && irqs == 0;
}

/* Unbind D as the device model would and bind it again, so the next call of
 * amd_bootfb_probe_resources() runs. Leaves the device unbound to the driver
 * (drv NULL) until the caller sets it. */
static void remove_and_rebind(struct device *dev)
{
    const struct driver *driver = amd_bootfb_driver_for_test();
    driver->remove(dev);
    dev->drv = NULL;
    dev->drvdata = NULL;
    amd_bootfb_probe(dev);
}

int main(void)
{
    const struct dev_match *m = amd_bootfb_match_table();
    check(m[0].vendor == 0x1002 && m[0].device == DEV_ANY &&
          m[0].class_code == PCI_CLASS_DISPLAY &&
          m[0].subclass == DEV_ANYC && m[0].prog_if == DEV_ANYC,
          "match table covers only AMD/ATI PCI display functions");
    check(m[1].vendor == 0 && m[1].device == 0 && m[1].class_code == 0 &&
          m[1].subclass == 0 && m[1].prog_if == 0 && m[1].data == 0,
          "match table has exactly one production rule");

    static const uint16_t generations[] = {
        0x5046, /* QEMU Rage 128 / ati-vga integration model */
        0x67df, /* Polaris */
        0x731f, /* Navi 10 */
        0x73bf, /* Navi 21 */
        0x744c, /* Navi 31 */
    };
    for (unsigned i = 0; i < sizeof generations / sizeof generations[0]; i++) {
        reset_bus();
        struct device d = amd_gpu(generations[i]);
        check(amd_bootfb_probe(&d) == 0,
              "AMD display generation with owned LFB binds passively");
        check(d.drvdata != NULL,
              "successful passive bind records observer state");
        check(no_takeover(),
              "successful probe performs no write, enable, map or IRQ request");
    }

    reset_bus();
    struct device rv100 = amd_gpu(0x5159);
    accel_result = 0;
    check(amd_bootfb_probe(&rv100) == 0,
          "owned RV100 boot framebuffer remains bound after accel canary");
    check(accel_prepares == 1 && accel_lfb == lfb_addr &&
          accel_bytes == lfb_bytes && accel_width == screen_w &&
          accel_height == screen_h,
          "RV100 helper receives the exact validated LFB geometry");
    check(no_takeover(),
          "bootfb layer itself performs no takeover around RV100 helper");

    static const uint8_t subclasses[] = { 0x00, 0x01, 0x02, 0x80 };
    for (unsigned i = 0; i < sizeof subclasses / sizeof subclasses[0]; i++) {
        reset_bus();
        struct device d = amd_gpu(0x67df);
        d.subclass = subclasses[i];
        check(amd_bootfb_probe(&d) == 0,
              "AMD vendor-specific display subclass remains eligible");
    }

    reset_bus();
    struct device d = amd_gpu(0x67df);
    d.vendor = 0x10de;
    check(amd_bootfb_probe(&d) != 0, "foreign display vendor remains unclaimed");
    check(no_takeover(), "foreign display refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    d.class_code = PCI_CLASS_NETWORK;
    check(amd_bootfb_probe(&d) != 0, "AMD non-display function remains unclaimed");
    check(no_takeover(), "non-display refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    d.header_type = 1;
    check(amd_bootfb_probe(&d) != 0, "AMD PCI bridge header remains unclaimed");
    check(no_takeover(), "bridge refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_command = 0;
    check(amd_bootfb_probe(&d) != 0,
          "memory-decode-off display is refused without a config write");
    check(no_takeover(), "memory-decode refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_command = UINT16_MAX;
    check(amd_bootfb_probe(&d) != 0, "unreachable PCI command register is refused");
    check(no_takeover(), "all-ones command refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_pmcsr = 3;
    check(amd_bootfb_probe(&d) != 0, "D3 display is refused rather than woken");
    check(no_takeover(), "D3 refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_pm_cap = 0;
    check(amd_bootfb_probe(&d) == 0,
          "absence of optional PCI PM capability does not fabricate a state");
    check(no_takeover(), "PM-absent probe remains read-only");

    reset_bus();
    d = amd_gpu(0x67df);
    memset(d.res, 0, sizeof d.res);
    check(amd_bootfb_probe(&d) != 0, "display without a memory BAR is refused");
    check(no_takeover(), "missing-BAR refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    d.res[0].start = UINT64_MAX - 7;
    d.res[0].size = 16;
    memset(&d.res[2], 0, sizeof d.res[2]);
    check(amd_bootfb_probe(&d) != 0, "wrapped memory BAR aperture is refused");
    check(no_takeover(), "wrapped-BAR refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    d.res[0].flags |= DEV_RES_IO;
    memset(&d.res[2], 0, sizeof d.res[2]);
    check(amd_bootfb_probe(&d) != 0,
          "ambiguous memory-plus-I/O BAR is refused");
    check(no_takeover(), "mixed-BAR refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_pm_cap = 0xfc;
    check(amd_bootfb_probe(&d) != 0,
          "PM capability whose PMCSR crosses config space is refused");
    check(no_takeover(), "malformed-PM refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    have_lfb = 0;
    check(amd_bootfb_probe(&d) != 0,
          "absent or virtio-backed Multiboot LFB is refused");
    check(no_takeover(), "missing-LFB refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    screen_w = 0;
    check(amd_bootfb_probe(&d) != 0, "zero-sized display mode is refused");
    check(no_takeover(), "zero-mode refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    lfb_addr = UINT64_MAX - 3;
    lfb_bytes = 8;
    check(amd_bootfb_probe(&d) != 0, "wrapped LFB range is refused");
    check(no_takeover(), "wrapped-LFB refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    lfb_addr = 0xd0000000ull;
    check(amd_bootfb_probe(&d) != 0,
          "LFB belonging to another GPU remains unclaimed");
    check(no_takeover(), "foreign-LFB refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    lfb_addr = d.res[0].start + d.res[0].size - 1024;
    lfb_bytes = 2048;
    check(amd_bootfb_probe(&d) != 0,
          "partial LFB overlap is refused by full-range containment");
    check(no_takeover(), "partial-LFB refusal has no takeover side effects");

    reset_bus();
    d = amd_gpu(0x67df);
    lfb_addr = d.res[0].start + d.res[0].size - 4096;
    lfb_bytes = 4096;
    check(amd_bootfb_probe(&d) == 0,
          "LFB ending exactly at BAR boundary is fully contained");

    reset_bus();
    d = amd_gpu(0x67df);
    cfg_command = PCI_CMD_MEM | PCI_CMD_MASTER;
    check(amd_bootfb_probe(&d) == 0,
          "firmware-enabled BME is observed without being modified");
    check(no_takeover(), "pre-existing BME does not cause a config write");

    amd_bootfb_probe_resources();
    check(resource_probes == 1, "post-mount resource discovery reaches its production entry");
    check(resource_lfb == lfb_addr && resource_bytes == lfb_bytes &&
          resource_width == screen_w && resource_height == screen_h,
          "resource discovery receives the actual owned framebuffer");
    amd_bootfb_probe_resources();
    check(resource_probes == 1, "repeated post-mount hook does not reread all firmware");

    /* Stage B3: the self-test is handed what the probe established, once. */
    if (SELFTEST_EXPECTED) {
        check(selftest_runs == 1, "an HWHUD build runs the GPU self-test once per probe");
        check(selftest_inputs.pci_vendor == 0x1002 && selftest_inputs.pci_device == 0x67df &&
              selftest_inputs.pci_revision == d.revision,
              "the self-test is told the PCI identity and revision of the device");
        check(selftest_inputs.arena_plan == NULL,
              "a display survey that did not run gives the self-test no arena");
        check(selftest_inputs.width == screen_w && selftest_inputs.height == screen_h &&
              selftest_inputs.pitch == lfb_bytes / screen_h &&
              selftest_inputs.bar0_physical == d.res[0].start,
              "the self-test gets the boot surface geometry and the BAR0 address");
    } else {
        check(selftest_runs == 0, "a build without HWHUD never runs the GPU self-test");
        check(amd_bootfb_gpu_runtime_hud_line() == NULL,
              "a build without HWHUD has no gpu-rt HUD line");
    }

    if (SURVEY_EXPECTED) {
        const char *skipped = amd_bootfb_display_hud_line(0);
        check(skipped && !strcmp(skipped,
              "skipped: no proven MC window or framebuffer pitch") &&
              amd_bootfb_display_hud_line(1) == NULL,
              "a skipped display survey leaves its reason on the HUD line");
    } else {
        check(amd_bootfb_display_hud_line(0) == NULL,
              "no display survey line without a proven MC window");
    }
    check(clock_reads == 0, "a skipped display survey does not time the frame counter");

    /* A proven MC window but a BAR5 that cannot be mapped: skipped, with the
     * reason in the HUD line, and still no clock read. */
    reset_bus();
    d = amd_gpu(0x67df);
    survey_fixture = 1;
    survey_report = (struct polaris_resource_report){
        .device = {.result = POLARIS_PROBE_OBSERVED, .layout_covers_vram = 1,
                   .mc_base = 0xf400000000ull, .vram_bytes = 0x200000000ull,
                   .cpu_aperture_bytes = 256ull << 20},
    };
    bar5_unmappable = 1;
    check(amd_bootfb_probe(&d) == 0, "unmappable-BAR5 fixture device binds");
    amd_bootfb_probe_resources();
    bar5_unmappable = 0;
    if (SURVEY_EXPECTED) {
        const char *skipped = amd_bootfb_display_hud_line(0);
        check(skipped && !strcmp(skipped, "skipped: BAR5 not mapped") &&
              amd_bootfb_display_hud_line(1) == NULL,
              "an unmappable BAR5 leaves its reason on the HUD line");
    } else {
        check(amd_bootfb_display_hud_line(0) == NULL && maps == 0,
              "a build without HWHUD does not even try to map BAR5");
    }
    check(clock_reads == 0, "an unmappable BAR5 does not time the frame counter");
    if (SELFTEST_EXPECTED) {
        check(selftest_runs == 2 && selftest_inputs.arena_plan == NULL,
              "an unmappable BAR5 leaves the self-test without an arena, and it is asked anyway");
    } else {
        check(selftest_runs == 0, "a build without HWHUD still runs no self-test here");
    }

    /* The survey glue: real device fields in, the serial/HUD text out. CRTC0
     * scans the Multiboot LFB (BAR0 + 1 MiB) at the MC base + 1 MiB. */
    reset_bus();
    d = amd_gpu(0x67df);
    d.res[5].start = 0xf7000000ull;
    d.res[5].size = 0x40000;
    d.res[5].flags = DEV_RES_MEM;
    survey_fixture = 1;
    survey_report = (struct polaris_resource_report){
        .device = {.result = POLARIS_PROBE_OBSERVED, .layout_covers_vram = 1,
                   .mc_base = 0xf400000000ull, .vram_bytes = 0x200000000ull,
                   .cpu_aperture_bytes = 256ull << 20},
        .atom_result = POLARIS_ATOM_OK,
        .reservations = {
            .firmware = {0xf400000000ull + 0x1ffff0000ull, 0x10000},
            .driver_scratch = {0xf400000000ull + 0xff00000ull, 0x100000}
        },
        .firmware_bundle_bytes = 0x80000
    };
    fake_bar5[0x1b9c] = 1;                          /* CRTC_CONTROL.MASTER_EN */
    fake_bar5[0x1a00] = 1;                          /* GRPH_ENABLE */
    fake_bar5[0x1a01] = 2;                          /* GRPH_DEPTH: 32 bpp, linear */
    fake_bar5[0x1a04] = 0x00100000;                 /* surface low */
    fake_bar5[0x1a07] = 0xf4;                       /* surface high */
    fake_bar5[0x1a06] = 1280;                       /* pitch in pixels */
    fake_bar5[0x1b80] = 1687;                       /* H_TOTAL - 1 */
    fake_bar5[0x1b81] = 1640 | (360u << 16);        /* H blank start / end */
    fake_bar5[0x1b87] = 1065;                       /* V_TOTAL - 1 */
    fake_bar5[0x1b8d] = 841 | (41u << 16);          /* V blank: 800 active lines */
    fake_bar5[0x1b5d] = (1280u << 16) | 800u;       /* CRTC0 viewport */
    check(amd_bootfb_probe(&d) == 0, "survey fixture device binds");
    amd_bootfb_probe_resources();
    if (SURVEY_EXPECTED) {
        const char *dce_line = amd_bootfb_display_hud_line(0);
        check(dce_line && !strncmp(dce_line, "crtc=0 mode=1280x800 refresh=60.0", 33) &&
              strstr(dce_line, " pitch=5120 surface=0xf400100000 gop bar5=262144B"),
              "display survey line carries the GOP CRTC, pitch and BAR5 size");
        const char *arena_line = amd_bootfb_display_hud_line(1);
        check(arena_line && !strcmp(arena_line,
              "base=0xf40fc00000 size=0x200000 cpu=0xefc00000 reserved=5"),
              "display survey line carries the arena");
        check(amd_bootfb_display_hud_line(2) == NULL, "there are exactly two survey lines");
        check(clock_reads > 0, "the survey times the frame counter in an HWHUD build");
    } else {
        /* A fully valid fixture, and still nothing runs: no HUD line, no BAR5
         * mapping, no clock read (the measurement is the 0.1 s boot stall). */
        check(amd_bootfb_display_hud_line(0) == NULL &&
              amd_bootfb_display_hud_line(1) == NULL,
              "no display survey line in a build without HWHUD");
        check(maps == 0 && clock_reads == 0,
              "a build without HWHUD neither maps BAR5 nor times the frame counter");
    }

    /* Stage B3 glue: the survey fixture above produced an arena; the self-test
     * stub now exercises every adapter in the ops table against the stub kernel. */
    if (SELFTEST_EXPECTED) {
        check(selftest_runs == 3 && selftest_inputs.arena_plan != NULL &&
              selftest_inputs.arena_plan->arena.bytes == 0x200000 &&
              selftest_inputs.arena_plan->arena.base == 0xf40fc00000ull,
              "the arena the survey chose is the one the self-test is given");
        check(selftest_inputs.vram.base == 0xf400000000ull &&
              selftest_inputs.vram.bytes == 0x200000000ull &&
              selftest_inputs.bar5_physical == 0xf7000000ull &&
              selftest_inputs.bar5_bytes == 0x40000 &&
              selftest_inputs.firmware_bundle_bytes == 0x80000,
              "the self-test is given the MC window, the BAR5 range and the firmware size");
        const char *runtime_line = amd_bootfb_gpu_runtime_hud_line();
        check(runtime_line && !strcmp(runtime_line, "stub-hud"),
              "the gpu-rt HUD line is the self-test's own text");

        const struct driver *driver = amd_bootfb_driver_for_test();
        check(driver && driver->quiesce && driver->remove, "the driver has quiesce and remove hooks");

        /* Not bound to amd-bootfb: no lease. */
        exercise_ops = 1;
        remove_and_rebind(&d);
        amd_bootfb_probe_resources();
        check(glue.acquire_result != 0 && glue.held_after_acquire == 0,
              "a device that is not bound to amd-bootfb cannot be leased");
        check(window_maps == 0 && bar0_whole_maps == 0,
              "no BAR0 mapping is requested without the lease");

        /* Bound: every adapter works, and BAR0 is never mapped as a whole. */
        window_maps = 0;
        bar0_whole_maps = 0;
        leave_lease_held = 1;
        remove_and_rebind(&d);
        d.drv = driver;
        amd_bootfb_probe_resources();
        check(glue.acquire_result == 0 && glue.held_after_acquire == 1,
              "a device bound to amd-bootfb is leased");
        check(glue.registers_mapped, "BAR5 is mapped through dev_bar_map");
        check(glue.window_mapped && window_maps == 1 &&
              window_offset == 0xfc00000ull && window_bytes == 0x200000ull,
              "BAR0 is mapped only as the 2 MiB arena window at its planned offset");
        check(bar0_whole_maps == 0, "BAR0 is never mapped as a whole, so the scanout stays write-combining");
        check(glue.config_value == (0x00100000u | 0x04u), "PCI config reads reach pci_cfg_read");
        check(glue.diagnostic_ok && glue.diagnostic_value == 0x12345678u &&
              glue.diagnostic_unaligned_refused && glue.diagnostic_out_of_range_refused,
              "diagnostic register reads reach BAR5 and refuse unaligned or out-of-range offsets");
        check(glue.clock_us > 0, "the clock adapter returns microseconds from the monotonic clock");
        check(glue.firmware_result == 0 && firmware_loads == 1 && firmware_releases == 1 &&
              firmware_device == 0x67df && firmware_revision == d.revision && firmware_key == 1,
              "firmware is loaded for this device, revision and the requested key");
        check(glue.workspace_ok && workspace_allocs == 1 && workspace_frees == 1,
              "the workspace comes from and returns to the kernel heap");

        /* Quiesce reports the self-test's verdict for the leased device only. */
        selftest_quiesces = 0;
        selftest_quiesce_result = 3;
        check(driver->quiesce(&d) == 3 && selftest_quiesces == 1,
              "quiesce of the Polaris device returns the self-test's verdict");
        selftest_quiesce_result = 0;
        struct device other = amd_gpu(0x5046);
        check(driver->quiesce(&other) == 0 && selftest_quiesces == 1,
              "quiesce of any other AMD device does nothing");

        /* Unbinding a leased device ends the lease for good. */
        remove_and_rebind(&d);
        d.drv = driver;
        amd_bootfb_probe_resources();
        check(glue.acquire_result != 0 && glue.held_after_acquire == 0,
              "after the leased device is unbound the lease is never handed out again");
    } else {
        const struct driver *driver = amd_bootfb_driver_for_test();
        check(driver && driver->quiesce, "the driver has a quiesce hook in every build");
        check(driver->quiesce(&d) == 0 && selftest_quiesces == 0,
              "a build without HWHUD has nothing to quiesce");
    }

    printf("AMD_BOOTFB: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
