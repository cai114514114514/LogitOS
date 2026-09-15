#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver.h"
#include "pci.h"
#include "nvidia_pascal.h"

static int checks, failures;
static uint16_t cfg_command = PCI_CMD_MEM;
static uint16_t cfg_pmcsr;
static uint8_t cfg_pm_cap = 0x40;
static uint32_t screen_w = 1280, screen_h = 800;
static uint64_t lfb_addr = 0xe0100000ull, lfb_bytes = 1280ull * 800 * 4;
static int have_lfb = 1;
static unsigned writes, enables, maps, irqs;

static void check(int yes, const char *what)
{
    checks++;
    if (!yes) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* The probe is allowed only these read-side seams.  The other mocks make a
 * later accidental takeover visible as a failed zero-side-effects assertion. */
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
    *addr = lfb_addr; *bytes = lfb_bytes;
    return 1;
}
void kprintf(const char *fmt, ...) { (void)fmt; }

static struct device gpu(uint16_t vendor, uint16_t id)
{
    struct device d;
    memset(&d, 0, sizeof d);
    memcpy(d.name, "0000:01:00.0", 13);
    d.bus_type = DEV_BUS_PCI;
    d.bus = 1; d.slot = 0; d.func = 0;
    d.vendor = vendor; d.device = id;
    d.class_code = PCI_CLASS_DISPLAY; d.subclass = 0; d.header_type = 0;
    d.res[0].start = 0xf6000000ull;
    d.res[0].size = 16ull << 20;
    d.res[0].flags = DEV_RES_MEM;
    d.res[1].start = 0xe0000000ull;
    d.res[1].size = 256ull << 20;
    d.res[1].flags = DEV_RES_MEM | DEV_RES_PREFETCH;
    return d;
}

static void reset_bus(void)
{
    cfg_command = PCI_CMD_MEM;
    cfg_pmcsr = 0;
    cfg_pm_cap = 0x40;
    screen_w = 1280; screen_h = 800;
    lfb_addr = 0xe0100000ull; lfb_bytes = 1280ull * 800 * 4; have_lfb = 1;
    writes = enables = maps = irqs = 0;
}

int main(void)
{
    static const uint16_t ids[] = {
        0x1c21, 0x1c22, 0x1c61, 0x1c62, 0x1c81, 0x1c82,
        0x1c83, 0x1c8c, 0x1c8d, 0x1c8f, 0x1c91, 0x1c92,
    };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
        check(nvidia_pascal_model_name(ids[i]) != NULL,
              "NVIDIA official GTX 1050 ID is recognized");

    check(nvidia_pascal_model_name(0x1c90) == NULL,
          "adjacent MX150 ID is not accepted as GTX 1050");
    check(nvidia_pascal_model_name(0x1cb3) == NULL,
          "adjacent Quadro P400 ID is not accepted as GTX 1050");

    const struct dev_match *m = nvidia_pascal_bootfb_match_table();
    unsigned nmatch = 0;
    for (; m[nmatch].vendor || m[nmatch].device || m[nmatch].class_code ||
           m[nmatch].subclass || m[nmatch].prog_if || m[nmatch].data; nmatch++) {
        check(m[nmatch].vendor == 0x10de,
              "every production match is restricted to NVIDIA vendor");
        check(nmatch < sizeof ids / sizeof ids[0] &&
              m[nmatch].device == ids[nmatch],
              "declarative match table equals the audited ID list");
    }
    check(nmatch == sizeof ids / sizeof ids[0],
          "declarative table has exactly twelve GTX 1050 entries");

    reset_bus();
    struct device d = gpu(0x10de, 0x1c81);
    check(nvidia_pascal_bootfb_probe(&d) == 0,
          "D0 GTX 1050 with boot framebuffer binds passively");
    check(d.drvdata == (void *)nvidia_pascal_model_name(0x1c81),
          "bound device records its audited model name");
    check(writes == 0 && enables == 0 && maps == 0 && irqs == 0,
          "probe performs no config write, enable, BAR map or IRQ request");

    reset_bus();
    d = gpu(0x10de, 0x1c82); d.subclass = 2;
    check(nvidia_pascal_bootfb_probe(&d) == 0,
          "GTX 1050 Ti 3D-controller form binds passively");

    reset_bus();
    d = gpu(0x10de, 0x1c81); cfg_pmcsr = 3;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "D3 GPU is refused instead of being woken by an unsafe config write");

    reset_bus();
    d = gpu(0x10de, 0x1c81); cfg_command = 0;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "GPU with memory decoding disabled is refused");

    reset_bus();
    d = gpu(0x10de, 0x1c81); memset(d.res, 0, sizeof d.res);
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "GPU without a sane memory BAR is refused");

    reset_bus();
    d = gpu(0x10de, 0x1c81); d.res[0].start = UINT64_MAX - 7; d.res[0].size = 16;
    memset(&d.res[1], 0, sizeof d.res[1]);
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "wrapped memory BAR aperture is refused");

    reset_bus();
    d = gpu(0x10de, 0x1c81); d.class_code = PCI_CLASS_NETWORK;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "matching number on a non-display function remains unclaimed");

    reset_bus();
    d = gpu(0x10de, 0x1c81); d.subclass = 1;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "unsupported display subclass remains unclaimed");

    reset_bus();
    d = gpu(0x10de, 0x1c81); d.header_type = 1;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "matching bridge header remains unclaimed");

    reset_bus();
    d = gpu(0x1234, 0x1c81);
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "matching device number from another vendor remains unclaimed");

    reset_bus();
    d = gpu(0x10de, 0x1c81); have_lfb = 0;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "observer refuses a virtio or absent Multiboot LFB");

    reset_bus();
    d = gpu(0x10de, 0x1c81); lfb_addr = 0xd0000000ull;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "observer refuses an LFB owned by another GPU BAR");

    reset_bus();
    d = gpu(0x10de, 0x1c81); lfb_addr = UINT64_MAX - 3; lfb_bytes = 8;
    check(nvidia_pascal_bootfb_probe(&d) != 0,
          "observer refuses a wrapped LFB range");

    reset_bus();
    d = gpu(0x10de, 0x1c81); cfg_pm_cap = 0;
    check(nvidia_pascal_bootfb_probe(&d) == 0,
          "absence of optional PCI PM capability does not fabricate a state");
    check(writes == 0 && enables == 0 && maps == 0 && irqs == 0,
          "PM-absent probe remains read-only");

    printf("NV_BOOTFB: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
