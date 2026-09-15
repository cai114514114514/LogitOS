#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver.h"
#include "intel_bootfb.h"
#include "pci.h"

static int checks, failures;
static uint16_t cfg_command = PCI_CMD_MEM;
static uint16_t cfg_pmcsr;
static uint8_t cfg_pm_cap = 0x40;
static uint32_t screen_w = 1920, screen_h = 1080;
static uint64_t lfb_addr = 0xc1000000ull;
static uint64_t lfb_bytes = 1920ull * 1080 * 4;
static int have_lfb = 1;
static unsigned writes, enables, maps, irqs, bad_cfg_reads;

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
    if (off > 0xff) bad_cfg_reads++;
    if (off == PCI_CFG_COMMAND) return cfg_command;
    if (cfg_pm_cap && off == (uint16_t)(cfg_pm_cap + 4)) return cfg_pmcsr;
    return 0;
}

uint8_t pci_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap)
{
    (void)bus; (void)slot; (void)func;
    return cap == 1 ? cfg_pm_cap : 0;
}

/* These operations are absent from intel_bootfb.c.  Keeping counters in the
 * host fixture makes a future accidental takeover visible in the same gate. */
void pci_cfg_write16(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint16_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; writes++; }
void dev_enable(struct device *d, int master)
{ (void)d; (void)master; enables++; }
uint64_t dev_bar_map(struct device *d, int i)
{ (void)d; (void)i; maps++; return 0; }
int dev_irq_request(struct device *d, irq_handler_t fn, void *arg, const char *name)
{ (void)d; (void)fn; (void)arg; (void)name; irqs++; return -1; }

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

static struct device gpu(uint16_t vendor, uint16_t device, uint8_t subclass)
{
    struct device d;
    memset(&d, 0, sizeof d);
    memcpy(d.name, "0000:00:02.0", 13);
    d.bus_type = DEV_BUS_PCI;
    d.bus = 0; d.slot = 2; d.func = 0;
    d.vendor = vendor; d.device = device;
    d.class_code = PCI_CLASS_DISPLAY; d.subclass = subclass;
    d.header_type = 0;
    d.res[0].start = 0xf0000000ull;
    d.res[0].size = 16ull << 20;
    d.res[0].flags = DEV_RES_MEM;
    d.res[2].start = 0xc0000000ull;
    d.res[2].size = 256ull << 20;
    d.res[2].flags = DEV_RES_MEM | DEV_RES_PREFETCH;
    return d;
}

static void reset_fixture(void)
{
    cfg_command = PCI_CMD_MEM;
    cfg_pmcsr = 0;
    cfg_pm_cap = 0x40;
    screen_w = 1920; screen_h = 1080;
    lfb_addr = 0xc1000000ull;
    lfb_bytes = 1920ull * 1080 * 4;
    have_lfb = 1;
}

static void check_quiet(const char *what)
{
    check(writes == 0 && enables == 0 && maps == 0 && irqs == 0, what);
}

