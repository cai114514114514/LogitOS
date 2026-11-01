#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver.h"
#include "pci.h"
#include "amd/bootfb.h"
#include "amd/polaris/resource/device.h"

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
static uint32_t fake_bar5[0x4600];
static uint64_t resource_lfb, resource_bytes;
static uint32_t resource_width, resource_height;

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
    if (bar5_unmappable) return 0;
    return survey_fixture && bar == 5 ? (uint64_t)(uintptr_t)fake_bar5 : 0;
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

    printf("AMD_BOOTFB: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