int main(void)
{
    const struct dev_match *m = intel_bootfb_match_table();
    unsigned nmatch = 0;
    for (; m[nmatch].vendor || m[nmatch].device || m[nmatch].class_code ||
           m[nmatch].subclass || m[nmatch].prog_if || m[nmatch].data; nmatch++) {
        check(m[nmatch].vendor == 0x8086,
              "every production match is restricted to Intel vendor");
        check(m[nmatch].device == DEV_ANY &&
              m[nmatch].class_code == PCI_CLASS_DISPLAY,
              "Intel table covers display generations without broadening class");
        check(m[nmatch].subclass == 0x00 || m[nmatch].subclass == 0x02,
              "Intel table covers only VGA and 3D display subclasses");
    }
    check(nmatch == 2, "production table has exactly two Intel display entries");

    reset_fixture();
    struct device d = gpu(0x8086, 0x0412, 0x00); /* Haswell desktop */
    check(intel_bootfb_probe(&d) == 0,
          "Haswell-class Intel VGA boot display binds passively");
    check(d.drvdata != NULL, "successful passive bind records observer state");
    check_quiet("successful VGA probe performs no writes, enable, BAR map or IRQ request");

    reset_fixture();
    d = gpu(0x8086, 0x4692, 0x00); /* Alder/Raptor desktop family */
    check(intel_bootfb_probe(&d) == 0,
          "newer Intel VGA ID is covered without an ID allowlist");

    reset_fixture();
    d = gpu(0x8086, 0x56a0, 0x02); /* DG2/Arc 3D-controller form */
    check(intel_bootfb_probe(&d) == 0,
          "Intel 3D-controller boot display binds when it owns the LFB");

    reset_fixture();
    d = gpu(0x1002, 0x67df, 0x00);
    check(intel_bootfb_probe(&d) != 0,
          "a foreign display vendor is never claimed by the Intel probe");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); d.class_code = PCI_CLASS_NETWORK;
    check(intel_bootfb_probe(&d) != 0,
          "an Intel non-display function remains unclaimed");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x01);
    check(intel_bootfb_probe(&d) != 0,
          "unsupported display subclass remains unclaimed");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); d.header_type = 1;
    check(intel_bootfb_probe(&d) != 0,
          "bridge-header function remains unclaimed");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); cfg_command = 0;
    check(intel_bootfb_probe(&d) != 0,
          "memory-decode-off Intel GPU is refused rather than enabled");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); cfg_command = UINT16_MAX;
    check(intel_bootfb_probe(&d) != 0,
          "unreachable PCI command register is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); cfg_pmcsr = 3;
    check(intel_bootfb_probe(&d) != 0,
          "D3 Intel GPU is refused rather than woken");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); cfg_pm_cap = 0xfc;
    check(intel_bootfb_probe(&d) != 0 && bad_cfg_reads == 0,
          "malformed PM capability cannot cause an out-of-range config read");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); cfg_pm_cap = 0;
    check(intel_bootfb_probe(&d) == 0,
          "absence of optional PCI PM capability does not fabricate state");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); have_lfb = 0;
    check(intel_bootfb_probe(&d) != 0,
          "absent or virtio-backed Multiboot LFB is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); screen_w = 0;
    check(intel_bootfb_probe(&d) != 0,
          "zero-size display mode is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); lfb_addr = UINT64_MAX - 3; lfb_bytes = 8;
    check(intel_bootfb_probe(&d) != 0,
          "wrapped LFB aperture is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00); lfb_addr = 0xd0000000ull;
    check(intel_bootfb_probe(&d) != 0,
          "LFB outside every BAR is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00);
    lfb_addr = d.res[2].start + d.res[2].size - 4096;
    lfb_bytes = 8192;
    check(intel_bootfb_probe(&d) != 0,
          "partially overlapping LFB is refused unless fully contained");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00);
    memset(d.res, 0, sizeof d.res);
    check(intel_bootfb_probe(&d) != 0,
          "Intel GPU without a memory BAR is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00);
    memset(d.res, 0, sizeof d.res);
    d.res[2].start = UINT64_MAX - 7; d.res[2].size = 16;
    d.res[2].flags = DEV_RES_MEM;
    check(intel_bootfb_probe(&d) != 0,
          "wrapped memory BAR is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00);
    d.res[2].flags = DEV_RES_MEM | DEV_RES_IO;
    lfb_addr = d.res[2].start;
    check(intel_bootfb_probe(&d) != 0,
          "corrupt BAR marked as both memory and I/O is refused");

    reset_fixture();
    d = gpu(0x8086, 0x0412, 0x00);
    lfb_addr = d.res[2].start;
    lfb_bytes = d.res[2].size;
    check(intel_bootfb_probe(&d) == 0,
          "LFB exactly equal to a sane BAR aperture is accepted");

    check_quiet("all accepted and rejected paths remain free of GPU side effects");

    printf("INTEL_BOOTFB: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
